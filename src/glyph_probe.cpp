// tbc - --glyph_probe (development): how a font's texture page reaches the GPU.
//
// Key glyphs (assets/keys/*.png) are to be drawn the way the button characters are: from a
// texture, through DrawString's or DrawTile's FTexture. To make a texture of our own the game
// can draw, we need to know what one looks like in memory: UFont -> its texture page
// (UTexture2D) -> the render resource (FTexture2DResource, FTexture: what DrawTile takes) ->
// the GPU texture (a D3D texture header with the fetch constant the GPU reads).
//
// The first time ControllerButtonFont is drawn, this dumps the font object, every object it
// points to whose name says Texture, and two levels of plain (non-object) memory those point
// to, so the chain and its offsets can be read off the log.
//
//   --glyph_probe=true

#include "generated/default/tbc_pch.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "prompts.h"

REXCVAR_DEFINE_BOOL(glyph_probe, false, "Debug",
                    "Development: dump how ControllerButtonFont's texture reaches the GPU");

namespace {

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 | uint32_t(base[addr + 2]) << 8 |
         base[addr + 3];
}

bool Readable(uint32_t addr) {
  static std::unordered_map<uint32_t, bool> pages;
  const uint32_t page = addr >> 12;
  if (const auto it = pages.find(page); it != pages.end()) return it->second;
  bool ok = false;
  auto* runtime = rex::Runtime::instance();
  if (runtime && runtime->memory()) {
    if (auto* heap = runtime->memory()->LookupHeap(addr)) {
      uint32_t protect = 0;
      ok = heap->QueryProtect(addr, &protect) && (protect & rex::memory::kMemoryProtectRead);
    }
  }
  pages.emplace(page, ok);
  return ok;
}

bool Pointer(uint32_t a) { return a >= 0x40000000 && a < 0xE0000000 && (a & 3) == 0 && Readable(a); }

// A UObject: +0x30 is its class, and the class is an object too (its own +0x30 readable).
bool LooksLikeObject(const uint8_t* base, uint32_t a) {
  if (!Pointer(a) || !Readable(a + 0x34)) return false;
  const uint32_t cls = Load32(base, a + 0x30);
  if (!Pointer(cls) || !Readable(cls + 0x34)) return false;
  const uint32_t meta = Load32(base, cls + 0x30);
  return Pointer(meta) && (Load32(base, a) >> 24) == 0x82;  // vtable in the executable
}

std::string Words(const uint8_t* base, uint32_t addr, uint32_t size) {
  std::string out;
  char buf[24];
  for (uint32_t i = 0; i < size; i += 4) {
    if (i % 32 == 0) {
      std::snprintf(buf, sizeof(buf), "\n    +%03X", i);
      out += buf;
    }
    std::snprintf(buf, sizeof(buf), " %08X", Readable(addr + i) ? Load32(base, addr + i) : 0xDEADDEADu);
    out += buf;
  }
  return out;
}

// Heap or physical memory, not the executable (vtables and code, which the first run followed).
bool Data(uint32_t a) {
  return ((a >= 0x40000000 && a < 0x80000000) || (a >= 0xA0000000 && a < 0xE0000000)) && (a & 3) == 0 &&
         Readable(a);
}

void Plain(const uint8_t* base, uint32_t from, uint32_t at, uint32_t size, int depth, std::set<uint32_t>& seen) {
  for (uint32_t i = 0; i < size; i += 4) {
    const uint32_t p = Load32(base, at + i);
    if (!Data(p) || LooksLikeObject(base, p) || !seen.insert(p).second || seen.size() > 80) continue;
    REXLOG_INFO("glyph_probe: {:08X}+{:03X} -> memory {:08X}{}", at, i, p, Words(base, p, 0x80));
    if (depth > 0) Plain(base, at, p, 0x80, depth - 1, seen);
  }
  (void)from;
}

}  // namespace

namespace tbc::glyphs {

void ProbeFont(const PPCContext& ctx, uint8_t* base, uint32_t font) {
  if (!REXCVAR_GET(glyph_probe)) return;
  static bool done = false;
  static std::unordered_map<uint32_t, bool> checked;
  if (done) return;
  if (const auto it = checked.find(font); it != checked.end() && !it->second) return;
  const std::string name = tbc::prompts::ObjectPathName(ctx, base, font);
  const bool button = name == "BourneUIPlatformSpecific.ControllerButtonFont";
  checked.emplace(font, button);
  if (!button) return;
  done = true;
  REXLOG_INFO("glyph_probe: font {:08X} {}{}", font, name, Words(base, font, 0x100));
  std::set<uint32_t> seen;
  for (uint32_t i = 0; i < 0x100; i += 4) {
    const uint32_t p = Load32(base, font + i);
    if (!LooksLikeObject(base, p) || !seen.insert(p).second) continue;
    const std::string what = tbc::prompts::ObjectPathName(ctx, base, p);
    const std::string cls = tbc::prompts::ObjectPathName(ctx, base, Load32(base, p + 0x30));
    REXLOG_INFO("glyph_probe: font+{:03X} -> object {:08X} {} ({})", i, p, what, cls);
    if (cls.find("Texture") == std::string::npos) continue;
    REXLOG_INFO("glyph_probe: texture {:08X}{}", p, Words(base, p, 0x180));
    Plain(base, p, p, 0x180, 2, seen);
  }
  // Second run: the render resource (Texture2D +0xB8, an FTexture2DResource, which the first
  // object check skipped since it has a vtable too) and what it points to (the GPU texture
  // header with the fetch constant); and the glyph table (UFont +0x3C data, +0x40 count; 24
  // bytes each: StartU, StartV, USize, VSize (ints, texels), TextureIndex, ...) so the free
  // space on the 512x512 page shows.
  {
    const uint32_t chars = Load32(base, font + 0x3C), count = Load32(base, font + 0x40);
    if (Data(chars) && count && count <= 512) {
      std::string used;
      char buf[96];
      for (uint32_t c = 0; c < count; ++c) {
        const uint32_t e = chars + 24 * c;
        const uint32_t u = Load32(base, e), v = Load32(base, e + 4), w = Load32(base, e + 8), h = Load32(base, e + 12);
        if (!w || !h) continue;
        std::snprintf(buf, sizeof(buf), "\n    char %3u '%c': %u,%u %ux%u page %u (+16 %08X +20 %08X)", c,
                      c >= 32 && c < 127 ? char(c) : '?', u, v, w, h, Load32(base, e + 16), Load32(base, e + 16),
                      Load32(base, e + 20));
        used += buf;
      }
      REXLOG_INFO("glyph_probe: glyph table {:08X}, {} entries{}", chars, count, used);
    }
    const uint32_t page = [&] {
      const uint32_t data = Load32(base, font + 0x48);
      return Data(data) ? Load32(base, data) : 0u;
    }();
    if (page && Readable(page + 0xBC)) {
      const uint32_t resource = Load32(base, page + 0xB8);
      if (Data(resource)) {
        REXLOG_INFO("glyph_probe: texture {:08X} resource {:08X}{}", page, resource, Words(base, resource, 0x100));
        std::set<uint32_t> seen2;
        for (uint32_t i = 4; i < 0x100; i += 4) {
          const uint32_t p = Load32(base, resource + i);
          if (!Data(p) || !seen2.insert(p).second) continue;
          REXLOG_INFO("glyph_probe: resource+{:03X} -> {:08X}{}", i, p, Words(base, p, 0x80));
          for (uint32_t j = 0; j < 0x80; j += 4) {
            const uint32_t q = Load32(base, p + j);
            if (!Data(q) || !seen2.insert(q).second || seen2.size() > 40) continue;
            REXLOG_INFO("glyph_probe: resource+{:03X}+{:03X} -> {:08X}{}", i, j, q, Words(base, q, 0x60));
          }
        }
      }
    }
  }
  // The font's texture list may be a TArray (data, count, max) rather than direct pointers.
  for (uint32_t i = 0; i < 0x100; i += 4) {
    const uint32_t data = Load32(base, font + i), count = Load32(base, font + i + 4);
    if (!Pointer(data) || count == 0 || count > 8 || LooksLikeObject(base, data)) continue;
    for (uint32_t k = 0; k < count; ++k) {
      const uint32_t p = Load32(base, data + 4 * k);
      if (!LooksLikeObject(base, p) || !seen.insert(p).second) continue;
      const std::string cls = tbc::prompts::ObjectPathName(ctx, base, Load32(base, p + 0x30));
      if (cls.find("Texture") == std::string::npos) continue;
      REXLOG_INFO("glyph_probe: font+{:03X}[{}] -> texture {:08X} {} ({}){}", i, k, p,
                  tbc::prompts::ObjectPathName(ctx, base, p), cls, Words(base, p, 0x180));
      Plain(base, p, p, 0x180, 2, seen);
    }
  }
}

}  // namespace tbc::glyphs
