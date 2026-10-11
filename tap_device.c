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

#define _DEFAULT_SOURCE 1 //get rid of warning about usleep

#include "timers.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "tuntap.h"
#include "private.h"
#include "tap_device.h"
#include "charon.h"
#include "tcp_subs.h"
#include "config.h"

#include "ethernet.h"

#ifdef CHARON_SIM
#include "sim/sim_traffic.h"
#endif

static struct device *dev;
static int debug_tap=1;

const uint8_t mac_all_zero[6]={0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
const uint8_t mac_all_one[6]={0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

uint8_t ofdm0_mac[6];
static uint8_t tap_mac_a[18];

/////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////
// Derive the ofdm0 MAC ("xx:xx:xx:xx:xx:xx") from a node IP.
void tap_mac_from_ip(uint32_t ip32, char *mac_a) {
  srandom(ip32);  //always the same mac for a given IP. keeps the mesh from getting behind too much when a 
                  //client comes back up after re-start.
  sprintf(mac_a, "%02x:%02x:%02x:%02x:%02x:%02x",
      (uint8_t) ((random()%255)&0xfc),
      (uint8_t) (random()%255),
      (uint8_t) (random()%255),
      (uint8_t) (random()%255),
      (uint8_t) (random()%255),
    (uint8_t) (random()%255 ));
  // Re-seed for everything else that uses random(): frame pids (duplicate
  // detection at the receiver) and retransmission backoff.  Mix in the IP
  // and pid: nodes started in the same second -- every Pluto boots with its
  // clock at the epoch -- would otherwise share one sequence, and a
  // receiver then drops one sender's frames as duplicates of another's.
  srandom((unsigned)time(NULL) ^ ip32 ^ ((unsigned)getpid() << 16));
}

/////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////
static void parse_mac(const char *mac_a, uint8_t *mac) {
  unsigned int m[6];
  sscanf(mac_a, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
  for(int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
}

/////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////
int init_tap_device(void) {
	int ret;
	char *hwaddr;

	ret = 0;
  fprintf(stderr, "\ntuntap init");

#ifdef CHARON_SIM
  // Simulator traffic mode: no TAP device, frames come from sim_traffic.c
  if(sim_traffic_enabled()) {
    tap_mac_from_ip(usb_ip32, tap_mac_a);
    parse_mac(tap_mac_a, ofdm0_mac);
    sim_traffic_init(ofdm0_mac, usb_ip32);
    return 0;
  }
#endif

  dev = tuntap_init();

  if(dev==NULL) {
    fprintf(stderr, "\ngot null device when trying to create tap");
    return -1;
  }
  else {
    fprintf(stderr, "\ncreated tap device");
  }

	if (tuntap_start(dev, TUNTAP_MODE_ETHERNET, 0/*TUNTAP_ID_ANY*/) == -1) {
		fprintf(stderr, "\ntuntap_start failed");
		ret = 1;
		goto clean;
	}



  tap_mac_from_ip(usb_ip32, tap_mac_a);

	if (tuntap_set_hwaddr(dev, tap_mac_a) == -1) {
		ret = 1;
    fprintf(stderr, "\nerr: %s", tap_mac_a);
		goto clean;
	}

	if (tuntap_set_mtu(dev, 2342) == -1) {
		ret = 1;
		goto clean;
	}

	if (tuntap_up(dev) == -1) {
		ret = 1;
		goto clean;
	}

  parse_mac(tap_mac_a, ofdm0_mac);

  setNonblocking(dev->tun_fd);

  return 0;

clean:
	fprintf(stderr, "\ntuntap clean");
	tuntap_destroy(dev);
	return ret;
}


/////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////
int setNonblocking(int fd)
{
      int flags;

  #if defined(O_NONBLOCK)
      if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
                flags = 0;
      return fcntl(fd, F_SETFL, flags | O_NONBLOCK | O_SYNC);
  #else
        flags = 1;
      return ioctl(fd, FIOBIO, &flags);
  #endif
}  

/////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////
int read_tap_dev(char *buffer, int max_len, char *dst_mac, char *is_broadcast) {

 int nbytes;
 int new_len;

 eth_frame *ethframe = (eth_frame *) buffer;

  batman_frame_unicast *batucast = (batman_frame_unicast *) buffer;
  batman_unicast *bathdr_u = &(batucast->bathdr_unicast);

 eth_hdr *ethhdr = (eth_hdr *) &(ethframe->ethhdr);

#ifdef CHARON_SIM
 if(sim_traffic_enabled())
   return sim_traffic_read((uint8_t *)buffer, max_len, (uint8_t *)dst_mac, (uint8_t *)is_broadcast);
#endif

 nbytes = read(dev->tun_fd, buffer, max_len);

 if(nbytes<=0) return 0;

  memcpy(dst_mac, mac_all_zero, 6);
    
  uint16_t frame_type  = htons(ethhdr->eth_type);
  switch( frame_type ) {
    case  ETH_IP_TYPE :
      //fprintf(stderr,"\neth_type: IP");
      memcpy(dst_mac, &(ethhdr->dst_mac), 6);

       if( htons(ethhdr->eth_type)==0x0800 ) {
         new_len = do_tcp_subs(buffer, nbytes);
         if(new_len > 0) nbytes = new_len;
       }
    break;

    case  ETH_ARP_TYPE :
      //fprintf(stderr,"\neth_type: ARP");
      memcpy(dst_mac, &(ethhdr->dst_mac), 6);
    break;

    case  ETH_BATMAN_TYPE :
      //fprintf(stderr,"\neth_type: BATMAN");

        if( (int) bathdr_u->type >=0x00 && (int) bathdr_u->type<=0x3f ) { 
          is_broadcast[0] = 1;
          memcpy( dst_mac, mac_all_one, 6 );  
          return nbytes;
        }
        else {
          is_broadcast[0] = 0;
          memcpy( dst_mac, bathdr_u->dst_mac, 6 );  //this is the tap destination interface.  
                                                    //need to see if it is one our "next hops in the originators table"

          if( (uint8_t) bathdr_u->type >(uint8_t) 0x3f ) { 
            //fprintf(stderr,"\nbat_unicast, try tcp_sub");
            eth_hdr *ethhdr = (eth_hdr *) &(batucast->ethhdr);
            if( htons(ethhdr->eth_type)==ETH_IP_TYPE) {
              new_len = do_tcp_subs((char *)ethhdr, nbytes - (int)(sizeof(batman_unicast)+sizeof(eth_hdr)));
              if(new_len > 0) nbytes = new_len + sizeof(batman_unicast)+sizeof(eth_hdr); 
            }
          }
        }
      
    break;

    default :
      memcpy(dst_mac, mac_all_zero, 6);
      is_broadcast[0]=0;
      return 0;
  }

      if( memcmp(mac_all_one,dst_mac,6)==0 ) {
        is_broadcast[0] = 1;
      }
      else {
        is_broadcast[0] = 0;
      }


  if(nbytes>0 && debug_tap) {
   fprintf(stderr,"\n\nRead %d bytes from ofdm0\n", nbytes);
  }
 return nbytes;
}
/////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////
int write_tap_dev(char *buffer, int len) {
 int i;
 int new_len;

 eth_frame *ethframe = (eth_frame *) buffer;

 batman_frame_unicast *batucast = (batman_frame_unicast *) buffer;
 batman_unicast *bathdr_u = &(batucast->bathdr_unicast);

 eth_hdr *ethhdr = (eth_hdr *) &(ethframe->ethhdr);


 if(len<=0) return 0;

#ifdef CHARON_SIM
 if(sim_traffic_enabled())
   return sim_traffic_write((const uint8_t *)buffer, len);
#endif

  uint16_t frame_type  = htons(ethhdr->eth_type);
  switch( frame_type ) {
    case  ETH_IP_TYPE :
      //fprintf(stderr,"\neth_type: IP");
       if( htons(ethhdr->eth_type)==ETH_IP_TYPE ) {
         new_len = do_tcp_subs(buffer, len);
         if(new_len > 0) len = new_len; 
       }
    break;

    case  ETH_BATMAN_TYPE :
      //fprintf(stderr,"\neth_type: BATMAN");

      if( (uint8_t) bathdr_u->type >(uint8_t) 0x3f ) { 

         //fprintf(stderr,"\nbat_unicast, try tcp_sub");
         eth_hdr *ethhdr = (eth_hdr *) &(batucast->ethhdr);
         if( htons(ethhdr->eth_type)==ETH_IP_TYPE) {
           new_len = do_tcp_subs((char *)ethhdr, len - (int)(sizeof(batman_unicast)+sizeof(eth_hdr)));
           if(new_len > 0) len = new_len + sizeof(batman_unicast)+sizeof(eth_hdr); 
         }
       }
      
    break;
  }


 if(len>0 && debug_tap) {
   fprintf(stderr,"\n\nRF_IN -> writing %d bytes to ofdm0\n", len);
  }

  i = write(dev->tun_fd, buffer, len);
  return i; 
}
