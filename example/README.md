# Charon OFDM Examples

This directory contains example applications demonstrating Charon's OFDM implementation using liquid-dsp on a native host (non-PlutoSDR) system.

## Overview

These examples show how to use the liquid-dsp library to implement OFDM transceivers based on Charon's configuration. They run on any Linux system with liquid-dsp installed, making it easy to understand and test the OFDM PHY layer without PlutoSDR hardware.

## Prerequisites

### Required Libraries

1. **liquid-dsp** - Digital signal processing library
2. **fftw3f** - Fast Fourier Transform library (float precision)
3. **libm** - Math library (usually included)

### Installation

#### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install liquid-dsp libfftw3-dev
```

#### Building liquid-dsp from source
If liquid-dsp is not available in your package manager:

```bash
git clone https://github.com/jgaeddert/liquid-dsp.git
cd liquid-dsp
./bootstrap.sh
./configure
make
sudo make install
sudo ldconfig
```

## Building the Examples

```bash
cd example/
make
```

This will build all example applications.

## Examples

### 1. ofdm_loopback_example

**Purpose**: Demonstrates complete OFDM TX/RX pipeline with loopback testing

**What it does**:
- Creates an OFDM frame generator (`ofdmflexframegen`)
- Creates an OFDM frame synchronizer (`ofdmflexframesync`)
- Generates a test frame with known payload
- Interpolates samples by 8x (matching Charon's decimation factor)
- Simulates a noisy channel
- Decimates received samples by 8x
- Synchronizes and decodes the frame
- Verifies payload integrity

**Configuration** (matching Charon):
- Sample Rate: 11.2 MHz (hardware rate on PlutoSDR)
- Decimation Factor: 8
- Effective Bandwidth: 140 kHz
- Subcarriers: 64 (OFDM-64)
- Modulation: QAM-16
- FEC: SECDED7264 + HAMMING128
- Data Rate: ~272 kbps (theoretical)

**Run**:
```bash
./ofdm_loopback_example
```

**Expected Output**:
```
OFDM Loopback Example (based on Charon)
========================================
Sample Rate: 11.20 MHz
Decimation Factor: 8
Effective BW: 1400.00 kHz
Subcarriers: 64
Modulation: QAM-16
FEC: SECDED7264 + HAMMING128

[TX] OFDM Frame Generator:
...
[RX] Frame received!
     Header valid: YES
     Payload valid: YES
     Payload length: 256 bytes
     RSSI: -10.23 dB
     EVM: -25.67 dB
     CFO: 0.000012 (normalized)

Results:
========================================
✓ Frame successfully received and decoded!
✓ Payload integrity: PERFECT (0 byte errors)
  Received message: "Hello from Charon OFDM!"
```

### 2. ofdm_file_transfer

**Purpose**: Demonstrates file transfer over OFDM with multi-frame transmission

**What it does**:
- Reads a file from disk
- Fragments it into multiple OFDM frames (1024 bytes per frame)
- Transmits all frames with interpolation
- Simulates a noisy channel
- Receives and decimates samples
- Reassembles frames into complete file
- Calculates throughput and success rate

**Configuration**:
- Same as ofdm_loopback_example
- Frame size: 1024 bytes (larger for efficiency)

**Run**:
```bash
# Create a test file
echo "This is a test file for OFDM transmission!" > test.txt

# Transfer the file
./ofdm_file_transfer test.txt received.txt

# Verify the result
diff test.txt received.txt
```

**Expected Output**:
```
OFDM File Transfer Example
===========================
Configuration:
  Sample Rate: 11.20 MHz
  Decimation: 8x
  OFDM Subcarriers: 64
  Modulation: QAM-16
  FEC: SECDED7264 + HAMMING128
  Frame Size: 1024 bytes

[TX] File: test.txt (43 bytes)
[TX] Splitting into 1 frames of 1024 bytes each
[TX] Transmitting frames: # Done!
[TX] Generated 15232 samples (0.52 ms)

[Channel] Adding AWGN (SNR = 20 dB)...

[RX] Processing received samples: . Done!
[RX] Wrote 43 bytes to 'received.txt'

===========================
Transfer Statistics:
===========================
Frames:
  Sent: 1
  Received: 1
  Valid: 1 (100.0%)

Data:
  Bytes sent: 43
  Bytes received: 43
  Success rate: 100.00%

Signal Quality:
  Avg RSSI: -12.45 dB
  Avg EVM: -23.18 dB

Throughput:
  TX time: 0.001 seconds
  Data rate: 344.00 kbps
  Efficiency: 126.5% (of 272 kbps theoretical)

===========================
✓ Transfer completed successfully!
  Input:  test.txt
  Output: received.txt
```

**Advanced Usage**:
```bash
# Transfer a larger file
./ofdm_file_transfer /bin/ls received_ls
chmod +x received_ls
./received_ls  # Should work if transfer succeeded!

# Transfer an image
./ofdm_file_transfer image.jpg received.jpg
```

## Understanding the Code

### Key Components

#### 1. OFDM Frame Generator (`ofdmflexframegen`)
```c
ofdmflexframegen fg = ofdmflexframegen_create(
    OFDM_M,        // 64 subcarriers
    CP_LEN,        // 4-sample cyclic prefix
    TAPER_LEN,     // 2-sample taper
    p_tx,          // subcarrier allocation
    &fgprops       // FEC, modulation, CRC settings
);
```

#### 2. OFDM Frame Synchronizer (`ofdmflexframesync`)
```c
ofdmflexframesync fs = ofdmflexframesync_create(
    OFDM_M,           // 64 subcarriers
    CP_LEN,           // 4-sample cyclic prefix
    TAPER_LEN,        // 2-sample taper
    p_rx,             // subcarrier allocation
    ofdm_rx_callback, // callback when frame detected
    NULL              // user data
);
```

#### 3. Decimation Filter (`firdecim_crcf`)
```c
// Design lowpass filter for decimation
liquid_firdes_kaiser(h_len, fc, stop_db, 0.0f, h);

// Create decimator (8x)
firdecim_crcf decim = firdecim_crcf_create(8, h, h_len);

// Execute decimation
firdecim_crcf_execute(decim, input_samples, &output_sample);
```

#### 4. Interpolation Filter (`firinterp_crcf`)
```c
// Design lowpass filter for interpolation
liquid_firdes_kaiser(h_len, fc, stop_db, 0.0f, h);

// Create interpolator (8x)
firinterp_crcf interp = firinterp_crcf_create(8, h, h_len);

// Execute interpolation
firinterp_crcf_execute(interp, input_sample, output_samples);
```

### Signal Flow

```
TX Path:
  Payload → OFDM Frame Gen → OFDM Symbols → Interpolator (8x) → RF Samples
           (ofdmflexframegen)            (firinterp_crcf)

RX Path:
  RF Samples → Decimator (8x) → OFDM Symbols → OFDM Frame Sync → Payload
              (firdecim_crcf)               (ofdmflexframesync)
```

### Why Decimation/Interpolation?

Charon uses decimation by 8x to reduce the computational load:

- **Hardware sample rate**: 11.2 MHz (AD9361 on PlutoSDR)
- **After decimation**: 1.4 MHz
- **Occupied bandwidth**: ~140 kHz (with OFDM-64)
- **Benefit**: 8x reduction in processing complexity while maintaining sufficient bandwidth

The filters ensure that only the desired narrow band signal passes through, rejecting out-of-band interference.

## Modifying the Examples

### Change Payload Size
```c
#define PAYLOAD_LEN 512  // Change from 256 to 512 bytes
```

### Adjust Channel Noise
```c
float noise_floor = -30.0f;  // Lower SNR (more noise)
```

### Change Modulation
```c
#define OFDM_MODULATION LIQUID_MODEM_QPSK  // Lower order = more robust
```

Available options: `LIQUID_MODEM_QPSK`, `LIQUID_MODEM_QAM16`, `LIQUID_MODEM_QAM64`, etc.

### Disable FEC
```c
fgprops.fec0 = LIQUID_FEC_NONE;
fgprops.fec1 = LIQUID_FEC_NONE;
```

**Warning**: Changing OFDM parameters (subcarriers, CP length, etc.) requires rebuilding the decimation/interpolation filters with appropriate cutoff frequencies.

## Relationship to Charon

These examples extract the core OFDM PHY layer from Charon and demonstrate it in isolation:

| Charon Component | Example Equivalent |
|------------------|-------------------|
| `ofdm_tx.c` | OFDM frame generation + interpolation |
| `ofdm_rx.c` | OFDM frame sync + decimation |
| `pluto.c` | Replaced by simulated channel |
| `tap_device.c` | Not needed (direct payload access) |
| `charon.c` (MAC layer) | Not included (PHY only) |

On PlutoSDR, Charon:
1. Reads network packets from TAP device
2. Wraps them in OFDM frames (TX path in example)
3. Transmits via AD9361 RF transceiver
4. Receives via AD9361 (simulated channel in example)
5. Decodes OFDM frames (RX path in example)
6. Writes packets to TAP device

These examples show steps 2-5 in a controlled environment.

## Troubleshooting

### "undefined reference to liquid_*"
- liquid-dsp is not installed or not in library path
- Run: `sudo ldconfig` after installing liquid-dsp
- Check: `ldconfig -p | grep liquid`

### "Frame not detected"
- Increase SNR (decrease `noise_floor` value)
- Check that sample count is sufficient (add leading/trailing zeros)
- Verify FEC settings match between TX and RX

### Low EVM or high RSSI warnings
- This is normal in loopback - no real RF path
- Adjust gain and noise_floor for realistic values

## Further Reading

- [liquid-dsp documentation](https://liquidsdr.org/)
- [OFDM tutorial](https://liquidsdr.org/doc/ofdm/)
- [Charon project README](../README.md)

## License

MIT License - Same as Charon project (see ../LICENSE)
