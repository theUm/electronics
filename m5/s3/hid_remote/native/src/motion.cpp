#include "motion.hpp"
#include <driver/gpio.h>

int8_t Motion::readRegs(uint8_t reg, uint8_t* data, uint32_t len, void*) {
  return M5.In_I2C.readRegister(0x68, reg, data, len, 400000) ? 0 : -1;
}
int8_t Motion::writeRegs(uint8_t reg, const uint8_t* data, uint32_t len, void*) {
  return M5.In_I2C.writeRegister(0x68, reg, data, len, 400000) ? 0 : -1;
}
void Motion::waitUs(uint32_t us, void*) {
  if (us >= 1000) delay(us / 1000);
  if (us % 1000) delayMicroseconds(us % 1000);
}

bool Motion::begin() {
  // PM1 GPIO level-change IRQs (0x40/0x43) wake the sleeping ESP via GPIO13.
  // GPIO_WAKE_EN instead powers PM1/the system back on after shutdown; its
  // setting survives Reset/download mode, so explicitly undo our old setting.
  const bool power_wake_disabled = M5.In_I2C.bitOff(0x6e, 0x18, 0x10, 100000);
  uint8_t wake_enable = 0, button_config[2]{};
  if (M5.In_I2C.readRegister(0x6e, 0x18, &wake_enable, 1, 100000) &&
      M5.In_I2C.readRegister(0x6e, 0x49, button_config, 2, 100000)) {
    Serial.printf("PM1 gpio_wake=0x%02x button_cfg=0x%02x/0x%02x double_off=%u\n",
                  wake_enable, button_config[0], button_config[1], !(button_config[1] & 1));
  }
  if (!power_wake_disabled) {
    Serial.println("PM1 failed to disable IMU power-on wake");
    return false;
  }
  dev_.intf = BMI2_I2C_INTF;
  dev_.read = readRegs;
  dev_.write = writeRegs;
  dev_.delay_us = waitUs;
  dev_.read_write_len = 32;
  const int8_t result = bmi270_init(&dev_);
  if (result != BMI2_OK) {
    Serial.printf("IMU init failed %d\n", result);
    return false;
  }
  bmi2_sens_config cfg[2]{};
  cfg[0].type = BMI2_ACCEL;
  cfg[1].type = BMI2_ANY_MOTION;
  if (bmi270_get_sensor_config(cfg, 2, &dev_) != BMI2_OK) return false;
  cfg[0].cfg.acc.odr = BMI2_ACC_ODR_50HZ;
  cfg[0].cfg.acc.range = BMI2_ACC_RANGE_4G;
  cfg[0].cfg.acc.bwp = BMI2_ACC_OSR2_AVG2;
  cfg[0].cfg.acc.filter_perf = BMI2_POWER_OPT_MODE;
  cfg[1].cfg.any_motion.duration = 2; // 20 ms units.
  cfg[1].cfg.any_motion.threshold = 205; // 0.488 mg units: about 100 mg.
  cfg[1].cfg.any_motion.select_x = 1;
  cfg[1].cfg.any_motion.select_y = 1;
  cfg[1].cfg.any_motion.select_z = 1;
  if (bmi270_set_sensor_config(cfg, 2, &dev_) != BMI2_OK) return false;
  const uint8_t features[] = {BMI2_ACCEL, BMI2_ANY_MOTION};
  if (bmi270_sensor_enable(features, 2, &dev_) != BMI2_OK) return false;
  // Only ACC on: gyro, auxiliary interface and temperature remain off.
  uint8_t power = 0x04;
  if (bmi2_set_regs(BMI2_PWR_CTRL_ADDR, &power, 1, &dev_) != BMI2_OK) return false;
  initialized_ = true;
  bmi2_int_pin_config pin{};
  pin.pin_type = BMI2_INT1;
  pin.int_latch = BMI2_INT_LATCH;
  pin.pin_cfg[0].lvl = BMI2_INT_ACTIVE_LOW;
  pin.pin_cfg[0].od = BMI2_INT_PUSH_PULL;
  pin.pin_cfg[0].output_en = BMI2_INT_OUTPUT_ENABLE;
  bmi2_sens_int_config mapping{};
  mapping.type = BMI2_ANY_MOTION;
  mapping.hw_int_pin = BMI2_INT1;
  wake_ready_ = bmi2_set_int_pin_config(&pin, &dev_) == BMI2_OK &&
      bmi270_map_feat_int(&mapping, 1, &dev_) == BMI2_OK &&
      bmi2_set_adv_power_save(BMI2_ENABLE, &dev_) == BMI2_OK;
  using P = m5::M5PM1_Class;
  wake_ready_ = pmic_.setGPIOFunction(P::gpio4, P::gpio) &&
      pmic_.setGPIOMode(P::gpio4, P::input) &&
      pmic_.setGPIOPull(P::gpio4, P::pull_up) &&
      pmic_.setGPIOFunction(P::gpio1, P::irq) &&
      pmic_.setGPIOMode(P::gpio1, P::output) &&
      pmic_.setGPIODrive(P::gpio1, P::open_drain) &&
      pmic_.setGPIOPull(P::gpio1, P::pull_up) &&
      pmic_.setButtonIRQMaskBits(0x07) &&
      pmic_.setSystemIRQMaskBits(0x3c) &&
      pmic_.setGPIOIRQMaskBits(wake_ready_ ? 0x0f : 0x1f) && wake_ready_;
  serviceIrq();
  Serial.printf("IMU accel=50Hz gyro=off motion_irq=%u\n", wake_ready_);
  return true;
}

bool Motion::read(float& x, float& y, float& z) {
  if (!initialized_) return false;
  bmi2_sens_data data{};
  if (bmi2_get_sensor_data(&data, &dev_) != BMI2_OK) return false;
  // Same board axis transform as M5Unified's StickS3 driver.
  x = data.acc.y / 8192.0f;
  y = -data.acc.x / 8192.0f;
  z = data.acc.z / 8192.0f;
  return true;
}

bool Motion::acknowledge() {
  uint16_t status = 0;
  return initialized_ && bmi2_get_int_status(&status, &dev_) == BMI2_OK &&
         (status & BMI270_ANY_MOT_STATUS_MASK);
}

bool Motion::serviceIrq() {
  const bool moved = acknowledge();
  const bool cleared = clearOwnedIrqs();
  // A constantly asserted source must not prevent the application from blocking.
  if (!cleared || digitalRead(13) == LOW) {
    if (++stuck_count_ >= 8) {
      wake_ready_ = false;
      pmic_.setGPIOIRQMaskBits(0x1f);
      pmic_.setSystemIRQMaskBits(0x3f);
      clearOwnedIrqs();
      gpio_wakeup_disable(GPIO_NUM_13);
      detachInterrupt(13);
      Serial.println("IMU IRQ stuck: falling back to polling");
    }
  } else stuck_count_ = 0;
  return moved;
}

bool Motion::clearOwnedIrqs() {
  // Never clear button status (0x42): bits 0/2 also drive PM1 reset/shutdown.
  // Button IRQs are masked; only GPIO and USB/system sources belong to us.
  const bool gpio_cleared = pmic_.clearGPIOIRQStatus();
  const bool system_cleared = pmic_.clearSystemIRQStatus();
  return gpio_cleared && system_cleared;
}

bool Motion::usbPowered() {
  uint8_t source = 0;
  // Fail awake on an I2C error, preserving USB/debug access.
  if (!M5.In_I2C.readRegister(0x6e, 0x04, &source, 1, 100000)) return true;
  return source & 1;
}

void Motion::stop() {
  const uint8_t feature = BMI2_ANY_MOTION;
  if (initialized_) bmi270_sensor_disable(&feature, 1, &dev_);
  pmic_.setGPIOIRQMaskBits(0x1f);
  clearOwnedIrqs();
  wake_ready_ = false;
}
