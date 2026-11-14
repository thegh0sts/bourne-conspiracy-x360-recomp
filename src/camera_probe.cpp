// tbc - --camera_probe (development): which values turn the camera.
//
// For mouse look (the mouse turning the camera directly, not through the right stick's dead
// zone and acceleration) we need to know where Bourne keeps the camera's yaw and pitch, and
// where it reads the stick's turn input. This finds the player controller
// (BourneNative.BnPlayerController) and the objects its PlayerInput, PlayerCamera and Pawn
// properties point to, lists every property of their classes (found by reflection: each
// *Property object whose Outer is one of those classes; UProperty +0x44 ArrayDim, +0x48
// ElementSize, +0x60 Offset), and every 100 ms logs each property whose value changed, with
// the right stick at that moment, to logs\camera_probe.txt. Stand still and turn the camera
// with the stick (left, right, up, down): what changes with it is the camera.
//
// A class's parent (SuperField) is found as the first word of the class object (+0x3C..+0x90)
// pointing to another object of the same class (Core.Class).
//
//   --camera_probe=true

#include "generated/default/tbc_pch.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

REXCVAR_DEFINE_BOOL(camera_probe, false, "Debug",
                    "Development: log which player controller / input / camera values change "
                    "(logs\\camera_probe.txt)");

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

bool IsObject(uint32_t a) { return a >= 0x40000000 && a < 0xE0000000 && (a & 3) == 0 && Readable(a) && Readable(a + 0x40); }

struct Prop {
  std::string name;  // "BnPlayerInput.SomeYaw"
  std::string type;  // "FloatProperty"
  uint32_t offset, size;
};

struct Watched {
  std::string what;  // "controller", "input", ...
  uint32_t object = 0;
  std::vector<Prop> props;
  std::vector<uint8_t> last;
};

struct Probe {
  std::mutex mutex;
  FILE* file = nullptr;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next{}, next_find{};
  std::unordered_map<uint32_t, std::string> names;  // object -> path name
  std::vector<Watched> watched;
  uint32_t controller = 0;
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
  while (cls && IsObject(cls) && chain.size() < 16) {
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
    p.file = std::fopen((dir / "camera_probe.txt").string().c_str(), "w");
  }
  return p.file;
}

// The objects worth watching, from the controller: itself, its PlayerInput (+0x408), its
// camera (PlayerCamera) and the camera's active strategy (HmCamera +0x350), that strategy's
// CameraState (+0xE4) and AnchorPoint (+0xE8) and its behaviours (+0xF8, an array), and the
// current look scales (HmPlayerController +0x554).
std::vector<std::pair<std::string, uint32_t>> Targets(uint8_t* base, uint32_t controller, const std::vector<Prop>& props) {
  std::vector<std::pair<std::string, uint32_t>> out;
  auto add = [&](const std::string& what, uint32_t o) {
    if (!IsObject(o)) return;
    for (const auto& t : out)
      if (t.second == o) return;
    out.emplace_back(what, o);
  };
  add("controller", controller);
  add("input", Load32(base, controller + 0x408));
  add("lookscales", Load32(base, controller + 0x554));
  uint32_t camera = 0;
  for (const Prop& pr : props)
    if (pr.name == "PlayerController:PlayerCamera") camera = Load32(base, controller + pr.offset);
  add("camera", camera);
  if (IsObject(camera)) {
    const uint32_t strategy = Load32(base, camera + 0x350);
    add("strategy", strategy);
    if (IsObject(strategy)) {
      add("strategy.state", Load32(base, strategy + 0xE4));
      add("strategy.anchor", Load32(base, strategy + 0xE8));
      const uint32_t data = Load32(base, strategy + 0xF8), count = Load32(base, strategy + 0xFC);
      for (uint32_t i = 0; IsObject(data) && i < count && i < 12; ++i)
        add("strategy.behavior" + std::to_string(i), Load32(base, data + 4 * i));
    }
  }
  return out;
}

void Find(Probe& p, const PPCContext& ctx, uint8_t* base) {
  const uint32_t data = Load32(base, 0x83655774), count = Load32(base, 0x83655778);
  if (!Readable(data) || count > 2000000) return;
  static std::unordered_map<uint32_t, std::string> class_names;
  auto class_name = [&](uint32_t cls) -> const std::string& {
    auto it = class_names.find(cls);
    if (it == class_names.end()) it = class_names.emplace(cls, Name(p, ctx, base, cls)).first;
    return it->second;
  };
  static std::unordered_map<uint32_t, std::vector<uint32_t>> by_outer;  // properties by class
  const bool controller_ok = IsObject(p.controller) && IsObject(Load32(base, p.controller + 0x30)) &&
                             class_name(Load32(base, p.controller + 0x30)) == "BourneNative.BnPlayerController";
  if (!controller_ok) {
    p.controller = 0;
    p.watched.clear();
    for (uint32_t i = 0; i < count && !p.controller; ++i) {
      const uint32_t o = Load32(base, data + 4 * i);
      if (!IsObject(o)) continue;
      const uint32_t cls = Load32(base, o + 0x30);
      if (!IsObject(cls) || class_name(cls) != "BourneNative.BnPlayerController") continue;
      if (Name(p, ctx, base, o).find("Default__") != std::string::npos) continue;
      p.controller = o;
    }
    if (!p.controller) return;
    by_outer.clear();
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t o = Load32(base, data + 4 * i);
      if (!IsObject(o)) continue;
      const uint32_t cls = Load32(base, o + 0x30);
      if (!IsObject(cls)) continue;
      const std::string& c = class_name(cls);
      if (c.size() < 8 || c.compare(c.size() - 8, 8, "Property") != 0) continue;
      by_outer[Load32(base, o + 0x28)].push_back(o);
    }
  }
  auto props_of = [&](uint32_t object) {
    std::vector<Prop> props;
    for (uint32_t cls : ClassChain(base, Load32(base, object + 0x30))) {
      const auto it = by_outer.find(cls);
      if (it == by_outer.end()) continue;
      for (uint32_t prop : it->second) {
        const uint32_t dim = Load32(base, prop + 0x44), size = Load32(base, prop + 0x48), off = Load32(base, prop + 0x60);
        if (!dim || dim > 64 || !size || size > 256 || off > 0x4000) continue;
        std::string name = Name(p, ctx, base, prop);
        if (const size_t dot = name.find('.'); dot != std::string::npos) name = name.substr(dot + 1);
        props.push_back({name, class_name(Load32(base, prop + 0x30)), off, std::min<uint32_t>(dim * size, 64)});
      }
    }
    return props;
  };
  if (p.watched.empty()) p.watched.push_back(Watched{"controller", p.controller, props_of(p.controller), {}});
  const auto targets = Targets(base, p.controller, p.watched[0].props);
  FILE* f = File(p);
  std::vector<Watched> next;
  for (const auto& [what, object] : targets) {
    bool kept = false;
    for (Watched& w : p.watched) {
      if (w.object == object) {
        w.what = what;
        next.push_back(std::move(w));
        kept = true;
        break;
      }
    }
    if (kept) continue;
    Watched w{what, object, props_of(object), {}};
    if (f) {
      std::fprintf(f, "# %s %08X %s (%s), %zu properties\n", what.c_str(), object, Name(p, ctx, base, object).c_str(),
                   class_name(Load32(base, object + 0x30)).c_str(), w.props.size());
      for (const Prop& pr : w.props)
        std::fprintf(f, "#   +%03X %-3u %-18s %s\n", pr.offset, pr.size, pr.type.c_str(), pr.name.c_str());
    }
    next.push_back(std::move(w));
  }
  p.watched = std::move(next);
  if (f) std::fflush(f);
}

}  // namespace

namespace tbc::camera {

// Every controller poll (input_trace.cpp): rx / ry are the right stick the game is given.
void Probe(const PPCContext& ctx, uint8_t* base, int16_t rx, int16_t ry) {
  if (!REXCVAR_GET(camera_probe)) return;
  ::Probe& p = P();
  std::lock_guard<std::mutex> lock(p.mutex);
  const auto now = std::chrono::steady_clock::now();
  if (now < p.next) return;
  p.next = now + std::chrono::milliseconds(100);
  if (now >= p.next_find) {
    p.next_find = now + std::chrono::seconds(1);
    Find(p, ctx, base);
  }
  FILE* f = File(p);
  if (!f) return;
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.start).count();
  bool any = false;
  for (Watched& w : p.watched) {
    if (!IsObject(w.object)) continue;
    std::vector<uint8_t> cur(0x4000 + 64);
    for (const Prop& pr : w.props) std::memcpy(&cur[pr.offset], base + w.object + pr.offset, pr.size);
    if (w.last.empty()) {
      w.last = cur;
      continue;
    }
    for (const Prop& pr : w.props) {
      if (pr.type.find("ObjectProperty") != std::string::npos || pr.type.find("NameProperty") != std::string::npos ||
          pr.type.find("StrProperty") != std::string::npos || pr.type.find("ArrayProperty") != std::string::npos)
        continue;
      if (std::memcmp(&cur[pr.offset], &w.last[pr.offset], pr.size) == 0) continue;
      if (pr.type == "Core.BoolProperty" || pr.name.find("Timer") != std::string::npos ||
          pr.name.find("SightCounter") != std::string::npos || pr.name.find("OverlapTag") != std::string::npos ||
          pr.name.find("ObjectFlags") != std::string::npos)
        continue;
      std::string vals;
      char buf[64];
      for (uint32_t k = 0; k + 4 <= pr.size && k < 24; k += 4) {
        const uint32_t a = Load32(w.last.data(), pr.offset + k), b = Load32(cur.data(), pr.offset + k);
        if (a == b) continue;
        float fa, fb;
        std::memcpy(&fa, &a, 4);
        std::memcpy(&fb, &b, 4);
        std::snprintf(buf, sizeof(buf), " [+%u] %d->%d (%g->%g)", k, int32_t(a), int32_t(b), fa, fb);
        vals += buf;
      }
      if (pr.size < 4) {
        std::snprintf(buf, sizeof(buf), " %u->%u", w.last[pr.offset], cur[pr.offset]);
        vals += buf;
      }
      std::fprintf(f, "%lld\tstick %6d %6d\t%s +%03X %s:%s\n", ms, rx, ry, w.what.c_str(), pr.offset, pr.name.c_str(),
                   vals.c_str());
      any = true;
    }
    w.last = std::move(cur);
  }
  if (any) std::fflush(f);
}

}  // namespace tbc::camera
