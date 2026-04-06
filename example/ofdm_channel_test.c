/*
 * OFDM Channel Impairment Test
 * Tests the liquid-dsp OFDM TX/RX chain under varying channel conditions:
 *   - AWGN noise at multiple SNR levels
 *   - Carrier frequency offset (CFO)
 *   - Phase offset
 *   - Multipath fading (2-tap)
 *   - Combined impairments
 *
 * Based on Charon's OFDM configuration (OFDM-64, QAM-16 for examples).
 * Runs without hardware — pure loopback through simulated channel.
 *
 * Compile: gcc -Wall -O2 -g -o ofdm_channel_test ofdm_channel_test.c -lliquid -lm -lfftw3f
 * Run:     ./ofdm_channel_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <liquid/liquid.h>

// OFDM parameters (matching loopback example)
#define OFDM_M          64
#define CP_LEN          4
#define TAPER_LEN       2
#define OFDM_MOD        LIQUID_MODEM_QAM16
#define OFDM_FEC0       LIQUID_FEC_SECDED7264
#define OFDM_FEC1       LIQUID_FEC_HAMMING128
#define OFDM_CRC        LIQUID_CRC_32
#define PAYLOAD_LEN     256

///////////////////////////////////////////////////////////////////////////////
// Test infrastructure
///////////////////////////////////////////////////////////////////////////////

static int test_pass_count = 0;
static int test_fail_count = 0;

#define RUN_TEST(fn) fn()
#define TEST_SUMMARY() \
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count)

///////////////////////////////////////////////////////////////////////////////
// RX callback state
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    int frame_detected;
    int header_valid;
    int payload_valid;
    int payload_len;
    unsigned char payload[PAYLOAD_LEN];
    framesyncstats_s stats;
} rx_result_t;

static rx_result_t rx_result;

static int rx_callback(unsigned char *_header,
                       int _header_valid,
                       unsigned char *_payload,
                       unsigned int _payload_len,
                       int _payload_valid,
                       framesyncstats_s _stats,
                       void *_userdata)
{
    rx_result.frame_detected = 1;
    rx_result.header_valid = _header_valid;
    rx_result.payload_valid = _payload_valid;
    rx_result.stats = _stats;
    if (_payload_valid && _payload_len <= PAYLOAD_LEN) {
        memcpy(rx_result.payload, _payload, _payload_len);
        rx_result.payload_len = _payload_len;
    }
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Channel model helpers
///////////////////////////////////////////////////////////////////////////////

static float randn_bm(void)
{
    float u1 = ((float)rand() + 1.0f) / ((float)RAND_MAX + 1.0f);
    float u2 = ((float)rand() + 1.0f) / ((float)RAND_MAX + 1.0f);
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

// Apply AWGN to buffer
static void apply_awgn(float complex *buf, int len, float snr_db)
{
    // Estimate signal power
    float sig_power = 0.0f;
    for (int i = 0; i < len; i++)
        sig_power += crealf(buf[i] * conjf(buf[i]));
    sig_power /= (float)len;

    float noise_power = sig_power * powf(10.0f, -snr_db / 10.0f);
    float sigma = sqrtf(noise_power / 2.0f);

    for (int i = 0; i < len; i++) {
        buf[i] += sigma * (randn_bm() + _Complex_I * randn_bm());
    }
}

// Apply carrier frequency offset (normalized, cycles/sample)
static void apply_cfo(float complex *buf, int len, float cfo)
{
    for (int i = 0; i < len; i++) {
        float phase = 2.0f * (float)M_PI * cfo * (float)i;
        buf[i] *= (cosf(phase) + sinf(phase) * _Complex_I);
    }
}

// Apply constant phase offset
static void apply_phase_offset(float complex *buf, int len, float phi)
{
    float complex rot = cosf(phi) + sinf(phi) * _Complex_I;
    for (int i = 0; i < len; i++)
        buf[i] *= rot;
}

// Apply 2-tap multipath: y[n] = x[n] + alpha*e^{j*theta} * x[n-delay]
static void apply_multipath(float complex *buf, int len,
                            float alpha, float theta, int delay)
{
    float complex h1 = alpha * (cosf(theta) + sinf(theta) * _Complex_I);
    // Work backwards to avoid overwriting needed samples
    for (int i = len - 1; i >= delay; i--)
        buf[i] = buf[i] + h1 * buf[i - delay];
}

///////////////////////////////////////////////////////////////////////////////
// Generate TX frame, apply channel, receive — return success
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    float snr_db;       // AWGN SNR (set > 100 to disable)
    float cfo;          // CFO in cycles/sample
    float phase;        // constant phase offset (rad)
    float mp_alpha;     // multipath reflection amplitude (0 = disabled)
    float mp_theta;     // multipath reflection phase
    int   mp_delay;     // multipath delay in samples
} channel_params_t;

static int run_ofdm_channel_test(channel_params_t *ch, const char *label)
{
    // TX setup
    unsigned char p[OFDM_M];
    ofdmframe_init_default_sctype(OFDM_M, p);

    ofdmflexframegenprops_s fgprops;
    ofdmflexframegenprops_init_default(&fgprops);
    fgprops.check = OFDM_CRC;
    fgprops.fec0 = OFDM_FEC0;
    fgprops.fec1 = OFDM_FEC1;
    fgprops.mod_scheme = OFDM_MOD;

    ofdmflexframegen fg = ofdmflexframegen_create(OFDM_M, CP_LEN, TAPER_LEN,
                                                   p, &fgprops);

    // Prepare payload
    unsigned char header[8] = {0};
    unsigned char payload[PAYLOAD_LEN];
    for (int i = 0; i < PAYLOAD_LEN; i++)
        payload[i] = (unsigned char)(i & 0xFF);

    ofdmflexframegen_assemble(fg, header, payload, PAYLOAD_LEN);

    // Count symbols first
    int num_samples = 0;
    {
        float complex tmp[OFDM_M + CP_LEN];
        int last = 0;
        while (!last) {
            last = ofdmflexframegen_write(fg, tmp, OFDM_M + CP_LEN);
            num_samples += (OFDM_M + CP_LEN);
        }
    }

    // Generate frame
    float complex *tx_buf = calloc(num_samples + 64, sizeof(float complex));
    ofdmflexframegen_reset(fg);
    ofdmflexframegen_assemble(fg, header, payload, PAYLOAD_LEN);

    int idx = 0;
    int last = 0;
    while (!last) {
        float complex sym[OFDM_M + CP_LEN];
        last = ofdmflexframegen_write(fg, sym, OFDM_M + CP_LEN);
        memcpy(&tx_buf[idx], sym, (OFDM_M + CP_LEN) * sizeof(float complex));
        idx += (OFDM_M + CP_LEN);
    }

    // Apply channel impairments
    if (ch->cfo != 0.0f)
        apply_cfo(tx_buf, idx, ch->cfo);
    if (ch->phase != 0.0f)
        apply_phase_offset(tx_buf, idx, ch->phase);
    if (ch->mp_alpha > 0.0f)
        apply_multipath(tx_buf, idx, ch->mp_alpha, ch->mp_theta, ch->mp_delay);
    if (ch->snr_db < 100.0f)
        apply_awgn(tx_buf, idx, ch->snr_db);

    // RX
    memset(&rx_result, 0, sizeof(rx_result));
    ofdmflexframesync fs = ofdmflexframesync_create(OFDM_M, CP_LEN, TAPER_LEN,
                                                     p, rx_callback, NULL);
    ofdmflexframesync_execute(fs, tx_buf, idx);

    // Check result
    int success = rx_result.frame_detected && rx_result.payload_valid;
    int data_ok = 0;
    if (success) {
        data_ok = 1;
        for (int i = 0; i < PAYLOAD_LEN; i++) {
            if (rx_result.payload[i] != payload[i]) {
                data_ok = 0;
                break;
            }
        }
    }

    if (success && data_ok) {
        printf("  PASS: %s  (RSSI=%.1f EVM=%.1f CFO=%.4f)\n",
               label, rx_result.stats.rssi, rx_result.stats.evm,
               rx_result.stats.cfo);
        test_pass_count++;
    } else {
        printf("  FAIL: %s  (det=%d hdr=%d pay=%d data=%d)\n",
               label, rx_result.frame_detected, rx_result.header_valid,
               rx_result.payload_valid, data_ok);
        test_fail_count++;
    }

    // Cleanup
    ofdmflexframegen_destroy(fg);
    ofdmflexframesync_destroy(fs);
    free(tx_buf);

    return success && data_ok;
}

// Run test expecting failure (frame should NOT decode)
static void run_ofdm_channel_test_expect_fail(channel_params_t *ch,
                                               const char *label)
{
    unsigned char p[OFDM_M];
    ofdmframe_init_default_sctype(OFDM_M, p);

    ofdmflexframegenprops_s fgprops;
    ofdmflexframegenprops_init_default(&fgprops);
    fgprops.check = OFDM_CRC;
    fgprops.fec0 = OFDM_FEC0;
    fgprops.fec1 = OFDM_FEC1;
    fgprops.mod_scheme = OFDM_MOD;

    ofdmflexframegen fg = ofdmflexframegen_create(OFDM_M, CP_LEN, TAPER_LEN,
                                                   p, &fgprops);
    unsigned char header[8] = {0};
    unsigned char payload[PAYLOAD_LEN];
    for (int i = 0; i < PAYLOAD_LEN; i++)
        payload[i] = (unsigned char)(i & 0xFF);

    ofdmflexframegen_assemble(fg, header, payload, PAYLOAD_LEN);

    int num_samples = 0;
    {
        float complex tmp[OFDM_M + CP_LEN];
        int last = 0;
        while (!last) {
            last = ofdmflexframegen_write(fg, tmp, OFDM_M + CP_LEN);
            num_samples += (OFDM_M + CP_LEN);
        }
    }

    float complex *tx_buf = calloc(num_samples + 64, sizeof(float complex));
    ofdmflexframegen_reset(fg);
    ofdmflexframegen_assemble(fg, header, payload, PAYLOAD_LEN);
    int idx = 0;
    int last = 0;
    while (!last) {
        float complex sym[OFDM_M + CP_LEN];
        last = ofdmflexframegen_write(fg, sym, OFDM_M + CP_LEN);
        memcpy(&tx_buf[idx], sym, (OFDM_M + CP_LEN) * sizeof(float complex));
        idx += (OFDM_M + CP_LEN);
    }

    if (ch->cfo != 0.0f)
        apply_cfo(tx_buf, idx, ch->cfo);
    if (ch->phase != 0.0f)
        apply_phase_offset(tx_buf, idx, ch->phase);
    if (ch->mp_alpha > 0.0f)
        apply_multipath(tx_buf, idx, ch->mp_alpha, ch->mp_theta, ch->mp_delay);
    if (ch->snr_db < 100.0f)
        apply_awgn(tx_buf, idx, ch->snr_db);

    memset(&rx_result, 0, sizeof(rx_result));
    ofdmflexframesync fs = ofdmflexframesync_create(OFDM_M, CP_LEN, TAPER_LEN,
                                                     p, rx_callback, NULL);
    ofdmflexframesync_execute(fs, tx_buf, idx);

    int decoded = rx_result.frame_detected && rx_result.payload_valid;

    if (!decoded) {
        printf("  PASS: %s  (correctly failed to decode)\n", label);
        test_pass_count++;
    } else {
        printf("  FAIL: %s  (unexpectedly decoded — channel too benign?)\n", label);
        test_fail_count++;
    }

    ofdmflexframegen_destroy(fg);
    ofdmflexframesync_destroy(fs);
    free(tx_buf);
}

///////////////////////////////////////////////////////////////////////////////
// Test cases
///////////////////////////////////////////////////////////////////////////////

static void test_clean_channel(void)
{
    channel_params_t ch = {.snr_db = 200.0f};
    run_ofdm_channel_test(&ch, "clean channel (no impairments)");
}

// AWGN sweep
static void test_awgn_30db(void)
{
    srand(100);
    channel_params_t ch = {.snr_db = 30.0f};
    run_ofdm_channel_test(&ch, "AWGN 30 dB SNR");
}

static void test_awgn_20db(void)
{
    srand(101);
    channel_params_t ch = {.snr_db = 20.0f};
    run_ofdm_channel_test(&ch, "AWGN 20 dB SNR");
}

static void test_awgn_15db(void)
{
    srand(102);
    channel_params_t ch = {.snr_db = 15.0f};
    run_ofdm_channel_test(&ch, "AWGN 15 dB SNR");
}

static void test_awgn_10db(void)
{
    srand(103);
    channel_params_t ch = {.snr_db = 10.0f};
    run_ofdm_channel_test(&ch, "AWGN 10 dB SNR");
}

static void test_awgn_5db_should_fail(void)
{
    srand(104);
    channel_params_t ch = {.snr_db = 5.0f};
    run_ofdm_channel_test_expect_fail(&ch, "AWGN 5 dB SNR (expect failure)");
}

// CFO tests
static void test_cfo_small(void)
{
    srand(200);
    channel_params_t ch = {.snr_db = 30.0f, .cfo = 0.0001f};
    run_ofdm_channel_test(&ch, "CFO +0.0001 cyc/samp @ 30 dB");
}

static void test_cfo_medium(void)
{
    srand(201);
    channel_params_t ch = {.snr_db = 30.0f, .cfo = 0.001f};
    run_ofdm_channel_test(&ch, "CFO +0.001 cyc/samp @ 30 dB");
}

static void test_cfo_negative(void)
{
    srand(202);
    channel_params_t ch = {.snr_db = 30.0f, .cfo = -0.001f};
    run_ofdm_channel_test(&ch, "CFO -0.001 cyc/samp @ 30 dB");
}

static void test_cfo_large_should_fail(void)
{
    srand(203);
    channel_params_t ch = {.snr_db = 30.0f, .cfo = 0.05f};
    run_ofdm_channel_test_expect_fail(&ch, "CFO +0.05 cyc/samp (expect failure)");
}

// Phase offset tests
static void test_phase_pi4(void)
{
    channel_params_t ch = {.snr_db = 30.0f, .phase = (float)M_PI / 4.0f};
    run_ofdm_channel_test(&ch, "phase offset pi/4 @ 30 dB");
}

static void test_phase_pi2(void)
{
    channel_params_t ch = {.snr_db = 30.0f, .phase = (float)M_PI / 2.0f};
    run_ofdm_channel_test(&ch, "phase offset pi/2 @ 30 dB");
}

static void test_phase_pi(void)
{
    channel_params_t ch = {.snr_db = 30.0f, .phase = (float)M_PI};
    run_ofdm_channel_test(&ch, "phase offset pi @ 30 dB");
}

static void test_phase_arbitrary(void)
{
    channel_params_t ch = {.snr_db = 30.0f, .phase = 2.71828f};
    run_ofdm_channel_test(&ch, "phase offset 2.718 rad @ 30 dB");
}

// Multipath tests
static void test_multipath_weak(void)
{
    channel_params_t ch = {.snr_db = 30.0f,
                           .mp_alpha = 0.1f, .mp_theta = 0.5f, .mp_delay = 1};
    run_ofdm_channel_test(&ch, "multipath alpha=0.1 delay=1 @ 30 dB");
}

static void test_multipath_moderate(void)
{
    srand(300);
    channel_params_t ch = {.snr_db = 25.0f,
                           .mp_alpha = 0.3f, .mp_theta = 1.0f, .mp_delay = 2};
    run_ofdm_channel_test(&ch, "multipath alpha=0.3 delay=2 @ 25 dB");
}

static void test_multipath_strong_should_fail(void)
{
    srand(301);
    channel_params_t ch = {.snr_db = 20.0f,
                           .mp_alpha = 0.9f, .mp_theta = 0.0f, .mp_delay = 8};
    run_ofdm_channel_test_expect_fail(&ch, "multipath alpha=0.9 delay=8 (expect failure)");
}

// Combined impairments
static void test_combined_mild(void)
{
    srand(400);
    channel_params_t ch = {.snr_db = 25.0f, .cfo = 0.0005f, .phase = 0.3f,
                           .mp_alpha = 0.1f, .mp_theta = 0.8f, .mp_delay = 1};
    run_ofdm_channel_test(&ch, "combined: 25dB + small CFO + phase + weak multipath");
}

static void test_combined_moderate(void)
{
    srand(401);
    channel_params_t ch = {.snr_db = 20.0f, .cfo = 0.001f, .phase = 1.5f,
                           .mp_alpha = 0.2f, .mp_theta = 1.2f, .mp_delay = 2};
    run_ofdm_channel_test(&ch, "combined: 20dB + CFO 0.001 + phase + multipath");
}

static void test_combined_stress(void)
{
    srand(402);
    channel_params_t ch = {.snr_db = 15.0f, .cfo = 0.001f, .phase = 2.0f,
                           .mp_alpha = 0.2f, .mp_theta = 0.5f, .mp_delay = 3};
    run_ofdm_channel_test(&ch, "combined stress: 15dB + CFO + phase + multipath");
}

///////////////////////////////////////////////////////////////////////////////
// Main
///////////////////////////////////////////////////////////////////////////////

int main(void)
{
    printf("=== OFDM Channel Impairment Tests ===\n");
    printf("Config: OFDM-%d QAM-16 SECDED+HAMMING CRC-32 payload=%d\n\n",
           OFDM_M, PAYLOAD_LEN);

    printf("--- Clean channel ---\n");
    RUN_TEST(test_clean_channel);

    printf("\n--- AWGN noise sweep ---\n");
    RUN_TEST(test_awgn_30db);
    RUN_TEST(test_awgn_20db);
    RUN_TEST(test_awgn_15db);
    RUN_TEST(test_awgn_10db);
    RUN_TEST(test_awgn_5db_should_fail);

    printf("\n--- Carrier frequency offset ---\n");
    RUN_TEST(test_cfo_small);
    RUN_TEST(test_cfo_medium);
    RUN_TEST(test_cfo_negative);
    RUN_TEST(test_cfo_large_should_fail);

    printf("\n--- Phase offset ---\n");
    RUN_TEST(test_phase_pi4);
    RUN_TEST(test_phase_pi2);
    RUN_TEST(test_phase_pi);
    RUN_TEST(test_phase_arbitrary);

    printf("\n--- Multipath ---\n");
    RUN_TEST(test_multipath_weak);
    RUN_TEST(test_multipath_moderate);
    RUN_TEST(test_multipath_strong_should_fail);

    printf("\n--- Combined impairments ---\n");
    RUN_TEST(test_combined_mild);
    RUN_TEST(test_combined_moderate);
    RUN_TEST(test_combined_stress);

    TEST_SUMMARY();

    return test_fail_count ? 1 : 0;
}
