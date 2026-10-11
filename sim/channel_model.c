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

// RF channel impairment building blocks for the Charon simulator (rfchan).

#include <math.h>
#include <string.h>

#include "channel_model.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
static uint64_t splitmix64(uint64_t *x) {
  uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

void cm_rng_seed(cm_rng *r, uint64_t seed) {
  uint64_t a = splitmix64(&seed), b = splitmix64(&seed);
  r->s[0] = (uint32_t)a; r->s[1] = (uint32_t)(a >> 32);
  r->s[2] = (uint32_t)b; r->s[3] = (uint32_t)(b >> 32) | 1;
  r->has_spare = 0;
}

static inline uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

static uint32_t cm_rng_next(cm_rng *r) {
  uint32_t res = r->s[0] + r->s[3];
  uint32_t t = r->s[1] << 9;
  r->s[2] ^= r->s[0]; r->s[3] ^= r->s[1];
  r->s[1] ^= r->s[2]; r->s[0] ^= r->s[3];
  r->s[2] ^= t;
  r->s[3] = rotl(r->s[3], 11);
  return res;
}

float cm_rng_uniform(cm_rng *r) {
  return (cm_rng_next(r) >> 8) * (1.0f / 16777216.0f);
}

float cm_rng_gauss(cm_rng *r) {
  if (r->has_spare) { r->has_spare = 0; return r->spare; }
  float u, v, s;
  do {
    u = 2.0f * cm_rng_uniform(r) - 1.0f;
    v = 2.0f * cm_rng_uniform(r) - 1.0f;
    s = u * u + v * v;
  } while (s >= 1.0f || s == 0.0f);
  float m = sqrtf(-2.0f * logf(s) / s);
  r->spare = v * m;
  r->has_spare = 1;
  return u * m;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
double cm_power(const cf_t *x, int n) {
  double acc = 0.0;
  for (int i = 0; i < n; i++) acc += crealf(x[i]) * crealf(x[i]) + cimagf(x[i]) * cimagf(x[i]);
  return n > 0 ? acc / n : 0.0;
}

double cm_db(double lin) { return lin > 1e-30 ? 10.0 * log10(lin) : -300.0; }
double cm_from_db(double db) { return pow(10.0, db / 10.0); }

void cm_awgn(cf_t *x, int n, double power, cm_rng *r) {
  if (power <= 0.0) return;
  float s = (float)sqrt(power / 2.0);
  for (int i = 0; i < n; i++)
    x[i] += s * (cm_rng_gauss(r) + _Complex_I * cm_rng_gauss(r));
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void cm_nco_set_freq(cm_nco *o, double freq_hz, double fs_hz) {
  o->dphi = 2.0 * M_PI * freq_hz / fs_hz;
}

void cm_nco_mix(cm_nco *o, cf_t *x, int n) {
  if (o->dphi == 0.0 && o->phase == 0.0) return;
  for (int i = 0; i < n; i++) {
    x[i] *= (float)cos(o->phase) + _Complex_I * (float)sin(o->phase);
    o->phase += o->dphi;
  }
  o->phase = fmod(o->phase, 2.0 * M_PI);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void cm_phase_noise_init(cm_phase_noise *p, double linewidth_hz, double fs_hz) {
  p->phase = 0.0;
  p->sigma = linewidth_hz > 0.0 ? sqrt(2.0 * M_PI * linewidth_hz / fs_hz) : 0.0;
}

void cm_phase_noise_run(cm_phase_noise *p, double *ph, int n, cm_rng *r) {
  for (int i = 0; i < n; i++) {
    if (p->sigma > 0.0) p->phase += p->sigma * cm_rng_gauss(r);
    ph[i] = p->phase;
  }
  p->phase = fmod(p->phase, 2.0 * M_PI);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
// I = Re{x}, Q = g*(sin(phi)*Re{x} + cos(phi)*Im{x})  (Q branch gain/phase error)
//   => y = mu*x + nu*conj(x),  mu = (1 + g e^{-j phi})/2,  nu = (1 - g e^{j phi})/2
void cm_iq_init(cm_iq *q, double gain_db, double phase_deg) {
  double g = pow(10.0, gain_db / 20.0);
  double ph = phase_deg * M_PI / 180.0;
  q->mu = (float)((1.0 + g * cos(ph)) / 2.0) - _Complex_I * (float)(g * sin(ph) / 2.0);
  q->nu = (float)((1.0 - g * cos(ph)) / 2.0) - _Complex_I * (float)(g * sin(ph) / 2.0);
}

void cm_iq_apply(const cm_iq *q, cf_t *x, int n) {
  if (q->nu == 0.0f && q->mu == 1.0f) return;
  for (int i = 0; i < n; i++) x[i] = q->mu * x[i] + q->nu * conjf(x[i]);
}

double cm_iq_irr_db(const cm_iq *q) {
  double m = cabsf(q->mu), v = cabsf(q->nu);
  if (v <= 0.0) return INFINITY;
  return 20.0 * log10(m / v);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void cm_fir_init(cm_fir *f, const cf_t *taps, int ntaps) {
  if (ntaps > CM_FIR_MAX_TAPS) ntaps = CM_FIR_MAX_TAPS;
  if (ntaps < 1) ntaps = 1;
  f->ntaps = ntaps;
  for (int i = 0; i < ntaps; i++) f->taps[i] = taps ? taps[i] : (i == 0 ? 1.0f : 0.0f);
  cm_fir_reset(f);
}

void cm_fir_reset(cm_fir *f) {
  memset(f->hist, 0, sizeof(f->hist));
  f->pos = 0;
}

void cm_fir_run(cm_fir *f, cf_t *x, int n) {
  if (f->ntaps == 1) {
    if (f->taps[0] != 1.0f)
      for (int i = 0; i < n; i++) x[i] *= f->taps[0];
    return;
  }
  for (int i = 0; i < n; i++) {
    f->hist[f->pos] = x[i];
    cf_t acc = 0.0f;
    int k = f->pos;
    for (int t = 0; t < f->ntaps; t++) {
      acc += f->taps[t] * f->hist[k];
      k = (k == 0) ? f->ntaps - 1 : k - 1;
    }
    x[i] = acc;
    f->pos = (f->pos + 1 == f->ntaps) ? 0 : f->pos + 1;
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
// Windowed-sinc polyphase table shared by all resamplers.
//   table[p][j] = h(mu_p - (j - CM_RS_TAPS/2 + 1)),  mu_p = p / CM_RS_PHASES
// Output at fractional input time t uses inputs floor(t)-N/2+1 .. floor(t)+N/2.
static float rs_table[CM_RS_PHASES + 1][CM_RS_TAPS];
static int   rs_table_ready = 0;

static double bessel_i0(double x) {
  double s = 1.0, t = 1.0;
  for (int k = 1; k < 40; k++) {
    t *= (x / (2.0 * k)) * (x / (2.0 * k));
    s += t;
  }
  return s;
}

static void rs_build_table(void) {
  const double beta = 7.0;
  const double half = CM_RS_TAPS / 2.0;
  for (int p = 0; p <= CM_RS_PHASES; p++) {
    double mu = (double)p / CM_RS_PHASES;
    double sum = 0.0;
    double h[CM_RS_TAPS];
    for (int j = 0; j < CM_RS_TAPS; j++) {
      double tau = mu - (double)(j - CM_RS_TAPS / 2 + 1);
      double s = (fabs(tau) < 1e-12) ? 1.0 : sin(M_PI * tau) / (M_PI * tau);
      double r = tau / half;
      double w = (fabs(r) <= 1.0) ? bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta) : 0.0;
      h[j] = s * w;
      sum += h[j];
    }
    for (int j = 0; j < CM_RS_TAPS; j++) rs_table[p][j] = (float)(h[j] / sum);
  }
  rs_table_ready = 1;
}

// State: kin = number of inputs consumed (as double offset), tout = time of
// next output in the same input-sample index units.  Both are rebased
// whenever they get large so the double keeps sub-1e-9 resolution.
void cm_resamp_init(cm_resamp *rs, double ratio, double frac_delay) {
  if (!rs_table_ready) rs_build_table();
  rs->ratio = ratio;
  cm_resamp_reset(rs, frac_delay);
}

void cm_resamp_reset(cm_resamp *rs, double frac_delay) {
  memset(rs->hist, 0, sizeof(rs->hist));
  rs->hpos = 0;
  // t = (time of next output) - (index of next input).  Output time
  // is delayed by frac_delay input samples.
  rs->t = -frac_delay;
}

void cm_resamp_set_ratio(cm_resamp *rs, double ratio) {
  rs->ratio = ratio;
}

double cm_resamp_delay(void) {
  return CM_RS_TAPS / 2;
}

int cm_resamp_run(cm_resamp *rs, const cf_t *x, int nx, cf_t *y, int max_y) {
  int ny = 0;
  const int N = CM_RS_TAPS;
  const double step = 1.0 / rs->ratio;
  // hist ring holds the last N inputs; hist[hpos-1] (mod N) is the newest.
  // Doubled storage (hist[i] == hist[i+N]) gives contiguous reads.
  for (int i = 0; i < nx; i++) {
    rs->hist[rs->hpos] = x[i];
    rs->hist[rs->hpos + N] = x[i];
    rs->hpos = (rs->hpos + 1) % N;
    rs->t -= 1.0;   // one more input is now available
    // Newest input sits at relative index -1 from "next input" (t origin).
    // Output at relative time t needs inputs up to floor(t) + N/2 <= -1.
    while (floor(rs->t) + N / 2 <= -1.0 && ny < max_y) {
      double fl = floor(rs->t);
      double mu = rs->t - fl;
      double pp = mu * CM_RS_PHASES;
      int p = (int)pp;
      float a = (float)(pp - p);
      // first needed input relative index: fl - N/2 + 1  (>= -N)
      int rel0 = (int)fl - N / 2 + 1;           // in [-N, -1]
      int start = rs->hpos + rel0 + N;          // ring index of rel0 (+N keeps it >= 0)
      if (start >= N) start -= N;
      const cf_t *h = &rs->hist[start];
      const float *c0 = rs_table[p], *c1 = rs_table[p + 1];
      cf_t acc = 0.0f;
      for (int j = 0; j < N; j++) acc += h[j] * (c0[j] + a * (c1[j] - c0[j]));
      y[ny++] = acc;
      rs->t += step;
    }
  }
  return ny;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int cm_adc(cf_t *x, int n, int bits) {
  const float fs = (float)(1 << (bits - 1));
  const float lo = -fs, hi = fs - 1.0f;
  int clipped = 0;
  for (int i = 0; i < n; i++) {
    float re = roundf(crealf(x[i]) * fs);
    float im = roundf(cimagf(x[i]) * fs);
    if (re > hi) { re = hi; clipped++; } else if (re < lo) { re = lo; clipped++; }
    if (im > hi) { im = hi; clipped++; } else if (im < lo) { im = lo; clipped++; }
    x[i] = (re + _Complex_I * im) / fs;
  }
  return clipped;
}
