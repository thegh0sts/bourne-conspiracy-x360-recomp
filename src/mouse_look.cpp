// tbc - mouse look: the camera turns by as much as the mouse moves (keyboard and mouse).
//
// The keyboard-and-mouse driver normally hands each poll's mouse movement to the game as
// right-stick deflection. But a stick sets a turn *rate*: the further it's pushed, the faster
// the camera turns. So a mouse moved the camera by its movement times the frame time (at 30
// fps twice as far as at 60), and Bourne's camera (the orbit behaviours of each camera
// strategy, [*OrbitYawCameraBehavior] in BourneCamera.ini) adds a ramp that speeds the turn
// up after the stick has been held for half a second.
//
// With mouse look on, the driver keeps the mouse's movement aside (SDK patch 0009,
// SetMouseDirect / ConsumeMouseDelta), and on each poll it is turned into the stick deflection
// whose turn rate, over the time since the last poll, covers that movement: deflection =
// (degrees moved / seconds since the last poll) / the camera's full-stick turn rate. So the
// camera turns by the mouse's movement whatever the frame rate. The game's stick dead zone is
// skipped the way SDK patch 0004 does it (any movement starts at --mnk_mouse_deadzone). The
// launcher raises the camera's full-stick turn rate (6x the game's 65 degrees a second, passed
// as --mouse_look_full_rate) and turns the ramp off in BourneCamera.ini, so most flicks fit under full stick.
// Movement past full stick isn't dropped: the turn left over is carried into the next polls
// (up to --mouse_look_max_carry degrees), so a fast flick turns the camera all the way, over a
// frame or two. Keys bound to the right stick still push it as before.
//
//   --mouse_look=false                 the mouse moves the right stick, as before
//   --mouse_look_sensitivity=2.0       0.022 degrees per mouse count times this (as in Source
//                                      games)
//   --mouse_look_invert_y=true         mouse up looks down
//   --mouse_look_direct=false          fake a right-stick push instead of adding the mouse's
//                                      movement straight to the camera's angles (the default)
//
// Direct mode (the default): the camera's orbit is kept in three places, all FRotators (pitch,
// yaw, roll as ints, 65536 to a turn), found with --mouse_look_probe:
//   - Bourne's player input (BourneNative.BnPlayerInput, the controller's +0x408):
//     HmPlayerInput.CurrentControlRotation (+0x180), where the orbit yaw / pitch behaviours add
//     stick x turn rate x frame time;
//   - the active strategy's orbit smoother (HmOrbitSmootherCameraBehavior):
//     SmoothedOrbitRotation (+0x44), which eases the view towards it;
//   - the strategy's result, +0x38: in the camera state (strategy +0xE4 -> +0x3C) and the
//     camera's copy (BnCamera +0x370). Some strategies spring the view back to this one, so
//     without it the camera was pulled back within a second.
// Adding the mouse's movement to all of them, in those units, turns the camera exactly as far as
// the mouse moved: no stick dead zone, turn rate cap or the game's look acceleration
// (PlayerInput.LookRightScale grows while the stick is held). The smoother still applies (the
// launcher's Mouse camera smoothing). The right stick isn't touched, so keys bound to it still
// work.

#include "generated/default/tbc_pch.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "prompts.h"

REXCVAR_DEFINE_BOOL(mouse_look, true, "Input",
                    "Keyboard and mouse: the camera turns by as much as the mouse moves, "
                    "whatever the frame rate (needs the launcher's camera settings)");
REXCVAR_DEFINE_DOUBLE(mouse_look_sensitivity, 2.0, "Input",
                      "Mouse look: 0.022 degrees of turn per mouse count times this")
    .range(0.05, 20.0);
REXCVAR_DEFINE_BOOL(mouse_look_invert_y, false, "Input", "Mouse look: moving the mouse up looks down");
REXCVAR_DEFINE_BOOL(mouse_look_direct, true, "Input",
                    "Mouse look: add the mouse's movement straight to the camera's angles (1:1, no "
                    "stick dead zone, turn cap or look acceleration); false fakes a right-stick push");
REXCVAR_DEFINE_BOOL(mouse_look_probe, false, "Debug",
                    "Development: every 100 ms, log the camera's stored orbit (control and smoothed "
                    "rotations, the camera state's strategy result, the camera's cache) to game.log");
REXCVAR_DEFINE_DOUBLE(mouse_look_max_carry, 180.0, "Input",
                      "Mouse look: most turn (degrees) carried into later polls when the mouse moves "
                      "faster than full stick; 0 drops it, as before 0.2.7")
    .range(0.0, 720.0);
REXCVAR_DEFINE_DOUBLE(mouse_look_full_rate, 390.0, "Input",
                      "Mouse look: the camera's turn rate at full stick, degrees a second (the "
                      "launcher sets the game's YawSpeed / PitchSpeed to match)")
    .range(10.0, 2000.0);

namespace {

std::mutex g_mutex;
std::chrono::steady_clock::time_point g_last{};
bool g_logged = false;
double g_carry_yaw = 0.0, g_carry_pitch = 0.0;  // degrees still to turn (past full stick earlier)

uint16_t Load16(const uint8_t* base, uint32_t addr) { return uint16_t(base[addr] << 8 | base[addr + 1]); }
void Store16(uint8_t* base, uint32_t addr, uint16_t v) {
  base[addr] = uint8_t(v >> 8);
  base[addr + 1] = uint8_t(v);
}

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 | uint32_t(base[addr + 2]) << 8 | base[addr + 3];
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  base[addr] = uint8_t(v >> 24);
  base[addr + 1] = uint8_t(v >> 16);
  base[addr + 2] = uint8_t(v >> 8);
  base[addr + 3] = uint8_t(v);
}

bool Readable(uint32_t addr) {
  static std::unordered_map<uint32_t, bool> pages;
  const uint32_t page = addr >> 12;
  if (const auto it = pages.find(page); it != pages.end()) return it->second;
  bool ok = false;
  auto* runtime = rex::Runtime::instance();
  if (runtime && runtime->memory()) {
    if (auto* heap = runtime->memory()->LookupHeap(addr)) {
      uint32_t protect = 0;
      ok = heap->QueryProtect(addr, &protect) && (protect & rex::memory::kMemoryProtectRead);
    }
  }
  pages.emplace(page, ok);
  return ok;
}

bool IsObject(uint32_t a) {
  return a >= 0x40000000 && a < 0xE0000000 && (a & 3) == 0 && Readable(a) && Readable(a + 0x200);
}

// Direct mode: the player's BnPlayerController and the level's BnCamera, found in the object
// table (GObjObjects at 0x83655774). A checkpoint reload or a failed QTE makes new ones while the
// old ones linger (same class, still readable) until garbage collection, so a camera counts as
// live only while its CameraCache time (+0x2A0, a float) keeps changing: once it has stood
// still for a second, the table is scanned again (at most once a second) and a camera whose
// time moved since the last scan is taken, with the controller it points to. While the game is
// paused nothing moves and the current pair is kept.
struct Player {
  uint32_t controller = 0, controller_class = 0, camera = 0, camera_class = 0;
  float camera_time = 0.0f;
  std::chrono::steady_clock::time_point camera_moved{}, next_find{};
  std::unordered_map<uint32_t, float> seen_times;  // camera -> its time at the last scan
  std::unordered_map<uint32_t, std::string> class_names;  // class object -> path name
};
Player g_player;

const std::string& ClassName(const PPCContext& ctx, uint8_t* base, uint32_t cls) {
  auto& names = g_player.class_names;
  auto it = names.find(cls);
  if (it == names.end()) it = names.emplace(cls, tbc::prompts::ObjectPathName(ctx, base, cls)).first;
  return it->second;
}

float CameraTime(const uint8_t* base, uint32_t camera) {
  const uint32_t v = Load32(base, camera + 0x2A0);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// The controller among `controllers` that `camera` points to (its PCOwner), else the last one.
uint32_t OwnerOf(const uint8_t* base, uint32_t camera, const std::vector<std::pair<uint32_t, uint32_t>>& controllers) {
  for (uint32_t off = 0; off < 0x800 && Readable(camera + off); off += 4) {
    const uint32_t v = Load32(base, camera + off);
    for (const auto& c : controllers)
      if (c.first == v) return v;
  }
  return controllers.empty() ? 0 : controllers.back().first;
}

bool FindPlayer(const PPCContext& ctx, uint8_t* base) {
  Player& p = g_player;
  const auto now = std::chrono::steady_clock::now();
  bool ok = IsObject(p.controller) && Load32(base, p.controller + 0x30) == p.controller_class &&
            IsObject(p.camera) && Load32(base, p.camera + 0x30) == p.camera_class;
  if (ok) {
    const float t = CameraTime(base, p.camera);
    if (t != p.camera_time) {
      p.camera_time = t;
      p.camera_moved = now;
      return true;
    }
    if (now - p.camera_moved < std::chrono::seconds(1)) return true;
  }
  // Gone, or its camera has stood still for a second: scan again (at most once a second).
  if (now < p.next_find) return ok;
  p.next_find = now + std::chrono::seconds(1);
  const uint32_t data = Load32(base, 0x83655774), count = Load32(base, 0x83655778);
  if (!Readable(data) || count > 2000000) return ok;
  std::vector<std::pair<uint32_t, uint32_t>> controllers, cameras;  // (object, class)
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = Load32(base, data + 4 * i);
    if (!IsObject(o)) continue;
    const uint32_t cls = Load32(base, o + 0x30);
    if (!IsObject(cls)) continue;
    const std::string& name = ClassName(ctx, base, cls);
    const bool controller = name == "BourneNative.BnPlayerController", camera = name == "BourneNative.BnCamera";
    if (!controller && !camera) continue;
    if (tbc::prompts::ObjectPathName(ctx, base, o).find("Default__") != std::string::npos) continue;
    (controller ? controllers : cameras).emplace_back(o, cls);
  }
  // The camera: one whose time moved since the last scan; else one that's new since then (a
  // reload's, on the first scan after it); else, if the current one is still there (paused),
  // keep the pair; else the last found.
  std::unordered_map<uint32_t, float> times;
  uint32_t camera = 0, camera_class = 0, fresh = 0, fresh_class = 0;
  for (const auto& c : cameras) {
    const float t = CameraTime(base, c.first);
    times[c.first] = t;
    const auto seen = p.seen_times.find(c.first);
    if (seen == p.seen_times.end()) {
      fresh = c.first, fresh_class = c.second;
    } else if (seen->second != t) {
      camera = c.first, camera_class = c.second;
    }
  }
  const bool first_scan = p.seen_times.empty();
  p.seen_times = std::move(times);
  if (!camera && !first_scan) camera = fresh, camera_class = fresh_class;
  if (!camera && ok) return true;
  if (!camera && !cameras.empty()) camera = cameras.back().first, camera_class = cameras.back().second;
  const uint32_t controller = camera ? OwnerOf(base, camera, controllers) : 0;
  if (!camera || !controller) {
    p.controller = p.camera = 0;
    return false;
  }
  for (const auto& c : controllers)
    if (c.first == controller) p.controller_class = c.second;
  if (camera != p.camera || controller != p.controller) {
    REXLOG_INFO("mouse_look_direct: player controller {:08X} (input {:08X}), camera {:08X} ({} cameras, {} controllers)",
                controller, Load32(base, controller + 0x408), camera, cameras.size(), controllers.size());
  }
  p.controller = controller;
  p.camera = camera;
  p.camera_class = camera_class;
  p.camera_time = CameraTime(base, camera);
  p.camera_moved = now;
  return true;
}

// The active camera strategy's orbit smoother (HmOrbitSmootherCameraBehavior): the camera's
// strategy at BnCamera +0x350, its behaviours at +0xF8 (array data, count).
uint32_t Smoother(const PPCContext& ctx, uint8_t* base) {
  const uint32_t strategy = Load32(base, g_player.camera + 0x350);
  if (!IsObject(strategy)) return 0;
  const uint32_t data = Load32(base, strategy + 0xF8), count = Load32(base, strategy + 0xFC);
  if (!Readable(data) || count > 32) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t b = Load32(base, data + 4 * i);
    if (!IsObject(b) || !IsObject(Load32(base, b + 0x30))) continue;
    if (ClassName(ctx, base, Load32(base, b + 0x30)) == "HM_Engine.HmOrbitSmootherCameraBehavior") return b;
  }
  return 0;
}

float LoadF(const uint8_t* base, uint32_t addr) {
  const uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// --mouse_look_probe: where the camera keeps its orbit. Logs, with the right stick: the player
// input's CurrentControlRotation (+0x180), the smoother's SmoothedOrbitRotation (+0x44), the
// camera state's CameraStrategyResult (strategy +0xE4 -> +0x3C, 64 bytes) and the camera's
// CurrentStrategyResult (+0x370) and CameraCache (+0x2A0), as floats (and ints where they
// look like rotator units).
void Probe(const PPCContext& ctx, uint8_t* base, int16_t rx, int16_t ry) {
  static std::chrono::steady_clock::time_point next{};
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::milliseconds(100);
  if (!FindPlayer(ctx, base)) return;
  const uint32_t input = Load32(base, g_player.controller + 0x408);
  const uint32_t smoother = Smoother(ctx, base);
  const uint32_t strategy = Load32(base, g_player.camera + 0x350);
  const uint32_t state = IsObject(strategy) ? Load32(base, strategy + 0xE4) : 0;
  auto words = [&](uint32_t addr, uint32_t n) {
    std::string out;
    char buf[48];
    for (uint32_t i = 0; i < n; ++i) {
      const int32_t v = int32_t(Load32(base, addr + 4 * i));
      const float fv = LoadF(base, addr + 4 * i);
      if (std::abs(v) < 0x200000)
        std::snprintf(buf, sizeof(buf), " %d", v);
      else
        std::snprintf(buf, sizeof(buf), " %.4g", fv);
      out += buf;
    }
    return out;
  };
  REXLOG_INFO("camprobe stick {} {} | ccr{} | smooth{} | state{} | cur{} | cache{}", rx, ry,
              IsObject(input) ? words(input + 0x180, 3) : std::string(" -"),
              smoother ? words(smoother + 0x44, 3) : std::string(" -"),
              IsObject(state) ? words(state + 0x3C, 16) : std::string(" -"), words(g_player.camera + 0x370, 16),
              words(g_player.camera + 0x2A0, 8));
}

void AddRotator(uint8_t* base, uint32_t rot, int32_t pitch, int32_t yaw) {
  Store32(base, rot + 0, uint32_t(int32_t(Load32(base, rot + 0)) + pitch));
  Store32(base, rot + 4, uint32_t(int32_t(Load32(base, rot + 4)) + yaw));
}

// The turn (degrees) to make this poll: what's carried plus this poll's movement. Returns the
// turn rate for it, at most full stick; what doesn't fit stays in `carry` for the next polls.
double TakeRate(double& carry, double degrees, double dt) {
  const double full = REXCVAR_GET(mouse_look_full_rate);
  const double max_carry = REXCVAR_GET(mouse_look_max_carry);
  const double want = carry + degrees;
  const double rate = std::clamp(want / dt, -full, full);
  carry = std::clamp(want - rate * dt, -max_carry, max_carry);
  if (std::abs(carry) < 1e-6) carry = 0.0;  // rounding left over when it all fit: none carried
  return rate;
}

// A turn rate (degrees a second) as stick deflection, past the dead zone.
int32_t Deflection(double rate) {
  if (std::abs(rate) < 1e-4) return 0;  // no movement (not a dead-zone-sized push)
  const double dz = rex::cvar::Query<double>("mnk_mouse_deadzone");
  const double t = std::min(std::abs(rate) / REXCVAR_GET(mouse_look_full_rate), 1.0);
  const double d = (dz + (1.0 - dz) * t) * 32767.0;
  return int32_t(rate < 0 ? -d : d);
}

}  // namespace

namespace tbc::camera {

// Every controller poll (input_trace.cpp), right after the driver filled the state.
void MouseLook(const PPCContext& ctx, uint8_t* base, uint32_t state) {
  const bool on = REXCVAR_GET(mouse_look) && rex::cvar::Query<bool>("mnk_mode") &&
                  rex::cvar::Query<bool>("mnk_mouse");
  rex::input::mnk::SetMouseDirect(on);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (REXCVAR_GET(mouse_look_probe)) {
    Probe(ctx, base, int16_t(Load16(base, state + 12)), int16_t(Load16(base, state + 14)));
  }
  if (!on) {
    g_carry_yaw = g_carry_pitch = 0.0;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  double dt = std::chrono::duration<double>(now - g_last).count();
  g_last = now;
  float dx = 0, dy = 0;
  const bool moved = rex::input::mnk::ConsumeMouseDelta(&dx, &dy);
  if (!moved && g_carry_yaw == 0.0 && g_carry_pitch == 0.0) return;
  if (dt > 0.1) g_carry_yaw = g_carry_pitch = 0.0;  // a long gap (loading): drop what was left
  dt = std::clamp(dt, 0.004, 0.1);  // one poll's time; a long gap (loading) isn't a slow move
  const double degrees = 0.022 * REXCVAR_GET(mouse_look_sensitivity);
  // Direct when the player's controller and camera are there; otherwise (menus, loading, a
  // level without them) the stick method below.
  const uint32_t input = REXCVAR_GET(mouse_look_direct) && FindPlayer(ctx, base)
                             ? Load32(base, g_player.controller + 0x408)
                             : 0;
  if (IsObject(input)) {
    g_carry_yaw = g_carry_pitch = 0.0;
    const uint32_t smoother = Smoother(ctx, base);
    if (!moved) return;
    // Degrees to rotator units (65536 a turn), keeping the fraction for the next poll.
    static double frac_yaw = 0.0, frac_pitch = 0.0;
    const double units = 65536.0 / 360.0;
    frac_yaw += dx * degrees * units;
    frac_pitch += -dy * degrees * units * (REXCVAR_GET(mouse_look_invert_y) ? -1.0 : 1.0);
    const int32_t add_yaw = int32_t(frac_yaw), add_pitch = int32_t(frac_pitch);
    frac_yaw -= add_yaw;
    frac_pitch -= add_pitch;
    // HmPlayerInput.CurrentControlRotation and the smoother's SmoothedOrbitRotation (+0x44):
    // pitch, yaw, roll.
    AddRotator(base, input + 0x180, add_pitch, add_yaw);
    if (smoother) AddRotator(base, smoother + 0x44, add_pitch, add_yaw);
    // The orbit the camera springs back to (a probe found it fixed at the level's start angle
    // while the view was pulled back to it): the strategy result's second rotator, +0x38, in
    // the camera state (strategy +0xE4 -> +0x3C) and the camera's copy (+0x370).
    const uint32_t strategy = Load32(base, g_player.camera + 0x350);
    const uint32_t cam_state = IsObject(strategy) ? Load32(base, strategy + 0xE4) : 0;
    auto add_orbit = [&](uint32_t rot) {
      const int32_t p0 = int32_t(Load32(base, rot)), y0 = int32_t(Load32(base, rot + 4));
      if (std::abs(p0) < 0x200000 && std::abs(y0) < 0x200000) AddRotator(base, rot, add_pitch, add_yaw);
    };
    if (IsObject(cam_state)) add_orbit(cam_state + 0x3C + 0x38);
    add_orbit(g_player.camera + 0x370 + 0x38);
    if (!g_logged) {
      g_logged = true;
      REXLOG_INFO("mouse_look: direct, mouse movement added to the camera's angles (sensitivity {})",
                  REXCVAR_GET(mouse_look_sensitivity));
    }
    return;
  }
  const double yaw_rate = TakeRate(g_carry_yaw, dx * degrees, dt);
  const double pitch_rate =
      TakeRate(g_carry_pitch, -dy * degrees * (REXCVAR_GET(mouse_look_invert_y) ? -1.0 : 1.0), dt);
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
