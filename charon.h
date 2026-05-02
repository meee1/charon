/* This file was automatically generated.  Do not edit! */
void do_process_iq16_batch(const int16_t *buf, int count);
void check_agc(void);
void lbt_backoff(void);
void reset_agc_timer(void);
void do_tx(uint8_t *buff,int len,int is_retrans,uint8_t *dst_mac,uint8_t is_broadcast,uint32_t pid);
void do_send_ack(uint8_t *ack_mac);
void do_got_ack();
int run_pluto_test(void);
int run_loopback_test(void);
void main_loop(void);
int main(int argc,char **argv);
extern int first_rx_after_agc_reset;
extern uint8_t mac_to_ack[6];
