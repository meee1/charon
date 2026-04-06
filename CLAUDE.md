# CLAUDE.md - Charon Project Guide

## Project Overview

Charon is an embedded C application that transforms Analog Devices PlutoSDR devices into autonomous OFDM transceivers with batman-adv mesh networking. It runs on the PlutoSDR's ARM processor (Xilinx Zynq-7000 Cortex-A9) and implements a 1.4 MHz OFDM-64 QPSK wireless physical layer with CW-tone-based CFO estimation, providing layer-2 mesh routing for TCP/IP traffic between host systems over ISM bands (915 MHz or 2.4 GHz).

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
This produces `charon-host`, which connects to a remote PlutoSDR over the network via libiio (IIO daemon). All RF operations (AD9361 config, IQ streaming, gain, AGC) work identically to on-device — the difference is IIO commands travel over the network. See `HOST_MODE.md` for URI options and integration details.

### Build example programs
```bash
cd example && make
```
Builds `ofdm_loopback_example` (TX/RX loopback test), `ofdm_file_transfer` (multi-frame simulation), `pss_sync_example` (PSS frequency-offset sync demo), and `ofdm_channel_test` (channel impairment test). Note: examples use QAM-16 modulation for demonstration; production Charon uses QPSK (see `ofdm_conf.h`).

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
charon.c          Main event loop, MAC layer (CSMA/LBT), ACK/retransmission state machine
pluto.c           PlutoSDR hardware interface via libiio/AD9361 (AGC, frequency, gain)
pluto_host.c      Host-mode replacement: remote PlutoSDR via libiio network context
ofdm_tx.c         OFDM frame modulation using liquid-dsp ofdmflexframegen
ofdm_rx.c         OFDM frame demodulation using liquid-dsp ofdmflexframesync
tap_device.c      TAP network device (ofdm0) creation, bridge setup, frame wrapping
tcp_subs.c        TCP MSS/window rewriting to prevent link overflow
config.c          Runtime config via PlutoSDR u-boot environment (fw_printenv), and
                  network bridge setup (batman-adv, TAP, mesh-bridge, taskset)
util.c            Batman-adv route table parsing
crc.c             CRC-32 calculations
timers.c          Microsecond-resolution timers (gettimeofday-based)
glibc_compat.c    glibc compatibility shims — wraps __isoc23_strtol, __isoc23_strtoll,
                  __isoc23_strtoul, and __isoc23_strtoull via linker --wrap flags

cw_tone.c         CW tone CFO estimator — TX generates 128-sample DC tone, RX detects
                  via lag-64 autocorrelation and estimates carrier frequency offset
pss_sync.c        Legacy PSS Zadoff-Chu correlator (replaced by cw_tone.c, retained for reference)

ofdm_conf.h       OFDM parameters (64 subcarriers, QPSK, FEC, 1x decimation)
ofdm.h            liquid-dsp internal struct definitions
ethernet.h        Ethernet frame structures

filters/pluto/    Pre-calculated FIR filter coefficients (131 KB)
third_party/      Bundled libfec (FEC) and libtuntap (TAP device)
example/          Host-mode loopback and file transfer examples
  QUICKREF.md     Quick reference for example build and usage
  PACKAGE.md      Packaging notes
  test.sh         Automated test script
  benchmark.sh    Throughput benchmark script

tests/            Unit test suite (CRC, TCP, timers, CW tone, channel simulation)
  test_harness.h  Minimal assert-based test framework (no external dependencies)
  test_crc.c      CRC-32 correctness tests
  test_tcp_subs.c TCP MSS/window rewriting tests
  test_timers.c   Microsecond timer tests
  test_cw_tone.c  CW tone generation and detection tests
  test_cw_tone_channel.c  CW tone under channel impairments (CFO, noise, multipath)

deploy.sh         Script to deploy charon binary to PlutoSDR via SSH
build_pluto_image/              Helper scripts/configs for PlutoSDR firmware builds
changes_to_plutosdr_fw_configs_rel_to_v28/  Diffs of config changes vs firmware v0.28
buildroot_static_libs.patch     Patch enabling static library builds in buildroot
deploy_callgrind.sh             Script to deploy and run Callgrind/Valgrind profiling on device
```

## Signal Flow

```
RX: AD9361 IQ samples -> pluto.c -> ofdm_rx.c (CW tone CFO + demodulate) -> tap_device.c -> Linux stack
TX: tap_device.c -> ofdm_tx.c (CW tone + modulate) -> pluto.c -> AD9361 RF output
MAC: charon.c manages frame queueing, ACK tracking, retransmission, batman frame wrapping
```

### CFO Estimation (`cw_tone.c`)

A single CW (continuous wave) DC tone is used for carrier frequency offset estimation,
replacing the earlier PSS Zadoff-Chu correlator which was too CPU-intensive for the Cortex-A9.

- **TX**: 128 samples of DC tone (`1.0 + 0.0j`) transmitted before each OFDM frame
- **RX**: While in SEEKPLCP state, a lag-64 autocorrelation detects the tone and estimates CFO:
  `cfo = arg(R(64)) / (2*pi*64)` in cycles/sample
- **Detection threshold**: Normalized autocorrelation metric `|R|/(P/2)` >= 0.85
- **Correction**: CFO applied to liquid-dsp NCO and hardware XO (`pluto_apply_pss_xo_correction`)
- **Cost**: ~4 multiply-adds per sample (vs ~441 for the old PSS multi-hypothesis correlator)
- **Over-the-air frame**: `[CW tone 128 samples] [OFDM PLCP + data]`

### MAC Layer Detail (`charon.c`)

`main_loop()` implements CSMA with listen-before-talk:
1. Call `pluto_receive()` continuously while channel is active (`OFDMFRAMESYNC_STATE_SEEKPLCP` not set)
2. AGC is checked periodically (fast: 10 ms, slow: 1 s)
3. After channel clears, check for pending retransmissions (exponential backoff with randomness)
4. Read new frame from TAP device if no retry pending
5. Transmit after `symbol_delay_timeout` has elapsed
6. Set `tx_retry` to `max_short_retrans` (<128 B frames) or `max_long_retrans` (>=128 B)
7. Broadcast frames get `bcast_retrans` transmissions with no ACK wait

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
- `Makefile` — ARM cross-compilation with Linaro GCC (`arm-linux-gnueabihf-gcc`)
  - Compiler flags: `-O2 -flto -std=gnu99 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ggdb -D_TIME_BITS=32 -fno-builtin-strtol`
  - Output directory: `.build/`
  - Statically links: libliquid.a, libfftw3f.a, libfec, libtuntap
  - Dynamically links: libc, libiio, libad9361, libini, libusb-1.0, libserialport, libavahi-client, libavahi-common, libxml2, libz, libdbus-1
  - Linker wraps `__isoc23_strtol`, `__isoc23_strtoll`, `__isoc23_strtoul`, `__isoc23_strtoull` via `glibc_compat.c`
- `Makefile.host` — Native host build with system gcc
  - Compiler flags: `-O0 -std=gnu99 -ggdb -D_FILE_OFFSET_BITS=64`
  - Output directory: `.build_host/`
  - Uses `pluto_host.c` instead of `pluto.c` (remote IIO via network)
  - Dynamically links all libraries (libiio, libad9361, liquid-dsp, fftw3, etc.)
- `tests/Makefile` — Unit tests with system gcc
  - Compiler flags: `-Wall -Wextra -O0 -g -std=gnu99`
  - Each test `#include`s the source `.c` directly — no separate library step
  - `make check` builds and runs all tests

### Key Dependencies
- **liquid-dsp** — OFDM PHY layer (ofdmflexframegen, ofdmflexframesync, FIR filters)
- **libiio** / **libad9361-iio** — AD9361 RF transceiver hardware control
- **FFTW3** — FFT (used by liquid-dsp)
- **libfec** — Forward error correction (Viterbi, Reed-Solomon)
- **libtuntap** — TAP device creation
- **batman-adv** — Kernel module for layer-2 mesh routing

## OFDM Parameters (`ofdm_conf.h`)

| Parameter | Value | Notes |
|-----------|-------|-------|
| `OFDM_M` | 64 | Subcarrier count |
| `CP_LEN` | 16 | Cyclic prefix length (samples) |
| `TAPER_LEN` | 2 | Taper length (samples) |
| `DECIMATE_INTERPOLATE_FACTOR` | 1 | Software oversampling ratio (hardware FIR handles decimation) |
| `OFDM_MODULATION` | `LIQUID_MODEM_QPSK` | Production modulation |
| `OFDM_FEC0` | `LIQUID_FEC_NONE` | Inner FEC |
| `OFDM_FEC1` | `LIQUID_FEC_SECDED7264` | Outer FEC |
| `OFDM_CRC` | `LIQUID_CRC_32` | Frame CRC |
| `PAYLOAD_LEN` | 1514 | Max frame payload (bytes) |
| `RX_TIMEOUT` | 4096 | RX loop iterations before timeout |

## Critical Constraints (DO NOT)

- **Do not change `sample_freq_hz`** — it is fixed at 1.4 MHz. The AD9361 hardware filter chain (`ad9361_set_bb_rate_custom_filter_auto`) is configured for this rate; there is no longer a software FIR stage.
- **Do not remove `maxcpus`** u-boot setting — it enables the second CPU core needed for real-time sample processing.
- **Do not set `enable_charon=0`** unless intentionally disabling mesh mode.

## Testing

### Unit tests (`tests/` directory)
```bash
cd tests && make check
```
- Self-contained C tests using a minimal assert-based harness (`test_harness.h`)
- Each test file `#include`s the `.c` under test directly, allowing access to `static` functions
- No external dependencies beyond the standard library and `-lm`
- Tests: `test_crc`, `test_tcp_subs`, `test_timers`, `test_cw_tone`, `test_cw_tone_channel`
- CI runs these first; build proceeds only if all pass

### Example programs (`example/` directory)
- `ofdm_loopback_example` — validates OFDM TX/RX in loopback without RF (uses QAM-16)
- `ofdm_file_transfer` — simulates multi-frame file transfer
- `pss_sync_example` — PSS Zadoff-Chu sync demonstration
- `ofdm_channel_test` — OFDM under channel impairments
- Run: `cd example && make && ./ofdm_loopback_example`
- Automated tests: `cd example && ./test.sh`

### Host mode (`make host`)
- Produces `charon-host` connecting to a remote PlutoSDR via libiio network context
- All RF control (AD9361 config, IQ streaming, gain, AGC, frequency) works over the network
- Specify PlutoSDR URI: `sudo ./charon-host --uri ip:192.168.2.1`
- IQ recording/playback: `--save-tx <file>` and `--load-rx <file>`
- See `HOST_MODE.md` for URI formats and integration details

### On-device testing (via SSH)
- `ssh root@192.168.2.1` (password: `analog`)
- Restart: `/etc/init.d/S100-start_charon restart`
- Performance: `iperf3 -c <remote_ip>` (iperf3 server auto-starts on PlutoSDR)

### Profiling (`deploy_callgrind.sh`)
- Deploys and runs Callgrind/Valgrind on-device for performance analysis

### CI (`.github/workflows/nomod.yml`)
- GitHub Actions on ubuntu-24.04
- **Stage 1**: Builds and runs unit tests (`cd tests && make check`)
- **Stage 2** (after tests pass): Cross-compiles charon binary with full toolchain
- **Stage 3**: Builds full PlutoSDR firmware image
- Applies `buildroot_static_libs.patch` to buildroot before building
- Uploads artifacts: `charon` binary, `out.txt` disassembly, and `plutosdr-fw/build/` firmware
- Triggered on push/PR to `master` and `dev` branches, and manual dispatch

## Runtime Configuration

All parameters stored in PlutoSDR u-boot environment (set via `fw_setenv` on-device, read via `fw_printenv` in `config.c`). Unset variables fall back to compiled-in defaults:

| Variable | Default | Purpose |
|----------|---------|---------|
| `enable_charon` | 1 | Start daemon on boot; exits immediately if 0 |
| `ref_correction_ppm` | 6.15 | Frequency correction (**most critical** — narrow OFDM needs <1 ppm) |
| `freq_rxtx_hz` | 915000000 | Operating frequency (915 MHz ISM) |
| `sample_freq_hz` | 1400000 | Hardware sample rate delivered to software (AD9361 decimates internally; fixed, do not change) |
| `rf_bandwidth` | 1400000 | RF filter bandwidth (1.4 MHz) |
| `tx_output_power_minus_dbm` | 10 | TX power stored as positive; negated in code (-10 dBm / 100 uW, FCC Part 15) |
| `max_short_retrans` | 8 | Retries for <128 byte frames |
| `max_long_retrans` | 1 | Retries for >=128 byte frames |
| `bcast_retrans` | 1 | Broadcast frame retransmissions |
| `bat_ogm_interval` | 10000 | Batman OGM interval (ms) |
| `ack_delay_timeout` | 25000 | ACK wait timeout (usec) |
| `symbol_delay_timeout` | `(OFDM_M+CP_LEN+TAPER_LEN)*2` ≈ 268 | TX delay after RX (usec) |
| `max_tcp_segs` | 2 | TCP window size limit (segments) |
| `usb_batman_if` | 0 | Whether USB interface participates in batman-adv (0 = standard bridge mode) |
| `max_tcp_share_backoff` | `symbol_delay_timeout * 12` | Max backoff for TCP connection sharing |
| `ipaddr` | (from usb0) | Override mesh-bridge IP address |

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
- `usb_batman_if=0` (default): standard bridge — usb0 and bat0 bridged together
- `usb_batman_if=1`: usb0 also added to batman-adv (non-batman clients see mesh via ARP, less OGM traffic, slower discovery)
- `charon` is pinned to CPU core 1 via `taskset` at startup (requires `maxcpus=2` u-boot env)

## Performance Expectations

- Good link: RSSI > -100 dBm, EVM < -20 dB, TCP ~117 Kbps (single hop)
- Multi-hop: ~40% throughput reduction per hop
- Frequency calibration is critical: measure with spectrum analyzer, adjust `ref_correction_ppm`

## Regulatory

Default 915 MHz @ -10 dBm complies with FCC Part 15 unlicensed operation. Verify local regulations before operating. See `REGULATIONS` file.
