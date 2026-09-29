#pragma once
#include <cstdint>

namespace remote {
inline uint32_t remaining(uint32_t now, uint32_t since, uint32_t interval) {
  const uint32_t elapsed = now - since;
  return elapsed >= interval ? 0 : interval - elapsed;
}

struct Activity {
  static constexpr uint32_t idle_ms = 3000;
  bool active = true;
  uint32_t last_activity;
  explicit Activity(uint32_t now) : last_activity(now) {}
  bool touch(uint32_t now) {
    const bool woke = !active;
    active = true;
    last_activity = now;
    return woke;
  }
  bool poll(uint32_t now, bool pending) {
    if (active && !pending && !remaining(now, last_activity, idle_ms)) {
      active = false;
      return true;
    }
    return false;
  }
};
}
