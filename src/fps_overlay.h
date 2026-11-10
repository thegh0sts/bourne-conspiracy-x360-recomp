// tbc - FPS counter in its own small window over the game.
#pragma once

namespace tbc::overlay {

// Starts/stops a thread that shows the frame rate from frame_pacing.cpp in a small
// always-on-top window at the game window's top-left corner.
void StartFpsOverlay();
void StopFpsOverlay();

}  // namespace tbc::overlay
