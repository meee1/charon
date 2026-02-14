# CLAUDE.md - Charon Project Guide

## Project Overview

Charon is an embedded C application that transforms Analog Devices PlutoSDR devices into autonomous OFDM transceivers with batman-adv mesh networking. It runs on the PlutoSDR's ARM processor (Xilinx Zynq-7000 Cortex-A9) and implements a 1.4 MHz OFDM-64 QPSK wireless physical layer, providing layer-2 mesh routing for TCP/IP traffic between host systems over ISM bands (915 MHz or 2.4 GHz).

Named after one of Pluto's moons.

## Build Commands

### Cross-compile for PlutoSDR (ARM)
```bash
# One-time toolchain setup (requires plutosdr-fw submodule)
./setup_build_env.sh

# Build charon binary
make
```

### Build for host testing (x86_64, no hardware needed)
```bash
make host
```
This produces `charon-host`, which replaces RF hardware with UDP sockets (ports 5001 RX, 5002 TX).

### Build example programs
```bash
cd example && make
```
Builds `ofdm_loopback_example` (TX/RX loopback test) and `ofdm_file_transfer` (multi-frame simulation).

### Full firmware image
```bash
cp charon plutosdr-fw/buildroot/output/target/usr/bin/
cd plutosdr-fw && make
# Output: build/pluto.frm
```

### Clean
```bash
make clean        # ARM build artifacts in .build/
make -f Makefile.host clean  # Host build artifacts in .build_host/
```

## Project Structure

```
charon.c          Main event loop, MAC layer (CSMA), ACK/retransmission state machine
pluto.c           PlutoSDR hardware interface via libiio/AD9361 (AGC, frequency, gain)
pluto_host.c      Host-mode replacement: UDP sockets instead of hardware
ofdm_tx.c         OFDM frame modulation using liquid-dsp ofdmflexframegen
ofdm_rx.c         OFDM frame demodulation using liquid-dsp ofdmflexframesync
tap_device.c      TAP network device (ofdm0) creation, bridge setup, frame wrapping
tcp_subs.c        TCP MSS/window rewriting to prevent link overflow
config.c          Runtime config via PlutoSDR u-boot environment (fw_printenv)
util.c             Batman-adv route table parsing
crc.c             CRC-32 calculations
timers.c          Microsecond-resolution timers (gettimeofday-based)
glibc_compat.c    glibc compatibility shims (strtol/strtoll wrapping)

ofdm_conf.h       OFDM parameters (64 subcarriers, QPSK, FEC, 8x decimation)
ofdm.h            liquid-dsp internal struct definitions
ethernet.h        Ethernet frame structures

filters/pluto/    Pre-calculated FIR filter coefficients (131 KB)
third_party/      Bundled libfec (FEC) and libtuntap (TAP device)
example/          Host-mode loopback and file transfer examples
```

## Signal Flow

```
RX: AD9361 IQ samples -> pluto.c -> ofdm_rx.c (demodulate) -> tap_device.c -> Linux stack
TX: tap_device.c -> ofdm_tx.c (modulate) -> pluto.c -> AD9361 RF output
MAC: charon.c manages frame queueing, ACK tracking, retransmission, batman frame wrapping
```

## Architecture and Conventions

### Language and Standard
- **C99 with GNU extensions** (`-std=gnu99`)
- No C++ anywhere in the project
- MIT License header required on all source files

### Coding Style
- Embedded systems style: minimal abstraction, global state, static module-level variables
- Functions use `snake_case` (e.g., `do_ofdm_tx`, `read_tap_dev`, `pluto_set_out_gain`)
- Module-scoped globals declared `static` at file top
- Stack-allocated frame buffers in hot path (e.g., `uint8_t tap_buffer[2342]`)
- No dynamic allocation in the main loop — liquid-dsp objects managed as static globals
- Diagnostic output via `fprintf(stderr, ...)` — visible when daemon runs in foreground
- Section dividers use `///...///` comment blocks between functions
- Minimal error handling: failures are typically fatal or silently ignored (embedded convention)

### Header Files
- Header files are **auto-generated** (marked `/* This file was automatically generated. Do not edit! */`)
- They contain only function declarations extracted from corresponding `.c` files
- Do not manually edit `.h` files that carry this warning

### Build Targets
- `Makefile` — ARM cross-compilation with Linaro GCC 7.3 (`arm-linux-gnueabihf-gcc`)
  - Compiler flags: `-O2 -std=gnu99 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ggdb`
  - Output directory: `.build/`
  - Statically links: libliquid.a, libfftw3f.a, libfec, libtuntap
  - Dynamically links: libc, libiio, libad9361, libusb-1.0, and others
- `Makefile.host` — Native host build with system gcc
  - Compiler flags: `-O0 -std=gnu99 -ggdb`
  - Output directory: `.build_host/`
  - Uses `pluto_host.c` instead of `pluto.c`

### Key Dependencies
- **liquid-dsp** — OFDM PHY layer (ofdmflexframegen, ofdmflexframesync, FIR filters)
- **libiio** / **libad9361-iio** — AD9361 RF transceiver hardware control
- **FFTW3** — FFT (used by liquid-dsp)
- **libfec** — Forward error correction (Viterbi, Reed-Solomon)
- **libtuntap** — TAP device creation
- **batman-adv** — Kernel module for layer-2 mesh routing

## Critical Constraints (DO NOT)

- **Do not modify OFDM parameters** in `ofdm_conf.h` without regenerating FIR filters in `filters/pluto/pluto_filters.h`. The filter coefficients are tightly coupled to the 8x decimation factor and 64-subcarrier configuration.
- **Do not change `sample_freq_hz`** — it is fixed at 11.2 MHz hardware / 1.4 MHz decimated.
- **Do not remove `maxcpus`** u-boot setting — it enables the second CPU core needed for real-time sample processing.
- **Do not set `enable_charon=0`** unless intentionally disabling mesh mode.

## Testing

There is no traditional unit test suite. Testing is done through:

1. **Example programs** (`example/` directory):
   - `ofdm_loopback_example` — validates OFDM TX/RX in loopback without RF
   - `ofdm_file_transfer` — simulates multi-frame file transfer
   - Run: `cd example && make && ./ofdm_loopback_example`

2. **Host mode** (`make host`):
   - Produces `charon-host` using UDP sockets instead of RF hardware
   - Validates OFDM PHY + MAC layer in software
   - See `HOST_MODE.md` for details

3. **On-device testing** (via SSH):
   - `ssh root@192.168.2.1` (password: `analog`)
   - Restart: `/etc/init.d/S100-start_charon restart`
   - Performance: `iperf3 -c <remote_ip>` (iperf3 server auto-starts on PlutoSDR)

4. **CI** (`.github/workflows/nomod.yml`):
   - GitHub Actions on ubuntu-24.04
   - Builds full cross-compiled binary and firmware image
   - Triggered on push/PR to `master` and `dev` branches

## Runtime Configuration

All parameters stored in PlutoSDR u-boot environment (set via `fw_setenv` on-device, read via `fw_printenv` in `config.c`):

| Variable | Default | Purpose |
|----------|---------|---------|
| `enable_charon` | 1 | Start daemon on boot |
| `ref_correction_ppm` | 6.15 | Frequency correction (**most critical** — narrow OFDM needs <1 ppm) |
| `freq_rxtx_hz` | 915000000 | Operating frequency (915 MHz ISM) |
| `sample_freq_hz` | 11200000 | Hardware sample rate (fixed, do not change) |
| `rf_bandwidth` | 250000 | RF filter bandwidth |
| `tx_output_power_minus_dbm` | 10 | TX power (-10 dBm / 100 uW, FCC Part 15) |
| `max_short_retrans` | 8 | Retries for <128 byte frames |
| `max_long_retrans` | 1 | Retries for >=128 byte frames |
| `bcast_retrans` | 1 | Broadcast frame retransmissions |
| `bat_ogm_interval` | 10000 | Batman OGM interval (ms) |
| `ack_delay_timeout` | 25000 | ACK wait timeout (usec) |
| `symbol_delay_timeout` | 144 | TX delay after RX (usec) |
| `max_tcp_segs` | 2 | TCP window size limit (segments) |

Set defaults on-device with: `sh /root/set_charon_env.sh`

## Network Bridge Architecture

```
Host USB <-> usb0 <-> mesh-bridge <-> bat0 (batman-adv) <-> ofdm0 (TAP) <-> Charon <-> RF
```

- `ofdm0`: TAP device with MAC derived from PlutoSDR USB IP
- `bat0`: batman-adv mesh interface
- `mesh-bridge`: Linux bridge joining bat0 and usb0
- Charon ethernet frame type: `0x0420` with 4-byte PID for duplicate detection
- ACK frames: 6-byte MAC address without payload

## Performance Expectations

- Good link: RSSI > -100 dBm, EVM < -20 dB, TCP ~117 Kbps (single hop)
- Multi-hop: ~40% throughput reduction per hop
- Frequency calibration is critical: measure with spectrum analyzer, adjust `ref_correction_ppm`

## Regulatory

Default 915 MHz @ -10 dBm complies with FCC Part 15 unlicensed operation. Verify local regulations before operating. See `REGULATIONS` file.
