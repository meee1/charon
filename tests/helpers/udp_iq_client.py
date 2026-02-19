#!/usr/bin/env python3
# MIT License
# UDP IQ sample client for charon-host testing.
#
# Sends int16_t I/Q pairs to charon-host's RX UDP port (127.0.0.1:5002).
# Optionally receives TX IQ samples from the same port to observe the
# self-loopback (charon-host TX also sends to 127.0.0.1:5002).
#
# Modes:
#   --mode silence   Send all-zero I/Q samples (idle channel simulation)
#   --mode noise     Send low-amplitude random noise samples
#   --mode recv      Receive and report IQ packets from port 5002

import argparse
import socket
import struct
import sys
import time
import random


TARGET_IP = "127.0.0.1"
TARGET_PORT = 5002
SAMPLES_PER_PACKET = 256  # 256 I/Q pairs per UDP packet (1024 bytes)


def make_silence_packet(n_samples=SAMPLES_PER_PACKET):
    """Return a UDP payload of n_samples zero I/Q pairs (int16_t)."""
    return struct.pack("<" + "hh" * n_samples, *([0] * (n_samples * 2)))


def make_noise_packet(n_samples=SAMPLES_PER_PACKET, amplitude=64):
    """Return a UDP payload of low-amplitude random noise I/Q pairs."""
    samples = []
    for _ in range(n_samples):
        i = random.randint(-amplitude, amplitude)
        q = random.randint(-amplitude, amplitude)
        samples.extend([i, q])
    return struct.pack("<" + "hh" * n_samples, *samples)


def send_mode(mode, count, interval):
    """Send IQ packets to charon-host RX port."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)

    sent = 0
    try:
        for i in range(count):
            if mode == "silence":
                pkt = make_silence_packet()
            elif mode == "noise":
                pkt = make_noise_packet()
            else:
                raise ValueError(f"Unknown mode: {mode}")

            bytes_sent = sock.sendto(pkt, (TARGET_IP, TARGET_PORT))
            sent += 1
            if interval > 0:
                time.sleep(interval)

        print(f"Sent {sent} IQ packets ({mode}) to {TARGET_IP}:{TARGET_PORT} "
              f"({SAMPLES_PER_PACKET} samples/packet)")
    except OSError as e:
        print(f"Send error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        sock.close()


def recv_mode(timeout, max_packets):
    """Receive IQ packets from port 5002 (charon-host TX output)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("", TARGET_PORT))
    except OSError as e:
        print(f"Cannot bind to port {TARGET_PORT}: {e}", file=sys.stderr)
        print("Note: charon-host may already be bound to this port.", file=sys.stderr)
        sys.exit(1)

    sock.settimeout(timeout)
    received = 0

    print(f"Listening for IQ packets on port {TARGET_PORT} "
          f"(timeout={timeout}s, max={max_packets} packets)...")

    try:
        while received < max_packets:
            data, addr = sock.recvfrom(65536)
            n_samples = len(data) // 4  # 4 bytes per I/Q pair (2x int16)
            received += 1
            print(f"  Packet {received}: {len(data)} bytes ({n_samples} I/Q pairs) from {addr}")
    except socket.timeout:
        pass
    finally:
        sock.close()

    print(f"Received {received} IQ packets total.")
    return received


def main():
    parser = argparse.ArgumentParser(
        description="UDP IQ sample client for charon-host testing"
    )
    parser.add_argument(
        "--mode",
        choices=["silence", "noise", "recv"],
        default="silence",
        help="silence: send zeros, noise: send random, recv: receive packets",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=10,
        help="Number of packets to send (send modes) or max to receive",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=0.01,
        help="Seconds between sent packets (default 0.01)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=2.0,
        help="Receive timeout in seconds (recv mode)",
    )
    args = parser.parse_args()

    if args.mode in ("silence", "noise"):
        send_mode(args.mode, args.count, args.interval)
    elif args.mode == "recv":
        recv_mode(args.timeout, args.count)


if __name__ == "__main__":
    main()
