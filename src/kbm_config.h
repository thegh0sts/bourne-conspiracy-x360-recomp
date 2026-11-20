// tbc - the keyboard-and-mouse copy of the game's engine config.
#pragma once

#include <filesystem>

namespace rex {
class Runtime;
}

namespace tbc::kbm_config {
// The folder, under the game data root, holding the keyboard-and-mouse Coalesced.ini and
// Xbox360TOC.txt the launcher writes.
inline constexpr const char* kFolder = "BourneGame/KeyboardMouse";

// With --mnk_mode=true, points the game's reads of BourneGame\Config\Xenon\Cooked\Coalesced.ini
// and BourneGame\Xbox360TOC.txt at the copies in kFolder, so the keyboard-and-mouse config
// (fight attacks on the mouse buttons, mouse camera rates) is only loaded in that mode and a
// controller run never sees it. Must run after the runtime's file system is set up and before
// the game starts (OnPostLoadXexImage).
void Mount(rex::Runtime* runtime, const std::filesystem::path& game_data_root);
}  // namespace tbc::kbm_config
