#!/usr/bin/env python3
"""USB MicroPython raw-REPL utility; uses PlatformIO's bundled pyserial."""
import argparse
import hashlib
from pathlib import Path
import time

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parent
APP_ENTRY = '/flash/apps/hid_remote.py'
PROBE_ENTRY = '/flash/apps/bond_probe.py'
MAIN_ENTRY = '/flash/main.py'
MAIN_BACKUP = '/flash/main.py.uiflow-backup'
STICK_USB_IDS = {(0x303A, 0x832B)}


def resolve_port(requested=None, ports=None):
    if requested:
        return requested
    available = list(list_ports.comports() if ports is None else ports)
    sticks = [port for port in available
              if (port.vid, port.pid) in STICK_USB_IDS or
              'StickS3' in (port.description or '')]
    if len(sticks) == 1:
        return sticks[0].device
    candidates = ', '.join(port.device for port in available) or 'none'
    if sticks:
        raise RuntimeError('Multiple StickS3 ports; specify --port. Available: ' + candidates)
    raise RuntimeError('StickS3 USB port not found; specify --port. Available: ' + candidates)


class Device:
    def __init__(self, port):
        self.serial = serial.Serial(
            port,
            115200,
            timeout=0.1,
            write_timeout=5,
            dsrdtr=False,
            rtscts=False,
        )
        # StickS3 exposes USB CDC control lines.  Keep them inactive so
        # closing pyserial does not toggle the board's reset path.
        self.serial.dtr = False
        self.serial.rts = False

    def until(self, suffix, timeout=10):
        data = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data.extend(self.serial.read(1))
            if data.endswith(suffix):
                return bytes(data[:-len(suffix)])
        raise TimeoutError(repr(bytes(data)[-1000:]))

    def enter(self):
        self.serial.write(b'\r\x03\x03')
        time.sleep(0.3)
        self.serial.reset_input_buffer()
        self.serial.write(b'\r\x01')
        self.until(b'raw REPL; CTRL-B to exit\r\n>')

    def send(self, source):
        data = source.encode()
        for offset in range(0, len(data), 256):
            self.serial.write(data[offset:offset + 256])
            time.sleep(0.005)
        self.serial.write(b'\x04')
        self.until(b'OK')

    def execute(self, source, timeout=10):
        self.send(source)
        out = self.until(b'\x04', timeout)
        err = self.until(b'\x04', timeout)
        self.until(b'>', timeout)
        if err:
            raise RuntimeError(err.decode(errors='replace'))
        return out.decode(errors='replace')

    def put(self, local, remote):
        data = local.read_bytes()
        self.execute("_f=open(%r,'wb')" % (remote + '.tmp'))
        for offset in range(0, len(data), 512):
            self.execute('_f.write(%r)' % data[offset:offset + 512])
        self.execute('_f.close()')
        actual = self.execute("import hashlib, binascii; print(binascii.hexlify(hashlib.sha256(open(%r,'rb').read()).digest()).decode())" % (remote + '.tmp')).strip()
        if actual != hashlib.sha256(data).hexdigest():
            raise RuntimeError('Upload checksum mismatch: ' + remote)
        self.execute("import os; os.rename(%r,%r)" % (remote + '.tmp', remote))
        print('Verified:', remote)

    def close(self):
        self.serial.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='Serial port (e.g. COM5 or /dev/cu.usbmodem2101); auto-detect by default')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('probe')
    ex = sub.add_parser('exec')
    ex.add_argument('code')
    sub.add_parser('upload')
    sub.add_parser('install', help='Upload the remote and make it start at boot')
    sub.add_parser('app', help='Boot directly into the already installed remote')
    sub.add_parser('menu', help='Boot into the UiFlow launcher without deleting the remote')
    sub.add_parser('stop')
    sub.add_parser('forget', help='Erase only this app\'s BLE bonds; then remove it on the host too')
    run = sub.add_parser('run')
    run.add_argument(
        '--seconds',
        type=float,
        default=0,
        help='Show logs for this many seconds; 0 keeps the USB session open until Ctrl-C',
    )
    bond_probe = sub.add_parser('bond-probe', help='Run isolated BLE bond diagnostic')
    bond_probe.add_argument('--seconds', type=float, default=0)
    mon = sub.add_parser('monitor')
    mon.add_argument('--seconds', type=float, default=10)
    imu = sub.add_parser('imu', help='Print raw accelerometer axes for a short diagnostic run')
    imu.add_argument('--seconds', type=float, default=20)
    args = parser.parse_args()
    args.port = resolve_port(args.port)
    print('USB port:', args.port, flush=True)
    dev = Device(args.port)
    try:
        if args.command != 'monitor':
            dev.enter()
        if args.command == 'probe':
            print(dev.execute("import os, sys, M5, bluetooth, esp32\nprint(sys.implementation)\nprint('apps', os.listdir('/flash/apps'))\nprint('main.py', open('/flash/main.py').read())\nprint('boot.py', open('/flash/boot.py').read())\nprint('M5', dir(M5))\nprint('BtnA', dir(M5.BtnA))\nprint('Imu', dir(M5.Imu))\nprint('NVS', hasattr(esp32,'NVS'))\nprint('BLE', dir(bluetooth.BLE()))"))
        elif args.command == 'exec':
            print(dev.execute(args.code))
        elif args.command in ('upload', 'install'):
            for path in ['/flash/libs/hid_remote']:
                dev.execute("import os\ntry:\n os.mkdir(%r)\nexcept OSError:\n pass" % path)
            for name in ['hid_services.py', 'hid_keystores.py']:
                dev.put(ROOT / 'vendor' / name, '/flash/libs/hid_remote/' + name)
            for name in ['remote_input.py', 'media_hid.py', 'remote_app.py']:
                dev.put(ROOT / name, '/flash/libs/hid_remote/' + name)
            dev.put(ROOT / 'hid_remote.py', APP_ENTRY)
            if args.command == 'install':
                print(dev.execute(
                    "import os\n"
                    "try:\n"
                    " os.stat(%r)\n"
                    " print('Keeping existing UiFlow main.py backup')\n"
                    "except OSError:\n"
                    " _src=open(%r,'rb')\n"
                    " _dst=open(%r,'wb')\n"
                    " _dst.write(_src.read())\n"
                    " _src.close()\n"
                    " _dst.close()\n"
                    " print('Saved original main.py backup')\n"
                    % (MAIN_BACKUP, MAIN_ENTRY, MAIN_BACKUP)
                ), end='')
                dev.put(ROOT / 'hid_remote.py', MAIN_ENTRY)
                print(dev.execute(
                    "import esp32\n"
                    "_nvs=esp32.NVS('uiflow')\n"
                    "_nvs.set_u8('boot_option',0)\n"
                    "_nvs.commit()\n"
                    "print('Boot mode: M5 Media Remote')"
                ), end='')
                dev.send("import machine; machine.reset()")
        elif args.command == 'stop':
            print('Application interrupted; Ctrl-B returns to the Python prompt.')
        elif args.command == 'forget':
            print(dev.execute("import esp32\n_nvs=esp32.NVS('hidremote')\ntry:\n _nvs.erase_key('keys')\n _nvs.commit()\nexcept OSError:\n pass\nprint('Remote bonds cleared; remove M5 Media Remote on the computer before pairing again.')"))
        elif args.command == 'bond-probe':
            dev.put(ROOT / 'bond_probe.py', PROBE_ENTRY)
            dev.send("exec(open(%r).read(), {'__name__':'__main__'})" % PROBE_ENTRY)
        elif args.command == 'run':
            dev.send("exec(open(%r).read(), {'__name__':'__main__'})" % APP_ENTRY)
        elif args.command in ('app', 'menu'):
            boot_option = 0 if args.command == 'app' else 1
            label = 'M5 Media Remote' if args.command == 'app' else 'UiFlow launcher'
            print(dev.execute(
                "import esp32\n"
                "_nvs=esp32.NVS('uiflow')\n"
                "_nvs.set_u8('boot_option',%d)\n"
                "_nvs.commit()\n"
                "print('Boot mode: %s')" % (boot_option, label)
            ), end='')
            dev.send("import machine; machine.reset()")
        elif args.command == 'imu':
            duration_ms = max(1, int(args.seconds * 1000))
            dev.send(
                "import M5,time\n"
                "M5.begin()\n"
                "_imu_until=time.ticks_add(time.ticks_ms(), %d)\n"
                "while time.ticks_diff(_imu_until,time.ticks_ms()) > 0:\n"
                " print('IMU', M5.Imu.getAccel())\n"
                " time.sleep_ms(100)\n" % duration_ms
            )
        if args.command in ('run', 'bond-probe', 'monitor'):
            log = bytearray()
            if args.command in ('run', 'bond-probe') and args.seconds <= 0:
                print('USB session is kept open; press Ctrl-C after pairing/testing.', flush=True)
                while True:
                    data = dev.serial.read(4096)
                    if data:
                        log.extend(data)
                        print(data.decode(errors='replace'), end='', flush=True)
            else:
                deadline = time.monotonic() + args.seconds
                while time.monotonic() < deadline:
                    data = dev.serial.read(4096)
                    if data:
                        log.extend(data)
                        print(data.decode(errors='replace'), end='', flush=True)
            if b'Traceback (most recent call last)' in log:
                raise RuntimeError('The device application failed; see traceback above.')
        elif args.command == 'imu':
            deadline = time.monotonic() + args.seconds + 2
            while time.monotonic() < deadline:
                data = dev.serial.read(4096)
                if data:
                    print(data.decode(errors='replace'), end='', flush=True)
        elif args.command in ('probe', 'exec', 'upload', 'stop', 'forget'):
            dev.serial.write(b'\x02')
    finally:
        dev.close()


if __name__ == '__main__':
    main()
