// The game's achievements for the launcher's Achievements window: once the title's metadata has
// loaded, <user data>/achievements/<title id>-list.json (id, name, descriptions, gamerscore,
// icon file, plus names/descriptions/locked_descriptions in every language the launcher offers,
// keyed by XLanguage id) and each icon as <user data>/achievements/icons/<image id>.png, the PNG from the
// title's XDBF. Which ones are unlocked, and when, the runtime already keeps beside them in
// <title id>.toml (AchievementManager::SaveUnlockState). Written on every start, so a list
// survives a reinstall; the icons only once.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <thread>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>

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

// The launcher's languages (XLanguage ids), so its Achievements tab can show the text in the
// language picked there without the game being run in it first.
constexpr rex::system::XLanguage kLanguages[] = {
    rex::system::XLanguage::kEnglish, rex::system::XLanguage::kGerman,
    rex::system::XLanguage::kFrench, rex::system::XLanguage::kSpanish,
    rex::system::XLanguage::kItalian};

// {"1": "...", "3": "...", ...} for one string id, leaving out languages the XDBF lacks.
std::string Translations(const rex::system::util::XdbfWrapper& xdbf, uint16_t string_id) {
  std::string out = "{";
  for (auto lang : kLanguages) {
    const auto text = xdbf.GetStringTableEntry(lang, string_id);
    if (text.empty()) continue;
    if (out.size() > 1) out += ", ";
    out += "\"" + std::to_string(uint32_t(lang)) + "\": " + Json(text);
  }
  return out + "}";
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
      // Every language's strings, by achievement id, straight from the title's XDBF.
      std::map<uint32_t, rex::system::util::XdbfAchievementTableEntry> entries;
      if (xdbf.is_valid()) {
        for (const auto& e : xdbf.GetAchievements()) entries[uint32_t(e.id)] = e;
      }
      for (size_t i = 0; i < list.size(); ++i) {
        const auto& a = list[i];
        std::string names = "{}", descriptions = "{}", locked = "{}";
        if (auto it = entries.find(a.id); it != entries.end()) {
          names = Translations(xdbf, it->second.label_id);
          descriptions = Translations(xdbf, it->second.description_id);
          locked = Translations(xdbf, it->second.unachieved_id);
        }
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
                ", \"icon\": " + Json(icon) +
                ", \"names\": " + names + ", \"descriptions\": " + descriptions +
                ", \"locked_descriptions\": " + locked + "}" + (i + 1 < list.size() ? ",\n" : "\n");
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
