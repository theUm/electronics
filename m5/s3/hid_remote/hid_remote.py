# SPDX-License-Identifier: GPL-3.0-or-later
# UiFlow entry point: /flash/apps/hid_remote.py or auto-start /flash/main.py
import sys

APP_DIR = '/flash/libs/hid_remote'
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

# Also allow a freshly uploaded version to be run without a firmware reboot.
for name in ('remote_app', 'remote_input', 'media_hid', 'hid_services', 'hid_keystores'):
    sys.modules.pop(name, None)

from remote_app import run

run()
