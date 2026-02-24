/*
 * PSS Sync Example - LTE-style Primary Synchronization Signal
 *
 * This example demonstrates:
 * - Generating a Zadoff-Chu (ZC) PSS sequence (same root as LTE PSS0, u=25, N=63)
 * - Transmitting multiple copies as a frequency-acquisition preamble
 * - Detecting the PSS with a sliding-window correlator that tests
 *   PSS_N_HYPOTHESES = 7 carrier-frequency-offset (CFO) hypotheses
 *   covering ±3 subcarrier spacings
 * - Reporting which hypothesis wins and the estimated CFO
 *
 * The key feature of the "multiple frequency offset hypotheses" approach is
 * that the receiver doesn't need the NCO to be pre-tuned: it exhaustively
 * tests a set of discrete CFO candidates and picks the one with the highest
 * normalized cross-correlation against the reference PSS.  This is directly
 * analogous to the initial cell search in LTE.
 *
 * Compile (standalone, no external deps):
 *   gcc -O2 -o pss_sync_example pss_sync_example.c -lm
 * Run:
 *   ./pss_sync_example
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>

// ---------------------------------------------------------------------------
// Configuration -- must match pss_sync.c / ofdm_conf.h values
// ---------------------------------------------------------------------------
#define OFDM_M              64      // number of OFDM subcarriers
#define PSS_ZC_ROOT         25      // ZC root (LTE PSS0 = u=25)
#define PSS_ZC_LEN          63      // ZC sequence length (prime)
#define PSS_HALF_HYPO       3       // half-range of hypotheses (K)
#define PSS_N_HYPOTHESES    (2 * PSS_HALF_HYPO + 1)  // 7
#define PSS_CORR_THRESH     0.7f
#define PSS_TX_REPS         2       // repetitions sent in preamble
#define PSS_FREQ_STEP       (1.0f / (float)OFDM_M)  // one subcarrier spacing

// ---------------------------------------------------------------------------
// Generate ZC sequence: x(n) = exp(-j*pi*u*n*(n+1)/N)
// ---------------------------------------------------------------------------
static void pss_gen_zc(float complex *seq, int N, int u)
{
    int n;
    for (n = 0; n < N; n++) {
        float phase = -(float)M_PI * (float)u * (float)n * (float)(n + 1) / (float)N;
        seq[n] = cosf(phase) + _Complex_I * sinf(phase);
    }
}

// ---------------------------------------------------------------------------
// Simple pseudo-random noise (Box-Muller) for AWGN
// ---------------------------------------------------------------------------
static float randn(void)
{
    static int have_spare = 0;
    static float spare;
    if (have_spare) { have_spare = 0; return spare; }
    float u, v, s;
    do { u = 2.0f * rand() / RAND_MAX - 1.0f;
         v = 2.0f * rand() / RAND_MAX - 1.0f;
         s = u*u + v*v; } while (s >= 1.0f || s == 0.0f);
    float mul = sqrtf(-2.0f * logf(s) / s);
    spare = v * mul;
    have_spare = 1;
    return u * mul;
}

// ---------------------------------------------------------------------------
// PSS correlator: test PSS_N_HYPOTHESES CFO hypotheses on a buffer of
// PSS_ZC_LEN samples using precomputed rot_ref tables.
// Returns the best normalized correlation and sets *best_h to the winning
// hypothesis index.
// rot_ref[hi][n] = exp(-j·2π·h·PSS_FREQ_STEP·n) · conj(ref[n])  (precomputed)
// ---------------------------------------------------------------------------
static float pss_correlate(const float complex *buf,
                            float complex rot_ref[PSS_N_HYPOTHESES][PSS_ZC_LEN],
                            int *best_h_out)
{
    int h, n;
    float best_corr = 0.0f;
    int   best_h    = 0;

    // Received-signal power for normalization
    float rx_power = 0.0f;
    for (n = 0; n < PSS_ZC_LEN; n++)
        rx_power += crealf(buf[n] * conjf(buf[n]));
    if (rx_power < 1e-12f) { *best_h_out = 0; return 0.0f; }

    for (h = -PSS_HALF_HYPO; h <= PSS_HALF_HYPO; h++) {
        float complex corr = 0.0f + 0.0f * _Complex_I;
        int hi = h + PSS_HALF_HYPO;
        for (n = 0; n < PSS_ZC_LEN; n++)
            corr += buf[n] * rot_ref[hi][n];
        float corr_mag = cabsf(corr) / sqrtf(rx_power * (float)PSS_ZC_LEN);
        if (corr_mag > best_corr) {
            best_corr = corr_mag;
            best_h    = h;
        }
    }

    *best_h_out = best_h;
    return best_corr;
}

// ---------------------------------------------------------------------------
// Run one test: apply cfo_offset (in subcarrier spacings) to the PSS
// preamble, add AWGN at snr_db, then run the sliding-window correlator.
// rot_ref is the precomputed rotation×reference table.
// Returns 1 if the correct hypothesis is found, 0 otherwise.
// ---------------------------------------------------------------------------
static int run_test(float complex *pss_ref,
                    float complex rot_ref[PSS_N_HYPOTHESES][PSS_ZC_LEN],
                    int applied_h, float snr_db,
                    int verbose)
{
    // Build PSS preamble (PSS_TX_REPS copies)
    int preamble_len = PSS_TX_REPS * PSS_ZC_LEN;
    float complex *preamble = malloc(preamble_len * sizeof(float complex));
    int r, n;
    for (r = 0; r < PSS_TX_REPS; r++)
        memcpy(preamble + r * PSS_ZC_LEN, pss_ref,
               PSS_ZC_LEN * sizeof(float complex));

    // Apply frequency offset of applied_h subcarrier spacings
    float cfo_norm = (float)applied_h * PSS_FREQ_STEP; // cycles/sample
    for (n = 0; n < preamble_len; n++) {
        float phase = 2.0f * (float)M_PI * cfo_norm * (float)n;
        float complex rot = cosf(phase) + _Complex_I * sinf(phase);
        preamble[n] *= rot;
    }

    // Add AWGN
    float noise_amp = powf(10.0f, -snr_db / 20.0f);
    for (n = 0; n < preamble_len; n++) {
        preamble[n] += noise_amp * (randn() + _Complex_I * randn()) * 0.5f;
    }

    // Slide a PSS_ZC_LEN window over the received preamble and correlate
    int detected_h = 0;
    float peak_corr = 0.0f;
    int detection_sample = -1;
    int h;

    float complex window[PSS_ZC_LEN];
    for (n = 0; n <= preamble_len - PSS_ZC_LEN; n++) {
        memcpy(window, preamble + n, PSS_ZC_LEN * sizeof(float complex));
        float corr = pss_correlate(window, rot_ref, &h);
        if (corr > peak_corr) {
            peak_corr  = corr;
            detected_h = h;
            detection_sample = n;
        }
    }

    int success = (peak_corr >= PSS_CORR_THRESH) && (detected_h == applied_h);

    if (verbose) {
        printf("  Applied CFO: %+d sc (%.4f cyc/samp = %.2f Hz @ 1.4 MHz)\n",
               applied_h, cfo_norm, cfo_norm * 1400000.0f);
        printf("  Best hypothesis: %+d sc  peak=%.4f  at sample %d  %s\n",
               detected_h, peak_corr, detection_sample,
               success ? "DETECTED ✓" : "MISSED ✗");
        // Print correlation for every hypothesis at the peak window
        if (detection_sample >= 0) {
            memcpy(window, preamble + detection_sample,
                   PSS_ZC_LEN * sizeof(float complex));
            printf("  Per-hypothesis correlations:\n");
            for (h = -PSS_HALF_HYPO; h <= PSS_HALF_HYPO; h++) {
                int hi = h + PSS_HALF_HYPO;
                float rx_power = 0.0f;
                for (int i = 0; i < PSS_ZC_LEN; i++)
                    rx_power += crealf(window[i] * conjf(window[i]));
                float complex corr_c = 0.0f + 0.0f * _Complex_I;
                for (int i = 0; i < PSS_ZC_LEN; i++)
                    corr_c += window[i] * rot_ref[hi][i];
                float cm = (rx_power > 1e-12f) ?
                    cabsf(corr_c) / sqrtf(rx_power * (float)PSS_ZC_LEN) : 0.0f;
                printf("    h=%+d: corr=%.4f%s\n", h, cm,
                       h == applied_h ? " <-- applied" :
                       h == detected_h ? " <-- detected" : "");
            }
        }
    }

    free(preamble);
    return success;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void)
{
    srand(42);

    printf("PSS Sync Example - LTE-style with Multiple Frequency Hypotheses\n");
    printf("================================================================\n\n");
    printf("Configuration:\n");
    printf("  ZC root (u):        %d  (LTE PSS0)\n", PSS_ZC_ROOT);
    printf("  ZC length (N):      %d  (LTE standard)\n", PSS_ZC_LEN);
    printf("  Hypotheses tested:  %d  (h = %+d .. %+d subcarrier spacings)\n",
           PSS_N_HYPOTHESES, -PSS_HALF_HYPO, PSS_HALF_HYPO);
    printf("  Subcarrier spacing: %.4f cycles/sample (OFDM_M=%d)\n",
           PSS_FREQ_STEP, OFDM_M);
    printf("  Detection thresh:   %.2f (normalized cross-correlation)\n",
           PSS_CORR_THRESH);
    printf("  TX preamble reps:   %d  (%d samples total)\n\n",
           PSS_TX_REPS, PSS_TX_REPS * PSS_ZC_LEN);

    // Generate reference PSS
    float complex pss_ref[PSS_ZC_LEN];
    pss_gen_zc(pss_ref, PSS_ZC_LEN, PSS_ZC_ROOT);

    // Precompute rotation×reference table once (avoids trig in the correlator hot-path).
    // rot_ref[hi][n] = exp(-j·2π·h·PSS_FREQ_STEP·n) · conj(pss_ref[n])
    // where h = hi - PSS_HALF_HYPO.
    float complex rot_ref[PSS_N_HYPOTHESES][PSS_ZC_LEN];
    {
        int hi, n;
        for (hi = 0; hi < PSS_N_HYPOTHESES; hi++) {
            int h = hi - PSS_HALF_HYPO;
            for (n = 0; n < PSS_ZC_LEN; n++) {
                float phase = -2.0f * (float)M_PI * (float)h * PSS_FREQ_STEP * (float)n;
                float complex rot = cosf(phase) + _Complex_I * sinf(phase);
                rot_ref[hi][n] = rot * conjf(pss_ref[n]);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Detailed single-test output
    // -----------------------------------------------------------------------
    int applied_h = 2; // 2-subcarrier offset
    float snr_db  = 20.0f;

    printf("--- Detailed test: applied CFO = %+d subcarrier spacings, SNR = %.0f dB ---\n\n",
           applied_h, snr_db);
    run_test(pss_ref, rot_ref, applied_h, snr_db, 1 /*verbose*/);

    // -----------------------------------------------------------------------
    // Sweep all in-range hypotheses at several SNRs
    // -----------------------------------------------------------------------
    float snrs[]  = {30.0f, 20.0f, 10.0f, 5.0f};
    int n_snrs    = (int)(sizeof(snrs) / sizeof(snrs[0]));
    int pass, total;
    int h;

    printf("\n--- Detection rate vs SNR (each cell = %d trials) ---\n\n",
           PSS_N_HYPOTHESES);
    printf("  CFO\\SNR ");
    for (int si = 0; si < n_snrs; si++) printf(" %5.0f dB", snrs[si]);
    printf("\n  ");
    for (int si = 0; si < n_snrs + 1; si++) printf("----------");
    printf("\n");

    for (h = -PSS_HALF_HYPO; h <= PSS_HALF_HYPO; h++) {
        printf("  h=%+2d sc ", h);
        for (int si = 0; si < n_snrs; si++) {
            int ok = run_test(pss_ref, rot_ref, h, snrs[si], 0 /*quiet*/);
            printf(" %s        ", ok ? "PASS" : "FAIL");
        }
        printf("\n");
    }

    // -----------------------------------------------------------------------
    // Summary pass/fail across all hypotheses at 20 dB SNR
    // -----------------------------------------------------------------------
    pass = 0; total = 0;
    printf("\n--- Summary at 20 dB SNR ---\n");
    for (h = -PSS_HALF_HYPO; h <= PSS_HALF_HYPO; h++) {
        int ok = run_test(pss_ref, rot_ref, h, 20.0f, 0);
        pass += ok; total++;
        printf("  h=%+d: %s\n", h, ok ? "PASS" : "FAIL");
    }
    printf("\n%d / %d hypotheses correctly detected\n", pass, total);
    printf("\n");

    if (pass == total) {
        printf("✓ All PSS frequency-offset hypotheses detected successfully!\n");
        return 0;
    } else {
        printf("✗ Some PSS detections failed.\n");
        return 1;
    }
}
