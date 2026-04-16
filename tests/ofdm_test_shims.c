//MIT License
//
//Copyright (c) 2018 tvelliott
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

// Stub implementations for hardware and MAC-layer dependencies.
// Compiled as a separate object and linked with the real ofdm_tx.o,
// ofdm_rx.o, cw_tone.o, timers.o, and test_ofdm.o.

#include <stdint.h>
#include <string.h>
#include <complex.h>

///////////////////////////////////////////////////////////////////////////////
// pluto.h stubs
///////////////////////////////////////////////////////////////////////////////

int pluto_transmit_call_count = 0;
int pluto_transmit_last_is_last = 0;
int pluto_transmit_total_samples = 0;

int pluto_transmit(float complex *buffer, int len, int do_dump_rx, int is_last) {
    (void)buffer; (void)do_dump_rx;
    pluto_transmit_call_count++;
    pluto_transmit_total_samples += len;
    pluto_transmit_last_is_last = is_last;
    return 0;
}

void pluto_transmit_flush_tx(void) {}
int pluto_receive(void) { return 0; }

static long long stub_in_gain = 40;

long long pluto_get_in_gain(void) { return stub_in_gain; }
void pluto_set_in_gain(long long gain) { stub_in_gain = gain; }
void pluto_bump_agc_up(int delta) { (void)delta; }
void pluto_bump_agc_down(int delta) { (void)delta; }
long long pluto_get_in_rssi(void) { return -60; }
void pluto_set_in_gain_auto_fast(void) {}
void pluto_apply_pss_xo_correction(float cfo_cycles_per_sample) {
    (void)cfo_cycles_per_sample;
}
long long pluto_get_xo_correction(void) { return 40000000LL; }
void pluto_set_xo_correction(long long xo_hz) { (void)xo_hz; }
void pluto_init_xo_correction(void) {}

long long current_sample_freq = 1400000;
long long current_rx_freq = 915000000;
long long pluto_current_gain = 40;
int rx_timeout = 4096;

///////////////////////////////////////////////////////////////////////////////
// charon.h stubs
///////////////////////////////////////////////////////////////////////////////

int send_ack_call_count = 0;
int got_ack_call_count = 0;
int lbt_backoff_call_count = 0;

void lbt_backoff(void) { lbt_backoff_call_count++; }
void do_send_ack(uint8_t *ack_mac) { (void)ack_mac; send_ack_call_count++; }
void do_got_ack(void) { got_ack_call_count++; }
void reset_agc_timer(void) {}
void check_agc(void) {}

int first_rx_after_agc_reset = 0;
uint8_t mac_to_ack[6] = {0};

///////////////////////////////////////////////////////////////////////////////
// tap_device.h stubs
///////////////////////////////////////////////////////////////////////////////

int write_tap_dev_call_count = 0;
int write_tap_dev_last_len = 0;
static char write_tap_dev_last_buf[2346];

int write_tap_dev(char *buffer, int len) {
    write_tap_dev_call_count++;
    write_tap_dev_last_len = len;
    if (len > 0 && len <= (int)sizeof(write_tap_dev_last_buf))
        memcpy(write_tap_dev_last_buf, buffer, len);
    return len;
}

int read_tap_dev(char *buffer, int max_len, char *dst_mac, char *is_broadcast) {
    (void)buffer; (void)max_len; (void)dst_mac; (void)is_broadcast;
    return 0;
}

int setNonblocking(int fd) { (void)fd; return 0; }
int init_tap_device(void) { return 0; }

uint8_t ofdm0_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
const uint8_t mac_all_one[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t mac_all_zero[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

///////////////////////////////////////////////////////////////////////////////
// config.h globals
///////////////////////////////////////////////////////////////////////////////

long long tx_output_power_minus_dbm = 10;
long long rf_bandwidth = 1400000;
long long sample_freq_hz = 1400000;
long long freq_rxtx_hz = 915000000;
int tcp_share_backoff = 0;
int max_tcp_share_backoff = 3216;
int symbol_delay_timeout = 268;
int ack_delay_timeout = 25000;
int usb_batman_if = 0;
int max_tcp_segs = 2;
int bcast_retrans = 1;
int max_long_retrans = 1;
int max_short_retrans = 8;
int bat_ogm_interval = 10000;
int enable_charon = 1;
double ref_correction_ppm = 6.15;
char usb_ip[16] = "192.168.2.1";
uint32_t usb_ip32 = 0;

///////////////////////////////////////////////////////////////////////////////
// crc.h stubs
///////////////////////////////////////////////////////////////////////////////

uint32_t crc32_val = 0;

uint32_t crc32_range(uint8_t *ucBuffer, int32_t ulCount) {
    (void)ucBuffer; (void)ulCount;
    return 0;
}

int is_dup(uint32_t pid) { (void)pid; return 0; }
void add_dup(uint32_t pid) { (void)pid; }
void clear_duplicates(void) {}

///////////////////////////////////////////////////////////////////////////////
// tcp_subs stub
///////////////////////////////////////////////////////////////////////////////

void do_tcp_subs(uint8_t *buffer, int len) { (void)buffer; (void)len; }

///////////////////////////////////////////////////////////////////////////////
// reset_shim_counters — called by tests to reset state between test cases
///////////////////////////////////////////////////////////////////////////////

void reset_shim_counters(void) {
    pluto_transmit_call_count = 0;
    pluto_transmit_last_is_last = 0;
    pluto_transmit_total_samples = 0;
    send_ack_call_count = 0;
    got_ack_call_count = 0;
    lbt_backoff_call_count = 0;
    write_tap_dev_call_count = 0;
    write_tap_dev_last_len = 0;
    stub_in_gain = 40;
    first_rx_after_agc_reset = 0;
}
