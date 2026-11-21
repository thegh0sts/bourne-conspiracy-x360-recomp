// tbc - the keyboard-and-mouse copy of the game's engine config.
//
// Keyboard and mouse needs changes to the game's own config (Coalesced.ini): the fight attacks
// moved off X / Y onto the triggers (the mouse buttons), the camera's turn rates raised for mouse
// look, aim assist off. Up to 0.3.3 the launcher wrote them into the game's Coalesced.ini, so they
// stayed there for any later run without the launcher's keyboard-and-mouse flags (Play.bat, a
// build started from Visual Studio): a controller's X and Y then did nothing in fights.
//
// Now the launcher writes two configs: the game's own (settings shared by both modes) and,
// with keyboard and mouse ticked, a second Coalesced.ini in game\BourneGame\KeyboardMouse with
// the keyboard-and-mouse changes on top, padded to the same size as the game's own since
// Xbox360TOC.txt lists one size. This file mounts that folder as its own read-only device and,
// only with --mnk_mode=true, links the game's BourneGame\Config\Xenon\Cooked folder (which
// holds nothing else the game reads) to it. The link is on the folder, not the file: the
// runtime opens a file by resolving its folder through the links and then looking the name up
// in that folder. Links are resolved repeatedly, so a path through game:\ or d:\ first becomes
// \Device\Harddisk0\Partition1\BourneGame\..., which the link below then matches.

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
constexpr const char* kCookedFolder = "\\Device\\Harddisk0\\Partition1\\BourneGame\\Config\\Xenon\\Cooked";
constexpr const char* kFile = "Coalesced.ini";
}  // namespace

void Mount(rex::Runtime* runtime, const std::filesystem::path& game_data_root) {
  if (!runtime || !runtime->file_system() || game_data_root.empty()) return;
  if (!rex::cvar::Query<bool>("mnk_mode")) return;

  const std::filesystem::path folder = std::filesystem::absolute(game_data_root / kFolder);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(folder / kFile, ec)) {
    REXLOG_WARN(
        "Keyboard and mouse: no {} in {}; the game's own config is used (start the game from "
        "the launcher with Keyboard and mouse ticked to write it)",
        kFile, folder.string());
    return;
  }

  auto* fs = runtime->file_system();
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kDevice, folder, true);
  if (!device->Initialize() || !fs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("Keyboard and mouse: could not mount {}; the game's own config is used",
                folder.string());
    return;
  }
  fs->RegisterSymbolicLink(kCookedFolder, kDevice);
  REXLOG_INFO("Keyboard and mouse: game config from {}", (folder / kFile).string());
}
}  // namespace tbc::kbm_config
