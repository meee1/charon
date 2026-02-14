/*
 * OFDM Loopback Example using liquid-dsp
 * Based on Charon's OFDM implementation
 * 
 * This example demonstrates:
 * - OFDM frame generation with ofdmflexframegen
 * - OFDM frame synchronization with ofdmflexframesync
 * - Direct sample loopback transmission and reception
 * 
 * Compile: gcc -o ofdm_loopback_example ofdm_loopback_example.c -lliquid -lm -lfftw3f
 * Run: ./ofdm_loopback_example
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <time.h>
#include <liquid/liquid.h>

// OFDM Configuration (matching Charon's settings)
#define OFDM_M              64              // Number of subcarriers
#define CP_LEN              4               // Cyclic prefix length
#define TAPER_LEN           2               // Taper length
#define OFDM_MODULATION     LIQUID_MODEM_QAM16
#define OFDM_FEC0           LIQUID_FEC_SECDED7264
#define OFDM_FEC1           LIQUID_FEC_HAMMING128
#define OFDM_CRC            LIQUID_CRC_32

// Sample rate (OFDM rate, no oversampling)
#define SAMPLE_RATE_HZ      1400000         // 1.4 MHz

// Payload size
#define PAYLOAD_LEN         256             // Bytes

// Global variables for RX callback
static int frame_received = 0;
static unsigned char received_payload[PAYLOAD_LEN];
static int received_payload_len = 0;
static int header_valid = 0;
static int payload_valid = 0;
static framesyncstats_s rx_stats;

/*
 * OFDM RX Callback
 * Called by ofdmflexframesync when a frame is detected
 */
int ofdm_rx_callback(unsigned char *  _header,
                     int              _header_valid,
                     unsigned char *  _payload,
                     unsigned int     _payload_len,
                     int              _payload_valid,
                     framesyncstats_s _stats,
                     void *           _userdata)
{
    frame_received = 1;
    header_valid = _header_valid;
    payload_valid = _payload_valid;
    rx_stats = _stats;
    
    if (_payload_valid && _payload_len <= PAYLOAD_LEN) {
        memcpy(received_payload, _payload, _payload_len);
        received_payload_len = _payload_len;
        
        printf("\n[RX] Frame received!\n");
        printf("     Header valid: %s\n", _header_valid ? "YES" : "NO");
        printf("     Payload valid: %s\n", _payload_valid ? "YES" : "NO");
        printf("     Payload length: %u bytes\n", _payload_len);
        printf("     RSSI: %.2f dB\n", _stats.rssi);
        printf("     EVM: %.2f dB\n", _stats.evm);
        printf("     CFO: %.6f (normalized)\n", _stats.cfo);
    }
    
    return 0;
}

/*
 * Main application
 */
int main(int argc, char *argv[])
{
    printf("OFDM Loopback Example (based on Charon)\n");
    printf("========================================\n");
    printf("Sample Rate: %.2f MHz\n", SAMPLE_RATE_HZ / 1e6);
    printf("OFDM BW: %.2f kHz\n", SAMPLE_RATE_HZ / 1e3);
    printf("Subcarriers: %d\n", OFDM_M);
    printf("Modulation: QAM-16\n");
    printf("FEC: SECDED7264 + HAMMING128\n\n");
    
    // ===========================
    // TX Setup
    // ===========================
    
    // Initialize subcarrier allocation
    unsigned char p_tx[OFDM_M];
    ofdmframe_init_default_sctype(OFDM_M, p_tx);
    
    // Configure frame generator properties
    ofdmflexframegenprops_s fgprops;
    ofdmflexframegenprops_init_default(&fgprops);
    fgprops.check = OFDM_CRC;
    fgprops.fec0 = OFDM_FEC0;
    fgprops.fec1 = OFDM_FEC1;
    fgprops.mod_scheme = OFDM_MODULATION;
    
    // Create OFDM frame generator
    ofdmflexframegen fg = ofdmflexframegen_create(OFDM_M, CP_LEN, TAPER_LEN, 
                                                   p_tx, &fgprops);
    
    printf("[TX] OFDM Frame Generator:\n");
    ofdmflexframegen_print(fg);
    
    // ===========================
    // RX Setup
    // ===========================
    
    // Initialize subcarrier allocation
    unsigned char p_rx[OFDM_M];
    ofdmframe_init_default_sctype(OFDM_M, p_rx);
    
    // Create OFDM frame synchronizer
    ofdmflexframesync fs = ofdmflexframesync_create(OFDM_M, CP_LEN, TAPER_LEN,
                                                     p_rx, ofdm_rx_callback, NULL);
    
    printf("\n[RX] OFDM Frame Synchronizer:\n");
    ofdmflexframesync_print(fs);
    
    // ===========================
    // Prepare Test Payload
    // ===========================
    
    unsigned char header[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    unsigned char payload[PAYLOAD_LEN];
    
    // Fill payload with test pattern
    printf("\n[TX] Preparing test payload...\n");
    for (int i = 0; i < PAYLOAD_LEN; i++) {
        payload[i] = (unsigned char)(i & 0xFF);
    }
    
    // Add some ASCII text at the beginning
    const char *msg = "Hello from Charon OFDM!";
    memcpy(payload, msg, strlen(msg));
    
    printf("[TX] Payload: \"%s\" + %d bytes of test data\n", 
           msg, PAYLOAD_LEN - (int)strlen(msg));
    
    // ===========================
    // Generate OFDM Frame
    // ===========================
    
    printf("\n[TX] Assembling frame...\n");
    ofdmflexframegen_assemble(fg, header, payload, PAYLOAD_LEN);
    
    // Count samples needed
    int sample_count = 0;
    int last_symbol = 0;
    float complex temp_buffer[OFDM_M + CP_LEN];
    
    while (!last_symbol) {
        last_symbol = ofdmflexframegen_write(fg, temp_buffer, OFDM_M + CP_LEN);
        sample_count += (OFDM_M + CP_LEN);
    }
    
    printf("[TX] Frame requires %d symbols (%d samples)\n",
           sample_count / (OFDM_M + CP_LEN), sample_count);
    
    // Allocate buffer for samples
    int tx_buffer_len = sample_count;
    float complex *tx_buffer = malloc(tx_buffer_len * sizeof(float complex));
    
    if (!tx_buffer) {
        fprintf(stderr, "Error: Failed to allocate TX buffer\n");
        return 1;
    }
    
    // Reset frame generator and generate frame
    printf("[TX] Generating frame...\n");
    ofdmflexframegen_reset(fg);
    ofdmflexframegen_assemble(fg, header, payload, PAYLOAD_LEN);
    
    int tx_index = 0;
    last_symbol = 0;
    
    while (!last_symbol) {
        float complex symbol_buffer[OFDM_M + CP_LEN];
        last_symbol = ofdmflexframegen_write(fg, symbol_buffer, OFDM_M + CP_LEN);
        
        // Output samples directly to TX buffer
        for (int i = 0; i < (OFDM_M + CP_LEN); i++) {
            tx_buffer[tx_index++] = symbol_buffer[i];
        }
    }
    
    printf("[TX] Generated %d samples\n", tx_index);
    
    // ===========================
    // Simulate Channel (loopback with optional noise/gain)
    // ===========================
    
    printf("\n[Channel] Simulating loopback with AWGN...\n");
    float noise_floor = -40.0f; // dB
    float gain = 1.0f;
    
    // Add noise and apply gain
    for (int i = 0; i < tx_index; i++) {
        float n_re = randnf() * powf(10.0f, noise_floor / 20.0f);
        float n_im = randnf() * powf(10.0f, noise_floor / 20.0f);
        tx_buffer[i] = gain * tx_buffer[i] + (n_re + _Complex_I * n_im);
    }
    
    // ===========================
    // Receive Samples
    // ===========================
    
    printf("\n[RX] Processing received samples...\n");
    frame_received = 0;
    
    for (int i = 0; i < tx_index; i++) {
        // Feed samples directly to frame synchronizer
        ofdmflexframesync_execute(fs, &tx_buffer[i], 1);
        
        if (frame_received) {
            break; // Frame detected and received
        }
    }
    
    // ===========================
    // Verify Results
    // ===========================
    
    printf("\n========================================\n");
    printf("Results:\n");
    printf("========================================\n");
    
    if (frame_received && payload_valid) {
        printf("✓ Frame successfully received and decoded!\n\n");
        
        // Verify payload integrity
        int errors = 0;
        for (int i = 0; i < received_payload_len; i++) {
            if (received_payload[i] != payload[i]) {
                errors++;
            }
        }
        
        if (errors == 0) {
            printf("✓ Payload integrity: PERFECT (0 byte errors)\n");
            printf("  Received message: \"%s\"\n", received_payload);
        } else {
            printf("✗ Payload integrity: %d byte errors out of %d\n",
                   errors, received_payload_len);
        }
        
        // Calculate theoretical data rate
        int data_subcarriers = 46; // Approximate for 64 subcarriers with pilots/nulls
        int mod_bps = 4; // QAM-16 = 4 bits per symbol
        float symbol_rate = SAMPLE_RATE_HZ / 
                           (float)(OFDM_M + CP_LEN + TAPER_LEN);
        float raw_rate_kbps = (symbol_rate * data_subcarriers * mod_bps) / 1e3;
        
        // Adjust for FEC coding
        float fec_rate = (8.0/12.0) * (64.0/72.0); // HAMMING(8,12) * SECDED(64,72)
        float effective_rate_kbps = raw_rate_kbps * fec_rate;
        
        printf("\nData Rate Analysis:\n");
        printf("  Symbol rate: %.2f kHz\n", symbol_rate / 1e3);
        printf("  Raw data rate: %.2f kbps\n", raw_rate_kbps);
        printf("  Effective rate (with FEC): %.2f kbps\n", effective_rate_kbps);
        
    } else if (frame_received && !payload_valid) {
        printf("✗ Frame detected but payload failed CRC check\n");
        printf("  This indicates channel impairments or errors\n");
    } else {
        printf("✗ No frame detected\n");
        printf("  Frame synchronization may have failed\n");
    }
    
    // ===========================
    // Cleanup
    // ===========================
    
    free(tx_buffer);
    ofdmflexframegen_destroy(fg);
    ofdmflexframesync_destroy(fs);
    
    printf("\n========================================\n");
    printf("Example completed successfully!\n");
    
    return (frame_received && payload_valid) ? 0 : 1;
}
