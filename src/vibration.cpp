// tbc - controller vibration strength (--vibration_strength=0..100).
//
// The game asks for rumble in one place: sub_823213E8 builds an X_INPUT_VIBRATION on its stack
// (two big-endian u16 motor speeds, left then right, at r1+80) and calls XamInputSetState with
// r5 pointing at it (the bl at 0x82321494). A mid-function hook just before that call
// (hooks.toml) scales both speeds by the strength. 100 leaves them as the game set them; 0 is
// silence (the launcher also passes --vibration=false then, the runtime's own switch).

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_INT32(vibration_strength, 100, "Input",
                     "Controller vibration strength in percent (100 = as the game sets it)")
    .range(0, 100);

void TbcVibration(PPCRegister& r5) {
  const int strength = std::clamp(REXCVAR_GET(vibration_strength), 0, 100);
  if (strength >= 100 || !r5.u32) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) return;
  uint8_t* p = kernel->memory()->TranslateVirtual<uint8_t*>(r5.u32);
  for (int motor = 0; motor < 2; ++motor, p += 2) {
    const uint32_t speed = uint32_t(p[0]) << 8 | p[1];
    const uint32_t scaled = speed * uint32_t(strength) / 100;
    p[0] = uint8_t(scaled >> 8);
    p[1] = uint8_t(scaled);
  }
}
