// tbc - scene feed (n1 research): a per-frame description of what UE3 is drawing, read from the live engine objects
// through their reflection data, written as one JSON file per frame. See scene_feed.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace tbc::scenefeed {

// Game thread, once per controller poll: runs --scenefeed once, --scenefeed_delay seconds after the first call, then
// does whatever work a command asked for. Nothing to do (the usual case) costs a cvar read.
void Poll(uint8_t* base);

// "scenefeed dump <frames> [every]" and the other commands; returns false for anything else.
bool Command(uint8_t* base, const std::string& cmd);
void Tick(uint8_t* base);

// Game thread, from BeginRenderingViewFamily (hybrid.cpp): with --hybrid_fill=4, publish this frame's meshes to the
// n1 renderer (HybridState::scene).
void Live(uint8_t* base);

}  // namespace tbc::scenefeed
