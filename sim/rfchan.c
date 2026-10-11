//MIT License
//
//Copyright (c) 2026 Charon contributors
//
//Permission is hereby granted, free of charge, to any person obtaining a copy
//of this software and associated documentation files (the "Software"), to deal
//in the Software without restriction, including without limitation the rights
//to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//copies of the Software, and to permit persons to whom the Software is
//furnished to do so, subject to the following conditions:
//
//The above copyright notice and this permission notice shall be included in all
//copies or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//SOFTWARE.

// rfchan: real-time RF channel simulator for Charon nodes (charon_sim).
//
// Every 1 ms tick it takes 1400 TX samples from each node's queue (zeros
// when idle), runs every transmitter->receiver link through its impairments,
// sums all transmitters at each receiver (collisions are real), adds the
// receiver's own impairments, applies AGC/gain and a 12-bit ADC, and
// publishes 1400 RX samples to every node.
//
// Signal chain for transmitter t -> receiver r:
//   TX:   DAC codes -> TX IQ imbalance -> TX LO leakage -> TX gain (dBm)
//   link: path loss -> multipath FIR -> SFO resampler (k_r/k_t) + delay
//         -> CFO (lo_t*k_t - lo_r*k_r) -> phase noise (t and r)
//   RX:   sum of links + CW interferers + thermal noise (NF)
//         -> gain (manual or fast-attack AGC) -> RX IQ imbalance -> DC
//         -> 12-bit ADC (quantize + clip)
//
// Each node's oscillator scale factor k = (1 + ppm*1e-6) * 40 MHz / xo_corr,
// where ppm = base + random-walk drift + DCXO pull.  It scales both its LO and
// its sample clock, so CFO and SFO stay consistent.  DCXO writes are refused
// (-ENODEV on the node) when the node is configured with ext_refclk=1.
//
// AD9361 corrections are emulated: enabling DC / quadrature tracking ramps
// the corresponding impairment down by track_suppress_db over track_time_ms;
// calib_mode tx_quad suppresses TX IQ imbalance and LO leakage.
//
// Usage: rfchan [-c config] [-s key=value]... [-t seconds] [-g truth.jsonl]
// See sim/README.md for config keys.

#define _GNU_SOURCE 1
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <zmq.h>

#include "channel_model.h"
#include "sim_proto.h"

#define MAX_NODES        8
#define MAX_INTERFERERS  8
#define TXQ_SAMPLES      (1 << 20)
#define TXQ_EVENTS       1024
#define LINK_FIFO        (SIM_CHUNK * 4)
#define LINK_MARGIN      32
#define TX_FS_DBM        7.0     // DAC full scale (|x|=1) at 0 dB TX attenuation
#define ADC_FS_DBM       0.0     // ADC full scale input power at 0 dB RX gain
#define ADC_BITS         12
#define AGC_TARGET_DBFS  -14.0
#define AGC_HIGH_DBFS    -8.0
#define AGC_LOW_DBFS     -24.0
#define GAIN_MAX_DB      73.0
#define GAIN_MIN_DB      -3.0

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

///////////////////////////////////////////////////////////////////////////////////////
// Configuration
///////////////////////////////////////////////////////////////////////////////////////
typedef struct {
  // oscillator
  double ppm;                 // static offset
  double drift_ppm_per_rts;   // random-walk std-dev per sqrt(second)
  double pn_linewidth_hz;     // LO phase noise (Wiener) linewidth
  int    ext_refclk;          // 1: no DCXO (stock PlutoSDR)
  double dcxo_true_ppb_per_lsb;
  double dcxo_coarse_ppm_per_step;
  int    dcxo_coarse0, dcxo_fine0;
  // receiver
  double nf_db;
  double rx_dc_dbfs;          // DC offset at the ADC (dBFS), -inf = none
  double rx_dc_phase_deg;
  double rx_iq_gain_db, rx_iq_phase_deg;
  // transmitter
  double tx_iq_gain_db, tx_iq_phase_deg;
  double tx_lo_leak_dbc;      // LO leakage relative to DAC full scale
  // AD9361 correction emulation
  int    honour_tracking;
  double track_suppress_db;
  double track_time_ms;
  double txcal_suppress_db;
  // TX->own RX coupling (inf = isolated)
  double self_isolation_db;
  // 0: the node's xo_correction / DCXO writes have no effect (measures raw
  // PHY tolerance to CFO/SFO); the node still sees its writes succeed
  int    osc_correctable;
} node_cfg;

typedef struct {
  double loss_db;             // inf = no path
  int    delay;               // integer samples
  double frac_delay;          // fractional samples
  int    ntaps;
  cf_t   taps[CM_FIR_MAX_TAPS];
} link_cfg;

typedef struct {
  int    rx;                  // receiver index, -1 = all
  double offset_hz;           // relative to the receiver LO
  double power_dbm;
  double duty;                // 0..1 fraction of each period that it is on
  double period_ms;
} interferer_cfg;

static int    n_nodes = 2;
static uint64_t seed = 1;
static double fc_hz = 915e6;
static double duration_s = 0;
static char   tx_ep[256] = SIM_DEFAULT_TX_EP;
static char   rx_ep[256] = SIM_DEFAULT_RX_EP;
static char   gt_path[512] = "";
static node_cfg ncfg[MAX_NODES];
static link_cfg lcfg[MAX_NODES][MAX_NODES];   // [tx][rx]
static interferer_cfg icfg[MAX_INTERFERERS];
static int    n_interferers;

static double parse_db(const char *v) {
  if (!strcmp(v, "inf") || !strcmp(v, "+inf")) return INFINITY;
  if (!strcmp(v, "-inf")) return -INFINITY;
  return atof(v);
}

static void set_defaults(void) {
  for (int i = 0; i < MAX_NODES; i++) {
    node_cfg *c = &ncfg[i];
    memset(c, 0, sizeof(*c));
    c->dcxo_true_ppb_per_lsb = 2.5;
    c->dcxo_coarse_ppm_per_step = 3.0;
    c->dcxo_coarse0 = 8;
    c->dcxo_fine0 = 4096;
    c->nf_db = 5.0;
    c->rx_dc_dbfs = -INFINITY;
    c->tx_lo_leak_dbc = -INFINITY;
    c->honour_tracking = 1;
    c->track_suppress_db = 40.0;
    c->track_time_ms = 50.0;
    c->txcal_suppress_db = 40.0;
    c->self_isolation_db = INFINITY;
    c->osc_correctable = 1;
    for (int j = 0; j < MAX_NODES; j++) {
      link_cfg *l = &lcfg[i][j];
      memset(l, 0, sizeof(*l));
      l->loss_db = (i == j) ? INFINITY : 80.0;
      l->ntaps = 1;
      l->taps[0] = 1.0f;
    }
  }
}

// taps: "1, 0.3:0.1@2, 0.1@5"  ->  re[:im][@delay]
static void parse_taps(link_cfg *l, const char *v) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", v);
  memset(l->taps, 0, sizeof(l->taps));
  l->ntaps = 1;
  int idx = 0;
  for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
    double re = 0, im = 0;
    int d = idx;
    char *at = strchr(tok, '@');
    if (at) { d = atoi(at + 1); *at = 0; }
    char *colon = strchr(tok, ':');
    re = atof(tok);
    if (colon) im = atof(colon + 1);
    if (d >= 0 && d < CM_FIR_MAX_TAPS) {
      l->taps[d] = (float)re + _Complex_I * (float)im;
      if (d + 1 > l->ntaps) l->ntaps = d + 1;
    }
    idx = d + 1;
  }
}

static int set_node_key(node_cfg *c, const char *k, const char *v) {
#define KD(name) if (!strcmp(k, #name)) { c->name = parse_db(v); return 0; }
#define KI(name) if (!strcmp(k, #name)) { c->name = atoi(v); return 0; }
  KD(ppm) KD(drift_ppm_per_rts) KD(pn_linewidth_hz) KI(ext_refclk)
  KD(dcxo_true_ppb_per_lsb) KD(dcxo_coarse_ppm_per_step) KI(dcxo_coarse0) KI(dcxo_fine0)
  KD(nf_db) KD(rx_dc_dbfs) KD(rx_dc_phase_deg) KD(rx_iq_gain_db) KD(rx_iq_phase_deg)
  KD(tx_iq_gain_db) KD(tx_iq_phase_deg) KD(tx_lo_leak_dbc)
  KI(honour_tracking) KD(track_suppress_db) KD(track_time_ms) KD(txcal_suppress_db)
  KD(self_isolation_db) KI(osc_correctable)
#undef KD
#undef KI
  return -1;
}

static int set_link_key(link_cfg *l, const char *k, const char *v) {
  if (!strcmp(k, "loss_db")) { l->loss_db = parse_db(v); return 0; }
  if (!strcmp(k, "delay")) {
    double d = atof(v);
    l->delay = (int)floor(d);
    l->frac_delay = d - floor(d);
    return 0;
  }
  if (!strcmp(k, "taps")) { parse_taps(l, v); return 0; }
  return -1;
}

static int set_key(const char *key, const char *v) {
  char k[1024];
  snprintf(k, sizeof(k), "%s", key);
  if (!strcmp(k, "nodes")) { n_nodes = atoi(v); if (n_nodes > MAX_NODES) n_nodes = MAX_NODES; return 0; }
  if (!strcmp(k, "seed")) { seed = strtoull(v, NULL, 0); return 0; }
  if (!strcmp(k, "fc")) { fc_hz = atof(v); return 0; }
  if (!strcmp(k, "duration")) { duration_s = atof(v); return 0; }
  if (!strcmp(k, "tx_ep")) { snprintf(tx_ep, sizeof(tx_ep), "%s", v); return 0; }
  if (!strcmp(k, "rx_ep")) { snprintf(rx_ep, sizeof(rx_ep), "%s", v); return 0; }
  if (!strcmp(k, "gt_log")) { snprintf(gt_path, sizeof(gt_path), "%s", v); return 0; }

  // node.<i|*>.<param>
  if (!strncmp(k, "node.", 5)) {
    char *sel = k + 5, *param = strchr(sel, '.');
    if (!param) return -1;
    *param++ = 0;
    int rc = 0;
    for (int i = 0; i < MAX_NODES; i++)
      if (!strcmp(sel, "*") || atoi(sel) == i)
        rc |= set_node_key(&ncfg[i], param, v);
    return rc;
  }
  // link.<a>-<b>.<param> (both directions), link.<a>><b>.<param> (one way),
  // link.*.<param> (all pairs)
  if (!strncmp(k, "link.", 5)) {
    char *sel = k + 5, *param = strchr(sel, '.');
    if (!param) return -1;
    *param++ = 0;
    int rc = 0;
    if (!strcmp(sel, "*")) {
      for (int a = 0; a < MAX_NODES; a++)
        for (int b = 0; b < MAX_NODES; b++)
          if (a != b) rc |= set_link_key(&lcfg[a][b], param, v);
      return rc;
    }
    int a, b;
    if (sscanf(sel, "%d-%d", &a, &b) == 2 && a >= 0 && b >= 0 && a < MAX_NODES && b < MAX_NODES) {
      rc |= set_link_key(&lcfg[a][b], param, v);
      rc |= set_link_key(&lcfg[b][a], param, v);
      return rc;
    }
    if (sscanf(sel, "%d>%d", &a, &b) == 2 && a >= 0 && b >= 0 && a < MAX_NODES && b < MAX_NODES)
      return set_link_key(&lcfg[a][b], param, v);
    return -1;
  }
  // interferer.<i>.<param>
  if (!strncmp(k, "interferer.", 11)) {
    int i = atoi(k + 11);
    char *param = strchr(k + 11, '.');
    if (!param || i < 0 || i >= MAX_INTERFERERS) return -1;
    param++;
    interferer_cfg *c = &icfg[i];
    if (i + 1 > n_interferers) {
      for (int j = n_interferers; j <= i; j++) {
        icfg[j].rx = -1; icfg[j].duty = 1.0; icfg[j].period_ms = 100; icfg[j].power_dbm = -INFINITY;
      }
      n_interferers = i + 1;
    }
    if (!strcmp(param, "rx")) { c->rx = !strcmp(v, "*") ? -1 : atoi(v); return 0; }
    if (!strcmp(param, "offset_hz")) { c->offset_hz = atof(v); return 0; }
    if (!strcmp(param, "power_dbm")) { c->power_dbm = parse_db(v); return 0; }
    if (!strcmp(param, "duty")) { c->duty = atof(v); return 0; }
    if (!strcmp(param, "period_ms")) { c->period_ms = atof(v); return 0; }
    return -1;
  }
  return -1;
}

static void trim(char *s) {
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
  char *b = s;
  while (*b == ' ' || *b == '\t') b++;
  memmove(s, b, strlen(b) + 1);
}

static int apply_kv(const char *line) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s", line);
  char *hash = strchr(buf, '#');
  if (hash) *hash = 0;
  char *eq = strchr(buf, '=');
  if (!eq) { trim(buf); return buf[0] ? -1 : 0; }
  *eq = 0;
  char *k = buf, *v = eq + 1;
  trim(k); trim(v);
  if (set_key(k, v) != 0) {
    fprintf(stderr, "rfchan: unknown config key '%s'\n", k);
    return -1;
  }
  return 0;
}

static int load_config(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); return -1; }
  char line[1024];
  int rc = 0;
  while (fgets(line, sizeof(line), f)) rc |= apply_kv(line);
  fclose(f);
  return rc;
}

///////////////////////////////////////////////////////////////////////////////////////
// Runtime state
///////////////////////////////////////////////////////////////////////////////////////
typedef struct {
  int64_t  pos;               // TX queue sample position at which it applies
  sim_ctrl c;
} tx_event;

typedef struct {
  int       connected;
  // radio state (from the node)
  long long rx_lo, tx_lo;
  double    tx_gain_db;
  double    man_gain_db, agc_gain_db;
  int       gain_mode;
  long long xo_corr;
  int       dcxo_coarse, dcxo_fine;
  int       trk_bbdc, trk_rfdc, trk_quad;
  double    t_dc_on, t_quad_on;    // seconds when tracking was (re)enabled
  int       rfdc_cal, txquad_cal;
  // oscillator
  double    walk_ppm;
  cm_phase_noise pn;
  double    ph[SIM_CHUNK];
  // TX queue (int16 IQ)
  int16_t  *txq;
  int64_t   q_in, q_out;
  tx_event  ev[TXQ_EVENTS];
  int       ev_head, ev_tail;
  double    tx_amp;                // sqrt(mW) per DAC unit at current gain
  cf_t      txc[SIM_CHUNK];
  int       tx_active;
  // impairment objects
  cm_iq     rx_iq, tx_iq;
  // RX
  uint32_t  rx_seq;
  cm_rng    rng;
  // stats for the truth log (reset every report)
  double    rssi_sum; int rssi_n;
  long      clipped;
  long      tx_samples;
  long      tx_gaps;               // TX queue ran dry mid-burst (node too slow)
  int       tx_dry;
  long      underflows;
} node_t;

typedef struct {
  cm_fir    fir;
  cm_resamp rs;
  cm_nco    nco;
  cf_t      fifo[LINK_FIFO];
  int       fifo_n;
  int       quiet;
  long      idle;
  double    sig_sum; long sig_n;   // received signal power while active
} link_t;

static node_t nodes[MAX_NODES];
static link_t links[MAX_NODES][MAX_NODES];
static uint64_t sample_time;
static volatile sig_atomic_t stop;
static FILE *gt;
static void *zpull, *zpub;
static cm_rng chan_rng;

static double node_dcxo_ppm(const node_t *n, const node_cfg *c) {
  if (c->ext_refclk) return 0.0;
  return (n->dcxo_fine - c->dcxo_fine0) * c->dcxo_true_ppb_per_lsb * 1e-3 +
         (n->dcxo_coarse - c->dcxo_coarse0) * c->dcxo_coarse_ppm_per_step *
         (c->dcxo_true_ppb_per_lsb >= 0 ? 1.0 : -1.0);
}

// Oscillator scale factor (actual / nominal) for LO and sample clock.
static double node_k(int i) {
  const node_t *n = &nodes[i];
  const node_cfg *c = &ncfg[i];
  if (!c->osc_correctable) return 1.0 + (c->ppm + n->walk_ppm) * 1e-6;
  double ppm = c->ppm + n->walk_ppm + node_dcxo_ppm(n, c);
  return (1.0 + ppm * 1e-6) * ((double)SIM_XO_NOMINAL_HZ / (double)n->xo_corr);
}

static void link_reset(int t, int r) {
  link_t *L = &links[t][r];
  link_cfg *lc = &lcfg[t][r];
  cm_fir_init(&L->fir, lc->taps, lc->ntaps);
  cm_resamp_init(&L->rs, 1.0, lc->frac_delay);
  // Prefill: integer delay + resampler latency + margin so SFO drift during
  // a burst never underflows.
  L->fifo_n = lc->delay + (int)cm_resamp_delay() + LINK_MARGIN;
  if (L->fifo_n > LINK_FIFO - 2 * SIM_CHUNK) L->fifo_n = LINK_FIFO - 2 * SIM_CHUNK;
  memset(L->fifo, 0, sizeof(cf_t) * L->fifo_n);
  L->quiet = 1;
  L->idle = 0;
}

static void node_reset(int i) {
  node_t *n = &nodes[i];
  node_cfg *c = &ncfg[i];
  int16_t *q = n->txq;
  memset(n, 0, sizeof(*n));
  n->txq = q ? q : calloc(TXQ_SAMPLES * 2, sizeof(int16_t));
  n->rx_lo = n->tx_lo = (long long)fc_hz;
  n->tx_gain_db = -80.0;
  n->man_gain_db = n->agc_gain_db = 71.0;
  n->gain_mode = 1;
  n->xo_corr = SIM_XO_NOMINAL_HZ;
  n->dcxo_coarse = c->dcxo_coarse0;
  n->dcxo_fine = c->dcxo_fine0;
  n->tx_amp = sqrt(cm_from_db(TX_FS_DBM + n->tx_gain_db));
  cm_phase_noise_init(&n->pn, c->pn_linewidth_hz, SIM_FS_NOMINAL);
  cm_rng_seed(&n->rng, seed * 1000003ULL + i);
  for (int j = 0; j < MAX_NODES; j++) {
    link_reset(i, j);
    link_reset(j, i);
  }
}

///////////////////////////////////////////////////////////////////////////////////////
// Messaging
///////////////////////////////////////////////////////////////////////////////////////
static void publish(int node, uint16_t type, const void *a, size_t alen, const void *b, size_t blen, uint32_t count) {
  uint16_t topic = (uint16_t)node;
  sim_hdr h = { SIM_MAGIC_HDR, type, (uint16_t)node, count, nodes[node].rx_seq++ };
  zmq_send(zpub, &topic, sizeof(topic), ZMQ_SNDMORE);
  zmq_msg_t m;
  zmq_msg_init_size(&m, sizeof(h) + alen + blen);
  char *p = zmq_msg_data(&m);
  memcpy(p, &h, sizeof(h));
  if (alen) memcpy(p + sizeof(h), a, alen);
  if (blen) memcpy(p + sizeof(h) + alen, b, blen);
  zmq_msg_send(&m, zpub, 0);
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double sim_t(void) { return sample_time / SIM_FS_NOMINAL; }

// Controls that act on the transmitter are applied at their position in the
// TX sample stream; everything else immediately.
static int ctrl_is_tx_side(uint32_t key) {
  return key == CTRL_TX_GAIN || key == CTRL_TX_LO;
}

static void apply_ctrl(int i, const sim_ctrl *c) {
  node_t *n = &nodes[i];
  switch (c->key) {
    case CTRL_RX_LO:   n->rx_lo = c->ival; break;
    case CTRL_TX_LO:   n->tx_lo = c->ival; break;
    case CTRL_TX_GAIN:
      n->tx_gain_db = c->dval;
      n->tx_amp = sqrt(cm_from_db(TX_FS_DBM + n->tx_gain_db));
      break;
    case CTRL_RX_GAIN: n->man_gain_db = c->dval; if (n->gain_mode == 0) n->agc_gain_db = c->dval; break;
    case CTRL_GAIN_MODE: n->gain_mode = (int)c->ival; break;
    case CTRL_XO_CORR: n->xo_corr = c->ival; break;
    case CTRL_DCXO_COARSE: if (!ncfg[i].ext_refclk) n->dcxo_coarse = (int)c->ival; break;
    case CTRL_DCXO_FINE:   if (!ncfg[i].ext_refclk) n->dcxo_fine = (int)c->ival; break;
    case CTRL_TRACK_BBDC:
    case CTRL_TRACK_RFDC: {
      int was = n->trk_bbdc || n->trk_rfdc;
      if (c->key == CTRL_TRACK_BBDC) n->trk_bbdc = (int)c->ival; else n->trk_rfdc = (int)c->ival;
      if (!was && (n->trk_bbdc || n->trk_rfdc)) n->t_dc_on = sim_t();
      break;
    }
    case CTRL_TRACK_QUAD:
      if (!n->trk_quad && c->ival) n->t_quad_on = sim_t();
      n->trk_quad = (int)c->ival;
      break;
    case CTRL_CAL_TXQUAD: n->txquad_cal = 1; break;
    case CTRL_CAL_RFDC:   n->rfdc_cal = 1; break;
  }
}

static void handle_msg(const uint8_t *p, size_t len) {
  sim_hdr h;
  if (len < sizeof(h)) return;
  memcpy(&h, p, sizeof(h));
  if (h.magic != SIM_MAGIC_HDR || h.node >= n_nodes) return;
  int i = h.node;
  node_t *n = &nodes[i];

  if (h.type == SIM_HELLO) {
    if (!n->connected || n->q_in > 0) {
      node_reset(i);
      fprintf(stderr, "rfchan: node %d attached\n", i);
    }
    n->connected = 1;
    sim_caps caps = { !ncfg[i].ext_refclk, n->dcxo_coarse, n->dcxo_fine, 0, n->xo_corr };
    publish(i, SIM_CAPS, &caps, sizeof(caps), NULL, 0, 0);
  } else if (h.type == SIM_TX && len >= sizeof(h) + h.count * 4) {
    if (n->q_in - n->q_out + h.count > TXQ_SAMPLES) {
      fprintf(stderr, "rfchan: node %d TX queue overflow\n", i);
      return;
    }
    const int16_t *s = (const int16_t *)(p + sizeof(h));
    for (uint32_t k = 0; k < h.count; k++) {
      int64_t pos = (n->q_in + k) & (TXQ_SAMPLES - 1);
      n->txq[pos * 2] = s[k * 2];
      n->txq[pos * 2 + 1] = s[k * 2 + 1];
    }
    n->q_in += h.count;
  } else if (h.type == SIM_CTRL && len >= sizeof(h) + sizeof(sim_ctrl)) {
    sim_ctrl c;
    memcpy(&c, p + sizeof(h), sizeof(c));
    if (ctrl_is_tx_side(c.key) && n->q_in > n->q_out) {
      int nt = (n->ev_tail + 1) % TXQ_EVENTS;
      if (nt == n->ev_head) { apply_ctrl(i, &c); return; }
      n->ev[n->ev_tail].pos = n->q_in;
      n->ev[n->ev_tail].c = c;
      n->ev_tail = nt;
    } else {
      // RX-side control, or TX queue empty.  If the queue is empty any
      // pending TX events are due now (keeps their order before this one).
      if (n->q_in == n->q_out) {
        while (n->ev_head != n->ev_tail) {
          apply_ctrl(i, &n->ev[n->ev_head].c);
          n->ev_head = (n->ev_head + 1) % TXQ_EVENTS;
        }
      }
      apply_ctrl(i, &c);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////
// One channel tick (SIM_CHUNK samples)
///////////////////////////////////////////////////////////////////////////////////////
static double ramp_suppress_db(double full_db, double t_on, double time_ms) {
  double dt = (sim_t() - t_on) * 1e3;
  if (time_ms <= 0 || dt >= time_ms) return full_db;
  return dt <= 0 ? 0 : full_db * dt / time_ms;
}

static void build_tx(int i) {
  node_t *n = &nodes[i];
  node_cfg *c = &ncfg[i];
  n->tx_active = 0;
  for (int k = 0; k < SIM_CHUNK; k++) {
    while (n->ev_head != n->ev_tail && n->ev[n->ev_head].pos <= n->q_out) {
      apply_ctrl(i, &n->ev[n->ev_head].c);
      n->ev_head = (n->ev_head + 1) % TXQ_EVENTS;
    }
    if (n->q_out < n->q_in) {
      if (n->tx_dry && n->tx_dry < 3 * SIM_CHUNK) n->tx_gaps++;
      n->tx_dry = 0;
      int64_t pos = n->q_out & (TXQ_SAMPLES - 1);
      float re = n->txq[pos * 2] / 32768.0f, im = n->txq[pos * 2 + 1] / 32768.0f;
      n->txc[k] = (re + _Complex_I * im) * (float)n->tx_amp;
      n->q_out++;
      n->tx_active = 1;
    } else {
      n->txc[k] = 0.0f;
      if (n->tx_active || n->tx_dry) n->tx_dry++;
    }
  }
  if (!n->tx_active) return;
  n->tx_samples += SIM_CHUNK;

  // TX impairments (scaled by tx_amp already applied -> apply relative)
  double cal = n->txquad_cal ? c->txcal_suppress_db : 0.0;
  cm_iq_init(&n->tx_iq, c->tx_iq_gain_db, c->tx_iq_phase_deg);
  n->tx_iq.nu *= (float)pow(10.0, -cal / 20.0);
  cm_iq_apply(&n->tx_iq, n->txc, SIM_CHUNK);
  if (isfinite(c->tx_lo_leak_dbc)) {
    float leak = (float)(n->tx_amp * pow(10.0, (c->tx_lo_leak_dbc - cal) / 20.0));
    for (int k = 0; k < SIM_CHUNK; k++) n->txc[k] += leak;
  }
}

static void run_link(int t, int r, cf_t *acc) {
  link_t *L = &links[t][r];
  link_cfg *lc = &lcfg[t][r];
  node_t *nt = &nodes[t], *nr = &nodes[r];
  double loss = (t == r) ? ncfg[r].self_isolation_db : lc->loss_db;
  if (!isfinite(loss)) return;

  if (!nt->tx_active) {
    if (L->quiet) return;
    L->idle += SIM_CHUNK;
  } else {
    L->idle = 0;
    L->quiet = 0;
  }

  static cf_t x[SIM_CHUNK], y[SIM_CHUNK + 64];
  float g = (float)pow(10.0, -loss / 20.0);
  for (int k = 0; k < SIM_CHUNK; k++) x[k] = nt->txc[k] * g;
  if (t != r) cm_fir_run(&L->fir, x, SIM_CHUNK);

  double kt = node_k(t), kr = node_k(r);
  cm_resamp_set_ratio(&L->rs, t == r ? 1.0 : kr / kt);
  int ny = cm_resamp_run(&L->rs, x, SIM_CHUNK, y, SIM_CHUNK + 64);

  // CFO in the receiver's sample domain, then both LOs' phase noise
  double cfo = (double)nt->tx_lo * kt - (double)nr->rx_lo * kr;
  cm_nco_set_freq(&L->nco, cfo, SIM_FS_NOMINAL * kr);
  cm_nco_mix(&L->nco, y, ny);
  if (ncfg[t].pn_linewidth_hz > 0 || ncfg[r].pn_linewidth_hz > 0)
    for (int k = 0; k < ny && k < SIM_CHUNK; k++)
      y[k] *= cexpf(_Complex_I * (float)(nt->ph[k] - nr->ph[k]));

  if (L->fifo_n + ny > LINK_FIFO) ny = LINK_FIFO - L->fifo_n;
  memcpy(&L->fifo[L->fifo_n], y, ny * sizeof(cf_t));
  L->fifo_n += ny;

  int take = L->fifo_n < SIM_CHUNK ? L->fifo_n : SIM_CHUNK;
  if (take < SIM_CHUNK) nr->underflows++;
  // A receiver tuned more than half a sample rate away sees nothing.
  int tuned = llabs(nt->tx_lo - nr->rx_lo) < (long long)(SIM_FS_NOMINAL / 2);
  if (tuned) {
    for (int k = 0; k < take; k++) acc[k] += L->fifo[k];
    if (nt->tx_active && t != r) {
      L->sig_sum += cm_power(L->fifo, take);
      L->sig_n++;
    }
  }
  L->fifo_n -= take;
  memmove(L->fifo, &L->fifo[take], L->fifo_n * sizeof(cf_t));

  // Long idle: re-anchor the link timeline (drops accumulated SFO drift).
  if (L->idle > 4 * SIM_CHUNK) link_reset(t, r);
}

static void run_rx(int r) {
  node_t *n = &nodes[r];
  node_cfg *c = &ncfg[r];
  static cf_t acc[SIM_CHUNK];
  memset(acc, 0, sizeof(acc));

  for (int t = 0; t < n_nodes; t++) run_link(t, r, acc);

  // Interferers (relative to this receiver's LO)
  for (int k = 0; k < n_interferers; k++) {
    interferer_cfg *ic = &icfg[k];
    if (!isfinite(ic->power_dbm) || (ic->rx >= 0 && ic->rx != r)) continue;
    double amp = sqrt(cm_from_db(ic->power_dbm));
    double period = ic->period_ms * 1e-3 * SIM_FS_NOMINAL;
    for (int s = 0; s < SIM_CHUNK; s++) {
      double ts = (double)(sample_time + s);
      if (fmod(ts, period) >= ic->duty * period) continue;
      double ph = 2.0 * M_PI * fmod(ic->offset_hz * ts / SIM_FS_NOMINAL, 1.0);
      acc[s] += (float)amp * cexpf(_Complex_I * (float)ph);
    }
  }

  // Thermal noise referred to the antenna
  double noise_mw = cm_from_db(-174.0 + 10.0 * log10(SIM_FS_NOMINAL) + c->nf_db);
  cm_awgn(acc, SIM_CHUNK, noise_mw, &n->rng);

  double pin = cm_power(acc, SIM_CHUNK);
  double pin_dbm = cm_db(pin);
  n->rssi_sum += pin_dbm;
  n->rssi_n++;

  // Gain / AGC
  double g = n->gain_mode == 0 ? n->man_gain_db : n->agc_gain_db;
  if (n->gain_mode != 0) {
    double padc = pin_dbm + g - ADC_FS_DBM;
    if (padc > AGC_HIGH_DBFS) g -= padc - AGC_TARGET_DBFS;          // fast attack
    else if (padc < AGC_LOW_DBFS) g += 1.0;                         // slow decay
    if (g > GAIN_MAX_DB) g = GAIN_MAX_DB;
    if (g < GAIN_MIN_DB) g = GAIN_MIN_DB;
    n->agc_gain_db = g;
  }
  float scale = (float)sqrt(cm_from_db(g - ADC_FS_DBM));
  for (int s = 0; s < SIM_CHUNK; s++) acc[s] *= scale;

  // RX IQ imbalance with quadrature tracking
  double qsup = (c->honour_tracking && n->trk_quad) ?
                ramp_suppress_db(c->track_suppress_db, n->t_quad_on, c->track_time_ms) : 0.0;
  cm_iq_init(&n->rx_iq, c->rx_iq_gain_db, c->rx_iq_phase_deg);
  n->rx_iq.nu *= (float)pow(10.0, -qsup / 20.0);
  cm_iq_apply(&n->rx_iq, acc, SIM_CHUNK);

  // DC offset with DC tracking / RF DC calibration
  if (isfinite(c->rx_dc_dbfs)) {
    double dsup = 0.0;
    if (c->honour_tracking) {
      if (n->trk_bbdc || n->trk_rfdc) dsup = ramp_suppress_db(c->track_suppress_db, n->t_dc_on, c->track_time_ms);
      if (n->rfdc_cal && dsup < c->track_suppress_db / 2) dsup = c->track_suppress_db / 2;
    }
    double a = pow(10.0, (c->rx_dc_dbfs - dsup) / 20.0);
    cf_t dc = (float)a * cexpf(_Complex_I * (float)(c->rx_dc_phase_deg * M_PI / 180.0));
    for (int s = 0; s < SIM_CHUNK; s++) acc[s] += dc;
  }

  n->clipped += cm_adc(acc, SIM_CHUNK, ADC_BITS);

  static int16_t out[SIM_CHUNK * 2];
  for (int s = 0; s < SIM_CHUNK; s++) {
    out[s * 2] = (int16_t)lrintf(crealf(acc[s]) * 32768.0f);
    out[s * 2 + 1] = (int16_t)lrintf(cimagf(acc[s]) * 32768.0f);
  }
  if (n->connected) {
    sim_rxinfo info = { sample_time, pin_dbm, g };
    publish(r, SIM_RX, &info, sizeof(info), out, sizeof(out), SIM_CHUNK);
  }
}

static void tick(void) {
  // Oscillator drift (random walk) and phase noise for this tick
  double dt = SIM_CHUNK / SIM_FS_NOMINAL;
  for (int i = 0; i < n_nodes; i++) {
    if (ncfg[i].drift_ppm_per_rts > 0)
      nodes[i].walk_ppm += ncfg[i].drift_ppm_per_rts * sqrt(dt) * cm_rng_gauss(&chan_rng);
    cm_phase_noise_run(&nodes[i].pn, nodes[i].ph, SIM_CHUNK, &chan_rng);
    build_tx(i);
  }
  for (int r = 0; r < n_nodes; r++) run_rx(r);
  sample_time += SIM_CHUNK;
}

///////////////////////////////////////////////////////////////////////////////////////
// Ground truth log (JSON lines, once per second)
///////////////////////////////////////////////////////////////////////////////////////
static void log_truth(void) {
  if (!gt) return;
  double t = sim_t();
  double noise_dbm = 0;
  for (int i = 0; i < n_nodes; i++) {
    node_t *n = &nodes[i];
    node_cfg *c = &ncfg[i];
    double k = node_k(i);
    fprintf(gt, "{\"t\":%.3f,\"node\":%d,\"connected\":%d,\"eff_ppm\":%.5f,\"dcxo_ppm\":%.5f,"
                "\"dcxo_coarse\":%d,\"dcxo_fine\":%d,\"xo_corr\":%lld,\"rx_gain_db\":%.1f,"
                "\"rssi_dbm\":%.1f,\"rx_lo\":%lld,\"tx_lo\":%lld,\"tx_samples\":%ld,"
                "\"clipped\":%ld,\"underflows\":%ld,\"tx_gaps\":%ld,\"trk\":[%d,%d,%d],\"cal\":[%d,%d]}\n",
            t, i, n->connected, (k - 1.0) * 1e6, node_dcxo_ppm(n, c),
            n->dcxo_coarse, n->dcxo_fine, n->xo_corr,
            n->gain_mode == 0 ? n->man_gain_db : n->agc_gain_db,
            n->rssi_n ? n->rssi_sum / n->rssi_n : -300.0, n->rx_lo, n->tx_lo, n->tx_samples,
            n->clipped, n->underflows, n->tx_gaps, n->trk_bbdc, n->trk_rfdc, n->trk_quad, n->rfdc_cal, n->txquad_cal);
    n->rssi_sum = 0; n->rssi_n = 0;
  }
  for (int a = 0; a < n_nodes; a++)
    for (int b = 0; b < n_nodes; b++) {
      if (a == b || !isfinite(lcfg[a][b].loss_db)) continue;
      link_t *L = &links[a][b];
      noise_dbm = -174.0 + 10.0 * log10(SIM_FS_NOMINAL) + ncfg[b].nf_db;
      double sig_dbm = L->sig_n ? cm_db(L->sig_sum / L->sig_n) : -300.0;
      // CFO the receiver b sees from a at the nominal carrier
      double cfo = fc_hz * (node_k(a) - node_k(b));
      fprintf(gt, "{\"t\":%.3f,\"link\":[%d,%d],\"cfo_hz\":%.2f,\"sfo_ppm\":%.5f,"
                  "\"sig_dbm\":%.1f,\"snr_db\":%.1f}\n",
              t, a, b, cfo, (node_k(a) / node_k(b) - 1.0) * 1e6, sig_dbm,
              L->sig_n ? sig_dbm - noise_dbm : -300.0);
      L->sig_sum = 0; L->sig_n = 0;
    }
  fflush(gt);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
static void on_signal(int s) { (void)s; stop = 1; }

static void usage(void) {
  fprintf(stderr, "usage: rfchan [-c config] [-s key=value]... [-t seconds] [-g truth.jsonl]\n");
}

int main(int argc, char **argv) {
  set_defaults();
  int opt;
  while ((opt = getopt(argc, argv, "c:s:t:g:h")) != -1) {
    switch (opt) {
      case 'c': if (load_config(optarg)) return 2; break;
      case 's': if (apply_kv(optarg)) return 2; break;
      case 't': duration_s = atof(optarg); break;
      case 'g': snprintf(gt_path, sizeof(gt_path), "%s", optarg); break;
      default: usage(); return 2;
    }
  }

  cm_rng_seed(&chan_rng, seed);
  for (int i = 0; i < n_nodes; i++) node_reset(i);
  if (gt_path[0] && !(gt = fopen(gt_path, "w"))) { perror(gt_path); return 1; }

  void *zctx = zmq_ctx_new();
  zpull = zmq_socket(zctx, ZMQ_PULL);
  zpub = zmq_socket(zctx, ZMQ_PUB);
  int hwm = 8000;
  zmq_setsockopt(zpub, ZMQ_SNDHWM, &hwm, sizeof(hwm));
  if (zmq_bind(zpull, tx_ep) || zmq_bind(zpub, rx_ep)) {
    fprintf(stderr, "rfchan: bind failed (%s / %s): %s\n", tx_ep, rx_ep, zmq_strerror(errno));
    return 1;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  fprintf(stderr, "rfchan: %d nodes, fc=%.0f Hz, fs=%.0f, tx=%s rx=%s seed=%llu\n",
          n_nodes, fc_hz, SIM_FS_NOMINAL, tx_ep, rx_ep, (unsigned long long)seed);

  const double tick_s = SIM_CHUNK / SIM_FS_NOMINAL;
  double t_start = now_s(), t_next = t_start, t_report = 1.0;
  long overruns = 0;
  double max_lag = 0;
  zmq_msg_t m;
  zmq_msg_init(&m);
  while (!stop) {
    double now = now_s();
    int wait_ms = (int)((t_next - now) * 1e3);
    zmq_pollitem_t it = { zpull, 0, ZMQ_POLLIN, 0 };
    if (zmq_poll(&it, 1, wait_ms > 0 ? wait_ms : 0) > 0) {
      while (zmq_msg_recv(&m, zpull, ZMQ_DONTWAIT) >= 0)
        handle_msg(zmq_msg_data(&m), zmq_msg_size(&m));
    }
    now = now_s();
    if (now < t_next) {
      // zmq_poll only has ms resolution: sleep out the sub-ms remainder
      // in short steps instead of spinning.
      double rem = t_next - now;
      if (rem < 0.001) {
        struct timespec ts = { 0, (long)((rem < 0.0002 ? rem : 0.0002) * 1e9) };
        nanosleep(&ts, NULL);
      }
      continue;
    }

    tick();
    t_next += tick_s;
    // Behind real time: catch up tick by tick (no samples lost, nodes just
    // see a latency hiccup).  Only if hopelessly behind, resync and count
    // it -- that drops channel time and is reported as an overrun.
    double lag = now - t_next;
    if (lag > max_lag) max_lag = lag;
    if (lag > 1.0) {
      overruns++;
      fprintf(stderr, "rfchan: overrun at t=%.2f s, %.0f ms behind real time, resyncing\n", sim_t(), lag * 1e3);
      t_next = now;
    }
    if (sim_t() >= t_report) {
      log_truth();
      t_report += 1.0;
    }
    if (duration_s > 0 && sim_t() >= duration_s) break;
  }
  zmq_msg_close(&m);
  fprintf(stderr, "rfchan: done, %.1f s simulated, %ld overruns, max lag %.1f ms\n",
          sim_t(), overruns, max_lag * 1e3);
  if (gt) fclose(gt);
  zmq_close(zpull);
  zmq_close(zpub);
  zmq_ctx_destroy(zctx);
  return 0;
}
