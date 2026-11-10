// tbc - frame rate unlock and pacing.
//
// 1. Present interval. The game creates its Direct3D device with
//    D3DPRESENT_PARAMETERS::PresentationInterval = D3DPRESENT_INTERVAL_TWO (code at
//    0x827BFCD8), so with the imitated 60 Hz vblank every frame waits for two vblanks: a
//    hard 30 FPS cap. (The game's own "-novsync" switch would use
//    D3DPRESENT_INTERVAL_IMMEDIATE instead.) The recompiled Direct3D_CreateDevice
//    (sub_822F01E0) is replaced with a wrapper that changes the interval first.
//
// 2. Frame limiter. With the graphics plugin's guest vsync off (--vsync=false, the
//    launcher's default), the vblank is imitated at 1 kHz, so the interval no longer holds
//    the frame rate. The D3D Swap function (sub_822FE688, the one that calls VdSwap) is
//    wrapped to wait for each frame's deadline first (frame_pacing.cpp). It also counts
//    frames for the FPS counter.
//
// --frame_rate=30   original rate (interval two)
// --frame_rate=60   60 FPS (interval one)
// --frame_rate=N    any other N > 0: no interval wait, limiter at N. The engine's own
//                   limiter ([Engine.GameEngine] MaxSmoothedFrameRate, 62 by default)
//                   must be raised too; the launcher does that.
// --frame_rate=0    no interval wait and no limiter.
//
// With guest vsync on (--vsync=true), the original vblank pacing is used and the limiter
// stays off.

#include "generated/default/tbc_pch.h"

#include <rex/cvar.h>

#include <string>

#include "frame_pacing.h"

REXCVAR_DEFINE_INT32(frame_rate, 30, "Game",
                     "Frame rate: 30 (original), 60, another limit, or 0 for no limit");

REX_EXTERN(__imp__sub_822F01E0);
REX_EXTERN(__imp__sub_822FE688);

namespace {
constexpr uint32_t kPresentationIntervalOffset = 0x34;  // in D3DPRESENT_PARAMETERS
constexpr uint32_t kIntervalOne = 0x00000001;
constexpr uint32_t kIntervalTwo = 0x00000002;
constexpr uint32_t kIntervalImmediate = 0x80000000;

// The plugin's guest vsync setting; read on first use (the plugin is loaded by then).
bool GuestVsyncOn() {
  static const bool on = [] {
    const std::string v = rex::cvar::GetFlagByName("vsync");
    const bool result = !(v == "false" || v == "0");
    REXLOG_INFO("Frame pacing: guest vsync {} -> {}", v.empty() ? "(unset)" : v,
                result ? "vblank pacing" : "host frame limiter");
    return result;
  }();
  return on;
}
}  // namespace

// Direct3D_CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
//                       pPresentationParameters (r7), ppReturnedDeviceInterface (r8))
extern "C" REX_FUNC(sub_822F01E0) {
  const int32_t fps = REXCVAR_GET(frame_rate);
  const uint32_t params = ctx.r7.u32;
  if (fps != 30 && params != 0) {
    const uint32_t ea = params + kPresentationIntervalOffset;
    if (REX_LOAD_U32(ea) == kIntervalTwo) {
      const uint32_t interval = fps == 60 ? kIntervalOne : kIntervalImmediate;
      REX_STORE_U32(ea, interval);
      REXLOG_INFO("frame_rate={}: presentation interval 2 -> {:#x}", fps, interval);
    }
  }
  __imp__sub_822F01E0(ctx, base);
}

// D3D Swap (calls VdSwap). Callers: 0x8231193C, 0x827C0014, 0x827DB428 (end of frame).
extern "C" REX_FUNC(sub_822FE688) {
  const int32_t fps = REXCVAR_GET(frame_rate);
  tbc::pacing::BeforeSwap(GuestVsyncOn() ? 0 : fps);
  __imp__sub_822FE688(ctx, base);
}
