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

// Host version - uses TCP sockets instead of PlutoSDR hardware

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <complex.h>
#include <unistd.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
  
#include "liquid/liquid.h"

#include "pluto.h"
#include "charon.h"
#include "ofdm_conf.h"
#include "ofdm_tx.h"
#include "timers.h"
#include "config.h"

// UDP socket configuration
#define TX_DEST_IP "127.0.0.1"  // Destination for TX packets
#define TX_DEST_PORT 5002
#define RX_BIND_IP "127.0.0.1"  // Listen address for RX
#define RX_BIND_PORT 5002

static int tx_sock_fd = -1;
static int rx_sock_fd = -1;
static struct sockaddr_in tx_dest_addr;
static struct sockaddr_in rx_src_addr;
static socklen_t rx_addr_len;
static int tx_dest_valid = 0;

static int pluto_rx_initialized=0;
static int pluto_tx_initialized=0;
static int tx_enabled=0;

static unsigned int M=DECIMATE_INTERPOLATE_FACTOR;
static unsigned int h_len;

static float h[2048];
static firinterp_crcf q;
static firdecim_crcf q_rx;

static float complex x;
static float complex y[DECIMATE_INTERPOLATE_FACTOR];
static int llen;
static int ii=0;
static int jj=0;

static long long agc_gain = 72;
static int agc_locked=0;

int rx_timeout;

long long pluto_current_gain;
long long current_rx_freq;
long long current_sample_freq;

static int16_t rx_buffer[16384];  // Larger buffer for UDP packets
static int rx_buf_fill = 0;
static int ofdm_initialized = 0;

///////////////////////////////////////////////////////////////////////////////////////
// Socket helper functions
///////////////////////////////////////////////////////////////////////////////////////
static int create_rx_udp_socket(const char *ip, int port) {
    int sock_fd;
    struct sockaddr_in addr;
    int opt = 1;

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("UDP socket creation failed");
        return -1;
    }

    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(sock_fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address: %s\n", ip);
        close(sock_fd);
        return -1;
    }
    addr.sin_port = htons(port);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(sock_fd);
        return -1;
    }

    // Set non-blocking
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    return sock_fd;
}

static int create_tx_udp_socket(void) {
    int sock_fd;
    int opt = 1;

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("UDP socket creation failed");
        return -1;
    }

    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(sock_fd);
        return -1;
    }

    // Set non-blocking
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    return sock_fd;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
struct iio_context * pluto_init_txrx() {
    
    fprintf(stderr, "\nInitializing host mode with UDP sockets...");

    // Create TX UDP socket (unbound, for sending only)
    tx_sock_fd = create_tx_udp_socket();
    if (tx_sock_fd < 0) {
        fprintf(stderr, "\nFailed to create TX UDP socket");
        exit(1);
    }

    // Set up TX destination address
    memset(&tx_dest_addr, 0, sizeof(tx_dest_addr));
    tx_dest_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, TX_DEST_IP, &tx_dest_addr.sin_addr) <= 0) {
        fprintf(stderr, "\nInvalid TX destination IP: %s", TX_DEST_IP);
        exit(1);
    }
    tx_dest_addr.sin_port = htons(TX_DEST_PORT);
    tx_dest_valid = 1;
    fprintf(stderr, "\nTX will send to %s:%d", TX_DEST_IP, TX_DEST_PORT);

    // Create RX UDP socket (bound to listen for incoming packets)
    rx_sock_fd = create_rx_udp_socket(RX_BIND_IP, RX_BIND_PORT);
    if (rx_sock_fd < 0) {
        fprintf(stderr, "\nFailed to create RX UDP socket on %s:%d", RX_BIND_IP, RX_BIND_PORT);
        exit(1);
    }
    fprintf(stderr, "\nRX UDP socket listening on %s:%d", RX_BIND_IP, RX_BIND_PORT);

    rx_addr_len = sizeof(rx_src_addr);
    
    // Initialize TX filter
    if(!pluto_tx_initialized) {
        h_len = estimate_req_filter_len((1.0/(DECIMATE_INTERPOLATE_FACTOR*2.0*OFDM_TX_BW_FACTOR)), OFDM_TX_STOP_DB);
        fprintf(stderr, "\ntx h_len: %d", h_len);
        
        liquid_firdes_kaiser(h_len, (1.0/(DECIMATE_INTERPOLATE_FACTOR*2.0*OFDM_TX_BW_FACTOR)), OFDM_TX_STOP_DB, 0.0f, h);
        q = firinterp_crcf_create(DECIMATE_INTERPOLATE_FACTOR, h, h_len);
        
        pluto_tx_initialized = 1;
    }
    
    // Initialize RX filter
    if(!pluto_rx_initialized) {
        h_len = estimate_req_filter_len((1.0/(DECIMATE_INTERPOLATE_FACTOR*2.0*OFDM_RX_BW_FACTOR)), OFDM_RX_STOP_DB);
        fprintf(stderr, "\nrx h_len: %d", h_len);
        
        liquid_firdes_kaiser(h_len, (1.0/(DECIMATE_INTERPOLATE_FACTOR*2.0*OFDM_RX_BW_FACTOR)), OFDM_RX_STOP_DB, 0.0f, h);
        q_rx = firdecim_crcf_create(DECIMATE_INTERPOLATE_FACTOR, h, h_len);
        
        pluto_rx_initialized = 1;
    }
    
    pluto_current_gain = 50;
    current_sample_freq = sample_freq_hz;
    
    ofdm_initialized = 1;
    
    fprintf(stderr, "\nHost mode initialization complete");
    
    return NULL; // No real iio context in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_filter() {
    // No-op in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_enable_fir(int enable) {
    // No-op in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
long long pluto_get_in_gain(void) {
    return pluto_current_gain;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_gain_auto_fast(void) {
    // No-op in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
long long pluto_get_in_rssi(void) {
    return -50; // Simulated RSSI
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void enable_rx() {
    // No-op in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void disable_rx() {
    // No-op in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_bump_agc_down(int delta) {
    pluto_current_gain += delta;
    if (pluto_current_gain > 73) pluto_current_gain = 73;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_bump_agc_up(int delta) {
    pluto_current_gain += delta;
    if (pluto_current_gain > 73) pluto_current_gain = 73;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_gain(long long gain) {
    if(gain < 6) gain = 6;
    if(gain > 73) gain = 73;
    pluto_current_gain = gain;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_out_bw(long long chbw) {
    // No-op in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_bw(long long chbw) {
    // No-op in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_in_sample_freq(long long sfreq) {
    current_sample_freq = sfreq;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_out_gain(long long gain) {
    // No-op in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_tx_freq(long long freq_tx_hz) {
    // No-op in host mode
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_enable_tx(int enable) {
    tx_enabled = enable;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void pluto_set_rx_freq(long long freq_rx_hz) {
    current_rx_freq = freq_rx_hz;
    fprintf(stderr, "\nHost mode: simulating rx_freq %lld", freq_rx_hz);
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int pluto_transmit(float complex *buffer, int len, int do_dump_rx, int is_last)
{
    if (tx_sock_fd < 0) {
        return 0; // Socket not initialized
    }

    // If we don't have a destination yet, can't transmit
    if (!tx_dest_valid) {
        return 0;
    }

    llen = len;

    // Calculate total output samples after interpolation
    int total_samples = llen * DECIMATE_INTERPOLATE_FACTOR;
    static int16_t tx_buffer[16384]; // Large enough buffer for interpolated samples (2 int16 per sample)
    int buf_idx = 0;

    // Interpolate all samples and collect into buffer
    for(ii=0; ii<llen; ii++) {
        x = buffer[ii];

        firinterp_crcf_execute(q, x, y);  // interpolate

        for(jj=0; jj<DECIMATE_INTERPOLATE_FACTOR; jj++) {
            tx_buffer[buf_idx++] = (int16_t)(creal(y[jj]) * 8192.0);
            tx_buffer[buf_idx++] = (int16_t)(cimag(y[jj]) * 8192.0);
        }
    }

    // Send all interpolated data in one UDP packet
    ssize_t sent = sendto(tx_sock_fd, tx_buffer, buf_idx * sizeof(int16_t), 0,
                         (struct sockaddr *)&tx_dest_addr, sizeof(tx_dest_addr));
    if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "\nTX UDP sendto error: %s", strerror(errno));
        }
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int pluto_receive() {
    if (rx_sock_fd < 0) {
        return 0; // Socket not initialized
    }
    
    if (!ofdm_initialized) {
        return 0; // OFDM not ready yet
    }
    
    // Read samples from UDP socket
    ssize_t bytes_read = recvfrom(rx_sock_fd, rx_buffer, sizeof(rx_buffer),
                                  MSG_DONTWAIT, (struct sockaddr *)&rx_src_addr, &rx_addr_len);
    
    if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "\nRX UDP recvfrom error: %s", strerror(errno));
        }
        return 0;
    }
    
    if (bytes_read == 0 || bytes_read < 4) {
        return 0; // Need at least one I/Q pair
    }

    int samples_read = bytes_read / sizeof(int16_t) / 2;  // I/Q pairs
    
    for (int i = 0; i < samples_read; i++) {
        do_process_iq16(rx_buffer[i*2], rx_buffer[i*2 + 1]);
    }
    
    return 0;
}
