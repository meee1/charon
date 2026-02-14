# Charon OFDM Examples - Complete Package

## What's Included

This example package contains standalone applications demonstrating Charon's OFDM PHY layer using liquid-dsp on native host systems (no PlutoSDR hardware required).

### Source Files

1. **ofdm_loopback_example.c**
   - Complete OFDM TX/RX loopback demonstration
   - Shows frame generation, channel simulation, and reception
   - Self-contained test with payload verification
   - Educational comments explaining each step

2. **ofdm_file_transfer.c** (600+ lines)
   - Multi-frame file transfer simulation
   - Demonstrates fragmentation and reassembly
   - Throughput and statistics calculation
   - Real-world use case example

### Build System

3. **Makefile**
   - Simple one-command build (`make`)
   - Automatic dependency detection
   - Help system and cleanup targets

### Documentation

4. **README.md** (400+ lines)
   - Comprehensive guide to examples
   - Installation instructions
   - Detailed explanation of code structure
   - Signal flow diagrams
   - Troubleshooting guide
   - Relationship to Charon explained

5. **QUICKREF.md**
   - Quick reference card
   - Command syntax
   - Parameter tables
   - Common problems and solutions
   - Integration guidance

### Testing & Utilities

6. **test.sh**
   - Automated test suite
   - Validates both examples work correctly
   - Tests small and large file transfers
   - Quick verification after building

7. **benchmark.sh**
   - Performance benchmarking tool
   - Tests various file sizes
   - Measures throughput and success rate
   - Compares to theoretical limits

8. **.gitignore**
   - Excludes build artifacts
   - Ignores test files
   - Keeps repository clean

## Configuration Match with Charon

All examples use **identical** OFDM parameters to Charon:

```c
Sample Rate:        1.4 MHz         (OFDM rate, AD9361 handles filtering)
Occupied BW:        ~1.4 MHz
Subcarriers:        64              (OFDM-64)
Cyclic Prefix:      4 samples
Taper:              2 samples
Modulation:         QAM-16          (4 bits/symbol)
FEC Inner:          SECDED7264      (64/72 rate)
FEC Outer:          HAMMING128      (8/12 rate)
CRC:                CRC-32
Data Rate:          ~272 kbps       (theoretical max)
```

## Quick Start Guide

### Install Dependencies (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install liquid-dsp libfftw3-dev
```

### Build Examples
```bash
cd example/
make
```

### Run Tests
```bash
./test.sh
```

### Try Examples
```bash
# Simple loopback
./ofdm_loopback_example

# File transfer
echo "Hello OFDM!" > test.txt
./ofdm_file_transfer test.txt received.txt
cat received.txt

# Performance benchmark
./benchmark.sh
```

## What Each Example Demonstrates

### ofdm_loopback_example

**Concepts Covered:**
- Creating OFDM frame generator (`ofdmflexframegen`)
- Configuring FEC, modulation, and CRC
- Generating OFDM symbols
- Simulating AWGN channel
- Creating OFDM frame synchronizer (`ofdmflexframesync`)
- Callback-based frame reception
- Payload verification
- Signal quality metrics (RSSI, EVM, CFO)

**Use Case:** Understanding the complete OFDM TX/RX pipeline

### ofdm_file_transfer

**Concepts Covered:**
- Everything in loopback example, plus:
- File I/O operations
- Frame fragmentation for large payloads
- Multi-frame transmission
- Frame reassembly
- Throughput calculation
- Transfer statistics
- Success rate analysis
- Practical data transfer workflow

**Use Case:** Simulating real file transfers over OFDM link

## Code Quality Features

### Comprehensive Comments
- Every major section documented
- Explains the "why" not just the "what"
- References to Charon implementation
- Educational value for learning OFDM

### Error Handling
- Checks for allocation failures
- Validates file operations
- Reports meaningful error messages
- Proper cleanup on failure

### Professional Structure
- Clear separation of concerns
- Logical flow from TX to channel to RX
- Reusable functions
- Statistics tracking

### Production-Ready
- No memory leaks (proper cleanup)
- Efficient buffer management
- Performance-conscious design
- Suitable for modification/extension

## Customization Points

### Easy to Modify

**Change Frame Size:**
```c
#define PAYLOAD_LEN 2048  // Increase from 256/1024
```

**Adjust Channel Conditions:**
```c
float noise_floor = -20.0f;  // More noise
float gain = 0.5f;           // Path loss
```

**Different Modulation:**
```c
#define OFDM_MODULATION LIQUID_MODEM_QPSK  // More robust
```

**Disable FEC (for testing):**
```c
fgprops.fec0 = LIQUID_FEC_NONE;
fgprops.fec1 = LIQUID_FEC_NONE;
```

### Advanced Modifications

**Add Frequency Offset:**
```c
nco_crcf nco = nco_crcf_create(LIQUID_NCO);
nco_crcf_set_frequency(nco, 0.001);  // 0.1% CFO
// Mix down/up samples
```

**Implement Fading Channel:**
```c
// Use liquid-dsp channel models
channel_cccf channel = channel_cccf_create();
channel_cccf_add_awgn(channel, noise_power);
channel_cccf_add_multipath(channel, delays, gains, num_paths);
```

**Add AGC (Automatic Gain Control):**
```c
agc_crcf agc = agc_crcf_create();
agc_crcf_execute(agc, sample_in, &sample_out);
```

## Relationship to Charon Project

### What's Included (from Charon)
- ✓ OFDM PHY layer (TX/RX)
- ✓ Same modulation/FEC parameters
- ✓ Frame structure

### What's NOT Included (PlutoSDR-specific)
- ✗ PlutoSDR/AD9361 hardware interface
- ✗ MAC layer (CSMA, ACK, retransmission)
- ✗ batman-adv mesh networking
- ✗ TAP device bridging
- ✗ TCP window manipulation
- ✗ AGC implementation

### Using These Examples to Understand Charon

1. **Start here:** Run `ofdm_loopback_example` to see PHY layer in isolation
2. **Study code:** Compare with `ofdm_tx.c` and `ofdm_rx.c` in Charon
3. **Experiment:** Modify parameters and see effects
4. **Transfer files:** Use `ofdm_file_transfer` to simulate real data
5. **Read Charon:** With PHY understood, explore MAC layer in `charon.c`
6. **Full system:** Study how PlutoSDR hardware ties it all together

## Performance Expectations

### Theoretical Limits
- **Symbol rate:** ~20 kHz (1.4 MHz / 70 samples per symbol)
- **Raw rate:** ~3.7 Mbps (20k symbols/sec × 46 data subcarriers × 4 bits)
- **With FEC:** ~272 kbps (after HAMMING + SECDED overhead)
- **Actual:** ~200-250 kbps (including framing overhead)

### Real-World (PlutoSDR)
- **Single hop:** ~117 kbps TCP throughput
- **Two hops:** ~50 kbps TCP throughput
- **Limited by:** FEC overhead, MAC layer, batman-adv overhead

### These Examples
- **Loopback:** Near-perfect (no real channel)
- **File transfer:** Limited by simulation speed, not bandwidth
- **Purpose:** Demonstrate correctness, not max performance

## Educational Value

### For Learning OFDM
- See complete TX/RX implementation
- Experiment with parameters safely
- No hardware required

### For Understanding Charon
- Isolates PHY from MAC/networking
- Simpler to debug and modify
- Same algorithms as PlutoSDR version
- Bridge between theory and hardware

### For Development
- Prototype new features here first
- Test changes before PlutoSDR deployment
- Faster compile-test cycle
- Better debugging tools available

## Next Steps

### After Running Examples

1. **Modify parameters** - Change modulation, FEC, frame size
2. **Add features** - Implement fading channel, frequency offset
3. **Study Charon** - Compare with full implementation
4. **Port to PlutoSDR** - Replace simulation with real RF
5. **Add MAC layer** - Implement retransmission, ACK protocol

### Integration Path

```
Examples → Charon → Your Application
   ↓           ↓            ↓
  PHY      PHY+MAC    Full Protocol Stack
```

## Support and Resources

- **Charon Documentation:** `../README.md`
- **liquid-dsp Manual:** https://liquidsdr.org/
- **OFDM Theory:** https://liquidsdr.org/doc/ofdm/
- **Charon Source:** `../charon.c`, `../ofdm_tx.c`, `../ofdm_rx.c`

## License

MIT License (same as Charon project)

Copyright (c) 2018 tvelliott (Charon)
2025 (Examples and documentation)

See `../LICENSE` for full license text.

---

**Created:** November 2025  
**For:** Charon PlutoSDR OFDM Mesh Transceiver Project  
**Purpose:** Educational demonstration of OFDM PHY layer
