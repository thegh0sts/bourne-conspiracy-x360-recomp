// tbc - key glyphs: the player's keys drawn as images in button prompts (keyboard and mouse).
//
// The button prompts are characters of BourneUIPlatformSpecific.ControllerButtonFont
// (keyboard_prompts.cpp). Found with --glyph_probe (glyph_probe.cpp):
//   UFont +0x3C/+0x40   the glyph table (TArray: data, count; 256 entries, plain ASCII, no
//                       remap: +140 = 0): 24 bytes each, StartU, StartV, USize, VSize (texels),
//                       TextureIndex, and +20 (copied from 'A' for ours)
//   UFont +0x48         the texture pages (TArray of UTexture2D; one, 512x512 DXT5)
//   UTexture2D +0xB8    its render resource (FTexture2DResource); resource +0x14 -> the RHI
//                       texture, +0x08 of that -> the GPU texture header, whose fetch constant
//                       is at +0x1C: dword0 (bit 31 tiled, bits 22..30 pitch / 32), dword1
//                       (bits 0..5 format, 0x14 = DXT4/5; bits 6..7 endianness; base address in
//                       bits 12..31), dword2 (width - 1 in bits 0..12, height - 1 in 13..25)
// The table's glyphs end at row 360; rows 368 to 512 are free. So, the first time the font is
// drawn, each assets/keys PNG (installed beside the game as keys/*.png) is scaled to a 40 px
// cell (80 px wide when the image is wide), encoded as DXT5, written into that free space in
// the page's own tiled layout, and given a glyph table entry for an unused character (from 0x80
// up). A prompt for a key with a glyph then draws that one character in the button font, which
// sizes, places and colours it exactly like the Xbox button it replaces. Keys without a glyph
// keep the text label.
//
// File names are key names as in the binds (W.png, Space.png, LMB.png), case-insensitive, and
// may have a "keyboard_" prefix and an "_icon" suffix (keyboard_backspace_icon.png);
// mouse_left / mouse_right / mouse_middle name the mouse buttons, enter / esc / ctrl the keys
// the binds call Return / Escape / Control.
//
//   --key_glyphs=false        text labels only
//   --key_glyphs_dump=true    write the font page before and after (logs/font_page_*.tga)

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/image_decode.h>

REX_EXTERN(sub_82920EE8);  // UObject::StaticFindObject(UClass*, UObject* outer, const TCHAR*, UBOOL exact)

REXCVAR_DEFINE_BOOL(key_glyphs, true, "Input",
                    "Keyboard and mouse: draw keys that have a glyph (keys/*.png) as images in "
                    "button prompts; others stay text");
REXCVAR_DEFINE_BOOL(key_glyphs_dump, false, "Debug",
                    "Write the button font's page before and after the key glyphs go in "
                    "(logs/font_page_before.tga, font_page_after.tga)");

namespace {

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 | uint32_t(base[addr + 2]) << 8 |
         base[addr + 3];
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  base[addr] = uint8_t(v >> 24);
  base[addr + 1] = uint8_t(v >> 16);
  base[addr + 2] = uint8_t(v >> 8);
  base[addr + 3] = uint8_t(v);
}

bool Readable(uint32_t addr) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return false;
  auto* heap = runtime->memory()->LookupHeap(addr);
  uint32_t protect = 0;
  return heap && heap->QueryProtect(addr, &protect) && (protect & rex::memory::kMemoryProtectRead);
}

// Xenos tiled 2D layout (as in Xenia's texture code): byte offset of block (x, y) in a texture
// `width` blocks wide (a multiple of 32), 2^log_bpp bytes a block.
uint32_t TiledOuter(uint32_t y, uint32_t width, uint32_t log_bpp) {
  const uint32_t macro = ((y / 32) * (width / 32)) << (log_bpp + 7);
  const uint32_t micro = ((y & 6) << 2) << log_bpp;
  return macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
}
uint32_t TiledInner(uint32_t x, uint32_t y, uint32_t log_bpp, uint32_t outer) {
  const uint32_t macro = (x / 32) << (log_bpp + 7);
  const uint32_t micro = (x & 7) << log_bpp;
  const uint32_t offset = outer + (macro + ((micro & ~15u) << 1) + (micro & 15));
  return ((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

struct Page {
  uint32_t data = 0;          // guest address of the pixels
  uint32_t blocks_wide = 0;   // pitch in 4x4 blocks
  uint32_t width = 0, height = 0;
  bool tiled = false;
  uint32_t endian = 0;        // 1: bytes swapped in 16-bit words
};

uint32_t BlockOffset(const Page& p, uint32_t bx, uint32_t by) {
  if (!p.tiled) return (by * p.blocks_wide + bx) * 16;
  return TiledInner(bx, by, 4, TiledOuter(by, p.blocks_wide, 4));
}

void ReadBlock(const uint8_t* base, const Page& p, uint32_t bx, uint32_t by, uint8_t (&out)[16]) {
  std::memcpy(out, base + p.data + BlockOffset(p, bx, by), 16);
  if (p.endian == 1)
    for (int i = 0; i < 16; i += 2) std::swap(out[i], out[i + 1]);
}
void WriteBlock(uint8_t* base, const Page& p, uint32_t bx, uint32_t by, uint8_t (&in)[16]) {
  if (p.endian == 1)
    for (int i = 0; i < 16; i += 2) std::swap(in[i], in[i + 1]);
  std::memcpy(base + p.data + BlockOffset(p, bx, by), in, 16);
}

// --- DXT5 -------------------------------------------------------------------------------
uint16_t To565(const uint8_t* c) { return uint16_t((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | (c[2] >> 3)); }
void From565(uint16_t v, int* c) {
  c[0] = ((v >> 11) & 31) * 255 / 31;
  c[1] = ((v >> 5) & 63) * 255 / 63;
  c[2] = (v & 31) * 255 / 31;
}

// One 4x4 block (16 RGBA texels) to DXT5's 16 bytes, little-endian as the format defines.
void EncodeBlock(const uint8_t (&px)[16][4], uint8_t (&out)[16]) {
  // Alpha: endpoints max and min, 8-step palette.
  int amax = 0, amin = 255;
  for (const auto& t : px) { amax = std::max<int>(amax, t[3]); amin = std::min<int>(amin, t[3]); }
  out[0] = uint8_t(amax);
  out[1] = uint8_t(amin);
  int apal[8] = {amax, amin};
  for (int i = 1; i < 7; ++i) apal[i + 1] = ((7 - i) * amax + i * amin) / 7;
  uint64_t abits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0, bestd = 1 << 30;
    for (int k = 0; k < 8; ++k) {
      const int d = std::abs(apal[k] - px[i][3]);
      if (d < bestd) { bestd = d; best = k; }
    }
    if (amax == amin) best = 0;
    abits |= uint64_t(best) << (3 * i);
  }
  for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(abits >> (8 * i));
  // Colour: endpoints from the brightest and darkest visible texels, 4-step palette (c0 > c1).
  int lo = 1 << 30, hi = -1, ilo = 0, ihi = 0;
  for (int i = 0; i < 16; ++i) {
    if (px[i][3] < 8) continue;
    const int l = px[i][0] * 299 + px[i][1] * 587 + px[i][2] * 114;
    if (l < lo) { lo = l; ilo = i; }
    if (l > hi) { hi = l; ihi = i; }
  }
  uint16_t c0 = To565(px[ihi]), c1 = To565(px[ilo]);
  if (hi < 0) c0 = c1 = 0;
  if (c0 < c1) std::swap(c0, c1);
  int pal[4][3];
  From565(c0, pal[0]);
  From565(c1, pal[1]);
  for (int k = 0; k < 3; ++k) {
    pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
    pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
  }
  uint32_t cbits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0, bestd = 1 << 30;
    for (int k = 0; k < (c0 == c1 ? 1 : 4); ++k) {
      int d = 0;
      for (int j = 0; j < 3; ++j) d += (pal[k][j] - px[i][j]) * (pal[k][j] - px[i][j]);
      if (d < bestd) { bestd = d; best = k; }
    }
    cbits |= uint32_t(best) << (2 * i);
  }
  out[8] = uint8_t(c0);
  out[9] = uint8_t(c0 >> 8);
  out[10] = uint8_t(c1);
  out[11] = uint8_t(c1 >> 8);
  for (int i = 0; i < 4; ++i) out[12 + i] = uint8_t(cbits >> (8 * i));
}

void DecodeBlock(const uint8_t (&in)[16], uint8_t (&px)[16][4]) {
  int apal[8] = {in[0], in[1]};
  if (in[0] > in[1]) {
    for (int i = 1; i < 7; ++i) apal[i + 1] = ((7 - i) * in[0] + i * in[1]) / 7;
  } else {
    for (int i = 1; i < 5; ++i) apal[i + 1] = ((5 - i) * in[0] + i * in[1]) / 5;
    apal[6] = 0;
    apal[7] = 255;
  }
  uint64_t abits = 0;
  for (int i = 0; i < 6; ++i) abits |= uint64_t(in[2 + i]) << (8 * i);
  const uint16_t c0 = uint16_t(in[8] | in[9] << 8), c1 = uint16_t(in[10] | in[11] << 8);
  int pal[4][3];
  From565(c0, pal[0]);
  From565(c1, pal[1]);
  for (int k = 0; k < 3; ++k) {
    pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
    pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
  }
  const uint32_t cbits = uint32_t(in[12] | in[13] << 8 | in[14] << 16 | uint32_t(in[15]) << 24);
  for (int i = 0; i < 16; ++i) {
    const int k = (cbits >> (2 * i)) & 3;
    for (int j = 0; j < 3; ++j) px[i][j] = uint8_t(pal[k][j]);
    px[i][3] = uint8_t(apal[(abits >> (3 * i)) & 7]);
  }
}

void DumpPage(const uint8_t* base, const Page& p, const std::string& name) {
  std::vector<uint8_t> rgba(size_t(p.width) * p.height * 4);
  for (uint32_t by = 0; by < p.height / 4; ++by) {
    for (uint32_t bx = 0; bx < p.width / 4; ++bx) {
      uint8_t blk[16], px[16][4];
      ReadBlock(base, p, bx, by, blk);
      DecodeBlock(blk, px);
      for (int i = 0; i < 16; ++i) {
        const size_t o = (size_t(by * 4 + i / 4) * p.width + bx * 4 + i % 4) * 4;
        std::memcpy(&rgba[o], px[i], 4);
      }
    }
  }
  std::filesystem::create_directories("logs");
  std::ofstream f("logs/" + name, std::ios::binary);
  const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(p.width), uint8_t(p.width >> 8),
                              uint8_t(p.height), uint8_t(p.height >> 8), 32, 0x28};
  f.write(reinterpret_cast<const char*>(header), 18);
  for (size_t i = 0; i < rgba.size(); i += 4) {
    const char bgra[4] = {char(rgba[i + 2]), char(rgba[i + 1]), char(rgba[i]), char(rgba[i + 3])};
    f.write(bgra, 4);
  }
  REXLOG_INFO("key_glyphs: wrote logs/{}", name);
}

// An image scaled to w x h by averaging (alpha-weighted, so edges don't darken).
std::vector<uint8_t> Scale(const std::vector<uint8_t>& src, int sw, int sh, int w, int h) {
  std::vector<uint8_t> out(size_t(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int x0 = x * sw / w, x1 = std::max(x0 + 1, (x + 1) * sw / w);
      const int y0 = y * sh / h, y1 = std::max(y0 + 1, (y + 1) * sh / h);
      double r = 0, g = 0, b = 0, a = 0;
      int n = 0;
      for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
          const uint8_t* s = &src[(size_t(yy) * sw + xx) * 4];
          const double al = s[3] / 255.0;
          r += s[0] * al; g += s[1] * al; b += s[2] * al; a += al;
          ++n;
        }
      }
      uint8_t* d = &out[(size_t(y) * w + x) * 4];
      d[0] = uint8_t(a > 0 ? std::lround(r / a) : 0);
      d[1] = uint8_t(a > 0 ? std::lround(g / a) : 0);
      d[2] = uint8_t(a > 0 ? std::lround(b / a) : 0);
      d[3] = uint8_t(std::lround(a / n * 255.0));
    }
  }
  return out;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

std::mutex g_mutex;
bool g_tried = false;
std::unordered_map<std::string, uint32_t> g_chars;  // lower-case key name -> character
struct Image {
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
};
std::unordered_map<std::string, Image> g_images;  // lower-case key name -> its PNG, decoded

// An image without its transparent margin, so a badge's box is filled by the key itself.
Image Cropped(const std::vector<uint8_t>& rgba, int w, int h) {
  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (rgba[(size_t(y) * w + x) * 4 + 3] > 8) {
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
      }
  if (x1 < 0) return Image{rgba, w, h};
  Image out;
  out.w = x1 - x0 + 1;
  out.h = y1 - y0 + 1;
  out.rgba.resize(size_t(out.w) * out.h * 4);
  for (int y = 0; y < out.h; ++y)
    std::memcpy(&out.rgba[size_t(y) * out.w * 4], &rgba[(size_t(y0 + y) * w + x0) * 4], size_t(out.w) * 4);
  return out;
}

std::filesystem::path KeysDir() {
  for (const char* dir : {"keys", "assets/keys"}) {
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) return dir;
  }
  return {};
}

// --- HUD badges ---------------------------------------------------------------------------
// Some button badges aren't text but parts of HUD textures, drawn in the 3D scene: the A by a
// door and the B on the takedown meter (BourneUIPlatformSpecific.InteractButtonIcons, 512x256
// DXT5: A, B, X, Y in the top row, RT, LT, RB, LB below, one 128x128 cell each with the badge
// in its lower-right part) and the single QTE prompt (ButtonPresses, 128x512: A, B, X, Y
// cells top to bottom, each badge where it sits on the controller's diamond). Their layout is
// from the textures as extracted (assets/*.dds). Each badge's area is cleared and the bound
// key's glyph drawn there instead, scaled to fit, the way glyphs go into the button font.

struct Badge {
  int cell_x, cell_y;            // the cell's top-left texel
  int clear[4];                  // area cleared, relative to the cell (x0, y0, x1, y1; 4-aligned)
  float cx, cy, box_w, box_h;    // where the glyph goes (centre) and its largest size
  const char* bind;              // whose key it shows
};

struct HudTexture {
  const char* name;
  uint32_t width, height;
  std::vector<Badge> badges;
  uint32_t object = 0, data = 0;  // patched: which object, its pixels, and a check block
  uint8_t check[16] = {};
  uint32_t check_x = 0, check_y = 0;
  std::string keys;               // the keys drawn, to redo it when binds change
};

std::vector<HudTexture>& HudTextures() {
  static std::vector<HudTexture> list = [] {
    std::vector<HudTexture> v;
    HudTexture interact{"BourneUIPlatformSpecific.InteractButtonIcons", 512, 256, {}};
    const char* row0[4] = {"keybind_a", "keybind_b", "keybind_x", "keybind_y"};
    const char* row1[4] = {"keybind_right_trigger", "keybind_left_trigger", "keybind_right_shoulder",
                           "keybind_left_shoulder"};
    for (int i = 0; i < 4; ++i) {
      interact.badges.push_back({i * 128, 0, {64, 64, 128, 128}, 97.5f, 97.5f, 56, 40, row0[i]});
      interact.badges.push_back({i * 128, 128, {64, 64, 128, 128}, 98.0f, 101.5f, 56, 40, row1[i]});
    }
    v.push_back(interact);
    // QTE prompt: "@qte" binds show the W / A / S / D keys while --qte_wasd is on.
    HudTexture presses{"BourneUIPlatformSpecific.ButtonPresses", 128, 512, {}};
    presses.badges.push_back({0, 0, {40, 60, 88, 100}, 64.0f, 80.5f, 46, 34, "keybind_a@qte"});
    presses.badges.push_back({0, 128, {80, 36, 128, 72}, 102.5f, 53.0f, 44, 32, "keybind_b@qte"});
    presses.badges.push_back({0, 256, {0, 36, 48, 72}, 25.5f, 53.0f, 44, 32, "keybind_x@qte"});
    presses.badges.push_back({0, 384, {44, 12, 84, 44}, 64.0f, 29.0f, 38, 30, "keybind_y@qte"});
    v.push_back(presses);
    return v;
  }();
  return list;
}

uint32_t FindObject(const PPCContext& ctx, uint8_t* base, const char* name) {
  PPCContext tmp = ctx;
  const uint32_t text = ctx.r1.u32 - 0x300;  // UTF-16BE name below the caller's frame
  for (size_t i = 0;; ++i) {
    const uint16_t c = uint8_t(name[i]);
    base[text + 2 * i] = uint8_t(c >> 8);
    base[text + 2 * i + 1] = uint8_t(c);
    if (!c) break;
  }
  tmp.r1.u32 = ctx.r1.u32 - 0x600;
  tmp.r3.u64 = 0;
  tmp.r4.u64 = 0;
  tmp.r5.u64 = text;
  tmp.r6.u64 = 0;
  sub_82920EE8(tmp, base);
  return tmp.r3.u32;
}

// The texture's pixels, as for the font page; false if it isn't on the GPU (yet).
bool TexturePage(uint8_t* base, uint32_t texture, Page& p, uint32_t& format) {
  const uint32_t resource = Readable(texture + 0xBC) ? Load32(base, texture + 0xB8) : 0;
  const uint32_t rhi = resource && Readable(resource + 0x18) ? Load32(base, resource + 0x14) : 0;
  const uint32_t header = rhi && Readable(rhi + 0x0C) ? Load32(base, rhi + 0x08) : 0;
  if (!header || !Readable(header + 0x28)) return false;
  const uint32_t dw0 = Load32(base, header + 0x1C), dw1 = Load32(base, header + 0x20),
                 dw2 = Load32(base, header + 0x24);
  p.tiled = (dw0 >> 31) != 0;
  p.blocks_wide = ((dw0 >> 22) & 0x1FF) * 32 / 4;
  p.width = (dw2 & 0x1FFF) + 1;
  p.height = ((dw2 >> 13) & 0x1FFF) + 1;
  p.endian = (dw1 >> 6) & 3;
  p.data = dw1 & 0xFFFFF000;
  format = dw1 & 0x3F;
  return p.data != 0 && Readable(p.data);
}

// Clears a badge's area and draws the key's image into it, centred, scaled to fit its box.
void DrawBadge(uint8_t* base, const Page& p, const Badge& b, const Image* img) {
  const int x0 = b.cell_x + b.clear[0], y0 = b.cell_y + b.clear[1];
  const int x1 = b.cell_x + b.clear[2], y1 = b.cell_y + b.clear[3];
  std::vector<uint8_t> glyph;
  int gw = 0, gh = 0, gx = 0, gy = 0;
  if (img && img->w > 0 && img->h > 0) {
    const float scale = std::min(b.box_w / img->w, b.box_h / img->h);
    gw = std::max(1, int(std::lround(img->w * scale)));
    gh = std::max(1, int(std::lround(img->h * scale)));
    gx = b.cell_x + int(std::lround(b.cx - gw / 2.0f));
    gy = b.cell_y + int(std::lround(b.cy - gh / 2.0f));
    glyph = Scale(img->rgba, img->w, img->h, gw, gh);
  }
  for (int by = y0 / 4; by < y1 / 4; ++by) {
    for (int bx = x0 / 4; bx < x1 / 4; ++bx) {
      uint8_t px[16][4] = {}, blk[16];
      for (int i = 0; i < 16; ++i) {
        const int x = bx * 4 + i % 4 - gx, y = by * 4 + i / 4 - gy;
        if (!glyph.empty() && x >= 0 && y >= 0 && x < gw && y < gh)
          std::memcpy(px[i], &glyph[(size_t(y) * gw + x) * 4], 4);
      }
      EncodeBlock(px, blk);
      WriteBlock(base, p, uint32_t(bx), uint32_t(by), blk);
    }
  }
}

}  // namespace

namespace tbc::glyphs {

// Called with the button font on its first draw (keyboard_prompts.cpp).
void Install(uint8_t* base, uint32_t font) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_tried || !REXCVAR_GET(key_glyphs)) return;
  g_tried = true;
  const std::filesystem::path dir = KeysDir();
  if (dir.empty()) {
    REXLOG_INFO("key_glyphs: no keys folder; prompts use text");
    return;
  }
  // The page and its GPU header.
  const uint32_t pages = Load32(base, font + 0x48);
  const uint32_t table = Load32(base, font + 0x3C), count = Load32(base, font + 0x40);
  if (!Readable(pages) || !Readable(table) || count < 256 || Load32(base, font + 140) != 0) {
    REXLOG_WARN("key_glyphs: the font isn't laid out as expected; prompts use text");
    return;
  }
  const uint32_t texture = Load32(base, pages);
  const uint32_t resource = Readable(texture + 0xBC) ? Load32(base, texture + 0xB8) : 0;
  const uint32_t rhi = resource && Readable(resource + 0x18) ? Load32(base, resource + 0x14) : 0;
  const uint32_t header = rhi && Readable(rhi + 0x0C) ? Load32(base, rhi + 0x08) : 0;
  if (!header || !Readable(header + 0x28)) {
    REXLOG_WARN("key_glyphs: no GPU header for the font page; prompts use text");
    return;
  }
  const uint32_t dw0 = Load32(base, header + 0x1C), dw1 = Load32(base, header + 0x20),
                 dw2 = Load32(base, header + 0x24);
  Page p;
  p.tiled = (dw0 >> 31) != 0;
  p.blocks_wide = ((dw0 >> 22) & 0x1FF) * 32 / 4;
  p.width = (dw2 & 0x1FFF) + 1;
  p.height = ((dw2 >> 13) & 0x1FFF) + 1;
  p.endian = (dw1 >> 6) & 3;
  p.data = dw1 & 0xFFFFF000;
  REXLOG_INFO("key_glyphs: font page {}x{}, format {:02X}, {}, pitch {} blocks, endian {}, data {:08X}",
              p.width, p.height, dw1 & 0x3F, p.tiled ? "tiled" : "linear", p.blocks_wide, p.endian, p.data);
  if ((dw1 & 0x3F) != 0x14 || p.width != 512 || p.height != 512 || p.blocks_wide < 128 ||
      !Readable(p.data) || !Readable(p.data + 0x3FFFF)) {
    REXLOG_WARN("key_glyphs: not the 512x512 DXT5 page expected; prompts use text");
    return;
  }
  if (REXCVAR_GET(key_glyphs_dump)) DumpPage(base, p, "font_page_before.tga");

  // Cells in the free rows (368..508), 4-texel aligned for DXT5.
  constexpr int kCell = 40, kTop = 368, kGap = 4;
  int x = 4, y = kTop;
  uint32_t next_char = 0x80;
  const uint32_t a_entry = table + 24 * 'A';
  std::vector<std::filesystem::directory_entry> files;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().extension() == ".png" || e.path().extension() == ".PNG") files.push_back(e);
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path() < b.path(); });
  for (const auto& e : files) {
    std::ifstream f(e.path(), std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int sw = 0, sh = 0;
    const std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), sw, sh);
    if (rgba.empty() || sw <= 0 || sh <= 0) {
      REXLOG_WARN("key_glyphs: can't read {}", e.path().filename().string());
      continue;
    }
    const int w = sw >= sh * 3 / 2 ? kCell * 2 : kCell, h = kCell;
    if (x + w > int(p.width)) {
      x = 4;
      y += kCell + kGap;
    }
    if (y + h > int(p.height) || next_char > 0xFF) {
      REXLOG_WARN("key_glyphs: no room left on the page for {}", e.path().filename().string());
      break;
    }
    const std::vector<uint8_t> cell = Scale(rgba, sw, sh, w, h);
    for (int by = 0; by < h / 4; ++by) {
      for (int bx = 0; bx < w / 4; ++bx) {
        uint8_t px[16][4], blk[16];
        for (int i = 0; i < 16; ++i)
          std::memcpy(px[i], &cell[(size_t(by * 4 + i / 4) * w + bx * 4 + i % 4) * 4], 4);
        EncodeBlock(px, blk);
        WriteBlock(base, p, uint32_t(x / 4 + bx), uint32_t(y / 4 + by), blk);
      }
    }
    const uint32_t c = next_char++;
    const uint32_t entry = table + 24 * c;
    Store32(base, entry + 0, uint32_t(x));
    Store32(base, entry + 4, uint32_t(y));
    Store32(base, entry + 8, uint32_t(w));
    Store32(base, entry + 12, uint32_t(h));
    Store32(base, entry + 16, Load32(base, a_entry + 16));
    Store32(base, entry + 20, Load32(base, a_entry + 20));
    std::string key = Lower(e.path().stem().string());
    // "keyboard_w", "keyboard_backspace_icon", "mouse_left": the key name in the middle.
    if (key.starts_with("keyboard_")) key = key.substr(9);
    if (key.ends_with("_icon")) key.resize(key.size() - 5);
    static const std::unordered_map<std::string, std::string> kAlias = {
        {"mouse_left", "lmb"}, {"mouse_right", "rmb"}, {"mouse_middle", "mmb"}, {"enter", "return"},
        {"esc", "escape"},     {"ctrl", "control"}};
    if (const auto it = kAlias.find(key); it != kAlias.end()) key = it->second;
    g_chars[key] = c;
    g_images[key] = Cropped(rgba, sw, sh);
    REXLOG_INFO("key_glyphs: {} -> character {:02X} at {},{} ({}x{})", e.path().filename().string(), c, x, y, w, h);
    x += w + kGap;
  }
  if (REXCVAR_GET(key_glyphs_dump)) DumpPage(base, p, "font_page_after.tga");
}

// The character drawing this key's glyph in the button font, or 0 for none.
uint32_t CharFor(const std::string& key) {
  if (!REXCVAR_GET(key_glyphs)) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_chars.find(Lower(key));
  return it == g_chars.end() ? 0 : it->second;
}

// Called on every button-font draw while keyboard and mouse are on (keyboard_prompts.cpp), with
// the key each bind shows ("Space", "F", "LMB"). Once a second it checks each HUD texture:
// loaded, and not yet patched with these keys (or reloaded since), it gets the key glyphs.
void PatchHud(const PPCContext& ctx, uint8_t* base, const std::function<std::string(const char*)>& key_for) {
  if (!REXCVAR_GET(key_glyphs)) return;
  static auto next = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(1);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_images.empty()) return;
  for (HudTexture& t : HudTextures()) {
    const uint32_t object = FindObject(ctx, base, t.name);
    if (!object) continue;
    Page p;
    uint32_t format = 0;
    if (!TexturePage(base, object, p, format)) continue;
    std::string keys;
    std::vector<const Image*> images;
    for (const Badge& b : t.badges) {
      const std::string key = Lower(key_for(b.bind));
      keys += key + ",";
      const auto it = g_images.find(key);
      images.push_back(it == g_images.end() ? nullptr : &it->second);
    }
    if (t.object == object && t.data == p.data && t.keys == keys) {
      uint8_t blk[16];
      ReadBlock(base, p, t.check_x, t.check_y, blk);
      if (std::memcmp(blk, t.check, 16) == 0) continue;  // still ours
    }
    if (format != 0x14 || p.width != t.width || p.height != t.height ||
        !Readable(p.data + p.blocks_wide * 16 * (p.height / 4) - 1)) {
      if (t.object != object) {
        REXLOG_WARN("key_glyphs: {} is {}x{} format {:02X}, not as expected; left as it is", t.name, p.width,
                    p.height, format);
        t.object = object;
        t.data = p.data;
        t.keys = keys;
      }
      continue;
    }
    for (size_t i = 0; i < t.badges.size(); ++i) {
      // A key without an image keeps the controller badge: better than an empty space.
      if (images[i]) DrawBadge(base, p, t.badges[i], images[i]);
    }
    const Badge& first = t.badges[0];
    t.check_x = uint32_t((first.cell_x + first.clear[0]) / 4);
    t.check_y = uint32_t((first.cell_y + first.clear[1]) / 4);
    ReadBlock(base, p, t.check_x, t.check_y, t.check);
    t.object = object;
    t.data = p.data;
    t.keys = keys;
    REXLOG_INFO("key_glyphs: {} badges drawn as keys {}", t.name, keys);
  }
}

// Development (--prompt_trace): a texture's top mip to logs/<name>.tga, to see how a HUD image
// is laid out. DXT1, DXT3/5 and 8888 only. False if it isn't on the GPU yet.
bool DumpTexture(uint8_t* base, uint32_t texture, const std::string& name) {
  const uint32_t resource = Readable(texture + 0xBC) ? Load32(base, texture + 0xB8) : 0;
  const uint32_t rhi = resource && Readable(resource + 0x18) ? Load32(base, resource + 0x14) : 0;
  const uint32_t header = rhi && Readable(rhi + 0x0C) ? Load32(base, rhi + 0x08) : 0;
  if (!header || !Readable(header + 0x28)) return false;
  const uint32_t dw0 = Load32(base, header + 0x1C), dw1 = Load32(base, header + 0x20),
                 dw2 = Load32(base, header + 0x24);
  const uint32_t format = dw1 & 0x3F;
  const bool tiled = (dw0 >> 31) != 0;
  const uint32_t pitch = ((dw0 >> 22) & 0x1FF) * 32;  // texels
  const uint32_t width = (dw2 & 0x1FFF) + 1, height = ((dw2 >> 13) & 0x1FFF) + 1;
  const uint32_t endian = (dw1 >> 6) & 3, data = dw1 & 0xFFFFF000;
  REXLOG_INFO("texture_dump: {} {}x{}, format {:02X}, {}, pitch {}, endian {}, data {:08X}", name, width,
              height, format, tiled ? "tiled" : "linear", pitch, endian, data);
  if (!data || !Readable(data) || width > 4096 || height > 4096) return true;
  const bool block = format == 0x12 || format == 0x13 || format == 0x14;
  const uint32_t unit = block ? 4 : 1;  // texels per block side
  const uint32_t bytes = format == 0x12 ? 8 : block ? 16 : 4;
  const uint32_t log_bpp = bytes == 16 ? 4 : bytes == 8 ? 3 : 2;
  if (!block && format != 0x06) return true;
  const uint32_t bw = std::max<uint32_t>(pitch / unit, 1);
  const uint32_t aligned_h = (height + 31) & ~31u;
  if (!Readable(data + bw * (aligned_h / unit) * bytes - 1)) return true;
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  for (uint32_t by = 0; by < (height + unit - 1) / unit; ++by) {
    for (uint32_t bx = 0; bx < (width + unit - 1) / unit; ++bx) {
      const uint32_t off = tiled ? TiledInner(bx, by, log_bpp, TiledOuter(by, bw, log_bpp)) : (by * bw + bx) * bytes;
      uint8_t in[16] = {};
      std::memcpy(in, base + data + off, bytes);
      if (endian == 1)
        for (uint32_t i = 0; i < bytes; i += 2) std::swap(in[i], in[i + 1]);
      else if (endian == 2)
        for (uint32_t i = 0; i < bytes; i += 4) std::swap(in[i], in[i + 3]), std::swap(in[i + 1], in[i + 2]);
      uint8_t px[16][4];
      if (format == 0x06) {  // k_8_8_8_8 as stored: A R G B? log as B G R A after swap
        px[0][0] = in[2]; px[0][1] = in[1]; px[0][2] = in[0]; px[0][3] = in[3];
      } else if (format == 0x12) {
        uint8_t full[16] = {255, 255, 255, 255, 255, 255, 255, 255};
        std::memcpy(full + 8, in, 8);
        DecodeBlock(full, px);
        const uint16_t c0 = uint16_t(in[0] | in[1] << 8), c1 = uint16_t(in[2] | in[3] << 8);
        if (c0 <= c1) {  // 3-colour mode: index 3 is transparent
          const uint32_t bits = uint32_t(in[4] | in[5] << 8 | in[6] << 16 | uint32_t(in[7]) << 24);
          for (int i = 0; i < 16; ++i)
            if (((bits >> (2 * i)) & 3) == 3) px[i][3] = 0;
        }
      } else {
        DecodeBlock(in, px);  // DXT3's alpha isn't decoded (shown as DXT5's)
      }
      for (uint32_t i = 0; i < unit * unit; ++i) {
        const uint32_t x = bx * unit + i % unit, y = by * unit + i / unit;
        if (x < width && y < height) std::memcpy(&rgba[(size_t(y) * width + x) * 4], px[i], 4);
      }
    }
  }
  std::filesystem::create_directories("logs");
  std::ofstream f("logs/" + name + ".tga", std::ios::binary);
  const uint8_t tga[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(width), uint8_t(width >> 8),
                           uint8_t(height), uint8_t(height >> 8), 32, 0x28};
  f.write(reinterpret_cast<const char*>(tga), 18);
  for (size_t i = 0; i < rgba.size(); i += 4) {
    const char bgra[4] = {char(rgba[i + 2]), char(rgba[i + 1]), char(rgba[i]), char(rgba[i + 3])};
    f.write(bgra, 4);
  }
  return true;
}

}  // namespace tbc::glyphs

