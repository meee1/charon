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

// LTE-style Primary Synchronization Signal (PSS) synchronizer.
//
// Uses a Zadoff-Chu (ZC) sequence (same root as LTE PSS0, u=25, N=63) as a
// known preamble.  The receiver slides a window of PSS_ZC_LEN samples and
// computes the normalized cross-correlation for PSS_N_HYPOTHESES evenly-spaced
// carrier-frequency-offset (CFO) hypotheses.  The hypothesis with the highest
// correlation metric is chosen; when it exceeds PSS_CORR_THRESH the sync is
// declared and the estimated CFO is reported in normalized cycles/sample.
//
// TX side: pss_sync_get_tx_samples() fills a caller-supplied buffer with
// PSS_TX_REPS copies of the ZC sequence so the receiver has enough copies to
// acquire.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <sys/time.h>

#include "ofdm_conf.h"
#include "pss_sync.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Zadoff-Chu root index (LTE PSS sequence 0 uses u = 25)
#define PSS_ZC_ROOT         25

// ZC sequence length (must be prime; LTE uses 63)
#define PSS_ZC_LEN          63

// Half-range of frequency hypotheses tested: offsets h = -K .. +K
// Gives PSS_N_HYPOTHESES = 2*K+1 = 7 tests covering ±3 subcarrier spacings
#define PSS_HALF_HYPO       3
#define PSS_N_HYPOTHESES    (2 * PSS_HALF_HYPO + 1)

// Normalized correlation threshold for declaring PSS detection (0..1)
#define PSS_CORR_THRESH     0.7f

// Number of PSS repetitions to transmit as preamble
#define PSS_TX_REPS         2

// Lockout duration after detection (seconds)
#define PSS_LOCKOUT_SEC     10

// One subcarrier spacing in normalized frequency (cycles/sample)
#define PSS_FREQ_STEP       (1.0f / (float)OFDM_M)

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

// Reference ZC sequence (time domain, complex baseband)
static float complex pss_ref[PSS_ZC_LEN];

// Precomputed per-hypothesis, per-sample rotation×reference table.
//
// pss_rot_ref[hi][n] = exp(-j·2π·h·PSS_FREQ_STEP·n) · conj(pss_ref[n])
//   where h = hi - PSS_HALF_HYPO  (so hi=0 → h=-3, hi=3 → h=0, hi=6 → h=+3)
//
// Filled once in pss_sync_init(); used in pss_sync_execute() to avoid
// recomputing cosf/sinf/conjf on every incoming sample.
static float complex pss_rot_ref[PSS_N_HYPOTHESES][PSS_ZC_LEN];

// Circular input-sample buffer
static float complex pss_buf[PSS_ZC_LEN];
static int           pss_buf_idx;

// Results from the last pss_sync_execute() call
static float pss_cfo_est;    // estimated CFO in normalized cycles/sample
static float pss_peak_corr;  // normalized correlation of the best hypothesis

// Lockout timestamp: when non-zero, pss_sync_execute() skips correlation
// until PSS_LOCKOUT_SEC seconds have elapsed since detection
static struct timeval pss_lockout_tv;
static int pss_lockout_active;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Generate a Zadoff-Chu sequence of length N with root u:
//   x(n) = exp( -j * pi * u * n * (n+1) / N )  for n = 0 .. N-1
static void pss_gen_zc(float complex *seq, int N, int u)
{
    int n;
    for (n = 0; n < N; n++) {
        float phase = -(float)M_PI * (float)u * (float)n * (float)(n + 1) / (float)N;
        seq[n] = cosf(phase) + _Complex_I * sinf(phase);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void pss_sync_init(void)
{
    int hi, n;
    fprintf(stderr, "\n[pss_sync] init: ZC root=%d len=%d, hypotheses=%d, thresh=%.2f, freq_step=%.6f",
            PSS_ZC_ROOT, PSS_ZC_LEN, PSS_N_HYPOTHESES, PSS_CORR_THRESH, PSS_FREQ_STEP);
    pss_gen_zc(pss_ref, PSS_ZC_LEN, PSS_ZC_ROOT);

    // Precompute pss_rot_ref[hi][n] = exp(-j·2π·h·PSS_FREQ_STEP·n) · conj(pss_ref[n])
    // for each hypothesis index hi (h = hi - PSS_HALF_HYPO) and sample n.
    for (hi = 0; hi < PSS_N_HYPOTHESES; hi++) {
        int h = hi - PSS_HALF_HYPO;
        for (n = 0; n < PSS_ZC_LEN; n++) {
            float phase = -2.0f * (float)M_PI * (float)h * PSS_FREQ_STEP * (float)n;
            float complex rot = cosf(phase) + _Complex_I * sinf(phase);
            pss_rot_ref[hi][n] = rot * conjf(pss_ref[n]);
        }
    }

    memset(pss_buf, 0, sizeof(pss_buf));
    pss_buf_idx  = 0;
    pss_cfo_est  = 0.0f;
    pss_peak_corr = 0.0f;
    pss_lockout_active = 0;
    memset(&pss_lockout_tv, 0, sizeof(pss_lockout_tv));
    fprintf(stderr, "\n[pss_sync] init complete, ref[0]=(%.4f,%.4f) ref[1]=(%.4f,%.4f)",
            crealf(pss_ref[0]), cimagf(pss_ref[0]), crealf(pss_ref[1]), cimagf(pss_ref[1]));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void pss_sync_reset(void)
{
    fprintf(stderr, "\n[pss_sync] reset");
    memset(pss_buf, 0, sizeof(pss_buf));
    pss_buf_idx  = 0;
    pss_cfo_est  = 0.0f;
    pss_peak_corr = 0.0f;
    pss_lockout_active = 0;
    memset(&pss_lockout_tv, 0, sizeof(pss_lockout_tv));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process one incoming IQ sample.
//
// The function maintains a circular buffer of PSS_ZC_LEN samples.  For every
// new sample it tests PSS_N_HYPOTHESES frequency offsets h = -K .. +K
// (each separated by one subcarrier spacing = PSS_FREQ_STEP cycles/sample).
// For each hypothesis, the buffer is frequency-shifted and correlated with
// the reference PSS; the best hypothesis is selected by peak magnitude.
//
// Returns 1 if PSS detected (normalized correlation >= PSS_CORR_THRESH),
// 0 otherwise.  On detection, pss_sync_get_freq_offset() returns the
// estimated CFO in normalized cycles/sample.
////////////////////////////////////////////////////////////////////////////////////////////////////////
int pss_sync_execute(float complex sample)
{
    int h, n;
    float best_corr = 0.0f;
    int   best_h    = 0;

    // After a successful detection, skip correlation for a cooldown period
    if (pss_lockout_active) {
        struct timeval now;
        gettimeofday(&now, NULL);
        long long elapsed = (now.tv_sec - pss_lockout_tv.tv_sec) * 1000000LL
                          + (now.tv_usec - pss_lockout_tv.tv_usec);
        if (elapsed < PSS_LOCKOUT_SEC * 1000000LL)
            return 0;
        pss_lockout_active = 0;
    }

    // Append new sample to circular buffer
    pss_buf[pss_buf_idx] = sample;
    pss_buf_idx = (pss_buf_idx + 1) % PSS_ZC_LEN;

    // Compute received-signal power for normalization
    float rx_power = 0.0f;
    for (n = 0; n < PSS_ZC_LEN; n++)
        rx_power += crealf(pss_buf[n] * conjf(pss_buf[n]));

    if (rx_power < 1e-12f)
        return 0;

    // Test each frequency-offset hypothesis h = -PSS_HALF_HYPO .. +PSS_HALF_HYPO
    for (h = -PSS_HALF_HYPO; h <= PSS_HALF_HYPO; h++) {
        float complex corr = 0.0f + 0.0f * _Complex_I;
        int hi = h + PSS_HALF_HYPO;   // table row index (0 .. PSS_N_HYPOTHESES-1)

        for (n = 0; n < PSS_ZC_LEN; n++) {
            // Read chronologically-ordered sample from circular buffer
            int idx = (pss_buf_idx + n) % PSS_ZC_LEN;

            // Multiply by precomputed rot*conj(ref): no trig evaluation needed
            corr += pss_buf[idx] * pss_rot_ref[hi][n];
        }

        // Normalized correlation magnitude (0 = no match, 1 = perfect match)
        float corr_mag = cabsf(corr) / sqrtf(rx_power * (float)PSS_ZC_LEN);

        if (corr_mag > best_corr) {
            best_corr = corr_mag;
            best_h    = h;
        }
    }

    pss_peak_corr = best_corr;

    {
        static int dbg_counter = 0;
        if (++dbg_counter >= 100000) {
            fprintf(stderr, "\n[pss_sync] noise floor: best_corr=%.4f best_h=%d rx_pwr=%.4f",
                    best_corr, best_h, rx_power);
            dbg_counter = 0;
        }
    }

    if (best_corr >= PSS_CORR_THRESH) {
        // CFO estimate: the offset that was removed by the best hypothesis
        // equals best_h subcarrier spacings = best_h / OFDM_M cycles/sample
        pss_cfo_est = (float)best_h * PSS_FREQ_STEP;
        gettimeofday(&pss_lockout_tv, NULL);
        pss_lockout_active = 1;
        fprintf(stderr, "\n[pss_sync] DETECTED: corr=%.4f hypo=%d cfo=%.6f cyc/samp rx_pwr=%.2f (lockout %ds)",
                best_corr, best_h, pss_cfo_est, rx_power, PSS_LOCKOUT_SEC);
        return 1;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// Return estimated CFO from the last successful detection in normalized
// cycles/sample.  Multiply by 2*pi to get radians/sample for an NCO.
////////////////////////////////////////////////////////////////////////////////////////////////////////
float pss_sync_get_freq_offset(void)
{
    return pss_cfo_est;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// Return the peak normalized correlation magnitude from the last
// pss_sync_execute() call (diagnostic / debug use).
////////////////////////////////////////////////////////////////////////////////////////////////////////
float pss_sync_get_peak(void)
{
    return pss_peak_corr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// Fill *buf with PSS_TX_REPS copies of the time-domain ZC sequence suitable
// for prepending to an OFDM frame as a PSS preamble.
// *len is set to the total number of complex samples written.
// buf must point to at least PSS_TX_REPS * PSS_ZC_LEN float complex elements.
////////////////////////////////////////////////////////////////////////////////////////////////////////
void pss_sync_get_tx_samples(float complex *buf, int *len)
{
    int r;
    for (r = 0; r < PSS_TX_REPS; r++)
        memcpy(buf + r * PSS_ZC_LEN, pss_ref, PSS_ZC_LEN * sizeof(float complex));
    *len = PSS_TX_REPS * PSS_ZC_LEN;
    fprintf(stderr, "\n[pss_sync] get_tx_samples: reps=%d zc_len=%d total=%d", PSS_TX_REPS, PSS_ZC_LEN, *len);
}
