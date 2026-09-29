# Motion wake validation — 2026-09-27

## Local checks

- Windows PlatformIO build: passed, Arduino 2.0.17 + ESP-IDF 4.4.7.
- Host C++ tests: passed (tilt, immediate directional click, flat single/double/
  triple, mixed ordering, invalid acceleration, disconnect reset, shake,
  display deadlines, pending-work idle guard, millis wraparound).
- HID descriptor: unchanged, 160 matching bytes.
- Generated SDK: PM, tickless idle, BLE modem sleep, main XTAL retention enabled.
- Only the IDF controller is built; NimBLE-Arduino remains the host.
- Partition binary: identical to baseline; NVS remains at 0x9000, size 0x5000.

## First hardware run

- Firmware flashed with hash verification and booted.
- BMI270 setup reported `motion_irq=1`, accelerometer 50 Hz, gyro disabled.
- Existing bond survived: `BLE boot bonds 1`; Windows encrypted reconnect
  reached `STATUS Connected encrypted=1 bonded=1 key_size=16`.
- A GPIO level-interrupt storm caused interrupt watchdog resets. Decoded
  backtrace identified `gpio_isr_loop`. The fix masks each interrupt in the ISR
  and rearms it only after the task has cleared the source and the pin is high.
  Corrected build passed and was flashed with hash verification.
- USB monitor now sets DTR/RTS before opening the port to avoid a reset on open.

## Corrected firmware: USB observation

- Saved-bond reconnect reached encrypted/bonded, key size 16, both HID subscriptions.
- Connection event reported interval 9 (11.25 ms), peripheral latency 0,
  supervision timeout 200 (2 s).
- Volume-up and volume-down queued at 25 and 33 ms after raw release.
- A click before reconnect completed was dropped, as in the previous firmware.
- Motion activity, shake screen wake, return to application idle and screen-off
  were observed. No watchdog reset appeared in this observation window.
- Battery-powered first-click and actual automatic sleep validation is pending.
- User reported that the requested pickup/volume/shake checks all work. The
  concern was that wake feels too fast, not a screen staying on or repeated
  unattended wake. Fast response alone neither proves nor disproves light sleep.
- Captured USB PM statistics show the expected active USB sleep lock and zero
  sleep time in those windows. Battery sleep and current savings remain unverified.

## Battery display correction — 2026-09-28

- User reported whole-screen/backlight flicker only after unplugging USB.
- The previous monitor expired on September 27 at 21:54, before the later
  unplug/replug. Fresh capture recorded USB disconnect/reconnect and subsequent
  `POWER USB=1`, showing that the USB sleep lock had been released on battery.
- StickS3 uses ESP GPIO38 PWM for the backlight. The application previously
  allowed automatic light sleep while that PWM was needed by the lit screen.
- Added display NO_LIGHT_SLEEP and APB_FREQ_MAX locks: acquire before panel wake,
  restore brightness after redraw, release only after backlight/panel shutdown.
- USB source fallback checks now run every 1 s on USB / 5 s on battery; PM1 IRQ
  remains the immediate path. This avoids a missed IRQ delaying the lock change
  until the old 60 s check.
- Corrected firmware builds successfully and was flashed on September 28 with
  all written image hashes verified. Fresh USB capture after manual reset confirms
  the application is running: `irq_ok=1`; both new display locks were held for
  about 5.02 s and are inactive after screen timeout. USB still blocks sleep as
  intended. Startup/reconnect messages were missed because the earlier monitor
  expired before the reset. User reports the display is better after this fix.
- A fresh pre-fix capture after reattachment reported `SLEEP 21201827 us`
  (21.2 s accumulated over 481.6 s uptime), `motion_wakes=2`, `irq_ok=1`.
  This confirms actual automatic sleep entry before the display correction;
  it does not establish battery-only duty cycle or current savings.

## Button-only screen wake — 2026-09-28

- Removed shake detection and its screen-wake path at the user's request.
- BMI270 motion wake and active orientation sampling remain enabled for button
  response. Button presses still light the screen for 5 s; boot/stop screens remain.
- PlatformIO build and existing host input/display tests passed. Flashed on
  September 28 with all written image hashes verified. Device validation pending.

## Hardware acceptance — pending

### Reset/Power regression — 2026-09-29

- User reports double-click shutdown no longer works in recent firmware.
- Source review found two consumers of PM1 button status: Motion's blanket
  `clearIRQStatus()` and M5Unified's default `pmic_button` polling/getPekPress.
  PM1 datasheet section 6, IRQ Status 3 (0x42), says bits 0/2 also participate in
  reset/shutdown decisions. Clearing these during motion IRQ handling can
  interfere with the hardware action; the failure mechanism is not yet captured.
- Disabled M5Unified PMIC button polling; motion handling now acknowledges only
  GPIO/system IRQ status (0x40/0x41), including fallback and stop paths.
- Removed IMU GPIO4 power-on wake (0x18 bit4), which survives reset/download/off.
  GPIO level-change IRQs remain unmasked for ESP light-sleep wake. The old code
  unnecessarily armed a PM1 power-on wake source as well as ESP sleep wake.
- Startup logs GPIO wake and button configuration registers, including whether
  hardware double-click shutdown is enabled. Button timing/config is not rewritten.
- Manufacturer reference: [PM1 datasheet, sections 2, 6, 7](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_CN.pdf).
- PlatformIO build passed. Flashed September 29 with all written image hashes
  verified. Boot capture confirms `PM1 gpio_wake=0x00 button_cfg=0x2a/0x00
  double_off=1`, `motion_irq=1`, and `BLE boot bonds 1`. Display timeout and
  application idle observed. Encrypted bonded Windows reconnect reached both
  HID subscriptions. User confirmed the requested unplug/double-click/off-state
  check works. Separate post-fix battery IMU light-sleep wake measurement pending.
- Side button B already shares the central button's 5 s display wake path in
  the flashed build (GPIO12, M5.BtnB). Confirmed by source inspection and made
  explicit in README; no additional firmware change required for a short click.

### Remaining device checks

- Boot, sensor initialization, encrypted bonded Windows reconnect.
- 30 pickup → immediate volume clicks, including after a stationary interval.
- Direction mapping, flat triple Num Lock, motion with screen off, button display (5 s).
- 30 minutes stationary while connected; no wake loop or unexpected screen wake.
- Couch/movie vibration test and threshold calibration if needed.
- Five power cycles, Windows sleep/resume, out-of-range recovery.
- Battery operation, USB attach/detach, actual sleep entry and first-click response.
- B stop: screen off after 3 s, Reset resumes.

## Energy and latency

No battery-current measurement has been made. 10–15 mA is a target, not a result.
Compare battery-powered, screen-off idle using the same adapter and connection
parameters. Also compare five volume clicks every five minutes, with ordinary
screen timing. Record average mA, wake reliability and negotiated BLE parameters.
Estimate ideal hours as 250 / average_mA; actual usable battery capacity is lower
and must be established by a controlled runtime test.

`release_to_queue_ms` measures firmware processing only. Windows response under
100 ms remains a hardware target; USB logs alone cannot establish it.
