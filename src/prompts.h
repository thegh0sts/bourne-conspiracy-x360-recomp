// tbc - button prompts: the trace (prompt_trace.cpp) and keyboard labels (keyboard_prompts.cpp).
#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

namespace tbc::prompts {
// Logs the text about to be drawn, with --prompt_trace (DrawString, before any change).
void TraceText(PPCContext& ctx, uint8_t* base);
// A game object's full name ("Package.Object"), through the game's own GetPathName.
std::string ObjectPathName(const PPCContext& ctx, uint8_t* base, uint32_t object);
}  // namespace tbc::prompts
