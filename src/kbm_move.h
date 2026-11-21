// tbc - keyboard and mouse: W / A / S / D set the direction, the scroll wheel the speed (kbm_move.cpp).
#pragma once

#include <cstdint>

namespace tbc::kbm_move {
// A scroll of the mouse wheel (rex::ui::MouseEvent::scroll_y, 120 a notch, up positive).
void OnWheel(int32_t scroll_y);
// From the game's XInputGetState (input_trace.cpp), on the state it is about to read.
void OnControllerState(uint8_t* base, uint32_t state);
}  // namespace tbc::kbm_move
