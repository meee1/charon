# Charon OFDM Examples - Quick Reference

## Build Commands

```bash
cd example/
make              # Build all examples
make clean        # Remove binaries
make help         # Show available targets
./test.sh         # Run automated tests
```

## Example Programs

### ofdm_loopback_example
Simple loopback test demonstrating OFDM TX/RX

```bash
./ofdm_loopback_example
```

**No arguments needed** - runs self-contained test with predefined payload.

---

### ofdm_file_transfer  
Transfer files over simulated OFDM link

```bash
./ofdm_file_transfer <input_file> [output_file]
```

**Examples:**
```bash
# Basic usage
./ofdm_file_transfer myfile.txt received.txt

# Transfer binary file
./ofdm_file_transfer image.png output.png

# Default output name (received.dat)
./ofdm_file_transfer data.bin
```

## OFDM Parameters (Matching Charon)

| Parameter | Value | Notes |
|-----------|-------|-------|
| Sample Rate | 11.2 MHz | Hardware rate on PlutoSDR |
| Decimation Factor | 8 | Reduces to 1.4 MHz |
| Occupied BW | ~140 kHz | After decimation |
| Subcarriers | 64 | OFDM-64 |
| Cyclic Prefix | 4 samples | |
| Taper | 2 samples | |
| Modulation | QAM-16 | 4 bits/symbol |
| FEC (inner) | SECDED7264 | 64/72 rate |
| FEC (outer) | HAMMING128 | 8/12 rate |
| CRC | CRC-32 | |
| Theoretical Rate | ~272 kbps | With FEC overhead |

## Modifying Parameters

Edit the `#define` values at the top of each .c file:

```c
#define OFDM_M              64              // Subcarriers
#define OFDM_MODULATION     LIQUID_MODEM_QAM16  // Modulation
#define PAYLOAD_LEN         256             // Frame size
#define DECIMATE_INTERPOLATE_FACTOR 8       // Decimation
```

**Warning:** Changing OFDM parameters requires careful filter redesign!

## Troubleshooting

### Build Errors

**Problem:** `undefined reference to liquid_*`  
**Solution:** Install liquid-dsp library
```bash
sudo apt-get install liquid-dsp
# OR build from source (see README.md)
```

**Problem:** `fftw3.h: No such file or directory`  
**Solution:** Install FFTW3 development headers
```bash
sudo apt-get install libfftw3-dev
```

### Runtime Issues

**Problem:** Frame not detected in loopback  
**Solution:** 
- Ensure liquid-dsp is properly installed
- Try rebuilding with debug symbols: `make clean && CFLAGS="-g" make`
- Increase SNR by reducing noise_floor value in code

**Problem:** File transfer corrupted  
**Solution:**
- Check that file isn't too large (start with <100KB)
- Reduce noise level in channel simulation
- Increase frame size for better efficiency

### Performance

**Problem:** Low throughput  
**Causes:**
- FEC overhead (~55% of raw rate)
- Small frame sizes (increase PAYLOAD_LEN)
- High frame error rate (check SNR)

**Problem:** High EVM (Error Vector Magnitude)  
**Normal Range:** -20 to -30 dB for QAM-16  
**If worse:** Adjust channel noise or modulation scheme

## Advanced Usage

### Run with Valgrind (memory debugging)
```bash
valgrind --leak-check=full ./ofdm_loopback_example
```

### Measure execution time
```bash
time ./ofdm_file_transfer largefile.bin output.bin
```

### Batch testing
```bash
for i in {1..10}; do
    ./ofdm_loopback_example || echo "Run $i failed"
done
```

## Integration with Charon

These examples extract the PHY layer from Charon:

```
Charon Full Stack:
┌──────────────────────┐
│  Network (TCP/IP)    │
├──────────────────────┤
│  batman-adv (L2)     │  ← Not in examples
├──────────────────────┤
│  MAC Layer           │  ← Not in examples  
├──────────────────────┤
│  OFDM PHY (TX/RX)    │  ← These examples!
├──────────────────────┤
│  AD9361 RF           │  ← Replaced by simulation
└──────────────────────┘
```

To use this code with actual RF hardware:
1. Replace channel simulation with IIO calls (see `pluto.c`)
2. Add MAC layer with ACK/retransmission (see `charon.c`)
3. Bridge to network interface (see `tap_device.c`)

## Resources

- Charon Documentation: `../README.md`
- liquid-dsp Manual: https://liquidsdr.org/doc/
- OFDM Tutorial: https://liquidsdr.org/doc/ofdm/

## License

MIT License (same as Charon) - See `../LICENSE`
