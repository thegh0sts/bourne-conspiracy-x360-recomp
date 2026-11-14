// tbc - quick-time events on W / A / S / D (qte.cpp).
#pragma once

#include <cstdint>

struct PPCContext;

namespace tbc::qte {
// From UObject::ProcessEvent (script_trace.cpp), before the event runs.
void OnProcessEvent(const PPCContext& ctx, uint8_t* base, uint32_t function);
// From the game's XInputGetState (input_trace.cpp), on the state it is about to read.
void OnControllerState(uint8_t* base, uint32_t state);
}  // namespace tbc::qte
