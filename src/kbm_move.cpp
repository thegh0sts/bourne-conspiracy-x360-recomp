// tbc - keyboard and mouse: W / A / S / D set the direction, the scroll wheel the speed.
//
// The keyboard driver turns W / A / S / D into a left stick pushed all the way (32767) along
// each axis held, so Bourne always moved at his top speed, and two keys together pushed the
// stick about 1.4x past full. A stick's push sets how fast he goes, and from top speed he takes
// the longest to stop (21 Nov input trace: the keys reach the game at once, the carry-on after
// letting go is his slowing down).
//
// Here, on the state the game reads from XInputGetState (input_trace.cpp), a keyboard push
// (each axis exactly 0 or full) becomes a push in the same direction, one stick length long
// whatever the keys (diagonals no stronger than straight), times the move speed. The scroll
// wheel sets the move speed, --kbm_move_step percent a notch (up faster, down slower), from
// --kbm_move_min up to 100%; it starts at --kbm_move_speed and stays where it's left. Sprinting
// (Shift, the right shoulder) is always full speed. Left alone: a controller's stick (anything
// between 0 and full), menus and the title screen (the menus step on the stick), and the
// quick-time events, which qte.cpp reads before this. Keyboard and mouse only (--mnk_mode).
// Each notch shows the speed on screen for a moment (osd.cpp; native renderer). In the first
// mission, once the game's movement tutorial ("Use ... to move and ... to look around") has gone,
// a hint says the wheel sets the speed, for about five seconds (the renderer shows a message for
// 1.5 s, so it's sent again each second); once a session, and not if the wheel's been used.

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "osd.h"
#include "prompts.h"

REXCVAR_DEFINE_INT32(kbm_move_speed, 100, "Input",
                     "Keyboard and mouse: move speed at the start, percent of a full stick push "
                     "(the scroll wheel changes it)")
    .range(10, 100);
REXCVAR_DEFINE_INT32(kbm_move_step, 10, "Input",
                     "Keyboard and mouse: move speed change per scroll wheel notch, percent")
    .range(1, 50);
REXCVAR_DEFINE_INT32(kbm_move_min, 20, "Input",
                     "Keyboard and mouse: the slowest move speed the scroll wheel reaches, percent")
    .range(10, 100);

namespace tbc::kbm_move {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int16_t kFull = 32767;
constexpr uint16_t kRightShoulder = 0x0200;  // sprint

std::atomic<int> g_speed{-1};  // percent; -1 until first used
std::atomic<int> g_wheel{0};   // scroll left over, under one notch
std::atomic<bool> g_wheel_used{false};
bool g_hinted = false;                         // the hint has started (input thread only)
Clock::time_point g_hint_until{}, g_hint_next{};
constexpr const char* kHintTitle = "Keyboard and mouse";
constexpr const char* kHint = "Use scroll wheel to control speed";

bool KeyboardOn() {
  static Clock::time_point next{};
  static bool on = false;
  const auto now = Clock::now();
  if (now >= next) {
    on = rex::cvar::Query<bool>("mnk_mode");
    next = now + std::chrono::milliseconds(500);
  }
  return on;
}

int Speed() {
  int s = g_speed.load();
  if (s < 0) {
    s = std::clamp(REXCVAR_GET(kbm_move_speed), 10, 100);
    int expected = -1;
    if (!g_speed.compare_exchange_strong(expected, s)) s = expected;
  }
  return s;
}

int16_t Load16(const uint8_t* base, uint32_t a) { return int16_t(uint16_t(base[a] << 8 | base[a + 1])); }
void Store16(uint8_t* base, uint32_t a, int16_t v) {
  base[a] = uint8_t(uint16_t(v) >> 8);
  base[a + 1] = uint8_t(v);
}
bool KeyValue(int16_t v) { return v == 0 || v == kFull || v == -kFull; }
}  // namespace

void OnWheel(int32_t scroll_y) {
  if (!KeyboardOn() || scroll_y == 0) return;
  const int total = g_wheel.fetch_add(scroll_y) + scroll_y;
  const int notches = total / 120;  // rex::ui::MouseEvent::kScrollPerDetent
  if (notches == 0) return;
  g_wheel.fetch_sub(notches * 120);
  g_wheel_used.store(true);
  const int lo = std::clamp(REXCVAR_GET(kbm_move_min), 10, 100);
  const int before = Speed();
  const int after = std::clamp(before + notches * REXCVAR_GET(kbm_move_step), lo, 100);
  g_speed.store(after);
  if (after != before) REXLOG_INFO("kbm_move: move speed {}%", after);
  osd::Show("Keyboard and mouse", "Move speed " + std::to_string(after) + "%");  // at the ends too
}

void OnControllerState(uint8_t* base, uint32_t state) {
  if (!KeyboardOn() || prompts::MenuUp() || prompts::TitleScreenUp()) return;
  const int16_t lx = Load16(base, state + 8), ly = Load16(base, state + 10);
  const bool keys = !(lx == 0 && ly == 0) && KeyValue(lx) && KeyValue(ly);  // not still, not a real stick
  const auto now = Clock::now();
  if (!g_hinted && !g_wheel_used.load() && prompts::NavigationTutorialSeen() &&
      !prompts::NavigationTutorialUp()) {  // the movement tutorial has been and gone: the hint
    g_hinted = true;
    g_hint_until = now + std::chrono::seconds(5);
    g_hint_next = now;
  }
  if (now < g_hint_until && now >= g_hint_next && !g_wheel_used.load()) {
    osd::Show(kHintTitle, kHint);
    g_hint_next = now + std::chrono::seconds(1);
  }
  if (!keys) return;
  const uint16_t buttons = uint16_t(base[state + 4] << 8 | base[state + 5]);
  const double speed = (buttons & kRightShoulder) ? 1.0 : Speed() / 100.0;
  const double x = lx / double(kFull), y = ly / double(kFull);
  const double k = speed / std::sqrt(x * x + y * y);
  Store16(base, state + 8, int16_t(std::lround(x * k * kFull)));
  Store16(base, state + 10, int16_t(std::lround(y * k * kFull)));
}
}  // namespace tbc::kbm_move
