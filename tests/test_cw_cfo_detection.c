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

// Tests for coarse and fine CW CFO detection stages.
//
// The CW tone estimator uses a two-stage approach:
//   Stage 1 (coarse): lag-4 autocorrelation, capture range ±1/(2*4) = ±0.125 cyc/samp (±175 kHz)
//   Stage 2 (fine):   lag-64 autocorrelation on de-rotated signal, refines residual CFO
//
// These tests verify that:
//   - Small CFOs (within fine-only range) are estimated accurately
//   - Large CFOs (requiring coarse correction) are detected and refined
//   - The fine stage improves accuracy beyond what coarse-only would provide
//   - Edge cases at stage boundaries are handled correctly

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

#define SAMPLE_RATE 1400000.0f

///////////////////////////////////////////////////////////////////////////////
// Helpers
///////////////////////////////////////////////////////////////////////////////

// Generate CW tone with a given CFO (cycles/sample) and feed to detector.
// Returns 1 if detected, 0 otherwise.  est_cfo is set to the estimated CFO.
static int feed_cfo_tone(float cfo_cyc_per_samp, float *est_cfo)
{
    cw_tone_init();
    int detected = 0;
    for (int n = 0; n < 512; n++) {
        float phase = 2.0f * (float)M_PI * cfo_cyc_per_samp * (float)n;
        float complex sample = cosf(phase) + sinf(phase) * _Complex_I;
        if (cw_tone_execute(sample)) {
            detected = 1;
            break;
        }
    }
    if (est_cfo)
        *est_cfo = cw_tone_get_freq_offset();
    return detected;
}

// Convert Hz offset to cycles/sample at 1.4 MHz sample rate
static float hz_to_cyc(float hz)
{
    return hz / SAMPLE_RATE;
}

///////////////////////////////////////////////////////////////////////////////
// Fine-stage tests: CFO within lag-64 unambiguous range (±0.0078 cyc/samp)
// These offsets are small enough that the coarse stage returns ~0,
// so accuracy depends entirely on the fine (lag-64) correlator.
///////////////////////////////////////////////////////////////////////////////

static void test_fine_1khz(void)
{
    TEST_BEGIN("fine: +1 kHz CFO (0.000714 cyc/samp)");
    float target = hz_to_cyc(1000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.0002f, msg);
    TEST_PASS();
}

static void test_fine_neg_2khz(void)
{
    TEST_BEGIN("fine: -2 kHz CFO (-0.00143 cyc/samp)");
    float target = hz_to_cyc(-2000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.0002f, msg);
    TEST_PASS();
}

static void test_fine_5khz(void)
{
    TEST_BEGIN("fine: +5 kHz CFO (0.00357 cyc/samp)");
    float target = hz_to_cyc(5000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.0002f, msg);
    TEST_PASS();
}

static void test_fine_10khz(void)
{
    TEST_BEGIN("fine: +10 kHz CFO (0.00714 cyc/samp, near fine-only limit)");
    float target = hz_to_cyc(10000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.0005f, msg);
    TEST_PASS();
}

static void test_fine_neg_500hz(void)
{
    TEST_BEGIN("fine: -500 Hz CFO (very small offset)");
    float target = hz_to_cyc(-500.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.0002f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Coarse-stage tests: CFO beyond fine-only range but within coarse capture
// range (±0.125 cyc/samp = ±175 kHz).  These require the coarse (lag-4)
// stage to bring the residual into the fine stage's range.
///////////////////////////////////////////////////////////////////////////////

static void test_coarse_20khz(void)
{
    TEST_BEGIN("coarse+fine: +20 kHz CFO (0.0143 cyc/samp)");
    float target = hz_to_cyc(20000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.001f, msg);
    TEST_PASS();
}

static void test_coarse_neg_30khz(void)
{
    TEST_BEGIN("coarse+fine: -30 kHz CFO (-0.0214 cyc/samp)");
    float target = hz_to_cyc(-30000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.001f, msg);
    TEST_PASS();
}

static void test_coarse_75khz(void)
{
    TEST_BEGIN("coarse+fine: +75 kHz CFO (0.0536 cyc/samp)");
    float target = hz_to_cyc(75000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.001f, msg);
    TEST_PASS();
}

static void test_coarse_neg_120khz(void)
{
    TEST_BEGIN("coarse+fine: -120 kHz CFO (-0.0857 cyc/samp)");
    float target = hz_to_cyc(-120000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.001f, msg);
    TEST_PASS();
}

static void test_coarse_150khz(void)
{
    TEST_BEGIN("coarse+fine: +150 kHz CFO (0.107 cyc/samp, near coarse limit)");
    float target = hz_to_cyc(150000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.002f, msg);
    TEST_PASS();
}

static void test_coarse_neg_170khz(void)
{
    TEST_BEGIN("coarse+fine: -170 kHz CFO (-0.121 cyc/samp, very near coarse limit)");
    float target = hz_to_cyc(-170000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.002f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Fine-stage refinement verification: prove that the two-stage estimator
// achieves better accuracy than a coarse-only (lag-4) estimator would.
//
// A lag-4 estimator has resolution limited by quantization of arg() over
// only 4 samples of phase rotation.  The fine stage should push error
// well below what lag-4 alone achieves.
///////////////////////////////////////////////////////////////////////////////

static void test_fine_refines_coarse(void)
{
    TEST_BEGIN("refinement: fine stage improves accuracy over coarse-only");

    // Test several offsets that require coarse correction, and verify
    // the combined estimate is much more accurate than coarse-only.
    // Coarse-only (lag-4) resolution: ~0.003 cyc/samp typical error
    // Combined should be < 0.001 cyc/samp.
    float offsets_hz[] = {25000.0f, 50000.0f, 80000.0f, -40000.0f, -90000.0f};
    int n_offsets = sizeof(offsets_hz) / sizeof(offsets_hz[0]);
    float max_err = 0.0f;

    for (int i = 0; i < n_offsets; i++) {
        float target = hz_to_cyc(offsets_hz[i]);
        float est;
        int det = feed_cfo_tone(target, &est);
        if (!det) {
            char msg[128];
            snprintf(msg, sizeof(msg), "failed to detect at %.0f Hz", offsets_hz[i]);
            TEST_ASSERT_MSG(0, msg);
        }
        float err = fabsf(est - target);
        if (err > max_err)
            max_err = err;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "max error across offsets: %.6f cyc/samp (want < 0.001)", max_err);
    TEST_ASSERT_MSG(max_err < 0.001f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Boundary tests: offsets right at the transition between fine-only and
// coarse+fine operation (~10.9 kHz = 0.0078 cyc/samp)
///////////////////////////////////////////////////////////////////////////////

static void test_boundary_just_below(void)
{
    TEST_BEGIN("boundary: +10 kHz (just below fine-only limit)");
    float target = hz_to_cyc(10000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.0005f, msg);
    TEST_PASS();
}

static void test_boundary_just_above(void)
{
    TEST_BEGIN("boundary: +12 kHz (just above fine-only limit)");
    float target = hz_to_cyc(12000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.001f, msg);
    TEST_PASS();
}

static void test_boundary_neg_just_below(void)
{
    TEST_BEGIN("boundary: -10 kHz (just below fine-only limit, negative)");
    float target = hz_to_cyc(-10000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.0005f, msg);
    TEST_PASS();
}

static void test_boundary_neg_just_above(void)
{
    TEST_BEGIN("boundary: -12 kHz (just above fine-only limit, negative)");
    float target = hz_to_cyc(-12000.0f);
    float est;
    int det = feed_cfo_tone(target, &est);
    TEST_ASSERT_MSG(det, "should detect");
    float err = fabsf(est - target);
    char msg[128];
    snprintf(msg, sizeof(msg), "est=%.6f target=%.6f err=%.6f", est, target, err);
    TEST_ASSERT_MSG(err < 0.001f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Sweep test: verify accuracy across a range of CFOs from -150 kHz to +150 kHz
///////////////////////////////////////////////////////////////////////////////

static void test_cfo_sweep(void)
{
    TEST_BEGIN("sweep: accuracy across -150 to +150 kHz in 10 kHz steps");
    float max_err = 0.0f;
    float worst_hz = 0.0f;

    for (float hz = -150000.0f; hz <= 150000.0f; hz += 10000.0f) {
        if (fabsf(hz) < 1.0f) continue;  // skip 0 Hz
        float target = hz_to_cyc(hz);
        float est;
        int det = feed_cfo_tone(target, &est);
        if (!det) {
            char msg[128];
            snprintf(msg, sizeof(msg), "failed to detect at %.0f Hz", hz);
            TEST_ASSERT_MSG(0, msg);
        }
        float err = fabsf(est - target);
        if (err > max_err) {
            max_err = err;
            worst_hz = hz;
        }
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "worst error: %.6f cyc/samp at %.0f Hz (want < 0.002)",
             max_err, worst_hz);
    TEST_ASSERT_MSG(max_err < 0.002f, msg);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Symmetry test: positive and negative CFOs should give symmetric estimates
///////////////////////////////////////////////////////////////////////////////

static void test_cfo_symmetry(void)
{
    TEST_BEGIN("symmetry: +/- CFO estimates are symmetric");
    float offsets_hz[] = {5000.0f, 20000.0f, 50000.0f, 100000.0f};
    int n = sizeof(offsets_hz) / sizeof(offsets_hz[0]);

    for (int i = 0; i < n; i++) {
        float est_pos, est_neg;
        feed_cfo_tone(hz_to_cyc(offsets_hz[i]), &est_pos);
        feed_cfo_tone(hz_to_cyc(-offsets_hz[i]), &est_neg);

        float asymmetry = fabsf(est_pos + est_neg);
        char msg[128];
        snprintf(msg, sizeof(msg), "asymmetry at %.0f Hz: %.6f (want < 0.001)",
                 offsets_hz[i], asymmetry);
        TEST_ASSERT_MSG(asymmetry < 0.001f, msg);
    }
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main(void)
{
    fprintf(stderr, "=== CW Coarse & Fine CFO Detection Tests ===\n");

    // Fine-stage (small CFO) tests
    RUN_TEST(test_fine_1khz);
    RUN_TEST(test_fine_neg_2khz);
    RUN_TEST(test_fine_5khz);
    RUN_TEST(test_fine_10khz);
    RUN_TEST(test_fine_neg_500hz);

    // Coarse+fine (large CFO) tests
    RUN_TEST(test_coarse_20khz);
    RUN_TEST(test_coarse_neg_30khz);
    RUN_TEST(test_coarse_75khz);
    RUN_TEST(test_coarse_neg_120khz);
    RUN_TEST(test_coarse_150khz);
    RUN_TEST(test_coarse_neg_170khz);

    // Fine-stage refinement verification
    RUN_TEST(test_fine_refines_coarse);

    // Boundary tests
    RUN_TEST(test_boundary_just_below);
    RUN_TEST(test_boundary_just_above);
    RUN_TEST(test_boundary_neg_just_below);
    RUN_TEST(test_boundary_neg_just_above);

    // Sweep and symmetry
    RUN_TEST(test_cfo_sweep);
    RUN_TEST(test_cfo_symmetry);

    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
