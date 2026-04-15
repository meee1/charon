/* This file was automatically generated.  Do not edit! */
int do_ofdm_tx(uint8_t *buffer,int len,int is_retrans,int do_dump_rx,int is_broadcast,uint8_t *dst_ofdm0_mac,uint32_t pid);
void init_ofdm_tx();
int ofdm_get_sample_count(int payload_len);
int ofdm_tx_loopback(uint8_t *payload,int len);
int ofdm_tx_loopback_rf(uint8_t *payload,int len);
extern const int charon_added_length;
extern const uint32_t charon_ack_magic;
