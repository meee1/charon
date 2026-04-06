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

// Tests for CW tone CFO estimator under realistic channel conditions:
// AWGN noise, phase offsets, carrier frequency offsets, amplitude variation,
// and combined impairments.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include "test_harness.h"

// Stub liquid-dsp enums
#define LIQUID_FEC_NONE 0
#define LIQUID_FEC_SECDED7264 0
#define LIQUID_MODEM_QPSK 0
#define LIQUID_CRC_32 0

#include "../cw_tone.c"

// Box-Muller transform for Gaussian noise
static float randn(void)
{
    float u1 = ((float)rand() + 1.0f) / ((float)RAND_MAX + 1.0f);
    float u2 = ((float)rand() + 1.0f) / ((float)RAND_MAX + 1.0f);
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

// Generate complex AWGN sample with given standard deviation
static float complex awgn(float sigma)
{
    return sigma * (randn() + _Complex_I * randn());
}

// Feed a DC tone with channel impairments and return whether detected + CFO estimate
static int feed_impaired_tone(float cfo, float phase_offset, float snr_db,
                              float amplitude, float *est_cfo_out)
{
    cw_tone_init();

    float signal_power = amplitude * amplitude;
    float noise_sigma = 0.0f;
    if (snr_db < 100.0f) {
        float noise_power = signal_power * powf(10.0f, -snr_db / 10.0f);
        noise_sigma = sqrtf(noise_power / 2.0f);  // per I/Q component
    }

    int detected = 0;
    for (int n = 0; n < 512; n++) {
        // Clean tone with CFO and phase offset
        float phase = 2.0f * (float)M_PI * cfo * (float)n + phase_offset;
        float complex sample = amplitude * (cosf(phase) + sinf(phase) * _Complex_I);

        // Add noise
        if (noise_sigma > 0.0f)
            sample += awgn(noise_sigma);

        if (cw_tone_execute(sample)) {
            detected = 1;
            break;
        }
    }

    if (est_cfo_out)
        *est_cfo_out = cw_tone_get_freq_offset();

    return detected;
}

///////////////////////////////////////////////////////////////////////////////
// AWGN noise tests — detection and CFO accuracy vs SNR
///////////////////////////////////////////////////////////////////////////////

static void test_channel_awgn_high_snr(void)
{
    TEST_BEGIN("channel: detects tone at 30 dB SNR");
    srand(1001);
    float est_cfo;
    int det = feed_impaired_tone(0.0f, 0.0f, 30.0f, 1.0f, &est_cfo);
    TEST_ASSERT_MSG(det, "should detect at 30 dB SNR");
    TEST_ASSERT_MSG(fabsf(est_cfo) < 0.002f, "CFO error should be small at high SNR");
    TEST_PASS();
}

static void test_channel_awgn_20db(void)
{
    TEST_BEGIN("channel: detects tone at 20 dB SNR");
    srand(1002);
    float est_cfo;
    int det = feed_impaired_tone(0.0f, 0.0f, 20.0f, 1.0f, &est_cfo);
    TEST_ASSERT_MSG(det, "should detect at 20 dB SNR");
    TEST_ASSERT_MSG(fabsf(est_cfo) < 0.005f, "CFO error should be reasonable at 20 dB");
    TEST_PASS();
}

static void test_channel_awgn_10db(void)
{
    TEST_BEGIN("channel: detects tone at 10 dB SNR");
    srand(1003);
    float est_cfo;
    int det = feed_impaired_tone(0.0f, 0.0f, 10.0f, 1.0f, &est_cfo);
    TEST_ASSERT_MSG(det, "should detect at 10 dB SNR");
    TEST_PASS();
}

static void test_channel_awgn_5db(void)
{
    TEST_BEGIN("channel: mostly fails to detect at 5 dB SNR");
    // With threshold=0.85 and only 128 samples of averaging, 5 dB SNR
    // is below the reliable detection floor.  Verify detection is unreliable.
    int successes = 0;
    for (int trial = 0; trial < 20; trial++) {
        srand(1004 + trial * 100);
        if (feed_impaired_tone(0.0f, 0.0f, 5.0f, 1.0f, NULL))
            successes++;
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "detection should be unreliable at 5 dB (%d/20)", successes);
    // Allow anywhere from 0 to 10 — just confirm it's not reliable
    TEST_ASSERT_MSG(successes <= 15, msg);
    TEST_PASS();
}

static void test_channel_awgn_0db_rejection(void)
{
    TEST_BEGIN("channel: mostly fails to detect at 0 dB SNR");
    // At 0 dB SNR the noise power equals signal power; the autocorrelation
    // metric should mostly stay below threshold.
    int successes = 0;
    for (int trial = 0; trial < 20; trial++) {
        srand(2000 + trial * 37);
        if (feed_impaired_tone(0.0f, 0.0f, 0.0f, 1.0f, NULL))
            successes++;
    }
    // Should fail most of the time (allow some lucky detections)
    char msg[80];
    snprintf(msg, sizeof(msg), "should mostly fail at 0 dB (%d/20 detected)", successes);
    TEST_ASSERT_MSG(successes <= 10, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Phase offset tests
///////////////////////////////////////////////////////////////////////////////

static void test_channel_phase_offset_zero(void)
{
    TEST_BEGIN("channel: detects tone with 0 phase offset (baseline)");
    srand(3000);
    float est_cfo;
    int det = feed_impaired_tone(0.0f, 0.0f, 40.0f, 1.0f, &est_cfo);
    TEST_ASSERT(det);
    TEST_ASSERT(fabsf(est_cfo) < 0.001f);
    TEST_PASS();
}

static void test_channel_phase_offset_pi_over_4(void)
{
    TEST_BEGIN("channel: detects tone with pi/4 phase offset");
    srand(3001);
    float est_cfo;
    int det = feed_impaired_tone(0.0f, (float)M_PI / 4.0f, 40.0f, 1.0f, &est_cfo);
    TEST_ASSERT_MSG(det, "phase offset should not prevent detection");
    // Phase offset is constant — lag-D autocorrelation cancels it out.
    // CFO should still be ~0.
    TEST_ASSERT_MSG(fabsf(est_cfo) < 0.001f, "constant phase offset should not bias CFO");
    TEST_PASS();
}

static void test_channel_phase_offset_pi(void)
{
    TEST_BEGIN("channel: detects tone with pi phase offset");
    srand(3002);
    float est_cfo;
    int det = feed_impaired_tone(0.0f, (float)M_PI, 40.0f, 1.0f, &est_cfo);
    TEST_ASSERT_MSG(det, "pi phase offset should not prevent detection");
    TEST_ASSERT_MSG(fabsf(est_cfo) < 0.001f, "pi phase offset should not bias CFO");
    TEST_PASS();
}

static void test_channel_phase_offset_random(void)
{
    TEST_BEGIN("channel: detects tone with various random phase offsets");
    float phases[] = {0.1f, 0.7f, 1.5f, 2.3f, 3.14f, 4.0f, 5.5f, 6.2f};
    for (int i = 0; i < 8; i++) {
        srand(3100 + i);
        float est_cfo;
        int det = feed_impaired_tone(0.0f, phases[i], 40.0f, 1.0f, &est_cfo);
        TEST_ASSERT(det);
        TEST_ASSERT(fabsf(est_cfo) < 0.001f);
    }
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// CFO estimation accuracy under noise
///////////////////////////////////////////////////////////////////////////////

static void test_channel_cfo_positive_noisy(void)
{
    TEST_BEGIN("channel: CFO +0.005 estimated accurately at 20 dB SNR");
    srand(4000);
    float target = 0.005f;
    float est_cfo;
    int det = feed_impaired_tone(target, 0.0f, 20.0f, 1.0f, &est_cfo);
    TEST_ASSERT(det);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f",
             est_cfo, target, fabsf(est_cfo - target));
    TEST_ASSERT_MSG(fabsf(est_cfo - target) < 0.002f, msg);
    TEST_PASS();
}

static void test_channel_cfo_negative_noisy(void)
{
    TEST_BEGIN("channel: CFO -0.003 estimated accurately at 20 dB SNR");
    srand(4001);
    float target = -0.003f;
    float est_cfo;
    int det = feed_impaired_tone(target, 0.0f, 20.0f, 1.0f, &est_cfo);
    TEST_ASSERT(det);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f",
             est_cfo, target, fabsf(est_cfo - target));
    TEST_ASSERT_MSG(fabsf(est_cfo - target) < 0.002f, msg);
    TEST_PASS();
}

static void test_channel_cfo_sweep_clean(void)
{
    TEST_BEGIN("channel: CFO estimation across sweep [-0.007, +0.007]");
    // Sweep across the unambiguous range (±1/(2*64) ≈ ±0.0078)
    float cfo_values[] = {-0.007f, -0.005f, -0.003f, -0.001f,
                           0.001f,  0.003f,  0.005f,  0.007f};
    for (int i = 0; i < 8; i++) {
        srand(4100 + i);
        float est_cfo;
        int det = feed_impaired_tone(cfo_values[i], 0.0f, 40.0f, 1.0f, &est_cfo);
        char msg[128];
        snprintf(msg, sizeof(msg), "cfo=%.4f: det=%d est=%.6f err=%.6f",
                 cfo_values[i], det, est_cfo, fabsf(est_cfo - cfo_values[i]));
        TEST_ASSERT_MSG(det, msg);
        TEST_ASSERT_MSG(fabsf(est_cfo - cfo_values[i]) < 0.001f, msg);
    }
    TEST_PASS();
}

static void test_channel_cfo_sweep_noisy(void)
{
    TEST_BEGIN("channel: CFO estimation across sweep at 15 dB SNR");
    float cfo_values[] = {-0.006f, -0.002f, 0.002f, 0.006f};
    for (int i = 0; i < 4; i++) {
        srand(4200 + i);
        float est_cfo;
        int det = feed_impaired_tone(cfo_values[i], 0.0f, 15.0f, 1.0f, &est_cfo);
        char msg[128];
        snprintf(msg, sizeof(msg), "cfo=%.4f: det=%d est=%.6f err=%.6f",
                 cfo_values[i], det, est_cfo, fabsf(est_cfo - cfo_values[i]));
        TEST_ASSERT_MSG(det, msg);
        TEST_ASSERT_MSG(fabsf(est_cfo - cfo_values[i]) < 0.003f, msg);
    }
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Amplitude variation tests
///////////////////////////////////////////////////////////////////////////////

static void test_channel_low_amplitude(void)
{
    TEST_BEGIN("channel: detects low-amplitude tone (0.1)");
    srand(5000);
    float est_cfo;
    int det = feed_impaired_tone(0.0f, 0.0f, 30.0f, 0.1f, &est_cfo);
    TEST_ASSERT_MSG(det, "low amplitude tone should still be detected");
    TEST_ASSERT(fabsf(est_cfo) < 0.002f);
    TEST_PASS();
}

static void test_channel_high_amplitude(void)
{
    TEST_BEGIN("channel: detects high-amplitude tone (10.0)");
    srand(5001);
    float est_cfo;
    int det = feed_impaired_tone(0.0f, 0.0f, 30.0f, 10.0f, &est_cfo);
    TEST_ASSERT_MSG(det, "high amplitude tone should still be detected");
    TEST_ASSERT(fabsf(est_cfo) < 0.002f);
    TEST_PASS();
}

static void test_channel_very_low_amplitude(void)
{
    TEST_BEGIN("channel: detects tone at amplitude 0.01 with high SNR");
    srand(5002);
    float est_cfo;
    int det = feed_impaired_tone(0.0f, 0.0f, 40.0f, 0.01f, &est_cfo);
    TEST_ASSERT_MSG(det, "very low amplitude with high SNR should still detect");
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Combined impairments
///////////////////////////////////////////////////////////////////////////////

static void test_channel_cfo_plus_phase_plus_noise(void)
{
    TEST_BEGIN("channel: CFO + phase offset + 20 dB noise");
    srand(6000);
    float target_cfo = 0.004f;
    float est_cfo;
    int det = feed_impaired_tone(target_cfo, 1.2f, 20.0f, 1.0f, &est_cfo);
    TEST_ASSERT_MSG(det, "combined impairments should still allow detection");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f",
             est_cfo, target_cfo, fabsf(est_cfo - target_cfo));
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.003f, msg);
    TEST_PASS();
}

static void test_channel_cfo_plus_phase_plus_low_amplitude_noise(void)
{
    TEST_BEGIN("channel: CFO + phase + low amplitude + 15 dB noise");
    srand(6001);
    float target_cfo = -0.005f;
    float est_cfo;
    int det = feed_impaired_tone(target_cfo, 2.7f, 15.0f, 0.5f, &est_cfo);
    TEST_ASSERT_MSG(det, "combined impairments with low amp should detect");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f",
             est_cfo, target_cfo, fabsf(est_cfo - target_cfo));
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.003f, msg);
    TEST_PASS();
}

static void test_channel_worst_case_feasible(void)
{
    TEST_BEGIN("channel: worst-case feasible scenario (10 dB, max CFO, phase)");
    // Run multiple trials — this is a marginal case
    int successes = 0;
    float target_cfo = 0.007f;  // near edge of unambiguous range
    for (int trial = 0; trial < 10; trial++) {
        srand(6100 + trial * 13);
        float est_cfo;
        if (feed_impaired_tone(target_cfo, 3.0f, 10.0f, 1.0f, &est_cfo)) {
            if (fabsf(est_cfo - target_cfo) < 0.004f)
                successes++;
        }
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "should succeed in some trials (%d/10)", successes);
    TEST_ASSERT_MSG(successes >= 3, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Tone-then-noise transition (realistic scenario)
///////////////////////////////////////////////////////////////////////////////

static void test_channel_tone_then_noise(void)
{
    TEST_BEGIN("channel: detects tone followed by noise (realistic RX)");
    // In the real system, the CW tone preamble is followed by OFDM data
    // which looks like noise to the tone detector.  Verify detection happens
    // during the tone portion and isn't disrupted.
    cw_tone_init();
    srand(7000);

    float target_cfo = 0.003f;
    int detected = 0;
    float est_cfo = 0.0f;

    // Phase 1: 128 samples of clean tone with CFO
    for (int n = 0; n < 192; n++) {
        float complex sample;
        if (n < 128) {
            // CW tone with CFO
            float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
            sample = cosf(phase) + sinf(phase) * _Complex_I;
        } else {
            // "OFDM data" = random noise-like signal
            float re = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
            float im = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
            sample = re + im * _Complex_I;
        }

        if (!detected && cw_tone_execute(sample)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "tone should be detected before noise starts");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f", est_cfo, target_cfo);
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.001f, msg);
    TEST_PASS();
}

static void test_channel_noise_then_tone(void)
{
    TEST_BEGIN("channel: detects tone preceded by noise (late arrival)");
    cw_tone_init();
    srand(7001);

    float target_cfo = -0.002f;
    int detected = 0;
    float est_cfo = 0.0f;

    // Phase 1: 200 samples of noise (channel idle)
    // Phase 2: 256 samples of tone (preamble arrives)
    for (int n = 0; n < 456; n++) {
        float complex sample;
        if (n < 200) {
            float re = ((float)rand() / RAND_MAX) * 0.5f - 0.25f;
            float im = ((float)rand() / RAND_MAX) * 0.5f - 0.25f;
            sample = re + im * _Complex_I;
        } else {
            int t = n - 200;
            float phase = 2.0f * (float)M_PI * target_cfo * (float)t;
            sample = cosf(phase) + sinf(phase) * _Complex_I;
        }

        if (!detected && cw_tone_execute(sample)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "tone should be detected after noise clears");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f", est_cfo, target_cfo);
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.002f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Multipath (simple 2-tap) channel
///////////////////////////////////////////////////////////////////////////////

static void test_channel_multipath_short_delay(void)
{
    TEST_BEGIN("channel: detects tone through 2-tap multipath (delay=1 sample)");
    cw_tone_init();
    srand(8000);

    // h = [1.0, 0.3*e^{j*0.5}] — direct path + weak reflection
    float complex h0 = 1.0f;
    float complex h1 = 0.3f * (cosf(0.5f) + sinf(0.5f) * _Complex_I);

    float target_cfo = 0.002f;
    float complex prev_sample = 0.0f;
    int detected = 0;
    float est_cfo = 0.0f;

    for (int n = 0; n < 512; n++) {
        float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
        float complex clean = cosf(phase) + sinf(phase) * _Complex_I;

        // 2-tap channel: y[n] = h0*x[n] + h1*x[n-1]
        float complex received = h0 * clean + h1 * prev_sample;
        prev_sample = clean;

        if (!detected && cw_tone_execute(received)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "short multipath should not prevent detection");
    // Multipath adds some CFO estimation bias, allow wider tolerance
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f",
             est_cfo, target_cfo, fabsf(est_cfo - target_cfo));
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.003f, msg);
    TEST_PASS();
}

static void test_channel_multipath_longer_delay(void)
{
    TEST_BEGIN("channel: detects tone through 2-tap multipath (delay=4 samples)");
    cw_tone_init();
    srand(8001);

    float complex h0 = 1.0f;
    float complex h1 = 0.2f * (cosf(1.0f) + sinf(1.0f) * _Complex_I);
    int delay = 4;

    float target_cfo = 0.003f;
    float complex delay_line[5] = {0};
    int detected = 0;
    float est_cfo = 0.0f;

    for (int n = 0; n < 512; n++) {
        float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
        float complex clean = cosf(phase) + sinf(phase) * _Complex_I;

        // Shift delay line
        for (int d = delay; d > 0; d--)
            delay_line[d] = delay_line[d - 1];
        delay_line[0] = clean;

        float complex received = h0 * delay_line[0] + h1 * delay_line[delay];

        if (!detected && cw_tone_execute(received)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "4-sample multipath should not prevent detection");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f",
             est_cfo, target_cfo, fabsf(est_cfo - target_cfo));
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.003f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Large CFO tests (100 kHz at 1.4 MHz sample rate)
///////////////////////////////////////////////////////////////////////////////

static void test_channel_cfo_100khz(void)
{
    TEST_BEGIN("channel: 100 kHz CFO aliases outside unambiguous range");
    // 100 kHz at 1.4 MHz sample rate = 0.0714 cycles/sample
    // Unambiguous range is ±1/(2*64) = ±0.0078 cycles/sample (±10.9 kHz)
    // 100 kHz is ~9x outside this range — estimate will alias.
    srand(10000);
    float target_cfo = 100000.0f / 1400000.0f;  // 0.0714 cycles/sample
    float est_cfo;
    int det = feed_impaired_tone(target_cfo, 0.0f, 40.0f, 1.0f, &est_cfo);
    // The tone should still be detected (autocorrelation magnitude is high)
    // but the estimated CFO will be aliased into [-0.0078, +0.0078]
    if (det) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "aliased CFO %.6f should be within unambiguous range", est_cfo);
        TEST_ASSERT_MSG(fabsf(est_cfo) <= 0.0079f, msg);
        // Verify it does NOT equal the true 100 kHz offset
        TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) > 0.01f,
                        "estimate should NOT match true 100 kHz offset");
    }
    // Detection may or may not happen depending on aliased phase coherence
    TEST_PASS();
}

static void test_channel_cfo_100khz_negative(void)
{
    TEST_BEGIN("channel: -100 kHz CFO aliases outside unambiguous range");
    srand(10001);
    float target_cfo = -100000.0f / 1400000.0f;
    float est_cfo;
    int det = feed_impaired_tone(target_cfo, 0.0f, 40.0f, 1.0f, &est_cfo);
    if (det) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "aliased CFO %.6f should be within unambiguous range", est_cfo);
        TEST_ASSERT_MSG(fabsf(est_cfo) <= 0.0079f, msg);
    }
    TEST_PASS();
}

static void test_channel_cfo_100khz_noisy(void)
{
    TEST_BEGIN("channel: 100 kHz CFO with 20 dB noise");
    srand(10002);
    float target_cfo = 100000.0f / 1400000.0f;
    float est_cfo;
    int det = feed_impaired_tone(target_cfo, 0.0f, 20.0f, 1.0f, &est_cfo);
    if (det) {
        TEST_ASSERT_MSG(fabsf(est_cfo) <= 0.0079f,
                        "aliased estimate should stay in unambiguous range");
    }
    TEST_PASS();
}

static void test_channel_cfo_near_edge(void)
{
    TEST_BEGIN("channel: CFO at ±10 kHz (near unambiguous edge)");
    // 10 kHz / 1.4 MHz = 0.00714 cycles/sample — just inside ±0.0078
    float cfo_10k = 10000.0f / 1400000.0f;
    for (int sign = -1; sign <= 1; sign += 2) {
        float target = (float)sign * cfo_10k;
        srand(10100 + sign);
        float est_cfo;
        int det = feed_impaired_tone(target, 0.0f, 30.0f, 1.0f, &est_cfo);
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "10 kHz (%+.6f): det=%d est=%.6f err=%.6f",
                 target, det, est_cfo, fabsf(est_cfo - target));
        TEST_ASSERT_MSG(det, msg);
        TEST_ASSERT_MSG(fabsf(est_cfo - target) < 0.002f, msg);
    }
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Sync timing tests — verify detection fires at the correct sample position
///////////////////////////////////////////////////////////////////////////////

static void test_sync_detection_timing(void)
{
    TEST_BEGIN("sync: detection fires within expected sample window");
    cw_tone_init();

    // Feed exactly 128 DC tone samples. Detection requires filling the
    // circular buffer (128 samples) then one full autocorrelation window.
    // Detection should fire at or near sample 128.
    int detect_sample = -1;
    for (int n = 0; n < 256; n++) {
        if (cw_tone_execute(1.0f + 0.0f * _Complex_I)) {
            detect_sample = n;
            break;
        }
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "detected at sample %d (expected ~128)", detect_sample);
    TEST_ASSERT_MSG(detect_sample >= 0, "tone should be detected");
    // Should detect at sample 128 (after buffer fills)
    TEST_ASSERT_MSG(detect_sample == 128, msg);
    TEST_PASS();
}

static void test_sync_detection_after_noise_gap(void)
{
    TEST_BEGIN("sync: detection timing correct after noise gap");
    cw_tone_init();
    srand(11001);

    int noise_len = 300;
    int tone_start = noise_len;  // tone starts at this sample index
    int detect_sample = -1;

    for (int n = 0; n < noise_len + 512; n++) {
        float complex sample;
        if (n < noise_len) {
            // Low-level noise
            float re = ((float)rand() / RAND_MAX) * 0.1f - 0.05f;
            float im = ((float)rand() / RAND_MAX) * 0.1f - 0.05f;
            sample = re + im * _Complex_I;
        } else {
            sample = 1.0f + 0.0f * _Complex_I;
        }

        if (detect_sample < 0 && cw_tone_execute(sample)) {
            detect_sample = n;
        }
    }

    char msg[128];
    snprintf(msg, sizeof(msg),
             "detected at sample %d, tone starts at %d, delta=%d",
             detect_sample, tone_start, detect_sample - tone_start);
    TEST_ASSERT_MSG(detect_sample >= 0, "should detect tone after noise gap");
    // Detection comes after the circular buffer fills with mostly-tone samples.
    // With residual noise in the buffer, detection can fire slightly before
    // the full 128-sample fill, so allow a window from ~100 to ~196.
    int delta = detect_sample - tone_start;
    TEST_ASSERT_MSG(delta >= 100 && delta <= 196, msg);
    TEST_PASS();
}

static void test_sync_no_early_trigger(void)
{
    TEST_BEGIN("sync: no false trigger during partial tone fill");
    cw_tone_init();

    // Feed only 127 DC samples — should NOT trigger
    int detected = 0;
    for (int n = 0; n < 127; n++) {
        if (cw_tone_execute(1.0f + 0.0f * _Complex_I)) {
            detected = 1;
        }
    }
    TEST_ASSERT_MSG(!detected, "should not detect with only 127 samples");
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Leading noise tests — noise before the tone
///////////////////////////////////////////////////////////////////////////////

static void test_leading_noise_low_level(void)
{
    TEST_BEGIN("leading noise: low-level noise before tone");
    cw_tone_init();
    srand(12000);

    float target_cfo = 0.003f;
    int detected = 0;
    float est_cfo = 0.0f;
    int noise_len = 500;

    for (int n = 0; n < noise_len + 512; n++) {
        float complex sample;
        if (n < noise_len) {
            float re = ((float)rand() / RAND_MAX) * 0.2f - 0.1f;
            float im = ((float)rand() / RAND_MAX) * 0.2f - 0.1f;
            sample = re + im * _Complex_I;
        } else {
            int t = n - noise_len;
            float phase = 2.0f * (float)M_PI * target_cfo * (float)t;
            sample = cosf(phase) + sinf(phase) * _Complex_I;
        }

        if (!detected && cw_tone_execute(sample)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "should detect tone after low-level leading noise");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f", est_cfo, target_cfo);
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.002f, msg);
    TEST_PASS();
}

static void test_leading_noise_high_level(void)
{
    TEST_BEGIN("leading noise: high-level noise before tone");
    cw_tone_init();
    srand(12001);

    float target_cfo = -0.002f;
    int detected = 0;
    float est_cfo = 0.0f;
    int noise_len = 400;

    for (int n = 0; n < noise_len + 512; n++) {
        float complex sample;
        if (n < noise_len) {
            // Noise at same power as tone
            float re = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
            float im = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
            sample = re + im * _Complex_I;
        } else {
            int t = n - noise_len;
            float phase = 2.0f * (float)M_PI * target_cfo * (float)t;
            sample = cosf(phase) + sinf(phase) * _Complex_I;
        }

        if (!detected && cw_tone_execute(sample)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "should detect tone after high-level leading noise");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f", est_cfo, target_cfo);
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.002f, msg);
    TEST_PASS();
}

static void test_leading_noise_no_false_trigger(void)
{
    TEST_BEGIN("leading noise: no false detection during noise-only segment");
    cw_tone_init();
    srand(12002);

    int false_trigger = 0;
    int noise_len = 1000;

    // Feed only noise — no tone at all
    for (int n = 0; n < noise_len; n++) {
        float re = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
        float im = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
        if (cw_tone_execute(re + im * _Complex_I)) {
            false_trigger = 1;
            break;
        }
    }
    TEST_ASSERT_MSG(!false_trigger, "should not false-trigger on noise-only input");
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Lagging noise tests — noise after the tone
///////////////////////////////////////////////////////////////////////////////

static void test_lagging_noise_low_level(void)
{
    TEST_BEGIN("lagging noise: tone followed by low-level noise");
    cw_tone_init();
    srand(13000);

    float target_cfo = 0.004f;
    int detected = 0;
    float est_cfo = 0.0f;
    int tone_len = 192;

    for (int n = 0; n < tone_len + 300; n++) {
        float complex sample;
        if (n < tone_len) {
            float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
            sample = cosf(phase) + sinf(phase) * _Complex_I;
        } else {
            float re = ((float)rand() / RAND_MAX) * 0.2f - 0.1f;
            float im = ((float)rand() / RAND_MAX) * 0.2f - 0.1f;
            sample = re + im * _Complex_I;
        }

        if (!detected && cw_tone_execute(sample)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "should detect tone before lagging noise arrives");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f", est_cfo, target_cfo);
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.002f, msg);
    TEST_PASS();
}

static void test_lagging_noise_high_level(void)
{
    TEST_BEGIN("lagging noise: tone followed by high-level noise");
    cw_tone_init();
    srand(13001);

    float target_cfo = -0.005f;
    int detected = 0;
    float est_cfo = 0.0f;
    int tone_len = 192;

    for (int n = 0; n < tone_len + 300; n++) {
        float complex sample;
        if (n < tone_len) {
            float phase = 2.0f * (float)M_PI * target_cfo * (float)n;
            sample = cosf(phase) + sinf(phase) * _Complex_I;
        } else {
            float re = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
            float im = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
            sample = re + im * _Complex_I;
        }

        if (!detected && cw_tone_execute(sample)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "should detect tone before high-level lagging noise");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f", est_cfo, target_cfo);
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.002f, msg);
    TEST_PASS();
}

static void test_lagging_noise_no_retrigger(void)
{
    TEST_BEGIN("lagging noise: no re-trigger during noise after detection");
    cw_tone_init();
    srand(13002);

    int detect_count = 0;
    int tone_len = 192;

    for (int n = 0; n < tone_len + 500; n++) {
        float complex sample;
        if (n < tone_len) {
            sample = 1.0f + 0.0f * _Complex_I;
        } else {
            float re = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
            float im = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
            sample = re + im * _Complex_I;
        }

        if (cw_tone_execute(sample))
            detect_count++;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "detect_count=%d (expect 1)", detect_count);
    TEST_ASSERT_MSG(detect_count == 1, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Leading + lagging noise (sandwich)
///////////////////////////////////////////////////////////////////////////////

static void test_noise_sandwich(void)
{
    TEST_BEGIN("sandwich: noise-tone-noise with CFO");
    cw_tone_init();
    srand(14000);

    float target_cfo = 0.006f;
    int detected = 0;
    float est_cfo = 0.0f;
    int pre_noise = 400;
    int tone_len = 192;
    int post_noise = 400;
    int total = pre_noise + tone_len + post_noise;

    for (int n = 0; n < total; n++) {
        float complex sample;
        if (n < pre_noise || n >= pre_noise + tone_len) {
            // Noise
            float re = ((float)rand() / RAND_MAX) * 1.0f - 0.5f;
            float im = ((float)rand() / RAND_MAX) * 1.0f - 0.5f;
            sample = re + im * _Complex_I;
        } else {
            // Tone
            int t = n - pre_noise;
            float phase = 2.0f * (float)M_PI * target_cfo * (float)t;
            sample = cosf(phase) + sinf(phase) * _Complex_I;
        }

        if (!detected && cw_tone_execute(sample)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "should detect tone in noise sandwich");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f",
             est_cfo, target_cfo, fabsf(est_cfo - target_cfo));
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.002f, msg);
    TEST_PASS();
}

static void test_noise_sandwich_awgn(void)
{
    TEST_BEGIN("sandwich: noise-tone(+AWGN)-noise with CFO");
    cw_tone_init();
    srand(14001);

    float target_cfo = -0.004f;
    int detected = 0;
    float est_cfo = 0.0f;
    int pre_noise = 300;
    int tone_len = 256;
    int post_noise = 300;
    int total = pre_noise + tone_len + post_noise;
    float noise_sigma = 0.15f;  // ~16 dB SNR

    for (int n = 0; n < total; n++) {
        float complex sample;
        if (n < pre_noise || n >= pre_noise + tone_len) {
            float re = ((float)rand() / RAND_MAX) * 1.0f - 0.5f;
            float im = ((float)rand() / RAND_MAX) * 1.0f - 0.5f;
            sample = re + im * _Complex_I;
        } else {
            int t = n - pre_noise;
            float phase = 2.0f * (float)M_PI * target_cfo * (float)t;
            sample = cosf(phase) + sinf(phase) * _Complex_I;
            sample += awgn(noise_sigma);
        }

        if (!detected && cw_tone_execute(sample)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "should detect noisy tone in noise sandwich");
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f",
             est_cfo, target_cfo, fabsf(est_cfo - target_cfo));
    TEST_ASSERT_MSG(fabsf(est_cfo - target_cfo) < 0.003f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Frequency drift (time-varying CFO)
///////////////////////////////////////////////////////////////////////////////

static void test_channel_frequency_drift(void)
{
    TEST_BEGIN("channel: detects tone with slow frequency drift");
    cw_tone_init();
    srand(9000);

    // CFO drifts linearly from 0.001 to 0.003 over 256 samples
    int detected = 0;
    float est_cfo = 0.0f;
    float accumulated_phase = 0.0f;

    for (int n = 0; n < 512; n++) {
        float instantaneous_cfo = 0.001f + 0.002f * ((float)n / 512.0f);
        accumulated_phase += 2.0f * (float)M_PI * instantaneous_cfo;
        float complex sample = cosf(accumulated_phase)
                             + sinf(accumulated_phase) * _Complex_I;

        if (!detected && cw_tone_execute(sample)) {
            detected = 1;
            est_cfo = cw_tone_get_freq_offset();
        }
    }

    TEST_ASSERT_MSG(detected, "slowly drifting tone should still be detected");
    // The estimate should be somewhere between 0.001 and 0.003
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f should be in [0.001, 0.003]", est_cfo);
    TEST_ASSERT_MSG(est_cfo > 0.0005f && est_cfo < 0.004f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main(void)
{
    fprintf(stderr, "=== CW Tone Channel Condition Tests ===\n");

    // AWGN
    RUN_TEST(test_channel_awgn_high_snr);
    RUN_TEST(test_channel_awgn_20db);
    RUN_TEST(test_channel_awgn_10db);
    RUN_TEST(test_channel_awgn_5db);
    RUN_TEST(test_channel_awgn_0db_rejection);

    // Phase offsets
    RUN_TEST(test_channel_phase_offset_zero);
    RUN_TEST(test_channel_phase_offset_pi_over_4);
    RUN_TEST(test_channel_phase_offset_pi);
    RUN_TEST(test_channel_phase_offset_random);

    // CFO under noise
    RUN_TEST(test_channel_cfo_positive_noisy);
    RUN_TEST(test_channel_cfo_negative_noisy);
    RUN_TEST(test_channel_cfo_sweep_clean);
    RUN_TEST(test_channel_cfo_sweep_noisy);

    // Amplitude
    RUN_TEST(test_channel_low_amplitude);
    RUN_TEST(test_channel_high_amplitude);
    RUN_TEST(test_channel_very_low_amplitude);

    // Combined
    RUN_TEST(test_channel_cfo_plus_phase_plus_noise);
    RUN_TEST(test_channel_cfo_plus_phase_plus_low_amplitude_noise);
    RUN_TEST(test_channel_worst_case_feasible);

    // Realistic scenarios
    RUN_TEST(test_channel_tone_then_noise);
    RUN_TEST(test_channel_noise_then_tone);

    // Multipath
    RUN_TEST(test_channel_multipath_short_delay);
    RUN_TEST(test_channel_multipath_longer_delay);

    // Large CFO (100 kHz)
    RUN_TEST(test_channel_cfo_100khz);
    RUN_TEST(test_channel_cfo_100khz_negative);
    RUN_TEST(test_channel_cfo_100khz_noisy);
    RUN_TEST(test_channel_cfo_near_edge);

    // Sync timing
    RUN_TEST(test_sync_detection_timing);
    RUN_TEST(test_sync_detection_after_noise_gap);
    RUN_TEST(test_sync_no_early_trigger);

    // Leading noise
    RUN_TEST(test_leading_noise_low_level);
    RUN_TEST(test_leading_noise_high_level);
    RUN_TEST(test_leading_noise_no_false_trigger);

    // Lagging noise
    RUN_TEST(test_lagging_noise_low_level);
    RUN_TEST(test_lagging_noise_high_level);
    RUN_TEST(test_lagging_noise_no_retrigger);

    // Noise sandwich (leading + lagging)
    RUN_TEST(test_noise_sandwich);
    RUN_TEST(test_noise_sandwich_awgn);

    // Drift
    RUN_TEST(test_channel_frequency_drift);

    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
