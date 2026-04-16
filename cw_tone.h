/* This file was automatically generated.  Do not edit! */

#define CW_TONE_TX_MAX_SAMPLES  (128 + 16)   /* tone + guard */

void cw_tone_init(void);
void cw_tone_reset(void);
int cw_tone_execute(float complex sample);
float cw_tone_get_freq_offset(void);
float cw_tone_get_peak(void);
int cw_tone_was_detected(void);
void cw_tone_disable(void);
float cw_tone_get_noise_floor(void);
void cw_tone_get_tx_samples(float complex *buf,int *len);
