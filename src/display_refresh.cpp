// tbc - guest video mode refresh rate from the monitor.
//
// The SDK reports a 60 Hz video mode to the game (video_mode_refresh_rate), and with guest
// vsync on (the launcher's "Original pacing") the game paces its frames off that vblank:
// presentation interval 2 is 30 FPS at 60 Hz. --match_display_refresh=true takes the rate
// from the primary monitor instead, so a 120 Hz panel gets a 120 Hz vblank (60 FPS at
// interval 2, 120 at interval 1). It has no effect with guest vsync off, where tbc's own
// limiter (--frame_rate) paces the game. The SDK clamps the rate to 24-240 Hz. The GPU
// vsync worker reads the rate once at setup, hence OnPreSetup.
//
// From WetRecomp by nikolaygorb (MIT), src/render/display_refresh.cpp.

#include "display_refresh.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(match_display_refresh, false, "Display",
                    "Use the primary monitor's refresh rate for the guest vblank (with vsync on)");

namespace {
// 0 when the rate can't be read.
uint32_t QueryPrimaryDisplayHz() {
#ifdef _WIN32
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  // 0 and 1 stand for the hardware default rate, not a real one.
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
    return mode.dmDisplayFrequency;
  }
#endif
  return 0;
}
}  // namespace

namespace tbc::display {
void MatchDisplayRefreshRate() {
  if (!REXCVAR_GET(match_display_refresh)) {
    return;
  }
  const uint32_t hz = QueryPrimaryDisplayHz();
  if (hz == 0) {
    REXLOG_WARN("Display refresh rate unavailable; keeping video_mode_refresh_rate = {}",
                REXCVAR_GET(video_mode_refresh_rate));
    return;
  }
  REXCVAR_SET(video_mode_refresh_rate, static_cast<double>(hz));
  REXLOG_INFO("video_mode_refresh_rate = {} Hz (primary display)", hz);
}
}  // namespace tbc::display
