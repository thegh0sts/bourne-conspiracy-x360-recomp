// tbc - mouse look: the mouse turns the camera directly (keyboard and mouse).
//
// The keyboard-and-mouse driver normally hands mouse movement to the game as right-stick
// deflection, which then goes through Bourne's stick handling: a 25% dead zone, a turn rate per
// camera mode ([*LookScales] in Xe-BourneGame.ini), a slow zone while aiming and a top yaw
// rate. So small movements did nothing and big ones were capped.
//
// Found with --camera_probe (camera_probe.cpp): the camera follows the player controller's
// Rotation (Actor.Rotation, a rotator at +0xE4: pitch, yaw, roll as ints, 65536 = 360
// degrees), which Bourne's input code turns a little each frame from the stick. So with mouse
// look on, the driver keeps the mouse's movement aside (SDK patch 0009, SetMouseDirect /
// ConsumeMouseDelta) and on each controller poll it is added to that rotation: 1:1, no dead
// zone, curve or cap. Keys bound to the right stick still go through the stick.
//
// While the controller isn't there (menus, loading) or ignores look input
// (PlayerController.bIgnoreLookInput, +0x2E6, a counter), the mouse goes back to the stick /
// is dropped.
//
//   --mouse_look=false                 the mouse moves the right stick, as before
//   --mouse_look_sensitivity=2.0       0.022 degrees per mouse count times this (as in Source
//                                      games, so a value from one of those feels the same)
//   --mouse_look_invert_y=true         mouse up looks down

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "prompts.h"

REXCVAR_DEFINE_BOOL(mouse_look, true, "Input",
                    "Keyboard and mouse: the mouse turns the camera directly (1:1), not through "
                    "the right stick's dead zone and turn rates");
REXCVAR_DEFINE_DOUBLE(mouse_look_sensitivity, 2.0, "Input",
                      "Mouse look: 0.022 degrees of turn per mouse count times this")
    .range(0.05, 20.0);
REXCVAR_DEFINE_BOOL(mouse_look_invert_y, false, "Input", "Mouse look: moving the mouse up looks down");

REX_EXTERN(sub_82920EE8);  // UObject::StaticFindObject(UClass*, UObject* outer, const TCHAR*, UBOOL exact)

namespace {

constexpr uint32_t kRotation = 0xE4;          // Actor.Rotation: pitch, yaw, roll
constexpr uint32_t kIgnoreLookInput = 0x2E6;  // PlayerController.bIgnoreLookInput (byte)
constexpr int32_t kPitchLimit = 13000;        // about 71 degrees up or down

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 | uint32_t(base[addr + 2]) << 8 |
         base[addr + 3];
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

uint32_t FindObject(const PPCContext& ctx, uint8_t* base, const char* name) {
  PPCContext tmp = ctx;
  const uint32_t text = ctx.r1.u32 - 0x300;  // UTF-16BE name below the caller's frame
  for (size_t i = 0;; ++i) {
    const uint16_t c = uint8_t(name[i]);
    base[text + 2 * i] = uint8_t(c >> 8);
    base[text + 2 * i + 1] = uint8_t(c);
    if (!c) break;
  }
  tmp.r1.u32 = ctx.r1.u32 - 0x600;
  tmp.r3.u64 = 0;
  tmp.r4.u64 = 0;
  tmp.r5.u64 = text;
  tmp.r6.u64 = 0;
  sub_82920EE8(tmp, base);
  return tmp.r3.u32;
}

struct State {
  std::mutex mutex;
  uint32_t controller_class = 0;
  uint32_t controller = 0;
  std::unordered_map<uint32_t, bool> is_default;  // controller -> its class's default object
  std::chrono::steady_clock::time_point next_find{};
  double rest_yaw = 0, rest_pitch = 0;  // fractions of a rotator unit carried over
  bool logged = false;
};

State& S() {
  static State s;
  return s;
}

bool Valid(uint8_t* base, State& s, uint32_t o) {
  return o && Readable(o) && Readable(o + 0x300) && Load32(base, o + 0x30) == s.controller_class;
}

// The player controller: the one BnPlayerController that isn't the class's default object.
void Find(State& s, const PPCContext& ctx, uint8_t* base) {
  if (!s.controller_class) s.controller_class = FindObject(ctx, base, "BourneNative.BnPlayerController");
  s.controller = 0;
  if (!s.controller_class) return;
  const uint32_t data = Load32(base, 0x83655774), count = Load32(base, 0x83655778);
  if (!Readable(data) || count > 2000000) return;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = Load32(base, data + 4 * i);
    if (!Valid(base, s, o)) continue;
    auto it = s.is_default.find(o);
    if (it == s.is_default.end()) {
      const bool d = tbc::prompts::ObjectPathName(ctx, base, o).find("Default__") != std::string::npos;
      it = s.is_default.emplace(o, d).first;
    }
    if (!it->second) {
      s.controller = o;
      return;
    }
  }
}

}  // namespace

namespace tbc::camera {

// Every controller poll (input_trace.cpp), right after the driver filled the state.
void MouseLook(const PPCContext& ctx, uint8_t* base) {
  const bool on = REXCVAR_GET(mouse_look) && rex::cvar::Query<bool>("mnk_mode") &&
                  rex::cvar::Query<bool>("mnk_mouse");
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!on) {
    rex::input::mnk::SetMouseDirect(false);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!Valid(base, s, s.controller) || now >= s.next_find) {
    s.next_find = now + std::chrono::seconds(2);
    const uint32_t before = s.controller;
    Find(s, ctx, base);
    if (s.controller != before) {
      REXLOG_INFO("mouse_look: player controller {:08X}", s.controller);
      s.rest_yaw = s.rest_pitch = 0;
    }
  }
  if (!s.controller) {
    rex::input::mnk::SetMouseDirect(false);  // menus: the mouse is the stick again
    return;
  }
  rex::input::mnk::SetMouseDirect(true);
  float dx = 0, dy = 0;
  if (!rex::input::mnk::ConsumeMouseDelta(&dx, &dy)) return;
  if (base[s.controller + kIgnoreLookInput] != 0) return;  // look input off (cutscenes...)
  const double k = REXCVAR_GET(mouse_look_sensitivity) * 0.022 * 65536.0 / 360.0;
  const double yaw = dx * k + s.rest_yaw;
  const double pitch = -dy * k * (REXCVAR_GET(mouse_look_invert_y) ? -1.0 : 1.0) + s.rest_pitch;
  const int32_t yaw_units = int32_t(std::trunc(yaw)), pitch_units = int32_t(std::trunc(pitch));
  s.rest_yaw = yaw - yaw_units;
  s.rest_pitch = pitch - pitch_units;
  const uint32_t r = s.controller + kRotation;
  int32_t p = int32_t(Load32(base, r));
  // Pitch may be kept as 0..65535; work with it as -32768..32767.
  p = int32_t(int16_t(uint16_t(p & 0xFFFF)));
  p = std::clamp(p + pitch_units, -kPitchLimit, kPitchLimit);
  Store32(base, r, uint32_t(p));
  Store32(base, r + 4, uint32_t(int32_t(Load32(base, r + 4)) + yaw_units));
  if (!s.logged) {
    s.logged = true;
    REXLOG_INFO("mouse_look: turning the camera by the mouse (sensitivity {})", REXCVAR_GET(mouse_look_sensitivity));
  }
}

}  // namespace tbc::camera
