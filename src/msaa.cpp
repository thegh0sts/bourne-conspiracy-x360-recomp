// tbc - the 3D scene's 2x anti-aliasing (MSAA) and predicated tiling, on or off.
//
// On the Xbox 360 the game draws its 720p scene with 2x MSAA. HDR colour plus depth at two
// samples per pixel is about 14.7 MB, more than the GPU's 10 MB of EDRAM, so the frame is drawn
// in two strips (predicated tiling: D3DDevice_BeginTiling with tile rects 0-512 and 512-720)
// and each strip is resolved out separately. ReXGlue's own renderer emulates this correctly;
// Skullburglar's native renderer (native-plugin) doesn't, and with ROV every pixel shader does
// its blending and depth test once per sample, so MSAA also costs the default renderer time at
// high --resolution_scale.
//
// --msaa=false turns it off: one sample per pixel, the scene fits in EDRAM, one pass and one
// resolve. The engine has a developer console command for this (TILING, sub_827C8AF0), so it
// supports running without it.
//
// The switch is the global at 0x8365CE40 ("use tiling"). sub_827B5908 sets it at start-up
// (from the RHI's initialisation, sub_8231DDF8): 1 when the display is at least 1280 wide.
// The scene renderer (sub_82E58198) reads it every frame to choose between BeginTiling with
// the two strips (sub_827BFA08) and an ordinary render target. The same start-up function
// also stores 2 at 0x8365CE6C (the sample count, by its use) and 864 at 0x834244EC (the
// shadow map size seen in --resolve_trace).

#include "generated/default/tbc_pch.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(msaa, true, "GPU",
                    "The 3D scene's 2x anti-aliasing (drawn in two strips on the console); "
                    "false draws it in one pass at one sample per pixel");

REX_EXTERN(__imp__sub_827B5908);

namespace {
constexpr uint32_t kUseTiling = 0x8365CE40;

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  base[addr] = uint8_t(value >> 24);
  base[addr + 1] = uint8_t(value >> 16);
  base[addr + 2] = uint8_t(value >> 8);
  base[addr + 3] = uint8_t(value);
}
}  // namespace

extern "C" REX_FUNC(sub_827B5908) {
  __imp__sub_827B5908(ctx, base);
  if (!REXCVAR_GET(msaa)) {
    Store32(base, kUseTiling, 0);
    REXLOG_INFO("msaa: off (no predicated tiling)");
  }
}
