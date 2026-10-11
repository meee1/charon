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

// Wire protocol between charon_sim nodes (sim/iio_zmq.c) and the channel
// broker (sim/rfchan.c).
//
//   node  --PUSH-->  rfchan (PULL, bound at SIM_TX_EP):  HELLO, TX, CTRL
//   node  <--SUB---  rfchan (PUB,  bound at SIM_RX_EP):  CAPS, RX
//
// PUB messages are two-part: [uint16 node id topic][payload].  Every payload
// starts with sim_hdr.  Samples are interleaved int16 I/Q, exactly what the
// AD9361 IIO buffers carry (12-bit ADC values left-justified for RX, DAC
// codes for TX).

#ifndef SIM_PROTO_H
#define SIM_PROTO_H

#include <stdint.h>

#define SIM_MAGIC_HDR       0x53494d31U       // "SIM1"
#define SIM_FS_NOMINAL      1400000.0
#define SIM_CHUNK           1400              // samples per channel tick (1 ms)
#define SIM_DEFAULT_TX_EP   "ipc:///tmp/charon_sim_tx"
#define SIM_DEFAULT_RX_EP   "ipc:///tmp/charon_sim_rx"
#define SIM_XO_NOMINAL_HZ   40000000LL

enum sim_msg_type {
  SIM_HELLO = 1,    // node -> chan: announce node id
  SIM_TX    = 2,    // node -> chan: count int16 IQ pairs follow
  SIM_CTRL  = 3,    // node -> chan: sim_ctrl follows
  SIM_RX    = 4,    // chan -> node: sim_rxinfo + count int16 IQ pairs
  SIM_CAPS  = 5,    // chan -> node: sim_caps (reply to HELLO)
};

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t type;
  uint16_t node;
  uint32_t count;
  uint32_t seq;
} sim_hdr;

enum sim_ctrl_key {
  CTRL_RX_LO = 1,        // ival: Hz           (RX side, immediate)
  CTRL_TX_LO,            // ival: Hz           (TX side, sample-timeline ordered)
  CTRL_TX_GAIN,          // dval: dB (<= 0)    (TX side)
  CTRL_RX_GAIN,          // dval: dB           (manual gain)
  CTRL_GAIN_MODE,        // ival: 0 manual, 1 fast_attack, 2 slow_attack
  CTRL_XO_CORR,          // ival: Hz
  CTRL_DCXO_COARSE,      // ival
  CTRL_DCXO_FINE,        // ival
  CTRL_TRACK_BBDC,       // ival: 0/1
  CTRL_TRACK_RFDC,       // ival: 0/1
  CTRL_TRACK_QUAD,       // ival: 0/1
  CTRL_CAL_TXQUAD,       // one-shot TX quadrature / LO leakage calibration
  CTRL_CAL_RFDC,         // one-shot RF DC offset calibration
};

typedef struct __attribute__((packed)) {
  uint32_t key;
  uint32_t pad;
  int64_t  ival;
  double   dval;
} sim_ctrl;

typedef struct __attribute__((packed)) {
  uint64_t sample_time;  // channel sample index of the first sample
  double   rssi_dbm;     // input power at the antenna over this chunk
  double   rx_gain_db;   // gain in effect (AGC or manual)
} sim_rxinfo;

typedef struct __attribute__((packed)) {
  int32_t dcxo_avail;    // 0: external reference clock (DCXO writes -> -ENODEV)
  int32_t dcxo_coarse;
  int32_t dcxo_fine;
  int32_t pad;
  int64_t xo_corr;
} sim_caps;

#endif
