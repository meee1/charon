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
#include "timers.h"

#include <iio.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>
#include <complex.h>
#include "liquid/liquid.h"

#include "pluto.h"
#include "ofdm_conf.h"
#include "ofdm_rx.h"
#include "ofdm_tx.h"
#include "charon.h"
#include "fftw3.h"
#include "ofdm.h"
#include "tap_device.h"
#include "crc.h"
#include "ethernet.h"
#include "config.h"

static unsigned int M = OFDM_M;        // number of subcarriers
static unsigned int cp_len = CP_LEN;   // cyclic prefix length
static unsigned int taper_len = TAPER_LEN; // taper length
static unsigned char p[OFDM_M];         // subcarrier allocation (null/pilot/data)
static void * userdata;            // user-defined data

static ofdmflexframesync fs;

int did_rx_ok;


static long long time_start;
static long long time_secs;
static long long kbps;
static float per;
static long long rx_gain;
static float rssi_dbm;

static struct timeval start;
static struct timeval end;
static long long kbit_bytes;
static long long kbit_count;
static long long drate;
static long long drate_bps;

static float last_good_nco_freq;

static int total_good_frames=0;
static int total_bad_frames=0;
static int64_t total_bytes=0;

static struct ofdmflexframesync_s *_q;
static struct ofdmframesync_s *_qq;
static uint8_t frame_buffer[2346];
static int frame_len;

static nco_crcf ofdm_nco;

static uint32_t in_pid;
static uint8_t ack_mac[6];
static uint8_t rec_ack[6];
static int is_broadcast = 0;
static int is_ack=0;
static int is_accept=0;

static int data_rate_kbps;

static int loopback_mode = 0;
int loopback_rx_ok = 0;
unsigned char loopback_rx_payload[PAYLOAD_LEN];
int loopback_rx_payload_len = 0;

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ofdm_rx_set_loopback(int enable) {
  loopback_mode = enable;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ofdm_rx_reset(void) {
  ofdmflexframesync_reset(fs);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void bump_nco() {
  static int nco_mod=0;
  if(nco_mod++%128==0) {
    nco_crcf_set_frequency(ofdm_nco, ((float)rand()/(float)RAND_MAX)*1e-4  );  //spread some of the dc offset /flicker noise out
                                                                 // by +/- 140Hz
  }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void do_ofdm_mix_down(float complex sample, float complex *y) {
  nco_crcf_step(ofdm_nco);
  nco_crcf_mix_down(ofdm_nco, sample, y);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
int ofdm_rx_state() {

  if(_q == NULL) {
    _q = fs;
    _qq = _q->fs;
  }

  return _qq->state; 
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
int ofdm_rx_callback(unsigned char *  _header,
int              _header_valid,
unsigned char *  _payload,
unsigned int     _payload_len,
int              _payload_valid,
framesyncstats_s _stats,
void *           _userdata) {

  if (loopback_mode) {
    loopback_rx_ok = 0;
    if (_header_valid && _payload_valid) {
      loopback_rx_ok = 1;
      loopback_rx_payload_len = _payload_len;
      if (_payload_len <= PAYLOAD_LEN)
        memcpy(loopback_rx_payload, _payload, _payload_len);
      fprintf(stderr, "  RSSI: %.2f dB, EVM: %.2f dB\n", _stats.rssi, _stats.evm);
      ofdmflexframesync_print(fs);
    }
    return 0;
  }

charon_frame *charonframe = (charon_frame *) &_payload[0];  //skip past pid area
eth_hdr *ethhdr_charon = (eth_hdr *) &(charonframe->ethhdr_charon);

in_pid=0;
is_broadcast = 0;
is_ack=0;
is_accept=0;

  if(data_rate_kbps==0) {
    data_rate_kbps = (((sample_freq_hz / DECIMATE_INTERPOLATE_FACTOR)/(OFDM_M+CP_LEN+TAPER_LEN)) * (92*_stats.mod_bps)) /1e3; //92 assumes 128-subcarriers with 92 being data
    data_rate_kbps *= (64.0/72.0);   //adjust for FEC coding rate: SECDED (64/72), inner FEC is NONE (rate 1.0)
  }

  lbt_backoff();  //we received something, so wait at least an ofdm symbol before transmitting

  if( _header_valid && _payload_valid ) {


    if( _payload_len==6) {
      memcpy(rec_ack, _payload, 6);

      if( memcmp(rec_ack, ofdm0_mac, 6)==0) {
        is_ack=1;
        is_accept=1;
      }
      else {
        fprintf(stderr, "\nACK_NOT_FOR_US  %02x:%02x:%02x:%02x:%02x:%02x",
            rec_ack[0],
            rec_ack[1],
            rec_ack[2],
            rec_ack[3],
            rec_ack[4],
            rec_ack[5]
          );
        if(tcp_share_backoff<max_tcp_share_backoff) tcp_share_backoff += symbol_delay_timeout; 
        return 0;
      }
    }
    else {
      is_ack=0;

      if( ethhdr_charon->eth_type == htons( ETH_CHARON_TYPE ) ) {
        memcpy( &in_pid, &(charonframe->first_four), 4 );

        is_accept=1;
        if( memcmp( &(ethhdr_charon->dst_mac), mac_all_one, 6) == 0 ) is_broadcast=1;


        if( memcmp( &(ethhdr_charon->src_mac), ofdm0_mac, 6) == 0 ) {
          fprintf(stderr,"\nRF_IN-> dropped (srcmac==ofdm0_mac)");
          return 0;
        }

        if( !is_broadcast && memcmp( &(ethhdr_charon->dst_mac), ofdm0_mac, 6) != 0 ) {
          fprintf(stderr,"\nRF_IN-> not-acking. drop (dstmac!=ofdm0_mac)");
          if(tcp_share_backoff<max_tcp_share_backoff) tcp_share_backoff += symbol_delay_timeout;
          return 0;
        }

        memcpy( ack_mac, &(ethhdr_charon->src_mac), 6);

      }
      else {
        return 0;
      }
    }

  }

  if(_q == NULL) {
    _q = fs;
    _qq = _q->fs;
  }


  if(time_start==0) {
    time_start = time(NULL);
  }
  else {
    time_secs = time(NULL)-time_start;
  }

  if(_header_valid && !_payload_valid) {

    //ths seems to be working well
    if(_stats.rssi > -25) {
      pluto_bump_agc_down( (-(_stats.rssi+25)) / 3);
    }
    else if(_stats.rssi < -30) {
      pluto_bump_agc_up( (-(_stats.rssi+30)) / 3);
    }
  }

  if(_header_valid && _payload_valid && is_accept) {

    if(first_rx_after_agc_reset) {
      first_rx_after_agc_reset=0;
      rx_gain = pluto_get_in_gain();
      pluto_set_in_gain( pluto_get_in_gain() + 15 );
    }

    if(!is_ack) {
      frame_len = _payload_len - charon_added_length;
      memcpy(frame_buffer, &_payload[charon_added_length], frame_len);
    }

    rx_gain = pluto_get_in_gain();
    rssi_dbm = (float) -rx_gain + _stats.rssi + 12.0f -4.7;  //12dB for OFDM crest factor @ p = 10e-7,  -4.7 dB for QAM crest

    reset_agc_timer(); 

    //ths seems to be working well
    if( _stats.evm > -28 ) {
      if(_stats.rssi > -20 && rx_gain > 12) {
        pluto_bump_agc_down(-(_stats.rssi+25)/3);
      }
      else if(_stats.rssi < -26 && rx_gain <73) {
        pluto_bump_agc_up(-(_stats.rssi+26)/3);
      }
    }

    //we didn't transmit this and it is an ack frame,  cancel tx retries
    if( is_ack ) {
      last_good_nco_freq = nco_crcf_get_frequency(_qq->nco_rx);
      fprintf(stderr, "\nRF_IN-> GOT ACK, %02x:%02x:%02x:%02x:%02x:%02x",
          rec_ack[0],
          rec_ack[1],
          rec_ack[2],
          rec_ack[3],
          rec_ack[4],
          rec_ack[5]
        );
      do_got_ack();
      return 0;
    }

    if(!is_ack ) { 

      if( !is_broadcast && is_dup(in_pid) ) {
        fprintf(stderr, "\nRF_IN->dropping duplicate.  sending ACK, gain: %lld", rx_gain);
        last_good_nco_freq = nco_crcf_get_frequency(_qq->nco_rx);
        if(!is_broadcast) do_send_ack(ack_mac);
        return 0;
      }
      
      if(!is_broadcast) add_dup(in_pid);
      if(!is_broadcast) do_send_ack(ack_mac);

      if( write_tap_dev(frame_buffer, frame_len) == frame_len) {
        fprintf(stderr, "\nRF_IN->TAPDEV_OUT_, len=%d, rssi: %3.1f dBm, rx EVM %3.1f dB, in-gain: %lld dB, d-rate %d Kbps, ", frame_len, rssi_dbm, _stats.evm, rx_gain, data_rate_kbps);
        fprintf(stderr,"#sub-carriers=%d, mod: %s", OFDM_M, modulation_types[_stats.mod_scheme].name);
        last_good_nco_freq = nco_crcf_get_frequency(_qq->nco_rx);
      }
      rx_timeout = RX_TIMEOUT;
    }


    did_rx_ok=1;
    total_good_frames++;
    total_bytes += frame_len;

    drate += _stats.num_framesyms * _stats.mod_bps;
    kbit_bytes += frame_len; 
    gettimeofday(&end, NULL);

    if(kbit_count++==4 ) {
      kbps = (long long) ((kbit_bytes*8L*1e3L)/ ( (end.tv_sec*1e6+end.tv_usec) - (start.tv_sec*1e6+start.tv_usec)) );
      drate_bps = (long long) ((drate*1e3L)/ ( (end.tv_sec*1e6+end.tv_usec) - (start.tv_sec*1e6+start.tv_usec)) );
      
      gettimeofday(&start, NULL);
      kbit_count=0;
      kbit_bytes=0;
      drate = 0;
    }

  }
  else {
    nco_crcf_set_frequency(_qq->nco_rx, last_good_nco_freq);

    did_rx_ok=0;
    total_bad_frames++;
    if(time(NULL)%4==0) {
      kbps=0;
      drate_bps=0;
    }
  }

  if(total_good_frames>0) per = (float) ( (float) total_bad_frames / (float) (total_good_frames+total_bad_frames)) * 100.0f;

  return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void init_ofdm_rx(void) {

  ofdmframe_init_default_sctype(M, p);  //default pilot/guard-nulls allocation

  fs = ofdmflexframesync_create(M, cp_len, taper_len, p, ofdm_rx_callback, userdata);

  ofdmflexframesync_print(fs);

  ofdm_nco = nco_crcf_create(LIQUID_NCO);
  nco_crcf_set_frequency(ofdm_nco, 0); 
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void do_ofdm_rx(float complex sample) {
  ofdmflexframesync_execute(fs, &sample, 1);
}
