# MicroPythonBLEHID provenance

- Upstream: https://github.com/Heerkog/MicroPythonBLEHID
- Commit: `a173d63cbd40da3aac99fa87e8cbc53be229b837`
- License: GPL-3.0-or-later; the upstream license is included in `LICENSE`.
- `hid_keystores.py` is unmodified upstream. `readme.md` has one trailing space removed.

Local changes to `hid_services.py`:

- Separate `mitm` from `le_secure`; default to Just Works pairing while retaining LE Secure Connections and bonding.
- Encode advertising names to UTF-8 bytes.
- Maintain the advertiser's running flag and stop advertising using `gap_advertise(None)`.
- Allow the application to update the advertising interval while advertising.
- Remove all print statements containing bonding keys; silence remaining vendor console messages to avoid synchronous logging in BLE callbacks.

The application subclasses `HumanInterfaceDevice` in `media_hid.py`. It provides separate Consumer Control and keyboard input reports, correct little-endian PnP data, DIS/BAS/HIDS services, a stable BLE public address, and a deferred NVS bond store in the `hidremote` namespace. It does not use upstream's keyboard class, mouse, joystick, or generic DID service.

UiFlow 2.5.2 omits `FLAG_READ_ENCRYPTED` / `FLAG_WRITE_ENCRYPTED` names. The extension uses their standard MicroPython flag values and checks encrypted report reads in the GATT callback. MicroPython NimBLE handles CCCD subscriptions internally and does not expose them as `GATTS_WRITE`; readiness therefore checks the encrypted connection, not a nonexistent subscription event.
