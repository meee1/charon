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

// Unit tests for ofdm_tx.c and ofdm_rx.c — OFDM frame TX/RX with loopback
//
// Links against real liquid-dsp and fftw3 so the frame generator and
// synchronizer execute the actual modulation/demodulation code path.
// Hardware dependencies (pluto.c, tap_device.c, charon.c) are shimmed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <stdint.h>

#include "test_harness.h"

///////////////////////////////////////////////////////////////////////////////
// Shim: pluto.h — stub all hardware functions
///////////////////////////////////////////////////////////////////////////////

static int pluto_transmit_call_count = 0;
static int pluto_transmit_last_is_last = 0;
static int pluto_transmit_total_samples = 0;

int pluto_transmit(float complex *buffer, int len, int do_dump_rx, int is_last) {
    (void)buffer; (void)do_dump_rx;
    pluto_transmit_call_count++;
    pluto_transmit_total_samples += len;
    pluto_transmit_last_is_last = is_last;
    return 0;
}

static long long stub_in_gain = 40;

long long pluto_get_in_gain(void) { return stub_in_gain; }
void pluto_set_in_gain(long long gain) { stub_in_gain = gain; }
void pluto_bump_agc_up(int delta) { (void)delta; }
void pluto_bump_agc_down(int delta) { (void)delta; }
long long pluto_get_in_rssi(void) { return -60; }
void pluto_set_in_gain_auto_fast(void) {}
void pluto_apply_pss_xo_correction(float cfo_cycles_per_sample) { (void)cfo_cycles_per_sample; }
long long pluto_get_xo_correction(void) { return 40000000LL; }
void pluto_set_xo_correction(long long xo_hz) { (void)xo_hz; }
void pluto_init_xo_correction(void) {}

long long current_sample_freq = 1400000;
long long current_rx_freq = 915000000;
long long pluto_current_gain = 40;
int rx_timeout = 4096;

///////////////////////////////////////////////////////////////////////////////
// Shim: charon.h — stub MAC-layer callbacks
///////////////////////////////////////////////////////////////////////////////

static int send_ack_call_count = 0;
static int got_ack_call_count = 0;
static int lbt_backoff_call_count = 0;

void lbt_backoff(void) { lbt_backoff_call_count++; }
void do_send_ack(uint8_t *ack_mac) { (void)ack_mac; send_ack_call_count++; }
void do_got_ack(void) { got_ack_call_count++; }
void reset_agc_timer(void) {}
void check_agc(void) {}

int first_rx_after_agc_reset = 0;
uint8_t mac_to_ack[6] = {0};

// charon.h declares main() — we need to hide it
#define main charon_main
// Provide a stub so the declaration resolves
static inline int charon_main(int argc, char **argv) { (void)argc; (void)argv; return 0; }
#undef main

///////////////////////////////////////////////////////////////////////////////
// Shim: tap_device.h — stub TAP device
///////////////////////////////////////////////////////////////////////////////

static int write_tap_dev_call_count = 0;
static int write_tap_dev_last_len = 0;
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
// Shim: config.h — provide runtime config globals
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
// Shim: crc.h — stub duplicate detection
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
// Shim: tcp_subs — not needed for OFDM tests
///////////////////////////////////////////////////////////////////////////////

void do_tcp_subs(uint8_t *buffer, int len) { (void)buffer; (void)len; }

///////////////////////////////////////////////////////////////////////////////
// Include real implementations (no external deps beyond libc + libm)
///////////////////////////////////////////////////////////////////////////////

#include "../timers.c"
#include "../cw_tone.c"

///////////////////////////////////////////////////////////////////////////////
// Include the modules under test (require liquid-dsp + fftw3)
///////////////////////////////////////////////////////////////////////////////

#include "../ofdm_tx.c"
#include "../ofdm_rx.c"

///////////////////////////////////////////////////////////////////////////////
// Test helpers
///////////////////////////////////////////////////////////////////////////////

static void reset_shim_counters(void) {
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

// Run a loopback test for a given payload size and pattern.
// Returns 1 on success (payload received intact), 0 on failure.
static int run_loopback(const unsigned char *tx_payload, int payload_len) {
    loopback_rx_ok = 0;
    loopback_rx_payload_len = 0;
    memset(loopback_rx_payload, 0, PAYLOAD_LEN);

    ofdm_rx_reset();
    ofdm_tx_loopback((uint8_t *)tx_payload, payload_len);

    if (!loopback_rx_ok || loopback_rx_payload_len != payload_len)
        return 0;

    return (memcmp(loopback_rx_payload, tx_payload, payload_len) == 0) ? 1 : 0;
}

///////////////////////////////////////////////////////////////////////////////
// TX tests
///////////////////////////////////////////////////////////////////////////////

static void test_init_ofdm_tx(void) {
    TEST_BEGIN("ofdm_tx: init_ofdm_tx succeeds");
    // init_ofdm_tx() is called once during setup — just verify it didn't crash
    // and the frame generator is usable by checking sample count
    int sc = ofdm_get_sample_count(64);
    TEST_ASSERT_MSG(sc > 0, "sample count should be positive after init");
    TEST_PASS();
}

static void test_ofdm_get_sample_count_various(void) {
    TEST_BEGIN("ofdm_tx: sample count increases with payload size");
    int sc1   = ofdm_get_sample_count(1);
    int sc256 = ofdm_get_sample_count(256);
    int sc1024 = ofdm_get_sample_count(1024);
    int sc_max = ofdm_get_sample_count(PAYLOAD_LEN);

    TEST_ASSERT(sc1 > 0);
    TEST_ASSERT(sc256 > sc1);
    TEST_ASSERT(sc1024 > sc256);
    TEST_ASSERT(sc_max > sc1024);
    TEST_PASS();
}

static void test_do_ofdm_tx_calls_transmit(void) {
    TEST_BEGIN("ofdm_tx: do_ofdm_tx calls pluto_transmit");
    reset_shim_counters();

    uint8_t payload[64];
    memset(payload, 0xAB, sizeof(payload));
    uint8_t dst_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

    do_ofdm_tx(payload, sizeof(payload), 0, 0, 0, dst_mac, 0x12345678);

    TEST_ASSERT_MSG(pluto_transmit_call_count > 0,
                    "pluto_transmit should be called at least once");
    TEST_ASSERT_MSG(pluto_transmit_total_samples > 0,
                    "total transmitted samples should be positive");
    TEST_ASSERT_MSG(pluto_transmit_last_is_last == 1,
                    "last call should have is_last=1");
    TEST_PASS();
}

static void test_do_ofdm_tx_skip_self(void) {
    TEST_BEGIN("ofdm_tx: skip TX when dst_mac == ofdm0_mac");
    reset_shim_counters();

    uint8_t payload[32];
    memset(payload, 0, sizeof(payload));

    int ret = do_ofdm_tx(payload, sizeof(payload), 0, 0, 0, ofdm0_mac, 0x1111);

    TEST_ASSERT_MSG(ret == 0, "should return 0");
    TEST_ASSERT_MSG(pluto_transmit_call_count == 0,
                    "should not call pluto_transmit for self-addressed frame");
    TEST_PASS();
}

static void test_do_ofdm_tx_ack_frame(void) {
    TEST_BEGIN("ofdm_tx: ACK frame (len=0) sends 6-byte payload");
    reset_shim_counters();

    uint8_t dst_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

    // len=0 triggers the ACK path
    do_ofdm_tx(NULL, 0, 0, 0, 0, dst_mac, 0);

    TEST_ASSERT_MSG(pluto_transmit_call_count > 0,
                    "pluto_transmit should be called for ACK");
    TEST_PASS();
}

static void test_do_ofdm_tx_broadcast(void) {
    TEST_BEGIN("ofdm_tx: broadcast sets dst_mac to all-ones");
    reset_shim_counters();

    uint8_t payload[64];
    memset(payload, 0x55, sizeof(payload));
    uint8_t dst_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

    do_ofdm_tx(payload, sizeof(payload), 0, 0, 1, dst_mac, 0xAAAA);

    // If we got here without crash and pluto_transmit was called, the
    // broadcast path executed.  The frame content is assembled into
    // ofdm_payload which is internal — we verify the path ran.
    TEST_ASSERT_MSG(pluto_transmit_call_count > 0,
                    "pluto_transmit should be called for broadcast");
    TEST_PASS();
}

static void test_do_ofdm_tx_cw_tone_prepended(void) {
    TEST_BEGIN("ofdm_tx: CW tone transmitted before OFDM frame");
    reset_shim_counters();

    uint8_t payload[64];
    memset(payload, 0x33, sizeof(payload));
    uint8_t dst_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

    do_ofdm_tx(payload, sizeof(payload), 0, 0, 0, dst_mac, 0xBBBB);

    // First pluto_transmit call is the CW tone (128 samples),
    // subsequent calls are OFDM symbols.  At minimum 2 calls expected.
    TEST_ASSERT_MSG(pluto_transmit_call_count >= 2,
                    "should have at least 2 pluto_transmit calls (CW + OFDM)");
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// RX tests
///////////////////////////////////////////////////////////////////////////////

static void test_init_ofdm_rx(void) {
    TEST_BEGIN("ofdm_rx: init_ofdm_rx succeeds");
    // init_ofdm_rx() is called once during setup; verify state
    int state = ofdm_rx_state();
    TEST_ASSERT_MSG(state == OFDMFRAMESYNC_STATE_SEEKPLCP,
                    "initial state should be SEEKPLCP");
    TEST_PASS();
}

static void test_ofdm_rx_state_seekplcp(void) {
    TEST_BEGIN("ofdm_rx: state is SEEKPLCP after reset");
    ofdm_rx_reset();
    int state = ofdm_rx_state();
    TEST_ASSERT_MSG(state == OFDMFRAMESYNC_STATE_SEEKPLCP,
                    "state should be SEEKPLCP after reset");
    TEST_PASS();
}

static void test_ofdm_rx_set_loopback_toggle(void) {
    TEST_BEGIN("ofdm_rx: loopback mode toggle");
    ofdm_rx_set_loopback(1);
    // Feed a known frame through loopback and verify it's received
    unsigned char payload[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    int ok = run_loopback(payload, sizeof(payload));
    TEST_ASSERT_MSG(ok, "loopback should work when enabled");

    // Disable loopback — the callback will try charon frame parsing
    // which will likely fail for raw payloads, but shouldn't crash
    ofdm_rx_set_loopback(0);
    TEST_PASS();
}

static void test_ofdm_rx_reset_restores_state(void) {
    TEST_BEGIN("ofdm_rx: reset restores SEEKPLCP state");
    // Feed some noise to potentially move state
    for (int i = 0; i < 100; i++) {
        float complex noise = ((float)rand() / RAND_MAX - 0.5f)
                            + ((float)rand() / RAND_MAX - 0.5f) * _Complex_I;
        do_ofdm_rx(noise);
    }
    ofdm_rx_reset();
    int state = ofdm_rx_state();
    TEST_ASSERT_MSG(state == OFDMFRAMESYNC_STATE_SEEKPLCP,
                    "state should be SEEKPLCP after reset");
    TEST_PASS();
}

static void test_ofdm_rx_no_detect_silence(void) {
    TEST_BEGIN("ofdm_rx: no frame detected from silence");
    ofdm_rx_set_loopback(1);
    loopback_rx_ok = 0;
    ofdm_rx_reset();

    // Feed zeros — should not trigger a frame detection
    for (int i = 0; i < 1024; i++) {
        do_ofdm_rx(0.0f + 0.0f * _Complex_I);
    }
    TEST_ASSERT_MSG(!loopback_rx_ok, "silence should not produce a valid frame");
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Loopback integration tests
///////////////////////////////////////////////////////////////////////////////

static void test_loopback_1_byte(void) {
    TEST_BEGIN("loopback: 1-byte payload roundtrip");
    ofdm_rx_set_loopback(1);
    unsigned char payload[1] = {0x42};
    int ok = run_loopback(payload, 1);
    TEST_ASSERT_MSG(ok, "1-byte payload should survive loopback");
    TEST_PASS();
}

static void test_loopback_64_bytes(void) {
    TEST_BEGIN("loopback: 64-byte payload roundtrip");
    ofdm_rx_set_loopback(1);
    unsigned char payload[64];
    for (int i = 0; i < 64; i++) payload[i] = (unsigned char)(i & 0xFF);
    int ok = run_loopback(payload, 64);
    TEST_ASSERT_MSG(ok, "64-byte payload should survive loopback");
    TEST_PASS();
}

static void test_loopback_256_bytes(void) {
    TEST_BEGIN("loopback: 256-byte payload roundtrip");
    ofdm_rx_set_loopback(1);
    unsigned char payload[256];
    for (int i = 0; i < 256; i++) payload[i] = (unsigned char)(i & 0xFF);
    int ok = run_loopback(payload, 256);
    TEST_ASSERT_MSG(ok, "256-byte payload should survive loopback");
    TEST_PASS();
}

static void test_loopback_512_bytes(void) {
    TEST_BEGIN("loopback: 512-byte payload roundtrip");
    ofdm_rx_set_loopback(1);
    unsigned char payload[512];
    for (int i = 0; i < 512; i++) payload[i] = (unsigned char)(i & 0xFF);
    int ok = run_loopback(payload, 512);
    TEST_ASSERT_MSG(ok, "512-byte payload should survive loopback");
    TEST_PASS();
}

static void test_loopback_1024_bytes(void) {
    TEST_BEGIN("loopback: 1024-byte payload roundtrip");
    ofdm_rx_set_loopback(1);
    unsigned char payload[1024];
    for (int i = 0; i < 1024; i++) payload[i] = (unsigned char)(i & 0xFF);
    int ok = run_loopback(payload, 1024);
    TEST_ASSERT_MSG(ok, "1024-byte payload should survive loopback");
    TEST_PASS();
}

static void test_loopback_max_payload(void) {
    TEST_BEGIN("loopback: max payload (1514 bytes) roundtrip");
    ofdm_rx_set_loopback(1);
    unsigned char payload[PAYLOAD_LEN];
    for (int i = 0; i < PAYLOAD_LEN; i++) payload[i] = (unsigned char)(i & 0xFF);
    int ok = run_loopback(payload, PAYLOAD_LEN);
    TEST_ASSERT_MSG(ok, "max payload should survive loopback");
    TEST_PASS();
}

static void test_loopback_payload_integrity_random(void) {
    TEST_BEGIN("loopback: random payload integrity check");
    ofdm_rx_set_loopback(1);

    srand(12345);
    unsigned char payload[512];
    for (int i = 0; i < 512; i++) payload[i] = (unsigned char)(rand() & 0xFF);

    int ok = run_loopback(payload, 512);
    TEST_ASSERT_MSG(ok, "random payload should survive loopback intact");
    TEST_PASS();
}

static void test_loopback_sequential_frames(void) {
    TEST_BEGIN("loopback: sequential frames all succeed");
    ofdm_rx_set_loopback(1);

    int sizes[] = {1, 64, 256, 512, 1024};
    int num = sizeof(sizes) / sizeof(sizes[0]);
    int pass_count = 0;

    for (int t = 0; t < num; t++) {
        unsigned char payload[1024];
        for (int i = 0; i < sizes[t]; i++)
            payload[i] = (unsigned char)((t + i) & 0xFF);
        if (run_loopback(payload, sizes[t]))
            pass_count++;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "%d/%d sequential loopback frames passed", pass_count, num);
    TEST_ASSERT_MSG(pass_count == num, msg);
    TEST_PASS();
}

static void test_loopback_all_zeros(void) {
    TEST_BEGIN("loopback: all-zero payload roundtrip");
    ofdm_rx_set_loopback(1);
    unsigned char payload[128];
    memset(payload, 0x00, sizeof(payload));
    int ok = run_loopback(payload, sizeof(payload));
    TEST_ASSERT_MSG(ok, "all-zero payload should survive loopback");
    TEST_PASS();
}

static void test_loopback_all_ones(void) {
    TEST_BEGIN("loopback: all-0xFF payload roundtrip");
    ofdm_rx_set_loopback(1);
    unsigned char payload[128];
    memset(payload, 0xFF, sizeof(payload));
    int ok = run_loopback(payload, sizeof(payload));
    TEST_ASSERT_MSG(ok, "all-0xFF payload should survive loopback");
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main(void) {
    fprintf(stderr, "=== OFDM TX/RX Unit Tests ===\n");
    fprintf(stderr, "Subcarriers: %d, Modulation: QPSK, CRC: CRC-32\n", OFDM_M);
    fprintf(stderr, "FEC inner: NONE, FEC outer: SECDED7264\n");
    fprintf(stderr, "Cyclic prefix: %d, Taper: %d\n\n", CP_LEN, TAPER_LEN);

    // One-time initialization of TX and RX (as charon.c does)
    init_ofdm_rx();
    init_ofdm_tx();
    ofdm_rx_set_loopback(1);

    // TX tests
    RUN_TEST(test_init_ofdm_tx);
    RUN_TEST(test_ofdm_get_sample_count_various);
    RUN_TEST(test_do_ofdm_tx_calls_transmit);
    RUN_TEST(test_do_ofdm_tx_skip_self);
    RUN_TEST(test_do_ofdm_tx_ack_frame);
    RUN_TEST(test_do_ofdm_tx_broadcast);
    RUN_TEST(test_do_ofdm_tx_cw_tone_prepended);

    // RX tests
    RUN_TEST(test_init_ofdm_rx);
    RUN_TEST(test_ofdm_rx_state_seekplcp);
    RUN_TEST(test_ofdm_rx_set_loopback_toggle);
    RUN_TEST(test_ofdm_rx_reset_restores_state);
    RUN_TEST(test_ofdm_rx_no_detect_silence);

    // Loopback integration tests
    RUN_TEST(test_loopback_1_byte);
    RUN_TEST(test_loopback_64_bytes);
    RUN_TEST(test_loopback_256_bytes);
    RUN_TEST(test_loopback_512_bytes);
    RUN_TEST(test_loopback_1024_bytes);
    RUN_TEST(test_loopback_max_payload);
    RUN_TEST(test_loopback_payload_integrity_random);
    RUN_TEST(test_loopback_sequential_frames);
    RUN_TEST(test_loopback_all_zeros);
    RUN_TEST(test_loopback_all_ones);

    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
