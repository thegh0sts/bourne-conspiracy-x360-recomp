// tbc - the keyboard-and-mouse copy of the game's engine config.
//
// Keyboard and mouse needs changes to the game's own config (Coalesced.ini): the fight attacks
// moved off X / Y onto the triggers (the mouse buttons), the camera's turn rates raised for mouse
// look, aim assist off. Up to 0.3.3 the launcher wrote them into the game's Coalesced.ini, so they
// stayed there for any later run without the launcher's keyboard-and-mouse flags (Play.bat, a
// build started from Visual Studio): a controller's X and Y then did nothing in fights.
//
// Now the launcher writes two configs: the game's own (settings shared by both modes) and,
// with keyboard and mouse ticked, a second one in game\BourneGame\KeyboardMouse with the
// keyboard-and-mouse changes on top. This file mounts that folder as its own read-only device
// and, only with --mnk_mode=true, adds symbolic links so the game's reads of the two files land
// there. The runtime resolves symbolic links repeatedly, so a path through game:\ or d:\ first
// becomes \Device\Harddisk0\Partition1\BourneGame\..., which the links below then match.

#include "kbm_config.h"

#include <memory>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace tbc::kbm_config {
namespace {
constexpr const char* kDevice = "\\Device\\TbcKeyboardMouse";
constexpr const char* kGameDevice = "\\Device\\Harddisk0\\Partition1";
struct Redirect {
  const char* game;  // under the game device
  const char* name;  // in kFolder
};
constexpr Redirect kRedirects[] = {
    {"\\BourneGame\\Config\\Xenon\\Cooked\\Coalesced.ini", "Coalesced.ini"},
    {"\\BourneGame\\Xbox360TOC.txt", "Xbox360TOC.txt"},
};
}  // namespace

void Mount(rex::Runtime* runtime, const std::filesystem::path& game_data_root) {
  if (!runtime || !runtime->file_system() || game_data_root.empty()) return;
  if (!rex::cvar::Query<bool>("mnk_mode")) return;

  const std::filesystem::path folder = std::filesystem::absolute(game_data_root / kFolder);
  std::error_code ec;
  for (const auto& r : kRedirects) {
    if (!std::filesystem::is_regular_file(folder / r.name, ec)) {
      REXLOG_WARN(
          "Keyboard and mouse: no {} in {}; the game's own config is used (start the game from "
          "the launcher with Keyboard and mouse ticked to write it)",
          r.name, folder.string());
      return;
    }
  }

  auto* fs = runtime->file_system();
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kDevice, folder, true);
  if (!device->Initialize() || !fs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("Keyboard and mouse: could not mount {}; the game's own config is used",
                folder.string());
    return;
  }
  for (const auto& r : kRedirects) {
    fs->RegisterSymbolicLink(std::string(kGameDevice) + r.game,
                             std::string(kDevice) + "\\" + r.name);
  }
  REXLOG_INFO("Keyboard and mouse: game config from {}", folder.string());
}
}  // namespace tbc::kbm_config
