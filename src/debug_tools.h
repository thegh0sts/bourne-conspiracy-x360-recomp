// tbc - developer tools for finding code the recompiler missed.
#pragma once

#include <cstdint>

namespace tbc::debug {
// With --dev_debug_runtime=true, after setup:
//  - logs\missed_functions.txt lists every 4-byte code address with no recompiled function;
//  - every one of those gets a stub that logs its first call (address, caller LR, r3-r6) to
//    logs\stub_sweep.txt and returns, instead of the runtime's "call to invalid function"
//    abort. One play session then finds every missing function the game actually reaches.
// The stubs do nothing, so the game may misbehave after one is hit: development only.
void RunIfEnabled(uint32_t code_base, uint32_t code_size);
}  // namespace tbc::debug
