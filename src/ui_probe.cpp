// tbc - --ui_probe (development): the menus' widgets, for mouse control in menus.
//
// Bourne's menus are Unreal Engine 3 UI scenes: widgets (UIScreenObject and subclasses:
// UILabelButton, UIList, UISlider...) that know their rectangle on screen (RenderBounds) and
// their states (StateStack: UIState_Enabled, UIState_Focused, UIState_Pressed...). To click
// them with the mouse we need to find the ones on screen, where they are, and which has focus.
//
// Every half second this lists every UI widget (any object whose class descends from
// Engine.UIScreenObject, by reflection as in camera_probe.cpp) with a non-empty RenderBounds,
// and logs it to logs\ui_probe.txt whenever its rectangle or states change: its full name,
// class, rectangle (left, top, right, bottom, in screen pixels) and states. The first time, it
// also lists UIScreenObject's and UIObject's properties with their offsets.
//
//   --ui_probe=true

#include "generated/default/tbc_pch.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

REXCVAR_DEFINE_BOOL(ui_probe, false, "Debug",
                    "Development: log the menus' widgets, rectangles and states (logs\\ui_probe.txt)");

namespace {

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 | uint32_t(base[addr + 2]) << 8 |
         base[addr + 3];
}
float LoadF(const uint8_t* base, uint32_t addr) {
  const uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
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

bool IsObject(uint32_t a) { return a >= 0x40000000 && a < 0xE0000000 && (a & 3) == 0 && Readable(a) && Readable(a + 0x40); }

struct ClassInfo {
  std::string name;
  bool ui = false;  // descends from Engine.UIScreenObject
};

struct Probe {
  std::mutex mutex;
  FILE* file = nullptr;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next{};
  std::unordered_map<uint32_t, std::string> names;
  std::unordered_map<uint32_t, ClassInfo> classes;
  std::unordered_map<uint32_t, std::string> last;  // widget -> last logged summary
  uint32_t render_bounds = 0, state_stack = 0;      // property offsets, once found
  bool props_listed = false;
};

Probe& P() {
  static Probe p;
  return p;
}

const std::string& Name(Probe& p, const PPCContext& ctx, uint8_t* base, uint32_t object) {
  auto it = p.names.find(object);
  if (it == p.names.end()) it = p.names.emplace(object, tbc::prompts::ObjectPathName(ctx, base, object)).first;
  return it->second;
}

std::vector<uint32_t> ClassChain(uint8_t* base, uint32_t cls) {
  std::vector<uint32_t> chain;
  while (cls && IsObject(cls) && chain.size() < 24) {
    chain.push_back(cls);
    const uint32_t meta = Load32(base, cls + 0x30);
    uint32_t super = 0;
    for (uint32_t off = 0x3C; off <= 0x90 && !super; off += 4) {
      const uint32_t q = Load32(base, cls + off);
      if (q != cls && IsObject(q) && Load32(base, q + 0x30) == meta) super = q;
    }
    cls = super;
  }
  return chain;
}

const ClassInfo& Class(Probe& p, const PPCContext& ctx, uint8_t* base, uint32_t cls) {
  auto it = p.classes.find(cls);
  if (it != p.classes.end()) return it->second;
  ClassInfo info;
  info.name = Name(p, ctx, base, cls);
  for (uint32_t c : ClassChain(base, cls)) {
    if (Name(p, ctx, base, c) == "Engine.UIScreenObject") {
      info.ui = true;
      break;
    }
  }
  return p.classes.emplace(cls, std::move(info)).first->second;
}

FILE* File(Probe& p) {
  if (!p.file) {
    std::filesystem::path dir = "logs";
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) dir = std::filesystem::path(buf).parent_path() / "logs";
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    p.file = std::fopen((dir / "ui_probe.txt").string().c_str(), "w");
  }
  return p.file;
}

// UIScreenObject's and UIObject's properties (each property object's Outer is its class):
// lists them once, and finds RenderBounds and StateStack.
void FindProps(Probe& p, const PPCContext& ctx, uint8_t* base, uint32_t data, uint32_t count) {
  FILE* f = File(p);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = Load32(base, data + 4 * i);
    if (!IsObject(o)) continue;
    const uint32_t cls = Load32(base, o + 0x30);
    if (!IsObject(cls)) continue;
    const std::string& c = Class(p, ctx, base, cls).name;
    if (c.size() < 8 || c.compare(c.size() - 8, 8, "Property") != 0) continue;
    const uint32_t outer = Load32(base, o + 0x28);
    if (!IsObject(outer)) continue;
    const std::string& owner = Name(p, ctx, base, outer);
    if (owner != "Engine.UIScreenObject" && owner != "Engine.UIObject" && owner != "Engine.UIScene") continue;
    const std::string name = Name(p, ctx, base, o);
    const uint32_t off = Load32(base, o + 0x60), size = Load32(base, o + 0x48), dim = Load32(base, o + 0x44);
    if (f) std::fprintf(f, "# +%03X %3u x%u %-22s %s\n", off, size, dim, c.c_str(), name.c_str());
    if (name.ends_with(":RenderBounds") || name.ends_with(".RenderBounds")) p.render_bounds = off;
    if (name.ends_with(":StateStack") || name.ends_with(".StateStack")) p.state_stack = off;
  }
  if (f) {
    std::fprintf(f, "# RenderBounds +%03X, StateStack +%03X\n", p.render_bounds, p.state_stack);
    std::fflush(f);
  }
}

}  // namespace

namespace tbc::ui {

// Every controller poll (input_trace.cpp).
void Probe(const PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(ui_probe)) return;
  ::Probe& p = P();
  std::lock_guard<std::mutex> lock(p.mutex);
  const auto now = std::chrono::steady_clock::now();
  if (now < p.next) return;
  p.next = now + std::chrono::milliseconds(500);
  const uint32_t data = Load32(base, 0x83655774), count = Load32(base, 0x83655778);
  if (!Readable(data) || count > 2000000) return;
  if (!p.props_listed) {
    p.props_listed = true;
    FindProps(p, ctx, base, data, count);
  }
  if (!p.render_bounds) return;
  FILE* f = File(p);
  if (!f) return;
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.start).count();
  bool any = false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = Load32(base, data + 4 * i);
    if (!IsObject(o) || !Readable(o + p.render_bounds + 16)) continue;
    const uint32_t cls = Load32(base, o + 0x30);
    if (!IsObject(cls)) continue;
    const ClassInfo& ci = Class(p, ctx, base, cls);
    if (!ci.ui) continue;
    const float l = LoadF(base, o + p.render_bounds), t = LoadF(base, o + p.render_bounds + 4);
    const float r = LoadF(base, o + p.render_bounds + 8), b = LoadF(base, o + p.render_bounds + 12);
    if (!(r > l + 1 && b > t + 1 && r - l < 5000 && b - t < 5000)) continue;
    std::string states;
    if (p.state_stack && Readable(o + p.state_stack + 8)) {
      const uint32_t arr = Load32(base, o + p.state_stack), n = Load32(base, o + p.state_stack + 4);
      for (uint32_t k = 0; IsObject(arr) && k < n && k < 8; ++k) {
        const uint32_t st = Load32(base, arr + 4 * k);
        if (!IsObject(st)) continue;
        std::string sc = Class(p, ctx, base, Load32(base, st + 0x30)).name;
        if (const size_t dot = sc.rfind('_'); dot != std::string::npos) sc = sc.substr(dot + 1);
        states += (states.empty() ? "" : ",") + sc;
      }
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%.0f,%.0f,%.0f,%.0f\t%s", l, t, r, b, states.c_str());
    std::string summary = buf;
    auto& last = p.last[o];
    if (last == summary) continue;
    last = summary;
    const std::string& name = Name(p, ctx, base, o);
    if (name.find("Default__") != std::string::npos) continue;
    std::fprintf(f, "%lld\t%08X\t%s\t%s\t%s\n", ms, o, ci.name.c_str(), summary.c_str(), name.c_str());
    any = true;
  }
  if (any) std::fflush(f);
}

}  // namespace tbc::ui
