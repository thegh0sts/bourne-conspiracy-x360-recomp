// tbc - the mouse in menus (keyboard and mouse): point at a menu option to select it, click
// to choose it.
//
// Bourne's menus are Unreal Engine 3 UI scenes. Found with --ui_probe (ui_probe.cpp): each
// option is a UILabelButton (a UIButton) whose rectangle on the 1280x720 (or wider) screen
// is UIObject.RenderBounds (+0x21C: left, top, right, bottom), whose scene is
// UIObject.OwnerScene (+0x19C), and whose states are UIScreenObject.StateStack (+0x9C, an
// array of UIState objects): the selected option has a UIState_Focused. Only one option has
// focus, so its scene is the menu on screen.
//
// While an option has focus and the menu's prompts are drawn (a menu is up), the mouse isn't captured for mouse look: the
// Windows cursor shows and moves freely (SetMouseLookActive(false); the stick gets nothing
// from it). When the cursor moves onto another option of that menu, the option is given focus
// with the game's own UIScreenObject.SetFocus (called through UObject::ProcessEvent, as script
// would), so it's highlighted exactly as if reached with the keys. The left mouse button (the
// right trigger in keyboard-and-mouse mode) on an option presses A; the right button (the left
// trigger) presses B, back. Away from options the mouse buttons do nothing in menus.
//
//   --menu_mouse=false      off: the mouse stays captured in menus, as before

#include "generated/default/tbc_pch.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
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

REXCVAR_DEFINE_BOOL(menu_mouse, true, "Input",
                    "Keyboard and mouse: in menus the cursor shows; point at an option to select "
                    "it, left click to choose it, right click to go back");

REX_EXTERN(sub_82920EE8);  // UObject::StaticFindObject(UClass*, UObject* outer, const TCHAR*, UBOOL exact)
REX_EXTERN(sub_828B8750);  // UObject::ProcessEvent(UFunction*, void* Parms, void* Result)

namespace {

constexpr uint32_t kRenderBounds = 0x21C;  // UIObject.RenderBounds: left, top, right, bottom
constexpr uint32_t kOwnerScene = 0x19C;    // UIObject.OwnerScene
constexpr uint32_t kStateStack = 0x9C;     // UIScreenObject.StateStack: TArray<UIState>
constexpr uint32_t kRenderWidth = 0x8342402C;  // the picture's width (ultrawide.cpp); 720 high
constexpr uint16_t kA = 0x1000, kB = 0x2000;

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

uint32_t FindObject(const PPCContext& ctx, uint8_t* base, const char* name) {
  PPCContext tmp = ctx;
  const uint32_t text = ctx.r1.u32 - 0x300;  // UTF-16BE name below the caller's frame
  for (size_t i = 0;; ++i) {
    const uint16_t c = uint8_t(name[i]);
    base[text + 2 * i] = uint8_t(c >> 8);
    base[text + 2 * i + 1] = uint8_t(c);
    if (!c) break;
  }
  tmp.r1.u32 = ctx.r1.u32 - 0x600;
  tmp.r3.u64 = 0;
  tmp.r4.u64 = 0;
  tmp.r5.u64 = text;
  tmp.r6.u64 = 0;
  sub_82920EE8(tmp, base);
  return tmp.r3.u32;
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

struct Rect {
  float l, t, r, b;
};

struct State {
  std::mutex mutex;
  bool ready = false;
  uint32_t button_class = 0, focused_class = 0, set_focus = 0, function_class = 0;
  std::unordered_map<uint32_t, bool> is_button;  // class -> descends from Engine.UIButton
  std::vector<uint32_t> buttons;                 // UIButtons in Transient (live menus)
  std::chrono::steady_clock::time_point next_scan{};
  bool menu = false;           // last poll: a menu was up (mouse released)
  long last_x = -1, last_y = -1;
  uint32_t requested = 0;      // the option last given focus from the mouse
  bool click_a = false, click_b = false;  // a mouse button held as A / B
};

State& S() {
  static State s;
  return s;
}

bool Focused(const State& s, const uint8_t* base, uint32_t o) {
  if (!Readable(o + kStateStack + 8)) return false;
  const uint32_t arr = Load32(base, o + kStateStack), n = Load32(base, o + kStateStack + 4);
  for (uint32_t k = 0; k < n && k < 8 && IsObject(arr); ++k) {
    const uint32_t st = Load32(base, arr + 4 * k);
    if (IsObject(st) && Load32(base, st + 0x30) == s.focused_class) return true;
  }
  return false;
}

Rect Bounds(const uint8_t* base, uint32_t o) {
  return {LoadF(base, o + kRenderBounds), LoadF(base, o + kRenderBounds + 4), LoadF(base, o + kRenderBounds + 8),
          LoadF(base, o + kRenderBounds + 12)};
}

// Classes, the SetFocus function, once.
void Init(State& s, const PPCContext& ctx, uint8_t* base) {
  s.button_class = FindObject(ctx, base, "Engine.UIButton");
  s.focused_class = FindObject(ctx, base, "Engine.UIState_Focused");
  s.set_focus = FindObject(ctx, base, "Engine.UIScreenObject.SetFocus");
  if (!s.set_focus) s.set_focus = FindObject(ctx, base, "Engine.UIScreenObject:SetFocus");
  s.ready = s.button_class && s.focused_class && s.set_focus;
  REXLOG_INFO("menu_mouse: UIButton {:08X}, UIState_Focused {:08X}, SetFocus {:08X}{}", s.button_class,
              s.focused_class, s.set_focus, s.ready ? "" : " - not all found, the mouse stays captured in menus");
}

// Every UIButton that's a live menu's (in the Transient package, where scenes are opened).
void Scan(State& s, const PPCContext& ctx, uint8_t* base) {
  s.buttons.clear();
  const uint32_t data = Load32(base, 0x83655774), count = Load32(base, 0x83655778);
  if (!Readable(data) || count > 2000000) return;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = Load32(base, data + 4 * i);
    if (!IsObject(o) || !Readable(o + kRenderBounds + 16)) continue;
    const uint32_t cls = Load32(base, o + 0x30);
    if (!IsObject(cls)) continue;
    auto it = s.is_button.find(cls);
    if (it == s.is_button.end()) {
      bool b = false;
      for (uint32_t c : ClassChain(base, cls)) b |= c == s.button_class;
      it = s.is_button.emplace(cls, b).first;
    }
    if (!it->second || !IsObject(Load32(base, o + kOwnerScene))) continue;
    s.buttons.push_back(o);
  }
  (void)ctx;
}

// UIScreenObject.SetFocus(Sender = none, PlayerIndex = 0), through ProcessEvent.
void SetFocus(const State& s, const PPCContext& ctx, uint8_t* base, uint32_t widget) {
  PPCContext tmp = ctx;
  const uint32_t parms = ctx.r1.u32 - 0x340;
  std::memset(base + parms, 0, 32);
  tmp.r1.u32 = ctx.r1.u32 - 0x800;
  tmp.r3.u64 = widget;
  tmp.r4.u64 = s.set_focus;
  tmp.r5.u64 = parms;
  tmp.r6.u64 = 0;
  sub_828B8750(tmp, base);
}

// The cursor in the game's picture coordinates (0..width, 0..720), or false if the game window
// isn't the one in front.
bool Cursor(const uint8_t* base, float& x, float& y, long& raw_x, long& raw_y) {
#ifdef _WIN32
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  if (!w || (GetWindowThreadProcessId(w, &pid), pid) != GetCurrentProcessId()) return false;
  POINT p;
  RECT r;
  if (!GetCursorPos(&p) || !ScreenToClient(w, &p) || !GetClientRect(w, &r)) return false;
  const float cw = float(r.right - r.left), ch = float(r.bottom - r.top);
  uint32_t gw = Load32(base, kRenderWidth);
  if (gw < 640 || gw > 4096) gw = 1280;
  const float scale = std::min(cw / float(gw), ch / 720.0f);
  if (scale <= 0) return false;
  x = (float(p.x) - (cw - gw * scale) / 2) / scale;
  y = (float(p.y) - (ch - 720.0f * scale) / 2) / scale;
  raw_x = p.x;
  raw_y = p.y;
  return true;
#else
  (void)base, (void)x, (void)y, (void)raw_x, (void)raw_y;
  return false;
#endif
}

void SetButtons(uint8_t* base, uint32_t state, uint16_t set, uint16_t clear) {
  uint16_t b = uint16_t(base[state + 4] << 8 | base[state + 5]);
  b = uint16_t((b | set) & ~clear);
  base[state + 4] = uint8_t(b >> 8);
  base[state + 5] = uint8_t(b);
}

}  // namespace

namespace tbc::ui {

// Every controller poll (input_trace.cpp), after the driver filled the state.
void MenuMouse(const PPCContext& ctx, uint8_t* base, uint32_t state) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  const bool on = REXCVAR_GET(menu_mouse) && rex::cvar::Query<bool>("mnk_mode") &&
                  rex::cvar::Query<bool>("mnk_mouse");
  if (!on) {
    if (s.menu) rex::input::mnk::SetMouseLookActive(true);
    s.menu = false;
    return;
  }
  if (!s.ready) {
    static int tries = 0;
    if (tries++ % 300 != 0) return;  // every few seconds until the engine's classes are there
    Init(s, ctx, base);
    if (!s.ready) return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= s.next_scan) {
    s.next_scan = now + std::chrono::milliseconds(250);
    Scan(s, ctx, base);
  }
  // The focused option: its scene is the menu on screen.
  uint32_t focused = 0;
  for (uint32_t o : s.buttons) {
    if (IsObject(o) && Focused(s, base, o)) {
      focused = o;
      break;
    }
  }
  // A focused option alone isn't enough: a closed menu's last option can keep its focus state.
  // The menu's own prompts (Accept / Back, Paused) say it's on screen.
  const bool menu = focused != 0 && tbc::prompts::MenuUp();
  if (menu != s.menu) {
    rex::input::mnk::SetMouseLookActive(!menu);  // released in menus: the cursor shows
    s.menu = menu;
    s.last_x = s.last_y = -1;
    s.requested = 0;
  }
  if (!menu) return;

  float x = 0, y = 0;
  long rx = 0, ry = 0;
  uint32_t hovered = 0;
  if (Cursor(base, x, y, rx, ry)) {
    const uint32_t scene = Load32(base, focused + kOwnerScene);
    for (uint32_t o : s.buttons) {
      if (!IsObject(o) || Load32(base, o + kOwnerScene) != scene) continue;
      const Rect r = Bounds(base, o);
      if (x >= r.l && x < r.r && y >= r.t && y < r.b) {
        hovered = o;
        break;
      }
    }
    // Only a moving cursor selects, so the keys still work while the mouse rests.
    const bool moved = rx != s.last_x || ry != s.last_y;
    s.last_x = rx;
    s.last_y = ry;
    if (moved && hovered && hovered != focused && hovered != s.requested) {
      SetFocus(s, ctx, base, hovered);
      s.requested = hovered;
    }
    if (hovered == focused) s.requested = 0;
  }
  // Mouse buttons (the triggers in keyboard-and-mouse mode): left = A on an option, right = B.
  const bool left = base[state + 7] > 64, right = base[state + 6] > 64;
  base[state + 6] = base[state + 7] = 0;
  s.click_a = left && (s.click_a || (hovered && hovered == focused));
  s.click_b = right;
  SetButtons(base, state, uint16_t((s.click_a ? kA : 0) | (s.click_b ? kB : 0)), 0);
}

}  // namespace tbc::ui
