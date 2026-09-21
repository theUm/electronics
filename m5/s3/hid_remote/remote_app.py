# SPDX-License-Identifier: GPL-3.0-or-later
import time
import M5
try:
    import machine
except ImportError:
    machine = None
try:
    import network
except ImportError:
    network = None

from media_hid import MediaRemote
from remote_input import (
    Clicks, DisplayIdle, ImuSchedule, Tilt, TILT_INVALID,
    FLAT, MODE_VOLUME_UP, MODE_VOLUME_DOWN, MODE_ARROW_LEFT, MODE_ARROW_RIGHT,
    VOLUME_UP, VOLUME_DOWN, PLAY_PAUSE, ARROW_LEFT, ARROW_RIGHT,
)

DISPLAY_BRIGHTNESS = 60
BATTERY_POLL_MS = 60000
CPU_FREQ_HZ = 80000000

MODE_ORIENTATION = {
    MODE_VOLUME_UP: 'USB UP',
    MODE_VOLUME_DOWN: 'USB DOWN',
    MODE_ARROW_LEFT: 'ROLL LEFT',
    MODE_ARROW_RIGHT: 'ROLL RIGHT',
    FLAT: 'FLAT',
}
MODE_COMMAND = {
    MODE_VOLUME_UP: 'VOLUME +',
    MODE_VOLUME_DOWN: 'VOLUME -',
    MODE_ARROW_LEFT: 'LEFT',
    MODE_ARROW_RIGHT: 'RIGHT',
    FLAT: 'PLAY/PAUSE',
}
ACTION_LABELS = {
    VOLUME_UP: 'VOLUME +',
    VOLUME_DOWN: 'VOLUME -',
    PLAY_PAUSE: 'PLAY/PAUSE',
    ARROW_LEFT: 'LEFT',
    ARROW_RIGHT: 'RIGHT',
}


def disable_wifi():
    if network is None or not hasattr(network, 'WLAN'):
        print('POWER WiFi API unavailable')
        return
    modes = []
    for name in ('STA_IF', 'AP_IF'):
        if hasattr(network, name):
            modes.append(getattr(network, name))
    for name in ('IF_STA', 'IF_AP'):
        if hasattr(network.WLAN, name):
            modes.append(getattr(network.WLAN, name))
    disabled = 0
    seen = []
    for mode in modes:
        if mode in seen:
            continue
        seen.append(mode)
        try:
            wlan = network.WLAN(mode)
            if hasattr(wlan, 'disconnect'):
                try:
                    wlan.disconnect()
                except Exception:
                    pass
            wlan.active(False)
            disabled += 1
        except Exception as exc:
            print('POWER WiFi disable failed', mode, repr(exc))
    print('POWER WiFi off', disabled)


def disable_unused_hardware():
    disabled = []
    for name in ('Speaker', 'Mic'):
        component = getattr(M5, name, None)
        if component is not None and hasattr(component, 'end'):
            try:
                component.end()
                disabled.append(name)
            except Exception as exc:
                print('POWER', name, 'disable failed', repr(exc))
    power = getattr(M5, 'Power', None)
    if power is not None and hasattr(power, 'setLed'):
        try:
            power.setLed(0)
            disabled.append('LED')
        except Exception as exc:
            print('POWER LED disable failed', repr(exc))
    print('POWER disabled', ','.join(disabled) if disabled else 'none')


def lower_cpu_frequency():
    if machine is None or not hasattr(machine, 'freq'):
        print('POWER CPU frequency API unavailable')
        return None
    try:
        original = machine.freq()
        if original > CPU_FREQ_HZ:
            machine.freq(CPU_FREQ_HZ)
        print('POWER CPU Hz', machine.freq(), 'was', original)
        return original
    except Exception as exc:
        print('POWER CPU frequency unchanged', repr(exc))
        return None


def read_battery():
    power = M5.Power
    level = max(0, min(100, power.getBatteryLevel()))
    voltage = None
    charging = None
    for name, target in (('getBatteryVoltage', 'voltage'), ('isCharging', 'charging')):
        method = getattr(power, name, None)
        if method is None:
            continue
        try:
            value = method()
            if target == 'voltage':
                voltage = int(value) if value > 0 else None
            else:
                charging = bool(value)
        except Exception as exc:
            print('BATTERY', name, 'unavailable', repr(exc))
    return level, voltage, charging


def sleep_display():
    M5.Lcd.setBrightness(0)
    if hasattr(M5.Lcd, 'powerSaveOn'):
        M5.Lcd.powerSaveOn()
    print('DISPLAY sleep')


def wake_display():
    if hasattr(M5.Lcd, 'powerSaveOff'):
        M5.Lcd.powerSaveOff()
    M5.Lcd.setBrightness(DISPLAY_BRIGHTNESS)
    print('DISPLAY wake')


def run():
    M5.begin()
    disable_wifi()
    disable_unused_hardware()
    original_freq = lower_cpu_frequency()
    M5.BtnA.setDebounceThresh(0)  # Debounce lives in the tested state machine.
    M5.Lcd.setRotation(0)
    M5.Lcd.setBrightness(DISPLAY_BRIGHTNESS)
    M5.Lcd.setFont(M5.Lcd.FONTS.DejaVu12)
    M5.Lcd.fillScreen(0x101820)
    remote = MediaRemote()
    tilt = Tilt()
    clicks = Clicks()
    now = time.ticks_ms()
    imu_schedule = ImuSchedule(now)
    display_idle = DisplayIdle(now)
    last_action = '--'
    last_draw = None
    last_drawn = None
    battery, battery_mv, charging = read_battery()
    print('BATTERY level', battery, 'mV', battery_mv, 'charging', charging)
    last_battery_poll = now
    epoch = remote.epoch
    old_status = None
    side_down = None
    try:
        remote.set_battery_level(battery)
        remote.start()
        while True:
            now = time.ticks_ms()
            M5.update()
            button_a = M5.BtnA.isPressed()
            button_b = M5.BtnB.isPressed()
            if button_a or button_b:
                if display_idle.activity(now):
                    wake_display()
                    last_draw = None
                    last_drawn = None
            elif display_idle.poll(now):
                sleep_display()

            remote.poll(now)
            if remote.epoch != epoch:
                epoch = remote.epoch
                clicks.reset(wait_release=True)

            sample_imu, fresh_press, sample_ms = imu_schedule.update(now, button_a)
            if sample_imu:
                if fresh_press:
                    tilt.reset_filter()
                accel = M5.Imu.getAccel() if M5.Imu.isEnabled() else (0, 0, 0)
                direction = tilt.update(accel, sample_ms)
            else:
                direction = tilt.direction
            gesture_direction = direction if tilt.valid else TILT_INVALID
            action = clicks.update(now, button_a, gesture_direction)
            if action is not None:
                sent = remote.send_action(action)
                last_action = ACTION_LABELS[action] if sent else 'NOT SENT'
                print('INPUT', ACTION_LABELS[action], 'queued' if sent else 'dropped',
                      'accel', accel, 'pitch', tilt.pitch_deg, 'roll', tilt.roll_deg)

            if button_b:
                if side_down is None:
                    side_down = now
                elif time.ticks_diff(now, side_down) >= 1500:
                    break
            else:
                side_down = None

            status = remote.status()
            connection_state = (status, remote.encrypted, remote.bonded,
                                remote.key_size, remote.suspended)
            if connection_state != old_status:
                print('STATUS', status, 'encrypted', remote.encrypted,
                      'bonded', remote.bonded, 'key_size', remote.key_size,
                      'suspended', remote.suspended)
                old_status = connection_state

            if time.ticks_diff(now, last_battery_poll) >= BATTERY_POLL_MS:
                new_battery, battery_mv, charging = read_battery()
                last_battery_poll = now
                print('BATTERY level', new_battery, 'mV', battery_mv,
                      'charging', charging)
                if new_battery != battery:
                    battery = new_battery
                    remote.set_battery_level(battery)
                    remote._ble.gatts_write(remote.h_bat, bytes((battery,)))

            if (display_idle.screen_on and
                    (last_draw is None or time.ticks_diff(now, last_draw) >= 200)):
                last_draw = now
                state = (status, direction, last_action, battery, battery_mv)
                if state != last_drawn:
                    draw(status, direction, last_action, battery, battery_mv, last_drawn)
                    last_drawn = state
            time.sleep_ms(20)
    finally:
        try:
            remote.stop()
        finally:
            if original_freq is not None and machine is not None:
                machine.freq(original_freq)
        wake_display()
        M5.Lcd.fillScreen(0x101820)
        M5.Lcd.setTextColor(0xFFFFFF, 0x101820)
        M5.Lcd.drawString('Remote stopped', 5, 40)
        M5.Lcd.drawString('Reset for UiFlow', 5, 65)


def _fields(status, direction, action, battery, battery_mv):
    return (
        (status.upper(), 32, 0x50DCC8 if status == 'Connected' else 0xFFCA66),
        (MODE_ORIENTATION[direction], 69, 0xA5B7C9),
        (MODE_COMMAND[direction], 91, 0xFFFFFF),
        ('Last: ' + action, 124, 0xFFFFFF),
        (('%d%% %dmV' % (battery, battery_mv)) if battery_mv is not None
         else ('Battery: %d%%' % battery), 220, 0xA5B7C9),
    )


def draw(status, direction, action, battery, battery_mv, previous=None):
    bg = 0x101820
    M5.Lcd.startWrite()
    try:
        if previous is None:
            M5.Lcd.fillRect(0, 0, 135, 240, bg)
            M5.Lcd.setTextColor(0xFFFFFF, bg)
            M5.Lcd.drawString('M5 REMOTE', 5, 8)
            M5.Lcd.setTextColor(0xA5B7C9, bg)
            M5.Lcd.drawString('Tilt: VOL +/-', 5, 157)
            M5.Lcd.drawString('Roll: LEFT/RIGHT', 5, 178)
            M5.Lcd.drawString('Flat: PLAY/PAUSE', 5, 199)
        fields = _fields(status, direction, action, battery, battery_mv)
        old_fields = _fields(*previous) if previous else ((None, 0, 0),) * len(fields)
        for index, (text, y, color) in enumerate(fields):
            if previous is None or text != old_fields[index][0]:
                M5.Lcd.fillRect(0, y, 135, 20, bg)
                M5.Lcd.setTextColor(color, bg)
                M5.Lcd.drawString(text, 5, y)
    finally:
        M5.Lcd.endWrite()
