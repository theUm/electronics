#include "../src/input.hpp"

#include <cassert>

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
  assert(clicks.update(75, false, Direction::volume_down) == Action::none);
  assert(clicks.update(424, false, Direction::flat) == Action::none);
  assert(clicks.update(425, false, Direction::flat) == Action::volume_up);

  clicks.reset();
  clicks.update(0, true, Direction::volume_up);
  clicks.update(25, true, Direction::volume_up);
  clicks.update(50, false, Direction::flat);
  clicks.update(75, false, Direction::flat);
  clicks.update(150, true, Direction::arrow_left);
  clicks.update(175, true, Direction::arrow_left);
  clicks.update(200, false, Direction::flat);
  assert(clicks.update(225, false, Direction::flat) == Action::none);
  assert(clicks.update(575, false, Direction::flat) == Action::volume_up);
  assert(clicks.update(595, false, Direction::flat) == Action::arrow_left);

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
  clicks.update(225, false, Direction::flat);
  clicks.update(300, true, Direction::flat);
  clicks.update(325, true, Direction::flat);
  clicks.update(350, false, Direction::flat);
  assert(clicks.update(375, false, Direction::flat) == Action::play_pause);
  assert(clicks.update(395, false, Direction::flat) == Action::volume_up);
  assert(clicks.update(415, false, Direction::flat) == Action::play_pause);
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

  Shake shake;
  assert(!shake.update(0, 0, 1, 0));
  assert(!shake.update(0, 0, 1.6f, 50));
  assert(shake.update(0, 0, 1.7f, 100));
  assert(!shake.update(0, 0, 1.7f, 150));
  assert(!shake.update(0, 0, 1.7f, 1800));
  assert(shake.update(0, 0, 1.7f, 1850));

  DisplayIdle display(0);
  assert(!display.poll(4999));
  assert(display.poll(5000));
  assert(display.activity(5010, 1300));
  assert(display.screen_on);
  assert(!display.poll(6309));
  assert(display.poll(6310));
  assert(display.activity(6320));
  assert(!display.poll(11319));
  assert(display.poll(11320));
}
