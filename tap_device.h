/* This file was automatically generated.  Do not edit! */
#include <stdint.h>
int write_tap_dev(char *buffer,int len);
int read_tap_dev(char *buffer,int max_len,char *dst_mac,char *is_broadcast);
int setNonblocking(int fd);
int init_tap_device(void);
void tap_mac_from_ip(uint32_t ip32,char *mac_a);
extern uint8_t ofdm0_mac[6];
extern const uint8_t mac_all_one[6];
extern const uint8_t mac_all_zero[6];
