# Charon Host Mode — Remote PlutoSDR via IIO

This is an x86_64 host build of Charon that controls a remote PlutoSDR over the network using libiio. All RF hardware functions (AD9361 configuration, IQ streaming, gain control, AGC, frequency tuning) operate identically to the on-device build — the only difference is that IIO commands travel over the network instead of the local bus.

## Prerequisites

Install libiio and libad9361 development libraries on the host:

```bash
# Debian/Ubuntu
sudo apt install libiio-dev libad9361-dev

# Or build from source:
# https://github.com/analogdevicesinc/libiio
# https://github.com/analogdevicesinc/libad9361-iio
```

Also required (same as on-device build): liquid-dsp, fftw3, libfec, libtuntap.

## Building

```bash
make host
```

This creates the `charon-host` binary for x86_64 host systems.

## Connecting to a Remote PlutoSDR

The host build connects to a PlutoSDR over the network via libiio's network backend. The PlutoSDR must be reachable and running its IIO daemon (iiod), which is enabled by default on PlutoSDR firmware.

### Specify the PlutoSDR URI

Use the `--uri` command line option:

```bash
sudo ./charon-host --uri ip:192.168.2.1
```

Or set the `PLUTO_URI` environment variable:

```bash
export PLUTO_URI=ip:192.168.2.1
sudo ./charon-host
```

Priority order: `--uri` flag > `PLUTO_URI` env var > default (`ip:192.168.2.1`).

### URI Formats

| Format | Example | Description |
|--------|---------|-------------|
| `ip:` | `ip:192.168.2.1` | Network (default PlutoSDR USB-Ethernet IP) |
| `ip:` | `ip:pluto.local` | Network with mDNS hostname |
| `usb:` | `usb:1.2.3` | USB (if PlutoSDR is connected directly) |

## Usage

```bash
# Default: connect to PlutoSDR at 192.168.2.1
sudo ./charon-host

# Specify PlutoSDR IP
sudo ./charon-host --uri ip:192.168.2.1

# With TX sample recording
sudo ./charon-host --uri ip:192.168.2.1 --save-tx /tmp/tx_samples.bin

# With RX sample playback
sudo ./charon-host --uri ip:10.0.0.50 --load-rx /tmp/rx_samples.bin
```

Root is required for TAP device creation (ofdm0).

## Verify Connectivity

Before running charon-host, verify the PlutoSDR is reachable:

```bash
# Check IIO context
iio_info -u ip:192.168.2.1

# Ping the device
ping 192.168.2.1
```

## Architecture

```
Host x86_64                          PlutoSDR
┌──────────────┐     libiio/network  ┌──────────────┐
│ charon-host  │ ◄────────────────► │ iiod         │
│              │     IQ + control    │ AD9361       │
│ OFDM TX/RX  │                     │ RF front-end │
│ MAC layer    │                     └──────────────┘
│ TAP device   │
│ batman-adv   │
└──────────────┘
```

The host runs the full Charon stack (OFDM modulation/demodulation, MAC layer, TAP device, batman-adv mesh routing) while the PlutoSDR handles RF — exactly as if Charon were running on the PlutoSDR itself, but with the processing offloaded to the host.

## Differences from On-Device Build

- IIO context is created via `iio_create_context_from_uri()` instead of `iio_create_local_context()`
- Network latency adds to TX/RX round-trip time (USB-Ethernet: negligible; WiFi: may affect timing)
- All hardware control (gain, frequency, AGC, filters) works identically over the network
- Same OFDM parameters, same MAC layer, same mesh networking

## Troubleshooting

| Issue | Solution |
|-------|----------|
| `failed to create IIO context` | Check PlutoSDR is powered and reachable (`ping 192.168.2.1`) |
| `Could not create RX buffer` | Another process may hold the IIO context — restart PlutoSDR or kill conflicting processes |
| `ad9361-phy device: (nil)` | IIO daemon not running on PlutoSDR, or wrong URI |
| High latency / dropped frames | Use USB-Ethernet connection instead of WiFi for lowest latency |
