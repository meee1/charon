/* This file was automatically generated.  Do not edit! */

// Maximum number of float complex samples that pss_sync_get_tx_samples() can
// produce (PSS_TX_REPS * PSS_ZC_LEN = 2 * 63 = 126).  Callers use this to
// size their stack/static buffers.
#define PSS_TX_MAX_SAMPLES  126

void pss_sync_init(void);
void pss_sync_reset(void);
int pss_sync_execute(float complex sample);
float pss_sync_get_freq_offset(void);
float pss_sync_get_peak(void);
void pss_sync_get_tx_samples(float complex *buf,int *len);
