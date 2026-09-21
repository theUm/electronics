# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure, clock-injected gesture and idle logic for firmware and host tests."""
try:
    from time import ticks_diff
except ImportError:
    def ticks_diff(a, b):
        return a - b
try:
    from math import atan2, degrees, sqrt
except ImportError:
    atan2 = degrees = sqrt = None

# HID usages returned by Clicks.
VOLUME_UP = 0xE9
VOLUME_DOWN = 0xEA
PLAY_PAUSE = 0xCD
ARROW_LEFT = 0x50
ARROW_RIGHT = 0x4F

# Physical orientation modes returned by Tilt.
FLAT = 0
MODE_VOLUME_UP = 1
MODE_VOLUME_DOWN = -1
MODE_ARROW_LEFT = 3
MODE_ARROW_RIGHT = 4
TILT_INVALID = 99

DEBOUNCE_MS = 25
SHORT_MS = 500
DISPLAY_IDLE_MS = 5000
ANGLE_ENTER_DEG = 30
ANGLE_NEUTRAL_DEG = 20
AXIS_SWITCH_MARGIN_DEG = 8
FILTER_ALPHA = 0.25  # Approximately the old response time at a 20 ms loop.

# Empirical UiFlow axis mapping. The physical sign is verified on the target
# StickS3 before release; +1 means positive ax is a physical left roll.
ROLL_LEFT_SIGN = 1


class Tilt:
    """Classify flat, longitudinal tilt, and left/right roll from gravity."""
    def __init__(self):
        self.pitch_deg = None
        self.roll_deg = None
        # Compatibility with earlier diagnostics that printed angle_deg.
        self.angle_deg = None
        self.direction = FLAT
        self.valid = False

    def reset_filter(self):
        self.pitch_deg = None
        self.roll_deg = None
        self.angle_deg = None
        self.direction = FLAT
        self.valid = False

    @staticmethod
    def _filtered(previous, current, alpha):
        return current if previous is None else previous + alpha * (current - previous)

    @staticmethod
    def _axis(mode):
        if mode in (MODE_VOLUME_UP, MODE_VOLUME_DOWN):
            return 'pitch'
        if mode in (MODE_ARROW_LEFT, MODE_ARROW_RIGHT):
            return 'roll'
        return None

    @staticmethod
    def _pitch_mode(angle):
        return MODE_VOLUME_UP if angle >= 0 else MODE_VOLUME_DOWN

    @staticmethod
    def _roll_mode(angle):
        physical = angle * ROLL_LEFT_SIGN
        return MODE_ARROW_LEFT if physical >= 0 else MODE_ARROW_RIGHT

    def _enter_from_flat(self, pitch_abs, roll_abs):
        if max(pitch_abs, roll_abs) < ANGLE_ENTER_DEG:
            return FLAT
        if pitch_abs >= roll_abs:
            return self._pitch_mode(self.pitch_deg)
        return self._roll_mode(self.roll_deg)

    def update(self, accel, elapsed_ms=20):
        # BMI270 values from this UiFlow build are in g (verified on hardware).
        magnitude2 = sum(v * v for v in accel)
        if not 0.36 <= magnitude2 <= 2.56:
            self.pitch_deg = None
            self.roll_deg = None
            self.angle_deg = None
            self.direction = FLAT
            self.valid = False
            return FLAT
        self.valid = True

        ax, ay, az = accel
        pitch = degrees(atan2(ay, sqrt(ax * ax + az * az)))
        roll = degrees(atan2(ax, sqrt(ay * ay + az * az)))
        # Keep the old 20 ms response when samples are spaced further apart.
        alpha = 1 - (1 - FILTER_ALPHA) ** (max(1, elapsed_ms) / 20)
        self.pitch_deg = self._filtered(self.pitch_deg, pitch, alpha)
        self.roll_deg = self._filtered(self.roll_deg, roll, alpha)
        self.angle_deg = self.pitch_deg
        pitch_abs = abs(self.pitch_deg)
        roll_abs = abs(self.roll_deg)

        current_axis = self._axis(self.direction)
        if current_axis is None:
            self.direction = self._enter_from_flat(pitch_abs, roll_abs)
        elif current_axis == 'pitch':
            if pitch_abs < ANGLE_NEUTRAL_DEG:
                self.direction = (self._roll_mode(self.roll_deg)
                                  if roll_abs >= ANGLE_ENTER_DEG else FLAT)
            elif (roll_abs >= ANGLE_ENTER_DEG and
                  roll_abs >= pitch_abs + AXIS_SWITCH_MARGIN_DEG):
                self.direction = self._roll_mode(self.roll_deg)
            else:
                self.direction = self._pitch_mode(self.pitch_deg)
        else:
            if roll_abs < ANGLE_NEUTRAL_DEG:
                self.direction = (self._pitch_mode(self.pitch_deg)
                                  if pitch_abs >= ANGLE_ENTER_DEG else FLAT)
            elif (pitch_abs >= ANGLE_ENTER_DEG and
                  pitch_abs >= roll_abs + AXIS_SWITCH_MARGIN_DEG):
                self.direction = self._pitch_mode(self.pitch_deg)
            else:
                self.direction = self._roll_mode(self.roll_deg)
        return self.direction


class ImuSchedule:
    """Sample sparsely at rest, but immediately and continuously during a press."""
    def __init__(self, now=0, idle_ms=100):
        self.idle_ms = idle_ms
        self.last_sample = now - idle_ms
        self.was_pressed = False

    def update(self, now, pressed):
        rising = pressed and not self.was_pressed
        self.was_pressed = pressed
        elapsed = ticks_diff(now, self.last_sample)
        if rising or pressed or elapsed >= self.idle_ms:
            self.last_sample = now
            return True, rising, max(1, elapsed)
        return False, False, 0


class Clicks:
    def __init__(self):
        self.reset()

    def reset(self, wait_release=False):
        self.raw = False
        self.stable = False
        self.raw_since = 0
        self.down_since = 0
        self.press_direction = FLAT
        self.wait_release = wait_release

    def update(self, now, pressed, direction):
        if self.wait_release:
            if not pressed:
                self.wait_release = False
            return None
        if pressed != self.raw:
            self.raw = pressed
            self.raw_since = now
        if self.raw != self.stable and ticks_diff(now, self.raw_since) >= DEBOUNCE_MS:
            self.stable = self.raw
            if self.stable:
                self.down_since = now
                # Freeze the mode at press time so movement during release
                # cannot change the selected command.
                self.press_direction = direction
            elif ticks_diff(now, self.down_since) <= SHORT_MS:
                return {
                    FLAT: PLAY_PAUSE,
                    MODE_VOLUME_UP: VOLUME_UP,
                    MODE_VOLUME_DOWN: VOLUME_DOWN,
                    MODE_ARROW_LEFT: ARROW_LEFT,
                    MODE_ARROW_RIGHT: ARROW_RIGHT,
                }.get(self.press_direction)
            else:
                self.press_direction = FLAT
        return None


class DisplayIdle:
    """Track display sleep without coupling the policy to M5 hardware APIs."""
    def __init__(self, now=0, timeout_ms=DISPLAY_IDLE_MS):
        self.timeout_ms = timeout_ms
        self.last_activity = now
        self.screen_on = True

    def activity(self, now):
        woke = not self.screen_on
        self.last_activity = now
        self.screen_on = True
        return woke

    def poll(self, now):
        if self.screen_on and ticks_diff(now, self.last_activity) >= self.timeout_ms:
            self.screen_on = False
            return True
        return False
