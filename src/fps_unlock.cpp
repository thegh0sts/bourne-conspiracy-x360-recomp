// tbc - frame rate unlock.
//
// The game creates its Direct3D device with D3DPRESENT_PARAMETERS::PresentationInterval
// set to D3DPRESENT_INTERVAL_TWO (code at 0x827BFCD8), so every frame waits for two 60 Hz
// vblanks: a hard 30 FPS cap. (The game's own "-novsync" switch would use
// D3DPRESENT_INTERVAL_IMMEDIATE instead.) This replaces the recompiled
// Direct3D_CreateDevice (sub_822F01E0) with a wrapper that changes the interval in the
// guest's present parameters before calling the original.
//
// --frame_rate=30  original (interval two)
// --frame_rate=60  interval one: 60 FPS, locked to the 60 Hz vblank
// --frame_rate=0   interval immediate: no vblank wait. The engine's own limiter
//                  ([Engine.GameEngine] MaxSmoothedFrameRate, 62 by default) then sets
//                  the cap, so the launcher raises that too.

#include "generated/default/tbc_pch.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(frame_rate, 30, "Game",
                     "Frame rate: 30 (original), 60, or 0 for no vblank wait (the engine's "
                     "MaxSmoothedFrameRate then caps it)");

REX_EXTERN(__imp__sub_822F01E0);

namespace {
constexpr uint32_t kPresentationIntervalOffset = 0x34;  // in D3DPRESENT_PARAMETERS
constexpr uint32_t kIntervalOne = 0x00000001;
constexpr uint32_t kIntervalTwo = 0x00000002;
constexpr uint32_t kIntervalImmediate = 0x80000000;
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
