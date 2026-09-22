#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>

#include <cstring>
#include "ble_remote.hpp"
#include "input.hpp"

namespace {

constexpr uint32_t kBackground = 0x101820;
constexpr uint32_t kMuted = 0xA5B7C9;
constexpr uint32_t kAccent = 0x50DCC8;
constexpr uint32_t kPending = 0xFFCA66;
constexpr uint8_t kBrightness = 60;

BleRemote ble;
remote::Tilt tilt;
remote::Clicks clicks;
remote::Shake shake;
remote::ImuSchedule* imu_schedule = nullptr;
remote::DisplayIdle* display_idle = nullptr;
remote::Direction direction = remote::Direction::flat;
uint32_t epoch = 0;
uint32_t last_draw = 0;
uint32_t last_battery_poll = 0;
uint32_t side_down = 0;
int battery_mv = -1;
uint8_t battery_level = 0;
bool stopped = false;
bool screen_dirty = true;
bool side_pressed = false;
String last_view;
String last_status;

const char* orientation(remote::Direction mode) {
  switch (mode) {
    case remote::Direction::volume_up: return "USB UP";
    case remote::Direction::volume_down: return "USB DOWN";
    case remote::Direction::arrow_left: return "ROLL LEFT";
    case remote::Direction::arrow_right: return "ROLL RIGHT";
    default: return "FLAT";
  }
}

const char* command(remote::Direction mode) {
  switch (mode) {
    case remote::Direction::volume_up: return "VOLUME +";
    case remote::Direction::volume_down: return "VOLUME -";
    case remote::Direction::arrow_left: return "LEFT";
    case remote::Direction::arrow_right: return "RIGHT";
    default: return "PLAY/PAUSE";
  }
}

const char* label(remote::Action action) {
  switch (action) {
    case remote::Action::volume_up: return "VOLUME +";
    case remote::Action::volume_down: return "VOLUME -";
    case remote::Action::play_pause: return "PLAY/PAUSE";
    case remote::Action::arrow_left: return "LEFT";
    case remote::Action::arrow_right: return "RIGHT";
    case remote::Action::num_lock: return "NUM LOCK";
    default: return "--";
  }
}

void readBattery() {
  const int level = M5.Power.getBatteryLevel();
  battery_level = static_cast<uint8_t>(constrain(level, 0, 100));
  battery_mv = M5.Power.getBatteryVoltage();
  Serial.printf("BATTERY level %u mV %d charging %d\n", battery_level,
                battery_mv, static_cast<int>(M5.Power.isCharging()));
}

void displayLine(int y, int height, const String& text, uint32_t color, float preferred_size) {
  M5.Display.fillRect(0, y, M5.Display.width(), height, kBackground);
  M5.Display.setTextColor(color, kBackground);
  float size = preferred_size;
  M5.Display.setTextSize(size);
  while ((M5.Display.textWidth(text) > M5.Display.width() - 10 ||
          M5.Display.fontHeight() > height - 2) && size > 0.55f) {
    size -= 0.05f;
    M5.Display.setTextSize(size);
  }
  M5.Display.drawString(text, 5, y + (height - M5.Display.fontHeight()) / 2);
}

void draw(bool force) {
  const String status = ble.status();
  const String battery = battery_mv > 0 ?
      String(battery_level) + "% " + String(battery_mv) + "mV" :
      String("Battery: ") + String(battery_level) + "%";
  const String view = status + '|' + orientation(direction) + '|' + battery;
  if (!force && view == last_view) return;
  M5.Display.startWrite();
  if (force) {
    M5.Display.fillScreen(kBackground);
    displayLine(4, 22, "M5 REMOTE", 0xFFFFFF, 2.15f);
    displayLine(124, 18, "Tilt: VOL +/-", kMuted, 1.5f);
    displayLine(147, 18, "Roll: LEFT/RIGHT", kMuted, 1.5f);
    displayLine(170, 18, "Flat: PLAY/PAUSE", kMuted, 1.5f);
    displayLine(191, 14, "Flat 3x: NUM LOCK", kMuted, 1.4f);
  }
  displayLine(29, 23, status, status == "Connected" ? kAccent : kPending, 2.15f);
  displayLine(58, 22, orientation(direction), kMuted, 2.1f);
  displayLine(82, 24, command(direction), 0xFFFFFF, 2.2f);
  displayLine(216, 22, battery, kMuted, 2.0f);
  M5.Display.endWrite();
  last_view = view;
}

void sleepDisplay() {
  M5.Display.setBrightness(0);
  M5.Display.powerSaveOn();
  Serial.println("DISPLAY sleep");
}

void wakeDisplay() {
  M5.Display.powerSaveOff();
  M5.Display.setBrightness(kBrightness);
  screen_dirty = true;
  Serial.println("DISPLAY wake");
}

void showStopped() {
  wakeDisplay();
  M5.Display.fillScreen(kBackground);
  displayLine(36, 28, "Remote stopped", 0xFFFFFF, 2.2f);
  displayLine(70, 28, "Reset to restart", kMuted, 2.0f);
}

}  // namespace

void setup() {
  M5.begin();
  Serial.begin(115200);
  Serial.println("M5 Media Remote native-0.1");
  WiFi.mode(WIFI_OFF);
  M5.Speaker.end();
  M5.Mic.end();
  M5.Power.setLed(0);
  setCpuFrequencyMhz(80);
  Serial.printf("POWER CPU MHz %u\n", getCpuFrequencyMhz());
  M5.BtnA.setDebounceThresh(0);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(kBrightness);
  M5.Display.setFont(&lgfx::fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.fillScreen(kBackground);

  const uint32_t now = millis();
  static remote::ImuSchedule schedule(now);
  static remote::DisplayIdle idle(now);
  imu_schedule = &schedule;
  display_idle = &idle;
  readBattery();
  last_battery_poll = now;
  if (!ble.begin(battery_level)) {
    Serial.println("BLE init failed");
    M5.Display.setTextColor(0xFFFFFF, kBackground);
    M5.Display.drawString("BLE init failed", 5, 40);
    stopped = true;
    return;
  }
  epoch = ble.epoch();
  draw(true);
  screen_dirty = false;
}

void loop() {
  if (stopped) {
    delay(100);
    return;
  }
  const uint32_t now = millis();
  M5.update();
  const bool button_a = M5.BtnA.isPressed();
  const bool button_b = M5.BtnB.isPressed();
  if (button_a || button_b) {
    if (display_idle->activity(now)) wakeDisplay();
  } else if (display_idle->poll(now)) {
    sleepDisplay();
  }

  ble.poll(now);
  if (ble.epoch() != epoch) {
    epoch = ble.epoch();
    clicks.reset(true);
  }
  const String status = ble.status();
  if (status != last_status) {
    Serial.printf("STATUS %s encrypted=%u bonded=%u key_size=%u suspended=%u\n",
                  status.c_str(), ble.encrypted(), ble.bonded(), ble.keySize(), ble.suspended());
    last_status = status;
  }

  bool fresh_press = false;
  uint32_t sample_ms = 0;
  float ax = 0, ay = 0, az = 0;
  if (imu_schedule->update(now, button_a, fresh_press, sample_ms)) {
    if (fresh_press) tilt.reset();
    const bool accel_ok = M5.Imu.isEnabled() && M5.Imu.getAccel(&ax, &ay, &az);
    if (!accel_ok) ax = ay = az = 0;
    direction = tilt.update(ax, ay, az, sample_ms);
    if (accel_ok && !display_idle->screen_on && shake.update(ax, ay, az, now)) {
      display_idle->activity(now, 1300);
      wakeDisplay();
      Serial.printf("DISPLAY shake wake accel %.3f %.3f %.3f\n", ax, ay, az);
    }
  }
  const auto gesture = tilt.valid ? direction : remote::Direction::invalid;
  const auto action = clicks.update(now, button_a, gesture);
  if (action != remote::Action::none) {
    const bool sent = ble.send(action);
    Serial.printf("INPUT %s %s accel %.3f %.3f %.3f pitch %.1f roll %.1f\n",
                  label(action), sent ? "queued" : "dropped", ax, ay, az,
                  tilt.pitch_deg, tilt.roll_deg);
  }

  if (button_b) {
    if (!side_pressed) {
      side_pressed = true;
      side_down = now;
    } else if (now - side_down >= 1500) {
      ble.stop();
      stopped = true;
      showStopped();
      return;
    }
  } else {
    side_pressed = false;
  }

  if (now - last_battery_poll >= 60000) {
    const uint8_t previous = battery_level;
    readBattery();
    last_battery_poll = now;
    if (battery_level != previous) ble.setBatteryLevel(battery_level);
  }
  if (display_idle->screen_on && (screen_dirty || now - last_draw >= 200)) {
    draw(screen_dirty);
    last_draw = now;
    screen_dirty = false;
  }
  delay(20);
}
