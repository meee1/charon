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

#include <stdint.h>
#include <stdio.h>
#include "tcp_subs.h"
#include "ethernet.h"
#include "charon.h"
#include "config.h"

#define TCP_MSS_SIZE 1460
/////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////
static uint16_t inet_chksum(uint16_t sum, uint8_t *data, int len)
{
  uint16_t t=0;
  int i=0;

  for (i=0; i<len; i+=2) {
    //odd length: last byte is padded with zero (without touching the buffer)
    t = (i + 1 < len) ? (uint16_t)((data[i] << 8) + data[i+1]) : (uint16_t)(data[i] << 8);
    sum += t;
    //carry
    if (sum < t) {
      sum++;
    }
  }
  return sum;
}

/////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////
uint16_t ipchksum(uint8_t *ipbuffer)
{
  uint16_t sum=0;
  sum = inet_chksum(0, ipbuffer, sizeof(ip_hdr));
  return (sum == 0) ? 0xffff : htons(sum);
}
/////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////
static uint16_t upper_layer_chksum(uint8_t proto, uint8_t *eth_buffer)
{

  uint16_t upper_layer_len=0;
  uint16_t sum=0;

  eth_frame *ethframe = (eth_frame *) eth_buffer;
  ip_hdr *ip = (ip_hdr *) &(ethframe->iphdr);

  upper_layer_len = htons( ip->len ) - sizeof(ip_hdr);

  switch (proto) {

  case  IP_PROTO_UDP  :
  case  IP_PROTO_TCP  :

    sum = upper_layer_len + proto;
    sum = inet_chksum(sum, (uint8_t *)&eth_buffer[sizeof(eth_hdr) + 12], 2 * 4); //2 * length of ip address
    sum = inet_chksum(sum, &eth_buffer[sizeof(ip_hdr) + sizeof(eth_hdr)], upper_layer_len);
    break;

  case  IP_PROTO_ICMP :
    sum = inet_chksum(sum, &eth_buffer[sizeof(ip_hdr) + sizeof(eth_hdr)], upper_layer_len);
    break;


  }

  return (sum == 0) ? 0xffff : htons(sum);
}
/////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////
uint16_t tcpchksum(uint8_t *framebuffer)
{
  return upper_layer_chksum(IP_PROTO_TCP, framebuffer);
}
/////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////
uint16_t udpchksum(uint8_t *framebuffer)
{
  return upper_layer_chksum(IP_PROTO_UDP, framebuffer);
}
/////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////
uint16_t icmpchksum(uint8_t *framebuffer)
{
  return upper_layer_chksum(IP_PROTO_ICMP, framebuffer);
}

/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////
// Walk the SYN's TCP options: lower the MSS to mss_sub_size and NOP out
// window scaling.  Each option is kind,len,body except END and NOP; a
// malformed length stops the walk without touching anything further.
static void sub_mss(uint16_t mss_sub_size, uint8_t *opt, int options_len)
{
  int i = 0;

  while (i < options_len) {
    uint8_t kind = opt[i];

    if (kind == TCP_OPT_END)
      return;
    if (kind == TCP_OPT_NOOP) {
      i++;
      continue;
    }
    if (i + 1 >= options_len)
      return;
    uint8_t len = opt[i + 1];
    if (len < 2 || i + len > options_len)
      return;

    if (kind == TCP_OPT_MSS && len == 4) {
      //only ever lower the MSS a host advertises
      uint16_t mss = (uint16_t)((opt[i + 2] << 8) | opt[i + 3]);
      if (mss > mss_sub_size) {
        opt[i + 2] = (uint8_t)(mss_sub_size >> 8);
        opt[i + 3] = (uint8_t)(mss_sub_size & 0xff);
      }
    } else if (kind == TCP_OPT_WIN_SCALE && len == 3) {
      //don't allow window scaling
      opt[i] = opt[i + 1] = opt[i + 2] = TCP_OPT_NOOP;
    }
    i += len;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
// frame_len: bytes actually present in eth_frame_buffer.  Frames whose IP or
// TCP header fields don't fit inside it, IPv4 headers with options, and
// fragments are passed through untouched (returns 0).  Otherwise returns the
// frame length implied by the IP total length (<= frame_len).
int do_tcp_subs(uint8_t *eth_frame_buffer, int frame_len) {

  int8_t recalc_chksum=0;

  eth_frame *ethframe = (eth_frame *) eth_frame_buffer;
  eth_hdr *ethhdr = (eth_hdr *) &(ethframe->ethhdr);
  ip_hdr *ip = (ip_hdr *) &(ethframe->iphdr);
  tcp_hdr *tcp = (tcp_hdr *) &(ethframe->ip_proto_payload);

  if( frame_len < (int)(sizeof(eth_hdr) + sizeof(ip_hdr) + sizeof(tcp_hdr)) ||
      htons( ethhdr->eth_type ) != ETH_IP_TYPE ) {
    return 0;
  }

  //only plain 20-byte IPv4 headers carrying an unfragmented TCP segment
  int ip_len = htons(ip->len);
  if( ip->version != 0x45 || ip->proto != IP_PROTO_TCP ||
      (htons(ip->offset) & 0x3fff) != 0 ||
      ip_len < (int)(sizeof(ip_hdr) + sizeof(tcp_hdr)) ||
      ip_len + (int)sizeof(eth_hdr) > frame_len ) {
    return 0;
  }
  int tcp_len = (tcp->hdr_len & 0xf0) >> 2;
  if( tcp_len < (int)sizeof(tcp_hdr) || tcp_len > ip_len - (int)sizeof(ip_hdr) ) {
    return 0;
  }

  if ( (tcp->tcp_flags == TCP_SYN || tcp->tcp_flags == (TCP_SYN | TCP_ACK)) ) {
    sub_mss(TCP_MSS_SIZE, tcp->payload, tcp_len - (int)sizeof(tcp_hdr));
    recalc_chksum=1;
  }

  //pretty much no chance buffers will be a problem for the remotes, so re-write a constant window that is reasonable for bandwidth-limited wireless system
  if ( ip->proto==IP_PROTO_TCP ) {
    tcp->win_size = htons( max_tcp_segs * TCP_MSS_SIZE ); 
    recalc_chksum=1;
  }

  if ( ip->proto==IP_PROTO_TCP && recalc_chksum) {
    tcp->chksum = 0;
    tcp->chksum = ~tcpchksum(eth_frame_buffer);
    ip->chksum = 0;
    ip->chksum = ~ipchksum( (void *) ip );

    return htons(ip->len) + sizeof(eth_hdr);
  }


  return 0;
}
