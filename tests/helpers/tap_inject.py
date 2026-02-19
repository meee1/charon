#!/usr/bin/env python3
# MIT License
# TAP device Ethernet frame injector for charon-host testing.
#
# Opens the ofdm0 TAP device and writes raw Ethernet frames.
# charon-host reads these frames, OFDM-modulates them, and sends IQ samples
# via UDP to 127.0.0.1:5002 (which loops back to its own RX socket).
#
# Requires root and that charon-host is running with ofdm0 TAP device created.

import argparse
import fcntl
import os
import struct
import sys
import time


# Linux TUN/TAP ioctl constants
TUNSETIFF = 0x400454CA
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000

# Ethernet frame layout
ETH_HEADER_LEN = 14


def open_tap(device_name):
    """Open an existing TAP device for reading and writing."""
    try:
        tun_fd = os.open("/dev/net/tun", os.O_RDWR)
    except OSError as e:
        print(f"Cannot open /dev/net/tun: {e}", file=sys.stderr)
        print("  Ensure running as root and kernel TUN/TAP support is available.", file=sys.stderr)
        sys.exit(1)

    # Attach to the named TAP device (IFF_NO_PI = no packet info header)
    ifr = struct.pack("16sH", device_name.encode(), IFF_TAP | IFF_NO_PI)
    try:
        fcntl.ioctl(tun_fd, TUNSETIFF, ifr)
    except OSError as e:
        os.close(tun_fd)
        print(f"Cannot attach to TAP device '{device_name}': {e}", file=sys.stderr)
        print(f"  Ensure charon-host is running and '{device_name}' device exists.", file=sys.stderr)
        sys.exit(1)

    return tun_fd


def make_eth_frame(src_mac, dst_mac, ethertype, payload):
    """Build a raw Ethernet frame."""
    return (
        bytes.fromhex(dst_mac.replace(":", ""))
        + bytes.fromhex(src_mac.replace(":", ""))
        + struct.pack(">H", ethertype)
        + payload
    )


def inject_frames(device_name, count, interval):
    """Inject Ethernet broadcast frames into the TAP device."""
    tun_fd = open_tap(device_name)

    # Use a test source MAC and broadcast destination
    src_mac = "02:00:00:00:00:01"   # Locally administered, unicast
    dst_mac = "ff:ff:ff:ff:ff:ff"   # Broadcast
    ethertype = 0x0800               # IPv4 (content will be garbage — just exercising TX path)

    # Minimal payload: 46 bytes minimum Ethernet payload (total frame >= 60)
    payload = b"CHARON-HOST-TEST " + b"A" * 30

    frame = make_eth_frame(src_mac, dst_mac, ethertype, payload)

    injected = 0
    try:
        for i in range(count):
            written = os.write(tun_fd, frame)
            injected += 1
            print(f"  Injected frame {i + 1}: {written} bytes "
                  f"({ETH_HEADER_LEN} hdr + {len(payload)} payload)")
            if interval > 0 and i < count - 1:
                time.sleep(interval)
    except OSError as e:
        print(f"Write error on frame {injected + 1}: {e}", file=sys.stderr)
        os.close(tun_fd)
        sys.exit(1)
    finally:
        os.close(tun_fd)

    print(f"Injected {injected} frames into {device_name}")


def main():
    parser = argparse.ArgumentParser(
        description="Inject raw Ethernet frames into a TAP device for charon-host testing"
    )
    parser.add_argument(
        "--device",
        default="ofdm0",
        help="TAP device name (default: ofdm0)",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=3,
        help="Number of frames to inject (default: 3)",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=0.5,
        help="Seconds between frames (default: 0.5)",
    )
    args = parser.parse_args()

    inject_frames(args.device, args.count, args.interval)


if __name__ == "__main__":
    main()
