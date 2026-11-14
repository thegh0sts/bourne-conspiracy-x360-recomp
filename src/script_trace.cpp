// tbc - the game's UnrealScript events, by name (development tool, and the base for detecting
// what's on screen: QTEs, fights, menus).
//
// UObject::ProcessEvent (sub_828B8750) is how the engine runs a script function from C++:
// every event (Touch, Timer, BeginState, the input events, Kismet actions, ...) goes through
// it. r3 = the object, r4 = the UFunction, r5 = its parameters, r6 = the return value. Found
// as the one function with a runtime-sized stack frame (alloca of UFunction +72,
// PropertiesSize) that 1,143 vtables share: ProcessEvent is virtual and nearly every class
// inherits it. Its body matches UE3's: the function flags (+128) and probe-mask checks, the
// remote-call path (vtable +252), ++ScriptEntryTag (0x835CC908), then an FFrame on the
// alloca'd locals, the parameters copied in (ParmsSize, +142) and the rest zeroed.
//
//   --script_trace=true            log each script function the first time it runs, with the
//                                  object it ran on, numbered in order: do something in the
//                                  game (a QTE) and the functions first seen then are its own
//   --script_trace_filter=Qte|Quick   also log every call of functions whose name contains one
//                                  of these (case-sensitive), at most 50 lines a second
//   --script_trace_dump=OnBnQuickAction   for functions whose name contains one of these, also
//                                  log the parameters (UFunction +142 = ParmsSize) and, when the
//                                  first parameter is an object (Kismet handlers get their
//                                  action: OnX(SeqAct_X Action)), its name and first 0x200 bytes

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "prompts.h"

REXCVAR_DEFINE_BOOL(script_trace, false, "Debug",
                    "Log each UnrealScript event the first time it runs (ProcessEvent); "
                    "development only");
REXCVAR_DEFINE_STRING(script_trace_filter, "", "Debug",
                      "Also log every call of script events whose name contains one of these "
                      "(separated by |); development only");

REXCVAR_DEFINE_STRING(script_trace_dump, "", "Debug",
                      "For script events whose name contains one of these (separated by |), "
                      "log their parameters and the object passed first; development only");

REX_EXTERN(__imp__sub_828B8750);  // UObject::ProcessEvent(UFunction*, void* Parms, void* Result)

namespace {

struct Trace {
  std::mutex mutex;
  std::unordered_map<uint32_t, std::string> names;  // UFunction -> path name
  uint32_t first_seen = 0;
  std::string filter_text;
  std::vector<std::string> filter;
  std::chrono::steady_clock::time_point second{};
  int lines_this_second = 0;
};

Trace& T() {
  static Trace t;
  return t;
}

std::vector<std::string> Split(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = std::min(text.find('|', start), text.size());
    if (end > start) out.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return uint32_t(base[addr]) << 24 | uint32_t(base[addr + 1]) << 16 | uint32_t(base[addr + 2]) << 8 |
         base[addr + 3];
}

std::string Hex(const uint8_t* base, uint32_t addr, uint32_t size) {
  std::string out;
  char buf[16];
  for (uint32_t i = 0; i < size; i += 4) {
    if (i % 32 == 0) {
      std::snprintf(buf, sizeof(buf), "%s    +%03X", i ? "\n" : "", i);
      out += buf;
    }
    std::snprintf(buf, sizeof(buf), " %08X", Load32(base, addr + i));
    out += buf;
  }
  return out;
}

// Parameters of a matching event, and the object passed first (see --script_trace_dump).
void Dump(PPCContext& ctx, uint8_t* base, const std::string& name, uint32_t object, uint32_t function) {
  static int dumps = 0;
  if (dumps >= 40) return;
  ++dumps;
  const uint32_t parms = ctx.r5.u32;
  const uint32_t size = std::min<uint32_t>(uint32_t(base[function + 142]) << 8 | base[function + 143], 64);
  REXLOG_INFO("script_trace dump: {} on {:08X} parms {:08X} ({} bytes)\n{}", name, object, parms, size,
              parms && size ? Hex(base, parms, (size + 3) & ~3u) : std::string("    (none)"));
  if (!parms || size < 4) return;
  const uint32_t first = Load32(base, parms);
  if (first < 0x40000000 || first >= 0x80000000) return;
  const std::string first_name = tbc::prompts::ObjectPathName(ctx, base, first);
  REXLOG_INFO("script_trace dump: first parameter {:08X} = {}\n{}", first, first_name, Hex(base, first, 0x200));
}

}  // namespace

extern "C" REX_FUNC(sub_828B8750) {
  const bool trace = REXCVAR_GET(script_trace);
  const std::string& filter_text = REXCVAR_GET(script_trace_filter);
  const uint32_t object = ctx.r3.u32;
  const uint32_t function = ctx.r4.u32;
  const std::string& dump_text = REXCVAR_GET(script_trace_dump);
  if ((!trace && filter_text.empty() && dump_text.empty()) || !object || !function) {
    __imp__sub_828B8750(ctx, base);
    return;
  }
  Trace& t = T();
  std::string name;
  bool known = false;
  {
    std::lock_guard<std::mutex> lock(t.mutex);
    if (const auto it = t.names.find(function); it != t.names.end()) {
      name = it->second;
      known = true;
    }
    if (filter_text != t.filter_text) {
      t.filter_text = filter_text;
      t.filter = Split(filter_text);
    }
  }
  if (!known) {
    // GetPathName runs as a guest call on a copy of the registers (prompt_trace.cpp).
    name = tbc::prompts::ObjectPathName(ctx, base, function);
    const std::string on = tbc::prompts::ObjectPathName(ctx, base, object);
    std::lock_guard<std::mutex> lock(t.mutex);
    if (t.names.emplace(function, name).second && trace) {
      REXLOG_INFO("script_trace: new #{}: {} on {}", ++t.first_seen, name, on);
    }
  } else {
    std::lock_guard<std::mutex> lock(t.mutex);
    for (const std::string& part : t.filter) {
      if (name.find(part) == std::string::npos) continue;
      const auto now = std::chrono::steady_clock::now();
      if (now - t.second > std::chrono::seconds(1)) {
        t.second = now;
        t.lines_this_second = 0;
      }
      if (++t.lines_this_second <= 50) {
        REXLOG_INFO("script_trace: {} on {:08X}", name, object);
      }
      break;
    }
  }
  if (!dump_text.empty()) {
    for (const std::string& part : Split(dump_text)) {
      if (name.find(part) != std::string::npos) {
        Dump(ctx, base, name, object, function);
        break;
      }
    }
  }
  __imp__sub_828B8750(ctx, base);
}
