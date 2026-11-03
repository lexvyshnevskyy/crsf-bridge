#!/usr/bin/env python3
"""Talk to the ground CH9121 over Ethernet, act as the radio handset, and
report CRSF frames coming back from the SineLink master / UAV.

The SineLink master is a CRSF transmitter module: it sends telemetry on the
port where it receives RC frames. --rc makes this PC send RC_CHANNELS frames
so the master starts talking. All channels are sent LOW/centred (throttle low,
switches low) - still remove props before testing.

Examples:
  # CH9121 UDP mode, local port 2000, destination = this PC:2000
  crsf_eth_monitor.py --udp 2000 --to 192.168.1.200 2000 --rc 50

  # CH9121 TCP CLIENT mode, destination = this PC:2000
  crsf_eth_monitor.py --tcp-server 2000 --rc 50

  # CH9121 TCP SERVER mode
  crsf_eth_monitor.py --tcp 192.168.1.200 2000 --rc 50

  # listen only
  crsf_eth_monitor.py --udp 2000
"""

import argparse
import socket
import struct
import sys
import time

SYNC_FC = 0xC8        # flight controller
SYNC_RADIO = 0xEA     # frames addressed to the handset
SYNC_MODULE = 0xEE    # frames addressed to the TX module (handset -> module)
CRSF_SYNC = {SYNC_FC, SYNC_MODULE, SYNC_RADIO, 0xEC}

FRAME_NAMES = {
    0x02: "GPS", 0x07: "VARIO", 0x08: "BATTERY", 0x09: "BARO_ALT",
    0x14: "LINK_STATISTICS", 0x16: "RC_CHANNELS", 0x1E: "ATTITUDE",
    0x21: "FLIGHT_MODE", 0x28: "DEVICE_PING", 0x29: "DEVICE_INFO",
    0x2B: "PARAM_ENTRY", 0x2C: "PARAM_READ", 0x2D: "PARAM_WRITE",
    0x32: "COMMAND", 0x7A: "MSP_REQ", 0x7B: "MSP_RESP",
}
FROM_UAV = {0x02, 0x07, 0x08, 0x09, 0x1E, 0x21}   # only a flight controller sends these

CH_LOW, CH_MID = 172, 992   # CRSF ticks for 988 us and 1500 us


def crc8(data):
    """Calculate the CRSF DVB-S2 CRC-8.

    :param bytes data: Bytes covered by the checksum.
    :return: The checksum as an integer from 0 to 255.
    """
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build_rc_frame():
    """Build a safe, low-throttle CRSF RC_CHANNELS test frame.

    :return: Complete CRSF frame addressed to the transmitter module.
    :rtype: bytes
    """
    ch = [CH_MID, CH_MID, CH_LOW, CH_MID] + [CH_LOW] * 12   # AETR, throttle low
    bits = 0
    for i, v in enumerate(ch):
        bits |= (v & 0x7FF) << (11 * i)
    payload = bytes([0x16]) + bits.to_bytes(22, "little")
    return bytes([SYNC_MODULE, len(payload) + 1]) + payload + bytes([crc8(payload)])


def decode(ftype, p):
    """Decode selected CRSF payloads into human-readable text.

    :param int ftype: CRSF frame type.
    :param bytes p: Frame payload without type and CRC bytes.
    :return: Decoded summary, or an empty string for unsupported data.
    :rtype: str
    """
    try:
        if ftype == 0x21:
            return "mode=" + p.split(b"\0")[0].decode(errors="replace")
        if ftype == 0x08:
            v, a = struct.unpack(">HH", p[:4])
            return f"{v / 10:.1f}V {a / 10:.1f}A"
        if ftype == 0x1E:
            pitch, roll, yaw = struct.unpack(">hhh", p[:6])
            return f"pitch={pitch / 10000:+.2f} roll={roll / 10000:+.2f} yaw={yaw / 10000:+.2f} rad"
        if ftype == 0x02:
            lat, lon, spd, hdg, alt, sats = struct.unpack(">iiHHHB", p[:15])
            return f"{lat / 1e7:.6f},{lon / 1e7:.6f} sats={sats} alt={alt - 1000}m"
        if ftype == 0x14:
            return f"rssi1=-{p[0]} lq={p[2]}% snr={struct.unpack('b', p[3:4])[0]} dl_lq={p[8]}%"
    except Exception:
        pass
    return ""


class Parser:
    """Incremental CRSF stream parser and statistics collector."""

    def __init__(self):
        """Initialize an empty parser and zero all counters."""
        self.buf = bytearray()
        self.frames = {}
        self.latest = {}
        self.bad_crc = 0
        self.raw = 0

    def feed(self, data):
        """Consume raw UART bytes and extract all complete valid frames.

        :param bytes data: Newly received stream data.
        """
        self.raw += len(data)
        self.buf += data
        while True:
            while self.buf and self.buf[0] not in CRSF_SYNC:
                del self.buf[0]
            if len(self.buf) < 2:
                return
            length = self.buf[1]
            if length < 2 or length > 62:
                del self.buf[0]
                continue
            if len(self.buf) < length + 2:
                return
            frame = bytes(self.buf[: length + 2])
            if crc8(frame[2:-1]) == frame[-1]:
                ftype = frame[2]
                self.frames[ftype] = self.frames.get(ftype, 0) + 1
                text = decode(ftype, frame[3:-1])
                if text:
                    self.latest[ftype] = text
                del self.buf[: length + 2]
            else:
                self.bad_crc += 1
                del self.buf[0]

    def report(self):
        """Create the current one-line statistics and telemetry report.

        :return: Human-readable report text.
        :rtype: str
        """
        parts = [f"rx {self.raw} B"]
        for t, n in sorted(self.frames.items()):
            parts.append(f"{FRAME_NAMES.get(t, f'0x{t:02X}')}={n}")
        parts.append(f"bad_crc={self.bad_crc}")
        line = "  ".join(parts)
        if self.latest:
            line += "\n    " + "  ".join(self.latest[t] for t in sorted(self.latest))
        uav = any(t in FROM_UAV for t in self.frames)
        return line + ("\n    UAV telemetry present: YES" if uav else "")


def main():
    """Run the UDP/TCP CRSF monitor command-line application."""
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--udp", type=int, metavar="PORT", help="listen on this UDP port")
    g.add_argument("--tcp-server", type=int, metavar="PORT", help="wait for CH9121 (TCP CLIENT mode)")
    g.add_argument("--tcp", nargs=2, metavar=("HOST", "PORT"), help="connect to CH9121 (TCP SERVER mode)")
    ap.add_argument("--to", nargs=2, metavar=("HOST", "PORT"), help="UDP only: CH9121 IP and its local port")
    ap.add_argument("--rc", type=float, metavar="HZ", default=0, help="send RC_CHANNELS frames at this rate")
    args = ap.parse_args()

    if args.udp is not None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("", args.udp))
        print(f"UDP listening on *:{args.udp}")
        dest = (args.to[0], int(args.to[1])) if args.to else None
        if args.rc and not dest:
            sys.exit("--rc over UDP needs --to CH9121_IP PORT")
        recv = lambda: sock.recvfrom(4096)[0]
        send = lambda d: sock.sendto(d, dest)
    elif args.tcp_server is not None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("", args.tcp_server))
        srv.listen(1)
        print(f"TCP waiting for CH9121 on *:{args.tcp_server} ...")
        sock, peer = srv.accept()
        print(f"connected from {peer}")
        recv = lambda: sock.recv(4096)
        send = sock.sendall
    else:
        sock = socket.create_connection((args.tcp[0], int(args.tcp[1])))
        print(f"TCP connected to {args.tcp[0]}:{args.tcp[1]}")
        recv = lambda: sock.recv(4096)
        send = sock.sendall

    period = 1.0 / args.rc if args.rc else None
    sock.settimeout(min(period or 1.0, 1.0) / 2)
    rc_frame = build_rc_frame()
    sent = 0
    p = Parser()
    next_rc = time.time()
    last = time.time()
    if period:
        print(f"sending RC_CHANNELS at {args.rc:g} Hz (throttle low, switches low)")

    while True:
        try:
            data = recv()
            if not data:
                sys.exit("connection closed")
            p.feed(data)
        except socket.timeout:
            pass
        now = time.time()
        if period and now >= next_rc:
            send(rc_frame)
            sent += 1
            next_rc += period
        if now - last >= 1.0:
            prefix = f"tx {sent} RC  " if period else ""
            print(prefix + p.report())
            last = now


if __name__ == "__main__":
    main()
