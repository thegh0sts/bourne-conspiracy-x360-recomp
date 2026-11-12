// tbc - guest video mode refresh rate from the monitor.
#pragma once

namespace tbc::display {
// With --match_display_refresh=true, sets video_mode_refresh_rate to the primary monitor's
// rate. Must run before the runtime is set up (OnPreSetup).
void MatchDisplayRefreshRate();
}  // namespace tbc::display
