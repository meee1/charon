# Charon - PlutoSDR OFDM Mesh Transceiver

## Project Overview
Charon transforms Analog Devices PlutoSDR devices into standalone OFDM transceivers with batman-adv mesh networking. The system implements a 1.4 MHz OFDM-64 QAM-16 wireless physical layer at 272 Kbps, providing layer-2 mesh routing for TCP/IP traffic between host systems.

**Key Architecture**: Embedded C application running on PlutoSDR's ARM processor (Xilinx Zynq-7000), interfacing with AD9361 RF transceiver via libiio, bridging wireless OFDM frames to/from TAP network device.

## Critical Components

### Core Signal Flow (charon.c main_loop)
1. **RX Path**: `pluto.c` → AD9361 IQ samples → `ofdm_rx.c` (liquid-dsp demodulation) → `tap_device.c` → Linux network stack
2. **TX Path**: `tap_device.c` → `ofdm_tx.c` (liquid-dsp modulation) → `pluto.c` → AD9361 RF output
3. **MAC Layer**: Custom CSMA with ACK/retransmission in `charon.c`, distinguishes broadcast vs unicast, batman-adv frames wrapped in custom Charon protocol

### OFDM Configuration (ofdm_conf.h)
- **Fixed Parameters**: 64 subcarriers, 16-QAM modulation, 8x decimation/interpolation factor
- Sample rate: 11.2 MHz hardware → decimated by 8x to 1.4 MHz → 1.4 MHz occupied bandwidth
- Subcarrier spacing: ~21.875 kHz (1.4 MHz / 64 subcarriers)
- FEC: SECDED7264 + Hamming128 for forward error correction
- **DO NOT change these without rebuilding filters** (see `filters/pluto/pluto_filters.h`)

### PlutoSDR Integration (pluto.c)
- Uses libiio for direct AD9361 hardware control
- Implements custom AGC with fast-attack and slow-decay timers (bypasses hardware AGC for OFDM requirements)
- **Critical**: Requires `maxcpus` u-boot variable set to run charon on second CPU core (prevents sample drops)
- TX gain control via `pluto_set_out_gain()`, defaults to -10dBm (100 µW) per FCC unlicensed operation

### Network Interface Bridge (tap_device.c)
- Creates `ofdm0` TAP device with MAC address derived from PlutoSDR USB IP
- Bridges `ofdm0` ↔ `bat0` (batman-adv) ↔ `mesh-bridge` ↔ `usb0` (host connection)
- **Frame wrapping**: Batman frames get Charon ethernet header (type 0x0420) + 4-byte PID for duplicate detection
- ACK frames are 6-byte MAC addresses without payload

### TCP Window Manipulation (tcp_subs.c)
- Rewrites TCP MSS options during SYN/SYN-ACK to limit window size (default 2 segments = 2920 bytes)
- Prevents TCP from overwhelming narrow-band link with excessive retransmissions
- Recalculates IP/TCP checksums after modification

## Configuration System (config.c)
All runtime parameters stored in PlutoSDR u-boot environment via `fw_setenv`:
- `ref_correction_ppm`: **Most critical** - frequency error correction (narrow OFDM requires <1 ppm accuracy)
- `freq_rxtx_hz`: Operating frequency (default 915 MHz ISM band)
- `max_short_retrans` (8) / `max_long_retrans` (1): Retry policy for <128 byte vs ≥128 byte frames
- `bcast_retrans` (1): Batman OGM retransmissions
- `bat_ogm_interval` (10000 ms): Reduces mesh overhead vs default 1000 ms
- `ack_delay_timeout` (25000 µs): Wait time for ACK before retry
- `symbol_delay_timeout`: TX backoff after RX to allow channel turnaround

Set all defaults via `/root/set_charon_env.sh` script on PlutoSDR

## Build System

### Quick Start for Building Charon
1. **Initial setup** (30-60 minutes first time):
   ```bash
   ./setup_build_env.sh  # Builds toolchain and dependencies
   ```
2. **Build charon binary**:
   ```bash
   make
   ```
3. **Full firmware rebuild** (after modifying charon):
   ```bash
   cp charon plutosdr-fw/buildroot/output/target/usr/bin/
   cd plutosdr-fw && make
   # Result: build/pluto.frm ready to flash
   ```

### Cross-Compilation Requirements (Makefile)
- **Toolchain**: Linaro GCC 7.3-2018.05 `arm-linux-gnueabihf-` with hard float ABI
- **Sysroot**: Points to `plutosdr-fw/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/`
- **Dependencies**: liquid-dsp (OFDM PHY), libiio, libad9361, fftw3, libfec, libtuntap
- **Third-party**: Custom builds of `libfec` and `libtuntap` in `third_party/` (static linking)
- **WSL Users**: Must use Linux native filesystem (not `/mnt/c/`), script cleans PATH automatically

### Full Firmware Build
1. Build Analog Devices `plutosdr-fw` (Buildroot + Xilinx Vivado 2016.4)
2. Apply Charon configs: `cp -fr charon/changes_to_plutosdr_fw_configs_rel_to_v28/* plutosdr-fw/`
3. Build Buildroot packages: batctl, bridge-utils, fftw, iperf3, iproute2, liquid-dsp, tunctl, util-linux
4. Cross-compile charon, copy to `plutosdr-fw/buildroot/output/target/usr/bin`
5. Run `make` in `plutosdr-fw/` → generates `pluto.frm` in `build/` directory
6. Flash via USB mass storage: `cp pluto.frm /media/PlutoSDR/`

### Development Workflow
- **Incremental builds**: Only recompile charon binary, copy to buildroot target, rebuild pluto.frm
- **Testing**: SSH to PlutoSDR (root/analog), restart daemon: `/etc/init.d/S100-start_charon restart`
- **Monitor packets**: Daemon logs to stderr showing RSSI, EVM, throughput when running in foreground

## Project-Specific Patterns

### Error Handling
- Minimal error checks (embedded systems style) - failures typically fatal or ignored
- `fprintf(stderr, ...)` for runtime diagnostics (visible when daemon runs in foreground)
- No exceptions (pure C99)

### Timing Critical Sections
- `timers.c` provides microsecond-resolution timers for ACK timeouts, AGC updates
- AGC runs dual-timer (fast 100ms attack, slow 5s decay) to handle near/far problem
- Symbol timing (`symbol_delay_timeout`) critical for TDMA-like channel access

### Memory Management
- Stack allocation for frame buffers (e.g., `uint8_t tap_buffer[2342]`)
- liquid-dsp objects (`ofdmflexframegen`, `firdecim_crcf`) managed as static globals
- No dynamic allocation in hot path

### Batman-adv Integration
- Reads `/sys/kernel/debug/batman_adv/bat0/originators` to query mesh routes (`util.c`)
- Does NOT implement routing logic - relies on kernel batman-adv module
- Charon only provides physical layer + basic MAC, batman-adv handles multi-hop routing

## Testing & Debugging

### Frequency Calibration (CRITICAL)
- Use one PlutoSDR as spectrum analyzer to measure transmit frequency error of others
- Adjust each node's `ref_correction_ppm` via `fw_setenv` until all within ~1 ppm
- Symptoms of misalignment: No packet reception, poor EVM (>-15 dB)

### Performance Metrics
- **Good link**: RSSI > -100 dBm, EVM < -20 dB, TCP throughput ~117 Kbps (single hop)
- **Degraded**: Multi-hop reduces throughput ~40% per hop (80 Kbps @ 1 hop, 50 Kbps @ 2 hops)
- Use `iperf3 -c <remote_ip>` to measure (iperf3 server runs automatically on PlutoSDR)

### Common Issues
- **Samples dropping**: Verify `maxcpus` u-boot var set (enables 2nd CPU core)
- **No mesh formation**: Check batman OGM broadcasts transmitting (10s interval), verify all nodes same frequency
- **Low throughput**: AGC may be stuck - separation >3 feet if co-located nodes

## DO NOT
- Modify OFDM parameters without regenerating FIR filters (requires liquid-dsp filter design)
- Change `sample_freq_hz` (fixed at 11.2 MHz hardware, 1.4 MHz decimated)
- Remove `maxcpus` setting (causes real-time sample starvation)
- Set `enable_charon=0` unless intentionally disabling mesh mode for normal PlutoSDR use

## External Dependencies
- **liquid-dsp**: Provides OFDM frame sync/gen, FIR filters, NCO, modem - core of PHY layer
- **batman-adv kernel module**: Layer-2 mesh routing protocol (not batman-adv daemon mode)
- **plutosdr-fw**: Full buildroot-based firmware (Linux kernel + Xilinx FPGA bitstream)
- Submodule at `plutosdr-fw/` NOT tracked by this repo - clone separately

## Regulatory Note
Default 915 MHz @ -10 dBm complies with FCC Part 15 unlicensed. **Verify local regulations** before operating. 2.4 GHz band also supported (`freq_rxtx_hz` range: 902-928 MHz, 2412-2462 MHz).
