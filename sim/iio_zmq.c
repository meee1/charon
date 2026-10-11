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

// Fake libiio + libad9361 backed by the rfchan ZMQ channel.
//
// pluto.c is compiled unmodified against the real <iio.h>/<ad9361.h>
// headers and linked against this file instead of libiio, so everything it
// does -- gain/AGC, LO tuning, DC/IQ tracking and calibration, xo_correction,
// DCXO tuning, buffer refill/push -- goes through the same calls as on a
// Pluto and lands in the channel simulator.
//
// Environment:
//   CHARON_SIM_NODE    node id (required, matches the rfchan config)
//   CHARON_SIM_TX_EP   rfchan PULL endpoint (default ipc:///tmp/charon_sim_tx)
//   CHARON_SIM_RX_EP   rfchan PUB endpoint  (default ipc:///tmp/charon_sim_rx)
//   CHARON_SIM_RX_DUMP optional file: every RX sample (int16 IQ) as received

#define _GNU_SOURCE 1
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include <iio.h>
#include <ad9361.h>
#include <zmq.h>

#include "sim_proto.h"

///////////////////////////////////////////////////////////////////////////////////////
// Object model: three devices, a handful of channels, two buffers.
enum { DEV_PHY, DEV_RX, DEV_TX };
enum { CH_VOLT0, CH_VOLT1, CH_ALT0, CH_ALT1 };

struct iio_context { int dummy; };
struct iio_device  { int kind; };
struct iio_channel { struct iio_device *dev; int kind; bool output; };
struct iio_buffer  {
  struct iio_device *dev;
  int16_t *data;
  size_t   samples;
  bool     blocking;
};

static struct iio_context ctx_obj;
static struct iio_device  dev_phy = {DEV_PHY}, dev_rx = {DEV_RX}, dev_tx = {DEV_TX};
static struct iio_channel chans[3][4][2];   // [dev][kind][output]

///////////////////////////////////////////////////////////////////////////////////////
// Radio state mirrored on the node side.
static void *zctx, *zpush, *zsub;
static int    node_id;
static uint32_t tx_seq;
static uint32_t rx_seq_expect;
static int    rx_seq_valid;
static unsigned long rx_dropped_chunks;

static long long rx_lo_hz = 2400000000LL, tx_lo_hz = 2450000000LL;
static long long sample_hz = 1400000, rx_bw = 1400000, tx_bw = 1400000;
static double    tx_gain_db = -10.0;
static double    manual_gain_db = 71.0;
static int       gain_mode = 1;              // fast_attack
static double    agc_gain_db = 71.0;
static double    rssi_dbm = -110.0;
static int       trk_bbdc = 1, trk_rfdc = 1, trk_quad = 1;
static char      calib_mode[32] = "auto";
static int       dcxo_avail;
static long long dcxo_coarse, dcxo_fine, xo_corr = SIM_XO_NOMINAL_HZ;
static bool      fir_en;

// RX staging: chunks from the channel accumulate here until a full IIO
// buffer can be returned (non-blocking refill semantics).
#define RX_STAGE_SAMPLES (65536)
static int16_t rx_stage[RX_STAGE_SAMPLES * 2];
static size_t  rx_stage_n;

// TX pacing: model the kernel's 4 DMA blocks; push blocks once more than
// that much airtime is queued ahead of real time.
#define TX_KERNEL_BUFFERS 4
static double tx_busy_until;
static FILE  *rx_dump;

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void send_msg(uint16_t type, const void *body, size_t body_len, uint32_t count) {
  sim_hdr h = { SIM_MAGIC_HDR, type, (uint16_t)node_id, count, tx_seq++ };
  zmq_msg_t m;
  zmq_msg_init_size(&m, sizeof(h) + body_len);
  memcpy(zmq_msg_data(&m), &h, sizeof(h));
  if (body_len) memcpy((char *)zmq_msg_data(&m) + sizeof(h), body, body_len);
  zmq_msg_send(&m, zpush, 0);
}

static void send_ctrl(uint32_t key, int64_t ival, double dval) {
  sim_ctrl c = { key, 0, ival, dval };
  send_msg(SIM_CTRL, &c, sizeof(c), 0);
}

// Receive and handle one PUB message.  Returns 1 if something was read.
static int pump_one(int timeout_ms) {
  zmq_pollitem_t it = { zsub, 0, ZMQ_POLLIN, 0 };
  if (zmq_poll(&it, 1, timeout_ms) <= 0) return 0;

  zmq_msg_t topic, body;
  zmq_msg_init(&topic);
  zmq_msg_init(&body);
  if (zmq_msg_recv(&topic, zsub, ZMQ_DONTWAIT) < 0) { zmq_msg_close(&topic); zmq_msg_close(&body); return 0; }
  int more = zmq_msg_more(&topic);
  zmq_msg_close(&topic);
  if (!more || zmq_msg_recv(&body, zsub, 0) < 0) { zmq_msg_close(&body); return 1; }

  const uint8_t *p = zmq_msg_data(&body);
  size_t len = zmq_msg_size(&body);
  sim_hdr h;
  if (len < sizeof(h)) goto done;
  memcpy(&h, p, sizeof(h));
  if (h.magic != SIM_MAGIC_HDR) goto done;

  if (h.type == SIM_CAPS && len >= sizeof(h) + sizeof(sim_caps)) {
    sim_caps c;
    memcpy(&c, p + sizeof(h), sizeof(c));
    dcxo_avail = c.dcxo_avail;
    dcxo_coarse = c.dcxo_coarse;
    dcxo_fine = c.dcxo_fine;
    xo_corr = c.xo_corr;
  } else if (h.type == SIM_RX && len >= sizeof(h) + sizeof(sim_rxinfo) + h.count * 4) {
    sim_rxinfo info;
    memcpy(&info, p + sizeof(h), sizeof(info));
    rssi_dbm = info.rssi_dbm;
    agc_gain_db = info.rx_gain_db;
    if (rx_seq_valid && h.seq != rx_seq_expect) {
      rx_dropped_chunks += (uint32_t)(h.seq - rx_seq_expect);
      fprintf(stderr, "\n[iio_zmq] RX overflow: %u chunks dropped (total %lu)",
              (unsigned)(h.seq - rx_seq_expect), rx_dropped_chunks);
    }
    rx_seq_expect = h.seq + 1;
    rx_seq_valid = 1;
    if (rx_stage_n + h.count > RX_STAGE_SAMPLES) {
      // Node isn't keeping up: drop the oldest data like an IIO overflow.
      rx_dropped_chunks++;
      rx_stage_n = 0;
    }
    memcpy(&rx_stage[rx_stage_n * 2], p + sizeof(h) + sizeof(info), h.count * 4);
    if (rx_dump) fwrite(p + sizeof(h) + sizeof(info), 4, h.count, rx_dump);
    rx_stage_n += h.count;
  }
done:
  zmq_msg_close(&body);
  return 1;
}

///////////////////////////////////////////////////////////////////////////////////////
// Context / device / channel lookup
///////////////////////////////////////////////////////////////////////////////////////
struct iio_context *iio_create_local_context(void) {
  const char *v;
  const char *tx_ep = (v = getenv("CHARON_SIM_TX_EP")) ? v : SIM_DEFAULT_TX_EP;
  const char *rx_ep = (v = getenv("CHARON_SIM_RX_EP")) ? v : SIM_DEFAULT_RX_EP;
  node_id = (v = getenv("CHARON_SIM_NODE")) ? atoi(v) : 0;

  for (int d = 0; d < 3; d++)
    for (int k = 0; k < 4; k++)
      for (int o = 0; o < 2; o++) {
        chans[d][k][o].dev = d == DEV_PHY ? &dev_phy : d == DEV_RX ? &dev_rx : &dev_tx;
        chans[d][k][o].kind = k;
        chans[d][k][o].output = o;
      }

  zctx = zmq_ctx_new();
  zpush = zmq_socket(zctx, ZMQ_PUSH);
  zsub = zmq_socket(zctx, ZMQ_SUB);
  int hwm = 4000;   // ~4 s of 1 ms RX chunks
  zmq_setsockopt(zsub, ZMQ_RCVHWM, &hwm, sizeof(hwm));
  uint16_t topic = (uint16_t)node_id;
  zmq_setsockopt(zsub, ZMQ_SUBSCRIBE, &topic, sizeof(topic));
  if (zmq_connect(zpush, tx_ep) != 0 || zmq_connect(zsub, rx_ep) != 0) {
    fprintf(stderr, "\n[iio_zmq] connect failed (%s / %s): %s", tx_ep, rx_ep, zmq_strerror(errno));
    return NULL;
  }

  // HELLO until the channel answers with CAPS (PUB/SUB joins are async).
  fprintf(stderr, "\n[iio_zmq] node %d connecting to rfchan %s / %s", node_id, tx_ep, rx_ep);
  double t_end = now_s() + 15.0, t_hello = 0;
  dcxo_avail = -1;
  while (dcxo_avail < 0) {
    if (now_s() > t_end) {
      fprintf(stderr, "\n[iio_zmq] no answer from rfchan");
      return NULL;
    }
    if (now_s() - t_hello > 0.2) {
      send_msg(SIM_HELLO, NULL, 0, 0);
      t_hello = now_s();
    }
    pump_one(50);
  }
  rx_stage_n = 0;
  if ((v = getenv("CHARON_SIM_RX_DUMP"))) rx_dump = fopen(v, "wb");
  fprintf(stderr, "\n[iio_zmq] node %d attached: dcxo_avail=%d coarse=%lld fine=%lld xo=%lld",
          node_id, dcxo_avail, dcxo_coarse, dcxo_fine, xo_corr);
  return &ctx_obj;
}

struct iio_device *iio_context_find_device(const struct iio_context *ctx, const char *name) {
  (void)ctx;
  if (!strcmp(name, "ad9361-phy")) return &dev_phy;
  if (!strcmp(name, "cf-ad9361-lpc")) return &dev_rx;
  if (!strcmp(name, "cf-ad9361-dds-core-lpc")) return &dev_tx;
  return NULL;
}

struct iio_channel *iio_device_find_channel(const struct iio_device *dev, const char *name, bool output) {
  int k;
  if (!strcmp(name, "voltage0")) k = CH_VOLT0;
  else if (!strcmp(name, "voltage1")) k = CH_VOLT1;
  else if (!strcmp(name, "altvoltage0")) k = CH_ALT0;
  else if (!strcmp(name, "altvoltage1")) k = CH_ALT1;
  else return NULL;
  return &chans[dev->kind][k][output ? 1 : 0];
}

void iio_channel_enable(struct iio_channel *chn) { (void)chn; }

///////////////////////////////////////////////////////////////////////////////////////
// Attributes
///////////////////////////////////////////////////////////////////////////////////////
// Channel attribute read.  Returns 0 and fills *v and/or str, or -errno.
static int chn_attr_get(const struct iio_channel *c, const char *attr, long long *v, char *str, size_t len) {
  long long val = 0;
  if (c->dev->kind != DEV_PHY) return -ENOENT;

  if (c->kind == CH_ALT0 && !strcmp(attr, "frequency")) val = rx_lo_hz;
  else if (c->kind == CH_ALT1 && !strcmp(attr, "frequency")) val = tx_lo_hz;
  else if (c->kind == CH_VOLT0 && !c->output) {
    if (!strcmp(attr, "hardwaregain")) {
      double g = gain_mode == 0 ? manual_gain_db : agc_gain_db;
      if (str) snprintf(str, len, "%.6f dB", g);
      if (v) *v = (long long)g;
      return 0;
    } else if (!strcmp(attr, "rssi")) {
      if (str) snprintf(str, len, "%.2f dB", -rssi_dbm);
      if (v) *v = (long long)(-rssi_dbm);
      return 0;
    } else if (!strcmp(attr, "gain_control_mode")) {
      if (str) snprintf(str, len, "%s", gain_mode == 0 ? "manual" : gain_mode == 1 ? "fast_attack" : "slow_attack");
      return 0;
    }
    else if (!strcmp(attr, "sampling_frequency")) val = sample_hz;
    else if (!strcmp(attr, "rf_bandwidth")) val = rx_bw;
    else if (!strcmp(attr, "bb_dc_offset_tracking_en")) val = trk_bbdc;
    else if (!strcmp(attr, "rf_dc_offset_tracking_en")) val = trk_rfdc;
    else if (!strcmp(attr, "quadrature_tracking_en")) val = trk_quad;
    else return -ENOENT;
  } else if (c->kind == CH_VOLT0 && c->output) {
    if (!strcmp(attr, "hardwaregain")) {
      if (str) snprintf(str, len, "%.6f dB", tx_gain_db);
      if (v) *v = (long long)tx_gain_db;
      return 0;
    }
    else if (!strcmp(attr, "rf_bandwidth")) val = tx_bw;
    else if (!strcmp(attr, "sampling_frequency")) val = sample_hz;
    else return -ENOENT;
  } else {
    return -ENOENT;
  }
  if (v) *v = val;
  if (str) snprintf(str, len, "%lld", val);
  return 0;
}

static int chn_attr_set(const struct iio_channel *c, const char *attr, const char *s) {
  long long val = strtoll(s, NULL, 10);
  if (c->dev->kind != DEV_PHY) return -ENOENT;

  if (c->kind == CH_ALT0 && !strcmp(attr, "frequency")) {
    rx_lo_hz = val;
    send_ctrl(CTRL_RX_LO, val, 0);
  } else if (c->kind == CH_ALT1 && !strcmp(attr, "frequency")) {
    tx_lo_hz = val;
    send_ctrl(CTRL_TX_LO, val, 0);
  } else if (c->kind == CH_VOLT0 && !c->output) {
    if (!strcmp(attr, "hardwaregain")) {
      if (gain_mode != 0) return -EINVAL;   // like the driver: only in manual
      manual_gain_db = (double)val;
      send_ctrl(CTRL_RX_GAIN, 0, manual_gain_db);
    } else if (!strcmp(attr, "gain_control_mode")) {
      int m = !strncmp(s, "manual", 6) ? 0 : !strncmp(s, "fast_attack", 11) ? 1 : 2;
      if (m == 0 && gain_mode != 0) {
        manual_gain_db = agc_gain_db;   // driver keeps the current gain
        send_ctrl(CTRL_RX_GAIN, 0, manual_gain_db);
      }
      gain_mode = m;
      send_ctrl(CTRL_GAIN_MODE, m, 0);
    }
    else if (!strcmp(attr, "sampling_frequency")) {
      sample_hz = val;
      if (val != (long long)SIM_FS_NOMINAL)
        fprintf(stderr, "\n[iio_zmq] WARNING: sample rate %lld unsupported by simulator (fixed %.0f)", val, SIM_FS_NOMINAL);
    }
    else if (!strcmp(attr, "rf_bandwidth")) rx_bw = val;
    else if (!strcmp(attr, "bb_dc_offset_tracking_en")) { trk_bbdc = val != 0; send_ctrl(CTRL_TRACK_BBDC, trk_bbdc, 0); }
    else if (!strcmp(attr, "rf_dc_offset_tracking_en")) { trk_rfdc = val != 0; send_ctrl(CTRL_TRACK_RFDC, trk_rfdc, 0); }
    else if (!strcmp(attr, "quadrature_tracking_en"))   { trk_quad = val != 0; send_ctrl(CTRL_TRACK_QUAD, trk_quad, 0); }
    else return -ENOENT;
  } else if (c->kind == CH_VOLT0 && c->output) {
    if (!strcmp(attr, "hardwaregain")) {
      double g = strtod(s, NULL);
      if (g > 0) g = 0;
      if (g < -89.75) g = -89.75;
      tx_gain_db = g;
      send_ctrl(CTRL_TX_GAIN, 0, g);
    }
    else if (!strcmp(attr, "rf_bandwidth")) tx_bw = val;
    else if (!strcmp(attr, "sampling_frequency")) sample_hz = val;
    else return -ENOENT;
  } else {
    return -ENOENT;
  }
  return 0;
}

ssize_t iio_channel_attr_write(const struct iio_channel *chn, const char *attr, const char *src) {
  int r = chn_attr_set(chn, attr, src);
  return r < 0 ? r : (ssize_t)strlen(src) + 1;
}

int iio_channel_attr_write_longlong(const struct iio_channel *chn, const char *attr, long long val) {
  char s[32];
  snprintf(s, sizeof(s), "%lld", val);
  return chn_attr_set(chn, attr, s);
}

int iio_channel_attr_write_bool(const struct iio_channel *chn, const char *attr, bool val) {
  return chn_attr_set(chn, attr, val ? "1" : "0");
}

int iio_channel_attr_read_longlong(const struct iio_channel *chn, const char *attr, long long *val) {
  return chn_attr_get(chn, attr, val, NULL, 0);
}

int iio_channel_attr_read_bool(const struct iio_channel *chn, const char *attr, bool *val) {
  long long v;
  int r = chn_attr_get(chn, attr, &v, NULL, 0);
  if (r == 0) *val = v != 0;
  return r;
}

// Device attributes (ad9361-phy)
static int dev_attr_get(const struct iio_device *d, const char *attr, long long *v, char *str, size_t len) {
  if (d->kind != DEV_PHY) return -ENOENT;
  long long val;
  if (!strcmp(attr, "xo_correction")) val = xo_corr;
  else if (!strcmp(attr, "dcxo_tune_coarse")) { if (!dcxo_avail) return -ENODEV; val = dcxo_coarse; }
  else if (!strcmp(attr, "dcxo_tune_fine"))   { if (!dcxo_avail) return -ENODEV; val = dcxo_fine; }
  else if (!strcmp(attr, "in_voltage_filter_fir_en") || !strcmp(attr, "out_voltage_filter_fir_en")) val = fir_en;
  else if (!strcmp(attr, "calib_mode")) { if (str) snprintf(str, len, "%s", calib_mode); return 0; }
  else if (!strcmp(attr, "filter_fir_config")) {
    if (str) snprintf(str, len, "TX 3 GAIN 0 INT 1\nRX 3 GAIN 0 DEC 1\n");
    return 0;
  } else if (!strcmp(attr, "rx_path_rates")) {
    if (str) snprintf(str, len, "SIM RXSAMP:%lld", sample_hz);
    return 0;
  } else if (!strcmp(attr, "tx_path_rates")) {
    if (str) snprintf(str, len, "SIM TXSAMP:%lld", sample_hz);
    return 0;
  }
  else return -ENOENT;
  if (v) *v = val;
  if (str) snprintf(str, len, "%lld", val);
  return 0;
}

static int dev_attr_set(const struct iio_device *d, const char *attr, const char *s) {
  if (d->kind != DEV_PHY) return -ENOENT;
  long long val = strtoll(s, NULL, 10);
  if (!strcmp(attr, "xo_correction")) {
    xo_corr = val;
    send_ctrl(CTRL_XO_CORR, val, 0);
  } else if (!strcmp(attr, "dcxo_tune_coarse")) {
    if (!dcxo_avail) return -ENODEV;
    dcxo_coarse = val < 0 ? 0 : val > 63 ? 63 : val;
    send_ctrl(CTRL_DCXO_COARSE, dcxo_coarse, 0);
  } else if (!strcmp(attr, "dcxo_tune_fine")) {
    if (!dcxo_avail) return -ENODEV;
    dcxo_fine = val < 0 ? 0 : val > 8191 ? 8191 : val;
    send_ctrl(CTRL_DCXO_FINE, dcxo_fine, 0);
  } else if (!strcmp(attr, "calib_mode")) {
    if (!strncmp(s, "tx_quad", 7)) send_ctrl(CTRL_CAL_TXQUAD, 1, 0);
    else if (!strncmp(s, "rf_dc_offs", 10)) send_ctrl(CTRL_CAL_RFDC, 1, 0);
    else snprintf(calib_mode, sizeof(calib_mode), "%s", s);
  } else {
    return -ENOENT;
  }
  return 0;
}

ssize_t iio_device_attr_read(const struct iio_device *dev, const char *attr, char *dst, size_t len) {
  int r = dev_attr_get(dev, attr, NULL, dst, len);
  return r < 0 ? r : (ssize_t)strlen(dst);
}

int iio_device_attr_read_longlong(const struct iio_device *dev, const char *attr, long long *val) {
  return dev_attr_get(dev, attr, val, NULL, 0);
}

int iio_device_attr_read_bool(const struct iio_device *dev, const char *attr, bool *val) {
  long long v;
  int r = dev_attr_get(dev, attr, &v, NULL, 0);
  if (r == 0) *val = v != 0;
  return r;
}

ssize_t iio_device_attr_write(const struct iio_device *dev, const char *attr, const char *src) {
  int r = dev_attr_set(dev, attr, src);
  return r < 0 ? r : (ssize_t)strlen(src) + 1;
}

int iio_device_attr_write_longlong(const struct iio_device *dev, const char *attr, long long val) {
  char s[32];
  snprintf(s, sizeof(s), "%lld", val);
  return dev_attr_set(dev, attr, s);
}

ssize_t iio_device_attr_write_raw(const struct iio_device *dev, const char *attr, const void *src, size_t len) {
  (void)dev; (void)src;
  if (!strcmp(attr, "filter_fir_config")) return (ssize_t)len;
  return -ENOENT;
}

///////////////////////////////////////////////////////////////////////////////////////
// Buffers
///////////////////////////////////////////////////////////////////////////////////////
struct iio_buffer *iio_device_create_buffer(const struct iio_device *dev, size_t samples_count, bool cyclic) {
  (void)cyclic;
  struct iio_buffer *b = calloc(1, sizeof(*b));
  b->dev = (struct iio_device *)dev;
  b->samples = samples_count;
  b->data = calloc(samples_count * 2, sizeof(int16_t));
  b->blocking = true;
  return b;
}

int iio_buffer_set_blocking_mode(struct iio_buffer *buf, bool blocking) {
  buf->blocking = blocking;
  return 0;
}

void *iio_buffer_first(const struct iio_buffer *buf, const struct iio_channel *chn) {
  return (char *)buf->data + (chn->kind == CH_VOLT1 ? 2 : 0);
}

void *iio_buffer_end(const struct iio_buffer *buf) {
  return (char *)buf->data + buf->samples * 4;
}

ptrdiff_t iio_buffer_step(const struct iio_buffer *buf) {
  (void)buf;
  return 4;
}

ssize_t iio_buffer_refill(struct iio_buffer *buf) {
  if (buf->dev->kind != DEV_RX) return -EINVAL;
  // Drain whatever has arrived; wait briefly (or until full when blocking)
  // so a node polling in a tight loop doesn't spin a whole core.
  while (pump_one(0)) ;
  if (!buf->blocking && rx_stage_n < buf->samples) {
    // Non-blocking: one short wait (avoids a busy spin), then -EAGAIN.
    if (pump_one(1)) while (pump_one(0)) ;
    if (rx_stage_n < buf->samples) return -EAGAIN;
  }
  while (rx_stage_n < buf->samples) {
    pump_one(100);
    while (pump_one(0)) ;
  }
  memcpy(buf->data, rx_stage, buf->samples * 4);
  rx_stage_n -= buf->samples;
  memmove(rx_stage, &rx_stage[buf->samples * 2], rx_stage_n * 4);
  return (ssize_t)(buf->samples * 4);
}

ssize_t iio_buffer_push(struct iio_buffer *buf) {
  if (buf->dev->kind != DEV_TX) return -EINVAL;
  double now = now_s();
  double air = buf->samples / SIM_FS_NOMINAL;
  if (tx_busy_until < now) tx_busy_until = now;
  double ahead = tx_busy_until - now;
  double limit = TX_KERNEL_BUFFERS * air;
  if (ahead > limit) usleep((useconds_t)((ahead - limit) * 1e6));
  tx_busy_until += air;
  send_msg(SIM_TX, buf->data, buf->samples * 4, (uint32_t)buf->samples);
  return (ssize_t)(buf->samples * 4);
}

///////////////////////////////////////////////////////////////////////////////////////
// libad9361
///////////////////////////////////////////////////////////////////////////////////////
int ad9361_set_bb_rate_custom_filter_manual(struct iio_device *dev, unsigned long rate,
                                            unsigned long Fpass, unsigned long Fstop,
                                            unsigned long wnom_tx, unsigned long wnom_rx) {
  (void)dev; (void)Fpass; (void)Fstop; (void)wnom_tx; (void)wnom_rx;
  sample_hz = (long long)rate;
  fir_en = true;
  return 0;
}

int ad9361_set_trx_fir_enable(struct iio_device *dev, int enable) {
  (void)dev;
  fir_en = enable != 0;
  return 0;
}
