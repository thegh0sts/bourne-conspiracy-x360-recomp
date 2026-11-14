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
// Whether the title screen's "PRESS START" was drawn in the last half second, with keyboard
// and mouse on (keyboard_prompts.cpp): its prompt shows Enter, so Enter has to work there.
bool TitleScreenUp();
// Key glyphs into the HUD textures' button badges (keyboard_prompts.cpp, key_glyphs.cpp);
// called on each controller poll.
void PatchHudTextures(const PPCContext& ctx, uint8_t* base);
}  // namespace tbc::prompts
