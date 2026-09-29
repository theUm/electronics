#pragma once
#include <M5Unified.h>
#include <bmi270.h>

class Motion {
 public:
  bool begin();
  bool read(float& x, float& y, float& z);
  bool acknowledge();
  bool wakeReady() const { return wake_ready_; }
  void stop();
  // Also services USB insertion/removal routed through the same PM1 IRQ.
  bool serviceIrq();
  bool usbPowered();
 private:
  bool clearOwnedIrqs();
  static int8_t readRegs(uint8_t reg, uint8_t* data, uint32_t len, void*);
  static int8_t writeRegs(uint8_t reg, const uint8_t* data, uint32_t len, void*);
  static void waitUs(uint32_t us, void*);
  bmi2_dev dev_{};
  m5::M5PM1_Class pmic_;
  bool initialized_ = false;
  bool wake_ready_ = false;
  uint8_t stuck_count_ = 0;
};
