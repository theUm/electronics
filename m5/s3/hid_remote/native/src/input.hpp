#pragma once

#include <cmath>
#include <cstdint>

namespace remote {

enum class Direction : int8_t {
  volume_down = -1,
  flat = 0,
  volume_up = 1,
  arrow_left = 3,
  arrow_right = 4,
  invalid = 99,
};

enum class Action : uint16_t {
  none = 0,
  volume_up = 0xE9,
  volume_down = 0xEA,
  play_pause = 0xCD,
  arrow_left = 0x50,
  arrow_right = 0x4F,
  num_lock = 0x53,
};

struct Tilt {
  float pitch_deg = 0;
  float roll_deg = 0;
  bool filtered = false;
  bool valid = false;
  Direction direction = Direction::flat;

  void reset() {
    pitch_deg = roll_deg = 0;
    filtered = valid = false;
    direction = Direction::flat;
  }

  Direction update(float ax, float ay, float az, uint32_t elapsed_ms) {
    const float magnitude2 = ax * ax + ay * ay + az * az;
    if (!std::isfinite(magnitude2) || magnitude2 < 0.36f || magnitude2 > 2.56f) {
      reset();
      return direction;
    }
    valid = true;
    constexpr float rad_to_deg = 57.2957795131f;
    const float pitch = std::atan2(ay, std::sqrt(ax * ax + az * az)) * rad_to_deg;
    const float roll = std::atan2(ax, std::sqrt(ay * ay + az * az)) * rad_to_deg;
    const float alpha = 1.0f - std::pow(0.75f, static_cast<float>(elapsed_ms ? elapsed_ms : 1) / 20.0f);
    if (filtered) {
      pitch_deg += alpha * (pitch - pitch_deg);
      roll_deg += alpha * (roll - roll_deg);
    } else {
      pitch_deg = pitch;
      roll_deg = roll;
      filtered = true;
    }
    const float p = std::fabs(pitch_deg);
    const float r = std::fabs(roll_deg);
    const auto pitch_mode = [&] { return pitch_deg >= 0 ? Direction::volume_up : Direction::volume_down; };
    // The tested UiFlow board reports positive ax for a physical left roll.
    const auto roll_mode = [&] { return roll_deg >= 0 ? Direction::arrow_left : Direction::arrow_right; };
    const bool on_pitch = direction == Direction::volume_up || direction == Direction::volume_down;
    const bool on_roll = direction == Direction::arrow_left || direction == Direction::arrow_right;
    if (!on_pitch && !on_roll) {
      direction = std::fmax(p, r) < 30 ? Direction::flat : (p >= r ? pitch_mode() : roll_mode());
    } else if (on_pitch) {
      if (p < 20) direction = r >= 30 ? roll_mode() : Direction::flat;
      else if (r >= 30 && r >= p + 8) direction = roll_mode();
      else direction = pitch_mode();
    } else {
      if (r < 20) direction = p >= 30 ? pitch_mode() : Direction::flat;
      else if (p >= 30 && p >= r + 8) direction = pitch_mode();
      else direction = roll_mode();
    }
    return direction;
  }
};

struct ImuSchedule {
  uint32_t last_sample;
  bool was_pressed = false;
  explicit ImuSchedule(uint32_t now) : last_sample(now - 100) {}

  bool update(uint32_t now, bool pressed, bool& fresh_press, uint32_t& elapsed_ms) {
    fresh_press = pressed && !was_pressed;
    was_pressed = pressed;
    elapsed_ms = now - last_sample;
    if (fresh_press || pressed || elapsed_ms >= 50) {
      last_sample = now;
      if (!elapsed_ms) elapsed_ms = 1;
      return true;
    }
    return false;
  }
};

struct Clicks {
  static constexpr uint32_t multi_click_gap_ms = 350;
  bool raw = false;
  bool stable = false;
  bool wait_release = false;
  uint32_t raw_since = 0;
  uint32_t down_since = 0;
  uint32_t last_click = 0;
  Direction press_direction = Direction::flat;
  Action pending[2] = {};
  uint8_t pending_count = 0;
  Action ready[4] = {};
  uint8_t ready_head = 0;
  uint8_t ready_count = 0;

  static Action actionFor(Direction direction) {
    switch (direction) {
      case Direction::flat: return Action::play_pause;
      case Direction::volume_up: return Action::volume_up;
      case Direction::volume_down: return Action::volume_down;
      case Direction::arrow_left: return Action::arrow_left;
      case Direction::arrow_right: return Action::arrow_right;
      default: return Action::none;
    }
  }

  void enqueue(Action action) {
    if (ready_count < 4) {
      ready[(ready_head + ready_count) % 4] = action;
      ++ready_count;
    }
  }

  void flushPending() {
    for (uint8_t i = 0; i < pending_count; ++i) enqueue(pending[i]);
    pending_count = 0;
  }

  void reset(bool wait = false) {
    raw = stable = false;
    wait_release = wait;
    raw_since = down_since = last_click = 0;
    press_direction = Direction::flat;
    pending_count = ready_head = ready_count = 0;
  }

  Action update(uint32_t now, bool pressed, Direction direction) {
    if (wait_release) {
      if (!pressed) wait_release = false;
      return Action::none;
    }
    if (pressed != raw) {
      raw = pressed;
      raw_since = now;
    }
    if (raw != stable && now - raw_since >= 25) {
      stable = raw;
      if (stable) {
        down_since = now;
        press_direction = direction;
      } else if (now - down_since <= 500) {
        const Action action = actionFor(press_direction);
        if (action != Action::none) {
          if (pending_count && now - last_click > multi_click_gap_ms) flushPending();
          if (pending_count < 2) pending[pending_count++] = action;
          else {
            if (pending[0] == Action::play_pause &&
                pending[1] == Action::play_pause &&
                action == Action::play_pause) {
              pending_count = 0;
              enqueue(Action::num_lock);
            } else {
              flushPending();
              enqueue(action);
            }
          }
          last_click = now;
        }
      } else {
        flushPending();
        press_direction = Direction::flat;
      }
    }
    if (pending_count && !raw && !stable && now - last_click >= multi_click_gap_ms) {
      flushPending();
    }
    if (!ready_count) return Action::none;
    const Action action = ready[ready_head];
    ready_head = (ready_head + 1) % 4;
    --ready_count;
    return action;
  }
};

struct Shake {
  bool first_hit = false;
  bool triggered = false;
  uint32_t last_hit = 0;
  uint32_t last_trigger = 0;

  bool update(float ax, float ay, float az, uint32_t now) {
    const float magnitude2 = ax * ax + ay * ay + az * az;
    if (!std::isfinite(magnitude2) || magnitude2 < 0.04f) {
      first_hit = false;
      return false;
    }
    if (triggered && now - last_trigger < 1600) return false;
    const bool strong = magnitude2 >= 2.25f || magnitude2 <= 0.25f;
    if (!strong) {
      if (first_hit && now - last_hit > 300) first_hit = false;
      return false;
    }
    if (first_hit && now - last_hit <= 300) {
      first_hit = false;
      triggered = true;
      last_trigger = now;
      return true;
    }
    first_hit = true;
    last_hit = now;
    return false;
  }
};

struct DisplayIdle {
  uint32_t last_activity;
  uint32_t timeout_ms = 5000;
  bool screen_on = true;
  explicit DisplayIdle(uint32_t now) : last_activity(now) {}
  bool activity(uint32_t now, uint32_t duration_ms = 5000) {
    const bool woke = !screen_on;
    last_activity = now;
    timeout_ms = duration_ms;
    screen_on = true;
    return woke;
  }
  bool poll(uint32_t now) {
    if (screen_on && now - last_activity >= timeout_ms) {
      screen_on = false;
      return true;
    }
    return false;
  }
};

}  // namespace remote
