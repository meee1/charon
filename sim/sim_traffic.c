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

// Simulator traffic generator: stands in for the ofdm0 TAP device so the
// full Charon MAC/PHY can be exercised without root or a network stack.
//
// Environment:
//   CHARON_SIM_TRAFFIC      mean frames/sec to offer (0 = receive only),
//                           Poisson arrivals.  Setting this variable enables
//                           traffic mode.
//   CHARON_SIM_TRAFFIC_LEN  UDP payload bytes per frame (default 200)
//   CHARON_SIM_PEERS        comma-separated peer IPs (unicast, round-robin)
//                           or "bcast" for broadcast frames
//   CHARON_SIM_NODE         node id used in log lines
//
// Each frame is a real Ethernet/IPv4/UDP frame whose payload carries a magic,
// source node id, sequence number, send timestamp and a CRC-32, so the
// receiving node can count delivered, corrupt, lost and duplicate frames and
// measure latency.  Stats are printed once a second as:
//   SIMSTAT node=N t=S gen=G rx_ok=R rx_bad=B lost=L dup=D lat_ms=X

#define _DEFAULT_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <math.h>

#include "sim_traffic.h"

#define SIM_MAGIC        0x4353494dU   // "CSIM"
#define ETH_HLEN_        14
#define IP_HLEN_         20
#define UDP_HLEN_        8
#define HDR_LEN          (ETH_HLEN_ + IP_HLEN_ + UDP_HLEN_)
#define MAX_PEERS        16
#define MAX_SOURCES      32

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t node;
  uint16_t len;        // payload bytes after this header
  uint32_t seq;
  uint64_t tx_us;
  uint32_t crc;        // CRC-32 over the pattern bytes that follow
} sim_payload_hdr;

static int      enabled = -1;
static double   pps;
static int      payload_len = 200;
static int      node_id;
static int      bcast;
static int      npeers;
static uint8_t  peer_mac[MAX_PEERS][6];
static uint32_t peer_ip[MAX_PEERS];
static int      next_peer;
static uint8_t  own_mac[6];
static uint32_t own_ip;
static uint32_t seq[MAX_PEERS];   // per destination, so each receiver sees 0,1,2...
static double   next_due_s;
static unsigned int rng_state;

// Exponential inter-arrival (Poisson process); own RNG so the MAC's use of
// random() is unaffected.
static double next_gap_s(void) {
  double u = (rand_r(&rng_state) + 1.0) / ((double)RAND_MAX + 2.0);
  return -log(u) / pps;
}

static struct {
  int      used;
  uint16_t node;
  uint32_t last_seq;
  int      have_seq;
} sources[MAX_SOURCES];

static unsigned long st_gen, st_rx_ok, st_rx_bad, st_lost, st_dup;
static double st_lat_sum;
static double t0_s, last_print_s;

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
static double now_s(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec * 1e-6;
}

static uint32_t crc32_bytes(const uint8_t *p, int n) {
  uint32_t c = 0xffffffffU;
  for (int i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320U & (-(c & 1)));
  }
  return ~c;
}

static uint16_t ip_checksum(const uint8_t *p, int n) {
  uint32_t s = 0;
  for (int i = 0; i + 1 < n; i += 2) s += (p[i] << 8) | p[i + 1];
  while (s >> 16) s = (s & 0xffff) + (s >> 16);
  return (uint16_t)~s;
}

// Same derivation as the node itself uses (tap_device.c).
void tap_mac_from_ip(uint32_t ip32, char *mac_a);

void sim_traffic_mac_from_ip(uint32_t ip32, uint8_t *mac) {
  char mac_a[18];
  unsigned int m[6];
  tap_mac_from_ip(ip32, mac_a);
  sscanf(mac_a, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int sim_traffic_enabled(void) {
  if (enabled < 0) enabled = getenv("CHARON_SIM_TRAFFIC") != NULL;
  return enabled;
}

void sim_traffic_init(const uint8_t *mac, uint32_t ip32) {
  const char *v;
  memcpy(own_mac, mac, 6);
  own_ip = ip32;
  pps = atof(getenv("CHARON_SIM_TRAFFIC"));
  if ((v = getenv("CHARON_SIM_TRAFFIC_LEN"))) payload_len = atoi(v);
  if (payload_len < (int)sizeof(sim_payload_hdr)) payload_len = sizeof(sim_payload_hdr);
  if (payload_len > 1400) payload_len = 1400;
  if ((v = getenv("CHARON_SIM_NODE"))) node_id = atoi(v);

  if ((v = getenv("CHARON_SIM_PEERS"))) {
    if (strcmp(v, "bcast") == 0) {
      bcast = 1;
    } else {
      char tmp[512];
      strncpy(tmp, v, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = 0;
      for (char *tok = strtok(tmp, ","); tok && npeers < MAX_PEERS; tok = strtok(NULL, ",")) {
        if (inet_pton(AF_INET, tok, &peer_ip[npeers]) == 1) {
          sim_traffic_mac_from_ip(peer_ip[npeers], peer_mac[npeers]);
          npeers++;
        }
      }
    }
  }
  rng_state = 0x9e3779b9u ^ (unsigned)(node_id * 7919 + 1);
  t0_s = last_print_s = now_s();
  next_due_s = t0_s + 1.0 + (pps > 0 ? next_gap_s() : 0);
  fprintf(stderr, "\nSIMTRAFFIC node=%d pps=%.1f len=%d peers=%d bcast=%d mac=%02x:%02x:%02x:%02x:%02x:%02x",
          node_id, pps, payload_len, npeers, bcast,
          own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4], own_mac[5]);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
static void maybe_print_stats(double now) {
  if (now - last_print_s < 1.0) return;
  last_print_s = now;
  fprintf(stderr, "\nSIMSTAT node=%d t=%.1f gen=%lu rx_ok=%lu rx_bad=%lu lost=%lu dup=%lu lat_ms=%.2f",
          node_id, now - t0_s, st_gen, st_rx_ok, st_rx_bad, st_lost, st_dup,
          st_rx_ok ? st_lat_sum / st_rx_ok * 1e3 : 0.0);
}

int sim_traffic_read(uint8_t *buf, int max_len, uint8_t *dst_mac, uint8_t *is_broadcast) {
  double now = now_s();
  maybe_print_stats(now);

  if (pps <= 0.0 || (!bcast && npeers == 0) || now < next_due_s) return 0;
  // Offered load is paced; if the MAC was busy, don't burst to catch up.
  next_due_s = (now - next_due_s > 1.0) ? now + next_gap_s() : next_due_s + next_gap_s();

  int total = HDR_LEN + payload_len;
  if (total > max_len) return 0;

  const uint8_t *dmac;
  static const uint8_t bmac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint32_t dip;
  int pi = 0;
  if (bcast) {
    dmac = bmac;
    dip = 0xffffffffU;
  } else {
    pi = next_peer;
    dmac = peer_mac[pi];
    dip = peer_ip[pi];
    next_peer = (next_peer + 1) % npeers;
  }

  memset(buf, 0, total);
  // Ethernet
  memcpy(buf, dmac, 6);
  memcpy(buf + 6, own_mac, 6);
  buf[12] = 0x08; buf[13] = 0x00;
  // IPv4
  uint8_t *ip = buf + ETH_HLEN_;
  int ip_len = IP_HLEN_ + UDP_HLEN_ + payload_len;
  ip[0] = 0x45;
  ip[2] = ip_len >> 8; ip[3] = ip_len & 0xff;
  ip[8] = 64; ip[9] = 17;
  memcpy(ip + 12, &own_ip, 4);
  memcpy(ip + 16, &dip, 4);
  uint16_t cs = ip_checksum(ip, IP_HLEN_);
  ip[10] = cs >> 8; ip[11] = cs & 0xff;
  // UDP (checksum 0 = none)
  uint8_t *udp = ip + IP_HLEN_;
  udp[0] = 0x4e; udp[1] = 0x20; udp[2] = 0x4e; udp[3] = 0x20;   // port 20000
  udp[4] = (UDP_HLEN_ + payload_len) >> 8; udp[5] = (UDP_HLEN_ + payload_len) & 0xff;
  // payload
  sim_payload_hdr h;
  uint8_t *pat = udp + UDP_HLEN_ + sizeof(h);
  int plen = payload_len - (int)sizeof(h);
  for (int i = 0; i < plen; i++) pat[i] = (uint8_t)(seq[pi] * 7 + i * 13);
  h.magic = SIM_MAGIC;
  h.node = node_id;
  h.len = plen;
  h.seq = seq[pi]++;
  h.tx_us = (uint64_t)(now * 1e6);
  h.crc = crc32_bytes(pat, plen);
  memcpy(udp + UDP_HLEN_, &h, sizeof(h));

  memcpy(dst_mac, dmac, 6);
  is_broadcast[0] = bcast ? 1 : 0;
  st_gen++;
  return total;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int sim_traffic_write(const uint8_t *buf, int len) {
  double now = now_s();
  maybe_print_stats(now);

  if (len < HDR_LEN + (int)sizeof(sim_payload_hdr) || buf[12] != 0x08 || buf[13] != 0x00 ||
      buf[ETH_HLEN_ + 9] != 17)
    return len;   // not ours (ARP etc.) -- swallow like a TAP would

  sim_payload_hdr h;
  memcpy(&h, buf + HDR_LEN, sizeof(h));
  if (h.magic != SIM_MAGIC) return len;

  const uint8_t *pat = buf + HDR_LEN + sizeof(h);
  if (HDR_LEN + (int)sizeof(h) + h.len > len || crc32_bytes(pat, h.len) != h.crc) {
    st_rx_bad++;
    return len;
  }

  int s;
  for (s = 0; s < MAX_SOURCES; s++)
    if (sources[s].used && sources[s].node == h.node) break;
  if (s == MAX_SOURCES) {
    for (s = 0; s < MAX_SOURCES && sources[s].used; s++);
    if (s == MAX_SOURCES) return len;
    sources[s].used = 1;
    sources[s].node = h.node;
  }

  if (sources[s].have_seq) {
    if (h.seq <= sources[s].last_seq) { st_dup++; return len; }
    st_lost += h.seq - sources[s].last_seq - 1;
  }
  sources[s].last_seq = h.seq;
  sources[s].have_seq = 1;

  st_rx_ok++;
  st_lat_sum += now - h.tx_us * 1e-6;
  return len;
}
