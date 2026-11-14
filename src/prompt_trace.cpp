// tbc - find out where and how the game draws its button prompts (development tool).
//
// --prompt_trace=true logs every distinct 2D image the game draws to logs\prompt_trace.txt,
// once each: which texture (its full object name, e.g.
// BourneUIPlatformSpecific.ButtonPressesDiamond), which part of it (U, V, width, height in
// texels) and where on screen (X, Y, width, height in pixels). With that we can tell which
// draw is which prompt, and which part of the image is which button, before hooking them to
// draw key names instead (see docs/ROADMAP.md, "Keyboard prompts").
//
// Two functions in Bourne's Unreal Engine 3 code, found from the native function table
// ("intUCanvasexecDrawTile" -> sub_82C52F40 -> sub_82C4B5A0 -> sub_82C4ACC0):
//   sub_82C4ACC0  UCanvas::DrawTile: everything the HUD draws with Canvas.DrawTile.
//                 r3 = UCanvas, r4 = UTexture, f1..f8 = X, Y, XL, YL, U, V, UL, VL (texels).
//                 The texture's size is at +200 / +204, its render resource at +184.
//   sub_82C499C8  the engine-wide DrawTile(FCanvas*, ...) that the one above ends in, and that
//                 menus and UI scenes call directly (26 callers). f1..f8 = X, Y, XL, YL and U,
//                 V, UL, VL as 0..1; the texture's render resource is on the stack at r1+100.
//                 Its name comes from the first function when that one has drawn it too.
//   sub_82C4ADD8  UCanvas::DrawMaterialTile (from "intUCanvasexecDrawMaterialTile"): HUD
//                 images drawn with a material (animated or tinted ones). r3 = UCanvas,
//                 r4 = the material, f1..f8 = X, Y, XL, YL, U, V, UL, VL (0..1).
//   sub_82C49D88  the engine-wide DrawString(FCanvas*, X, Y, ...) that all text ends in, HUD
//                 (UCanvas::DrawText, sub_82C4B988, via WrappedPrint) and menus alike (21
//                 callers; hooked in keyboard_prompts.cpp, which calls TraceText). f1, f2 =
//                 X, Y, r6 = the text (UTF-16, ends with 0), r7 = the
//                 font. Characters outside plain ASCII are written as \uXXXX: button icons
//                 inside text (ControllerButtonFont) show up that way.
// Names come from the game's own UObject::GetPathName (sub_8291E040, from
// "intUObjectexecPathName"), called once per texture.
//
// Off by default; costs nothing then.

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "prompts.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(prompt_trace, false, "Debug",
                    "Log each distinct 2D image the game draws (texture, part, screen position) "
                    "to logs\\prompt_trace.txt; development only");

REX_EXTERN(__imp__sub_82C4ACC0);  // UCanvas::DrawTile
REX_EXTERN(__imp__sub_82C499C8);  // DrawTile(FCanvas*, ...)
REX_EXTERN(__imp__sub_82C4ADD8);  // UCanvas::DrawMaterialTile
REX_EXTERN(__imp__sub_82C48EC0);  // DrawTile(FCanvas*, ..., material render proxy)
REX_EXTERN(__imp__sub_82C49C20);  // DrawTriangle2D(FCanvas*, ...)
REX_EXTERN(sub_8291E040);         // UObject::GetPathName(FString* out, UObject*, UObject* stop)

namespace {

constexpr size_t kMaxEntries = 20000;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
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

struct Trace {
  std::mutex mutex;
  FILE* file = nullptr;
  bool opened = false;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  std::unordered_map<uint32_t, std::string> texture_names;  // UTexture -> path name
  std::unordered_map<uint32_t, uint32_t> resource_owner;    // render resource -> UTexture
  std::unordered_set<std::string> seen;
  std::unordered_map<uint32_t, std::string> owners;  // pointer -> "Object+offset"
};

Trace& T() {
  static Trace t;
  return t;
}

FILE* File(Trace& t) {
  if (!t.opened) {
    t.opened = true;
    std::filesystem::path dir = "logs";
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      dir = std::filesystem::path(buf).parent_path() / "logs";
    }
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "prompt_trace.txt";
    t.file = std::fopen(path.string().c_str(), "w");
    if (t.file) {
      std::fprintf(t.file,
                   "# time_ms\tfunction\ttexture\ttexture_size\tpart(U,V,W,H)\tscreen(X,Y,W,H)\n");
      REXLOG_INFO("prompt_trace: logging 2D draws to {}", path.string());
    }
  }
  return t.file;
}

// The texture's full name through the game's own GetPathName. Called on the thread that is
// drawing, at the start of UCanvas::DrawTile, with a copy of its registers: the call gets
// its own stack below the caller's, and the caller's registers are left as they were.
std::string PathName(const PPCContext& ctx, uint8_t* base, uint32_t object) {
  PPCContext tmp = ctx;
  const uint32_t out = ctx.r1.u32 - 0x40;  // an FString {data, count, max}, below the caller
  std::memset(base + out, 0, 12);
  tmp.r1.u32 = ctx.r1.u32 - 0x200;
  tmp.r3.u64 = out;
  tmp.r4.u64 = object;
  tmp.r5.u64 = 0;
  sub_8291E040(tmp, base);
  const uint32_t data = Load32(base, out);
  const uint32_t count = Load32(base, out + 4);
  std::string name;
  if (data && count > 0 && count < 512) {
    for (uint32_t i = 0; i + 1 < count; ++i) {  // UTF-16 big-endian, ends with a 0
      const uint16_t c = uint16_t(base[data + 2 * i] << 8 | base[data + 2 * i + 1]);
      name.push_back(c < 0x80 ? char(c) : '?');
    }
  }
  return name.empty() ? "?" : name;
}

// Which object holds this pointer (a texture's render resource, a material's render proxy):
// every object in GObjObjects (0x83655774: data, count) is searched, its first 0x200 bytes.
// Slow, but once per pointer, and only with the trace on.
std::string Owner(Trace& t, const PPCContext& ctx, uint8_t* base, uint32_t pointer) {
  if (const auto it = t.owners.find(pointer); it != t.owners.end()) return it->second;
  std::string found;
  const uint32_t data = Load32(base, 0x83655774), count = Load32(base, 0x83655778);
  if (pointer && Readable(data) && count < 2000000) {
    for (uint32_t i = 0; i < count && found.empty(); ++i) {
      const uint32_t object = Load32(base, data + 4 * i);
      if (!object || !Readable(object) || !Readable(object + 0x1FC)) continue;
      for (uint32_t off = 0x34; off < 0x200; off += 4) {
        if (Load32(base, object + off) == pointer) {
          char buf[16];
          std::snprintf(buf, sizeof(buf), "+%X", off);
          found = PathName(ctx, base, object) + buf;
          break;
        }
      }
    }
  }
  if (found.empty()) found = "?" + std::to_string(pointer);
  return t.owners.emplace(pointer, found).first->second;
}

void Log(Trace& t, const char* function, const std::string& texture, uint32_t size_x,
         uint32_t size_y, const double (&part)[4], const double (&screen)[4]) {
  if (t.seen.size() >= kMaxEntries) {
    return;
  }
  char key[256];
  std::snprintf(key, sizeof(key), "%s|%s|%.3f|%.3f|%.3f|%.3f", function, texture.c_str(), part[0],
                part[1], part[2], part[3]);
  if (!t.seen.insert(key).second) {
    return;
  }
  FILE* f = File(t);
  if (!f) {
    return;
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t.start)
                      .count();
  std::fprintf(f, "%lld\t%s\t%s\t%ux%u\t%g,%g,%g,%g\t%.1f,%.1f,%.1f,%.1f\n",
               static_cast<long long>(ms), function, texture.c_str(), size_x, size_y, part[0],
               part[1], part[2], part[3], screen[0], screen[1], screen[2], screen[3]);
  std::fflush(f);
}

}  // namespace

// UCanvas::DrawTile(UTexture*, X, Y, XL, YL, U, V, UL, VL, color)
extern "C" REX_FUNC(sub_82C4ACC0) {
  if (REXCVAR_GET(prompt_trace) && ctx.r4.u32) {
    Trace& t = T();
    std::lock_guard<std::mutex> lock(t.mutex);
    const uint32_t texture = ctx.r4.u32;
    auto it = t.texture_names.find(texture);
    if (it == t.texture_names.end()) {
      it = t.texture_names.emplace(texture, PathName(ctx, base, texture)).first;
      if (const uint32_t resource = Load32(base, texture + 184)) {
        t.resource_owner[resource] = texture;
      }
    }
    const double screen[4] = {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64};
    const double part[4] = {ctx.f5.f64, ctx.f6.f64, ctx.f7.f64, ctx.f8.f64};
    Log(t, "UCanvas", it->second, Load32(base, texture + 200), Load32(base, texture + 204), part,
        screen);
  }
  __imp__sub_82C4ACC0(ctx, base);
}

// UCanvas::DrawMaterialTile(UMaterialInterface*, X, Y, XL, YL, U, V, UL, VL)
extern "C" REX_FUNC(sub_82C4ADD8) {
  if (REXCVAR_GET(prompt_trace) && ctx.r4.u32) {
    Trace& t = T();
    std::lock_guard<std::mutex> lock(t.mutex);
    const uint32_t material = ctx.r4.u32;
    auto it = t.texture_names.find(material);
    if (it == t.texture_names.end()) {
      it = t.texture_names.emplace(material, PathName(ctx, base, material)).first;
    }
    const double screen[4] = {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64};
    const double part[4] = {ctx.f5.f64, ctx.f6.f64, ctx.f7.f64, ctx.f8.f64};
    Log(t, "Material", it->second, 0, 0, part, screen);
  }
  __imp__sub_82C4ADD8(ctx, base);
}

// DrawString(FCanvas*, X, Y, ..., const TCHAR* text, UFont* font, ...): called from the hook
// in keyboard_prompts.cpp, before it changes anything.
namespace tbc::prompts {
std::string ObjectPathName(const PPCContext& ctx, uint8_t* base, uint32_t object) {
  return PathName(ctx, base, object);
}

void TraceText(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(prompt_trace) && ctx.r6.u32 && ctx.r7.u32) {
    Trace& t = T();
    std::lock_guard<std::mutex> lock(t.mutex);
    const uint32_t font = ctx.r7.u32;
    auto it = t.texture_names.find(font);
    if (it == t.texture_names.end()) {
      it = t.texture_names.emplace(font, PathName(ctx, base, font)).first;
    }
    std::string text;
    for (uint32_t i = 0, p = ctx.r6.u32; i < 300; ++i, p += 2) {
      const uint16_t c = uint16_t(base[p] << 8 | base[p + 1]);
      if (c == 0) {
        break;
      }
      if (c >= 0x20 && c < 0x7F && c != '\t') {
        text.push_back(char(c));
      } else {
        char esc[8];
        std::snprintf(esc, sizeof(esc), "\\u%04X", c);
        text += esc;
      }
    }
    const double screen[4] = {ctx.f1.f64, ctx.f2.f64, 0, 0};
    const double part[4] = {0, 0, 0, 0};
    // Who called DrawString (its return address): HUD text and menu screens come from
    // different callers, which is how the two can be told apart.
    char function[24];
    std::snprintf(function, sizeof(function), "Text@%08X", static_cast<uint32_t>(ctx.lr));
    Log(t, function, it->second + "\t\"" + text + "\"", 0, 0, part, screen);
  }
}
}  // namespace tbc::prompts

// DrawTile(FCanvas*, X, Y, SizeX, SizeY, U, V, SizeU, SizeV, color, FTexture*, alpha blend)
extern "C" REX_FUNC(sub_82C499C8) {
  if (REXCVAR_GET(prompt_trace)) {
    Trace& t = T();
    std::lock_guard<std::mutex> lock(t.mutex);
    const uint32_t resource = Load32(base, ctx.r1.u32 + 100);
    std::string name;
    uint32_t size_x = 0, size_y = 0;
    if (const auto owner = t.resource_owner.find(resource); owner != t.resource_owner.end()) {
      name = t.texture_names[owner->second];
      size_x = Load32(base, owner->second + 200);
      size_y = Load32(base, owner->second + 204);
    } else {
      name = Owner(t, ctx, base, resource);  // drawn without UCanvas: find the texture
    }
    const double screen[4] = {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64};
    const double part[4] = {ctx.f5.f64, ctx.f6.f64, ctx.f7.f64, ctx.f8.f64};
    char function[32];
    std::snprintf(function, sizeof(function), "FCanvas@%08X", static_cast<uint32_t>(ctx.lr));
    Log(t, function, name, size_x, size_y, part, screen);
  }
  __imp__sub_82C499C8(ctx, base);
}

// DrawTile(FCanvas*, X, Y, SizeX, SizeY, U, V, SizeU, SizeV, FMaterialRenderProxy* (stack
// r1+92), hit proxy (r1+100)): material images that don't come through
// UCanvas::DrawMaterialTile (UI scenes, from sub_82C49BD0 and sub_82C4A808).
extern "C" REX_FUNC(sub_82C48EC0) {
  if (REXCVAR_GET(prompt_trace)) {
    Trace& t = T();
    std::lock_guard<std::mutex> lock(t.mutex);
    const uint32_t proxy = Load32(base, ctx.r1.u32 + 92);
    const std::string name = Owner(t, ctx, base, proxy);
    char function[32];
    std::snprintf(function, sizeof(function), "FMaterial@%08X", static_cast<uint32_t>(ctx.lr));
    const double screen[4] = {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64};
    const double part[4] = {ctx.f5.f64, ctx.f6.f64, ctx.f7.f64, ctx.f8.f64};
    Log(t, function, name, 0, 0, part, screen);
  }
  __imp__sub_82C48EC0(ctx, base);
}

// DrawTriangle2D(FCanvas*, Pos0, UV0, Pos1, UV1, Pos2, UV2 (FVector2D*: r4..r9), color (r10),
// FTexture* (stack r1+84), alpha blend (r1+92)): meters and other shapes that aren't tiles.
// Logged as the bounding box of the three corners and of their UVs.
extern "C" REX_FUNC(sub_82C49C20) {
  if (REXCVAR_GET(prompt_trace)) {
    Trace& t = T();
    std::lock_guard<std::mutex> lock(t.mutex);
    auto F = [&](uint32_t a) {
      const uint32_t v = Load32(base, a);
      float f;
      std::memcpy(&f, &v, 4);
      return double(f);
    };
    const uint32_t pos[3] = {ctx.r4.u32, ctx.r6.u32, ctx.r8.u32};
    const uint32_t uv[3] = {ctx.r5.u32, ctx.r7.u32, ctx.r9.u32};
    double px0 = 1e9, py0 = 1e9, px1 = -1e9, py1 = -1e9, u0 = 1e9, v0 = 1e9, u1 = -1e9, v1 = -1e9;
    for (int i = 0; i < 3; ++i) {
      px0 = std::min(px0, F(pos[i])), px1 = std::max(px1, F(pos[i]));
      py0 = std::min(py0, F(pos[i] + 4)), py1 = std::max(py1, F(pos[i] + 4));
      u0 = std::min(u0, F(uv[i])), u1 = std::max(u1, F(uv[i]));
      v0 = std::min(v0, F(uv[i] + 4)), v1 = std::max(v1, F(uv[i] + 4));
    }
    const uint32_t resource = Load32(base, ctx.r1.u32 + 84);
    std::string name;
    uint32_t size_x = 0, size_y = 0;
    if (const auto owner = t.resource_owner.find(resource); owner != t.resource_owner.end()) {
      name = t.texture_names[owner->second];
      size_x = Load32(base, owner->second + 200);
      size_y = Load32(base, owner->second + 204);
    } else {
      name = Owner(t, ctx, base, resource);
    }
    char function[32];
    std::snprintf(function, sizeof(function), "Triangle@%08X", static_cast<uint32_t>(ctx.lr));
    // Rounded so a meter that fills smoothly doesn't log every frame.
    auto R = [](double x) { return std::round(x * 20) / 20; };
    const double screen[4] = {std::round(px0), std::round(py0), std::round(px1 - px0), std::round(py1 - py0)};
    const double part[4] = {R(u0), R(v0), R(u1 - u0), R(v1 - v0)};
    Log(t, function, name, size_x, size_y, part, screen);
  }
  __imp__sub_82C49C20(ctx, base);
}
