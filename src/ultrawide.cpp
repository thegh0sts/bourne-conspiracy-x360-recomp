// tbc - ultrawide (--aspect_ratio).
//
// The engine draws a 720-line picture whose width comes from the console's display aspect:
// sub_827BFB48 (device setup) asks for the video mode, takes 720 x aspect, clamps it to
// 900..1280 (4:3 to 16:9) and stores it at 0x8342402C, from which the back buffer, the scene's
// render targets and their EDRAM placement (sub_827B5B48) are sized. A mid-function hook just
// after the clamp (hooks.toml, 0x827BFC84, r31 = the width) replaces it with 720 x
// --aspect_ratio, rounded to 8 pixels: 1680 for 21:9 (2.3333), 1720 for 3440x1440 (2.3889),
// 1728 for 3840x1600 (2.4), 2560 for 32:9. The engine's EDRAM layout rounds widths up to its
// 80-pixel tiles itself (sub_827B5B48).
//
// Experimental, and for the native renderer: a wider picture needs more of the Xbox 360's 10 MB
// EDRAM than the console layout plans for (the shadow-map depth lands past the end of it at
// 21:9), which the native renderer doesn't mind and Xenos does. The HUD and menus are laid out
// for 16:9, and videos stay 16:9.

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(aspect_ratio, 0.0, "GPU",
                      "Picture aspect ratio: 0 (default) keeps the original 16:9; 2.3333 is 21:9, "
                      "3.5556 is 32:9 (experimental, native renderer)");

void TbcRenderWidth(PPCRegister& r31) {
  const double aspect = REXCVAR_GET(aspect_ratio);
  if (aspect <= 0.0) return;
  const double eighths = std::round(720.0 * aspect / 8.0);
  const uint32_t width = uint32_t(std::clamp(eighths, 120.0, 400.0)) * 8;  // 960..3200
  REXLOG_INFO("ultrawide: render width {} (aspect {:.4f}; the game chose {})", width, aspect, r31.u32);
  r31.u64 = width;
}
