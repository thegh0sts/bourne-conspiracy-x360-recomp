// tbc - developer tools for finding code the recompiler missed. See debug_tools.h.
//
// From WetRecomp by nikolaygorb (MIT), src/debug_tools.h.

#include "debug_tools.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(dev_debug_runtime, false, "Debug",
                    "Log missing functions and stub them (logs\\missed_functions.txt, "
                    "logs\\stub_sweep.txt); development only");

namespace {
std::filesystem::path LogDir() {
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
  return dir;
}

FILE* g_stub_log = nullptr;
std::mutex g_stub_mutex;
std::unordered_map<uint32_t, uint32_t> g_stub_hits;

// Called through the function table in place of a missing function: CTR holds the address
// the game branched to (indirect calls go through it).
void StubFunction(PPCContext& ctx, uint8_t*) {
  const uint32_t addr = ctx.ctr.u32;
  std::lock_guard<std::mutex> lock(g_stub_mutex);
  uint32_t& count = g_stub_hits[addr];
  if (count++ == 0 && g_stub_log) {
    std::fprintf(g_stub_log, "[stub] addr=0x%08X LR=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
                 addr, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                 ctx.r6.u32);
    std::fflush(g_stub_log);
  }
}
}  // namespace

namespace tbc::debug {
void RunIfEnabled(uint32_t code_base, uint32_t code_size) {
  if (!REXCVAR_GET(dev_debug_runtime)) {
    return;
  }
  auto* rt = rex::Runtime::instance();
  auto* fd = rt ? rt->function_dispatcher() : nullptr;
  if (!fd || code_size == 0) {
    return;
  }
  const auto dir = LogDir();
  const uint32_t end = code_base + code_size;

  // 1. Missing function scan.
  uint32_t registered = 0, missing = 0;
  if (FILE* log = std::fopen((dir / "missed_functions.txt").string().c_str(), "w")) {
    for (uint32_t addr = code_base; addr < end; addr += 4) {
      if (fd->GetFunction(addr)) {
        ++registered;
      } else {
        ++missing;
        std::fprintf(log, "[missed] addr=0x%08X\n", addr);
      }
    }
    std::fprintf(log, "=== scan: scanned=%u registered=%u missed=%u ===\n", (end - code_base) / 4,
                 registered, missing);
    std::fclose(log);
  }

  // 2. Stub sweep.
  g_stub_log = std::fopen((dir / "stub_sweep.txt").string().c_str(), "w");
  uint32_t stubbed = 0;
  for (uint32_t addr = code_base; addr < end; addr += 4) {
    if (!fd->GetFunction(addr)) {
      fd->SetFunction(addr, StubFunction);
      ++stubbed;
    }
  }
  if (g_stub_log) {
    std::fprintf(g_stub_log, "=== stub sweep: %08X-%08X, stubbed %u addresses ===\n", code_base, end,
                 stubbed);
    std::fflush(g_stub_log);
  }
  REXLOG_WARN("dev_debug_runtime: {} code addresses without a function, stubbed; see {}", stubbed,
              (dir / "stub_sweep.txt").string());
}
}  // namespace tbc::debug
