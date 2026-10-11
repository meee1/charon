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
// Pure DSP, no dependencies beyond libm, so they can be unit-tested directly.

#ifndef CHANNEL_MODEL_H
#define CHANNEL_MODEL_H

#include <complex.h>
#include <stdint.h>

typedef float complex cf_t;

///////////////////////////////////////////////////////////////////////////////////////
// Deterministic RNG (xoshiro128+ seeded via splitmix64) with Gaussian output.
typedef struct {
  uint32_t s[4];
  int   has_spare;
  float spare;
} cm_rng;

void  cm_rng_seed(cm_rng *r, uint64_t seed);
float cm_rng_uniform(cm_rng *r);            // [0,1)
float cm_rng_gauss(cm_rng *r);              // N(0,1)

///////////////////////////////////////////////////////////////////////////////////////
// Power helpers
double cm_power(const cf_t *x, int n);                 // mean |x|^2
double cm_db(double lin);                              // 10*log10, floored at -300
double cm_from_db(double db);                          // 10^(db/10)

// Add complex white Gaussian noise of the given mean power (|n|^2).
void cm_awgn(cf_t *x, int n, double power, cm_rng *r);

///////////////////////////////////////////////////////////////////////////////////////
// Mixer / NCO with double-precision phase (CFO).
typedef struct {
  double phase;   // radians
  double dphi;    // radians per sample
} cm_nco;

void cm_nco_set_freq(cm_nco *o, double freq_hz, double fs_hz);
void cm_nco_mix(cm_nco *o, cf_t *x, int n);

///////////////////////////////////////////////////////////////////////////////////////
// Wiener (random-walk) phase noise for one oscillator.  Linewidth is the
// 3 dB Lorentzian linewidth; per-sample phase variance = 2*pi*lw/fs.
typedef struct {
  double phase;
  double sigma;   // per-sample phase step std-dev
} cm_phase_noise;

void cm_phase_noise_init(cm_phase_noise *p, double linewidth_hz, double fs_hz);
// Writes n phase samples (radians) into ph.
void cm_phase_noise_run(cm_phase_noise *p, double *ph, int n, cm_rng *r);

///////////////////////////////////////////////////////////////////////////////////////
// IQ imbalance:  y = mu*x + nu*conj(x)
// gain_db: I/Q amplitude mismatch, phase_deg: I/Q phase skew.
typedef struct {
  cf_t mu;
  cf_t nu;
} cm_iq;

void   cm_iq_init(cm_iq *q, double gain_db, double phase_deg);
void   cm_iq_apply(const cm_iq *q, cf_t *x, int n);
// Image rejection ratio |mu|^2/|nu|^2 in dB (inf for a perfect mixer).
double cm_iq_irr_db(const cm_iq *q);

///////////////////////////////////////////////////////////////////////////////////////
// Static FIR (multipath).  taps[0] is the direct path.
#define CM_FIR_MAX_TAPS 32
typedef struct {
  int  ntaps;
  cf_t taps[CM_FIR_MAX_TAPS];
  cf_t hist[CM_FIR_MAX_TAPS];
  int  pos;
} cm_fir;

void cm_fir_init(cm_fir *f, const cf_t *taps, int ntaps);
void cm_fir_reset(cm_fir *f);
void cm_fir_run(cm_fir *f, cf_t *x, int n);       // in place

///////////////////////////////////////////////////////////////////////////////////////
// Arbitrary-ratio resampler (SFO + fractional delay): windowed-sinc,
// CM_RS_TAPS taps, CM_RS_PHASES polyphase table with linear interpolation
// between phases, double-precision time accumulator.
//
// ratio = output samples per input sample (fs_out / fs_in).
#define CM_RS_TAPS   16
#define CM_RS_PHASES 512
typedef struct {
  double ratio;
  double t;                    // position of next output, in input samples,
                               // relative to hist[CM_RS_TAPS/2-1]
  cf_t   hist[CM_RS_TAPS * 2]; // input history (doubled for contiguous reads)
  int    hpos;
} cm_resamp;

void cm_resamp_init(cm_resamp *rs, double ratio, double frac_delay);
void cm_resamp_reset(cm_resamp *rs, double frac_delay);
void cm_resamp_set_ratio(cm_resamp *rs, double ratio);
// Consumes nx input samples, writes up to max_y outputs, returns count.
int  cm_resamp_run(cm_resamp *rs, const cf_t *x, int nx, cf_t *y, int max_y);
// Group delay in input samples (for timing compensation in tests).
double cm_resamp_delay(void);

///////////////////////////////////////////////////////////////////////////////////////
// ADC: quantize I and Q to 'bits' (signed, full scale +/-1.0) with clipping.
// Returns the number of clipped components.
int cm_adc(cf_t *x, int n, int bits);

#endif
