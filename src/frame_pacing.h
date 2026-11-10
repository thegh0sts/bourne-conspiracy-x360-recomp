// tbc - host frame limiter and frame-rate measurement.
#pragma once

#include <cstdint>

namespace tbc::pacing {

// Called on the guest render thread at the start of every D3D Swap, before the frame is
// handed to the GPU. With target_fps > 0, waits until that frame's deadline (absolute
// deadlines on a high-resolution timer); with 0, only measures.
void BeforeSwap(int32_t target_fps);

// Game frames per second over the last half second or so (0 until measured).
double CurrentFps();

}  // namespace tbc::pacing
