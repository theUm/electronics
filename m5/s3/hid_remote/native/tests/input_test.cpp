#include "../src/input.hpp"
#include "../src/activity.hpp"

#include <cassert>
#include <initializer_list>

int main() {
  using namespace remote;
  Tilt tilt;
  assert(tilt.update(0, 0, 1, 20) == Direction::flat);
  tilt.reset();
  assert(tilt.update(0, 0.8f, 0.6f, 20) == Direction::volume_up);
  tilt.reset();
  assert(tilt.update(0, -0.8f, 0.6f, 20) == Direction::volume_down);
  tilt.reset();
  assert(tilt.update(0.8f, 0, 0.6f, 20) == Direction::arrow_left);
  tilt.reset();
  assert(tilt.update(-0.8f, 0, 0.6f, 20) == Direction::arrow_right);
  assert(tilt.update(0, 0, 0, 20) == Direction::flat && !tilt.valid);

  Clicks clicks;
  assert(clicks.update(0, true, Direction::volume_up) == Action::none);
  assert(clicks.update(25, true, Direction::volume_up) == Action::none);
  assert(clicks.update(50, false, Direction::volume_down) == Action::none);
  assert(clicks.update(75, false, Direction::volume_down) == Action::volume_up);
  assert(clicks.update(424, false, Direction::flat) == Action::none);
  assert(clicks.update(425, false, Direction::flat) == Action::none);

  clicks.reset();
  clicks.update(0, true, Direction::volume_up);
  clicks.update(25, true, Direction::volume_up);
  clicks.update(50, false, Direction::flat);
  assert(clicks.update(75, false, Direction::flat) == Action::volume_up);
  clicks.update(150, true, Direction::arrow_left);
  clicks.update(175, true, Direction::arrow_left);
  clicks.update(200, false, Direction::flat);
  assert(clicks.update(225, false, Direction::flat) == Action::arrow_left);
  assert(clicks.update(575, false, Direction::flat) == Action::none);
  assert(clicks.update(595, false, Direction::flat) == Action::none);

  clicks.reset();
  clicks.update(0, true, Direction::flat);
  clicks.update(25, true, Direction::flat);
  clicks.update(50, false, Direction::flat);
  clicks.update(75, false, Direction::flat);
  clicks.update(150, true, Direction::flat);
  clicks.update(175, true, Direction::flat);
  clicks.update(200, false, Direction::flat);
  clicks.update(225, false, Direction::flat);
  clicks.update(300, true, Direction::flat);
  clicks.update(325, true, Direction::flat);
  clicks.update(350, false, Direction::flat);
  assert(clicks.update(375, false, Direction::flat) == Action::num_lock);
  assert(clicks.update(725, false, Direction::flat) == Action::none);

  clicks.reset();
  clicks.update(0, true, Direction::flat);
  clicks.update(25, true, Direction::flat);
  clicks.update(50, false, Direction::flat);
  clicks.update(75, false, Direction::flat);
  clicks.update(150, true, Direction::volume_up);
  clicks.update(175, true, Direction::volume_up);
  clicks.update(200, false, Direction::flat);
  assert(clicks.update(225, false, Direction::flat) == Action::play_pause);
  assert(clicks.update(226, false, Direction::flat) == Action::volume_up);
  clicks.update(300, true, Direction::flat);
  clicks.update(325, true, Direction::flat);
  clicks.update(350, false, Direction::flat);
  assert(clicks.update(375, false, Direction::flat) == Action::none);
  assert(clicks.update(724, false, Direction::flat) == Action::none);
  assert(clicks.update(725, false, Direction::flat) == Action::play_pause);
  assert(clicks.update(765, false, Direction::flat) == Action::none);

  clicks.reset();
  clicks.update(0, true, Direction::flat);
  clicks.update(25, true, Direction::flat);
  clicks.update(600, false, Direction::flat);
  assert(clicks.update(625, false, Direction::flat) == Action::none);
  clicks.reset(true);
  assert(clicks.update(0, true, Direction::flat) == Action::none);
  assert(clicks.update(100, false, Direction::flat) == Action::none);

  ImuSchedule schedule(1000);
  bool fresh = false;
  uint32_t elapsed = 0;
  assert(schedule.update(1000, false, fresh, elapsed));
  assert(!schedule.update(1020, false, fresh, elapsed));
  assert(schedule.update(1040, true, fresh, elapsed) && fresh);
  assert(schedule.update(1060, true, fresh, elapsed) && !fresh);
  assert(!schedule.update(1080, false, fresh, elapsed));
  assert(schedule.update(1110, false, fresh, elapsed));

  DisplayIdle display(0);
  assert(!display.poll(4999));
  assert(display.poll(5000));
  assert(display.activity(5010));
  assert(display.screen_on);
  assert(!display.poll(10009));
  assert(display.poll(10010));
  assert(display.activity(10020));
  assert(!display.poll(15019));
  assert(display.poll(15020));

  // A flat single/double still waits; invalid samples never become Play/Pause.
  clicks.reset();
  clicks.update(0, true, Direction::flat);
  clicks.update(25, true, Direction::flat);
  clicks.update(50, false, Direction::flat);
  assert(clicks.update(75, false, Direction::flat) == Action::none);
  assert(clicks.busy());
  assert(clicks.update(425, false, Direction::flat) == Action::play_pause);
  assert(!clicks.busy());
  clicks.reset();
  clicks.update(0, true, Direction::invalid);
  clicks.update(25, true, Direction::invalid);
  clicks.update(50, false, Direction::flat);
  assert(clicks.update(75, false, Direction::flat) == Action::none);
  assert(clicks.update(425, false, Direction::flat) == Action::none);
  clicks.reset();
  for (uint32_t base : {0u, 150u}) {
    clicks.update(base, true, Direction::flat);
    clicks.update(base + 25, true, Direction::flat);
    clicks.update(base + 50, false, Direction::flat);
    assert(clicks.update(base + 75, false, Direction::flat) == Action::none);
  }
  assert(clicks.update(575, false, Direction::flat) == Action::play_pause);
  assert(clicks.update(576, false, Direction::flat) == Action::play_pause);
  assert(clicks.update(577, false, Direction::flat) == Action::none);
  // A connection change discards a partially collected gesture.
  clicks.reset();
  clicks.update(0, true, Direction::flat);
  clicks.update(25, true, Direction::flat);
  clicks.update(50, false, Direction::flat);
  clicks.update(75, false, Direction::flat);
  clicks.reset(true);
  clicks.update(100, false, Direction::flat);
  assert(clicks.update(500, false, Direction::flat) == Action::none);

  Activity activity(0);
  assert(!activity.poll(2999, false));
  assert(!activity.poll(3000, true)); // Pending HID/click/button work blocks idle.
  assert(activity.poll(3001, false));
  assert(!activity.active);
  assert(activity.touch(3010));
  assert(!activity.poll(6009, false));
  assert(activity.poll(6010, false));
  activity.touch(UINT32_MAX - 1000);
  assert(!activity.poll(1998, false));
  assert(activity.poll(1999, false)); // millis wraparound.
  assert(remaining(10, UINT32_MAX - 9, 40) == 20);
}
