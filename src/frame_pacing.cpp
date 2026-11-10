// tbc - host frame limiter and frame-rate measurement.
//
// With the graphics plugin's guest vsync off (--vsync=false), the Xbox 360's vblank
// interrupt is imitated at 1 kHz instead of 60 Hz, so the game no longer waits on it.
// That wait was the expensive part: with guest vsync on, the command processor sleeps
// at least 1 ms on every GPU wait. Something else then has to hold the frame rate, and
// that's this limiter, called at the start of each D3D Swap.
//
// The approach (pace before the swap, absolute deadlines that rebase after a late frame,
// a high-resolution waitable timer plus a short spin) follows the frame pacer of the
// Table Tennis PC port, https://github.com/davidptrssvea/Table-Tennis-Rockstar-PC
// (MIT licence, Copyright (c) 2026 davidptrssvea).

#include "frame_pacing.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <immintrin.h>

#include <atomic>
#include <chrono>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace tbc::pacing {
namespace {

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Wake this long before the deadline and spin the rest.
constexpr int64_t kSpinMarginNs = 600'000;

struct Timer {
  HANDLE handle = nullptr;
  bool high_resolution = false;
  Timer() {
    handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    high_resolution = handle != nullptr;
    if (!handle) handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
};

void SleepUntil(const Timer& timer, int64_t deadline_ns) {
  const int64_t margin = timer.high_resolution ? kSpinMarginNs : 2'000'000;
  const int64_t coarse = deadline_ns - NowNs() - margin;
  if (coarse > 0) {
    if (timer.handle) {
      LARGE_INTEGER due;
      due.QuadPart = -coarse / 100;  // negative = relative, in 100 ns units
      if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer.handle, INFINITE);
      }
    } else {
      ::Sleep(DWORD(coarse / 1'000'000));
    }
  }
  while (NowNs() < deadline_ns) _mm_pause();
}

// Absolute deadlines. A frame that missed its deadline goes out at once and the next
// deadline is rebased, so a stall neither adds a full extra wait nor makes the following
// frames rush to catch up.
struct Schedule {
  int64_t period = 0, deadline = 0;
  int64_t Next(int64_t now, int32_t fps) {
    const int64_t p = 1'000'000'000LL / fps;
    if (p != period) {
      period = p;
      deadline = now;
    }
    deadline += period;
    if (deadline < now) deadline = now;
    return deadline;
  }
};

struct State {
  Timer timer;
  Schedule schedule;
  int64_t window_start = 0;
  int frames = 0;
};

std::atomic<double> g_fps{0.0};

}  // namespace

void BeforeSwap(int32_t target_fps) {
  static State s;  // only the guest render thread swaps
  if (target_fps > 0) {
    SleepUntil(s.timer, s.schedule.Next(NowNs(), target_fps));
  }
  const int64_t now = NowNs();
  if (s.window_start == 0) s.window_start = now;
  ++s.frames;
  const int64_t elapsed = now - s.window_start;
  if (elapsed >= 500'000'000) {
    g_fps.store(s.frames * 1e9 / double(elapsed), std::memory_order_relaxed);
    s.frames = 0;
    s.window_start = now;
  }
}

double CurrentFps() { return g_fps.load(std::memory_order_relaxed); }

}  // namespace tbc::pacing
