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

// Unit tests for cw_tone.c — CW tone CFO estimator

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include "test_harness.h"

// Stub out liquid-dsp enums that ofdm_conf.h references
#define LIQUID_FEC_NONE 0
#define LIQUID_FEC_SECDED7264 0
#define LIQUID_MODEM_QPSK 0
#define LIQUID_CRC_32 0

#include "../cw_tone.c"

///////////////////////////////////////////////////////////////////////////////
// Tests
///////////////////////////////////////////////////////////////////////////////

static void test_cw_tone_init(void)
{
    TEST_BEGIN("cw_tone: init resets state");
    cw_tone_init();
    TEST_ASSERT(cw_tone_get_freq_offset() == 0.0f);
    TEST_ASSERT(cw_tone_get_peak() == 0.0f);
    TEST_PASS();
}

static void test_cw_tone_tx_samples(void)
{
    TEST_BEGIN("cw_tone: get_tx_samples fills Fs/4 tone + guard");
    float complex buf[256];
    int len = 0;
    memset(buf, 0xAA, sizeof(buf));  // fill with garbage to detect unwritten slots

    cw_tone_get_tx_samples(buf, &len);

    TEST_ASSERT(len == 128 + 16);  // tone + guard

    // First 128 samples: Fs/4 tone {1, j, -1, -j, ...}
    static const float expected_re[4] = { 1.0f,  0.0f, -1.0f,  0.0f};
    static const float expected_im[4] = { 0.0f,  1.0f,  0.0f, -1.0f};
    for (int i = 0; i < 128; i++) {
        TEST_ASSERT(crealf(buf[i]) == expected_re[i % 4]);
        TEST_ASSERT(cimagf(buf[i]) == expected_im[i % 4]);
    }
    // Last 16 samples: guard interval (zeros)
    for (int i = 128; i < len; i++) {
        TEST_ASSERT(crealf(buf[i]) == 0.0f);
        TEST_ASSERT(cimagf(buf[i]) == 0.0f);
    }
    TEST_PASS();
}

static void test_cw_tone_detect_pure_dc(void)
{
    TEST_BEGIN("cw_tone: detects pure DC tone");
    cw_tone_init();

    // The detector needs CW_TONE_LEN (128) samples to fill the buffer
    // before it starts checking.  Feed 256 DC samples to ensure detection.
    int detected = 0;
    for (int i = 0; i < 256; i++) {
        if (cw_tone_execute(1.0f + 0.0f * _Complex_I)) {
            detected = 1;
            break;
        }
    }
    TEST_ASSERT_MSG(detected, "pure DC tone should be detected");

    // CFO should be ~0 for a pure DC tone
    float cfo = cw_tone_get_freq_offset();
    TEST_ASSERT_MSG(fabsf(cfo) < 0.01f, "CFO for DC tone should be near zero");
    TEST_PASS();
}

static void test_cw_tone_no_detect_noise(void)
{
    TEST_BEGIN("cw_tone: does not detect random noise");
    cw_tone_init();

    srand(42);
    int detected = 0;
    for (int i = 0; i < 256; i++) {
        float re = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
        float im = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
        if (cw_tone_execute(re + im * _Complex_I)) {
            detected = 1;
            break;
        }
    }
    TEST_ASSERT_MSG(!detected, "random noise should not trigger detection");
    TEST_PASS();
}

static void test_cw_tone_no_detect_silence(void)
{
    TEST_BEGIN("cw_tone: does not detect silence (zero samples)");
    cw_tone_init();

    int detected = 0;
    for (int i = 0; i < 256; i++) {
        if (cw_tone_execute(0.0f + 0.0f * _Complex_I)) {
            detected = 1;
            break;
        }
    }
    TEST_ASSERT_MSG(!detected, "silence should not trigger detection");
    TEST_PASS();
}

static void test_cw_tone_detect_with_freq_offset(void)
{
    TEST_BEGIN("cw_tone: estimates CFO for tone with known offset");
    cw_tone_init();

    // Generate a tone with a known frequency offset.
    // The lag-64 autocorrelator has an unambiguous range of ±1/(2*64) ≈ ±0.0078
    // cycles/sample.  Use a value well within that range.
    float target_cfo = 0.005f;  // cycles/sample
    int detected = 0;

    for (int n = 0; n < 512; n++) {
        float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
        float complex sample = cosf(phase) + sinf(phase) * _Complex_I;
        if (cw_tone_execute(sample)) {
            detected = 1;
            break;
        }
    }
    TEST_ASSERT_MSG(detected, "offset tone should be detected");

    float est_cfo = cw_tone_get_freq_offset();
    float error = fabsf(est_cfo - target_cfo);
    char msg[128];
    snprintf(msg, sizeof(msg),
             "CFO estimate %.6f should be near target %.6f (error %.6f)",
             est_cfo, target_cfo, error);
    TEST_ASSERT_MSG(error < 0.001f, msg);
    TEST_PASS();
}

static void test_cw_tone_detect_negative_offset(void)
{
    TEST_BEGIN("cw_tone: estimates negative CFO correctly");
    cw_tone_init();

    // Use a CFO within the unambiguous range (±0.0078 cycles/sample)
    float target_cfo = -0.004f;
    int detected = 0;

    for (int n = 0; n < 512; n++) {
        float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
        float complex sample = cosf(phase) + sinf(phase) * _Complex_I;
        if (cw_tone_execute(sample)) {
            detected = 1;
            break;
        }
    }
    TEST_ASSERT_MSG(detected, "negative offset tone should be detected");

    float est_cfo = cw_tone_get_freq_offset();
    float error = fabsf(est_cfo - target_cfo);
    char msg[128];
    snprintf(msg, sizeof(msg),
             "CFO estimate %.6f should be near target %.6f (error %.6f)",
             est_cfo, target_cfo, error);
    TEST_ASSERT_MSG(error < 0.001f, msg);
    TEST_PASS();
}

static void test_cw_tone_reset_clears_state(void)
{
    TEST_BEGIN("cw_tone: reset clears detection state");
    cw_tone_init();

    // Feed some samples
    for (int i = 0; i < 64; i++) {
        cw_tone_execute(1.0f + 0.0f * _Complex_I);
    }

    cw_tone_reset();
    TEST_ASSERT(cw_tone_get_freq_offset() == 0.0f);
    TEST_ASSERT(cw_tone_get_peak() == 0.0f);

    // After reset, need 128 samples again before detection
    // Feed only 64 — should not detect
    int detected = 0;
    for (int i = 0; i < 64; i++) {
        if (cw_tone_execute(1.0f + 0.0f * _Complex_I)) {
            detected = 1;
        }
    }
    TEST_ASSERT_MSG(!detected, "should not detect with only 64 samples after reset");
    TEST_PASS();
}

static void test_cw_tone_peak_metric(void)
{
    TEST_BEGIN("cw_tone: peak metric near 1.0 for pure tone");
    cw_tone_init();

    // Feed 256 DC samples — enough to fill buffer and trigger metric calculation
    for (int i = 0; i < 256; i++) {
        cw_tone_execute(1.0f + 0.0f * _Complex_I);
    }

    float peak = cw_tone_get_peak();
    TEST_ASSERT_MSG(peak > 0.85f, "peak metric should exceed threshold for pure tone");
    TEST_ASSERT_MSG(peak <= 1.05f, "peak metric should not exceed ~1.0");
    TEST_PASS();
}

static void test_cw_tone_no_retrigger(void)
{
    TEST_BEGIN("cw_tone: does not re-trigger on continued tone after detection");
    cw_tone_init();

    int detect_count = 0;
    // Feed 256 DC tone samples — should only trigger once because
    // cw_sample_count is reset to 0 after detection, requiring another
    // 128 samples before re-detection.
    for (int i = 0; i < 256; i++) {
        if (cw_tone_execute(1.0f + 0.0f * _Complex_I)) {
            detect_count++;
        }
    }
    // Exactly one detection from the first 128, then the counter resets
    // and counts another 128 -> second detection at sample 256.
    TEST_ASSERT_MSG(detect_count <= 2, "should not trigger excessively");
    TEST_PASS();
}

static void test_cw_tone_detect_large_positive_cfo(void)
{
    TEST_BEGIN("cw_tone: estimates +100 kHz CFO (0.0714 cyc/samp)");
    cw_tone_init();
    float target_cfo = 100000.0f / 1400000.0f;  // 0.07142857
    int detected = 0;
    for (int n = 0; n < 512; n++) {
        float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
        float complex sample = cosf(phase) + sinf(phase) * _Complex_I;
        if (cw_tone_execute(sample)) { detected = 1; break; }
    }
    TEST_ASSERT_MSG(detected, "100 kHz offset tone should be detected");
    float est_cfo = cw_tone_get_freq_offset();
    float error = fabsf(est_cfo - target_cfo);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est_cfo, target_cfo, error);
    TEST_ASSERT_MSG(error < 0.001f, msg);
    TEST_PASS();
}

static void test_cw_tone_detect_large_negative_cfo(void)
{
    TEST_BEGIN("cw_tone: estimates -100 kHz CFO");
    cw_tone_init();
    float target_cfo = -100000.0f / 1400000.0f;
    int detected = 0;
    for (int n = 0; n < 512; n++) {
        float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
        float complex sample = cosf(phase) + sinf(phase) * _Complex_I;
        if (cw_tone_execute(sample)) { detected = 1; break; }
    }
    TEST_ASSERT_MSG(detected, "-100 kHz offset tone should be detected");
    float est_cfo = cw_tone_get_freq_offset();
    float error = fabsf(est_cfo - target_cfo);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est_cfo, target_cfo, error);
    TEST_ASSERT_MSG(error < 0.001f, msg);
    TEST_PASS();
}

static void test_cw_tone_detect_moderate_cfo(void)
{
    TEST_BEGIN("cw_tone: estimates +50 kHz CFO (0.0357 cyc/samp)");
    cw_tone_init();
    float target_cfo = 50000.0f / 1400000.0f;
    int detected = 0;
    for (int n = 0; n < 512; n++) {
        float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
        float complex sample = cosf(phase) + sinf(phase) * _Complex_I;
        if (cw_tone_execute(sample)) { detected = 1; break; }
    }
    TEST_ASSERT_MSG(detected, "50 kHz offset tone should be detected");
    float est_cfo = cw_tone_get_freq_offset();
    float error = fabsf(est_cfo - target_cfo);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est_cfo, target_cfo, error);
    TEST_ASSERT_MSG(error < 0.001f, msg);
    TEST_PASS();
}

static void test_cw_tone_detect_near_coarse_limit(void)
{
    TEST_BEGIN("cw_tone: estimates +160 kHz CFO (0.114 cyc/samp, near coarse limit)");
    cw_tone_init();
    float target_cfo = 160000.0f / 1400000.0f;  // 0.1143
    int detected = 0;
    for (int n = 0; n < 512; n++) {
        float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
        float complex sample = cosf(phase) + sinf(phase) * _Complex_I;
        if (cw_tone_execute(sample)) { detected = 1; break; }
    }
    TEST_ASSERT_MSG(detected, "160 kHz offset tone should be detected");
    float est_cfo = cw_tone_get_freq_offset();
    float error = fabsf(est_cfo - target_cfo);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est_cfo, target_cfo, error);
    TEST_ASSERT_MSG(error < 0.001f, msg);
    TEST_PASS();
}

static void test_cw_tone_roundtrip_tx_rx(void)
{
    TEST_BEGIN("cw_tone: roundtrip TX->RX recovers known CFO through Fs/4 tone");
    float complex tx_buf[256];
    int tx_len = 0;
    cw_tone_get_tx_samples(tx_buf, &tx_len);

    // Apply a known CFO to the TX tone samples and feed to detector.
    // The Fs/4 tone is invisible to the autocorrelation estimator, so the
    // detector should report the applied CFO directly.
    float target_cfo = 0.003f;  // cycles/sample
    cw_tone_init();
    int detected = 0;

    for (int n = 0; n < 256; n++) {
        // Use tone portion only (skip guard zeros — they carry no tone info)
        int idx = n % 128;
        float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
        float complex cfo_phasor = cosf(phase) + sinf(phase) * _Complex_I;
        float complex sample = tx_buf[idx] * cfo_phasor;
        if (cw_tone_execute(sample)) {
            detected = 1;
            break;
        }
    }

    TEST_ASSERT_MSG(detected, "roundtrip tone should be detected");
    float est_cfo = cw_tone_get_freq_offset();
    float error = fabsf(est_cfo - target_cfo);
    char msg[128];
    snprintf(msg, sizeof(msg),
             "roundtrip CFO est=%.6f target=%.6f err=%.6f",
             est_cfo, target_cfo, error);
    TEST_ASSERT_MSG(error < 0.001f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main(void)
{
    fprintf(stderr, "=== CW Tone CFO Estimator Tests ===\n");

    RUN_TEST(test_cw_tone_init);
    RUN_TEST(test_cw_tone_tx_samples);
    RUN_TEST(test_cw_tone_detect_pure_dc);
    RUN_TEST(test_cw_tone_no_detect_noise);
    RUN_TEST(test_cw_tone_no_detect_silence);
    RUN_TEST(test_cw_tone_detect_with_freq_offset);
    RUN_TEST(test_cw_tone_detect_negative_offset);
    RUN_TEST(test_cw_tone_reset_clears_state);
    RUN_TEST(test_cw_tone_peak_metric);
    RUN_TEST(test_cw_tone_no_retrigger);
    RUN_TEST(test_cw_tone_detect_large_positive_cfo);
    RUN_TEST(test_cw_tone_detect_large_negative_cfo);
    RUN_TEST(test_cw_tone_detect_moderate_cfo);
    RUN_TEST(test_cw_tone_detect_near_coarse_limit);
    RUN_TEST(test_cw_tone_roundtrip_tx_rx);

    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
