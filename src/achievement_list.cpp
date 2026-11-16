// The game's achievements for the launcher's Achievements window: once the title's metadata has
// loaded, <user data>/achievements/<title id>-list.json (id, name, descriptions, gamerscore,
// icon file) and each icon as <user data>/achievements/icons/<image id>.png, the PNG from the
// title's XDBF. Which ones are unlocked, and when, the runtime already keeps beside them in
// <title id>.toml (AchievementManager::SaveUnlockState). Written on every start, so a list
// survives a reinstall; the icons only once.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

namespace tbc::achievements {
namespace {

std::string Json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);  // UTF-8 passes through
        }
    }
  }
  return out + "\"";
}

}  // namespace

void ExportList(rex::Runtime* runtime) {
  if (!runtime) return;
  std::thread([runtime] {
    // The achievements are known once the game's module has loaded.
    for (int tries = 0; tries < 120; ++tries) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto* kernel = runtime->kernel_state();
      if (!kernel) continue;
      const auto list = kernel->achievements().ListAchievements();
      if (list.empty()) continue;
      const auto dir = runtime->user_data_root() / "achievements";
      std::error_code ec;
      std::filesystem::create_directories(dir / "icons", ec);
      const auto xdbf = kernel->title_xdbf();
      char title[16];
      std::snprintf(title, sizeof(title), "%08X", kernel->title_id());
      std::string json = "{\n  \"title_id\": \"" + std::string(title) + "\",\n  \"achievements\": [\n";
      for (size_t i = 0; i < list.size(); ++i) {
        const auto& a = list[i];
        std::string icon;
        if (a.image_id && xdbf.is_valid()) {
          const auto name = std::to_string(a.image_id) + ".png";
          icon = "icons/" + name;
          const auto path = dir / "icons" / name;
          if (!std::filesystem::exists(path, ec)) {
            const auto block = xdbf.GetEntry(rex::system::util::XdbfSection::kImage, a.image_id);
            if (block) {
              std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(block.buffer),
                                                          std::streamsize(block.size));
            } else {
              icon.clear();
            }
          }
        }
        json += "    {\"id\": " + std::to_string(a.id) + ", \"name\": " + Json(a.label) +
                ", \"description\": " + Json(a.description) + ", \"locked_description\": " +
                Json(a.unachieved_description) + ", \"gamerscore\": " + std::to_string(a.gamerscore) +
                ", \"icon\": " + Json(icon) + "}" + (i + 1 < list.size() ? ",\n" : "\n");
      }
      json += "  ]\n}\n";
      const auto path = dir / (std::string(title) + "-list.json");
      std::ofstream(path, std::ios::binary) << json;
      REXLOG_INFO("achievements: {} listed for the launcher in {}", list.size(), path.string());
      return;
    }
  }).detach();
}

}  // namespace tbc::achievements
