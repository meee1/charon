#!/usr/bin/env python3
# MIT License
#
# Copyright (c) 2026 Charon contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Charon RF channel simulator scenario runner.

Each scenario starts rfchan plus N charon_sim nodes (traffic-generator mode,
no root needed), lets them run in real time, then checks the nodes' logs and
rfchan's ground-truth log against pass/fail criteria.

  sim/run_scenarios.py                 # all scenarios
  sim/run_scenarios.py --quick         # CI subset
  sim/run_scenarios.py cfo_dcxo snr_sweep
  sim/run_scenarios.py --list

Results go to sim/runs/<scenario>/ (logs, gt.jsonl) and sim/runs/summary.json.
Exit status is non-zero if any check fails.
"""

import argparse
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
RUNS = os.path.join(HERE, "runs")

BASE_ENV = {
    "enable_charon": 1,
    "freq_rxtx_hz": 915000000,
    "ref_correction_ppm": 0,
    "no_batman": 1,
    "cpu_mask": 0,
}

STARTUP_S = 5.0          # charon init (config sleeps, IIO attach) before traffic


def node_ip(i):
    return "10.0.0.%d" % (i + 1)


###############################################################################
# One simulation run
###############################################################################
class Run:
    def __init__(self, path, n_nodes):
        self.path = path
        self.n = n_nodes
        self.logs = []
        for i in range(n_nodes):
            with open(os.path.join(path, "node%d.log" % i), "rb") as f:
                self.logs.append(f.read().decode("latin-1").replace("\r", "\n"))
        with open(os.path.join(path, "rfchan.log")) as f:
            self.rfchan_log = f.read()
        self.gt_nodes = {}
        self.gt_links = {}
        gt = os.path.join(path, "gt.jsonl")
        if os.path.exists(gt):
            with open(gt) as f:
                for line in f:
                    try:
                        d = json.loads(line)
                    except ValueError:
                        continue
                    if "node" in d:
                        self.gt_nodes.setdefault(d["node"], []).append(d)
                    else:
                        self.gt_links.setdefault(tuple(d["link"]), []).append(d)

    def stat(self, i):
        """Last SIMSTAT line of node i as a dict of numbers."""
        last = None
        for m in re.finditer(r"SIMSTAT node=\d+ (.*)", self.logs[i]):
            last = m.group(1)
        if not last:
            return {}
        return {k: float(v) for k, v in re.findall(r"(\w+)=([-\d.]+)", last)}

    def count(self, i, pattern):
        return len(re.findall(pattern, self.logs[i]))

    def evm(self, i):
        v = [float(x) for x in re.findall(r"rx EVM ([-\d.]+) dB", self.logs[i])]
        return sum(v) / len(v) if v else None

    def overruns(self):
        m = re.search(r"(\d+) overruns", self.rfchan_log)
        return int(m.group(1)) if m else -1

    def max_lag_ms(self):
        m = re.search(r"max lag ([\d.]+) ms", self.rfchan_log)
        return float(m.group(1)) if m else None

    def cfo_series(self, a, b):
        return [(d["t"], d["cfo_hz"]) for d in self.gt_links.get((a, b), [])]

    def final_abs_cfo(self, a, b, last_s=5):
        s = self.cfo_series(a, b)
        if not s:
            return None
        t_end = s[-1][0]
        return max(abs(c) for t, c in s if t >= t_end - last_s)

    def converge_time(self, a, b, thresh_hz):
        """First time after which |CFO| stays below thresh_hz."""
        s = self.cfo_series(a, b)
        t_conv = None
        for t, c in s:
            if abs(c) < thresh_hz:
                if t_conv is None:
                    t_conv = t
            else:
                t_conv = None
        return t_conv

    def snr(self, a, b):
        v = [d["snr_db"] for d in self.gt_links.get((a, b), []) if d["snr_db"] > -200]
        return sum(v) / len(v) if v else None

    def delivery(self, src, dst):
        """Frame delivery ratio rx_ok / (rx_ok + lost) at dst.

        'lost' counts sequence gaps between received frames, so this excludes
        frames sent before dst finished starting up and frames still in flight
        at shutdown.  Single-source receivers only (src is for reporting).
        """
        st = self.stat(dst)
        r, lost = st.get("rx_ok", 0), st.get("lost", 0)
        return r / (r + lost) if r + lost else 0.0

    def offered_delivery(self, src, dst):
        """rx_ok at dst over frames generated at src (includes start/end effects)."""
        g = self.stat(src).get("gen", 0)
        return self.stat(dst).get("rx_ok", 0) / g if g else 0.0


def run_sim(name, n_nodes, chan=(), node_env=None, traffic=None, duration=30,
            node_args=None, pluto_test=False):
    """Run rfchan + n_nodes charon_sim for `duration` seconds of traffic.

    chan:      rfchan key=value settings
    node_env:  {node: {uboot_key: value}} extra config per node ('*' = all)
    traffic:   {node: (pps, peers, length)}; peers = list of node ids or 'bcast'
    """
    node_env = node_env or {}
    traffic = traffic or {}
    node_args = node_args or {}
    path = os.path.join(RUNS, name)
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path)
    tag = re.sub(r"[^A-Za-z0-9_]", "_", name)
    tx_ep = "ipc:///tmp/charon_sim_%s_tx" % tag
    rx_ep = "ipc:///tmp/charon_sim_%s_rx" % tag

    total = STARTUP_S + duration
    rf_args = [os.path.join(BUILD, "rfchan"), "-s", "nodes=%d" % n_nodes,
               "-s", "tx_ep=" + tx_ep, "-s", "rx_ep=" + rx_ep,
               "-g", os.path.join(path, "gt.jsonl"), "-t", "%.1f" % (total + 2)]
    for kv in chan:
        rf_args += ["-s", kv]
    with open(os.path.join(path, "rfchan.cmd"), "w") as f:
        f.write(" ".join(rf_args) + "\n")
    rf_log = open(os.path.join(path, "rfchan.log"), "w")
    rf = subprocess.Popen(rf_args, stdout=rf_log, stderr=subprocess.STDOUT)
    time.sleep(0.3)

    procs = []
    for i in range(n_nodes):
        cfg = dict(BASE_ENV)
        cfg["ipaddr"] = node_ip(i)
        cfg.update(node_env.get("*", {}))
        cfg.update(node_env.get(i, {}))
        env_file = os.path.join(path, "node%d.env" % i)
        with open(env_file, "w") as f:
            for k, v in cfg.items():
                f.write("%s=%s\n" % (k, v))
        env = dict(os.environ)
        env.update({
            "CHARON_ENV_FILE": env_file,
            "CHARON_SIM_NODE": str(i),
            "CHARON_SIM_TX_EP": tx_ep,
            "CHARON_SIM_RX_EP": rx_ep,
        })
        args = [os.path.join(BUILD, "charon_sim")] + list(node_args.get(i, []))
        if pluto_test:
            args.append("--pluto-test")
            # traffic mode with no traffic: skips the ofdm0 TAP device,
            # which read_config() would otherwise create (needs root)
            env["CHARON_SIM_TRAFFIC"] = "0"
        else:
            pps, peers, length = traffic.get(i, (0, [], 200))
            env["CHARON_SIM_TRAFFIC"] = str(pps)
            env["CHARON_SIM_TRAFFIC_LEN"] = str(length)
            env["CHARON_SIM_PEERS"] = "bcast" if peers == "bcast" else \
                ",".join(node_ip(p) for p in peers)
        log = open(os.path.join(path, "node%d.log" % i), "w")
        procs.append((subprocess.Popen(args, env=env, stdout=log, stderr=subprocess.STDOUT), log))

    t_end = time.time() + total
    while time.time() < t_end:
        if pluto_test and all(p.poll() is not None for p, _ in procs):
            break
        time.sleep(0.2)
    rcs = []
    for p, log in procs:
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        rcs.append(p.returncode)
        log.close()
    rf.send_signal(signal.SIGTERM)
    try:
        rf.wait(5)
    except subprocess.TimeoutExpired:
        rf.kill()
    rf_log.close()
    for ep in (tx_ep, rx_ep):
        try:
            os.unlink(ep[len("ipc://"):])
        except OSError:
            pass
    r = Run(path, n_nodes)
    r.returncodes = rcs
    return r


###############################################################################
# Scenarios
###############################################################################
class Result:
    def __init__(self):
        self.checks = []     # (name, ok, detail)
        self.metrics = {}
        self.runs = []

    def check(self, name, ok, detail=""):
        self.checks.append((name, bool(ok), detail))

    def metric(self, k, v):
        self.metrics[k] = v

    def add_run(self, r):
        self.runs.append(r)
        ov = r.overruns()
        self.check("rfchan real-time (%s)" % os.path.relpath(r.path, RUNS), ov == 0,
                   "%d overruns, max lag %s ms" % (ov, fmt(r.max_lag_ms())))

    @property
    def ok(self):
        return all(c[1] for c in self.checks)


SCENARIOS = []


def scenario(quick=False):
    def deco(fn):
        SCENARIOS.append((fn.__name__, fn, quick, (fn.__doc__ or "").strip().split("\n")[0]))
        return fn
    return deco


def fmt(v, f="%.1f"):
    return "n/a" if v is None else f % v


ONE_WAY = {0: (10, [1], 200)}
TWO_WAY = {0: (10, [1], 200), 1: (10, [0], 200)}


@scenario(quick=True)
def ideal_link(res):
    """Ideal 2-node link: one-way unicast delivers every frame."""
    r = run_sim("ideal_link", 2, ["link.0-1.loss_db=70"], traffic=ONE_WAY, duration=20)
    res.add_run(r)
    d = r.delivery(0, 1)
    res.metric("delivery", d)
    res.metric("offered_delivery", r.offered_delivery(0, 1))
    res.metric("snr_db", r.snr(0, 1))
    res.metric("evm_db", r.evm(1))
    res.metric("latency_ms", r.stat(1).get("lat_ms"))
    res.check("delivery >= 99%", d >= 0.99, "%.1f%%" % (100 * d))
    res.check("no corrupt payloads", r.stat(1).get("rx_bad", 1) == 0)


@scenario(quick=True)
def broadcast(res):
    """Broadcast frames reach the peer without ACKs."""
    r = run_sim("broadcast", 2, ["link.0-1.loss_db=70"], traffic={0: (10, "bcast", 200)}, duration=20)
    res.add_run(r)
    d = r.delivery(0, 1)
    res.metric("delivery", d)
    res.check("delivery >= 98%", d >= 0.98, "%.1f%%" % (100 * d))


@scenario()
def bidirectional_load(res):
    """Two nodes sending to each other at 20 frames/s (CSMA contention)."""
    r = run_sim("bidirectional_load", 2, ["link.0-1.loss_db=70"],
                traffic={0: (20, [1], 200), 1: (20, [0], 200)}, duration=30)
    res.add_run(r)
    d0, d1 = r.delivery(0, 1), r.delivery(1, 0)
    res.metric("delivery_0to1", d0)
    res.metric("delivery_1to0", d1)
    res.check("delivery both ways >= 85%", min(d0, d1) >= 0.85, "%.1f%% / %.1f%%" % (100 * d0, 100 * d1))


@scenario(quick=True)
def snr_sweep(res):
    """Path-loss sweep: delivery vs SNR (PER waterfall)."""
    rows = []
    for loss in (72, 78, 82, 84, 86, 88):
        r = run_sim("snr_sweep/loss%d" % loss, 2, ["link.0-1.loss_db=%d" % loss],
                    traffic=ONE_WAY, duration=15)
        res.add_run(r)
        rows.append((loss, r.snr(0, 1), r.offered_delivery(0, 1), r.evm(1)))
    res.metric("curve", [{"loss_db": l, "snr_db": s, "delivery": d, "evm_db": e} for l, s, d, e in rows])
    best, worst = rows[0][2], rows[-1][2]
    res.check("delivery >= 95% of offered at highest SNR", best >= 0.95, "%.1f%% at %s dB SNR" % (100 * best, fmt(rows[0][1])))
    res.check("waterfall reached (lowest SNR < 50%)", worst < 0.5, "%.1f%% at %s dB SNR" % (100 * worst, fmt(rows[-1][1])))
    mono = all(rows[k + 1][2] <= rows[k][2] + 0.05 for k in range(len(rows) - 1))
    res.check("delivery non-increasing with path loss", mono,
              " ".join("%d:%.0f%%" % (l, 100 * d) for l, _, d, _ in rows))


def cfo_check(res, r, label, thresh=100.0):
    f = r.final_abs_cfo(0, 1)
    t = r.converge_time(0, 1, thresh)
    res.metric(label + "_final_abs_cfo_hz", f)
    res.metric(label + "_converge_s", t)
    res.check("%s: |CFO| < %.0f Hz at end" % (label, thresh), f is not None and f < thresh,
              "final %s Hz, converged at t=%s s" % (fmt(f), fmt(t)))


@scenario(quick=True)
def cfo_dcxo(res):
    """3 ppm XO offset, DCXO tuning closes it (crystal board)."""
    r = run_sim("cfo_dcxo", 2, ["link.0-1.loss_db=70", "node.1.ppm=3"], traffic=TWO_WAY, duration=40)
    res.add_run(r)
    cfo_check(res, r, "dcxo")
    res.check("DCXO path used", r.count(0, r"DCXO tuning available") == 1 and r.count(0, r"set DCXO") > 0,
              "%d DCXO writes" % (r.count(0, r"set DCXO") + r.count(1, r"set DCXO")))
    res.check("no xo_correction writes", r.count(0, r"set xo_correction") + r.count(1, r"set xo_correction") == 0)
    res.metric("delivery", r.delivery(0, 1))


@scenario()
def cfo_dcxo_large(res):
    """8 ppm offset (7.3 kHz at 915 MHz): pull-in through the step cap."""
    r = run_sim("cfo_dcxo_large", 2, ["link.0-1.loss_db=70", "node.1.ppm=8"], traffic=TWO_WAY, duration=60)
    res.add_run(r)
    cfo_check(res, r, "dcxo_large")


@scenario()
def cfo_2g4(res):
    """2.4 GHz band, 3 ppm offset (7.3 kHz): DCXO loop converges."""
    r = run_sim("cfo_2g4", 2, ["fc=2440e6", "link.0-1.loss_db=70", "node.1.ppm=3"],
                node_env={"*": {"freq_rxtx_hz": 2440000000}}, traffic=TWO_WAY, duration=50)
    res.add_run(r)
    cfo_check(res, r, "2g4", thresh=250.0)


@scenario(quick=True)
def dcxo_wrong_sign(res):
    """True DCXO slope negative while nodes assume positive: sign guard flips."""
    r = run_sim("dcxo_wrong_sign", 2, ["link.0-1.loss_db=70", "node.1.ppm=3",
                                       "node.*.dcxo_true_ppb_per_lsb=-2.5"],
                traffic=TWO_WAY, duration=50)
    res.add_run(r)
    flips = r.count(0, r"flipping tuning sign") + r.count(1, r"flipping tuning sign")
    res.metric("sign_flips", flips)
    res.check("sign guard fired", flips >= 1, "%d flips" % flips)
    cfo_check(res, r, "wrong_sign")


@scenario()
def dcxo_gain_high(res):
    """True DCXO gain 8x the assumed value: overshoot guard halves steps."""
    r = run_sim("dcxo_gain_high", 2, ["link.0-1.loss_db=70", "node.1.ppm=3",
                                      "node.*.dcxo_true_ppb_per_lsb=20"],
                traffic=TWO_WAY, duration=50)
    res.add_run(r)
    o = r.count(0, r"overshot") + r.count(1, r"overshot")
    res.metric("overshoot_halvings", o)
    res.check("overshoot guard fired", o >= 1, "%d halvings" % o)
    cfo_check(res, r, "gain_high")


@scenario(quick=True)
def xo_fallback(res):
    """External reference (stock Pluto): DCXO refused, xo_correction used."""
    r = run_sim("xo_fallback", 2, ["link.0-1.loss_db=70", "node.1.ppm=3", "node.*.ext_refclk=1"],
                traffic=TWO_WAY, duration=40)
    res.add_run(r)
    res.check("fallback detected", r.count(0, r"DCXO tuning unavailable") == 1)
    res.check("xo_correction written", r.count(0, r"set xo_correction") + r.count(1, r"set xo_correction") > 0)
    res.check("no DCXO writes", r.count(0, r"set DCXO") + r.count(1, r"set DCXO") == 0)
    cfo_check(res, r, "xo")
    sfo = [d["sfo_ppm"] for d in r.gt_links.get((0, 1), [])][-5:]
    res.metric("final_sfo_ppm", max(abs(x) for x in sfo) if sfo else None)
    res.check("SFO corrected along with CFO (< 0.15 ppm)", sfo and max(abs(x) for x in sfo) < 0.15,
              "final %s ppm" % fmt(max(abs(x) for x in sfo) if sfo else None, "%.3f"))


@scenario()
def drift(res):
    """XO random-walk drift: correction loop keeps tracking it."""
    r = run_sim("drift", 2, ["link.0-1.loss_db=70", "node.1.ppm=1", "node.*.drift_ppm_per_rts=0.05",
                             "seed=7"], traffic=TWO_WAY, duration=60)
    res.add_run(r)
    s = [abs(c) for t, c in r.cfo_series(0, 1) if t > 25]
    res.metric("max_abs_cfo_after_25s", max(s) if s else None)
    res.check("|CFO| stays < 200 Hz after pull-in", s and max(s) < 200, "max %s Hz" % fmt(max(s) if s else None))


def mean_ppm(r, nodes, t_from=None):
    """Mean effective ppm over nodes at each logged time."""
    series = {}
    for i in nodes:
        for d in r.gt_nodes.get(i, []):
            series.setdefault(d["t"], []).append(d["eff_ppm"])
    return sorted((t, sum(v) / len(v)) for t, v in series.items() if len(v) == len(nodes))


@scenario()
def mesh4_consensus(res):
    """4-node mesh, offsets 0/+3/-2/+5 ppm: pairwise CFO converges, mean doesn't walk."""
    r = run_sim("mesh4_consensus", 4, ["link.*.loss_db=70", "node.1.ppm=3", "node.2.ppm=-2", "node.3.ppm=5"],
                traffic={i: (3, [j for j in range(4) if j != i], 200) for i in range(4)}, duration=90)
    res.add_run(r)
    worst = max(r.final_abs_cfo(a, b) or 1e9 for a in range(4) for b in range(4) if a != b)
    m = mean_ppm(r, range(4))
    walk = max(v for _, v in m[10:]) - min(v for _, v in m[10:]) if len(m) > 12 else None
    flips = sum(r.count(i, r"flipping tuning sign") for i in range(4))
    res.metric("worst_pair_abs_cfo_hz", worst)
    res.metric("mean_ppm_walk_after_10s", walk)
    res.metric("sign_flips", flips)
    res.metric("delivery", [r.delivery(None, i) for i in range(4)])
    res.check("all pairs |CFO| < 150 Hz at end", worst < 150, "worst %.0f Hz" % worst)
    res.check("mesh mean frequency stable (< 0.3 ppm walk)", walk is not None and walk < 0.3,
              "walk %s ppm (start mean 1.5)" % fmt(walk, "%.2f"))
    res.check("no spurious DCXO sign flips", flips == 0, "%d flips" % flips)
    res.check("delivery >= 90% at every node", min(r.delivery(None, i) for i in range(4)) >= 0.90,
              " ".join("%.0f%%" % (100 * r.delivery(None, i)) for i in range(4)))


@scenario()
def long_drift(res):
    """4 minutes, XO drift 0.02 ppm/sqrt(s), bidirectional: loop stays stable."""
    r = run_sim("long_drift", 2, ["link.0-1.loss_db=72", "node.1.ppm=2", "node.*.drift_ppm_per_rts=0.02", "seed=3"],
                traffic={0: (15, [1], 600), 1: (15, [0], 300)}, duration=240)
    res.add_run(r)
    s = [abs(c) for t, c in r.cfo_series(0, 1) if t > 30]
    flips = r.count(0, r"flipping tuning sign") + r.count(1, r"flipping tuning sign")
    res.metric("max_abs_cfo_after_30s", max(s) if s else None)
    res.metric("sign_flips", flips)
    res.metric("delivery", [r.delivery(0, 1), r.delivery(1, 0)])
    res.check("no spurious DCXO sign flips", flips == 0, "%d flips" % flips)
    res.check("|CFO| < 250 Hz after pull-in", s and max(s) < 250, "max %s Hz" % fmt(max(s) if s else None))
    res.check("delivery >= 95% both ways", min(r.delivery(0, 1), r.delivery(1, 0)) >= 0.95,
              "%.1f%% / %.1f%%" % (100 * r.delivery(0, 1), 100 * r.delivery(1, 0)))


@scenario()
def cfo_tolerance(res):
    """Raw PHY CFO/SFO tolerance with corrections disabled in the channel."""
    rows = []
    for ppm in (1, 2, 4, 8):
        r = run_sim("cfo_tolerance/ppm%d" % ppm, 2,
                    ["link.0-1.loss_db=70", "node.1.ppm=%d" % ppm, "node.*.osc_correctable=0"],
                    traffic={0: (10, [1], 1000)}, duration=15)
        res.add_run(r)
        rows.append((ppm, ppm * 915.0, r.offered_delivery(0, 1)))
    res.metric("curve", [{"ppm": p, "cfo_hz": c, "delivery": d} for p, c, d in rows])
    res.check("1000-byte frames survive 4 ppm (3.7 kHz) uncorrected", rows[2][2] >= 0.90,
              " ".join("%dppm:%.0f%%" % (p, 100 * d) for p, _, d in rows))


@scenario(quick=True)
def dc_offset(res):
    """RX DC offset -15 dBFS: delivery with AD9361 DC tracking vs without."""
    out = {}
    for honour in (1, 0):
        r = run_sim("dc_offset/tracking%d" % honour, 2,
                    ["link.0-1.loss_db=70", "node.1.rx_dc_dbfs=-15", "node.*.honour_tracking=%d" % honour],
                    traffic=ONE_WAY, duration=20)
        res.add_run(r)
        out[honour] = (r.delivery(0, 1), r.evm(1))
    res.metric("delivery_tracking_on", out[1][0])
    res.metric("delivery_tracking_off", out[0][0])
    res.metric("evm_tracking_on", out[1][1])
    res.metric("evm_tracking_off", out[0][1])
    res.check("delivery >= 98% with DC tracking", out[1][0] >= 0.98, "%.1f%%" % (100 * out[1][0]))


@scenario(quick=True)
def iq_imbalance(res):
    """RX 1.5 dB / 10 deg and TX 1 dB / 5 deg IQ imbalance, with and without correction."""
    out = {}
    for honour in (1, 0):
        r = run_sim("iq_imbalance/tracking%d" % honour, 2,
                    ["link.0-1.loss_db=70", "node.*.rx_iq_gain_db=1.5", "node.*.rx_iq_phase_deg=10",
                     "node.*.tx_iq_gain_db=1", "node.*.tx_iq_phase_deg=5",
                     "node.*.honour_tracking=%d" % honour] +
                    ([] if honour else ["node.*.txcal_suppress_db=0"]),
                    traffic=ONE_WAY, duration=20)
        res.add_run(r)
        out[honour] = (r.delivery(0, 1), r.evm(1))
    res.metric("delivery_corrected", out[1][0])
    res.metric("delivery_uncorrected", out[0][0])
    res.metric("evm_corrected", out[1][1])
    res.metric("evm_uncorrected", out[0][1])
    res.check("delivery >= 98% with IQ correction", out[1][0] >= 0.98, "%.1f%%" % (100 * out[1][0]))
    res.check("correction improves EVM", out[1][1] is not None and out[0][1] is not None and out[1][1] < out[0][1],
              "%s vs %s dB" % (fmt(out[1][1]), fmt(out[0][1])))


@scenario()
def tx_lo_leakage(res):
    """TX LO leakage -25 dBc: TX quadrature calibration suppresses it."""
    r = run_sim("tx_lo_leakage", 2, ["link.0-1.loss_db=70", "node.0.tx_lo_leak_dbc=-25"],
                traffic=ONE_WAY, duration=20)
    res.add_run(r)
    d = r.delivery(0, 1)
    res.metric("delivery", d)
    res.check("TX quad calibration ran", r.count(0, r"running TX quadrature calibration") >= 1)
    res.check("delivery >= 98%", d >= 0.98, "%.1f%%" % (100 * d))


@scenario(quick=True)
def multipath(res):
    """Echo (0.5, delay 1..9 samples) inside the CP: delay-spread tolerance."""
    rows = []
    for d in (1, 2, 3, 4, 6, 9):
        r = run_sim("multipath/echo%d" % d, 2, ["link.0-1.loss_db=68", "link.0-1.taps=1, 0.5@%d" % d],
                    traffic=ONE_WAY, duration=12)
        res.add_run(r)
        rows.append((d, r.offered_delivery(0, 1), r.evm(1)))
    res.metric("curve", [{"echo_delay_samples": d, "delivery": dl, "evm_db": e} for d, dl, e in rows])
    ok = [dl for d, dl, _ in rows if d <= 3]
    # Known limit: liquid's ofdmframesync smooths the channel estimate with
    # an order-4 polynomial across subcarriers, which can't follow the gain
    # ripple of echoes >= ~4 samples (2.9 us) even though they fit the CP.
    res.check("echo <= 3 samples (2.1 us) delivers >= 95%", min(ok) >= 0.95,
              " ".join("%d:%.0f%%" % (d, 100 * dl) for d, dl, _ in rows))

@scenario()
def multipath_fractional(res):
    """Fractional delay (2.4 samples) plus a short echo."""
    r = run_sim("multipath_fractional", 2, ["link.0-1.loss_db=68", "link.0-1.delay=2.4",
                                            "link.0-1.taps=1, 0.3:0.2@2"], traffic=ONE_WAY, duration=20)
    res.add_run(r)
    d = r.delivery(0, 1)
    res.metric("delivery", d)
    res.metric("evm_db", r.evm(1))
    res.check("delivery >= 95%", d >= 0.95, "%.1f%%" % (100 * d))

@scenario()
def phase_noise(res):
    """LO phase noise sweep (Wiener linewidth per node), 1000-byte frames."""
    rows = []
    for lw in (50, 200, 500):
        r = run_sim("phase_noise/lw%d" % lw, 2, ["link.0-1.loss_db=70", "node.*.pn_linewidth_hz=%d" % lw],
                    traffic={0: (10, [1], 1000)}, duration=15)
        res.add_run(r)
        rows.append((lw, r.offered_delivery(0, 1), r.evm(1)))
    res.metric("curve", [{"linewidth_hz": lw, "delivery": d, "evm_db": e} for lw, d, e in rows])
    res.check("<= 200 Hz linewidth delivers >= 95%", min(d for lw, d, _ in rows if lw <= 200) >= 0.95,
              " ".join("%dHz:%.0f%%" % (lw, 100 * d) for lw, d, _ in rows))

@scenario(quick=True)
def cw_interferer(res):
    """CW interferer on the CW-tone frequency must not steer the XO."""
    r = run_sim("cw_interferer", 2, ["link.0-1.loss_db=70", "node.1.ppm=2",
                                     "interferer.0.rx=*", "interferer.0.offset_hz=352000",
                                     "interferer.0.power_dbm=-85", "interferer.0.duty=0.2",
                                     "interferer.0.period_ms=37"],
                traffic=TWO_WAY, duration=40)
    res.add_run(r)
    cw = r.count(0, r"CW_TONE:") + r.count(1, r"CW_TONE:")
    res.metric("cw_detections", cw)
    cfo_check(res, r, "with_interferer")
    res.metric("delivery", r.delivery(0, 1))


@scenario(quick=True)
def hidden_node(res):
    """Two senders to B: A and C hidden from each other vs audible."""
    out = {}
    for label, ac in (("hidden", "inf"), ("audible", "70")):
        r = run_sim("hidden_node/%s" % label, 3, ["link.0-1.loss_db=70", "link.1-2.loss_db=70",
                                                  "link.0-2.loss_db=%s" % ac],
                    traffic={0: (5, [1], 200), 2: (5, [1], 200)}, duration=25)
        res.add_run(r)
        out[label] = r.delivery(None, 1)
        res.metric("dup_drops_" + label, r.count(1, r"dropping duplicate"))
    res.metric("delivery_hidden", out["hidden"])
    res.metric("delivery_audible", out["audible"])
    res.check("B receives >= 85% from two senders (both cases)", min(out.values()) >= 0.85,
              "hidden %.1f%%, audible %.1f%%" % (100 * out["hidden"], 100 * out["audible"]))


@scenario(quick=True)
def equal_senders_pid(res):
    """Regression: senders started together must not share frame pids."""
    r = run_sim("equal_senders_pid", 3, ["link.*.loss_db=70"],
                traffic={0: (5, [1], 200), 2: (5, [1], 200)}, duration=25)
    res.add_run(r)
    acked = r.count(1, r"SENT ACK")
    dups = r.count(1, r"dropping duplicate")
    res.metric("acks_sent", acked)
    res.metric("dup_drops", dups)
    res.check("duplicate drops < 5% of ACKed frames", acked and dups < 0.05 * acked, "%d of %d" % (dups, acked))
    res.check("B receives >= 85%", r.delivery(None, 1) >= 0.85, "%.1f%%" % (100 * r.delivery(None, 1)))

@scenario()
def agc_dynamic_range(res):
    """Near/far: strong (50 dB) and weak (80 dB) senders to one receiver."""
    r = run_sim("agc_dynamic_range", 3, ["link.0-1.loss_db=50", "link.2-1.loss_db=78", "link.0-2.loss_db=70"],
                traffic={0: (5, [1], 200), 2: (5, [1], 200)}, duration=30)
    res.add_run(r)
    d = r.delivery(None, 1)
    clipped = r.gt_nodes.get(1, [{}])[-1].get("clipped", 0)
    res.metric("delivery_to_B", d)
    res.metric("adc_clipped_samples", clipped)
    res.check("B receives >= 85% from near and far senders", d >= 0.85, "%.1f%%" % (100 * d))


@scenario(quick=True)
def pluto_test(res):
    """Unmodified --pluto-test hardware smoke test via TX->RX self-coupling."""
    r = run_sim("pluto_test", 1, ["node.0.self_isolation_db=50"], duration=40, pluto_test=True)
    res.add_run(r)
    m = re.search(r"Results: (\d+)/(\d+) tests passed", r.logs[0])
    res.check("pluto-test 5/5", m and m.group(1) == m.group(2) == "5", m.group(0) if m else "no result line")


###############################################################################
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="scenarios to run (default: all)")
    ap.add_argument("--quick", action="store_true", help="CI subset")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=2, help="scenarios in parallel (default 2)")
    a = ap.parse_args()

    if a.list:
        for name, _, quick, doc in SCENARIOS:
            print("%-22s %s %s" % (name, "[quick]" if quick else "       ", doc))
        return 0
    for b in ("rfchan", "charon_sim"):
        if not os.path.exists(os.path.join(BUILD, b)):
            sys.exit("missing %s: run 'make -C sim' first" % os.path.join(BUILD, b))

    todo = [s for s in SCENARIOS if (not a.names or s[0] in a.names) and (not a.quick or s[2])]
    unknown = set(a.names) - {s[0] for s in SCENARIOS}
    if unknown:
        sys.exit("unknown scenario(s): %s" % ", ".join(sorted(unknown)))
    os.makedirs(RUNS, exist_ok=True)

    def run(s):
        name, fn, _, doc = s
        res = Result()
        t0 = time.time()
        try:
            fn(res)
        except Exception as e:   # report, don't abort the suite
            res.check("scenario ran", False, "%s: %s" % (type(e).__name__, e))
        res.elapsed = time.time() - t0
        print("[%s] %s (%.0f s)" % ("PASS" if res.ok else "FAIL", name, res.elapsed), flush=True)
        for c in res.checks:
            if not c[1] or not c[0].startswith("rfchan real-time"):
                print("    %s %s  %s" % ("ok  " if c[1] else "FAIL", c[0], c[2]), flush=True)
        return name, doc, res

    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        results = list(ex.map(run, todo))

    summary = {name: {"doc": doc, "ok": res.ok, "elapsed_s": round(res.elapsed, 1),
                      "checks": [{"name": c[0], "ok": c[1], "detail": c[2]} for c in res.checks],
                      "metrics": res.metrics}
               for name, doc, res in results}
    with open(os.path.join(RUNS, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2, default=str)

    n_fail = sum(1 for _, _, r in results if not r.ok)
    print("\n%d/%d scenarios passed (details: %s)" % (len(results) - n_fail, len(results),
                                                    os.path.join(RUNS, "summary.json")))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
