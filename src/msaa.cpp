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
// Without tiling the engine doesn't clear the scene's depth: D3DDevice_BeginTiling did that
// (ClearZ, stencil 0) and the non-tiled path only sets the render targets again. Left alone,
// depth from the half-resolution effects drawn earlier into the same EDRAM fails the scene's
// depth test in the top-left quarter, which then shows that half-resolution image (seen with
// both renderers). So with --msaa=false the depth and stencil are cleared there, as
// BeginTiling would: D3DDevice_Clear (sub_822FDCD0, called by RHIClear sub_827B4EA0) right
// after the scene renderer's second SetRenderTargets call (sub_827C4640, returning to
// 0x82E58360, taken only when tiling is off). ClearZ is BeginTiling's: 0 when the depth is
// inverted (the global at 0x8365CE38), else 1.
//
// The switch is the global at 0x8365CE40 ("use tiling"). sub_827B5908 sets it at start-up
// (from the RHI's initialisation, sub_8231DDF8): 1 when the display is at least 1280 wide.
// The scene renderer (sub_82E58198) reads it every frame to choose between BeginTiling with
// the two strips (sub_827BFA08) and an ordinary render target. The same start-up function
// also stores 2 at 0x8365CE6C (the sample count, by its use) and 864 at 0x834244EC (the
// shadow map size seen in --resolve_trace).

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(msaa, false, "GPU",
                    "The 3D scene's 2x anti-aliasing (drawn in two strips on the console); "
                    "false (default) draws it in one pass at one sample per pixel: faster, and "
                    "the only way the native renderer can draw it");

REXCVAR_DEFINE_BOOL(msaa_inner_as_tiled, false, "GPU",
                    "With --msaa=false: let the scene's inner pass (sub_82E537D0) still see "
                    "tiling on; experiment for the half-resolution box in the top-left corner");

REXCVAR_DEFINE_INT32(msaa_off_color_base, 0, "GPU",
                     "With --msaa=false: EDRAM tile where the scene colour (DefaultColor) is placed; "
                     "0 (default) keeps the game's own, 720");

REXCVAR_DEFINE_INT32(msaa_off_mask_depth_base, -1, "GPU",
                     "With --msaa=false: EDRAM tile for the half-resolution shadow-mask depth "
                     "(ShadowMaskDepthZ); the game puts it at 0, on top of the scene depth. "
                     "-1 (default): right after the shadow mask's colour (0x388 at 1280 wide); "
                     "0 keeps the game's own");

REXCVAR_DEFINE_BOOL(msaa_edram_log, false, "GPU",
                    "Log every EDRAM placement the engine looks up (sub_827B5B48)");

REXCVAR_DEFINE_BOOL(msaa_off_restore_depth, false, "GPU",
                    "With --msaa=false: copy the full scene depth back into the scene depth "
                    "texture before the scene (the shadow mask leaves half-resolution depth in "
                    "it, seen as stripes)");

REXCVAR_DEFINE_BOOL(msaa_off_clear_depth, false, "GPU",
                    "With --msaa=false: clear depth and stencil when the scene starts, as "
                    "BeginTiling does (off: the scene's lighting tests against the depth "
                    "pre-pass, so clearing it left Xenos dark)");

REX_EXTERN(sub_822FDCD0);  // D3DDevice_Clear
REX_EXTERN(sub_827C4D68);  // RHICopyToResolveTarget (hooked below)
REX_EXTERN(__imp__sub_827C4D68);

REXCVAR_DEFINE_BOOL(msaa_off_skip_mask_depth_resolve, true, "GPU",
                    "With --msaa=false: skip the scene renderer's second scene depth resolve, which "
                    "copies the shadow mask's half-resolution depth instead (seen as stripes)");
REX_EXTERN(__imp__sub_827B5908);
REX_EXTERN(__imp__sub_827B5B48);
REX_EXTERN(__imp__sub_827C4640);
REX_EXTERN(__imp__sub_82E537D0);

namespace {
constexpr uint32_t kUseTiling = 0x8365CE40;
constexpr uint32_t kInvertedDepth = 0x8365CE38;
constexpr uint32_t kDevice = 0x8359F774;          // the D3D device the RHI uses
constexpr uint32_t kSceneDepthSurface = 0x83661E18;  // scene render targets + 0xD8
constexpr uint32_t kNonTiledReturn = 0x82E58360;  // after the scene's non-tiled SetRenderTargets

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 |
         uint32_t(base[addr + 2]) << 8 | base[addr + 3];
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  base[addr] = uint8_t(value >> 24);
  base[addr + 1] = uint8_t(value >> 16);
  base[addr + 2] = uint8_t(value >> 8);
  base[addr + 3] = uint8_t(value);
}
// A UTF-16 big-endian guest string, narrowed (the engine's names are ASCII).
std::string ReadWide(const uint8_t* base, uint32_t addr) {
  std::string out;
  for (int i = 0; i < 64; ++i, addr += 2) {
    const uint16_t c = uint16_t(base[addr]) << 8 | base[addr + 1];
    if (!c) break;
    out.push_back(c < 0x80 ? char(c) : '?');
  }
  return out;
}
}  // namespace

extern "C" REX_FUNC(sub_827B5908) {
  __imp__sub_827B5908(ctx, base);
  if (!REXCVAR_GET(msaa)) {
    Store32(base, kUseTiling, 0);
    REXLOG_INFO("msaa: off (no predicated tiling)");
  }
}

// The scene's render targets are set; on the non-tiled path, clear depth and stencil as
// BeginTiling does on the tiled one.
extern "C" REX_FUNC(sub_827C4640) {
  const uint32_t return_to = static_cast<uint32_t>(ctx.lr);
  __imp__sub_827C4640(ctx, base);
  if (REXCVAR_GET(msaa) || return_to != kNonTiledReturn) {
    return;
  }
  if (REXCVAR_GET(msaa_off_restore_depth)) {
    // RHICopyToResolveTarget(scene depth, keep, {face 0, whole surface}), exactly as the depth
    // pre-pass does at 0x82E55F44: puts the full scene depth back in the scene depth texture
    // after the shadow mask has used it for its half-resolution depth.
    PPCContext resolve = ctx;
    const uint32_t params = ctx.r1.u32 - 0x100;
    Store32(base, params, 0);  // cube face
    for (uint32_t i = 1; i < 5; ++i) Store32(base, params + 4 * i, 0xFFFFFFFF);  // whole rect
    resolve.r1.u32 = ctx.r1.u32 - 0x200;
    resolve.r3.u64 = kSceneDepthSurface;
    resolve.r4.u64 = 1;
    resolve.r5.u64 = params;
    sub_827C4D68(resolve, base);
  }
  if (!REXCVAR_GET(msaa_off_clear_depth)) {
    return;
  }
  PPCContext call = ctx;
  call.r1.u32 = ctx.r1.u32 - 0x200;
  call.r3.u64 = Load32(base, kDevice);
  call.r4.u64 = 0;     // Count
  call.r5.u64 = 0;     // pRects
  call.r6.u64 = 0x30;  // D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL
  call.r7.u64 = 0;     // Color (not cleared)
  call.f1.f64 = Load32(base, kInvertedDepth) ? 0.0 : 1.0;  // Z
  call.r9.u64 = 0;     // Stencil
  call.r10.u64 = 0;
  sub_822FDCD0(call, base);
}

// Experiment (--msaa_inner_as_tiled): with tiling off, a quarter-size copy of the frame shows
// in the top-left corner with both renderers. The scene renderer (sub_82E58198) calls an inner
// pass (sub_82E537D0) that reads the tiling flag twice to choose its depth pre-pass setup; the
// half-resolution depth resolve that lands in the scene depth texture (640x720 into
// 1A248000) only appears with tiling off. This lets that inner pass see tiling on while the
// outer renderer, which would start predicated tiling, still sees it off.
extern "C" REX_FUNC(sub_82E537D0) {
  if (REXCVAR_GET(msaa) || !REXCVAR_GET(msaa_inner_as_tiled)) {
    __imp__sub_82E537D0(ctx, base);
    return;
  }
  const uint32_t saved = Load32(base, kUseTiling);
  Store32(base, kUseTiling, 1);
  __imp__sub_82E537D0(ctx, base);
  Store32(base, kUseTiling, saved);
}

// The engine's EDRAM placement table (sub_827B5B48: name, size in tiles -> base tile), used
// when its render targets are created. Depth starts at tile 0 and the scene colour
// ("DefaultColor", "DefaultColorRaw", and the back buffer "DefaultBB") sits right after a
// 1280x720 depth, tile 720 (0x2D0). With
// tiling the console's runtime puts the strip's colour at 0x400 instead, so the
// half-resolution effects can use 720 as scratch: "AuxDepth" is not in the table either and
// lands on 720 too. Without tiling the half-resolution depth pre-pass draws straight over the
// finished scene colour, and the distortion pass that reads it back shows a quarter of the
// screen of depth bits (the dark red box in the top-left). So with --msaa=false the scene
// colour goes to 0x400 as with tiling: 0x400 + 720 tiles still fits in the 2048.
extern "C" REX_FUNC(sub_827B5B48) {
  const uint32_t name_addr = ctx.r3.u32;
  const uint32_t tiles = ctx.r4.u32;
  __imp__sub_827B5B48(ctx, base);
  const bool log = REXCVAR_GET(msaa_edram_log);
  const bool off = !REXCVAR_GET(msaa);
  const uint32_t width = Load32(base, 0x8342402C);
  const bool wide = width > 1280;  // --aspect_ratio (ultrawide.cpp)
  if (!log && !off && !wide) return;
  const std::string name = ReadWide(base, name_addr);
  const uint32_t game = ctx.r3.u32;
  // The game's layout is S = one full-width 720-line surface's tiles (W/80 x 45): scene depth at
  // 0, scene colour (and the shadow mask) at S, then the 864x864 shadow-map depth and the other
  // effect targets at 2S (ShadowDepthRT after the shadow map, 594 tiles further). At 1280,
  // S = 720 and the 2S group fits in the 608 tiles left of the 2048. Wider, it doesn't (2S =
  // 1890 at 21:9): a surface only reaches to the end of EDRAM, so the shadow map was cut to its
  // top 224 lines and whole areas fell into shadow. With ultrawide the 2S group goes back to
  // where it is at 1280 (1440), and at 32:9 the colour moves down so its 720 lines fit
  // (2048 - S). Overlaps don't matter to the native renderer, which keeps each surface on its
  // own; on Xenos ultrawide doesn't fit EDRAM anyway.
  const uint32_t scene_tiles = (width + 79) / 80 * 45;
  const uint32_t colour_base = wide ? std::min(scene_tiles, 2048 - scene_tiles) : scene_tiles;  // S <= 1800
  if (wide) {
    if (game == scene_tiles) {
      ctx.r3.u64 = colour_base;
    } else if (game >= 2 * scene_tiles && 2 * scene_tiles + 608 > 2048) {
      ctx.r3.u64 = 1440 + (game - 2 * scene_tiles);
    }
  }
  const int32_t moved = REXCVAR_GET(msaa_off_color_base);
  if (off && moved > 0 && (name == "DefaultColor" || name == "DefaultColorRaw" || name == "DefaultBB") && uint32_t(moved) + tiles <= 2048) {
    ctx.r3.u64 = uint32_t(moved);
  }
  int32_t mask_depth = REXCVAR_GET(msaa_off_mask_depth_base);
  if (mask_depth < 0) {
    // After the shadow mask's colour, which sits after a full-width 720-line depth: the scene
    // depth's tiles (W x 720 x 4 bytes / 5120), then the half-width mask (80-pixel tiles across,
    // 23 tile rows for its 368 lines). 0x2D0 + 8 x 23 = 0x388 at 1280; wider with ultrawide.
    mask_depth = int32_t(colour_base + ((width / 2 + 79) / 80) * 23);
  }
  if (off && mask_depth > 0 && name == "ShadowMaskDepthZ" && uint32_t(mask_depth) + tiles <= 2048) {
    ctx.r3.u64 = uint32_t(mask_depth);
  }
  if (log || ctx.r3.u32 != game) {
    REXLOG_INFO("msaa: EDRAM {} ({} tiles) at {:#x}{}", name, tiles, ctx.r3.u32,
                ctx.r3.u32 != game ? " (moved from the game's)" : "");
  }
}

// Experiment (--msaa_skip_small_depth): the scene's depth pre-pass (sub_82E55C28, called by
// sub_82E58198 before tiling would start) resolves the scene depth and then makes the
// half-resolution copy of it (sub_82A3F460, the DownsampleDepthBuffer shaders). A frame dump
// with --msaa=false shows that copy landing in the top-left quarter of the scene depth
// texture and the scene's lighting dark there afterwards. With tiling the strips redraw the
// depth from scratch after it; without, the damage stays. This skips the half-resolution copy
// (confirmed: no box). Effects that read it, such as soft particles, may look off; set it
// to false to bring the copy (and the box) back.
REXCVAR_DEFINE_BOOL(msaa_skip_small_depth, false, "GPU",
                    "With --msaa=false: skip the half-resolution depth copy, which otherwise "
                    "leaves a dark box in the top-left corner");

REX_EXTERN(__imp__sub_82A3F460);

extern "C" REX_FUNC(sub_82A3F460) {
  if (!REXCVAR_GET(msaa) && REXCVAR_GET(msaa_skip_small_depth)) {
    return;
  }
  __imp__sub_82A3F460(ctx, base);
}

// What the box really is (a native-renderer draw trace and resolve dump, --msaa=false):
// before the scene, in its depth pre-pass, the engine builds the half-resolution shadow mask.
// It draws a half-resolution depth ("ShadowMaskDepthZ", 640 wide) from the scene depth
// texture, then the shadow volumes into its stencil, and resolves it. The table puts that
// depth at EDRAM tile 0: the same place as the scene depth ("DefaultDepth"). With tiling,
// BeginTiling clears the depth and each strip redraws it, so nothing is lost. Without tiling
// the scene is drawn against the depth left in EDRAM, whose top-left quarter is now the
// half-resolution shadow-mask depth: the lighting fails there (the dark box with a half-size
// silhouette) and the black area below it gives the stripes. --msaa_off_mask_depth_base puts
// ShadowMaskDepthZ at 0x388 instead: after the shadow mask's colour (ShadowMask, 0x2D0 + 184
// tiles) and before the shadow depths (0x5A0), which are all in use at that point.

// The stripes (--msaa_off_restore_depth): the shadow mask's half-resolution depth is also
// resolved into the scene depth texture (resolve 640x720 from ShadowMaskDepthZ into
// 1A248000, right after the shadow volumes). With tiling the strips resolve the scene depth
// again before anything reads it; without, the scene's lighting reads the texture as it is.
// So on the non-tiled path the scene depth is resolved again just before the scene draws
// (from the scene depth still in EDRAM, which the shadow mask no longer overwrites).

// The stripes, fixed (--msaa_off_skip_mask_depth_resolve). Logging every
// RHICopyToResolveTarget (sub_827C4D68) shows the scene depth (surface 0x83661E18) resolved
// by the depth pre-pass (returning to 0x82E55F48) and again by the scene renderer
// (0x82E58300), just before it sets the scene's render targets. In between the shadow mask
// has drawn with its own 640-wide depth, and that is still the surface the GPU has set up,
// so the second resolve copies the shadow mask's depth into the scene depth texture: the
// native renderer clamps it to 640x720 (the stripes, left half) and Xenos rejects it every
// frame ("Resolve region 0 <= x < 1280 is outside the surface pitch 640"). Nothing has
// changed the scene depth since the first resolve, so with --msaa=false the second one is
// skipped.
namespace {
constexpr uint32_t kSecondDepthResolve = 0x82E58300;  // in sub_82E58198, before SetRenderTargets
bool g_logged_mask_skip = false;
}  // namespace

extern "C" REX_FUNC(sub_827C4D68) {
  static int logged = 0;
  if (REXCVAR_GET(msaa_edram_log) && logged < 200) {
    ++logged;
    REXLOG_INFO("msaa: resolve surface {:#x} from {:#x}", ctx.r3.u32,
                static_cast<uint32_t>(ctx.lr));
  }
  if (!REXCVAR_GET(msaa) && REXCVAR_GET(msaa_off_skip_mask_depth_resolve) &&
      ctx.r3.u32 == kSceneDepthSurface && static_cast<uint32_t>(ctx.lr) == kSecondDepthResolve) {
    if (!g_logged_mask_skip) {
      g_logged_mask_skip = true;
      REXLOG_INFO("msaa: skipping the scene depth's second resolve (from {:#x})",
                  static_cast<uint32_t>(ctx.lr));
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_827C4D68(ctx, base);
}
