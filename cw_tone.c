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

// Single CW (continuous wave) tone for carrier frequency offset estimation.
//
// TX side: cw_tone_get_tx_samples() fills a buffer with CW_TONE_LEN samples
// of a DC tone (1.0 + 0.0j).  The tone is transmitted before the OFDM frame.
//
// RX side: cw_tone_execute() processes one sample at a time.  It maintains a
// circular buffer of CW_TONE_LEN samples and computes a lag-CW_TONE_HALF
// autocorrelation.  A pure tone produces |R(D)|/(P/2) ~ 1.0; when this metric
// exceeds CW_TONE_CORR_THRESH the tone is declared detected and the CFO is
// estimated from the autocorrelation phase:
//   cfo = arg(R(D)) / (2*pi*D)   cycles/sample

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>

#include "ofdm_conf.h"
#include "cw_tone.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define CW_TONE_LEN          128
#define CW_TONE_HALF         (CW_TONE_LEN / 2)
#define CW_TONE_COARSE_LAG   4
#define CW_TONE_CORR_THRESH  0.85f
#define CW_TONE_PWR_THRESH   1e-6f

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static float complex cw_buf[CW_TONE_LEN];
static int           cw_buf_idx;
static int           cw_sample_count;
static float         cw_cfo_est;
static float         cw_peak_metric;
static int           cw_detected;      // latch: once detected, stop until reset

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void cw_tone_init(void)
{
    fprintf(stderr, "\n[cw_tone] init: tone_len=%d coarse_lag=%d fine_lag=%d thresh=%.2f",
            CW_TONE_LEN, CW_TONE_COARSE_LAG, CW_TONE_HALF, CW_TONE_CORR_THRESH);
    memset(cw_buf, 0, sizeof(cw_buf));
    cw_buf_idx     = 0;
    cw_sample_count = 0;
    cw_cfo_est     = 0.0f;
    cw_peak_metric = 0.0f;
    cw_detected    = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void cw_tone_reset(void)
{
    memset(cw_buf, 0, sizeof(cw_buf));
    cw_buf_idx     = 0;
    cw_sample_count = 0;
    cw_cfo_est     = 0.0f;
    cw_peak_metric = 0.0f;
    cw_detected    = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process one incoming IQ sample.
//
// Maintains a circular buffer of CW_TONE_LEN samples.  Once the buffer is
// full, computes the lag-CW_TONE_HALF autocorrelation R and total power P.
// If the normalized metric |R|/(P/2) exceeds threshold, the tone is detected
// and CFO is estimated from arg(R).
//
// Returns 1 if tone detected, 0 otherwise.
////////////////////////////////////////////////////////////////////////////////////////////////////////
int cw_tone_execute(float complex sample)
{
    int n;

    // Once detected, stop correlating until cw_tone_reset() re-arms.
    // This prevents noise false-positives from continuously setting the NCO.
    if (cw_detected)
        return 0;

    cw_buf[cw_buf_idx] = sample;
    cw_buf_idx = (cw_buf_idx + 1) % CW_TONE_LEN;

    // Wait until the circular buffer is full before starting to correlate.
    if (cw_sample_count < CW_TONE_LEN) {
        cw_sample_count++;
        return 0;
    }

    // --- Stage 1: Coarse CFO estimation (lag-4, ±175 kHz capture range) ---
    int coarse_pairs = CW_TONE_LEN - CW_TONE_COARSE_LAG;
    float complex autocorr_coarse = 0.0f + 0.0f * _Complex_I;
    float power = 0.0f;

    for (n = 0; n < coarse_pairs; n++) {
        int idx_early = (cw_buf_idx + n) % CW_TONE_LEN;
        int idx_late  = (cw_buf_idx + n + CW_TONE_COARSE_LAG) % CW_TONE_LEN;
        autocorr_coarse += cw_buf[idx_late] * conjf(cw_buf[idx_early]);
    }

    for (n = 0; n < CW_TONE_LEN; n++)
        power += crealf(cw_buf[n] * conjf(cw_buf[n]));

    if (power < CW_TONE_PWR_THRESH)
        return 0;

    float power_scaled = power * (float)coarse_pairs / (float)CW_TONE_LEN;
    float metric = cabsf(autocorr_coarse) / power_scaled;
    cw_peak_metric = metric;

    if (metric < CW_TONE_CORR_THRESH)
        return 0;

    float coarse_cfo = cargf(autocorr_coarse) / (2.0f * (float)M_PI * (float)CW_TONE_COARSE_LAG);

    // --- Stage 2: Fine CFO estimation (lag-64, after de-rotation) ---
    float complex derot_buf[CW_TONE_LEN];
    float derot_phase_inc = -2.0f * (float)M_PI * coarse_cfo;
    float complex phasor = 1.0f + 0.0f * _Complex_I;
    float complex delta  = cosf(derot_phase_inc) + sinf(derot_phase_inc) * _Complex_I;
    for (n = 0; n < CW_TONE_LEN; n++) {
        int idx = (cw_buf_idx + n) % CW_TONE_LEN;
        derot_buf[n] = cw_buf[idx] * phasor;
        phasor *= delta;
    }

    float complex autocorr_fine = 0.0f + 0.0f * _Complex_I;
    for (n = 0; n < CW_TONE_HALF; n++)
        autocorr_fine += derot_buf[n + CW_TONE_HALF] * conjf(derot_buf[n]);

    float fine_cfo = cargf(autocorr_fine) / (2.0f * (float)M_PI * (float)CW_TONE_HALF);

    cw_cfo_est = coarse_cfo + fine_cfo;
    cw_detected = 1;
    return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
float cw_tone_get_freq_offset(void)
{
    return cw_cfo_est;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
float cw_tone_get_peak(void)
{
    return cw_peak_metric;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// Fill *buf with CW_TONE_LEN samples of a DC tone (1.0 + 0.0j).
// *len is set to the total number of complex samples written.
////////////////////////////////////////////////////////////////////////////////////////////////////////
void cw_tone_get_tx_samples(float complex *buf, int *len)
{
    int i;
    for (i = 0; i < CW_TONE_LEN; i++)
        buf[i] = 1.0f + 0.0f * _Complex_I;
    *len = CW_TONE_LEN;
    //fprintf(stderr, "\n[cw_tone] get_tx_samples: len=%d", *len);
}
