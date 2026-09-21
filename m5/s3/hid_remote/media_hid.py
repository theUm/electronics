# SPDX-License-Identifier: GPL-3.0-or-later
"""Consumer Control and keyboard extension of MicroPythonBLEHID."""
import bluetooth
import esp32
import json
import struct
import time
import binascii
from hid_services import HumanInterfaceDevice, Advertiser
from hid_keystores import KeyStore

# Report ID 1 is a 2-byte Consumer Control report. Report ID 2 is a
# conventional 8-byte keyboard report used for the left/right arrow keys.
# The layout follows Espressif's BLE HID Consumer Control example: the two
# volume controls occupy bits 6/7 of byte 0. The Consumer button array in
# byte 1 follows the declared Usage order; Play/Pause (0xCD) is entry 13.
REPORT_MAP = bytes((
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x02, 0xA1, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0A,
    0x15, 0x01, 0x25, 0x0A, 0x75, 0x04, 0x95, 0x01, 0x81, 0x00,
    0xC0,
    0x05, 0x0C, 0x09, 0x86, 0x15, 0xFF, 0x25, 0x01,
    0x75, 0x02, 0x95, 0x01, 0x81, 0x46,
    0x09, 0xE9, 0x09, 0xEA,
    0x15, 0x00, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x09, 0xE2, 0x09, 0x30, 0x09, 0x83, 0x09, 0x81,
    0x09, 0xB0, 0x09, 0xB1, 0x09, 0xB2, 0x09, 0xB3,
    0x09, 0xB4, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7,
    0x09, 0xCD,
    0x15, 0x01, 0x25, 0x0D, 0x75, 0x04, 0x95, 0x01, 0x81, 0x00,
    0x09, 0x80, 0xA1, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x01, 0x25, 0x03, 0x75, 0x02, 0x81, 0x00,
    0xC0,
    0x81, 0x03,
    0xC0,
    # Keyboard Application Collection, Report ID 2.
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x15, 0x00, 0x25, 0x65, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
    0xC0,
))
REPORTS = {
    0xE9: bytes((0x40, 0x00)),  # Volume Increment
    0xEA: bytes((0x80, 0x00)),  # Volume Decrement
    0xCD: bytes((0x00, 0x0D)),  # Play/Pause in the Consumer button array
}
KEYBOARD_RELEASE = b'\x00' * 8
KEYBOARD_REPORTS = {
    0x50: b'\x00\x00\x50\x00\x00\x00\x00\x00',  # Left Arrow
    0x4F: b'\x00\x00\x4f\x00\x00\x00\x00\x00',  # Right Arrow
}

# Give a bonded host a chance to restore link encryption by itself. Starting a
# new pairing procedure immediately on every connection can race that restore
# and make macOS show a pairing prompt again instead of reconnecting normally.
PAIR_FALLBACK_MS = 1200
BONDED_PAIR_FALLBACK_MS = 4000
PAIR_RETRY_MS = 2500
PAIR_RESULT_TIMEOUT_MS = 10000
MAX_PAIR_REQUESTS = 2
LOCAL_IRK_SECRET_TYPE = 10
FAST_ADV_US = 100000
SLOW_ADV_US = 500000
SLOW_ADV_AFTER_MS = 30000


class BondStore(KeyStore):
    """Defer flash writes out of BLE callbacks; use an application namespace."""
    def __init__(self):
        super().__init__()
        self.nvs = esp32.NVS('hidremote')
        self.dirty = False

    def load_secrets(self):
        try:
            size = self.nvs.get_blob('keys', bytearray())
        except OSError:
            return
        data = bytearray(size)
        self.nvs.get_blob('keys', data)
        self.add_json_secrets(json.loads(data.decode()))
        print('BLE secret types loaded:', sorted(set(kind for kind, _ in self.secrets)))

    def save_secrets(self):
        self.dirty = True

    def flush(self):
        if self.dirty:
            entries = [(kind, binascii.b2a_base64(key).decode().strip(),
                        binascii.b2a_base64(value).decode().strip())
                       for (kind, key), value in self.secrets.items()]
            self.nvs.set_blob('keys', json.dumps(entries).encode())
            self.nvs.commit()
            self.dirty = False


class MediaRemote(HumanInterfaceDevice):
    def __init__(self, name='M5 Media Remote'):
        super().__init__(name)
        self.set_keystore(BondStore())
        self.set_device_information('DIY', 'StickS3 Media', '1')
        self.set_device_revision('0.2', 'StickS3', 'UiFlow2')
        self.mitm = False
        self.pending_release = None
        self.link_since = None
        self.security_started = False
        self.pair_attempts = 0
        self.pair_requested_at = None
        self.pair_wait_ms = PAIR_FALLBACK_MS
        self.advertising_since = None
        self.advertising_slow = False
        self.epoch = 0
        self.suspended = False
        self.protocol_mode = 1
        self.queue = []
        self.diagnostics = []
        self.secret_hits = 0
        self.secret_misses = 0
        read = bluetooth.FLAG_READ
        # UiFlow omits these names, although the MicroPython BLE flag bits
        # retain their standard values. Also enforce access in the read IRQ.
        protected_read = read | getattr(bluetooth, 'FLAG_READ_ENCRYPTED', 0x0200)
        write = bluetooth.FLAG_WRITE | bluetooth.FLAG_WRITE_NO_RESPONSE
        U = bluetooth.UUID
        self.HIDS = (U(0x1812), (
            (U(0x2A4A), read),
            (U(0x2A4B), read),
            (U(0x2A4C), write | getattr(bluetooth, 'FLAG_WRITE_ENCRYPTED', 0x1000)),
            (U(0x2A4D), protected_read | bluetooth.FLAG_NOTIFY,
             ((U(0x2908), read),)),
            (U(0x2A4D), protected_read | bluetooth.FLAG_NOTIFY,
             ((U(0x2908), read),)),
            (U(0x2A4E), read | bluetooth.FLAG_WRITE_NO_RESPONSE),
            (U(0x2A22), protected_read | bluetooth.FLAG_NOTIFY),
            (U(0x2A32), protected_read | write),
        ))
        # The keyboard collection also needs Boot Keyboard characteristics.
        self.services = [self.DIS, self.BAS, self.HIDS]

    def _remember(self, event, detail):
        # Keep IRQ work small and never retain addresses or bonding material.
        if len(self.diagnostics) >= 32:
            self.diagnostics.pop(0)
        self.diagnostics.append((event, detail))

    def _has_peer_bond(self):
        # Type 10 is the controller's own IRK; it is not a Windows/Mac LTK.
        return any(kind != LOCAL_IRK_SECRET_TYPE for kind, _ in self.secrets.secrets)

    def start(self):
        self._ble.active(False)  # Own BLE while this app is running.
        super().start()
        self._ble.config(addr_mode=0)  # Stable public identity across restarts.
        handles = self._ble.gatts_register_services(self.services)
        self.h_ctrl = handles[2][2]
        self.h_consumer_report = handles[2][3]
        self.h_keyboard_report = handles[2][5]
        self.h_protocol = handles[2][7]
        self.h_boot_keyboard_input = handles[2][8]
        self.h_boot_keyboard_output = handles[2][9]
        self.h_rep = self.h_consumer_report  # Compatibility with older tools.
        values = (
            'StickS3 Media', '1', '0.2', 'StickS3', 'UiFlow2', 'DIY',
            struct.pack('<BHHH', 1, 0xFFFF, 1, 0x0101),
        )
        for handle, value in zip(handles[0], values):
            self.characteristics[handle] = ('Device information', value.encode() if isinstance(value, str) else value)
        self.h_bat, h_format = handles[1]
        self.characteristics[self.h_bat] = ('Battery level', bytes((self.battery_level,)))
        self.characteristics[h_format] = ('Battery format', b'\x04\x00\xad\x27\x01\x00\x00')
        (h_info, h_map, h_ctrl, h_consumer, h_consumer_ref,
         h_keyboard, h_keyboard_ref, h_protocol,
         h_boot_keyboard_input, h_boot_keyboard_output) = handles[2]
        for handle, value in (
            (h_info, b'\x11\x01\x00\x02'), (h_map, REPORT_MAP),
            (h_ctrl, b'\x00'),
            (h_consumer, b'\x00\x00'), (h_consumer_ref, b'\x01\x01'),
            (h_keyboard, KEYBOARD_RELEASE), (h_keyboard_ref, b'\x02\x01'),
            (h_protocol, b'\x01'),
            (h_boot_keyboard_input, KEYBOARD_RELEASE),
            (h_boot_keyboard_output, b'\x00'),
        ):
            self.characteristics[handle] = ('HID', value)
        self.write_service_characteristics()
        self.adv = Advertiser(self._ble, [bluetooth.UUID(0x1812)], 960, self.device_name)
        self._start_fast_advertising(time.ticks_ms())
        print('BLE advertising:', self.device_name)

    def _start_fast_advertising(self, now):
        self.adv.start_advertising(FAST_ADV_US)
        self.set_state(self.DEVICE_ADVERTISING)
        self.advertising_since = now
        self.advertising_slow = False

    def ble_irq(self, event, data):
        if event == 1:
            if self.conn_handle is not None:
                self._ble.gap_disconnect(data[0])
                return
            self.link_since = time.ticks_ms()
            self.security_started = False
            self.pair_attempts = 0
            self.pair_requested_at = None
            self.pair_wait_ms = (BONDED_PAIR_FALLBACK_MS if self._has_peer_bond()
                                 else PAIR_FALLBACK_MS)
            self.epoch += 1
            self.adv.advertising = False
            self.adv.interval_us = None
            self.advertising_since = None
            self.advertising_slow = False
            self.queue = []
            self.pending_release = None
            self.suspended = False
            self.protocol_mode = 1
            self._ble.gatts_write(self.h_protocol, b'\x01')
            self._remember('connect', len(self.secrets.secrets))
        elif event == 2:
            if data[0] != self.conn_handle:
                return
            self.queue = []
            self.pending_release = None
            self.link_since = None
            self.security_started = False
            self.pair_requested_at = None
            self.epoch += 1
            self._ble.gatts_write(self.h_consumer_report, b'\x00\x00')
            self._ble.gatts_write(self.h_keyboard_report, KEYBOARD_RELEASE)
            self._ble.gatts_write(self.h_boot_keyboard_input, KEYBOARD_RELEASE)
            self.protocol_mode = 1
            self._remember('disconnect', 0)
        elif event == 3:
            if data[1] == self.h_ctrl:
                self.suspended = self._ble.gatts_read(self.h_ctrl) == b'\x00'
                self.queue = []
                self.pending_release = None
                self._remember('suspend' if self.suspended else 'resume', 0)
            elif data[1] == self.h_protocol:
                value = self._ble.gatts_read(self.h_protocol)
                if value in (b'\x00', b'\x01'):
                    self.protocol_mode = value[0]
                    self.queue = []
                    self.pending_release = None
                    self._remember('protocol', self.protocol_mode)
        elif event == 4:
            # NimBLE enforces encrypted report access through GATT flags.
            # Permit discovery reads before pairing (required by HID hosts).
            if data[0] != self.conn_handle:
                return 2
            if data[1] in (self.h_consumer_report, self.h_keyboard_report,
                           self.h_boot_keyboard_input, self.h_boot_keyboard_output) and not self.encrypted:
                return 0x0F
            return 0
        elif event == 31:
            # Record the action category only. Never retain the passkey or
            # numeric comparison value supplied by the BLE stack.
            self._remember('passkey action', data[1])
        result = super().ble_irq(event, data)
        if event == 28 and data[0] == self.conn_handle:
            self._remember('encryption', (self.encrypted, self.bonded, self.key_size))
            if self.encrypted:
                self.security_started = False
                self.pair_requested_at = None
            elif self.pair_attempts:
                self.security_started = False
                self.link_since = time.ticks_ms()
                self.pair_wait_ms = PAIR_RETRY_MS
                self._remember('security retry armed', self.pair_attempts)
        elif event == 29:
            if result is None:
                self.secret_misses += 1
            else:
                self.secret_hits += 1
            self._remember('bond lookup', 'hit' if result is not None else 'miss')
        elif event == 30:
            self._remember('bond update', (data[0], data[2] is not None,
                                           len(self.secrets.secrets)))
        if event == 2:
            # A connection stops advertising in the controller. Advertise
            # again immediately after a disconnect so a bonded host can find
            # and reconnect to the remote without restarting the StickS3.
            self._start_fast_advertising(time.ticks_ms())
        return result

    def ready(self):
        # MicroPython NimBLE consumes CCCD subscription events internally;
        # they are NOT emitted as GATTS_WRITE. Requiring one deadlocks HID.
        return self.is_connected() and self.encrypted and not self.suspended

    def status(self):
        if not self.is_connected():
            return 'Pair via BT'
        if not self.encrypted:
            return 'Pairing...'
        if self.suspended:
            return 'Suspended'
        if not self.bonded or not self._has_peer_bond():
            return 'No bond'
        return 'Connected'

    def send_usage(self, usage):
        if usage not in REPORTS:
            raise ValueError('Unsupported media usage')
        if not self.ready() or self.protocol_mode == 0 or len(self.queue) >= 4:
            return False
        self.queue.append((self.h_consumer_report, REPORTS[usage], b'\x00\x00'))
        return True

    def send_key(self, usage):
        if usage not in KEYBOARD_REPORTS:
            raise ValueError('Unsupported keyboard usage')
        if not self.ready() or len(self.queue) >= 4:
            return False
        handle = (self.h_boot_keyboard_input if self.protocol_mode == 0
                  else self.h_keyboard_report)
        self.queue.append((handle, KEYBOARD_REPORTS[usage], KEYBOARD_RELEASE))
        return True

    def send_action(self, usage):
        if usage in REPORTS:
            return self.send_usage(usage)
        if usage in KEYBOARD_REPORTS:
            return self.send_key(usage)
        raise ValueError('Unsupported HID usage')

    def volume_up(self):
        return self.send_usage(0xE9)

    def volume_down(self):
        return self.send_usage(0xEA)

    def play_pause(self):
        return self.send_usage(0xCD)

    def arrow_left(self):
        return self.send_key(0x50)

    def arrow_right(self):
        return self.send_key(0x4F)

    def _report(self, handle, report):
        self._ble.gatts_write(handle, report)
        self._ble.gatts_notify(self.conn_handle, handle, report)

    def poll(self, now):
        self.secrets.flush()
        while self.diagnostics:
            event, detail = self.diagnostics.pop(0)
            print('BLE', event, detail)
        if self.get_state() == self.DEVICE_IDLE:
            self._start_fast_advertising(now)
        elif (self.get_state() == self.DEVICE_ADVERTISING and
              not self.advertising_slow and self.advertising_since is not None and
              time.ticks_diff(now, self.advertising_since) >= SLOW_ADV_AFTER_MS):
            self.adv.start_advertising(SLOW_ADV_US)
            self.advertising_slow = True
            print('BLE advertising interval:', SLOW_ADV_US)
        if (self.is_connected() and not self.encrypted and self.security_started and
                self.pair_requested_at is not None and
                time.ticks_diff(now, self.pair_requested_at) >= PAIR_RESULT_TIMEOUT_MS):
            self.security_started = False
            self.link_since = now
            self.pair_wait_ms = PAIR_RETRY_MS
            print('BLE security request timed out')
        if (self.is_connected() and not self.encrypted and
                not self.security_started and self.pair_attempts < MAX_PAIR_REQUESTS and
                self.link_since is not None and
                time.ticks_diff(now, self.link_since) >= self.pair_wait_ms):
            # Initial pairing still needs an explicit security request on this
            # UiFlow build. Delay it so an existing bond can resume first.
            self.security_started = True
            self.pair_attempts += 1
            self.pair_requested_at = now
            try:
                self._ble.gap_pair(self.conn_handle)
                print('BLE pair request sent', self.pair_attempts)
            except OSError as exc:
                print('BLE pair request failed', self.pair_attempts, repr(exc))
        if not self.ready():
            self.queue = []
            return
        try:
            if self.pending_release is not None:
                handle, release, pressed_at = self.pending_release
                if time.ticks_diff(now, pressed_at) >= 40:
                    self._report(handle, release)
                    self.pending_release = None
            elif self.queue:
                handle, press, release = self.queue.pop(0)
                self._report(handle, press)
                self.pending_release = (handle, release, now)
        except OSError:
            self.queue = []
            # Keep retrying a release; never replay a missed press.
            if self.pending_release is not None:
                handle, release, _ = self.pending_release
                self.pending_release = (handle, release, now)

    def stop(self):
        self.secrets.flush()
        try:
            if self.ready():
                self._report(self.h_consumer_report, b'\x00\x00')
                self._report(self.h_keyboard_report, KEYBOARD_RELEASE)
                self._report(self.h_boot_keyboard_input, KEYBOARD_RELEASE)
        finally:
            super().stop()
