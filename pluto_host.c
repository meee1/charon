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

// Host version - uses libiio network context to a remote PlutoSDR

#include <iio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <complex.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <sys/time.h>

#include "liquid/liquid.h"
#include "filters/pluto/pluto_filters.h"
#include "ad9361.h"

#include "pluto.h"
#include "charon.h"
#include "ofdm_conf.h"
#include "ofdm_tx.h"
#include "timers.h"
#include "config.h"

#define XO_BASE_HZ 40000000LL
#define DEFAULT_PLUTO_URI "ip:192.168.2.1"

struct iio_buffer {
    const struct iio_device *dev;
    void *buffer, *userdata;
    size_t length, data_length;

    uint32_t *mask;
    unsigned int dev_sample_size;
    unsigned int sample_size;
    bool is_output, dev_is_high_speed;
};

extern char *pluto_uri;

static struct iio_device *phy;
static struct iio_device *tx_dev;
static struct iio_device *rx_dev;
static struct iio_channel *tx0_i, *tx0_q;
static struct iio_channel *rx0_i, *rx0_q;

// cached phy channel pointers (avoid repeated iio_device_find_channel lookups)
static struct iio_channel *phy_voltage0_in;   // phy "voltage0" input (RX)
static struct iio_channel *phy_voltage0_out;  // phy "voltage0" output (TX)
static struct iio_channel *phy_altvoltage0;   // phy "altvoltage0" output (RX LO)
static struct iio_channel *phy_altvoltage1;   // phy "altvoltage1" output (TX LO)

static struct iio_buffer *rxbuf;
static void *p_dat, *p_end;
static ptrdiff_t p_inc;
static int pluto_rx_initialized=0;

static struct iio_buffer *txbuf;
static char *tx_p_dat, *tx_p_end;
static ptrdiff_t tx_p_inc;
static int pluto_tx_initialized=0;

static int tx_enabled=0;

static int llen;
static int ii=0;
static int n_rx;

static long long prev_gain;

int rx_timeout;
static struct iio_context *ctx;
static long long rssi;
static int more_data;
static int more_tx_data;

long long pluto_current_gain;
long long current_rx_freq;
long long current_sample_freq;

static long long current_xo_correction = XO_BASE_HZ;

static FILE *tx_save_fp = NULL;
static FILE *rx_load_fp = NULL;

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
struct iio_context * pluto_init_txrx() {

    const char *uri = NULL;

    // priority: --uri command line, then PLUTO_URI env var, then default
    if (pluto_uri && pluto_uri[0]) {
        uri = pluto_uri;
    } else {
        uri = getenv("PLUTO_URI");
        if (!uri || !uri[0]) {
            uri = DEFAULT_PLUTO_URI;
        }
    }

    fprintf(stderr, "\n[pluto-host] creating network IIO context: %s ...", uri);
    ctx = iio_create_context_from_uri(uri);
    if (!ctx) {
        fprintf(stderr, "\n[pluto-host] ERROR: failed to create IIO context for %s", uri);
        fprintf(stderr, "\n[pluto-host] make sure PlutoSDR is reachable and iio daemon is running");
        exit(1);
    } else {
        fprintf(stderr, "\n[pluto-host] IIO context created successfully");
    }

    phy = iio_context_find_device(ctx, "ad9361-phy");
    fprintf(stderr, "\n[pluto-host] ad9361-phy device: %p", (void*)phy);

    // cache phy channel pointers once at init
    phy_voltage0_in  = iio_device_find_channel(phy, "voltage0", false);
    phy_voltage0_out = iio_device_find_channel(phy, "voltage0", true);
    phy_altvoltage0  = iio_device_find_channel(phy, "altvoltage0", true);
    phy_altvoltage1  = iio_device_find_channel(phy, "altvoltage1", true);

    tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    rx_dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    fprintf(stderr, "\n[pluto-host] tx_dev: %p, rx_dev: %p", (void*)tx_dev, (void*)rx_dev);

    rx0_i = iio_device_find_channel(rx_dev, "voltage0", 0);
    rx0_q = iio_device_find_channel(rx_dev, "voltage1", 0);
    fprintf(stderr, "\n[pluto-host] RX channels: I=%p Q=%p", (void*)rx0_i, (void*)rx0_q);
    iio_channel_enable(rx0_i);
    iio_channel_enable(rx0_q);

    tx0_i = iio_device_find_channel(tx_dev, "voltage0", 1);
    tx0_q = iio_device_find_channel(tx_dev, "voltage1", 1);
    fprintf(stderr, "\n[pluto-host] TX channels: I=%p Q=%p", (void*)tx0_i, (void*)tx0_q);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    {
      unsigned long rate = (unsigned long)sample_freq_hz;
      unsigned long fpass = rate;            // passband edge at full sample rate
      unsigned long fstop = fpass * 5 / 4;  // stopband 25% beyond passband
      unsigned long wnom = rate;             // analog filter at full sample rate
      fprintf(stderr, "\n[pluto-host] setting bb rate custom filter manual: rate=%lu Fpass=%lu Fstop=%lu wnom_tx=%lu wnom_rx=%lu",
              rate, fpass, fstop, wnom, wnom);
      ad9361_set_bb_rate_custom_filter_manual(phy, rate, fpass, fstop, wnom, wnom);
    }

    {
      long long hw_sfreq = 0, hw_rx_bw = 0, hw_tx_bw = 0, hw_rx_lo = 0, hw_tx_lo = 0;
      iio_channel_attr_read_longlong(phy_voltage0_in, "sampling_frequency", &hw_sfreq);
      iio_channel_attr_read_longlong(phy_voltage0_in, "rf_bandwidth", &hw_rx_bw);
      iio_channel_attr_read_longlong(phy_voltage0_out, "rf_bandwidth", &hw_tx_bw);
      iio_channel_attr_read_longlong(phy_altvoltage0, "frequency", &hw_rx_lo);
      iio_channel_attr_read_longlong(phy_altvoltage1, "frequency", &hw_tx_lo);
      fprintf(stderr, "\n[pluto-host] HW sample rate: %lld Hz", hw_sfreq);
      fprintf(stderr, "\n[pluto-host] HW RX bandwidth: %lld Hz, TX bandwidth: %lld Hz", hw_rx_bw, hw_tx_bw);
      fprintf(stderr, "\n[pluto-host] HW RX LO: %lld Hz, TX LO: %lld Hz", hw_rx_lo, hw_tx_lo);
    }

    pluto_init_xo_correction();

    fprintf(stderr, "\n[pluto-host] setting initial TX gain to -80 dB");
    pluto_set_out_gain( -80 );

    iio_channel_attr_write(
        phy_voltage0_in,
        "gain_control_mode",
        "fast_attack");

    iio_channel_attr_write_longlong(
        phy_voltage0_in,
        "hardwaregain",
        73);


    //RX Buffer
    if(!pluto_rx_initialized) {
      fprintf(stderr, "\n[pluto-host] creating RX buffer (1400 samples)...");
      rxbuf = iio_device_create_buffer(rx_dev, 1400, false);


      if (!rxbuf) {
          perror("Could not create RX buffer");
          exit(0);
      }

      pluto_rx_initialized=1;
      iio_buffer_set_blocking_mode(rxbuf,false);

      p_inc = iio_buffer_step(rxbuf);
      fprintf(stderr, "\n[pluto-host] RX buffer created, step=%td", p_inc);
     }

    //TX Buffer
    if(!pluto_tx_initialized) {

      fprintf(stderr, "\n[pluto-host] creating TX buffer (%d samples)...", (OFDM_M+CP_LEN+TAPER_LEN)*22);
      txbuf = iio_device_create_buffer(tx_dev, (OFDM_M+CP_LEN+TAPER_LEN)*22, false);

      if (!txbuf) {
          perror("Could not create TX buffer");
          exit(0);
      }

      iio_buffer_set_blocking_mode(txbuf,true);
      pluto_tx_initialized=1;

      tx_p_inc = iio_buffer_step(txbuf);  //no need to init this every loop
      fprintf(stderr, "\n[pluto-host] TX buffer created, step=%td", tx_p_inc);

    }

    fprintf(stderr, "\n[pluto-host] init complete (remote SDR at %s)", uri);

    return ctx;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_filter() {
  fprintf(stderr, "\n[pluto-host] loading FIR filter (%d bytes)", LTE1p4_MHz_ftr_len);
  iio_device_attr_write_raw( phy,
        "filter_fir_config",
        LTE1p4_MHz_ftr,
        LTE1p4_MHz_ftr_len);
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_enable_fir(int enable) {
  fprintf(stderr, "\n[pluto-host] FIR enable=%d", enable);
  ad9361_set_trx_fir_enable(phy, enable);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
long long pluto_get_in_gain(void) {

  long long gain_val=72;

    iio_channel_attr_read_longlong(
        phy_voltage0_in,
      "hardwaregain",
      &gain_val);

  return gain_val;
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_gain_auto_fast(void) {

    fprintf(stderr, "\n[pluto-host] setting AGC mode: fast_attack");
    iio_channel_attr_write(
        phy_voltage0_in,
        "gain_control_mode",
        "fast_attack");

}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
long long pluto_get_in_rssi(void) {

  rssi=-110;

    iio_channel_attr_read_longlong(
        phy_voltage0_in,
      "rssi",
      &rssi);

  return -rssi;
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void enable_rx() {

  fprintf(stderr, "\n[pluto-host] enabling RX at %lld Hz", current_rx_freq);
  iio_channel_attr_write_longlong(
      phy_altvoltage0,
      "frequency",
      current_rx_freq);
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void disable_rx() {

  fprintf(stderr, "\n[pluto-host] disabling RX (detuning +20 MHz from %lld Hz)", current_rx_freq);
  prev_gain = pluto_get_in_gain();

  iio_channel_attr_write_longlong(
      phy_altvoltage0,
      "frequency",
      current_rx_freq+20000000 );
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_bump_agc_down(int delta) {
  fprintf(stderr, "\n[pluto-host] bumping AGC down by %d", delta);
  pluto_set_in_gain( pluto_get_in_gain()+delta );
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_bump_agc_up(int delta) {
  fprintf(stderr, "\n[pluto-host] bumping AGC up by %d", delta);
  pluto_set_in_gain( pluto_get_in_gain()+delta );
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_gain(long long gain) {

    if(gain<6) gain = 6;
    if(gain>73) gain = 73;

    fprintf(stderr, "\n[pluto-host] setting RX gain: %lld dB (manual)", gain);
    iio_channel_attr_write(
        phy_voltage0_in,
        "gain_control_mode",
        "manual");

    iio_channel_attr_write_longlong(
        phy_voltage0_in,
        "hardwaregain",
        gain);

}


///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_out_bw(long long chbw) {

    fprintf(stderr, "\n[pluto-host] setting TX bandwidth: %lld Hz", chbw);
    iio_channel_attr_write_longlong(
        phy_voltage0_out,
        "rf_bandwidth",
        chbw);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_bw(long long chbw) {

    fprintf(stderr, "\n[pluto-host] setting RX bandwidth: %lld Hz", chbw);
    iio_channel_attr_write_longlong(
        phy_voltage0_in,
        "rf_bandwidth",
        chbw);

    pluto_set_out_bw( chbw );
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_sample_freq(long long sfreq) {

    fprintf(stderr, "\n[pluto-host] setting sample freq: %lld Hz", sfreq);
    iio_channel_attr_write_longlong(
        phy_voltage0_in,
        "sampling_frequency",
        sfreq);

    {
      unsigned long rate = (unsigned long)sfreq;
      unsigned long fpass = rate;
      unsigned long fstop = fpass * 5 / 4;
      unsigned long wnom = rate;
      ad9361_set_bb_rate_custom_filter_manual(phy, rate, fpass, fstop, wnom, wnom);
    }

    current_sample_freq = sfreq;
    fprintf(stderr, "\n[pluto-host] sample freq set to %lld Hz", current_sample_freq);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_out_gain(long long gain) {

  fprintf(stderr, "\n[pluto-host] setting TX gain: %lld dB", gain);
  iio_channel_attr_write_longlong(
      phy_voltage0_out,
      "hardwaregain",
      gain);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_tx_freq(long long freq_tx_hz) {

  //NOTE: offset correction is done in set_rx_freq!!!!

  fprintf(stderr, "\n[pluto-host] setting TX LO freq: %lld Hz", freq_tx_hz);
  iio_channel_attr_write_longlong(
      phy_altvoltage1,
      "frequency",
      (long long)freq_tx_hz);   //tx lo freq

}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_enable_tx( int enable) {
  fprintf(stderr, "\n[pluto-host] TX enable=%d", enable);
  tx_enabled = enable;

  if(tx_enabled==0) {
    fprintf(stderr, "\n[pluto-host] TX disabled, parking TX LO and muting gain");
    pluto_set_tx_freq(5999000000);
    pluto_set_out_gain( -80 );
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_rx_freq(long long freq_rx_hz) {

  int in_range = 0;
  if( freq_rx_hz > 902200000L && freq_rx_hz < 927800000 ) in_range=1;
  if( freq_rx_hz > 2412200000L && freq_rx_hz < 2461800000 ) in_range=1;

  if(!in_range) {
    fprintf(stderr, "\nwarning tx/rx freq out of ISM range. setting to default");
    freq_rx_hz = 915000000;
  }

  long long offset_hz = (long long) ( (freq_rx_hz/1e6) * ref_correction_ppm );
  fprintf(stderr, "\nfreq correction hz:  %lld", offset_hz );

  current_rx_freq = (long long)freq_rx_hz + offset_hz;   //rx lo freq

  iio_channel_attr_read_longlong(
        phy_voltage0_in,
        "sampling_frequency",
        &current_sample_freq);


  iio_channel_attr_write_longlong(
      phy_altvoltage0,
      "frequency",
      current_rx_freq );

  fprintf(stderr, "\nsetting rx_freq %lld", current_rx_freq);
  fprintf(stderr, "\nsetting tx_freq %lld", current_rx_freq);

  if(tx_enabled) {
    pluto_set_tx_freq(current_rx_freq);
  }
  else {
    pluto_set_tx_freq(5999000000);
    pluto_set_out_gain( -80 );
  }

}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_init_xo_correction(void) {
  if (iio_device_attr_read_longlong(phy, "xo_correction", &current_xo_correction) == 0) {
    fprintf(stderr, "\n[pluto-host] initial xo_correction: %lld Hz", current_xo_correction);
  } else {
    current_xo_correction = XO_BASE_HZ;
    fprintf(stderr, "\n[pluto-host] could not read xo_correction, using default %lld Hz", current_xo_correction);
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
long long pluto_get_xo_correction(void) {
  return current_xo_correction;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_xo_correction(long long xo_hz) {
  current_xo_correction = xo_hz;
  iio_device_attr_write_longlong(phy, "xo_correction", xo_hz);
  fprintf(stderr, "\n[pluto-host] set xo_correction: %lld Hz", xo_hz);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_apply_pss_xo_correction(float cfo_cycles_per_sample) {
  double cfo_hz = (double)cfo_cycles_per_sample * (double)sample_freq_hz;
  double xo_delta = cfo_hz * ((double)XO_BASE_HZ / (double)freq_rxtx_hz);
  long long new_xo = current_xo_correction - (long long)round(xo_delta);

  fprintf(stderr, "\n[pluto-host] PSS XO correction: CFO=%.1f Hz, xo_delta=%.1f Hz, new_xo=%lld Hz (was %lld)",
          cfo_hz, xo_delta, new_xo, current_xo_correction);

  pluto_set_xo_correction(new_xo * 0.1 + current_xo_correction * 0.9);  //apply correction with some smoothing
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_tx_save_open(const char *path) {
  if(tx_save_fp) fclose(tx_save_fp);
  tx_save_fp = fopen(path, "wb");
  if(!tx_save_fp) {
    fprintf(stderr, "\n[pluto-host] ERROR: could not open TX save file: %s", path);
  } else {
    fprintf(stderr, "\n[pluto-host] saving TX samples to: %s", path);
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_tx_save_close(void) {
  if(tx_save_fp) {
    fclose(tx_save_fp);
    tx_save_fp = NULL;
    fprintf(stderr, "\n[pluto-host] TX save file closed");
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_rx_load_open(const char *path) {
  if(rx_load_fp) fclose(rx_load_fp);
  rx_load_fp = fopen(path, "rb");
  if(!rx_load_fp) {
    fprintf(stderr, "\n[pluto-host] ERROR: could not open RX load file: %s", path);
  } else {
    fprintf(stderr, "\n[pluto-host] loading RX samples from: %s", path);
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_rx_load_close(void) {
  if(rx_load_fp) {
    fclose(rx_load_fp);
    rx_load_fp = NULL;
    fprintf(stderr, "\n[pluto-host] RX load file closed");
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int pluto_transmit(float complex *buffer, int len, int do_dump_rx, int is_last)
{

    llen = len;
    fprintf(stderr, "\n[pluto-host] TX: len=%d is_last=%d dump_rx=%d", len, is_last, do_dump_rx);

    if(more_tx_data==0) {
      tx_p_dat = (char *) iio_buffer_first(txbuf,tx0_i);
      tx_p_end = (char *) iio_buffer_end(txbuf);
      more_tx_data=1;
    }


    for(ii=0; ii<llen; ii++) {

      ((int16_t*)tx_p_dat)[0] = ((const int16_t) (creal( buffer[ii] )*8192.0));  //scale to work well for OFDM waveforms
      ((int16_t*)tx_p_dat)[1] = ((const int16_t) (cimag( buffer[ii] )*8192.0));

      if(tx_save_fp) {
        fwrite(tx_p_dat, sizeof(int16_t), 2, tx_save_fp);
      }

      tx_p_dat += tx_p_inc;

      if(tx_p_dat == tx_p_end) {
        iio_buffer_push(txbuf);
        fprintf(stderr, "\n[pluto-host] TX: pushing intermitant buffer");
        tx_p_dat = (char *) iio_buffer_first(txbuf,tx0_i);
        tx_p_end = (char *) iio_buffer_end(txbuf);
        more_tx_data=1;
      }

    }

    if(is_last) {

      //zero pad the remaining buffer before final push
      fprintf(stderr, "\n[pluto-host] TX: zero padding final buffer, %td bytes to end of buffer", (tx_p_end - tx_p_dat)/tx_p_inc);
      while(tx_p_dat != tx_p_end) {
        ((int16_t*)tx_p_dat)[0] = 0;
        ((int16_t*)tx_p_dat)[1] = 0;
        tx_p_dat += tx_p_inc;
      }

      more_tx_data=0;
      iio_buffer_push(txbuf); //send out the last symbol to the dma
      fprintf(stderr, "\n[pluto-host] TX: pushing final buffer and flushing RX");

      if(tx_save_fp) fflush(tx_save_fp);

      while( iio_buffer_refill(rxbuf) > 0); //flush the rx buffer
    }

  return 0;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int pluto_receive() {

  static long long rx_sample_count = 0;
  static struct timeval rx_stat_tv = {0, 0};

  if(rx_load_fp) {
    int16_t file_buf[1400 * 2];
    size_t samples_read = fread(file_buf, sizeof(int16_t) * 2, 1400, rx_load_fp);
    if(samples_read == 0) {
      fprintf(stderr, "\n[pluto-host] RX load file: EOF reached");
      pluto_rx_load_close();
      return 0;
    }
    do_process_iq16_batch(file_buf, (int)samples_read);
    return 0;
  }

  if(p_dat == p_end || !more_data) {
    ssize_t nbytes = iio_buffer_refill(rxbuf);
    if(nbytes == -EAGAIN || nbytes < 0) return 0;
    n_rx = nbytes/4;
    p_dat = iio_buffer_first(rxbuf,rx0_i);
    p_end = iio_buffer_end(rxbuf);
    more_data=1;
  }

  if(false && p_inc == 4 && n_rx > 0) {
    // fast path: samples are contiguous int16 IQ pairs
    int avail = (p_end - p_dat) / 4;
    if(avail > n_rx) avail = n_rx;
    do_process_iq16_batch((const int16_t*)p_dat, avail);
    p_dat += avail * 4;
    n_rx -= avail;
    if(n_rx == 0) {
      more_data = (p_dat != p_end) ? 1 : 0;
    }
  } else {
    for ( ;p_dat < p_end; p_dat += p_inc) {
      do_process_iq16( ((const int16_t*)p_dat)[0], ((const int16_t*)p_dat)[1] );
      rx_sample_count++;
      if(--n_rx==0) {
        more_data = (p_dat!=p_end) ? 1 : 0;
        break;
      }
    }
  }



  struct timeval now;
  gettimeofday(&now, NULL);
  if(rx_stat_tv.tv_sec == 0) {
    rx_stat_tv = now;
  } else {
    long long elapsed_us = (now.tv_sec - rx_stat_tv.tv_sec) * 1000000LL
                         + (now.tv_usec - rx_stat_tv.tv_usec);
    if(elapsed_us >= 1000000LL) {
      double sps = (double)rx_sample_count / ((double)elapsed_us / 1e6);
      fprintf(stderr, "\n[pluto-host] RX: %.0f samples/sec", sps);
      rx_sample_count = 0;
      rx_stat_tv = now;
    }
  }

  return 0;
}
