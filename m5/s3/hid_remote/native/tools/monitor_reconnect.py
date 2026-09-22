"""Read StickS3 USB logs across a physical power cycle."""

import argparse
import time

import serial
from serial.tools import list_ports


def find_stick_port():
    for port in list_ports.comports():
        if (port.vid, port.pid) == (0x303A, 0x1001):
            return port.device
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=120)
    args = parser.parse_args()
    deadline = time.monotonic() + args.seconds
    port = None
    handle = None
    while time.monotonic() < deadline:
        current = find_stick_port()
        if current != port:
            if handle:
                handle.close()
                handle = None
            port = current
            print(f"[{time.strftime('%H:%M:%S')}] USB {port or 'disconnected'}", flush=True)
        if port and handle is None:
            try:
                handle = serial.Serial(port, 115200, timeout=0.2, dsrdtr=False, rtscts=False)
                handle.dtr = False
                handle.rts = False
            except serial.SerialException:
                handle = None
        if handle:
            try:
                data = handle.read(4096)
                if data:
                    print(data.decode(errors="replace"), end="", flush=True)
            except serial.SerialException:
                handle.close()
                handle = None
        else:
            time.sleep(0.2)
    if handle:
        handle.close()


if __name__ == "__main__":
    main()
