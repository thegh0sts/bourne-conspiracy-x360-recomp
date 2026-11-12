// tbc - don't busy-spin the CPU while the game waits on the GPU.
//
// sub_822F0108 is the XDK's wait poll, right before Direct3D_CreateDevice in the D3D library:
// a delay of 4 x 8 "or r31,r31,r31" (the PowerPC very-low-priority hint), then a check of the
// device's state and a 5000-tick timeout; it returns nonzero to keep waiting and zero once the
// wait is over. Its six callers read the time base (mftb), then call it in a loop until it
// returns zero; one of them is in the D3D Swap (0x822FED48, see fps_unlock.cpp). On a console
// the hint lets the other hardware thread run; recompiled, it's nothing, so the waiting thread
// spins a CPU core flat out while the emulated GPU catches up.
//
// The wrapper only changes the pause between polls; the result is passed on untouched.
//   --gpu_wait_mode=0  busy spin (original)
//   --gpu_wait_mode=1  yield the core (default)
//   --gpu_wait_mode=2  sleep 200 us
//
// Idea and the same hook for Wet: WetRecomp by nikolaygorb (MIT), src/render/gpu_wait_hook.cpp.

#include "generated/default/tbc_pch.h"

#include <chrono>
#include <thread>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(gpu_wait_mode, 1, "Performance",
                     "While the game waits for the GPU: 0 = busy spin (original), "
                     "1 = yield the core, 2 = sleep 200 us");

REX_EXTERN(__imp__sub_822F0108);

extern "C" REX_FUNC(sub_822F0108) {
  __imp__sub_822F0108(ctx, base);
  if (ctx.r3.u32 == 0) {
    return;  // the wait is over
  }
  switch (REXCVAR_GET(gpu_wait_mode)) {
    case 1:
      std::this_thread::yield();
      break;
    case 2:
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      break;
    default:
      break;
  }
}
