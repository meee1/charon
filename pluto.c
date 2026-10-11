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

#include <iio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <complex.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <stdbool.h>
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

struct iio_buffer {
    const struct iio_device *dev;
    void *buffer, *userdata;
    size_t length, data_length;

    uint32_t *mask;
    unsigned int dev_sample_size;
    unsigned int sample_size;
    bool is_output, dev_is_high_speed;
};

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
// Read back and log the AD9361 PFIR / clock-chain state so we can confirm what
// libad9361's auto-picker actually selected (notably the PFIR INT/DEC factor,
// which is chosen internally and not visible from the call-site arguments).
static void pluto_log_filter_chain(struct iio_device *phydev) {
    char buf[1024];
    ssize_t n;

    n = iio_device_attr_read(phydev, "filter_fir_config", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        // Header is the first two lines: "TX 3 GAIN x INT N" / "RX 3 GAIN x DEC N".
        int newlines = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' && ++newlines == 2) { buf[i] = '\0'; break; }
        }
        fprintf(stderr, "\n[pluto] FIR config header:\n%s", buf);
    } else {
        fprintf(stderr, "\n[pluto] filter_fir_config read failed: %zd", n);
    }

    n = iio_device_attr_read(phydev, "rx_path_rates", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; fprintf(stderr, "\n[pluto] rx_path_rates: %s", buf); }

    n = iio_device_attr_read(phydev, "tx_path_rates", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; fprintf(stderr, "\n[pluto] tx_path_rates: %s", buf); }

    bool fir_en = false;
    if (iio_device_attr_read_bool(phydev, "in_voltage_filter_fir_en", &fir_en) == 0)
        fprintf(stderr, "\n[pluto] in_voltage_filter_fir_en=%d", (int)fir_en);
    if (iio_device_attr_read_bool(phydev, "out_voltage_filter_fir_en", &fir_en) == 0)
        fprintf(stderr, "\n[pluto] out_voltage_filter_fir_en=%d", (int)fir_en);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
// Enable the AD9361's continuous RX correction loops:
//  - rf_dc_offset_tracking_en: RF DC offset tracking (LO leakage / self-mixing)
//  - bb_dc_offset_tracking_en: baseband DC offset tracking
//  - quadrature_tracking_en:   RX IQ gain/phase imbalance tracking
// These are shared-by-type attrs on the phy "voltage0" input channel
// (sysfs in_voltage_*_tracking_en).  DC at baseband lands on the CFO
// estimator and the OFDM null subcarrier; IQ imbalance creates an image
// that sits on mirror subcarriers and raises EVM.
void pluto_enable_dc_quad_tracking(void) {
  static const char *attrs[] = {
    "rf_dc_offset_tracking_en",
    "bb_dc_offset_tracking_en",
    "quadrature_tracking_en",
  };

  for (unsigned int i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
    int ret = iio_channel_attr_write_bool(phy_voltage0_in, attrs[i], true);
    bool en = false;
    iio_channel_attr_read_bool(phy_voltage0_in, attrs[i], &en);
    if (ret < 0)
      fprintf(stderr, "\n[pluto] ERROR: enabling %s failed: %d", attrs[i], ret);
    fprintf(stderr, "\n[pluto] %s=%d", attrs[i], (int)en);
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
// Run one-shot TX quadrature (IQ imbalance + TX LO leakage) and RF DC offset
// calibrations.  Must be called with the RX/TX LOs at the operating
// frequency: the driver only re-runs these automatically on LO moves
// >100 MHz, and the TX LO is parked at 5.999 GHz before the operating
// frequency is first set.
void pluto_run_dc_quad_calibration(void) {
  int ret;

  fprintf(stderr, "\n[pluto] running TX quadrature calibration...");
  ret = iio_device_attr_write(phy, "calib_mode", "tx_quad");
  if (ret < 0)
    fprintf(stderr, "\n[pluto] ERROR: tx_quad calibration failed: %d", ret);

  fprintf(stderr, "\n[pluto] running RF DC offset calibration...");
  ret = iio_device_attr_write(phy, "calib_mode", "rf_dc_offs");
  if (ret < 0)
    fprintf(stderr, "\n[pluto] ERROR: rf_dc_offs calibration failed: %d", ret);

  // keep the driver re-calibrating on large LO changes
  iio_device_attr_write(phy, "calib_mode", "auto");

  pluto_enable_dc_quad_tracking();
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
struct iio_context * pluto_init_txrx() {

    fprintf(stderr, "\n[pluto] creating local IIO context...");
    ctx = iio_create_local_context();
    if (!ctx) {
        fprintf(stderr, "\n[pluto] ERROR: failed to create local IIO context");
    } else {
        fprintf(stderr, "\n[pluto] IIO context created successfully");
    }

    phy = iio_context_find_device(ctx, "ad9361-phy");
    fprintf(stderr, "\n[pluto] ad9361-phy device: %p", (void*)phy);

    // cache phy channel pointers once at init
    phy_voltage0_in  = iio_device_find_channel(phy, "voltage0", false);
    phy_voltage0_out = iio_device_find_channel(phy, "voltage0", true);
    phy_altvoltage0  = iio_device_find_channel(phy, "altvoltage0", true);
    phy_altvoltage1  = iio_device_find_channel(phy, "altvoltage1", true);

    tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    rx_dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    fprintf(stderr, "\n[pluto] tx_dev: %p, rx_dev: %p", (void*)tx_dev, (void*)rx_dev);

    rx0_i = iio_device_find_channel(rx_dev, "voltage0", 0);
    rx0_q = iio_device_find_channel(rx_dev, "voltage1", 0);
    fprintf(stderr, "\n[pluto] RX channels: I=%p Q=%p", (void*)rx0_i, (void*)rx0_q);
    iio_channel_enable(rx0_i);
    iio_channel_enable(rx0_q);

    tx0_i = iio_device_find_channel(tx_dev, "voltage0", 1);
    tx0_q = iio_device_find_channel(tx_dev, "voltage1", 1);
    fprintf(stderr, "\n[pluto] TX channels: I=%p Q=%p", (void*)tx0_i, (void*)tx0_q);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    {
      unsigned long rate = (unsigned long)sample_freq_hz;
      unsigned long fpass = rate;            // passband edge at full sample rate
      unsigned long fstop = fpass * 5 / 4;  // stopband 25% beyond passband
      unsigned long wnom = rate;             // analog filter at full sample rate
      fprintf(stderr, "\n[pluto] setting bb rate custom filter manual: rate=%lu Fpass=%lu Fstop=%lu wnom_tx=%lu wnom_rx=%lu",
              rate, fpass, fstop, wnom, wnom);
      int ret = ad9361_set_bb_rate_custom_filter_manual(phy, rate, fpass, fstop, wnom, wnom);
      if (ret < 0)
        fprintf(stderr, "\n[pluto] ERROR: ad9361_set_bb_rate_custom_filter_manual failed: %d", ret);
    }

    {
      long long hw_sfreq = 0, hw_rx_bw = 0, hw_tx_bw = 0, hw_rx_lo = 0, hw_tx_lo = 0;
      iio_channel_attr_read_longlong(phy_voltage0_in, "sampling_frequency", &hw_sfreq);
      iio_channel_attr_read_longlong(phy_voltage0_in, "rf_bandwidth", &hw_rx_bw);
      iio_channel_attr_read_longlong(phy_voltage0_out, "rf_bandwidth", &hw_tx_bw);
      iio_channel_attr_read_longlong(phy_altvoltage0, "frequency", &hw_rx_lo);
      iio_channel_attr_read_longlong(phy_altvoltage1, "frequency", &hw_tx_lo);
      fprintf(stderr, "\n[pluto] HW sample rate: %lld Hz", hw_sfreq);
      fprintf(stderr, "\n[pluto] HW RX bandwidth: %lld Hz, TX bandwidth: %lld Hz", hw_rx_bw, hw_tx_bw);
      fprintf(stderr, "\n[pluto] HW RX LO: %lld Hz, TX LO: %lld Hz", hw_rx_lo, hw_tx_lo);
    }

    pluto_log_filter_chain(phy);

    pluto_init_xo_correction();

    pluto_enable_dc_quad_tracking();

    fprintf(stderr, "\n[pluto] setting initial TX gain to -80 dB");
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
      // 8192 samples ≈ 5.9 ms at 1.4 MHz.  Bigger buffers add ACK tail
      // latency (a frame received early in a refill window has to wait
      // for the buffer to drain before the demodulator sees it); smaller
      // buffers raise refill-syscall rate.  At 1.4 MS/s this is ~170
      // refills/sec, which is well under any meaningful overhead, while
      // staying comfortably below the 25 ms default ack_delay_timeout.
      fprintf(stderr, "\n[pluto] creating RX buffer (8192 samples)...");
      rxbuf = iio_device_create_buffer(rx_dev, 8192, false);


      if (!rxbuf) {
          perror("Could not create RX buffer");
          exit(0);
      }

      pluto_rx_initialized=1;
      iio_buffer_set_blocking_mode(rxbuf,false);

      p_inc = iio_buffer_step(rxbuf);
      fprintf(stderr, "\n[pluto] RX buffer created, step=%td", p_inc);
      // pluto_receive's hot path assumes p_inc == 4 (interleaved int16 IQ
      // pairs).  Bail loudly if the kernel ever changes the scan layout
      // rather than silently falling back to a slow scalar path.
      if (p_inc != 4) {
        fprintf(stderr, "\n[pluto] FATAL: unexpected RX buffer step=%td (expected 4)", p_inc);
        exit(1);
      }
     }

    //TX Buffer
    if(!pluto_tx_initialized) {

      fprintf(stderr, "\n[pluto] creating TX buffer (%d samples)...", (OFDM_M+CP_LEN+TAPER_LEN)*22);
      txbuf = iio_device_create_buffer(tx_dev, (OFDM_M+CP_LEN+TAPER_LEN)*22, false);

      if (!txbuf) {
          perror("Could not create TX buffer");
          exit(0);
      }

      iio_buffer_set_blocking_mode(txbuf,true);
      pluto_tx_initialized=1;

      tx_p_inc = iio_buffer_step(txbuf);  //no need to init this every loop
      fprintf(stderr, "\n[pluto] TX buffer created, step=%td", tx_p_inc);

    }

    fprintf(stderr, "\n[pluto] init complete");

    return ctx;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_filter() {
  fprintf(stderr, "\n[pluto] loading FIR filter (%d bytes)", LTE1p4_MHz_ftr_len);
  iio_device_attr_write_raw( phy,
        "filter_fir_config",
        LTE1p4_MHz_ftr,
        LTE1p4_MHz_ftr_len);
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_enable_fir(int enable) {
  fprintf(stderr, "\n[pluto] FIR enable=%d", enable);
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

    fprintf(stderr, "\n[pluto] setting AGC mode: fast_attack");
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

  //fprintf(stderr, "\n[pluto] enabling RX at %lld Hz", current_rx_freq);
  iio_channel_attr_write_longlong(
      phy_altvoltage0,
      "frequency",
      current_rx_freq);
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void disable_rx() {

  //fprintf(stderr, "\n[pluto] disabling RX (detuning +20 MHz from %lld Hz)", current_rx_freq);
  prev_gain = pluto_get_in_gain();

  iio_channel_attr_write_longlong(
      phy_altvoltage0,
      "frequency",
      current_rx_freq+20000000 );
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_bump_agc_down(int delta) {
  fprintf(stderr, "\n[pluto] bumping AGC down by %d", delta);
  pluto_set_in_gain( pluto_get_in_gain()+delta );
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_bump_agc_up(int delta) {
  fprintf(stderr, "\n[pluto] bumping AGC up by %d", delta);
  pluto_set_in_gain( pluto_get_in_gain()+delta );
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_gain(long long gain) {

    if(gain<6) gain = 6;
    if(gain>73) gain = 73;

    fprintf(stderr, "\n[pluto] setting RX gain: %lld dB (manual)", gain);
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

    fprintf(stderr, "\n[pluto] setting TX bandwidth: %lld Hz", chbw);
    iio_channel_attr_write_longlong(
        phy_voltage0_out,
        "rf_bandwidth",
        chbw);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_bw(long long chbw) {

    fprintf(stderr, "\n[pluto] setting RX bandwidth: %lld Hz", chbw);
    iio_channel_attr_write_longlong(
        phy_voltage0_in,
        "rf_bandwidth",
        chbw);

    pluto_set_out_bw( chbw );
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_sample_freq(long long sfreq) {

    fprintf(stderr, "\n[pluto] setting sample freq: %lld Hz", sfreq);
    iio_channel_attr_write_longlong(
        phy_voltage0_in,
        "sampling_frequency",
        sfreq);

    {
      unsigned long rate = (unsigned long)sfreq;
      unsigned long fpass = rate;
      unsigned long fstop = fpass * 5 / 4;
      unsigned long wnom = rate;
      int ret = ad9361_set_bb_rate_custom_filter_manual(phy, rate, fpass, fstop, wnom, wnom);
      if (ret < 0)
        fprintf(stderr, "\n[pluto] ERROR: ad9361_set_bb_rate_custom_filter_manual failed: %d", ret);
    }

    pluto_log_filter_chain(phy);

    current_sample_freq = sfreq;
    fprintf(stderr, "\n[pluto] sample freq set to %lld Hz", current_sample_freq);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_out_gain(long long gain) {

  //fprintf(stderr, "\n[pluto] setting TX gain: %lld dB", gain);
  iio_channel_attr_write_longlong(
      phy_voltage0_out,
      "hardwaregain",
      gain);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_tx_freq(long long freq_tx_hz) {

  //NOTE: offset correction is done in set_rx_freq!!!!

  fprintf(stderr, "\n[pluto] setting TX LO freq: %lld Hz", freq_tx_hz);
  iio_channel_attr_write_longlong(
      phy_altvoltage1,
      "frequency",
      (long long)freq_tx_hz);   //tx lo freq

}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_enable_tx( int enable) {
  fprintf(stderr, "\n[pluto] TX enable=%d", enable);
  tx_enabled = enable;

  if(tx_enabled==0) {
    fprintf(stderr, "\n[pluto] TX disabled, parking TX LO and muting gain");
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

  // LOs are now at the operating frequency; calibrate DC / IQ there.
  pluto_run_dc_quad_calibration();

}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
// Reference-frequency correction.
//
// Primary: AD936x DCXO tuning (dcxo_tune_coarse 0..63, dcxo_tune_fine 0..8191).
// This pulls the crystal itself, so the RF LOs and the BBPLL / sample clock
// all move together (CFO and SFO both corrected) without reprogramming any
// PLL dividers.  Only available when the AD936x drives a crystal; with an
// external reference (stock PlutoSDR: TCXO into XTALN,
// adi,xo-disable-use-ext-refclk-enable) the driver returns -ENODEV and we
// fall back to xo_correction, which tells the driver the reference's true
// frequency and recomputes the LO and BBPLL dividers.
static int dcxo_available = 0;
static long long dcxo_coarse;
static long long dcxo_fine;
static double dcxo_fine_accum;      // fractional fine-code remainder
static double dcxo_gain_sign = 1.0;  // flipped by the divergence guard
static double dcxo_gain_scale = 1.0; // raised by the overshoot guard

#define DCXO_FINE_MAX   8191
#define DCXO_COARSE_MAX 63

void pluto_init_xo_correction(void) {
  if (iio_device_attr_read_longlong(phy, "dcxo_tune_coarse", &dcxo_coarse) == 0 &&
      iio_device_attr_read_longlong(phy, "dcxo_tune_fine", &dcxo_fine) == 0) {
    dcxo_available = 1;
    fprintf(stderr, "\n[pluto] DCXO tuning available: coarse=%lld fine=%lld, %.3f ppb/LSB",
            dcxo_coarse, dcxo_fine, dcxo_ppb_per_lsb);
  } else {
    fprintf(stderr, "\n[pluto] DCXO tuning unavailable (external reference clock), using xo_correction");
  }

  if (iio_device_attr_read_longlong(phy, "xo_correction", &current_xo_correction) == 0) {
    fprintf(stderr, "\n[pluto] initial xo_correction: %lld Hz", current_xo_correction);
  } else {
    current_xo_correction = XO_BASE_HZ;
    fprintf(stderr, "\n[pluto] could not read xo_correction, using default %lld Hz", current_xo_correction);
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
  fprintf(stderr, "\n[pluto] set xo_correction: %lld Hz", xo_hz);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_dcxo(long long coarse, long long fine) {
  if (coarse != dcxo_coarse)
    iio_device_attr_write_longlong(phy, "dcxo_tune_coarse", coarse);
  if (fine != dcxo_fine)
    iio_device_attr_write_longlong(phy, "dcxo_tune_fine", fine);
  fprintf(stderr, "\n[pluto] set DCXO coarse=%lld fine=%lld (was %lld/%lld)",
          coarse, fine, dcxo_coarse, dcxo_fine);
  dcxo_coarse = coarse;
  dcxo_fine = fine;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
// Shift the DCXO by delta_ppm (positive = raise frequency).
//
// The fine-tune gain (dcxo_ppb_per_lsb) and its sign depend on the crystal
// and should be measured per board (see CLAUDE.md).  As a safety net, every
// node runs the same code, so a wrong setting shows up in the CFO between
// nodes after each correction:
//  - wrong sign: CFO keeps growing, or stops shrinking, with the same sign
//                                                        -> flip the sign
//  - gain set too high: CFO overshoots and reverses     -> halve step size
// Each correction is capped so a wrong setting can't throw the CFO outside
// the demodulator's capture range before the guard reacts.
//
// The guard only looks at CFOs well above what normal tracking produces
// (estimator noise, XO drift and the peer's own corrections move the CFO by
// a few hundredths of a ppm between corrections; steady-state error is
// ~0.1 ppm).  Below DCXO_GUARD_MIN_PPM it does nothing -- with a lower bar,
// ordinary drift looked like divergence and flipped a correct sign, sending
// the node off at the 0.25 ppm/correction cap (seen in a long simulator run).
#define DCXO_GUARD_MIN_PPM        0.3
#define DCXO_GUARD_SHRINK_PPM     0.05   // must shrink by at least this per correction
#define DCXO_GUARD_PATIENCE_MIN   3      // strikes before a flip: random in [3,6]
#define DCXO_GUARD_PATIENCE_SPAN  4
#define DCXO_SCALE_RECOVER        10     // clean corrections before undoing a halving
#define DCXO_MAX_STEP_PPM         0.25

static double dcxo_last_cfo_ppm = 0.0;

static void pluto_adjust_dcxo(double delta_ppm) {
  static int sign_strikes = 0;
  static int clean_steps = 0;
  static int patience = 0;

  double cfo_ppm = 2.0 * delta_ppm;   // caller applies half the measured CFO
  int overshoot = 0;
  if (dcxo_last_cfo_ppm != 0.0 && fabs(cfo_ppm) > DCXO_GUARD_MIN_PPM) {
    int same_sign = (cfo_ppm > 0) == (dcxo_last_cfo_ppm > 0);

    // A strike is a correction after which the CFO didn't shrink.  Growing
    // means our sign is wrong.  Standing still means exactly one of the two
    // nodes is wrong and their corrections cancel -- a stable state that a
    // growth-only test never leaves.  Patience is random per decision so
    // the two nodes don't flip in lockstep (which would just swap which
    // one is wrong); whoever flips first fixes it if it was the wrong one,
    // and if it wasn't, the CFO then grows and both flip.
    if (patience == 0)
      patience = DCXO_GUARD_PATIENCE_MIN + (int)(random() % DCXO_GUARD_PATIENCE_SPAN);
    if (same_sign && fabs(cfo_ppm) > fabs(dcxo_last_cfo_ppm) - DCXO_GUARD_SHRINK_PPM)
      sign_strikes++;
    else
      sign_strikes = 0;

    if (sign_strikes >= patience) {
      patience = 0;
      dcxo_gain_sign = -dcxo_gain_sign;
      sign_strikes = 0;
      fprintf(stderr, "\n[pluto] DCXO: CFO diverging (%.3f -> %.3f ppm), flipping tuning sign to %+.0f",
              dcxo_last_cfo_ppm, cfo_ppm, dcxo_gain_sign);
    }
    if (!same_sign && fabs(cfo_ppm) > 0.5 * fabs(dcxo_last_cfo_ppm)) {
      overshoot = 1;
      dcxo_gain_scale *= 2.0;
      fprintf(stderr, "\n[pluto] DCXO: CFO overshot (%.3f -> %.3f ppm), halving step size (scale %.0f)",
              dcxo_last_cfo_ppm, cfo_ppm, dcxo_gain_scale);
    }
  } else {
    sign_strikes = 0;
  }
  // A halving is undone gradually after a run of clean corrections, so one
  // noisy reversal doesn't slow the loop down for good.
  if (overshoot) {
    clean_steps = 0;
  } else if (dcxo_gain_scale > 1.0 && ++clean_steps >= DCXO_SCALE_RECOVER) {
    dcxo_gain_scale /= 2.0;
    clean_steps = 0;
  }
  dcxo_last_cfo_ppm = cfo_ppm;

  if (delta_ppm >  DCXO_MAX_STEP_PPM) delta_ppm =  DCXO_MAX_STEP_PPM;
  if (delta_ppm < -DCXO_MAX_STEP_PPM) delta_ppm = -DCXO_MAX_STEP_PPM;

  dcxo_fine_accum += delta_ppm * 1000.0 / (dcxo_ppb_per_lsb * dcxo_gain_sign * dcxo_gain_scale);
  long long step = (long long)dcxo_fine_accum;   // whole LSBs, remainder carried
  if (step == 0)
    return;
  dcxo_fine_accum -= (double)step;

  long long coarse = dcxo_coarse;
  long long fine = dcxo_fine + step;

  // Fine word ran off either end: step coarse in the same direction and
  // re-centre fine.  The coarse step size depends on the crystal, so this
  // jumps by an unknown amount; the loop pulls it back in afterwards.
  if (fine < 0 || fine > DCXO_FINE_MAX) {
    long long dir = (fine > DCXO_FINE_MAX) ? 1 : -1;
    if (coarse + dir >= 0 && coarse + dir <= DCXO_COARSE_MAX) {
      coarse += dir;
      fine = DCXO_FINE_MAX / 2;
      dcxo_fine_accum = 0.0;
      dcxo_last_cfo_ppm = 0.0;   // the jump is not a divergence signal
      fprintf(stderr, "\n[pluto] DCXO fine out of range, stepping coarse to %lld", coarse);
    } else {
      fine = (fine < 0) ? 0 : DCXO_FINE_MAX;
      fprintf(stderr, "\n[pluto] DCXO at end of tuning range");
    }
  }

  pluto_set_dcxo(coarse, fine);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_apply_pss_xo_correction(float cfo_cycles_per_sample) {
  static struct timeval last_correction_tv;
  static int has_last_correction = 0;

  // Debounce: allow XO to settle before applying another correction
  if (has_last_correction) {
    struct timeval now;
    gettimeofday(&now, NULL);
    long long elapsed_us = (now.tv_sec - last_correction_tv.tv_sec) * 1000000LL
                         + (now.tv_usec - last_correction_tv.tv_usec);
    if (elapsed_us < 500000LL) // 500 ms
      return;
  }

  double cfo_hz = (double)cfo_cycles_per_sample * (double)sample_freq_hz;

  // Reject obviously bogus CFO estimates.  Estimates come from CRC-valid
  // frames only (ofdm_rx.c) and liquid's S0 CFO capture is about +/-22 kHz
  // (pi/32 rad/sample at 1.4 MS/s), so anything beyond 20 kHz is suspect.
  if (fabs(cfo_hz) > 20000.0) {
    fprintf(stderr, "\n[pluto] XO correction: REJECTED CFO=%.1f Hz (exceeds 20 kHz limit)", cfo_hz);
    return;
  }

  // Apply half the correction: when two radios both measure and correct the
  // same CFO simultaneously, each shifting by half ensures the total
  // correction equals the full offset and they converge without oscillating.
  // Positive CFO means the peer is above us, so our reference must go up.
  double cfo_ppm = cfo_hz * 1e6 / (double)freq_rxtx_hz;

  if (dcxo_available) {
    fprintf(stderr, "\n[pluto] DCXO correction: CFO=%.1f Hz (%.4f ppm)", cfo_hz, cfo_ppm);
    pluto_adjust_dcxo(cfo_ppm * 0.5);
  } else {
    double xo_delta = cfo_ppm * 1e-6 * (double)XO_BASE_HZ;
    long long new_xo = current_xo_correction - (long long)round(xo_delta * 0.5);

    fprintf(stderr, "\n[pluto] XO correction: CFO=%.1f Hz, xo_delta=%.1f Hz, new_xo=%lld Hz (was %lld)",
            cfo_hz, xo_delta, new_xo, current_xo_correction);
    // Estimates are averaged over several CRC-valid frames (ofdm_rx.c), so a
    // small deadband is safe: 2 Hz at 40 MHz is 0.05 ppm.
    if (llabs(new_xo - current_xo_correction) >= 2)
      pluto_set_xo_correction(new_xo);
  }
  gettimeofday(&last_correction_tv, NULL);
  has_last_correction = 1;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_tx_save_open(const char *path) {
  if(tx_save_fp) fclose(tx_save_fp);
  tx_save_fp = fopen(path, "wb");
  if(!tx_save_fp) {
    fprintf(stderr, "\n[pluto] ERROR: could not open TX save file: %s", path);
  } else {
    fprintf(stderr, "\n[pluto] saving TX samples to: %s", path);
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_tx_save_close(void) {
  if(tx_save_fp) {
    fclose(tx_save_fp);
    tx_save_fp = NULL;
    fprintf(stderr, "\n[pluto] TX save file closed");
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_rx_load_open(const char *path) {
  if(rx_load_fp) fclose(rx_load_fp);
  rx_load_fp = fopen(path, "rb");
  if(!rx_load_fp) {
    fprintf(stderr, "\n[pluto] ERROR: could not open RX load file: %s", path);
  } else {
    fprintf(stderr, "\n[pluto] loading RX samples from: %s", path);
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_rx_load_close(void) {
  if(rx_load_fp) {
    fclose(rx_load_fp);
    rx_load_fp = NULL;
    fprintf(stderr, "\n[pluto] RX load file closed");
    exit(0);
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int pluto_transmit(float complex *buffer, int len, int do_dump_rx, int is_last)
{

    llen = len;
    //fprintf(stderr, "\n[pluto] TX: len=%d is_last=%d dump_rx=%d", len, is_last, do_dump_rx);

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
        fprintf(stderr, "\n[pluto] TX: pushing intermitant buffer");
        tx_p_dat = (char *) iio_buffer_first(txbuf,tx0_i);
        tx_p_end = (char *) iio_buffer_end(txbuf);
        more_tx_data=1;
      }

    }
    
    if(is_last) {

      //zero pad the remaining buffer before final push
      //fprintf(stderr, "\n[pluto] TX: zero padding final buffer, %td bytes to end of buffer", (tx_p_end - tx_p_dat)/tx_p_inc);
      while(tx_p_dat != tx_p_end) {
        ((int16_t*)tx_p_dat)[0] = 0;
        ((int16_t*)tx_p_dat)[1] = 0;
        tx_p_dat += tx_p_inc;
      }

      more_tx_data=0;
      iio_buffer_push(txbuf); //send out the last symbol to the dma
      fprintf(stderr, "\n[pluto] TX: pushing final buffer and flushing RX");

      if(tx_save_fp) fflush(tx_save_fp);

      while( iio_buffer_refill(rxbuf) > 0); //flush the rx buffer
    }

  return 0;
}

///////////////////////////////////////////////////////////////////////////////////////
// Flush the partially-filled TX buffer (zero-pad + push) without draining
// the RX buffer.  Used by the RF loopback test where we need the TX DMA to
// finish but must keep coupled-back RX samples for the demodulator.
///////////////////////////////////////////////////////////////////////////////////////
void pluto_transmit_flush_tx(void) {
  if (!more_tx_data)
    return;

  while(tx_p_dat != tx_p_end) {
    ((int16_t*)tx_p_dat)[0] = 0;
    ((int16_t*)tx_p_dat)[1] = 0;
    tx_p_dat += tx_p_inc;
  }

  more_tx_data = 0;
  iio_buffer_push(txbuf);
  fprintf(stderr, "\n[pluto] TX: flush_tx (no RX drain)");

  if(tx_save_fp) fflush(tx_save_fp);
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
      fprintf(stderr, "\n[pluto] RX load file: EOF reached");
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

  // Samples are contiguous int16 IQ pairs (validated at init: p_inc == 4).
  if(n_rx > 0) {
    int avail = (p_end - p_dat) / 4;
    if(avail > n_rx) avail = n_rx;
    rx_sample_count += avail;
    do_process_iq16_batch((const int16_t*)p_dat, avail);
    p_dat += avail * 4;
    n_rx -= avail;
    if(n_rx == 0) {
      more_data = (p_dat != p_end) ? 1 : 0;
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
      fprintf(stderr, "\n[pluto] RX: %.0f samples/sec", sps);
      rx_sample_count = 0;
      rx_stat_tv = now;
    }
  }

  return 0;
}
