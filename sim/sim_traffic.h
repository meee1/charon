/* This file was automatically generated.  Do not edit! */
int sim_traffic_write(const uint8_t *buf,int len);
int sim_traffic_read(uint8_t *buf,int max_len,uint8_t *dst_mac,uint8_t *is_broadcast);
void sim_traffic_init(const uint8_t *mac,uint32_t ip32);
int sim_traffic_enabled(void);
void sim_traffic_mac_from_ip(uint32_t ip32,uint8_t *mac);
