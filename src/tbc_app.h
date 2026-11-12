// tbc - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <memory>

#include <rex/cvar.h>
#include <rex/rex_app.h>

#include "crash_win.h"
#include "debug_tools.h"
#include "display_refresh.h"
#include "fps_overlay.h"

// --show_fps=true shows the game's frame rate in a small window at the top-left corner.
// It's a separate native window, not an ImGui overlay: the SDK's ImGui overlays cost
// about 20% of the frame rate (see fps_overlay.cpp). F3 still opens the SDK's full
// debug overlay.
REXCVAR_DEFINE_BOOL(show_fps, false, "UI", "Show a small frame rate counter in the corner");

class TbcApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TbcApp>(new TbcApp(ctx, "tbc",
        PPCImageConfig));
  }

  void OnPostInitLogging() override { tbc::crash::Install(); }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The runtime loads no graphics plugin unless --gpu_plugin names one, and then the game
    // runs with a black window. gpu_preload.cpp already loads xenos by default; match it.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // --match_display_refresh: the guest vblank follows the monitor (display_refresh.cpp).
    tbc::display::MatchDisplayRefreshRate();
  }

  void OnPostSetup() override {
    if (REXCVAR_GET(show_fps)) {
      tbc::overlay::StartFpsOverlay();
    }
    // --dev_debug_runtime: log and stub code addresses with no function (debug_tools.h).
    tbc::debug::RunIfEnabled(PPCImageConfig.code_base, PPCImageConfig.code_size);
  }

  void OnShutdown() override { tbc::overlay::StopFpsOverlay(); }

  // Other hooks available for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
