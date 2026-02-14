/*
 * OFDM File Transfer Example using liquid-dsp
 * Based on Charon's OFDM implementation
 * 
 * This example demonstrates transmitting a file over OFDM:
 * - Fragments file into OFDM frames
 * - Simulates multi-frame transmission
 * - Reconstructs file at receiver
 * - Calculates throughput and statistics
 * 
 * Compile: gcc -o ofdm_file_transfer ofdm_file_transfer.c -lliquid -lm -lfftw3f
 * Run: ./ofdm_file_transfer [input_file] [output_file]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <time.h>
#include <sys/time.h>
#include <liquid/liquid.h>

// OFDM Configuration (matching Charon)
#define OFDM_M              64
#define CP_LEN              4
#define TAPER_LEN           2
#define OFDM_MODULATION     LIQUID_MODEM_QAM16
#define OFDM_FEC0           LIQUID_FEC_SECDED7264
#define OFDM_FEC1           LIQUID_FEC_HAMMING128
#define OFDM_CRC            LIQUID_CRC_32

// Sample rate (OFDM rate, no oversampling)
#define SAMPLE_RATE_HZ      1400000

// Frame size
#define PAYLOAD_LEN         256  // Match loopback example for testing

// Statistics
typedef struct {
    int frames_sent;
    int frames_received;
    int frames_valid;
    int bytes_sent;
    int bytes_received;
    int byte_errors;
    double start_time;
    double end_time;
    double rssi_sum;
    double evm_sum;
} transfer_stats_t;

// Global state for receiver
typedef struct {
    unsigned char *rx_buffer;
    int rx_buffer_size;
    int rx_buffer_index;
    int frame_count;
    transfer_stats_t *stats;
} rx_state_t;

static rx_state_t rx_state;

/*
 * Get current time in seconds (high precision)
 */
double get_time_sec()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

/*
 * OFDM RX Callback
 */
int ofdm_rx_callback(unsigned char *  _header,
                     int              _header_valid,
                     unsigned char *  _payload,
                     unsigned int     _payload_len,
                     int              _payload_valid,
                     framesyncstats_s _stats,
                     void *           _userdata)
{
    rx_state_t *state = (rx_state_t *)_userdata;
    
    // Debug: callback was triggered
    fprintf(stderr, "[DEBUG] Callback triggered! header_valid=%d, payload_valid=%d, len=%d\n",
            _header_valid, _payload_valid, _payload_len);
    
    // Safety check
    if (!state || !state->stats) {
        fprintf(stderr, "[DEBUG] State or stats is NULL!\n");
        return 0;
    }
    
    state->stats->frames_received++;
    
    if (_header_valid && _payload_valid) {
        state->stats->frames_valid++;
        state->stats->rssi_sum += _stats.rssi;
        state->stats->evm_sum += _stats.evm;
        
        // Extract actual payload length from header
        unsigned int actual_len = (_header[0] << 8) | _header[1];
        if (actual_len > _payload_len) {
            actual_len = _payload_len;
        }
        
        // Copy to receive buffer
        if (state->rx_buffer_index + actual_len <= state->rx_buffer_size) {
            memcpy(state->rx_buffer + state->rx_buffer_index,
                   _payload, actual_len);
            state->rx_buffer_index += actual_len;
            state->stats->bytes_received += actual_len;
        }
        
        state->frame_count++;
        
        // Print progress every 10 frames
        if (state->frame_count % 10 == 0) {
            printf(".");
            fflush(stdout);
        }
        else if (state->frame_count == 1) {
            printf(".");
            fflush(stdout);
        }
    }
    
    return 0;
}

/*
 * Transmit file over OFDM
 */
int transmit_file(const char *filename,
                  ofdmflexframegen fg,
                  float complex **tx_buffer_out,
                  int *tx_buffer_len_out,
                  transfer_stats_t *stats)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
        return -1;
    }
    
    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    printf("\n[TX] File: %s (%ld bytes)\n", filename, file_size);
    
    // Read entire file
    unsigned char *file_data = malloc(file_size);
    if (!file_data) {
        fprintf(stderr, "Error: Cannot allocate file buffer\n");
        fclose(fp);
        return -1;
    }
    
    fread(file_data, 1, file_size, fp);
    fclose(fp);
    
    // Calculate number of frames needed
    int num_frames = (file_size + PAYLOAD_LEN - 1) / PAYLOAD_LEN;
    printf("[TX] Splitting into %d frames of %d bytes each\n", 
           num_frames, PAYLOAD_LEN);
    
    // Estimate total samples needed (increased for safety)
    int samples_per_frame = (OFDM_M + CP_LEN) * num_frames; // Increased estimate
    int total_samples = (num_frames * samples_per_frame); // Extra for padding
    
    float complex *tx_buffer = malloc(total_samples * sizeof(float complex));
    if (!tx_buffer) {
        fprintf(stderr, "Error: Cannot allocate TX buffer\n");
        free(file_data);
        return -1;
    }
    
    int tx_index = 0;
    
    stats->start_time = get_time_sec();
    
    printf("[TX] Transmitting frames: ");
    fflush(stdout);
    
    // Transmit each frame
    for (int frame = 0; frame < num_frames; frame++) {
        unsigned char header[8] = {0};
        unsigned char payload[PAYLOAD_LEN] = {0};
        
        // Calculate payload size for this frame
        int offset = frame * PAYLOAD_LEN;
        int remaining = file_size - offset;
        int payload_size = (remaining < PAYLOAD_LEN) ? remaining : PAYLOAD_LEN;
        
        // Store payload size in header
        header[0] = (payload_size >> 8) & 0xFF;
        header[1] = payload_size & 0xFF;
        
        // Copy data to payload
        memcpy(payload, file_data + offset, payload_size);
        
        // Assemble frame (reset before each frame)
        ofdmflexframegen_reset(fg);
        ofdmflexframegen_assemble(fg, header, payload, PAYLOAD_LEN);
        
        // Generate symbols and output directly
        int last_symbol = 0;
        int symbol_count = 0;
        while (!last_symbol) {
            float complex symbol_buffer[OFDM_M + CP_LEN];
            last_symbol = ofdmflexframegen_write(fg, symbol_buffer, 
                                                 OFDM_M + CP_LEN);
            symbol_count++;
            
            ofdmflexframegen_print(fg);

            for (int i = 0; i < (OFDM_M + CP_LEN); i++) {
                if (tx_index < total_samples) {
                    tx_buffer[tx_index++] = symbol_buffer[i];
                }
            }
        }
        
        fprintf(stderr, "[DEBUG] Frame %d: Generated %d symbols\n", frame, symbol_count);
        
        stats->frames_sent++;
        stats->bytes_sent += payload_size;
        
        if (frame % 10 == 0) {
            printf("#");
            fflush(stdout);
        }
    }
    
    printf(" Done!\n");
    
    stats->end_time = get_time_sec();
    
    free(file_data);
    
    *tx_buffer_out = tx_buffer;
    *tx_buffer_len_out = tx_index;
    
    printf("[TX] Generated %d samples (%.2f ms)\n", 
           tx_index,
           (stats->end_time - stats->start_time) * 1000.0);

    
    return 0;
}

/*
 * Receive and reconstruct file
 */
int receive_file(float complex *tx_buffer,
                 int tx_buffer_len,
                 ofdmflexframesync fs,
                 const char *output_file,
                 transfer_stats_t *stats)
{
    printf("\n[RX] Processing received samples: ");
    fflush(stdout);
    
    // Reset frame sync
    ofdmflexframesync_reset(fs);
    
    // Allocate receive buffer (must be large enough for entire file)
    rx_state.rx_buffer = malloc(stats->bytes_sent + 1024);
    rx_state.rx_buffer_size = stats->bytes_sent + 1024;
    rx_state.rx_buffer_index = 0;
    rx_state.frame_count = 0;
    rx_state.stats = stats;
    
    if (!rx_state.rx_buffer) {
        fprintf(stderr, "Error: Cannot allocate RX buffer\n");
        return -1;
    }
    
    stats->start_time = get_time_sec();
    
    // Feed samples directly to frame synchronizer
    for (int i = 0; i < tx_buffer_len; i++) {
        ofdmflexframesync_execute(fs, &tx_buffer[i], 1);
    }
    
    fprintf(stderr, "\n[DEBUG] Processed %d samples\n", tx_buffer_len);
    
    stats->end_time = get_time_sec();
    
    printf(" Done!\n");
    
    // Write received file
    if (rx_state.rx_buffer_index > 0 && output_file) {
        FILE *fp = fopen(output_file, "wb");
        if (fp) {
            fwrite(rx_state.rx_buffer, 1, rx_state.rx_buffer_index, fp);
            fclose(fp);
            printf("[RX] Wrote %d bytes to '%s'\n", 
                   rx_state.rx_buffer_index, output_file);
        }
    }
    
    return 0;
}

/*
 * Main application
 */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <input_file> [output_file]\n", argv[0]);
        printf("Example: %s test.txt received.txt\n", argv[0]);
        return 1;
    }
    
    const char *input_file = argv[1];
    const char *output_file = (argc >= 3) ? argv[2] : "received.dat";
    
    // Initialize rx_state
    memset(&rx_state, 0, sizeof(rx_state_t));
    
    printf("OFDM File Transfer Example\n");
    printf("===========================\n");
    printf("Configuration:\n");
    printf("  Sample Rate: %.2f MHz\n", SAMPLE_RATE_HZ / 1e6);
    printf("  OFDM Subcarriers: %d\n", OFDM_M);
    printf("  Modulation: QAM-16\n");
    printf("  FEC: SECDED7264 + HAMMING128\n");
    printf("  Frame Size: %d bytes\n\n", PAYLOAD_LEN);
    
    // Initialize statistics
    transfer_stats_t stats = {0};
    
    // ===========================
    // TX Setup
    // ===========================
    
    unsigned char p_tx[OFDM_M];
    ofdmframe_init_default_sctype(OFDM_M, p_tx);
    
    ofdmflexframegenprops_s fgprops;
    ofdmflexframegenprops_init_default(&fgprops);
    fgprops.check = OFDM_CRC;
    fgprops.fec0 = OFDM_FEC0;
    fgprops.fec1 = OFDM_FEC1;
    fgprops.mod_scheme = OFDM_MODULATION;
    
    ofdmflexframegen fg = ofdmflexframegen_create(OFDM_M, CP_LEN, TAPER_LEN,
                                                   p_tx, &fgprops);
    
    // ===========================
    // RX Setup
    // ===========================
    
    unsigned char p_rx[OFDM_M];
    ofdmframe_init_default_sctype(OFDM_M, p_rx);
    
    ofdmflexframesync fs = ofdmflexframesync_create(OFDM_M, CP_LEN, TAPER_LEN,
                                                     p_rx, ofdm_rx_callback,
                                                     &rx_state);
    
    printf("\n[RX] OFDM Frame Synchronizer created\n");
    ofdmflexframesync_print(fs);
    
    // ===========================
    // Transmit File
    // ===========================
    
    float complex *tx_buffer = NULL;
    int tx_buffer_len = 0;
    
    if (transmit_file(input_file, fg, &tx_buffer, 
                     &tx_buffer_len, &stats) < 0) {
        return 1;
    }
    
    // ===========================
    // Simulate Channel with Noise
    // ===========================
    
    printf("\n[Channel] Loopback (no noise for testing)...\n");
    float noise_floor = -80.0f; // Minimal noise
    float apply_noise = 1; // Disable noise for now
    if (apply_noise) {
    for (int i = 0; i < tx_buffer_len; i++) {
        float n_re = randnf() * powf(10.0f, noise_floor / 20.0f);
        float n_im = randnf() * powf(10.0f, noise_floor / 20.0f);
        tx_buffer[i] = tx_buffer[i] + (n_re + _Complex_I * n_im);
    }
    }
    
    // ===========================
    // Receive File
    // ===========================
    
    if (receive_file(tx_buffer, tx_buffer_len, fs, 
                    output_file, &stats) < 0) {
        free(tx_buffer);
        return 1;
    }
    
    // ===========================
    // Display Statistics
    // ===========================
    
    printf("\n===========================\n");
    printf("Transfer Statistics:\n");
    printf("===========================\n");
    
    printf("Frames:\n");
    printf("  Sent: %d\n", stats.frames_sent);
    printf("  Received: %d\n", stats.frames_received);
    printf("  Valid: %d (%.1f%%)\n", stats.frames_valid,
           100.0 * stats.frames_valid / (float)stats.frames_sent);
    
    printf("\nData:\n");
    printf("  Bytes sent: %d\n", stats.bytes_sent);
    printf("  Bytes received: %d\n", stats.bytes_received);
    printf("  Success rate: %.2f%%\n",
           100.0 * stats.bytes_received / (float)stats.bytes_sent);
    
    if (stats.frames_valid > 0) {
        printf("\nSignal Quality:\n");
        printf("  Avg RSSI: %.2f dB\n", stats.rssi_sum / stats.frames_valid);
        printf("  Avg EVM: %.2f dB\n", stats.evm_sum / stats.frames_valid);
    }
    
    double tx_time = stats.end_time - stats.start_time;
    printf("\nThroughput:\n");
    printf("  TX time: %.3f seconds\n", tx_time);
    printf("  Data rate: %.2f kbps\n", (stats.bytes_sent * 8) / (tx_time * 1000));
    printf("  Efficiency: %.1f%% (of 272 kbps theoretical)\n",
           100.0 * (stats.bytes_sent * 8) / (tx_time * 1000) / 272.0);
    
    // ===========================
    // Verify File Integrity
    // ===========================
    
    printf("\n===========================\n");
    
    if (stats.bytes_received == stats.bytes_sent) {
        printf("✓ Transfer completed successfully!\n");
        printf("  Input:  %s\n", input_file);
        printf("  Output: %s\n", output_file);
    } else {
        printf("✗ Transfer incomplete or corrupted\n");
        printf("  Expected: %d bytes\n", stats.bytes_sent);
        printf("  Received: %d bytes\n", stats.bytes_received);
    }
    
    // ===========================
    // Cleanup
    // ===========================
    
    free(tx_buffer);
    if (rx_state.rx_buffer) {
        free(rx_state.rx_buffer);
    }
    
    ofdmflexframegen_destroy(fg);
    ofdmflexframesync_destroy(fs);
    
    return (stats.bytes_received == stats.bytes_sent) ? 0 : 1;
}
