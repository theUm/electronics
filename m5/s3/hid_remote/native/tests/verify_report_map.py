"""Ensure the native descriptor stays byte-identical to the tested Python one."""

import ast
import pathlib
import re

root = pathlib.Path(__file__).resolve().parents[2]
python = (root / "media_hid.py").read_text(encoding="utf-8")
native = (root / "native/src/ble_remote.cpp").read_text(encoding="utf-8")

python_bytes = ast.literal_eval(
    "(" + python.split("REPORT_MAP = bytes((", 1)[1].split("))\nREPORTS", 1)[0] + ")"
)
native_section = native.split("uint8_t report_map[] = {", 1)[1].split("};", 1)[0]
native_bytes = tuple(int(value, 16) for value in re.findall(r"0x[0-9A-Fa-f]+", native_section))
assert native_bytes == python_bytes, "Native HID report map differs from the Python remote"
print(f"HID report map: {len(native_bytes)} matching bytes")
