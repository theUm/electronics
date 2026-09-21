# SPDX-License-Identifier: GPL-3.0-or-later
"""Temporary BLE bonding probe; uses its own identity and NVS namespace."""
import sys
import time
import bluetooth
import esp32
import M5

LIB_DIR = '/flash/libs/hid_remote'
if LIB_DIR not in sys.path:
    sys.path.insert(0, LIB_DIR)

from hid_keystores import KeyStore
from media_hid import BondStore

NAME = b'M5 Bond Probe'
SERVICE = bluetooth.UUID('7e28b110-51f3-4b8e-a799-32745543a001')
VALUE = bluetooth.UUID('7e28b110-51f3-4b8e-a799-32745543a002')


class ProbeStore(BondStore):
    def __init__(self):
        # Reuse the remote's serialization and deferred writes, but never
        # open or change its own hidremote namespace.
        KeyStore.__init__(self)
        self.nvs = esp32.NVS('hidprobe')
        self.dirty = False


def run():
    store = ProbeStore()
    store.load_secrets()
    ble = bluetooth.BLE()
    ble.active(False)
    state = {'conn': None, 'since': None, 'pair_sent': False,
             'encrypted': False, 'bonded': False}

    def advertise():
        payload = b'\x02\x01\x06' + bytes((len(NAME) + 1, 0x09)) + NAME
        ble.gap_advertise(100000, adv_data=payload)
        print('PROBE advertising')

    def irq(event, data):
        if event == 1:  # central connect
            state['conn'] = data[0]
            state['since'] = time.ticks_ms()
            state['pair_sent'] = False
            state['encrypted'] = False
            state['bonded'] = False
            print('PROBE connect')
        elif event == 2:  # central disconnect
            state['conn'] = None
            state['since'] = None
            state['encrypted'] = False
            state['bonded'] = False
            print('PROBE disconnect')
            advertise()
        elif event == 28:  # encryption state
            if data[0] == state['conn']:
                state['encrypted'] = bool(data[1])
                state['bonded'] = bool(data[3])
                print('PROBE encryption', data[1], data[3], data[4])
        elif event == 29:  # get secret
            kind, index, key = data
            result = store.get_secret(kind, index, key)
            print('PROBE secret lookup', kind, result is not None)
            return result
        elif event == 30:  # set or delete secret
            kind, key, value = data
            if value is None:
                if not store.has_secret(kind, key):
                    print('PROBE secret delete', kind, False)
                    return False
                store.remove_secret(kind, key)
            else:
                store.add_secret(kind, key, value)
            store.save_secrets()
            print('PROBE secret update', kind, value is not None,
                  len(store.secrets))
            return True
        elif event == 31:  # passkey action: never print the number
            print('PROBE passkey action', data[1])

    M5.begin()
    M5.Lcd.setRotation(0)
    if hasattr(M5.Lcd, 'powerSaveOff'):
        M5.Lcd.powerSaveOff()
    M5.Lcd.setBrightness(60)
    M5.Lcd.fillScreen(0x101820)
    M5.Lcd.setTextColor(0xFFFFFF, 0x101820)
    M5.Lcd.drawString('M5 BOND PROBE', 5, 12)
    M5.Lcd.drawString('Pair in Windows', 5, 46)

    ble.irq(irq)
    ble.active(True)
    ble.config(gap_name=NAME.decode())
    ble.config(addr_mode=1)  # Separate random-static identity from the remote.
    ble.config(bond=True)
    ble.config(le_secure=True)
    ble.config(mitm=False)
    ble.config(io=3)  # No input/output, like the remote.
    print('PROBE address mode', ble.config('mac')[0])  # Never print address.
    ((handle,),) = ble.gatts_register_services((
        (SERVICE, ((VALUE, bluetooth.FLAG_READ |
                     getattr(bluetooth, 'FLAG_READ_ENCRYPTED', 0x0200)),)),
    ))
    ble.gatts_write(handle, b'probe')
    advertise()
    previous = None
    try:
        while True:
            store.flush()
            now = time.ticks_ms()
            if (state['conn'] is not None and not state['encrypted'] and
                    not state['pair_sent'] and state['since'] is not None and
                    time.ticks_diff(now, state['since']) >= 1200):
                state['pair_sent'] = True
                try:
                    ble.gap_pair(state['conn'])
                    print('PROBE pair request sent')
                except OSError as exc:
                    print('PROBE pair request failed', repr(exc))
            display = ('CONNECTED' if state['encrypted'] else
                       'PAIRING' if state['conn'] is not None else 'ADVERTISING')
            snapshot = (display, len(store.secrets))
            if snapshot != previous:
                M5.Lcd.fillRect(0, 80, 135, 70, 0x101820)
                M5.Lcd.drawString(display, 5, 82)
                M5.Lcd.drawString('Keys: %d' % snapshot[1], 5, 112)
                previous = snapshot
            time.sleep_ms(50)
    finally:
        store.flush()
        ble.active(False)
        print('PROBE stopped; secret types', sorted(set(k for k, _ in store.secrets)))


if __name__ == '__main__':
    run()
