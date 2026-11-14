// tbc - mouse look: the camera turns by as much as the mouse moves (keyboard and mouse).
//
// The keyboard-and-mouse driver normally hands each poll's mouse movement to the game as
// right-stick deflection. But a stick sets a turn *rate*: the further it's pushed, the faster
// the camera turns. So a mouse moved the camera by its movement times the frame time (at 30
// fps twice as far as at 60), and Bourne's camera (found with --camera_probe: the orbit
// behaviours of each camera strategy, [*OrbitYawCameraBehavior] in BourneCamera.ini) adds a
// ramp that speeds the turn up after the stick has been held for half a second.
//
// With mouse look on, the driver keeps the mouse's movement aside (SDK patch 0009,
// SetMouseDirect / ConsumeMouseDelta), and on each poll it is turned into the stick deflection
// whose turn rate, over the time since the last poll, covers that movement: deflection =
// (degrees moved / seconds since the last poll) / the camera's full-stick turn rate. So the
// camera turns by the mouse's movement whatever the frame rate. The game's stick dead zone is
// skipped the way SDK patch 0004 does it (any movement starts at --mnk_mouse_deadzone). The
// launcher raises the camera's full-stick turn rate (kMaxRate, 4x the game's 65 degrees a
// second) and turns the ramp off in BourneCamera.ini, so fast flicks fit under full stick.
// Keys bound to the right stick still push it as before.
//
//   --mouse_look=false                 the mouse moves the right stick, as before
//   --mouse_look_sensitivity=2.0       0.022 degrees per mouse count times this (as in Source
//                                      games)
//   --mouse_look_invert_y=true         mouse up looks down

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mouse_look, true, "Input",
                    "Keyboard and mouse: the camera turns by as much as the mouse moves, "
                    "whatever the frame rate (needs the launcher's camera settings)");
REXCVAR_DEFINE_DOUBLE(mouse_look_sensitivity, 2.0, "Input",
                      "Mouse look: 0.022 degrees of turn per mouse count times this")
    .range(0.05, 20.0);
REXCVAR_DEFINE_BOOL(mouse_look_invert_y, false, "Input", "Mouse look: moving the mouse up looks down");
REXCVAR_DEFINE_DOUBLE(mouse_look_full_rate, 260.0, "Input",
                      "Mouse look: the camera's turn rate at full stick, degrees a second (the "
                      "launcher sets the game's YawSpeed / PitchSpeed to match)")
    .range(10.0, 2000.0);

namespace {

std::mutex g_mutex;
std::chrono::steady_clock::time_point g_last{};
bool g_logged = false;

uint16_t Load16(const uint8_t* base, uint32_t addr) { return uint16_t(base[addr] << 8 | base[addr + 1]); }
void Store16(uint8_t* base, uint32_t addr, uint16_t v) {
  base[addr] = uint8_t(v >> 8);
  base[addr + 1] = uint8_t(v);
}

// A turn rate (degrees a second) as stick deflection, past the dead zone.
int32_t Deflection(double rate) {
  if (rate == 0.0) return 0;
  const double dz = rex::cvar::Query<double>("mnk_mouse_deadzone");
  const double t = std::min(std::abs(rate) / REXCVAR_GET(mouse_look_full_rate), 1.0);
  const double d = (dz + (1.0 - dz) * t) * 32767.0;
  return int32_t(rate < 0 ? -d : d);
}

}  // namespace

namespace tbc::camera {

// Every controller poll (input_trace.cpp), right after the driver filled the state.
void MouseLook(uint8_t* base, uint32_t state) {
  const bool on = REXCVAR_GET(mouse_look) && rex::cvar::Query<bool>("mnk_mode") &&
                  rex::cvar::Query<bool>("mnk_mouse");
  rex::input::mnk::SetMouseDirect(on);
  if (!on) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  double dt = std::chrono::duration<double>(now - g_last).count();
  g_last = now;
  float dx = 0, dy = 0;
  if (!rex::input::mnk::ConsumeMouseDelta(&dx, &dy)) return;
  dt = std::clamp(dt, 0.004, 0.1);  // one poll's time; a long gap (loading) isn't a slow move
  const double degrees = 0.022 * REXCVAR_GET(mouse_look_sensitivity);
  const double yaw_rate = dx * degrees / dt;
  const double pitch_rate = -dy * degrees / dt * (REXCVAR_GET(mouse_look_invert_y) ? -1.0 : 1.0);
  const int32_t rx = int16_t(Load16(base, state + 12)) + Deflection(yaw_rate);
  const int32_t ry = int16_t(Load16(base, state + 14)) + Deflection(pitch_rate);
  Store16(base, state + 12, uint16_t(int16_t(std::clamp(rx, -32768, 32767))));
  Store16(base, state + 14, uint16_t(int16_t(std::clamp(ry, -32768, 32767))));
  if (!g_logged) {
    g_logged = true;
    REXLOG_INFO("mouse_look: mouse movement as camera turn (sensitivity {}, full stick {} degrees a second)",
                REXCVAR_GET(mouse_look_sensitivity), REXCVAR_GET(mouse_look_full_rate));
  }
}

}  // namespace tbc::camera
