#!/usr/bin/env python3
"""Send a STM32 .bin to the YD-ESP32-23 programmer firmware."""

import argparse
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing. Run: ~/.platformio/penv/bin/python tools/flash_via_esp32.py")


def reset_to_run(ser):
    # CH343 DTR/RTS auto-reset: DTR=GPIO0, RTS=EN, both active-low through transistors.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.05)


def read_until(ser, pred, timeout):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += ser.read(256)
        lines = buf.split(b"\n")
        buf = lines[-1]
        for line in lines[:-1]:
            text = line.decode(errors="replace").strip()
            if text:
                print(text)
            if pred(text):
                return text
    raise SystemExit("timed out waiting for the programmer")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--ping", action="store_true")
    parser.add_argument("--id", action="store_true")
    parser.add_argument("--gen", type=int, metavar="BAUD",
                        help="stream a valid CRSF RC_CHANNELS frame on GPIO17 at BAUD 8N1")
    parser.add_argument("--seconds", type=int, default=20, help="duration for --gen")
    parser.add_argument("bin", nargs="?", help="STM32 firmware.bin")
    args = parser.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.2)
    time.sleep(0.1)
    ser.reset_input_buffer()
    reset_to_run(ser)
    read_until(ser, lambda s: s == "READY", 4)

    if args.ping:
        ser.write(b"PING\n")
        read_until(ser, lambda s: s == "PONG" or s.startswith("ERR"), 2)
        return

    if args.gen:
        ser.write(f"GEN {args.gen} {args.seconds}\n".encode())
        read_until(ser, lambda s: s.startswith("GEN done") or s.startswith("ERR"), args.seconds + 5)
        return

    if args.id or not args.bin:
        ser.write(b"ID\n")
        read_until(ser, lambda s: s.startswith("ID ") or s.startswith("ERR"), 3)
        return

    data = open(args.bin, "rb").read()
    if not data or len(data) > 32 * 1024:
        sys.exit("firmware must be 1..32768 bytes")
    crc = zlib.crc32(data) & 0xFFFFFFFF
    ser.write(f"FLASH {len(data)} {crc:08x}\n".encode())
    ser.write(data)
    ser.flush()
    read_until(ser, lambda s: s.startswith("DONE") or s.startswith("ERR"), 30)


if __name__ == "__main__":
    main()
