// tbc - quick-time events on W / A / S / D (keyboard and mouse).
//
// A QTE starts with the Kismet action SeqAct_BnQuickAction calling the script event
// BnPlayerController:OnBnQuickAction(Action) through UObject::ProcessEvent (sub_828B8750,
// script_trace.cpp). Found with --script_trace_dump: the action's float at +0xEC is how long
// the prompt stays up (1.2 to 2.0 s in the first level). The button it wants is one of the four
// face buttons, shown on a diamond: Y top, X left, B right, A bottom.
//
// With keyboard and mouse on (--mnk_mode), for that long after a QTE starts, the left stick's
// four directions (W / A / S / D) press the face button in the same place on the diamond:
// W = Y, A = X, S = A, D = B, and the stick itself reads centred. Only a fresh press counts: a
// direction already held when the QTE starts (running with W) is ignored until it's let go,
// so it can't answer the QTE by accident. The usual face-button keys keep working too.
//
//   --qte_wasd=false        off
//   --qte_wasd_log=true     log each QTE and what was pressed

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "prompts.h"
#include "qte.h"

REXCVAR_DEFINE_BOOL(qte_wasd, true, "Input",
                    "Keyboard and mouse: during a quick-time event, W / A / S / D press the face "
                    "button in the same place on the prompt's diamond (Y / X / A / B)");
REXCVAR_DEFINE_BOOL(qte_wasd_log, false, "Debug", "Log quick-time events and the W / A / S / D presses");
REXCVAR_DEFINE_BOOL(qte_probe, false, "Debug",
                    "Development: find where the game keeps its quick-action state (property "
                    "lookups, and the player controller's changes for 4 s after a QTE or takedown)");

REX_EXTERN(sub_82920EE8);  // UObject::StaticFindObject(UClass*, UObject* outer, const TCHAR*, UBOOL exact)

namespace {
using Clock = std::chrono::steady_clock;

constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr int16_t kFull = 24000;  // a key moves the stick to 32767; a real stick has to be most of the way

std::atomic<int64_t> g_until{0};  // QTE active until this steady_clock tick count
std::atomic<uint32_t> g_starts{0};

std::mutex g_mutex;
std::unordered_map<uint32_t, bool> g_is_qte;  // UFunction -> is OnBnQuickAction
uint32_t g_seen_start = 0;
uint8_t g_blocked = 0;  // directions held when the QTE started: up 1, left 2, down 4, right 8

bool KeyboardOn() {
  static Clock::time_point next{};
  static bool on = false;
  const auto now = Clock::now();
  if (now >= next) {
    on = rex::cvar::Query<bool>("mnk_mode");
    next = now + std::chrono::milliseconds(500);
  }
  return on;
}

float LoadFloat(const uint8_t* base, uint32_t addr) {
  const uint32_t v = uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 |
                     uint32_t(base[addr + 2]) << 8 | base[addr + 3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 | uint32_t(base[addr + 2]) << 8 |
         base[addr + 3];
}

uint8_t Directions(int16_t lx, int16_t ly) {
  return (ly >= kFull ? 1 : 0) | (lx <= -kFull ? 2 : 0) | (ly <= -kFull ? 4 : 0) | (lx >= kFull ? 8 : 0);
}

// --qte_probe (development). Multi-takedowns run their quick actions from native takedown
// code, not through OnBnQuickAction; the strings name the state they keep (QuickActionButton,
// QuickActionStartTime, QuickActionType, QuickActionResult, QuickActionStageIdx,
// SkilledTakedownQuickActionLength). Look those properties up by name and dump them (their
// offset is in there), and log what changes in the player controller around a QTE or takedown.
std::atomic<uint32_t> g_probe_object{0};
std::atomic<int64_t> g_probe_until{0};
std::vector<uint8_t> g_probe_last;
int g_probe_lines = 0;

uint32_t FindObject(const PPCContext& ctx, uint8_t* base, const std::string& name, uint32_t outer) {
  PPCContext tmp = ctx;
  const uint32_t text = ctx.r1.u32 - 0x300;  // UTF-16BE name below the caller's frame
  for (size_t i = 0; i <= name.size() && i < 120; ++i) {
    const uint16_t c = i < name.size() ? uint16_t(name[i]) : 0;
    base[text + 2 * i] = uint8_t(c >> 8);
    base[text + 2 * i + 1] = uint8_t(c);
  }
  tmp.r1.u32 = ctx.r1.u32 - 0x600;
  tmp.r3.u64 = 0;
  tmp.r4.u64 = outer;
  tmp.r5.u64 = text;
  tmp.r6.u64 = 0;
  sub_82920EE8(tmp, base);
  return tmp.r3.u32;
}

std::string Words(const uint8_t* base, uint32_t addr, uint32_t size) {
  std::string out;
  char buf[24];
  for (uint32_t i = 0; i < size; i += 4) {
    if (i % 32 == 0) {
      std::snprintf(buf, sizeof(buf), "\n    +%03X", i);
      out += buf;
    }
    std::snprintf(buf, sizeof(buf), " %08X", Load32(base, addr + i));
    out += buf;
  }
  return out;
}

void ProbeProperties(const PPCContext& ctx, uint8_t* base) {
  static bool done = false;
  if (done) return;
  done = true;
  const char* props[] = {"QuickActionButton", "QuickActionStartTime", "QuickActionType", "QuickActionResult",
                         "QuickActionStageIdx", "ActualQuickActionStartTime", "SkilledTakedownQuickActionLength",
                         "CurrentTakedown", "TakedownRunner"};
  const char* classes[] = {"BourneNative.BnPlayerController", "BourneNative.BnPlayerPawn",
                           "BourneNative.BnPlayerFightingComponent", "BourneNative.BnTakedownRunner",
                           "BourneNative.BnTakedowner", "BourneNative.BnTakedownSequencePlayer",
                           "BourneNative.BnTakedownSequence", "BourneNative.SeqAct_BnQuickAction",
                           "BourneNative.BnTakedownBranch", "BourneNative.BnFightingComponent"};
  for (const char* prop : props) {
    bool found = false;
    for (const char* cls : classes) {
      const std::string path = std::string(cls) + "." + prop;
      if (const uint32_t obj = FindObject(ctx, base, path, 0)) {
        found = true;
        REXLOG_INFO("qte_probe: {} = {:08X} {}{}", path, obj, tbc::prompts::ObjectPathName(ctx, base, obj),
                    Words(base, obj, 0x80));
      }
    }
    if (!found) {
      const uint32_t obj = FindObject(ctx, base, prop, 0xFFFFFFFF);  // ANY_PACKAGE
      REXLOG_INFO("qte_probe: {} not in the listed classes; any package: {:08X} {}", prop, obj,
                  obj ? tbc::prompts::ObjectPathName(ctx, base, obj) : std::string("-"));
      if (obj) REXLOG_INFO("qte_probe: {}{}", prop, Words(base, obj, 0x80));
    }
  }
}

// From the input thread: what changed in the probed object since the last poll.
void ProbeDiff(uint8_t* base) {
  const int64_t until = g_probe_until.load();
  const uint32_t object = g_probe_object.load();
  if (!until || !object) return;
  constexpr uint32_t kSize = 0x1000;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (Clock::now().time_since_epoch().count() >= until) {
    g_probe_until.store(0);
    g_probe_last.clear();
    REXLOG_INFO("qte_probe: end");
    return;
  }
  if (g_probe_last.size() != kSize) {
    g_probe_last.assign(base + object, base + object + kSize);
    return;
  }
  for (uint32_t i = 0; i < kSize && g_probe_lines < 4000; i += 4) {
    if (std::memcmp(&g_probe_last[i], base + object + i, 4) != 0) {
      const uint32_t was = uint32_t(g_probe_last[i]) << 24 | uint32_t(g_probe_last[i + 1]) << 16 |
                           uint32_t(g_probe_last[i + 2]) << 8 | g_probe_last[i + 3];
      REXLOG_INFO("qte_probe: +{:03X} {:08X} -> {:08X}", i, was, Load32(base, object + i));
      ++g_probe_lines;
    }
  }
  std::memcpy(g_probe_last.data(), base + object, kSize);
}
}  // namespace

namespace tbc::qte {

void OnProcessEvent(const PPCContext& ctx, uint8_t* base, uint32_t function) {
  if (REXCVAR_GET(qte_probe) && function) {
    static std::unordered_map<uint32_t, int> kinds;  // 0 other, 1 QTE, 2 takedown
    int kind;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      auto it = kinds.find(function);
      if (it == kinds.end()) {
        const std::string name = tbc::prompts::ObjectPathName(ctx, base, function);
        kind = name.ends_with(":OnBnQuickAction") ? 1 : name.ends_with("Takedown") ? 2 : 0;
        it = kinds.emplace(function, kind).first;
      }
      kind = it->second;
    }
    if (kind) {
      ProbeProperties(ctx, base);
      const uint32_t object = ctx.r3.u32;
      REXLOG_INFO("qte_probe: {} on {:08X}: watching it for 4 s", kind == 1 ? "QTE" : "takedown", object);
      std::lock_guard<std::mutex> lock(g_mutex);
      g_probe_object.store(object);
      g_probe_last.clear();
      g_probe_until.store((Clock::now() + std::chrono::seconds(4)).time_since_epoch().count());
    }
  }
  if (!REXCVAR_GET(qte_wasd) || !function || !KeyboardOn()) return;
  bool is_qte;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_is_qte.find(function);
    if (it != g_is_qte.end()) {
      is_qte = it->second;
    } else {
      is_qte = false;
    }
    if (it != g_is_qte.end() && !is_qte) return;
    if (it == g_is_qte.end()) {
      // Named once per UFunction (a guest call); every later call is a map lookup.
      is_qte = tbc::prompts::ObjectPathName(ctx, base, function).ends_with(":OnBnQuickAction");
      g_is_qte.emplace(function, is_qte);
      if (!is_qte) return;
    }
  }
  const uint32_t parms = ctx.r5.u32;
  const uint32_t action = parms ? Load32(base, parms) : 0;
  float window = action >= 0x40000000 && action < 0x80000000 ? LoadFloat(base, action + 0xEC) : 0.0f;
  if (!(window > 0.2f && window < 10.0f)) window = 2.0f;  // not the layout we know: a safe default
  const auto until = Clock::now() + std::chrono::milliseconds(int((window + 0.3f) * 1000));
  g_until.store(until.time_since_epoch().count());
  g_starts.fetch_add(1);
  if (REXCVAR_GET(qte_wasd_log)) {
    REXLOG_INFO("qte: started, {:.1f} s (action {:08X})", window, action);
  }
}

void OnControllerState(uint8_t* base, uint32_t state) {
  if (REXCVAR_GET(qte_probe)) ProbeDiff(base);
  if (!REXCVAR_GET(qte_wasd)) return;
  const int64_t until = g_until.load();
  if (!until || Clock::now().time_since_epoch().count() >= until || !KeyboardOn()) return;
  auto load16 = [&](uint32_t a) { return int16_t(uint16_t(base[a] << 8 | base[a + 1])); };
  const int16_t lx = load16(state + 8), ly = load16(state + 10);
  const uint8_t dirs = Directions(lx, ly);
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint32_t starts = g_starts.load();
  if (starts != g_seen_start) {
    g_seen_start = starts;
    g_blocked = dirs;  // held when the QTE began: not an answer
  }
  g_blocked &= dirs;  // let go: from now on it counts
  const uint8_t fresh = dirs & ~g_blocked;
  uint16_t buttons = uint16_t(base[state + 4] << 8 | base[state + 5]);
  const uint16_t before = buttons;
  if (fresh & 1) buttons |= kY;
  if (fresh & 2) buttons |= kX;
  if (fresh & 4) buttons |= kA;
  if (fresh & 8) buttons |= kB;
  base[state + 4] = uint8_t(buttons >> 8);
  base[state + 5] = uint8_t(buttons);
  // The stick reads centred during the QTE, so the keys don't also walk Bourne.
  std::memset(base + state + 8, 0, 4);
  if (buttons != before && REXCVAR_GET(qte_wasd_log)) {
    REXLOG_INFO("qte: W/A/S/D -> buttons {:04X}", buttons);
  }
}

}  // namespace tbc::qte
