// tbc - the controller state the game reads: trigger easing, and a log (development tool).
//
// --input_trace=true writes each change of what the game gets from XInputGetState to the
// log: buttons, both triggers and the sticks, per player. Used to see why the triggers
// (draw / fire) don't answer to keys or mouse buttons while a real controller's do.
//
// sub_823213D8 is the game's XInputGetState(user, state*): it calls
// XamInputGetState(user, 1, state). The state it fills is an XINPUT_STATE, big-endian:
// +0 packet number, +4 buttons, +6 left trigger, +7 right trigger, +8..+15 the sticks.

#include "generated/default/tbc_pch.h"

#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "prompts.h"
#include "qte.h"

REXCVAR_DEFINE_BOOL(input_trace, false, "Debug",
                    "Log each change of the controller state the game reads; development only");

REXCVAR_DEFINE_BOOL(trigger_ramp, false, "Input",
                    "Ease the triggers in over a few polls when they jump from released to "
                    "fully pressed (keys and mouse buttons do; a real trigger travels)");

REX_EXTERN(__imp__sub_823213D8);

namespace tbc::camera {
void Probe(const PPCContext& ctx, uint8_t* base, int16_t rx, int16_t ry);  // camera_probe.cpp
void MouseLook(uint8_t* base, uint32_t state);                              // mouse_look.cpp
}

namespace {
struct Last {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
std::mutex g_mutex;
Last g_last[4];
uint8_t g_ramp[4][2];  // the trigger values last handed to the game, per player

// A key or mouse button takes a trigger from 0 to 255 in one poll; a real trigger passes
// through the values between. Hand the game 0 -> 64 -> 128 -> 192 -> 255 instead, so
// whatever in Bourne waits for the trigger to travel sees it do so. Letting go is at once.
uint8_t Ramp(uint8_t& out, uint8_t in) {
  if (in <= out || in < 255) {
    out = in;
  } else {
    out = uint8_t(out + 64 > 255 ? 255 : out + 64);
  }
  return out;
}

uint16_t Load16(const uint8_t* base, uint32_t addr) {
  return uint16_t(base[addr] << 8 | base[addr + 1]);
}
}  // namespace

extern "C" REX_FUNC(sub_823213D8) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_823213D8(ctx, base);
  if (!state || user >= 4 || ctx.r3.u32 != 0) {
    return;
  }
  tbc::qte::OnControllerState(base, state);
  tbc::prompts::PatchHudTextures(ctx, base);
  tbc::camera::MouseLook(base, state);
  // Title screen, keyboard and mouse: its prompt shows Enter (A's menu key), but the screen
  // only takes Start. A press of A (Enter, Space) also presses Start there.
  if (tbc::prompts::TitleScreenUp()) {
    const uint16_t buttons = Load16(base, state + 4);
    if (buttons & 0x1000) {
      base[state + 4] = uint8_t((buttons | 0x0010) >> 8);
      base[state + 5] = uint8_t(buttons | 0x0010);
    }
  }
  if (REXCVAR_GET(trigger_ramp)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    base[state + 6] = Ramp(g_ramp[user][0], base[state + 6]);
    base[state + 7] = Ramp(g_ramp[user][1], base[state + 7]);
  }
  tbc::camera::Probe(ctx, base, int16_t(Load16(base, state + 12)), int16_t(Load16(base, state + 14)));
  if (!REXCVAR_GET(input_trace)) {
    return;
  }
  Last now;
  now.buttons = Load16(base, state + 4);
  now.lt = base[state + 6];
  now.rt = base[state + 7];
  now.lx = int16_t(Load16(base, state + 8));
  now.ly = int16_t(Load16(base, state + 10));
  now.rx = int16_t(Load16(base, state + 12));
  now.ry = int16_t(Load16(base, state + 14));
  std::lock_guard<std::mutex> lock(g_mutex);
  Last& last = g_last[user];
  // Sticks only count when they cross the middle, so mouse movement doesn't flood the log.
  auto side = [](int16_t v) { return v > 8000 ? 1 : v < -8000 ? -1 : 0; };
  if (now.buttons != last.buttons || now.lt != last.lt || now.rt != last.rt ||
      side(now.lx) != side(last.lx) || side(now.ly) != side(last.ly)) {
    REXLOG_INFO("input_trace: user {} result {:#x} buttons {:#06x} LT {} RT {} L ({}, {}) R ({}, {})",
                user, ctx.r3.u32, now.buttons, now.lt, now.rt, now.lx, now.ly, now.rx, now.ry);
  }
  last = now;
}
