import math
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

import remote_input as ri
import device


def gravity(x_angle=0, y_angle=0):
    """Build a unit vector whose roll/pitch asin angles match the inputs."""
    x = math.sin(math.radians(x_angle))
    y = math.sin(math.radians(y_angle))
    z = math.sqrt(max(0.0, 1.0 - x * x - y * y))
    return x, y, z


class GestureTests(unittest.TestCase):
    def trace(self, events, until=1200):
        clicks = ri.Clicks()
        pressed, direction = False, ri.MODE_VOLUME_UP
        result = []
        for now in range(0, until, 5):
            if now in events:
                pressed, direction = events[now]
            action = clicks.update(now, pressed, direction)
            if action is not None:
                result.append((now, action))
        return result

    def test_all_five_click_modes(self):
        cases = (
            (ri.MODE_VOLUME_UP, ri.VOLUME_UP),
            (ri.MODE_VOLUME_DOWN, ri.VOLUME_DOWN),
            (ri.FLAT, ri.PLAY_PAUSE),
            (ri.MODE_ARROW_LEFT, ri.ARROW_LEFT),
            (ri.MODE_ARROW_RIGHT, ri.ARROW_RIGHT),
        )
        for mode, expected in cases:
            with self.subTest(mode=mode):
                self.assertEqual(
                    self.trace({0: (True, mode), 100: (False, mode)}),
                    [(125, expected)],
                )

    def test_direction_is_frozen_at_press(self):
        self.assertEqual(
            self.trace({0: (True, ri.MODE_ARROW_LEFT), 100: (False, ri.MODE_VOLUME_UP)}),
            [(125, ri.ARROW_LEFT)],
        )

    def test_invalid_tilt_does_not_generate_action(self):
        self.assertEqual(
            self.trace({0: (True, ri.TILT_INVALID), 100: (False, ri.TILT_INVALID)}),
            [],
        )

    def test_bounce(self):
        self.assertEqual(
            self.trace({0: (True, ri.MODE_VOLUME_UP), 10: (False, ri.MODE_VOLUME_UP),
                        15: (True, ri.MODE_VOLUME_UP), 100: (False, ri.MODE_VOLUME_UP)}),
            [(125, ri.VOLUME_UP)],
        )

    def test_long_hold(self):
        self.assertEqual(
            self.trace({0: (True, ri.MODE_VOLUME_UP), 800: (False, ri.MODE_VOLUME_UP)}),
            [],
        )

    def test_disconnect_discards_pending(self):
        clicks = ri.Clicks()
        for now, pressed in ((0, True), (30, True), (100, False), (130, False)):
            clicks.update(now, pressed, ri.MODE_VOLUME_UP)
        clicks.reset(wait_release=True)
        self.assertIsNone(clicks.update(150, False, ri.MODE_VOLUME_UP))
        self.assertIsNone(clicks.update(600, False, ri.MODE_VOLUME_UP))

    def test_five_clean_orientations(self):
        with patch.object(ri, 'FILTER_ALPHA', 1.0):
            self.assertEqual(ri.Tilt().update((0, 1, 0)), ri.MODE_VOLUME_UP)
            self.assertEqual(ri.Tilt().update((0, -1, 0)), ri.MODE_VOLUME_DOWN)
            self.assertEqual(ri.Tilt().update((1, 0, 0)), ri.MODE_ARROW_LEFT)
            self.assertEqual(ri.Tilt().update((-1, 0, 0)), ri.MODE_ARROW_RIGHT)
            self.assertEqual(ri.Tilt().update((0, 0, 1)), ri.FLAT)

    def test_diagonal_uses_stronger_axis(self):
        with patch.object(ri, 'FILTER_ALPHA', 1.0):
            self.assertEqual(ri.Tilt().update(gravity(35, 50)), ri.MODE_VOLUME_UP)
            self.assertEqual(ri.Tilt().update(gravity(50, 35)), ri.MODE_ARROW_LEFT)
            self.assertEqual(ri.Tilt().update(gravity(-50, -35)), ri.MODE_ARROW_RIGHT)

    def test_hysteresis_retains_mode_between_twenty_and_thirty_degrees(self):
        tilt = ri.Tilt()
        with patch.object(ri, 'FILTER_ALPHA', 1.0):
            self.assertEqual(tilt.update(gravity(0, 40)), ri.MODE_VOLUME_UP)
            self.assertEqual(tilt.update(gravity(0, 25)), ri.MODE_VOLUME_UP)
            self.assertEqual(tilt.update(gravity(0, 19)), ri.FLAT)

    def test_axis_switch_requires_eight_degree_margin(self):
        tilt = ri.Tilt()
        with patch.object(ri, 'FILTER_ALPHA', 1.0):
            self.assertEqual(tilt.update(gravity(0, 40)), ri.MODE_VOLUME_UP)
            self.assertEqual(tilt.update(gravity(45, 40)), ri.MODE_VOLUME_UP)
            self.assertEqual(tilt.update(gravity(49, 35)), ri.MODE_ARROW_LEFT)

    def test_invalid_magnitude_resets_both_angles(self):
        tilt = ri.Tilt()
        tilt.update((0, 1, 0))
        self.assertEqual(tilt.update((0, 0, 0)), ri.FLAT)
        self.assertFalse(tilt.valid)
        self.assertIsNone(tilt.pitch_deg)
        self.assertIsNone(tilt.roll_deg)

    def test_idle_imu_samples_sparsely_but_press_samples_immediately(self):
        schedule = ri.ImuSchedule(0)
        self.assertEqual(schedule.update(0, False)[:2], (True, False))
        self.assertEqual(schedule.update(20, False)[:2], (False, False))
        self.assertEqual(schedule.update(99, False)[:2], (False, False))
        self.assertEqual(schedule.update(100, False)[:2], (True, False))
        self.assertEqual(schedule.update(121, True)[:2], (True, True))
        self.assertEqual(schedule.update(141, True)[:2], (True, False))

    def test_press_resets_old_orientation_before_classification(self):
        tilt = ri.Tilt()
        with patch.object(ri, 'FILTER_ALPHA', 1.0):
            self.assertEqual(tilt.update((0, 1, 0)), ri.MODE_VOLUME_UP)
            tilt.reset_filter()
            self.assertEqual(tilt.update((1, 0, 0), 1), ri.MODE_ARROW_LEFT)

    def test_idle_imu_filter_keeps_twenty_ms_time_constant(self):
        tilt = ri.Tilt()
        tilt.update((0, 0, 1))
        tilt.update((0, 1, 0), 100)
        self.assertGreater(tilt.pitch_deg, 60)


class DisplayIdleTests(unittest.TestCase):
    def test_sleeps_at_five_seconds(self):
        idle = ri.DisplayIdle(100)
        self.assertFalse(idle.poll(5099))
        self.assertTrue(idle.poll(5100))
        self.assertFalse(idle.screen_on)

    def test_first_click_wakes_and_still_executes(self):
        idle = ri.DisplayIdle(0)
        self.assertTrue(idle.poll(5000))
        self.assertTrue(idle.activity(5010))
        clicks = ri.Clicks()
        result = None
        for now, pressed in ((5010, True), (5040, True), (5100, False), (5130, False)):
            result = clicks.update(now, pressed, ri.MODE_ARROW_LEFT) or result
        self.assertEqual(result, ri.ARROW_LEFT)
        self.assertTrue(idle.screen_on)

    def test_non_button_events_do_not_wake(self):
        idle = ri.DisplayIdle(0)
        idle.poll(5000)
        # IMU/BLE processing does not call activity().
        self.assertFalse(idle.poll(9000))
        self.assertFalse(idle.screen_on)


class TransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.modules = patch.dict(sys.modules, {
            'micropython': types.SimpleNamespace(const=lambda value: value),
            'bluetooth': types.SimpleNamespace(
                UUID=lambda value: value, BLE=lambda: None,
                FLAG_READ=2, FLAG_WRITE=8, FLAG_NOTIFY=16, FLAG_WRITE_NO_RESPONSE=4),
            'esp32': types.SimpleNamespace(),
        })
        cls.modules.start()
        sys.path.insert(0, str(Path(__file__).parent / 'vendor'))
        import media_hid
        cls.module = media_hid

    @classmethod
    def tearDownClass(cls):
        cls.modules.stop()
        sys.path.pop(0)

    def remote(self):
        remote = object.__new__(self.module.MediaRemote)
        remote.device_state = remote.DEVICE_CONNECTED
        remote.conn_handle = 1
        remote.encrypted = True
        remote.bonded = True
        remote.authenticated = False
        remote.key_size = 16
        remote.suspended = False
        remote.protocol_mode = 1
        remote.diagnostics = []
        remote.secret_hits = 0
        remote.secret_misses = 0
        remote.pending_release = None
        remote.queue = []
        remote.link_since = None
        remote.security_started = False
        remote.pair_attempts = 0
        remote.pair_requested_at = None
        remote.pair_wait_ms = self.module.PAIR_FALLBACK_MS
        remote.advertising_since = None
        remote.advertising_slow = False
        remote.h_consumer_report = 20
        remote.h_keyboard_report = 22
        remote.h_rep = 20
        remote.h_ctrl = 18
        remote.h_protocol = 24
        remote.h_boot_keyboard_input = 26
        remote.h_boot_keyboard_output = 28
        remote.epoch = 0
        remote.characteristics = {}
        remote.state_change_callback = None
        remote.secrets = types.SimpleNamespace(
            flush=lambda: None, secrets={(1, b'host'): b'bond'},
            get_secret=lambda *args: None,
        )
        self.notifications = []
        self.writes = []
        self.pair_requests = []
        self.advertise_requests = []
        self.control_value = b'\x01'

        advertiser = types.SimpleNamespace(advertising=False, interval_us=None)

        def start_advertising(interval_us=100000):
            self.advertise_requests.append(interval_us)
            advertiser.advertising = True
            advertiser.interval_us = interval_us

        advertiser.start_advertising = start_advertising
        remote.adv = advertiser
        remote._ble = types.SimpleNamespace(
            gatts_write=lambda handle, data: self.writes.append((handle, data)),
            gatts_notify=lambda conn, handle, data: self.notifications.append((handle, data)),
            gatts_read=lambda handle: self.control_value,
            gap_pair=lambda conn: self.pair_requests.append(conn),
        )
        return remote

    def poll_times(self, remote, times):
        with patch.object(self.module.time, 'ticks_diff', lambda a, b: a - b, create=True):
            for now in times:
                remote.poll(now)

    def test_each_media_key_sends_press_then_matching_release(self):
        for usage, expected in (
            (ri.VOLUME_UP, b'\x40\x00'),
            (ri.VOLUME_DOWN, b'\x80\x00'),
            (ri.PLAY_PAUSE, b'\x00\x0d'),
        ):
            remote = self.remote()
            with self.subTest(usage=usage):
                self.assertTrue(remote.send_action(usage))
                self.poll_times(remote, (100, 139, 140))
                self.assertEqual(
                    self.notifications,
                    [(20, expected), (20, b'\x00\x00')],
                )

    def test_arrow_keys_use_keyboard_handle_and_eight_byte_release(self):
        for usage, keycode in ((ri.ARROW_LEFT, 0x50), (ri.ARROW_RIGHT, 0x4F)):
            remote = self.remote()
            with self.subTest(usage=usage):
                self.assertTrue(remote.send_action(usage))
                self.poll_times(remote, (100, 140))
                self.assertEqual(self.notifications[0][0], 22)
                self.assertEqual(self.notifications[0][1][2], keycode)
                self.assertEqual(self.notifications[1], (22, b'\x00' * 8))

    def test_keyboard_collection_and_report_id_are_declared(self):
        self.assertIn(
            bytes((0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02)),
            self.module.REPORT_MAP,
        )

    def test_keyboard_boot_characteristics_and_protocol_permissions(self):
        with patch.object(self.module.esp32, 'NVS', lambda namespace: object(), create=True):
            remote = self.module.MediaRemote()
        characteristics = remote.HIDS[1]
        self.assertEqual(characteristics[5][0], 0x2A4E)
        self.assertEqual(characteristics[5][1] & 0x0006, 0x0006)
        self.assertEqual(characteristics[6][0], 0x2A22)
        self.assertEqual(characteristics[7][0], 0x2A32)

    def test_boot_protocol_arrows_use_boot_report_and_media_is_unavailable(self):
        remote = self.remote()
        remote.protocol_mode = 0
        self.assertTrue(remote.arrow_left())
        self.assertFalse(remote.volume_up())
        self.poll_times(remote, (100, 140))
        self.assertEqual(self.notifications,
                         [(26, self.module.KEYBOARD_REPORTS[0x50]),
                          (26, self.module.KEYBOARD_RELEASE)])

    def test_protocol_write_switches_mode_and_discards_pending_reports(self):
        remote = self.remote()
        self.assertTrue(remote.volume_up())
        self.control_value = b'\x00'
        remote.ble_irq(3, (1, remote.h_protocol))
        self.assertEqual(remote.protocol_mode, 0)
        self.assertEqual(remote.queue, [])
        self.assertEqual(remote.diagnostics[-1], ('protocol', 0))
        self.assertTrue(remote.arrow_right())
        self.poll_times(remote, (100, 140))
        self.assertEqual(self.notifications[0][0], 26)

    def test_play_pause_usage_is_declared_in_report_map(self):
        self.assertIn(
            bytes((0x09, 0xB7, 0x09, 0xCD, 0x15, 0x01, 0x25, 0x0D)),
            self.module.REPORT_MAP,
        )

    def test_no_command_without_encryption_or_when_suspended(self):
        remote = self.remote()
        remote.encrypted = False
        self.assertFalse(remote.arrow_left())
        remote.encrypted = True
        remote.suspended = True
        self.assertFalse(remote.volume_up())
        remote.poll(100)
        self.assertEqual(self.notifications, [])

    def test_status_distinguishes_encryption_from_suspend(self):
        remote = self.remote()
        self.assertEqual(remote.status(), 'Connected')
        remote.secrets.secrets.clear()
        remote.secrets.secrets[(self.module.LOCAL_IRK_SECRET_TYPE, b'irk')] = b'0' * 16
        self.assertEqual(remote.status(), 'No bond')
        remote.secrets.secrets[(1, b'host')] = b'bond'
        remote.suspended = True
        self.assertEqual(remote.status(), 'Suspended')
        remote.suspended = False
        remote.bonded = False
        self.assertEqual(remote.status(), 'No bond')
        remote.encrypted = False
        self.assertEqual(remote.status(), 'Pairing...')
        remote.device_state = remote.DEVICE_ADVERTISING
        remote.conn_handle = None
        self.assertEqual(remote.status(), 'Pair via BT')

    def test_control_point_suspend_and_resume_are_recorded_without_keys(self):
        remote = self.remote()
        self.control_value = b'\x00'
        remote.ble_irq(3, (1, remote.h_ctrl))
        self.assertEqual(remote.status(), 'Suspended')
        self.control_value = b'\x01'
        remote.ble_irq(3, (1, remote.h_ctrl))
        self.assertEqual(remote.status(), 'Connected')
        self.assertEqual([event for event, _ in remote.diagnostics], ['suspend', 'resume'])

    def test_bond_lookup_records_only_hit_or_miss(self):
        remote = self.remote()
        remote.ble_irq(29, (1, 0, b'private-key'))
        self.assertEqual(remote.secret_misses, 1)
        self.assertEqual(remote.diagnostics[-1], ('bond lookup', 'miss'))

    def test_passkey_diagnostic_records_action_without_numeric_value(self):
        remote = self.remote()
        remote.ble_irq(31, (1, 99, 123456))
        self.assertEqual(remote.diagnostics[-1], ('passkey action', 99))

    def test_two_bonds_survive_store_reload(self):
        class FakeNVS:
            blob = None

            def get_blob(self, key, buffer):
                if self.blob is None:
                    raise OSError('missing')
                if buffer:
                    buffer[:] = self.blob
                return len(self.blob)

            def set_blob(self, key, data):
                self.blob = bytes(data)

            def commit(self):
                pass

        nvs = FakeNVS()
        with patch.object(self.module.esp32, 'NVS', lambda namespace: nvs, create=True):
            store = self.module.BondStore()
            store.add_secret(1, b'host-a', b'secret-a')
            store.add_secret(1, b'host-b', b'secret-b')
            store.save_secrets()
            store.flush()
            restored = self.module.BondStore()
            restored.load_secrets()
        self.assertEqual(restored.secrets, store.secrets)
        self.assertEqual(len(restored.secrets), 2)

    def test_disconnect_clears_both_reports_and_restarts_fast_advertising(self):
        remote = self.remote()
        remote.volume_up()
        with patch.object(self.module.time, 'ticks_ms', lambda: 500, create=True):
            remote.ble_irq(2, (1, 0, b'address'))
        self.assertEqual(remote.queue, [])
        self.assertIsNone(remote.pending_release)
        self.assertFalse(remote.ready())
        self.assertIn((20, b'\x00\x00'), self.writes)
        self.assertIn((22, b'\x00' * 8), self.writes)
        self.assertIn((26, b'\x00' * 8), self.writes)
        self.assertEqual(self.advertise_requests, [self.module.FAST_ADV_US])

    def test_pairing_is_delayed_and_requested_only_once(self):
        remote = self.remote()
        remote.encrypted = False
        remote.link_since = 100
        self.poll_times(remote, (
            100 + self.module.PAIR_FALLBACK_MS - 1,
            100 + self.module.PAIR_FALLBACK_MS,
            100 + self.module.PAIR_FALLBACK_MS + 1000,
        ))
        self.assertEqual(self.pair_requests, [1])

    def test_bonded_reconnect_can_encrypt_before_pair_fallback(self):
        remote = self.remote()
        remote.encrypted = False
        remote.link_since = 100
        self.poll_times(remote, (500,))
        remote.encrypted = True
        remote.bonded = True
        self.poll_times(remote, (100 + self.module.PAIR_FALLBACK_MS + 1000,))
        self.assertEqual(self.pair_requests, [])

    def test_bonded_link_waits_longer_before_pair_fallback(self):
        remote = self.remote()
        remote.encrypted = False
        remote.link_since = 100
        remote.secrets.secrets[('bond', b'host')] = b'key'
        remote.pair_wait_ms = self.module.BONDED_PAIR_FALLBACK_MS
        self.poll_times(remote, (100 + self.module.PAIR_FALLBACK_MS,
                                 100 + self.module.BONDED_PAIR_FALLBACK_MS - 1))
        self.assertEqual(self.pair_requests, [])
        self.poll_times(remote, (100 + self.module.BONDED_PAIR_FALLBACK_MS,))
        self.assertEqual(self.pair_requests, [1])

    def test_local_irk_alone_does_not_count_as_peer_bond(self):
        remote = self.remote()
        remote.secrets.secrets.clear()
        remote.secrets.secrets[(self.module.LOCAL_IRK_SECRET_TYPE, b'loc')] = b'0' * 16
        self.assertFalse(remote._has_peer_bond())
        remote.secrets.secrets[(11, b'peer')] = b'1' * 32
        self.assertTrue(remote._has_peer_bond())

    def test_failed_encryption_rearms_one_bounded_security_retry(self):
        remote = self.remote()
        remote.encrypted = False
        remote.link_since = 100
        self.poll_times(remote, (100 + self.module.PAIR_FALLBACK_MS,))
        self.assertEqual(self.pair_requests, [1])
        with patch.object(self.module.time, 'ticks_ms', lambda: 3000, create=True):
            remote.ble_irq(28, (1, False, False, False, 0))
        self.assertFalse(remote.security_started)
        self.poll_times(remote, (3000 + self.module.PAIR_RETRY_MS - 1,
                                 3000 + self.module.PAIR_RETRY_MS))
        self.assertEqual(self.pair_requests, [1, 1])
        with patch.object(self.module.time, 'ticks_ms', lambda: 9000, create=True):
            remote.ble_irq(28, (1, False, False, False, 0))
        self.poll_times(remote, (9000 + self.module.PAIR_RETRY_MS + 1000,))
        self.assertEqual(self.pair_requests, [1, 1])

    def test_successful_encryption_clears_pending_security_request(self):
        remote = self.remote()
        remote.encrypted = False
        remote.security_started = True
        remote.pair_requested_at = 100
        remote.ble_irq(28, (1, True, False, True, 16))
        self.assertFalse(remote.security_started)
        self.assertIsNone(remote.pair_requested_at)

    def test_advertising_slows_after_thirty_seconds(self):
        remote = self.remote()
        remote.device_state = remote.DEVICE_ADVERTISING
        remote.encrypted = False
        remote.advertising_since = 100
        self.poll_times(remote, (100 + self.module.SLOW_ADV_AFTER_MS - 1,))
        self.assertEqual(self.advertise_requests, [])
        self.poll_times(remote, (100 + self.module.SLOW_ADV_AFTER_MS,))
        self.assertEqual(self.advertise_requests, [self.module.SLOW_ADV_US])
        self.assertTrue(remote.advertising_slow)

    def test_repeated_press_requires_release_between(self):
        remote = self.remote()
        remote.arrow_left()
        remote.arrow_left()
        self.poll_times(remote, (0, 40, 50, 90))
        self.assertEqual(
            self.notifications,
            [(22, self.module.KEYBOARD_REPORTS[ri.ARROW_LEFT]),
             (22, self.module.KEYBOARD_RELEASE),
             (22, self.module.KEYBOARD_REPORTS[ri.ARROW_LEFT]),
             (22, self.module.KEYBOARD_RELEASE)],
        )


class UsbPortTests(unittest.TestCase):
    def port(self, name, vid=None, pid=None, description=''):
        return types.SimpleNamespace(device=name, vid=vid, pid=pid,
                                     description=description)

    def test_autodetects_sticks3_on_windows_or_macos(self):
        for name in ('COM5', '/dev/cu.usbmodem2101'):
            with self.subTest(name=name):
                ports = [self.port('COM2', 0x1234, 0x5678),
                         self.port(name, 0x303A, 0x832B)]
                self.assertEqual(device.resolve_port(ports=ports), name)

    def test_explicit_port_and_ambiguous_detection(self):
        ports = [self.port('COM5', 0x303A, 0x832B),
                 self.port('COM6', description='StickS3(UiFlow2)')]
        self.assertEqual(device.resolve_port('COM7', ports), 'COM7')
        with self.assertRaisesRegex(RuntimeError, 'Multiple StickS3 ports'):
            device.resolve_port(ports=ports)


if __name__ == '__main__':
    unittest.main()
