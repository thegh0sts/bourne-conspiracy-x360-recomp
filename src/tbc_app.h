// tbc - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <system_error>
#include <thread>

#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/system/achievement_manager.h>
#include <rex/ui/overlay/achievement_notification.h>

#include "crash_win.h"
#include "debug_tools.h"
#include "display_refresh.h"
#include "fps_overlay.h"

namespace tbc {
// The log's first lines: the release, the build time and the launch options (version_log.cpp).
void LogVersion();
}  // namespace tbc

// --show_fps=true shows the game's frame rate at the top-left corner. The native renderer
// draws it into the picture itself (native-plugin/patches/0006-fps-counter.patch), so it also
// shows in fullscreen; with Xenos it's a small separate window (fps_overlay.cpp). Neither is
// an ImGui overlay: the SDK's ImGui overlays cost about 20% of the frame rate. F3 still opens
// the SDK's full debug overlay.
REXCVAR_DEFINE_BOOL(show_fps, false, "UI", "Show a small frame rate counter in the corner");
// --achievement_popups=false: no pop-up when an achievement unlocks. It's still unlocked and
// listed in the achievements overlay (F7); only the toast (an ImGui dialog) isn't made.
REXCVAR_DEFINE_BOOL(achievement_popups, true, "UI", "Show a pop-up when an achievement unlocks");
// --test_achievement_popups=true: 10 s after start, show the pop-ups of the game's first two
// achievements, without unlocking them (AchievementManager::ShowAchievementNotification), to
// check the pop-up without playing for one.
REXCVAR_DEFINE_BOOL(test_achievement_popups, false, "UI",
                    "Show two achievement pop-ups 10 s after start (nothing is unlocked)");
// --show_fps_detail=true (with --show_fps): the native renderer's counter also shows the frame
// time and slowest frame, GPU time and load, CPU load (machine and game), RAM and VRAM. The
// renderer reads it through the shared cvar registry (native-plugin/patches/0006-fps-counter.patch).
REXCVAR_DEFINE_BOOL(show_fps_detail, false, "UI",
                    "With show_fps: also frame time, GPU, CPU and memory (native renderer)");

// --brightness=1.2: the native renderer brightens (above 1) or darkens the picture as a gamma
// curve on its last pass, black and white staying put (native-plugin/patches/0006). Xenos
// ignores it.
REXCVAR_DEFINE_DOUBLE(brightness, 1.0, "GPU", "Picture brightness, native renderer (1 = the game's)")
    .range(0.5, 2.0);

namespace tbc::achievements {
// <user data>/achievements/<title id>-list.json and the icons, for the launcher (achievement_list.cpp).
void ExportList(rex::Runtime* runtime);
}  // namespace tbc::achievements

class TbcApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TbcApp>(new TbcApp(ctx, "tbc",
        PPCImageConfig));
  }

  void OnPostInitLogging() override {
    tbc::crash::Install();
    tbc::LogVersion();
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The runtime loads no graphics plugin unless --gpu_plugin names one, and then the game
    // runs with a black window. Default to Skullburglar's native Vulkan renderer
    // (native-plugin/), as gpu_preload.cpp does.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "native";
    }
    native_renderer_ = config.gpu_plugin == "native";
    // --match_display_refresh: the guest vblank follows the monitor (display_refresh.cpp).
    tbc::display::MatchDisplayRefreshRate();
  }

  void OnPostSetup() override {
    if (REXCVAR_GET(show_fps) && !native_renderer_) {  // the native renderer draws its own
      tbc::overlay::StartFpsOverlay();
    }
    // --dev_debug_runtime: log and stub code addresses with no function (debug_tools.h).
    tbc::debug::RunIfEnabled(PPCImageConfig.code_base, PPCImageConfig.code_size);
    tbc::achievements::ExportList(runtime());
    if (REXCVAR_GET(test_achievement_popups)) {
      std::thread([this] {
        // The game's achievements are known once its module has loaded: wait for them.
        for (int tries = 0; tries < 60; ++tries) {
          std::this_thread::sleep_for(std::chrono::seconds(tries ? 1 : 10));
          const auto list = achievements().ListAchievements();
          if (list.empty()) continue;
          for (size_t i = 0; i < list.size() && i < 2; ++i) {
            achievements().ShowAchievementNotification(list[i].id);
          }
          return;
        }
      }).detach();
    }
  }

  void OnShutdown() override { tbc::overlay::StopFpsOverlay(); }

  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    // None (the runtime copes): with --achievement_popups=false, and with the native renderer,
    // which draws the pop-up itself in the same look (native-plugin/patches/0006-fps-counter.patch)
    // so no ImGui window makes the runtime repaint while it shows.
    if (!REXCVAR_GET(achievement_popups) || native_renderer_) return nullptr;
    return rex::ReXApp::CreateAchievementNotificationDialog();
  }

 private:
  bool native_renderer_ = true;

 public:

  // Other hooks available for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // Folders the game lists at startup. Some disc extractors drop empty folders (the disc's Engine
  // folder is empty), and the runtime then throws "Failed to enumerate directory: The system
  // cannot find the path specified" instead of telling the game the folder isn't there: an
  // unhandled exception about a second after the window opens (a player's 0.2.2 and 0.2.3
  // reports, minidump showing that message beside game\Engine and game\BourneGame). Creating
  // any that are missing is harmless and lets the game start.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.game_data_root.empty()) return;
    for (const char* dir : {"Engine", "Binaries", "BourneGame/CheckpointPresets", "BourneGame/Config",
                            "BourneGame/CookedXenon", "BourneGame/Localization", "BourneGame/Movies"}) {
      const std::filesystem::path path = paths.game_data_root / dir;
      std::error_code ec;
      if (!std::filesystem::exists(path, ec) && std::filesystem::create_directories(path, ec)) {
        REXLOG_WARN("Created missing game folder {}", path.string());
      }
    }
  }
};
