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

// Unit tests for sim/channel_model.c — each impairment block must inject
// exactly the impairment it claims, measured independently here.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include "test_harness.h"
#include "../sim/channel_model.c"

#define FS 1.4e6
#define NBIG 200000

static cf_t buf[NBIG];
static cf_t out[NBIG + 64];

// Mean frequency (Hz) of a tone from the average phase increment.
static double measure_freq(const cf_t *x, int n, double fs) {
  double complex acc = 0.0;
  for (int i = 1; i < n; i++) acc += (double complex)x[i] * conj((double complex)x[i - 1]);
  return carg(acc) / (2.0 * M_PI) * fs;
}

// Complex amplitude of x at normalized frequency f (cycles/sample).
static cf_t tone_amp(const cf_t *x, int n, double f) {
  double complex acc = 0.0;
  for (int i = 0; i < n; i++) acc += x[i] * cexp(-2.0 * M_PI * I * f * i);
  return (cf_t)(acc / n);
}

static void make_tone(cf_t *x, int n, double f, double amp) {
  for (int i = 0; i < n; i++) x[i] = (float)amp * cexpf(2.0f * (float)M_PI * _Complex_I * (float)fmod(f * i, 1.0));
}

///////////////////////////////////////////////////////////////////////////////////////
static void test_rng_gauss_stats(void) {
  TEST_BEGIN("rng: gaussian mean 0, variance 1, deterministic");
  cm_rng r, r2;
  cm_rng_seed(&r, 42);
  cm_rng_seed(&r2, 42);
  double s = 0, s2 = 0;
  for (int i = 0; i < NBIG; i++) {
    float g = cm_rng_gauss(&r);
    TEST_ASSERT(g == cm_rng_gauss(&r2));
    s += g; s2 += g * g;
  }
  s /= NBIG; s2 = s2 / NBIG - s * s;
  TEST_ASSERT(fabs(s) < 0.01);
  TEST_ASSERT(fabs(s2 - 1.0) < 0.02);
  TEST_PASS();
}

static void test_awgn_power(void) {
  TEST_BEGIN("awgn: added noise power matches request (-40 dB)");
  cm_rng r; cm_rng_seed(&r, 1);
  memset(buf, 0, sizeof(buf));
  cm_awgn(buf, NBIG, 1e-4, &r);
  double p = cm_power(buf, NBIG);
  TEST_ASSERT(fabs(cm_db(p) - (-40.0)) < 0.1);
  TEST_PASS();
}

static void test_nco_cfo(void) {
  TEST_BEGIN("nco: CFO of +1234.5 Hz shifts a tone by exactly that");
  make_tone(buf, NBIG, 0.0, 1.0);
  cm_nco o = {0};
  cm_nco_set_freq(&o, 1234.5, FS);
  cm_nco_mix(&o, buf, NBIG);
  double f = measure_freq(buf, NBIG, FS);
  TEST_ASSERT(fabs(f - 1234.5) < 0.01);

  make_tone(buf, NBIG, 0.1, 1.0);
  cm_nco o2 = {0};
  cm_nco_set_freq(&o2, -3000.0, FS);
  cm_nco_mix(&o2, buf, NBIG);
  f = measure_freq(buf, NBIG, FS);
  TEST_ASSERT(fabs(f - (0.1 * FS - 3000.0)) < 0.01);
  TEST_PASS();
}

static void test_phase_noise_variance(void) {
  TEST_BEGIN("phase noise: per-sample phase step variance = 2*pi*lw/fs");
  cm_rng r; cm_rng_seed(&r, 7);
  cm_phase_noise p;
  cm_phase_noise_init(&p, 100.0, FS);
  static double ph[NBIG];
  cm_phase_noise_run(&p, ph, NBIG, &r);
  double s2 = 0;
  for (int i = 1; i < NBIG; i++) { double d = ph[i] - ph[i - 1]; s2 += d * d; }
  s2 /= (NBIG - 1);
  double expect = 2.0 * M_PI * 100.0 / FS;
  TEST_ASSERT(fabs(s2 / expect - 1.0) < 0.02);
  TEST_PASS();
}

static void test_iq_imbalance_image(void) {
  TEST_BEGIN("iq: image of a tone matches predicted IRR (1 dB, 5 deg)");
  cm_iq q;
  cm_iq_init(&q, 1.0, 5.0);
  double irr = cm_iq_irr_db(&q);
  // Analytic IRR for amplitude ratio g and phase phi:
  //   (1 + 2g cos(phi) + g^2) / (1 - 2g cos(phi) + g^2)
  double g = pow(10.0, 1.0 / 20.0), ph = 5.0 * M_PI / 180.0;
  double irr_ref = 10.0 * log10((1 + 2 * g * cos(ph) + g * g) / (1 - 2 * g * cos(ph) + g * g));
  TEST_ASSERT(fabs(irr - irr_ref) < 0.01);

  const int n = 14000;
  make_tone(buf, n, 0.1, 1.0);
  cm_iq_apply(&q, buf, n);
  double pw = cabsf(tone_amp(buf, n, 0.1));
  double im = cabsf(tone_amp(buf, n, -0.1));
  double meas = 20.0 * log10(pw / im);
  TEST_ASSERT(fabs(meas - irr) < 0.05);
  TEST_ASSERT(irr > 20.0 && irr < 30.0);   // sanity: ~24.6 dB

  cm_iq ideal;
  cm_iq_init(&ideal, 0.0, 0.0);
  TEST_ASSERT(isinf(cm_iq_irr_db(&ideal)));
  TEST_PASS();
}

static void test_fir_impulse(void) {
  TEST_BEGIN("fir: impulse response equals multipath taps");
  cf_t taps[3] = {1.0f, 0.0f, 0.3f - 0.2f * _Complex_I};
  cm_fir f;
  cm_fir_init(&f, taps, 3);
  cf_t x[8] = {1.0f};
  cm_fir_run(&f, x, 8);
  TEST_ASSERT(cabsf(x[0] - taps[0]) < 1e-6);
  TEST_ASSERT(cabsf(x[1]) < 1e-6);
  TEST_ASSERT(cabsf(x[2] - taps[2]) < 1e-6);
  for (int i = 3; i < 8; i++) TEST_ASSERT(cabsf(x[i]) < 1e-6);
  TEST_PASS();
}

static void test_resamp_identity(void) {
  TEST_BEGIN("resamp: ratio 1, no delay is a pure pass-through");
  cm_rng r; cm_rng_seed(&r, 3);
  const int n = 5000;
  for (int i = 0; i < n; i++) buf[i] = cm_rng_gauss(&r) + _Complex_I * cm_rng_gauss(&r);
  cm_resamp rs;
  cm_resamp_init(&rs, 1.0, 0.0);
  int ny = cm_resamp_run(&rs, buf, n, out, n + 64);
  int lat = (int)cm_resamp_delay();
  TEST_ASSERT(ny == n - lat);
  for (int i = 0; i < ny; i++) TEST_ASSERT(cabsf(out[i] - buf[i]) < 1e-4);
  TEST_PASS();
}

static void test_resamp_sfo(void) {
  TEST_BEGIN("resamp: +50 ppm SFO scales tone freq/length, SNR > 50 dB");
  const double ppm = 50.0, ratio = 1.0 + ppm * 1e-6, f = 0.35;
  const int n = NBIG;
  make_tone(buf, n, f, 0.5);
  cm_resamp rs;
  cm_resamp_init(&rs, ratio, 0.0);
  int ny = cm_resamp_run(&rs, buf, n, out, NBIG + 64);
  int expect = (int)floor((n - cm_resamp_delay()) * ratio);
  TEST_ASSERT(abs(ny - expect) <= 1);
  // Output sample m sits at input time m/ratio -> tone at f/ratio.
  double err = 0, sig = 0;
  for (int m = 0; m < ny; m++) {
    cf_t ref = 0.5f * cexpf(2.0f * (float)M_PI * _Complex_I * (float)fmod(f * m / ratio, 1.0));
    err += cabsf(out[m] - ref) * cabsf(out[m] - ref);
    sig += 0.25;
  }
  double snr = 10.0 * log10(sig / err);
  TEST_ASSERT_MSG(snr > 50.0, "resampled tone SNR too low");
  TEST_PASS();
}

static void test_resamp_frac_delay(void) {
  TEST_BEGIN("resamp: 0.37-sample fractional delay shifts tone phase");
  const double f = 0.2, d = 0.37;
  const int n = 20000;
  make_tone(buf, n, f, 1.0);
  cm_resamp rs;
  cm_resamp_init(&rs, 1.0, d);
  int ny = cm_resamp_run(&rs, buf, n, out, n + 64);
  cf_t a_in = tone_amp(buf, ny, f);
  cf_t a_out = tone_amp(out, ny, f);
  double dphi = cargf(a_out * conjf(a_in));
  double expect = -2.0 * M_PI * f * d;
  TEST_ASSERT(fabs(dphi - expect) < 1e-3);
  TEST_ASSERT(fabs(cabsf(a_out) - 1.0) < 0.01);
  TEST_PASS();
}

static void test_adc_quantize_clip(void) {
  TEST_BEGIN("adc: 12-bit quantization noise and clipping");
  cm_rng r; cm_rng_seed(&r, 9);
  const int n = 100000;
  for (int i = 0; i < n; i++) buf[i] = 0.1f * (cm_rng_gauss(&r) + _Complex_I * cm_rng_gauss(&r));
  memcpy(out, buf, n * sizeof(cf_t));
  int clipped = cm_adc(out, n, 12);
  TEST_ASSERT(clipped == 0);
  double e = 0;
  for (int i = 0; i < n; i++) e += cabsf(out[i] - buf[i]) * cabsf(out[i] - buf[i]);
  e /= n;
  double lsb = 1.0 / 2048.0;
  double expect = 2.0 * lsb * lsb / 12.0;   // uniform error on I and Q
  TEST_ASSERT(fabs(e / expect - 1.0) < 0.05);

  cf_t big[2] = {2.0f + 0.0f * _Complex_I, -3.0f - 3.0f * _Complex_I};
  clipped = cm_adc(big, 2, 12);
  TEST_ASSERT(clipped == 3);
  TEST_ASSERT(crealf(big[0]) == 2047.0f / 2048.0f);
  TEST_ASSERT(crealf(big[1]) == -1.0f && cimagf(big[1]) == -1.0f);
  TEST_PASS();
}

int main(void) {
  fprintf(stderr, "Channel model tests\n");
  RUN_TEST(test_rng_gauss_stats);
  RUN_TEST(test_awgn_power);
  RUN_TEST(test_nco_cfo);
  RUN_TEST(test_phase_noise_variance);
  RUN_TEST(test_iq_imbalance_image);
  RUN_TEST(test_fir_impulse);
  RUN_TEST(test_resamp_identity);
  RUN_TEST(test_resamp_sfo);
  RUN_TEST(test_resamp_frac_delay);
  RUN_TEST(test_adc_quantize_clip);
  TEST_SUMMARY();
  return TEST_EXIT_CODE();
}
