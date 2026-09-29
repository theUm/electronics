"""Check the SDK actually enables sleep and the existing NVS layout survives."""
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1]
config = (root / '.pio/build/sticks3/config/sdkconfig.h').read_text()
for option in (
    'PM_ENABLE', 'FREERTOS_USE_TICKLESS_IDLE', 'BT_CONTROLLER_ONLY',
    'BT_CTRL_MODEM_SLEEP', 'BT_CTRL_LPCLK_SEL_MAIN_XTAL',
    'BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP', 'SPIRAM_MODE_OCT',
):
    assert f'#define CONFIG_{option} 1' in config, option
assert '#define CONFIG_BT_BLUEDROID_ENABLED 1' not in config
assert '#define CONFIG_BT_NIMBLE_ENABLED 1' not in config
data = (root / '.pio/build/sticks3/partitions.bin').read_bytes()
partitions = {}
for offset in range(0, len(data), 32):
    entry = data[offset:offset + 32]
    if len(entry) < 32 or entry[:2] != b'\xaa\x50':
        break
    _, kind, subtype, address, size, name, _ = struct.unpack('<HBBII16sI', entry)
    partitions[name.split(b'\0', 1)[0].decode()] = (kind, subtype, address, size)
assert partitions['nvs'] == (1, 2, 0x9000, 0x5000)
assert partitions['otadata'] == (1, 0, 0xE000, 0x2000)
assert partitions['app0'] == (0, 0x10, 0x10000, 0x330000)
assert partitions['app1'] == (0, 0x11, 0x340000, 0x330000)
print('SDK sleep flags and existing NVS/app partition layout verified')
