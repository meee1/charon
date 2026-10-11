# Charon RF channel simulator

Runs several Charon nodes on one Linux host, connected through a simulated
RF channel with controllable impairments. No Pluto hardware is needed.

```
 charon_sim (node 0)                rfchan                 charon_sim (node 1)
   pluto.c  (unmodified)          ───────────               pluto.c
   iio_zmq.c (fake libiio) ─PUSH─▶ per-link channel ◀─PUSH─ iio_zmq.c
                           ◀─SUB── sum, impair,    ──SUB──▶
                                   AGC, ADC, pace
```

* **charon_sim** is Charon built for the host. `pluto.c` is compiled
  unchanged against the real `<iio.h>`/`<ad9361.h>` headers, but linked against
  `iio_zmq.c`, a fake libiio/libad9361. Gain/AGC, LO tuning, DC/IQ tracking
  and calibration, `xo_correction`, DCXO tuning and IIO buffer refill/push
  therefore run the same code as on a Pluto.
* **rfchan** is a real-time channel broker. Every 1 ms it takes 1400 TX
  samples from each node, runs every transmitter→receiver link through its
  impairments, sums all transmitters at each receiver (collisions are real),
  and publishes 1400 RX samples (12-bit ADC values) to every node.

## Build

```bash
sudo apt-get install libliquid-dev libfftw3-dev libzmq3-dev libiio-dev libad9361-dev
make -C sim            # -> sim/build/charon_sim, sim/build/rfchan
```

The libiio and libad9361 packages are only needed for their headers.

## Run the scenario suite

```bash
python3 sim/run_scenarios.py --list      # what's there
python3 sim/run_scenarios.py --quick     # CI subset (~5 min)
python3 sim/run_scenarios.py             # everything (~10 min, 2 in parallel)
python3 sim/run_scenarios.py cfo_dcxo dcxo_wrong_sign
```

Nodes run in traffic-generator mode (`sim_traffic.c`), which replaces the TAP
device with a Poisson UDP source/sink. No root is needed.
Each scenario writes to `sim/runs/<name>/`:

| File | Contents |
|---|---|
| `node<N>.log` | charon stderr (`SIMSTAT` lines once a second) |
| `node<N>.env` | the node's config (u-boot env equivalent) |
| `rfchan.log`, `rfchan.cmd` | channel log and command line |
| `gt.jsonl` | ground truth once a second: per node (effective ppm, DCXO codes, `xo_correction`, gain, RSSI, tracking state, TX gaps, clipping) and per link (true CFO, SFO, signal power, SNR) |

`sim/runs/summary.json` collects every check and metric.

## Run the full network stack

```bash
sudo sim/netns_run.sh                                 # 3-node line A-B-C
sudo sim/netns_run.sh --topology mesh --nodes 2
sudo sim/netns_run.sh --batman off -s node.2.ppm=3 --keep
sim/netns_run.sh                                      # as a normal user: rootless
```

**Rootless** (the default when not run as root; force it with `--rootless`):
each node runs under `unshare -Urn`, i.e. in its own user and network
namespace. Inside it the node is root and can create `ofdm0`, and the script
configures it with `nsenter --preserve-credentials`. Requirements:

* **Unprivileged user namespaces allowed.** Some distros restrict them, e.g.
  Ubuntu's `kernel.apparmor_restrict_unprivileged_userns`.
* **`/dev/net/tun` readable and writable by you.** It's 0666 on most distros.
* **Static routing only.** batman-adv isn't available rootless.

The script checks the first two up front and says which one is missing.

Either way, each node gets its own network namespace with a real `ofdm0` TAP
device. Neighbours ping each other once to fill ARP, then the script runs
`ping` and a 10 s minimum `iperf3` across the simulated RF links. The iperf3
check passes only if more than 10 kbit/s arrives.

* **batman-adv available:** Charon's own batman/bridge setup is used, with a
  dummy `usb0` in each namespace.
* **Otherwise** (`no_batman=1`): the script assigns /32 addresses and static
  routes. In the line topology, A reaches C through B, which forwards.

## Running by hand

```bash
sim/build/rfchan -s nodes=2 -s link.0-1.loss_db=70 -s node.1.ppm=3 -g gt.jsonl
CHARON_ENV_FILE=node0.env CHARON_SIM_NODE=0 CHARON_SIM_TRAFFIC=10 \
  CHARON_SIM_PEERS=10.0.0.2 sim/build/charon_sim
CHARON_ENV_FILE=node1.env CHARON_SIM_NODE=1 CHARON_SIM_TRAFFIC=0 sim/build/charon_sim
```

`nodeN.env` holds the u-boot settings, one `key=value` per line. Use at least
these:

```
freq_rxtx_hz=915000000
ref_correction_ppm=0
no_batman=1
cpu_mask=0
ipaddr=10.0.0.1
```

Leave out `CHARON_SIM_TRAFFIC` to get a real `ofdm0` TAP device (needs root).

### Node environment

| Variable | Meaning |
|---|---|
| `CHARON_ENV_FILE` | read config from this file instead of `fw_printenv` (works in any build) |
| `CHARON_SIM_NODE` | node index in rfchan |
| `CHARON_SIM_TX_EP`, `CHARON_SIM_RX_EP` | rfchan endpoints (default `ipc:///tmp/charon_sim_{tx,rx}`) |
| `CHARON_SIM_TRAFFIC` | mean frames/s (Poisson); enables traffic mode |
| `CHARON_SIM_TRAFFIC_LEN` | UDP payload bytes (default 200, max 2314 = `TAP_MTU` − 28) |
| `CHARON_SIM_PEERS` | comma-separated destination IPs, or `bcast` |
| `CHARON_SIM_RX_DUMP` | write every received int16 IQ sample to this file |

`--save-tx <file>` works as on hardware and records the TX samples.

## rfchan configuration

Use `-c file` (`key = value` lines, `#` comments) and/or `-s key=value`
(repeatable; later settings win).

**Global**

| Key | Default | |
|---|---|---|
| `nodes` | 2 | number of nodes (max 8) |
| `seed` | 1 | RNG seed (noise, drift, phase noise) |
| `fc` | 915e6 | carrier used for ground-truth CFO reporting |
| `duration` / `-t` | 0 (forever) | stop after this many simulated seconds |
| `gt_log` / `-g` | — | ground-truth JSON-lines file |
| `tx_ep`, `rx_ep` | `ipc:///tmp/charon_sim_{tx,rx}` | ZMQ endpoints |

**Per node: `node.<i|*>.<key>`**

| Key | Default | |
|---|---|---|
| `ppm` | 0 | static oscillator offset |
| `drift_ppm_per_rts` | 0 | random-walk drift, std-dev per √s |
| `pn_linewidth_hz` | 0 | LO phase noise (Wiener) |
| `ext_refclk` | 0 | 1 = external reference (stock Pluto): DCXO writes fail with -ENODEV |
| `dcxo_true_ppb_per_lsb` | 2.5 | the board's actual DCXO fine-tune gain (sign = direction) |
| `dcxo_coarse_ppm_per_step` | 3.0 | coarse-tune step |
| `dcxo_coarse0`, `dcxo_fine0` | 8, 4096 | power-on DCXO codes |
| `osc_correctable` | 1 | 0 = the node's XO/DCXO writes have no effect (raw PHY tolerance tests) |
| `nf_db` | 5 | receiver noise figure |
| `rx_dc_dbfs`, `rx_dc_phase_deg` | -inf, 0 | RX DC offset at the ADC |
| `rx_iq_gain_db`, `rx_iq_phase_deg` | 0, 0 | RX IQ imbalance |
| `tx_iq_gain_db`, `tx_iq_phase_deg` | 0, 0 | TX IQ imbalance |
| `tx_lo_leak_dbc` | -inf | TX LO leakage relative to DAC full scale |
| `honour_tracking` | 1 | emulate AD9361 DC/quadrature tracking and TX calibration |
| `track_suppress_db`, `track_time_ms` | 40, 50 | tracking suppression and ramp time |
| `txcal_suppress_db` | 40 | suppression from `calib_mode=tx_quad` |
| `self_isolation_db` | inf | TX→own-RX coupling (50 makes `--pluto-test` work) |

**Per link: `link.<a>-<b>.<key>` (both directions), `link.<a>><b>.<key>` (one way), `link.*.<key>`**

| Key | Default | |
|---|---|---|
| `loss_db` | 80 | path loss; `inf` = no path (hidden nodes) |
| `delay` | 0 | delay in samples; fractional values are allowed |
| `taps` | `1` | multipath FIR, e.g. `1, 0.5:0.2@3, 0.2@9` (`re[:im][@delay]`) |

**Interferers: `interferer.<k>.<key>`**: `rx` (node or `*`), `offset_hz`
(relative to the receiver LO), `power_dbm`, `duty`, `period_ms`.

## Models and assumptions

* **Oscillators:** each node has an oscillator factor
  `k = (1 + ppm·1e-6) · 40 MHz / xo_correction`, where ppm combines the static
  offset, drift and DCXO pull. It scales both the LO and the sample clock, so
  CFO = `lo_t·k_t − lo_r·k_r` and SFO = `k_r/k_t` stay consistent. This
  matches the AD9361 driver, where `xo_correction` recomputes the LO and BBPLL
  dividers.
* **Levels:**
  * DAC full scale is +7 dBm at 0 dB TX attenuation.
  * ADC full scale is 0 dBm at 0 dB RX gain.
  * Thermal noise is −174 dBm/Hz + NF over 1.4 MHz.
  * AGC (`fast_attack`) drops the gain immediately when the ADC level goes
    above −8 dBFS, and raises it 1 dB per ms below −24 dBFS.
  * `hardwaregain` reads back the AGC gain; `rssi` is the antenna input power.
* **Control timing:** TX gain and TX LO changes take effect at their position
  in the node's TX sample queue. Everything else (RX gain, RX LO, AGC mode,
  XO/DCXO, tracking) is immediate. On hardware a TX gain write is immediate
  even while DMA buffers are still airing. Here it is sample-accurate.
* **TX buffering:** `iio_buffer_push` blocks once more than 4 buffers of
  airtime are queued, like the kernel's 4 DMA blocks. RX refill only returns
  full 8192-sample buffers, as on hardware.
* **Tuning:** a receiver hears a transmitter only when their LOs are within
  700 kHz. `disable_rx()`'s +20 MHz detune therefore makes a node deaf, as
  intended.
* **SFO timeline:** during a burst, SFO is applied by a windowed-sinc resampler
  driven from a double-precision clock. When a link has been idle for 4 ms its
  timeline is re-anchored, so drift doesn't accumulate across bursts.
* **Correction emulation:** DC/IQ tracking and TX calibration attenuate the
  configured impairment. They don't model the AD9361 tracking loops.

## Files

| File | |
|---|---|
| `rfchan.c` | channel broker, config, oscillator / AGC / tracking models, ground truth |
| `channel_model.[ch]` | DSP blocks: RNG, AWGN, NCO, phase noise, IQ imbalance, FIR, resampler, ADC (unit tests: `tests/test_channel_model.c`) |
| `iio_zmq.c` | fake libiio + libad9361 |
| `sim_proto.h` | wire protocol |
| `sim_traffic.[ch]` | traffic generator/checker replacing the TAP device |
| `run_scenarios.py` | scenario suite |
| `netns_run.sh` | network-namespace full-stack runner |
