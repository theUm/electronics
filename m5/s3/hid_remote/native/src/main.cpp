#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <algorithm>

#include <cstring>
#include "ble_remote.hpp"
#include "input.hpp"
#include "activity.hpp"
#include "motion.hpp"

namespace {

constexpr uint32_t kBackground = 0x101820;
constexpr uint32_t kMuted = 0xA5B7C9;
constexpr uint32_t kAccent = 0x50DCC8;
constexpr uint32_t kPending = 0xFFCA66;
constexpr uint8_t kBrightness = 60;

BleRemote ble;
remote::Tilt tilt;
remote::Clicks clicks;
Motion motion;
remote::Activity* activity = nullptr;
remote::DisplayIdle* display_idle = nullptr;
TaskHandle_t app_task = nullptr;
esp_pm_lock_handle_t usb_lock = nullptr;
esp_pm_lock_handle_t button_lock = nullptr;
esp_pm_lock_handle_t display_sleep_lock = nullptr;
esp_pm_lock_handle_t display_clock_lock = nullptr;
bool display_locks_held = false;
bool usb_powered = false;
bool button_lock_held = false;
bool was_a = false;
uint32_t last_sample = 0;
uint32_t stopped_at = 0;
uint32_t last_usb_check = 0;
uint32_t motion_wakes = 0;
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

void holdDisplay(bool hold) {
  if (hold == display_locks_held) return;
  if (hold) {
    ESP_ERROR_CHECK(esp_pm_lock_acquire(display_sleep_lock));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(display_clock_lock));
  } else {
    ESP_ERROR_CHECK(esp_pm_lock_release(display_clock_lock));
    ESP_ERROR_CHECK(esp_pm_lock_release(display_sleep_lock));
  }
  display_locks_held = hold;
}

uint32_t usbCheckInterval() { return usb_powered ? 1000u : 5000u; }

void ARDUINO_ISR_ATTR wakeTask(void* arg) {
  // gpio_wakeup_enable uses a level interrupt even while the CPU is awake.
  // Mask until task context clears the source, otherwise it starves the task.
  gpio_intr_disable(static_cast<gpio_num_t>(reinterpret_cast<uintptr_t>(arg)));
  BaseType_t higher_priority = pdFALSE;
  if (app_task) vTaskNotifyGiveFromISR(app_task, &higher_priority);
  if (higher_priority) portYIELD_FROM_ISR();
}

void updateUsb() {
  const bool powered = motion.usbPowered();
  if (powered == usb_powered) return;
  usb_powered = powered;
  if (powered) esp_pm_lock_acquire(usb_lock);
  else esp_pm_lock_release(usb_lock);
  Serial.printf("POWER USB=%u light_sleep=%s motion_wakes=%lu\n", powered,
                powered ? "blocked for USB" : "enabled", static_cast<unsigned long>(motion_wakes));
}

void startActivity(uint32_t now) {
  if (activity->touch(now)) {
    tilt.reset();
    last_sample = now - 20;
    Serial.println("POWER active");
  }
}

void waitForWork(uint32_t milliseconds) {
  // Pending notifications survive the final state check and prevent a lost wake.
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::max<uint32_t>(1, milliseconds)));
}

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
  holdDisplay(false);
  Serial.println("DISPLAY sleep");
}

void wakeDisplay() {
  holdDisplay(true);
  M5.Display.powerSaveOff();
  // The loop restores brightness after the complete wake redraw.
  screen_dirty = true;
  Serial.println("DISPLAY wake");
}

void showStopped() {
  wakeDisplay();
  M5.Display.fillScreen(kBackground);
  displayLine(36, 28, "Remote stopped", 0xFFFFFF, 2.2f);
  displayLine(70, 28, "Reset to restart", kMuted, 2.0f);
  M5.Display.setBrightness(kBrightness);
}

}  // namespace

void setup() {
  auto config = M5.config();
  config.internal_imu = false;
  // PM1 owns Reset/Power single-reset and double-shutdown. getPekPress()
  // clears the same status bits PM1 uses for those hardware actions.
  config.pmic_button = false;
  M5.begin(config);
  Serial.begin(115200);
  Serial.println("M5 Media Remote native-0.2-motion");
  WiFi.mode(WIFI_OFF);
  M5.Speaker.end();
  M5.Mic.end();
  M5.Power.setLed(0);
  app_task = xTaskGetCurrentTaskHandle();
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &usb_lock));
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "button", &button_lock));
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "display", &display_sleep_lock));
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_spi", &display_clock_lock));
  holdDisplay(true);
  // Start locked until the PM1 power source has been read.
  ESP_ERROR_CHECK(esp_pm_lock_acquire(usb_lock));
  usb_powered = true;
  esp_pm_config_esp32s3_t pm_config{};
  pm_config.max_freq_mhz = 80;
  pm_config.min_freq_mhz = 40;
  pm_config.light_sleep_enable = true;
  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
  Serial.println("POWER DFS=40..80MHz automatic_light_sleep=enabled");
  M5.BtnA.setDebounceThresh(0);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(kBrightness);
  M5.Display.setFont(&lgfx::fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.fillScreen(kBackground);

  const uint32_t now = millis();
  static remote::Activity active(now);
  static remote::DisplayIdle idle(now);
  activity = &active;
  display_idle = &idle;
  pinMode(13, INPUT_PULLUP);
  if (!motion.begin()) Serial.println("IMU configuration failed: input requires valid acceleration");
  for (int pin : {11, 12, 13}) {
    pinMode(pin, INPUT_PULLUP);
    gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
    attachInterruptArg(pin, wakeTask, reinterpret_cast<void*>(static_cast<uintptr_t>(pin)), FALLING);
    ESP_ERROR_CHECK(gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL));
  }
  ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
  updateUsb();
  last_usb_check = now;
  readBattery();
  last_battery_poll = now;
  ble.setWakeTask(app_task);
  if (!ble.begin(battery_level)) {
    Serial.println("BLE init failed");
    M5.Display.setTextColor(0xFFFFFF, kBackground);
    M5.Display.drawString("BLE init failed", 5, 40);
    stopped = true;
    stopped_at = now;
    return;
  }
  epoch = ble.epoch();
  draw(true);
  screen_dirty = false;
}

void loop() {
  const uint32_t now = millis();
  const bool irq = digitalRead(13) == LOW;
  const bool moved = irq && motion.serviceIrq();
  if (irq || now - last_usb_check >= usbCheckInterval()) {
    updateUsb();
    last_usb_check = now;
  }
  if (stopped) {
    if (display_idle->screen_on && now - stopped_at >= 3000) {
      sleepDisplay();
      display_idle->screen_on = false;
    }
    if (digitalRead(13) == HIGH) gpio_intr_enable(GPIO_NUM_13);
    const uint32_t screen_wait = display_idle->screen_on ? remote::remaining(now, stopped_at, 3000) : 60000;
    waitForWork(std::min(screen_wait, remote::remaining(now, last_usb_check, usbCheckInterval())));
    return;
  }
  if (moved) {
    if (!activity->active) ++motion_wakes;
    startActivity(now);
  }
  M5.update();
  const bool button_a = M5.BtnA.isPressed();
  const bool button_b = M5.BtnB.isPressed();
  const bool pressed = button_a || button_b;
  if (pressed != button_lock_held) {
    if (pressed) esp_pm_lock_acquire(button_lock);
    else esp_pm_lock_release(button_lock);
    button_lock_held = pressed;
  }
  if (pressed) startActivity(now);
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

  const bool fresh_press = button_a && !was_a;
  was_a = button_a;
  const uint32_t sample_ms = now - last_sample;
  float ax = 0, ay = 0, az = 0;
  if (fresh_press || ((activity->active || !motion.wakeReady()) && sample_ms >= 20)) {
    last_sample = now;
    if (fresh_press) tilt.reset();
    const bool accel_ok = motion.read(ax, ay, az);
    if (!accel_ok) ax = ay = az = 0;
    direction = tilt.update(ax, ay, az, sample_ms);
  }
  const auto gesture = tilt.valid ? direction : remote::Direction::invalid;
  const auto action = clicks.update(now, button_a, gesture);
  if (action != remote::Action::none) {
    const bool sent = ble.send(action);
    Serial.printf("INPUT release_to_queue_ms=%lu\n", static_cast<unsigned long>(now - clicks.raw_since));
    Serial.printf("INPUT %s %s accel %.3f %.3f %.3f pitch %.1f roll %.1f\n",
                  label(action), sent ? "queued" : "dropped", ax, ay, az,
                  tilt.pitch_deg, tilt.roll_deg);
  }
  // Start newly queued HID reports this iteration, without waiting for another tick.
  ble.poll(now);

  if (button_b) {
    if (!side_pressed) {
      side_pressed = true;
      side_down = now;
    } else if (now - side_down >= 1500) {
      ble.stop();
      stopped = true;
      stopped_at = now;
      motion.stop();
      for (int pin : {11, 12}) {
        detachInterrupt(pin);
        gpio_wakeup_disable(static_cast<gpio_num_t>(pin));
      }
      if (button_lock_held) {
        esp_pm_lock_release(button_lock);
        button_lock_held = false;
      }
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
    if (usb_powered) {
      Serial.printf("POWER motion_wakes=%lu irq_ok=%u\n",
                    static_cast<unsigned long>(motion_wakes), motion.wakeReady());
      esp_pm_dump_locks(stdout);
    }
  }
  if (display_idle->screen_on && (screen_dirty || now - last_draw >= 200)) {
    draw(screen_dirty);
    if (screen_dirty) M5.Display.setBrightness(kBrightness);
    last_draw = now;
    screen_dirty = false;
  }
  if (activity->poll(now, clicks.busy() || ble.pendingInput() || pressed)) {
    Serial.println("POWER idle: waiting for motion");
  }
  uint32_t wait_ms = ble.waitMs(now);
  if (activity->active || !motion.wakeReady() || clicks.busy()) wait_ms = std::min(wait_ms, 10u);
  if (display_idle->screen_on) {
    wait_ms = std::min(wait_ms, remote::remaining(now, display_idle->last_activity, display_idle->timeout_ms));
  }
  wait_ms = std::min(wait_ms, remote::remaining(now, last_battery_poll, 60000));
  wait_ms = std::min(wait_ms, remote::remaining(now, last_usb_check, usbCheckInterval()));
  // A failed IRQ path retains periodic source checks as well as acceleration reads.
  if (!motion.wakeReady()) wait_ms = std::min(wait_ms, 50u);
  for (int pin : {11, 12, 13}) {
    if (pin == 13 && !motion.wakeReady()) continue;
    if (digitalRead(pin) == HIGH) gpio_intr_enable(static_cast<gpio_num_t>(pin));
    else wait_ms = std::min(wait_ms, 10u);
  }
  waitForWork(wait_ms);
}
