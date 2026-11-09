// tbc - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>

// --show_fps=true draws a small frame rate counter in the top-left corner.
// (F3 still opens the SDK's full debug overlay.)
REXCVAR_DEFINE_BOOL(show_fps, false, "UI", "Show a small frame rate counter in the corner");

class FpsCounterDialog : public rex::ui::ImGuiDialog {
 public:
  explicit FpsCounterDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    // Prefer the game's own frame time (perf counters), fall back to the
    // presenter's frame rate if the runtime was built without counters.
    double fps = 0;
    int64_t us = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
    if (us > 0) {
      fps = 1e6 / double(us);
    } else {
      int64_t f = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFps);
      fps = f > 0 ? double(f) : double(io.Framerate);
    }
    // Smooth, and refresh the shown number a few times a second so it's readable.
    avg_ = avg_ <= 0 ? fps : avg_ * 0.9 + fps * 0.1;
    since_update_ += io.DeltaTime;
    if (since_update_ >= 0.25f || shown_ <= 0) {
      shown_ = avg_;
      since_update_ = 0;
    }

    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##tbc_fps", nullptr, flags)) {
      ImGui::Text("%.0f FPS", std::max(0.0, shown_));
    }
    ImGui::End();
  }

 private:
  double avg_ = 0;
  double shown_ = 0;
  float since_update_ = 0;
};

class TbcApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TbcApp>(new TbcApp(ctx, "tbc",
        PPCImageConfig));
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (REXCVAR_GET(show_fps)) {
      fps_counter_ = std::make_unique<FpsCounterDialog>(drawer);
    }
  }

  void OnShutdown() override { fps_counter_.reset(); }

  // Other hooks available for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnConfigurePaths(rex::PathConfig& paths) override {}

 private:
  std::unique_ptr<FpsCounterDialog> fps_counter_;
};
