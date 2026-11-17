// tbc - quick-time events on W / A / S / D (keyboard and mouse).
//
// A QTE starts with the Kismet action SeqAct_BnQuickAction calling the script event
// BnPlayerController:OnBnQuickAction(Action) through UObject::ProcessEvent (sub_828B8750,
// script_trace.cpp). Found with --script_trace_dump: the action's float at +0xEC is how long
// the prompt stays up (1.2 to 2.0 s in the first level). The button it wants is one of the four
// face buttons, shown on a diamond: Y top, X left, B right, A bottom.
//
// Takedown prompts (multi-takedowns) run natively instead; see TakedownPromptUp below.
//
// With keyboard and mouse on (--mnk_mode), for that long after a QTE starts (or while a
// takedown prompt is up), the left stick's
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
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

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
std::unordered_map<uint32_t, int> g_kind;  // UFunction -> 0 other, 1 OnBnQuickAction, 2 *Takedown
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
// code, not through OnBnQuickAction. The first probe (property lookups by name) found where
// the state lives, from each UProperty's Offset (+0x60) and ElementSize (+0x48):
//   BnTakedownBranch           +0x64 QuickActionStartTime (float), +0x68 QuickActionButton
//                              (byte), +0x6A QuickActionType (byte), +0x8C QuickActionResult
//                              (byte), +0x98 ActualQuickActionStartTime (float)
//   BnTakedownSequencePlayer   +0x48 SkilledTakedownQuickActionLength (float), +0x58
//                              TakedownRunner, +0x74 QuickActionStageIdx (int)
// UObject: +0x04 index in GObjObjects, +0x28 Outer, +0x2C Name, +0x30 Class.
// This probe finds GObjObjects, then every instance of those two classes, and logs how those
// fields change for 4 s after each QTE or takedown starts.
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

bool Heap(uint32_t a) { return a >= 0x40000000 && a < 0x80000000; }

// Whether a guest address can be read: the first probe crashed reading through a word that only
// looked like a heap pointer. Answers are kept per 4 KB page, but only for one scan (ScanBegin):
// memory is freed and mapped again all the time, and a page remembered from an earlier scan was
// how GObjObjects, once it had grown into pages that had been unmapped, read as empty (no takedown
// prompts for the rest of the session), and how a freed page could still read as safe.
std::unordered_map<uint32_t, bool> g_pages;
void ScanBegin() { g_pages.clear(); }

bool ReadableNow(uint32_t addr) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return false;
  auto* heap = runtime->memory()->LookupHeap(addr);
  uint32_t protect = 0;
  return heap && heap->QueryProtect(addr, &protect) && (protect & rex::memory::kMemoryProtectRead);
}

bool Readable(uint32_t addr) {
  const uint32_t page = addr >> 12;
  if (const auto it = g_pages.find(page); it != g_pages.end()) return it->second;
  const bool ok = ReadableNow(addr);
  g_pages.emplace(page, ok);
  return ok;
}

struct Probe {
  uint32_t objects = 0;  // address of GObjObjects {Data, Num, Max}
  uint32_t player_class = 0, branch_class = 0;
  std::vector<uint32_t> players, branches;
  std::vector<std::vector<uint8_t>> player_last, branch_last;
  int64_t until = 0;
  int lines = 0;
  // Shooting takedown (PlayerOverTheShoulder.Takedown): its prompts aren't BnTakedownBranch
  // ones, so the player controller and its pawn are watched whole, for kWideFor.
  struct Wide {
    const char* what;
    uint32_t addr = 0;
    std::vector<uint8_t> last;
    std::vector<uint8_t> changes;  // per word; a word that changes all the time stops being logged
  };
  std::vector<Wide> wide;
  uint32_t pawn_class = 0;
} g_probe;

constexpr uint32_t kWideBytes = 0x1000;
constexpr auto kWideFor = std::chrono::seconds(14);
constexpr uint8_t kWideMaxChanges = 6;

constexpr uint32_t kPlayerBytes = 0x100;
constexpr uint32_t kBranchFrom = 0x60, kBranchBytes = 0x40;  // +0x60..+0x9F

void ProbeSetup(const PPCContext& ctx, uint8_t* base) {
  Probe& p = g_probe;
  ScanBegin();
  if (!p.player_class) {
    p.player_class = FindObject(ctx, base, "BourneNative.BnTakedownSequencePlayer", 0);
    p.branch_class = FindObject(ctx, base, "BourneNative.BnTakedownBranch", 0);
    REXLOG_INFO("qte_probe: classes: sequence player {:08X}, branch {:08X}", p.player_class, p.branch_class);
  }
  // GObjObjects is the global TArray at 0x83655774 (data, count). The probe used to search
  // for it and sometimes settled on the wrong array (0 or a few objects found).
  if (!p.objects && p.player_class && p.branch_class) {
    p.objects = 0x83655774;
    REXLOG_INFO("qte_probe: GObjObjects data {:08X}, {} objects", Load32(base, p.objects), Load32(base, p.objects + 4));
  }
  if (!p.objects) return;
  const uint32_t data = Load32(base, p.objects), count = std::min<uint32_t>(Load32(base, p.objects + 4), 2000000);
  p.players.clear();
  p.branches.clear();
  for (uint32_t i = 0; i < count; ++i) {
    if ((i & 1023) == 0 && !Readable(data + 4 * i)) break;
    const uint32_t obj = Load32(base, data + 4 * i);
    if (!Heap(obj) || !Readable(obj + 0x30)) continue;
    const uint32_t cls = Load32(base, obj + 0x30);
    if (cls == p.player_class) p.players.push_back(obj);
    else if (cls == p.branch_class) p.branches.push_back(obj);
  }
  REXLOG_INFO("qte_probe: {} sequence players, {} branches", p.players.size(), p.branches.size());
  for (const uint32_t obj : p.players) {
    // Its name only when its outer chain is all live objects: the object table can still list
    // one whose outer was freed, and the game's GetPathName then reads unmapped memory (a
    // crash with qte_probe on during a weapon takedown).
    bool live = true;
    uint32_t o = obj;
    for (int depth = 0; o && depth < 16; ++depth) {
      if (!Heap(o) || !Readable(o + 0x34) || !Readable(o)) { live = false; break; }
      const uint32_t cls = Load32(base, o + 0x30);
      if (!Heap(cls) || !Readable(cls + 0x30)) { live = false; break; }
      o = Load32(base, o + 0x28);
    }
    REXLOG_INFO("qte_probe: player {:08X} {}{}", obj,
                live ? tbc::prompts::ObjectPathName(ctx, base, obj) : std::string("(outer not live)"),
                Words(base, obj, kPlayerBytes));
  }
  p.player_last.assign(p.players.size(), {});
  p.branch_last.assign(p.branches.size(), {});
  for (size_t k = 0; k < p.players.size(); ++k)
    p.player_last[k].assign(base + p.players[k], base + p.players[k] + kPlayerBytes);
  for (size_t k = 0; k < p.branches.size(); ++k)
    p.branch_last[k].assign(base + p.branches[k] + kBranchFrom, base + p.branches[k] + kBranchFrom + kBranchBytes);
}

void Diff(uint8_t* base, const char* what, uint32_t obj, uint32_t from, std::vector<uint8_t>& last) {
  Probe& p = g_probe;
  if (!ReadableNow(obj + from) || !ReadableNow(obj + from + uint32_t(last.size()) - 1)) return;
  for (uint32_t i = 0; i < last.size(); i += 4) {
    if (std::memcmp(&last[i], base + obj + from + i, 4) == 0) continue;
    // A branch's +0x88 is its play time, counting every frame: known, and it would flood the log.
    const bool clock = std::strcmp(what, "branch") == 0 && from + i == 0x88;
    if (p.lines < 6000 && !clock) {
      const uint32_t was = uint32_t(last[i]) << 24 | uint32_t(last[i + 1]) << 16 | uint32_t(last[i + 2]) << 8 | last[i + 3];
      REXLOG_INFO("qte_probe: {} {:08X} +{:03X} {:08X} -> {:08X}", what, obj, from + i, was, Load32(base, obj + from + i));
      ++p.lines;
    }
    std::memcpy(&last[i], base + obj + from + i, 4);
  }
}

// From the input thread: what changed in the watched objects since the last poll.
void ProbeDiff(uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Probe& p = g_probe;
  if (!p.until) return;
  if (Clock::now().time_since_epoch().count() >= p.until) {
    p.until = 0;
    p.wide.clear();
    REXLOG_INFO("qte_probe: end");
    return;
  }
  for (size_t k = 0; k < p.players.size(); ++k) Diff(base, "player", p.players[k], 0, p.player_last[k]);
  for (size_t k = 0; k < p.branches.size(); ++k) Diff(base, "branch", p.branches[k], kBranchFrom, p.branch_last[k]);
  for (auto& w : p.wide) {
    if (!ReadableNow(w.addr) || !ReadableNow(w.addr + kWideBytes - 1)) continue;
    for (uint32_t i = 0; i < kWideBytes; i += 4) {
      if (std::memcmp(&w.last[i], base + w.addr + i, 4) == 0) continue;
      uint8_t& n = w.changes[i / 4];
      if (n < 255) ++n;
      if (n <= kWideMaxChanges && p.lines < 6000) {
        const uint32_t was = uint32_t(w.last[i]) << 24 | uint32_t(w.last[i + 1]) << 16 |
                             uint32_t(w.last[i + 2]) << 8 | w.last[i + 3];
        const uint32_t now = Load32(base, w.addr + i);
        float fw, fn;
        std::memcpy(&fw, &was, 4);
        std::memcpy(&fn, &now, 4);
        REXLOG_INFO("qte_probe: {} +{:03X} {:08X} -> {:08X} ({:g} -> {:g}){}", w.what, i, was, now, fw, fn,
                    n == kWideMaxChanges ? " [busy: no more]" : "");
        ++p.lines;
      }
      std::memcpy(&w.last[i], base + w.addr + i, 4);
    }
  }
}

void WideSetup(const PPCContext& ctx, uint8_t* base, uint32_t controller) {  // holds g_mutex
  Probe& p = g_probe;
  p.wide.clear();
  p.lines = 0;
  auto add = [&](const char* what, uint32_t addr) {
    if (!Heap(addr) || !ReadableNow(addr) || !ReadableNow(addr + kWideBytes - 1)) return;
    Probe::Wide w;
    w.what = what;
    w.addr = addr;
    w.last.assign(base + addr, base + addr + kWideBytes);
    w.changes.assign(kWideBytes / 4, 0);
    p.wide.push_back(std::move(w));
    REXLOG_INFO("qte_probe: watching {} {:08X} ({} bytes) for {} s", what, addr, kWideBytes,
                std::chrono::duration_cast<std::chrono::seconds>(kWideFor).count());
  };
  add("controller", controller);
  if (!p.pawn_class) p.pawn_class = FindObject(ctx, base, "BourneNative.BnPlayerPawn", 0);
  if (p.pawn_class && Heap(controller)) {
    for (uint32_t i = 0; i < 0x800; i += 4) {
      const uint32_t v = Load32(base, controller + i);
      if (Heap(v) && ReadableNow(v + 0x30) && Load32(base, v + 0x30) == p.pawn_class) {
        REXLOG_INFO("qte_probe: controller +{:03X} is its pawn", i);
        add("pawn", v);
        break;
      }
    }
  }
}

// Takedown prompts (multi-takedowns). These don't go through OnBnQuickAction: the takedown's
// branches (BnTakedownBranch objects, found with --qte_probe) run them natively. While a branch
// plays, +0x88 counts its time up; when its takedown starts, +0x98 (ActualQuickActionStartTime)
// is set to -1, and to the branch time at which the prompt appears once it does (0.78 s and
// 1.72 s in the two seen). So a prompt is up while +0x98 >= 0, the branch time is within
// kPromptWindow of it, and the branch time is still moving (a takedown that ends or is cut
// short leaves a branch frozen, which mustn't keep W / A / S / D remapped).
// The branches are looked up through GObjObjects (the engine's list of every object, a global
// TArray at 0x83655774: data, count) when a takedown starts, and again every kRescan for
// kRescanFor after it: a takedown's branches can be made only once it has begun, so a single
// look at the start found none of them (every multi-takedown on keys failed in a new session).
constexpr uint32_t kObjects = 0x83655774;
constexpr float kPromptWindow = 1.5f;
constexpr auto kRescan = std::chrono::milliseconds(250);
constexpr auto kRescanFor = std::chrono::seconds(8);
uint32_t g_branch_class = 0;
// Shooting takedowns (PlayerOverTheShoulder.Takedown, prompts A / B / X over about 9 s) aren't
// played by branches but by the game's BnTakedownSequencePlayer: found with --qte_probe, its
// +0x5C holds a pointer from the takedown's start to its end (0 otherwise; no melee takedown
// set it), and its +0x74 (QuickActionStageIdx) steps 0 -> 2 -> 4 as each prompt comes up. While
// +0x5C is set, W / A / S / D answer the prompts; Bourne can't be moved then anyway.
uint32_t g_player_class = 0;
std::vector<uint32_t> g_players;
constexpr auto kShootingMax = std::chrono::seconds(15);  // a takedown left set mustn't keep the keys
Clock::time_point g_shooting_since{};
struct Branch {
  uint32_t addr;
  float last_time;
  Clock::time_point moved;
};
std::vector<Branch> g_branches;
Clock::time_point g_rescan_until{}, g_rescan_next{};

void ScanBranches(uint8_t* base) {  // holds g_mutex; g_branch_class known
  const uint32_t data = Load32(base, kObjects);
  const uint32_t count = std::min<uint32_t>(Load32(base, kObjects + 4), 2000000);
  ScanBegin();
  if (!Heap(data) || !Readable(data + 4 * Load32(base, g_branch_class + 4)) ||
      Load32(base, data + 4 * Load32(base, g_branch_class + 4)) != g_branch_class) {
    return;  // not the list we know: leave takedown prompts alone
  }
  std::vector<Branch> found;
  std::vector<uint32_t> players;
  for (uint32_t i = 0; i < count; ++i) {
    if ((i & 1023) == 0 && !Readable(data + 4 * i)) break;
    const uint32_t obj = Load32(base, data + 4 * i);
    if (!Heap(obj) || !Readable(obj + 0x30) || !Readable(obj + 0x9C)) continue;
    const uint32_t cls = Load32(base, obj + 0x30);
    if (g_player_class && cls == g_player_class) players.push_back(obj);
    if (cls != g_branch_class) continue;
    Branch b{obj, LoadFloat(base, obj + 0x88), Clock::time_point{}};
    for (const Branch& old : g_branches) {
      if (old.addr == obj) b = old;  // keep how long it's been moving
    }
    found.push_back(b);
  }
  if ((found.size() != g_branches.size() || players.size() != g_players.size()) && REXCVAR_GET(qte_wasd_log)) {
    REXLOG_INFO("qte: {} takedown branches, {} sequence players", found.size(), players.size());
  }
  g_branches = std::move(found);
  g_players = std::move(players);
}

void RefreshBranches(const PPCContext& ctx, uint8_t* base) {  // holds g_mutex
  if (!g_branch_class) {
    g_branch_class = FindObject(ctx, base, "BourneNative.BnTakedownBranch", 0);
    if (!g_branch_class) return;
  }
  if (!g_player_class) g_player_class = FindObject(ctx, base, "BourneNative.BnTakedownSequencePlayer", 0);
  ScanBranches(base);
  const auto now = Clock::now();
  g_rescan_until = now + kRescanFor;
  g_rescan_next = now + kRescan;
}

bool TakedownPromptUp(uint8_t* base) {  // holds g_mutex
  const auto now = Clock::now();
  bool up = false;
  for (Branch& b : g_branches) {
    // A branch freed since the scan: skip it rather than read freed memory.
    if (!ReadableNow(b.addr + 0x30) || !ReadableNow(b.addr + 0x9C) || Load32(base, b.addr + 0x30) != g_branch_class) {
      continue;
    }
    const float time = LoadFloat(base, b.addr + 0x88);
    if (time != b.last_time) {
      b.last_time = time;
      b.moved = now;
    }
    const float start = LoadFloat(base, b.addr + 0x98);
    if (start >= 0.0f && time >= start && time <= start + kPromptWindow &&
        now - b.moved < std::chrono::milliseconds(250)) {
      up = true;
    }
  }
  return up;
}

bool ShootingTakedownUp(uint8_t* base) {  // holds g_mutex
  bool on = false;
  for (const uint32_t pl : g_players) {
    if (!ReadableNow(pl + 0x30) || !ReadableNow(pl + 0x78) || Load32(base, pl + 0x30) != g_player_class) continue;
    if (Load32(base, pl + 0x5C) != 0) on = true;
  }
  const auto now = Clock::now();
  if (!on) {
    g_shooting_since = {};
    return false;
  }
  if (g_shooting_since == Clock::time_point{}) g_shooting_since = now;
  return now - g_shooting_since < kShootingMax;
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
        kind = name.ends_with(":OnBnQuickAction") ? 1
               : name.ends_with("OverTheShoulder.Takedown") ? 3
               : name.ends_with("Takedown") ? 2 : 0;
        it = kinds.emplace(function, kind).first;
      }
      kind = it->second;
    }
    if (kind) {
      REXLOG_INFO("qte_probe: {} on {:08X}: watching for 4 s",
                  kind == 1 ? "QTE" : kind == 3 ? "shooting takedown" : "takedown", ctx.r3.u32);
      std::lock_guard<std::mutex> lock(g_mutex);
      ProbeSetup(ctx, base);
      auto until = Clock::now() + std::chrono::seconds(4);
      if (kind == 3) {
        WideSetup(ctx, base, ctx.r3.u32);
        until = Clock::now() + kWideFor;
      }
      // A later event (another takedown call) mustn't cut a longer watch short.
      g_probe.until = std::max(g_probe.until, until.time_since_epoch().count());
    }
  }
  if (!REXCVAR_GET(qte_wasd) || !function) return;
  // With qte_wasd_log the QTE is logged on a controller too, so a keyboard run can be compared.
  const bool keyboard = KeyboardOn();
  if (!keyboard && !REXCVAR_GET(qte_wasd_log)) return;
  int kind;  // 0 other, 1 QTE (OnBnQuickAction), 2 a takedown starting
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_kind.find(function);
    if (it == g_kind.end()) {
      // Named once per UFunction (a guest call); every later call is a map lookup.
      const std::string name = tbc::prompts::ObjectPathName(ctx, base, function);
      it = g_kind.emplace(function, name.ends_with(":OnBnQuickAction") ? 1 : name.ends_with("Takedown") ? 2 : 0)
               .first;
    }
    kind = it->second;
    if (kind == 2) {
      if (keyboard) RefreshBranches(ctx, base);
      return;
    }
  }
  if (kind != 1) return;
  const uint32_t parms = ctx.r5.u32;
  const uint32_t action = parms ? Load32(base, parms) : 0;
  const bool live = action >= 0x40000000 && action < 0x80000000;
  const float raw = live ? LoadFloat(base, action + 0xEC) : 0.0f;
  float window = raw;
  if (!(window > 0.2f && window < 10.0f)) window = 2.0f;  // not the layout we know: a safe default
  if (REXCVAR_GET(qte_wasd_log)) {
    // The Kismet node that raised it (its name is stable across runs) and its class, so a QTE
    // that misbehaves can be told apart from the ones that work.
    std::string node = "?", cls = "?";
    if (live) {
      node = tbc::prompts::ObjectPathName(ctx, base, action);
      const uint32_t c = Load32(base, action + 0x30);
      if (c >= 0x40000000 && c < 0x80000000) cls = tbc::prompts::ObjectPathName(ctx, base, c);
    }
    REXLOG_INFO("qte: started, {:.1f} s (+0xEC reads {:.3f}) node {} class {}{}", window, raw, node, cls,
                keyboard ? "" : " [controller: keys not mapped]");
  }
  if (!keyboard) return;
  const auto until = Clock::now() + std::chrono::milliseconds(int((window + 0.3f) * 1000));
  g_until.store(until.time_since_epoch().count());
  g_starts.fetch_add(1);
}

void OnControllerState(uint8_t* base, uint32_t state) {
  if (REXCVAR_GET(qte_probe)) ProbeDiff(base);
  if (!REXCVAR_GET(qte_wasd) || !KeyboardOn()) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_branch_class && g_rescan_until != Clock::time_point{}) {
    const auto now = Clock::now();
    if (now >= g_rescan_until) {
      g_rescan_until = {};
    } else if (now >= g_rescan_next) {
      g_rescan_next = now + kRescan;
      ScanBranches(base);
    }
  }
  const int64_t until = g_until.load();
  const bool qte = until && Clock::now().time_since_epoch().count() < until;
  static bool was_takedown = false, was_shooting = false;
  const bool takedown = !g_branches.empty() && TakedownPromptUp(base);
  if (takedown && !was_takedown) {
    g_starts.fetch_add(1);
    if (REXCVAR_GET(qte_wasd_log)) REXLOG_INFO("qte: takedown prompt");
  }
  was_takedown = takedown;
  const bool shooting = !g_players.empty() && ShootingTakedownUp(base);
  if (shooting != was_shooting) {
    if (shooting) g_starts.fetch_add(1);
    if (REXCVAR_GET(qte_wasd_log)) REXLOG_INFO("qte: shooting takedown {}", shooting ? "started" : "over");
  }
  was_shooting = shooting;
  if (!qte && !takedown && !shooting) return;
  auto load16 = [&](uint32_t a) { return int16_t(uint16_t(base[a] << 8 | base[a + 1])); };
  const int16_t lx = load16(state + 8), ly = load16(state + 10);
  const uint8_t dirs = Directions(lx, ly);
  const uint32_t starts = g_starts.load();
  if (starts != g_seen_start) {
    g_seen_start = starts;
    g_blocked = dirs;  // held when the prompt appeared: not an answer
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
