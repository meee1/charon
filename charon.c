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

#include <iio.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <complex.h>

#include <unistd.h>
#include <sys/time.h>

#include <getopt.h>

#include "liquid/liquid.h"

#include "pluto.h"
#include "ofdm_conf.h"
#include "ofdm_rx.h"
#include "ofdm_tx.h"
#include "charon.h"

#include "tap_device.h"
#include "fftw3.h"
#include "ofdm.h"
#include "util.h"
#include "config.h"
#include "cw_tone.h"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif



static int do_loopback_test = 0;
static int do_pluto_test = 0;
static char *save_tx_path = NULL;
static char *load_rx_path = NULL;

static int max_retrans;

static float IF;
static float QF;
static float complex sample;

static uint8_t tap_buffer[2342];
static int n;

static int tx_retry;
static timer_obj *ack_timer;
static timer_obj *symbol_timer;
static timer_obj *agc_timer_fast;
static timer_obj *agc_timer_slow;

static int got_ack;
static long long ack_timeout;

static uint8_t is_broadcast;
static uint8_t dst_mac[6];

static long long rssi;
static long long in_gain;

static uint8_t dst_ofdm0_mac[6];
static uint8_t bat_route[32];
static int is_route;
static uint32_t current_pid;
uint8_t mac_to_ack[6];

int first_rx_after_agc_reset;

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int run_loopback_test(void) {
  int t, j;
  int test_sizes[] = {1, 64, 256, 512, 1024, PAYLOAD_LEN};
  int num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);
  int pass_count = 0;
  unsigned char tx_payload[PAYLOAD_LEN];
  int errors;
  int payload_len;

  fprintf(stderr, "Charon OFDM internal loopback test\n");
  fprintf(stderr, "==================================\n");
  fprintf(stderr, "Subcarriers: %d, Modulation: QPSK, CRC: CRC-32\n", OFDM_M);
  fprintf(stderr, "FEC inner: NONE, FEC outer: SECDED7264\n");
  fprintf(stderr, "Cyclic prefix: %d, Taper: %d\n\n", CP_LEN, TAPER_LEN);

  init_ofdm_rx();
  init_ofdm_tx();
  ofdm_rx_set_loopback(1);

  for (t = 0; t < num_tests; t++) {
    payload_len = test_sizes[t];

    for (j = 0; j < payload_len; j++) {
      tx_payload[j] = (unsigned char)(j & 0xFF);
    }

    fprintf(stderr, "Test %d/%d: payload_len=%d ... ", t + 1, num_tests, payload_len);

    loopback_rx_ok = 0;
    loopback_rx_payload_len = 0;

    ofdm_rx_reset();

    ofdm_tx_loopback(tx_payload, payload_len);

    if (loopback_rx_ok && loopback_rx_payload_len == payload_len) {
      errors = 0;
      for (j = 0; j < payload_len; j++) {
        if (loopback_rx_payload[j] != tx_payload[j]) errors++;
      }
      if (errors == 0) {
        fprintf(stderr, "PASS\n");
        pass_count++;
      } else {
        fprintf(stderr, "FAIL (%d byte errors)\n", errors);
      }
    } else {
      fprintf(stderr, "FAIL (frame not received)\n");
    }
  }

  fprintf(stderr, "\n==================================\n");
  fprintf(stderr, "Results: %d/%d tests passed\n", pass_count, num_tests);

  return (pass_count == num_tests) ? 0 : 1;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int run_pluto_test(void) {
  int i;
  int pass_count = 0;
  int num_tests = 5;
  struct iio_context *ctx;
  struct timeval tv_start, tv_now;
  long long elapsed_us;

  long long test_rssi;
  long long test_gain;
  float test_noise_floor;

  fprintf(stderr, "Charon Pluto TX/RX hardware test\n");
  fprintf(stderr, "==================================\n");

  //--- Phase 1: Hardware Init ---
  fprintf(stderr, "\n[1/5] Hardware init ... ");

  read_config();
  ctx = pluto_init_txrx();

  if (!ctx) {
    fprintf(stderr, "FAIL (no IIO context)\n");
    goto test_summary;
  }

  pluto_set_enable_tx(1);
  pluto_set_rx_freq(freq_rxtx_hz);
  init_ofdm_rx();
  init_ofdm_tx();
  pluto_set_out_gain(-80);

  fprintf(stderr, "PASS\n");
  fprintf(stderr, "  Frequency: %lld Hz, Sample rate: %lld Hz\n", freq_rxtx_hz, sample_freq_hz);
  pass_count++;

  //--- Phase 2: TX test ---
  fprintf(stderr, "\n[2/5] TX push test ... ");
  {
    float complex tx_buf[256];
    for (i = 0; i < 256; i++) {
      tx_buf[i] = 1.0f + 0.0f * _Complex_I;
    }

    pluto_set_out_gain(tx_output_power_minus_dbm);
    pluto_transmit(tx_buf, 256, 0, 1);
    pluto_set_out_gain(-80);
  }
  fprintf(stderr, "PASS\n");
  pass_count++;

  //--- Phase 3: RX test ---
  fprintf(stderr, "\n[3/5] RX receive test ... ");

  pluto_set_in_gain(73);
  pluto_set_in_gain_auto_fast();

  test_rssi = -110;
  test_gain = 0;
  test_noise_floor = 0.0f;

  {
    int rx_calls = 0;
    long long min_rssi = 0, max_rssi = -200;
    int report_count = 0;

    gettimeofday(&tv_start, NULL);

    for (;;) {
      pluto_receive();
      rx_calls++;

      if (rx_calls % 64 == 0) {
        test_rssi = pluto_get_in_rssi();
        test_gain = pluto_get_in_gain();
        test_noise_floor = cw_tone_get_noise_floor();

        if (test_rssi < min_rssi) min_rssi = test_rssi;
        if (test_rssi > max_rssi) max_rssi = test_rssi;
      }

      if (rx_calls % 512 == 0) {
        gettimeofday(&tv_now, NULL);
        elapsed_us = (tv_now.tv_sec - tv_start.tv_sec) * 1000000LL
                   + (tv_now.tv_usec - tv_start.tv_usec);

        if (report_count < 4 && elapsed_us > (report_count + 1) * 500000LL) {
          fprintf(stderr, "\n    [%.1fs] RSSI: %lld dB, gain: %lld dB, noise_floor: %.1f dB",
                  (double)elapsed_us / 1e6, test_rssi, test_gain, test_noise_floor);
          report_count++;
        }

        if (elapsed_us >= 2000000LL) break;
      }
    }

    fprintf(stderr, "\n    RX calls: %d, RSSI range: [%lld, %lld]\n", rx_calls, min_rssi, max_rssi);

    if (test_rssi != -110 && rx_calls > 0) {
      fprintf(stderr, "  RX receive test: PASS\n");
      pass_count++;
    } else {
      fprintf(stderr, "  RX receive test: FAIL (RSSI stuck at %lld, rx_calls=%d)\n", test_rssi, rx_calls);
    }
  }

  //--- Phase 4: Coupled TX-RX test ---
  fprintf(stderr, "\n[4/5] Coupled TX-RX test ... ");

  {
    float complex tx_buf[256];
    long long baseline_rssi = -110;
    long long tx_on_rssi = -110;
    long long rssi_delta;
    int j;

    for (i = 0; i < 256; i++) {
      tx_buf[i] = 1.0f + 0.0f * _Complex_I;
    }

    // Measure baseline RSSI with TX muted
    pluto_set_out_gain(-80);
    pluto_set_in_gain(73);
    pluto_set_in_gain_auto_fast();
    usleep(10000);

    {
      long long rssi_sum = 0;
      int rssi_count = 0;
      int rx_calls = 0;

      gettimeofday(&tv_start, NULL);
      for (;;) {
        pluto_receive();
        rx_calls++;

        if (rx_calls % 64 == 0) {
          long long r = pluto_get_in_rssi();
          rssi_sum += r;
          rssi_count++;
        }

        if (rx_calls % 256 == 0) {
          gettimeofday(&tv_now, NULL);
          elapsed_us = (tv_now.tv_sec - tv_start.tv_sec) * 1000000LL
                     + (tv_now.tv_usec - tv_start.tv_usec);
          if (elapsed_us >= 1000000LL) break;
        }
      }
      if (rssi_count > 0) baseline_rssi = rssi_sum / rssi_count;
    }

    fprintf(stderr, "\n    Baseline RSSI (TX off): %lld dB", baseline_rssi);

    // Measure RSSI while transmitting DC tone
    pluto_set_out_gain(tx_output_power_minus_dbm);
    usleep(1000);

    {
      long long rssi_sum = 0;
      int rssi_count = 0;
      int rx_calls = 0;

      gettimeofday(&tv_start, NULL);
      for (;;) {
        // Keep TX active by pushing samples
        pluto_transmit(tx_buf, 256, 0, 0);
        pluto_receive();
        rx_calls++;

        if (rx_calls % 64 == 0) {
          long long r = pluto_get_in_rssi();
          rssi_sum += r;
          rssi_count++;
        }

        if (rx_calls % 256 == 0) {
          gettimeofday(&tv_now, NULL);
          elapsed_us = (tv_now.tv_sec - tv_start.tv_sec) * 1000000LL
                     + (tv_now.tv_usec - tv_start.tv_usec);
          if (elapsed_us >= 1000000LL) break;
        }
      }
      if (rssi_count > 0) tx_on_rssi = rssi_sum / rssi_count;

      // Finalize TX
      for (j = 0; j < 256; j++) tx_buf[j] = 0.0f + 0.0f * _Complex_I;
      pluto_transmit(tx_buf, 256, 0, 1);
    }

    pluto_set_out_gain(-80);

    rssi_delta = tx_on_rssi - baseline_rssi;
    fprintf(stderr, "\n    TX-on RSSI: %lld dB, delta: %lld dB", tx_on_rssi, rssi_delta);

    if (rssi_delta > 3) {
      fprintf(stderr, "\n  Coupled TX-RX test: PASS (delta %lld dB)\n", rssi_delta);
      pass_count++;
    } else {
      fprintf(stderr, "\n  Coupled TX-RX test: FAIL (delta %lld dB, need > 3 dB)\n", rssi_delta);
    }
  }

  //--- Phase 5: OFDM frame TX/RX over RF ---
  fprintf(stderr, "\n[5/5] OFDM frame TX/RX over RF ... ");

  {
    int test_sizes[] = {64, 256, 1024};
    int n_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);
    int ofdm_pass = 0;
    int ofdm_total = n_sizes;
    int t;
    unsigned char tx_payload[PAYLOAD_LEN];

    ofdm_rx_set_loopback(1);

    // Disable CW tone detector — self-coupling DC leakage triggers false
    // detections that corrupt the framesync NCO.  The OFDM framesync finds
    // the preamble on its own without CW-assisted CFO estimation.
    cw_tone_disable();

    // Set RX gain high to pick up TX leakage
    pluto_set_in_gain(73);
    pluto_set_in_gain_auto_fast();
    usleep(10000);

    for (t = 0; t < n_sizes; t++) {
      int payload_len = test_sizes[t];
      int j;

      for (j = 0; j < payload_len; j++)
        tx_payload[j] = (unsigned char)((j + t) & 0xFF);

      loopback_rx_ok = 0;
      loopback_rx_payload_len = 0;

      // Flush stale RX data so the framesync starts clean.
      pluto_receive();
      pluto_receive();
      ofdm_rx_reset();
      ofdm_rx_reset_nco();

      // TX the OFDM frame through the real hardware.
      pluto_set_out_gain(tx_output_power_minus_dbm);
      usleep(100);

      ofdm_tx_loopback_rf(tx_payload, payload_len);

      pluto_set_out_gain(-80);

      // Pump RX to demodulate the coupled-back frame
      {
        int rx_pump_count = 0;
        int saw_non_seekplcp = 0;

        gettimeofday(&tv_start, NULL);
        for (;;) {
          pluto_receive();
          rx_pump_count++;

          if (!saw_non_seekplcp && ofdm_rx_state() != 0) {
            saw_non_seekplcp = 1;
            fprintf(stderr, "\n    [debug] framesync left SEEKPLCP (state=%d) after %d rx pumps", ofdm_rx_state(), rx_pump_count);
          }

          if (loopback_rx_ok)
            break;

          gettimeofday(&tv_now, NULL);
          elapsed_us = (tv_now.tv_sec - tv_start.tv_sec) * 1000000LL
                     + (tv_now.tv_usec - tv_start.tv_usec);
          if (elapsed_us >= 2000000LL)
            break;
        }
        fprintf(stderr, "\n    [debug] rx_pumps=%d, saw_preamble=%d", rx_pump_count, saw_non_seekplcp);
      }

      // Check OFDM payload
      if (loopback_rx_ok && loopback_rx_payload_len == payload_len) {
        int errors = 0;
        for (j = 0; j < payload_len; j++) {
          if (loopback_rx_payload[j] != tx_payload[j]) errors++;
        }
        if (errors == 0) {
          fprintf(stderr, "\n    payload_len=%d: OFDM PASS", payload_len);
          ofdm_pass++;
        } else {
          fprintf(stderr, "\n    payload_len=%d: OFDM FAIL (%d byte errors)", payload_len, errors);
        }
      } else {
        fprintf(stderr, "\n    payload_len=%d: OFDM FAIL (frame not received, rx_ok=%d, rx_len=%d)",
                payload_len, loopback_rx_ok, loopback_rx_payload_len);
      }
    }

    ofdm_rx_set_loopback(0);

    if (ofdm_pass == ofdm_total) {
      fprintf(stderr, "\n  OFDM TX/RX test: PASS (%d/%d)\n", ofdm_pass, ofdm_total);
      pass_count++;
    } else {
      fprintf(stderr, "\n  OFDM TX/RX test: FAIL (%d/%d)\n", ofdm_pass, ofdm_total);
    }
  }

test_summary:
  fprintf(stderr, "\n==================================\n");
  fprintf(stderr, "Results: %d/%d tests passed\n", pass_count, num_tests);

  return (pass_count == num_tests) ? 0 : 1;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int main (int argc, char **argv) {

  int opt;
  static struct option long_options[] = {
    {"loopback-test", no_argument, 0, 'T'},
    {"pluto-test", no_argument, 0, 'P'},
    {"save-tx", required_argument, 0, 's'},
    {"load-rx", required_argument, 0, 'l'},
    {0, 0, 0, 0}
  };

  while ((opt = getopt_long(argc, argv, "TPs:l:", long_options, NULL)) != -1) {
    switch (opt) {
      case 'T':
        do_loopback_test = 1;
        break;
      case 'P':
        do_pluto_test = 1;
        break;
      case 's':
        save_tx_path = optarg;
        break;
      case 'l':
        load_rx_path = optarg;
        break;
    }
  }

  if (do_loopback_test) {
    return run_loopback_test();
  }

  if (do_pluto_test) {
    return run_pluto_test();
  }

  srandom(time(NULL));

  read_config();

  pluto_init_txrx();
  pluto_set_enable_tx( 1 );

  if(save_tx_path) pluto_tx_save_open(save_tx_path);
  if(load_rx_path) pluto_rx_load_open(load_rx_path);

  init_ofdm_rx();
  init_ofdm_tx();

  //pluto_set_in_sample_freq( sample_freq_hz );
  //pluto_set_in_bw( rf_bandwidth );
  //pluto_set_out_bw( rf_bandwidth );

  pluto_set_rx_freq( freq_rxtx_hz );  //tx freq also set here
  pluto_set_out_gain( -80 );

  ack_timer = create_timer();
  timer_reset(ack_timer);

  symbol_timer = create_timer();
  timer_reset(symbol_timer);

  agc_timer_fast = create_timer();
  timer_reset(agc_timer_fast);
  agc_timer_slow = create_timer();
  timer_reset(agc_timer_slow);

  main_loop();
  fprintf(stderr, "\n enter while main_loop");
  while(1) {
    main_loop();
  }

}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void do_got_ack() {
  fprintf(stderr, ", time: %lld usec", timer_elapsed_usec(ack_timer) );
  got_ack=1;
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void do_send_ack(uint8_t *ack_mac) {

  memcpy(mac_to_ack, ack_mac, 6);

  disable_rx();

  pluto_set_out_gain( tx_output_power_minus_dbm );
  usleep(1);

  do_ofdm_tx(NULL, 0, 0, 0, 0, mac_to_ack, 0);

  pluto_set_out_gain( -80 );

  ofdm_rx_apply_pending_xo();  // apply deferred XO correction while stream is idle
  enable_rx();
  ofdm_rx_reset();
  cw_tone_reset();
  usleep(200);

  fprintf(stderr, "\nSENT ACK TO ->%02x:%02x:%02x:%02x:%02x:%02x",
      mac_to_ack[0],
      mac_to_ack[1],
      mac_to_ack[2],
      mac_to_ack[3],
      mac_to_ack[4],
      mac_to_ack[5]
    );
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void do_tx( uint8_t *buff, int len, int is_retrans, uint8_t *dst_mac, uint8_t is_broadcast, uint32_t pid) {


 sprintf(bat_route, "%02x:%02x:%02x:%02x:%02x:%02x",
    dst_mac[0],
    dst_mac[1],
    dst_mac[2],
    dst_mac[3],
    dst_mac[4],
   dst_mac[5] );

 
 if(is_broadcast==0x00) {
   if(!is_retrans) is_route = is_batman_route(bat_route, dst_ofdm0_mac);

   fprintf(stderr, " lookup dst_mac,  %02x:%02x:%02x:%02x:%02x:%02x", 
      dst_ofdm0_mac[0],
      dst_ofdm0_mac[1],
      dst_ofdm0_mac[2],
      dst_ofdm0_mac[3],
      dst_ofdm0_mac[4],
      dst_ofdm0_mac[5] );
   if( !is_route ) {
     fprintf(stderr, "\n, not in batman tables. dropping");
     goto tx_done; 
   } 
 }

 disable_rx();

 pluto_set_out_gain( tx_output_power_minus_dbm );
 usleep(1);

  fprintf(stderr,"\nTAPDEV_IN -> RF_OUT , len=%d", n);
  do_ofdm_tx(buff, len, is_retrans, 1, is_broadcast, dst_ofdm0_mac, pid);
  timer_reset(ack_timer);

  if(tx_retry>0 && !is_broadcast) fprintf(stderr,", tx_retry=%d", (max_retrans+1-tx_retry));

  pluto_set_out_gain( -80 );
  ofdm_rx_apply_pending_xo();  // apply deferred XO correction while stream is idle
  enable_rx();
  ofdm_rx_reset();
  cw_tone_reset();
  usleep(200);


tx_done:
  timer_reset(ack_timer);
  timer_reset(symbol_timer);
  timer_reset(agc_timer_fast);
  timer_reset(agc_timer_slow);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void reset_agc_timer(void) {
  timer_reset(agc_timer_fast);
  timer_reset(agc_timer_slow);
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void lbt_backoff(void) {
  timer_reset(symbol_timer);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void check_agc(void) {

    rssi = pluto_get_in_rssi();
    in_gain = pluto_get_in_gain();

    fprintf(stderr, "\nnoise_floor: %.1f dB, RSSI: %lld, in_gain %lld", cw_tone_get_noise_floor(), rssi, in_gain);

  if( timer_elapsed_usec(agc_timer_fast) > 10000) {

    rssi = pluto_get_in_rssi();
    in_gain = pluto_get_in_gain();


    if( rssi > -30 && in_gain > 16) {
      pluto_bump_agc_down(-4);
      fprintf(stderr, "\ncurrent RSSI: %lld, in_gain %lld", rssi, in_gain);
    }

    timer_reset(agc_timer_fast);
  }

  if( timer_elapsed_usec(agc_timer_slow) > 1000000) {
    if( in_gain < 73) {
      pluto_set_in_gain(73);
      pluto_set_in_gain_auto_fast();

      first_rx_after_agc_reset=1;
    }

    timer_reset(agc_timer_slow);

    //seems like a good timer to put this here
    if(tcp_share_backoff>symbol_delay_timeout) tcp_share_backoff -= symbol_delay_timeout; 
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void main_loop(void) {

  static int rx_mod=0;

  //read, but don't wait around
  pluto_receive();

  //read while receiving
  while( ofdm_rx_state() != OFDMFRAMESYNC_STATE_SEEKPLCP || 
            (tx_retry > 0 && timer_elapsed_usec(ack_timer)<ack_timeout) ) { //make sure we get ACK

    pluto_receive();

    if(rx_mod++%256==0) {
      check_agc();
      timer_reset(symbol_timer);
    }
  } 

  //transmit
  if( ofdm_rx_state() == OFDMFRAMESYNC_STATE_SEEKPLCP) {

    long long now_usec = timer_now_usec();
    long long ack_elapsed = timer_elapsed_since(ack_timer, now_usec);
    long long sym_elapsed = timer_elapsed_since(symbol_timer, now_usec);

    //do retry tx
    if( tx_retry > 0 && got_ack==0) {

        if( ack_elapsed >( ack_timeout ) && sym_elapsed > symbol_delay_timeout) {
          ack_timeout = ack_delay_timeout;
          int backoff_exp = max_retrans - tx_retry;
          ack_timeout += (backoff_exp * backoff_exp) * (int) (symbol_delay_timeout+(random()%symbol_delay_timeout)); //exp backoff with randomness

          do_tx(tap_buffer, n, 1, dst_mac, is_broadcast, current_pid);

          if(tx_retry>0) {
            tx_retry--;
          }
          if(tx_retry==0) {
             n = read_tap_dev(tap_buffer, sizeof(tap_buffer), dst_mac, &is_broadcast);
          }
        }
     }
     else {
       if(got_ack==1) {
         got_ack=0;
         n=0;
         tx_retry=0;
         //timer_reset(symbol_timer);
       }
       if(tx_retry==0) {
         n = read_tap_dev(tap_buffer, sizeof(tap_buffer), dst_mac, &is_broadcast);
       }
     }

    tcp_share_backoff=0; //don't do this for now
    //start a new tx?
    if(n>0 && tx_retry==0 && sym_elapsed>(symbol_delay_timeout+tcp_share_backoff) ) {  //leave some bw.  better sharing of multiple tcp connections

      ack_timeout = ack_delay_timeout;

      current_pid = (random()%ULONG_MAX); //used as unique identifier to allow droppng duplicate receptions of frames from re-transmission
      do_tx(tap_buffer, n, 0, dst_mac, is_broadcast, current_pid);

      if(!is_broadcast) {
        if( n  < 128 ) max_retrans = max_short_retrans;
            else max_retrans = max_long_retrans;

        tx_retry = max_retrans; //num retries 
        got_ack=0;
      }
      else {
        tx_retry=bcast_retrans; 
        got_ack=1;  //don't wait between for reply
      }

    }

  } 

} //main loop



////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void do_process_iq16(const int16_t i, const int16_t q) {

  IF = ((float) i) * (1.0f/32768.0f);
  QF = ((float) q) * (1.0f/32768.0f);
  sample = (float complex) (IF + _Complex_I * QF);

  bump_nco();
  do_ofdm_mix_down(sample, &sample);
  do_ofdm_rx(sample);

}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
#define IQ_BATCH_SIZE 256
static float complex iq_batch[IQ_BATCH_SIZE];

void do_process_iq16_batch(const int16_t *buf, int count) {

  while(count > 0) {
    int n = (count > IQ_BATCH_SIZE) ? IQ_BATCH_SIZE : count;
    int j;

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    float32x4_t scale = vdupq_n_f32(1.0f/32768.0f);
    for(j = 0; j + 3 < n; j += 4) {
      int16x4_t vi = {buf[j*2], buf[(j+1)*2], buf[(j+2)*2], buf[(j+3)*2]};
      int16x4_t vq = {buf[j*2+1], buf[(j+1)*2+1], buf[(j+2)*2+1], buf[(j+3)*2+1]};
      float32x4_t fi = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vi)), scale);
      float32x4_t fq = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vq)), scale);
      float fi_arr[4], fq_arr[4];
      vst1q_f32(fi_arr, fi);
      vst1q_f32(fq_arr, fq);
      iq_batch[j]   = fi_arr[0] + _Complex_I * fq_arr[0];
      iq_batch[j+1] = fi_arr[1] + _Complex_I * fq_arr[1];
      iq_batch[j+2] = fi_arr[2] + _Complex_I * fq_arr[2];
      iq_batch[j+3] = fi_arr[3] + _Complex_I * fq_arr[3];
    }
    for(; j < n; j++) {
      iq_batch[j] = ((float)buf[j*2]) * (1.0f/32768.0f)
                   + _Complex_I * ((float)buf[j*2+1]) * (1.0f/32768.0f);
    }
#else
    for(j = 0; j < n; j++) {
      iq_batch[j] = ((float)buf[j*2]) * (1.0f/32768.0f)
                   + _Complex_I * ((float)buf[j*2+1]) * (1.0f/32768.0f);
    }
#endif

    for(j = 0; j < n; j++) {
      sample = iq_batch[j];
      bump_nco();
      do_ofdm_mix_down(sample, &sample);
      do_ofdm_rx(sample);
    }

    buf += n * 2;
    count -= n;
  }
}
