// tbc - ultrawide (--aspect_ratio).
//
// The engine draws a 720-line picture whose width comes from the console's display aspect:
// sub_827BFB48 (device setup) asks for the video mode, takes 720 x aspect, clamps it to
// 900..1280 (4:3 to 16:9) and stores it at 0x8342402C, from which the back buffer, the scene's
// render targets and their EDRAM placement (sub_827B5B48) are sized. A mid-function hook just
// after the clamp (hooks.toml, 0x827BFC84, r31 = the width) replaces it with 720 x
// --aspect_ratio, rounded to the EDRAM's 80-pixel tiles: 1680 for 21:9, 2560 for 32:9.
//
// Experimental, and for the native renderer: a wider picture needs more of the Xbox 360's 10 MB
// EDRAM than the console layout plans for (the shadow-map depth lands past the end of it at
// 21:9), which the native renderer doesn't mind and Xenos does. Videos stay 16:9.
//
// Menus (--ultrawide_ui_16_9): the game's UI is Unreal Engine 3 UI scenes, laid out inside the
// rectangle the scene client reports: UGameUISceneClient::GetViewportOrigin (vtable +312,
// sub_82E2CB68) and GetViewportSize (vtable +316, sub_82E2CC30), both (this, scene, out
// FVector2D*). With a picture wider than 16:9, these report a 16:9 rectangle in the middle, so
// the scenes are laid out and drawn there instead of being spread across the whole width.

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

REX_EXTERN(__imp__sub_82E2CB68);  // UGameUISceneClient::GetViewportOrigin
REX_EXTERN(__imp__sub_82E2CC30);  // UGameUISceneClient::GetViewportSize

REXCVAR_DEFINE_DOUBLE(aspect_ratio, 0.0, "GPU",
                      "Picture aspect ratio: 0 (default) keeps the original 16:9; 2.3333 is 21:9, "
                      "3.5556 is 32:9 (experimental, native renderer)");

void TbcRenderWidth(PPCRegister& r31) {
  const double aspect = REXCVAR_GET(aspect_ratio);
  if (aspect <= 0.0) return;
  const double tiles = std::round(720.0 * aspect / 80.0);
  const uint32_t width = uint32_t(std::clamp(tiles, 12.0, 40.0)) * 80;  // 960..3200
  REXLOG_INFO("ultrawide: render width {} (aspect {:.4f}; the game chose {})", width, aspect, r31.u32);
  r31.u64 = width;
}

REXCVAR_DEFINE_BOOL(ultrawide_ui_16_9, true, "GPU",
                    "With a wide --aspect_ratio, keep the menus (UI scenes) in a centred 16:9 "
                    "area; false stretches their layout across the whole width");

namespace {
float LoadFloat(const uint8_t* base, uint32_t addr) {
  const uint32_t v = uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 |
                     uint32_t(base[addr + 2]) << 8 | base[addr + 3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreFloat(uint8_t* base, uint32_t addr, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  base[addr] = uint8_t(v >> 24);
  base[addr + 1] = uint8_t(v >> 16);
  base[addr + 2] = uint8_t(v >> 8);
  base[addr + 3] = uint8_t(v);
}
constexpr float k16x9 = 16.0f / 9.0f;
// The full viewport the last GetViewportSize call saw, for the origin.
float g_view_w = 0.0f, g_view_h = 0.0f;
bool UiPillarbox() { return REXCVAR_GET(aspect_ratio) > 0.0 && REXCVAR_GET(ultrawide_ui_16_9); }
}  // namespace

extern "C" REX_FUNC(sub_82E2CC30) {
  const uint32_t out = ctx.r5.u32;
  __imp__sub_82E2CC30(ctx, base);
  if (!UiPillarbox() || !out) return;
  const float w = LoadFloat(base, out), h = LoadFloat(base, out + 4);
  if (h <= 0.0f || w <= h * k16x9 + 1.0f) return;
  if (w != g_view_w || h != g_view_h) {
    REXLOG_INFO("ultrawide: menus laid out in {:.0f}x{:.0f} in the middle of {:.0f}x{:.0f}",
                h * k16x9, h, w, h);
  }
  g_view_w = w;
  g_view_h = h;
  StoreFloat(base, out, h * k16x9);
}

extern "C" REX_FUNC(sub_82E2CB68) {
  const uint32_t out = ctx.r5.u32;
  __imp__sub_82E2CB68(ctx, base);
  if (!UiPillarbox() || !out || g_view_h <= 0.0f) return;
  const float pad = (g_view_w - g_view_h * k16x9) * 0.5f;
  if (pad <= 0.5f) return;
  StoreFloat(base, out, LoadFloat(base, out) + std::floor(pad));
}
