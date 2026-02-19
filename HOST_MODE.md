# Charon Host Mode

This is a host-architecture build of Charon that replaces PlutoSDR hardware access with UDP socket interfaces for testing and development.

## Building

```bash
make host
```

This creates the `charon-host` binary for x86-64/ARM64 host systems.

## Architecture

The host build uses `pluto_host.c` instead of `pluto.c`, replacing libiio/AD9361 hardware calls with UDP sockets:

- **TX Socket**: Port 5002 - Sends OFDM baseband IQ samples (int16_t pairs) via UDP
- **RX Socket**: Port 5002 - Receives OFDM baseband IQ samples (int16_t pairs) for demodulation via UDP

Both sockets operate in non-blocking mode. The TX destination is automatically learned from the first RX packet source address.

Samples are at the OFDM baseband rate (equivalent to 1.4 MHz) — there is no software FIR interpolation/decimation stage. External sources should send samples at the OFDM baseband rate directly.

## Usage

1. Start charon-host (requires root for TAP device creation):
   ```bash
   sudo ./charon-host
   ```

2. Connect external RF hardware or simulation:
   - Send IQ samples TO localhost:5002 (RX socket)
   - Receive IQ samples FROM localhost:5001 (TX socket)

The system automatically learns the TX destination from the first packet received on the RX socket.

## Sample Flow

### Transmit Path
```
TAP device → OFDM modulator → UDP port 5002
```

### Receive Path
```
UDP port 5002 → OFDM demodulator → TAP device
```

## Integration with SDR Hardware

You can connect this to GNU Radio, SDR++, or custom applications. Since there is no longer a software FIR stage, the external source/sink operates at the OFDM baseband rate (1.4 MHz equivalent). An external SDR application must handle any interpolation/decimation needed for its hardware sample rate.

### Example GNU Radio Flowgraph
```
UDP Source (port 5002) → Complex to Float → Interpolate (8x) → PlutoSDR Sink (11.2 MSPS)
PlutoSDR Source (11.2 MSPS) → Decimate (8x) → Float to Complex → UDP Sink (localhost:5002)
```

### Example Python UDP Client
```python
import socket
import struct

# Create UDP sockets
tx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# Send samples to charon RX (port 5002)
while True:
    i_sample, q_sample = get_samples_from_sdr()
    packet = struct.pack('hh', i_sample, q_sample)
    rx_sock.sendto(packet, ('localhost', 5002))

# Receive samples from charon TX (port 5001)
tx_sock.bind(('', 5001))
while True:
    data, addr = tx_sock.recvfrom(4)  # 2x int16
    i_sample, q_sample = struct.unpack('hh', data)
    send_to_sdr(i_sample, q_sample)
```

### Simple Loopback Test
```bash
# Terminal 1: Start charon-host
sudo ./charon-host

# Terminal 2: Create loopback with netcat
nc -u localhost 5002 | nc -u localhost 5001
```

## Differences from PlutoSDR Build

- No hardware AGC (simulated gain control)
- No frequency offset correction
- No hardware filters
- Sample rate configuration ignored (use external SDR settings)
- Transmit power control no-op

All OFDM modulation/demodulation, FEC, and network stack integration remain identical to the PlutoSDR version.

## Development Use Cases

- Test OFDM PHY layer without PlutoSDR hardware
- Prototype changes using software-defined radios
- Integrate with RF simulation environments
- Debug protocol logic with loopback connections
