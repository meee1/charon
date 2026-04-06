/* This file was automatically generated.  Do not edit! */

#define CW_TONE_TX_MAX_SAMPLES  128

void cw_tone_init(void);
void cw_tone_reset(void);
int cw_tone_execute(float complex sample);
float cw_tone_get_freq_offset(void);
float cw_tone_get_peak(void);
void cw_tone_get_tx_samples(float complex *buf,int *len);
