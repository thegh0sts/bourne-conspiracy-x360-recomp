// tbc - "Quit Game" at the bottom of the main menu and the pause menu (--menu_quit, on by
// default).
//
// The menus are Unreal Engine 3 UI scenes of UILabelButtons one under the other: the main menu
// (Transient.MainMenuUI_<n>: Campaign ... Cheats) and the pause menu (Resume Game ... Exit to
// Main Menu), the focused entry drawn in another colour; the main menu also has the button bar's
// hint line (ButtonBarUI_<n>:BlackBar.MenuHintText) describing it. The game knows nothing of
// one more entry, so this fakes one:
//
// - Drawing (DrawString, sub_82C49D88, hooked in keyboard_prompts.cpp): when the last entry's
//   label is drawn, "Quit Game" is drawn one row lower, same font, size and the colour of an
//   unfocused entry, or of the focused one while it's selected. Then the entry the game still
//   has focus on is drawn unfocused, and the hint line reads "Exit to the desktop" (right-aligned
//   with the engine's own measuring). The colour is the FLinearColor DrawString takes in r8.
// - Selecting (the controller state the game reads, sub_823213D8, hooked in input_trace.cpp):
//   Down on the last entry, or Up on the first (the menus wrap), selects Quit Game; that press
//   doesn't reach the game. From there Down goes to the first entry and Up to the last, and A
//   (Enter) closes the game as the window's close button does. B / Esc work as usual.
//
// The scenes are found by name and their entries by place, so it works in every language: the
// buttons' rectangles (UIObject.RenderBounds, +0x21C) give the first and last, their StateStack
// (+0x9C) which has focus (a UIState_Focused in it). What a menu's first and last labels say is
// learnt when it opens, so menus drawn over it in the same places (its submenus) are left alone.
// Each menu scene's buttons are logged once by name. Offsets from the development ui_probe (git
// history).

#include "generated/default/tbc_pch.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <array>
#include <cctype>
#include <tuple>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "prompts.h"

REXCVAR_DEFINE_BOOL(menu_quit, true, "Input", "A Quit Game entry at the bottom of the main menu and the pause menu");

REX_EXTERN(__imp__sub_82C49D88);  // DrawString
// The engine's text measuring (what Canvas.TextSize ends in, sub_82C288F0 -> sub_82C1A720):
// r3 = the font, f1 / f2 = X / Y scale, r6 / r7 = where to write the width / height (ints),
// r8 = the text. Measures as DrawString lays text out, character remapping and all.
REX_EXTERN(sub_82C12D38);

namespace {

constexpr uint32_t kObjects = 0x83655774;  // GObjObjects: data, count
constexpr uint32_t kRenderBounds = 0x21C;
constexpr uint32_t kStateStack = 0x9C;
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kStart = 0x0010, kA = 0x1000;
constexpr int kStick = 12000;
constexpr auto kRecent = std::chrono::milliseconds(300);

uint32_t Load32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}
float LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t v = Load32(base, a);
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
bool IsObject(uint32_t a) {
  return a >= 0x40000000 && a < 0xE0000000 && (a & 3) == 0 && Readable(a) && Readable(a + 0x240);
}

struct Rect {
  float l = 0, t = 0, r = 0, b = 0;
  bool valid() const { return r > l + 1 && b > t + 1 && r - l < 5000 && b - t < 5000; }
  // The focused entry can be drawn a little bigger, a pixel or two left or up of its place.
  bool at(double x, double y) const { return std::fabs(x - l) < 4.0 && std::fabs(y - t) < 3.0; }
};

enum Role { kOther, kFirst, kLast };

// The menus that get the entry, by their scene's name, and what the log calls them.
struct Kind {
  const char* scene;  // part of the scene's object name
  const char* name;
};
constexpr Kind kKinds[] = {{"MainMenuUI_", "main menu"}, {"Pause", "pause menu"}};

struct Button {
  uint32_t object = 0;
  Rect rect;
  bool focused = false;
};

// A menu scene and its buttons, top to bottom: the first and last are the menu's ends.
struct Scene {
  int kind = 0;
  std::string name;
  std::vector<Button> buttons;
  bool any_focused = false;
  Rect first, last;
  Role focused = kOther;
  bool live = false;  // has focus and both ends: it could be on screen
  // What its first and last labels say, learnt from the first draws at their places in the
  // second after it first has focus (other menus reuse the same places for their own entries,
  // and a menu's scene stays around underneath them), then compared on every draw.
  std::chrono::steady_clock::time_point first_seen{};
  std::u16string first_text, last_text;
};

struct State {
  std::mutex mutex;
  std::chrono::steady_clock::time_point next_scan{};
  std::unordered_map<uint32_t, std::string> class_names;  // class -> path name
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::string> names;  // (object, name, outer)
  std::map<uint32_t, Scene> scenes;  // by scene (the buttons' outer)
  std::map<std::string, bool> logged_scenes;
  uint32_t hint = 0;                 // MenuHintText label
  Rect hint_rect;
  // The menu on screen: the scene whose own first or last label was drawn last.
  uint32_t active = 0;
  std::chrono::steady_clock::time_point drawn{};
  // Drawing. Each button's colour as last drawn: the colour most of them share is the
  // unfocused one, the one only one of them has the focused one (whatever the game's focus
  // animation does, the menu's own colours are copied).
  std::vector<std::array<uint8_t, 16>> colours;
  // And how each was drawn: offset from its place and scale (the focused entry is bigger).
  struct Look {
    double dx = 0, dy = 0, sx = 1, sy = 1;
    bool set = false;
  };
  std::vector<Look> looks;
  Look focus_look, other_look;
  uint32_t colours_scene = 0;
  uint8_t focus_colour[16] = {};
  uint8_t other_colour[16] = {};
  bool have_focus_colour = false, have_other_colour = false;
  int logged = 0;
  uint8_t logged_colours[32] = {};
  uint32_t guest = 0;    // guest memory: two colours (32 bytes) then the strings
  uint32_t scratch = 0;  // guest memory: the width and height the engine measures
  std::u16string label_text, hint_text, confirm_text;
  // The pause menu asks first (progress since the last checkpoint is lost): the first A turns
  // the entry into "Press again to quit", a second A within a few seconds quits, moving off it
  // or waiting cancels.
  bool armed = false;
  std::chrono::steady_clock::time_point armed_at{};
  // Input.
  bool selected = false;  // Quit Game is selected
  uint16_t prev = 0;
  int prev_dir = 0;  // -1 up, 1 down, from the stick
  bool swallow_up = false, swallow_down = false, swallow_a = false;
  bool quit = false;
};

State& S() {
  static State s;
  return s;
}

const std::string& Name(State& s, const PPCContext& ctx, uint8_t* base, uint32_t o) {
  const auto key = std::make_tuple(o, Load32(base, o + 0x2C), Load32(base, o + 0x28));
  auto it = s.names.find(key);
  if (it == s.names.end()) it = s.names.emplace(key, tbc::prompts::ObjectPathName(ctx, base, o)).first;
  return it->second;
}

const std::string& ClassName(State& s, const PPCContext& ctx, uint8_t* base, uint32_t cls) {
  auto it = s.class_names.find(cls);
  if (it == s.class_names.end()) it = s.class_names.emplace(cls, tbc::prompts::ObjectPathName(ctx, base, cls)).first;
  return it->second;
}

Rect Bounds(const uint8_t* base, uint32_t o) {
  return {LoadF(base, o + kRenderBounds), LoadF(base, o + kRenderBounds + 4), LoadF(base, o + kRenderBounds + 8),
          LoadF(base, o + kRenderBounds + 12)};
}

bool Focused(State& s, const PPCContext& ctx, uint8_t* base, uint32_t o) {
  const uint32_t arr = Load32(base, o + kStateStack), n = Load32(base, o + kStateStack + 4);
  for (uint32_t k = 0; IsObject(arr) && k < n && k < 8; ++k) {
    const uint32_t st = Load32(base, arr + 4 * k);
    if (!IsObject(st)) continue;
    const uint32_t cls = Load32(base, st + 0x30);
    if (IsObject(cls) && ClassName(s, ctx, base, cls).ends_with("UIState_Focused")) return true;
  }
  return false;
}

// Every half second (every 50 ms while a menu is up and none is known): the menus' buttons and
// the hint line, from the object table. Each menu scene with buttons is logged once by name.
void Scan(State& s, const PPCContext& ctx, uint8_t* base) {
  const uint32_t data = Load32(base, kObjects), count = Load32(base, kObjects + 4);
  if (!Readable(data) || count > 2000000) return;
  std::map<uint32_t, Scene> found;
  std::map<std::string, std::string> seen;  // scene name -> its buttons, for the log
  s.hint = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = Load32(base, data + 4 * i);
    if (!IsObject(o)) continue;
    const uint32_t cls = Load32(base, o + 0x30);
    if (!IsObject(cls)) continue;
    const std::string& cn = ClassName(s, ctx, base, cls);
    const bool button = cn == "Engine.UILabelButton";
    if (!button && cn != "Engine.UILabel") continue;
    const std::string& name = Name(s, ctx, base, o);
    if (button) {
      const size_t colon = name.find(':');
      const std::string scene_name = name.substr(0, colon);
      if (scene_name.find("Default__") == std::string::npos) {
        auto& list = seen[scene_name];
        if (list.size() < 300) list += (list.empty() ? "" : ", ") + name.substr(colon == std::string::npos ? 0 : colon + 1);
      }
      for (int k = 0; k < int(std::size(kKinds)); ++k) {
        if (scene_name.find(kKinds[k].scene) == std::string::npos || scene_name.find("Default__") != std::string::npos)
          continue;
        const uint32_t scene = Load32(base, o + 0x28);
        Scene& sc = found[scene];
        sc.kind = k;
        sc.name = scene_name;
        sc.buttons.push_back({o});
        break;
      }
    } else if (name.find("ButtonBarUI_") != std::string::npos && name.ends_with(".MenuHintText")) {
      s.hint = o;
    }
  }
  for (const auto& [scene_name, list] : seen) {
    // Instance numbers aside (MainMenuUI_10130), each kind of menu once.
    std::string kind = scene_name;
    while (!kind.empty() && std::isdigit(uint8_t(kind.back()))) kind.pop_back();
    if (s.logged_scenes.size() < 64 && !s.logged_scenes[kind]) {
      s.logged_scenes[kind] = true;
      REXLOG_INFO("menu_quit: menu {}: {}", scene_name, list);
    }
  }
  // Keep what was learnt about the scenes still there.
  for (auto& [scene, sc] : found) {
    if (auto it = s.scenes.find(scene); it != s.scenes.end() && it->second.name == sc.name) {
      sc.first_seen = it->second.first_seen;
      sc.first_text = it->second.first_text;
      sc.last_text = it->second.last_text;
    }
  }
  s.scenes = std::move(found);
}

// Every controller poll: each menu's rectangles, ends and focus.
void Refresh(State& s, const PPCContext& ctx, uint8_t* base) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= s.next_scan) {
    const bool soon = now - s.drawn > kRecent && tbc::prompts::MenuUp();
    s.next_scan = now + std::chrono::milliseconds(soon ? 50 : 500);
    Scan(s, ctx, base);
  }
  for (auto& [scene, sc] : s.scenes) {
    sc.any_focused = false;
    sc.live = false;
    sc.focused = kOther;
    const Button* first = nullptr;
    const Button* last = nullptr;
    for (auto& b : sc.buttons) {
      if (!IsObject(b.object)) continue;
      b.rect = Bounds(base, b.object);
      b.focused = Focused(s, ctx, base, b.object);
      if (b.focused) sc.any_focused = true;
      if (!b.rect.valid()) continue;
      if (!first || b.rect.t < first->rect.t) first = &b;
      if (!last || b.rect.t > last->rect.t) last = &b;
    }
    if (!sc.any_focused || !first || !last || first == last) continue;
    sc.live = true;
    sc.first = first->rect;
    sc.last = last->rect;
    sc.focused = first->focused ? kFirst : last->focused ? kLast : kOther;
    if (sc.first_seen == std::chrono::steady_clock::time_point{}) sc.first_seen = now;
  }
  if (s.active && (!s.scenes.count(s.active) || !s.scenes[s.active].live)) s.active = 0;
  s.hint_rect = s.hint && IsObject(s.hint) ? Bounds(base, s.hint) : Rect{};
}

int Language() {
  try {
    return std::stoi(rex::cvar::GetFlagByName("user_language"));
  } catch (...) {
    return 1;
  }
}

void Texts(State& s) {
  switch (Language()) {
    case 3: s.label_text = u"Spiel beenden"; s.hint_text = u"Zum Desktop zur\u00FCckkehren";
            s.confirm_text = u"Zum Beenden erneut dr\u00FCcken"; break;
    case 4: s.label_text = u"Quitter le jeu"; s.hint_text = u"Retourner au bureau";
            s.confirm_text = u"Appuyez \u00E0 nouveau pour quitter"; break;
    case 5: s.label_text = u"Salir del juego"; s.hint_text = u"Volver al escritorio";
            s.confirm_text = u"Pulsa otra vez para salir"; break;
    case 6: s.label_text = u"Esci dal gioco"; s.hint_text = u"Torna al desktop";
            s.confirm_text = u"Premi di nuovo per uscire"; break;
    default: s.label_text = u"Quit Game"; s.hint_text = u"Exit to the desktop";
             s.confirm_text = u"Press again to quit"; break;
  }
}

// Guest memory: [0] focus colour, [16] other colour, [32] label text, then hint text (UTF-16BE).
bool Guest(State& s, uint8_t* base) {
  if (s.guest) return true;
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return false;
  Texts(s);
  s.guest = runtime->memory()->SystemHeapAlloc(
      32 + 2 * uint32_t(s.label_text.size() + s.hint_text.size() + s.confirm_text.size() + 3));
  if (!s.guest) return false;
  uint32_t p = s.guest + 32;
  for (const auto* t : {&s.label_text, &s.hint_text, &s.confirm_text}) {
    for (char16_t c : *t) {
      base[p++] = uint8_t(c >> 8);
      base[p++] = uint8_t(c);
    }
    base[p++] = 0;
    base[p++] = 0;
  }
  return true;
}
uint32_t LabelAddr(const State& s) { return s.guest + 32; }
uint32_t HintAddr(const State& s) { return s.guest + 32 + 2 * uint32_t(s.label_text.size() + 1); }
uint32_t ConfirmAddr(const State& s) { return HintAddr(s) + 2 * uint32_t(s.hint_text.size() + 1); }

// Width of a UTF-16 string in a font at scale 1, measured by the engine, or -1.
double TextWidth(const PPCContext& call, uint8_t* base, State& s, uint32_t font, uint32_t text) {
  if (!font || !text) return -1.0;
  auto* runtime = rex::Runtime::instance();
  if (!s.scratch && runtime && runtime->memory()) s.scratch = runtime->memory()->SystemHeapAlloc(16);
  if (!s.scratch) return -1.0;
  std::memset(base + s.scratch, 0, 8);
  PPCContext ctx = call;
  ctx.r3.u64 = font;
  ctx.f1.f64 = 1.0;
  ctx.f2.f64 = 1.0;
  ctx.r6.u64 = s.scratch;
  ctx.r7.u64 = s.scratch + 4;
  ctx.r8.u64 = text;
  sub_82C12D38(ctx, base);
  const int32_t width = int32_t(Load32(base, s.scratch));
  return width > 0 && width < 4096 ? double(width) : -1.0;
}

void RequestQuit() {
  REXLOG_INFO("menu_quit: Quit Game selected");
  auto* runtime = rex::Runtime::instance();
  if (!runtime) return;
  auto* window = runtime->display_window();
  auto* app = runtime->app_context();
  if (window && app) {
    app->CallInUIThread([window] { window->RequestClose(); });
  }
}

}  // namespace

namespace tbc::menu_quit {

// DrawString (keyboard_prompts.cpp): returns true when it drew the text itself.
bool OnDrawString(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(menu_quit)) return false;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.scenes.empty() || !ctx.r8.u32 || !Guest(s, base)) return false;
  const double x = ctx.f1.f64, y = ctx.f2.f64;
  const auto now = std::chrono::steady_clock::now();
  // A menu's own first or last label: that menu is on screen.
  Role end = kOther;
  for (auto& [scene, sc] : s.scenes) {
    if (!sc.live) continue;
    const bool at_first = sc.first.at(x, y), at_last = sc.last.at(x, y);
    if (!at_first && !at_last) continue;
    std::u16string text;
    for (uint32_t p = ctx.r6.u32; p && text.size() < 64; p += 2) {
      const char16_t c = char16_t(base[p] << 8 | base[p + 1]);
      if (!c) break;
      text.push_back(c);
    }
    std::u16string& known = at_last ? sc.last_text : sc.first_text;
    if (known.empty() && now - sc.first_seen < std::chrono::seconds(1)) known = text;
    if (known.empty() || text != known) continue;
    if (s.active != scene) {
      REXLOG_INFO("menu_quit: {} on screen ({})", kKinds[sc.kind].name, sc.name);
      s.selected = false;
    }
    s.active = scene;
    s.drawn = now;
    end = at_last ? kLast : kFirst;
    break;
  }
  const bool showing = s.active && now - s.drawn < kRecent;
  if (!showing) return false;
  Scene& sc = s.scenes[s.active];
  int index = -1;
  for (size_t i = 0; i < sc.buttons.size(); ++i) {
    if (sc.buttons[i].rect.at(x, y)) index = int(i);
  }
  if (s.colours_scene != s.active || s.colours.size() != sc.buttons.size()) {
    s.colours_scene = s.active;
    s.colours.assign(sc.buttons.size(), {});
    s.looks.assign(sc.buttons.size(), {});
    s.focus_look = s.other_look = {};
    s.have_focus_colour = s.have_other_colour = false;
  }
  // Labels at a button's place that aren't the menu's own (another menu over it) are left alone.
  if ((sc.first.at(x, y) || sc.last.at(x, y)) && end == kOther) return false;
  const bool last = end == kLast;
  const bool hint = s.selected && s.hint_rect.valid() && std::fabs(y - s.hint_rect.t) < 1.5 &&
                    x >= s.hint_rect.l - 1 && x < s.hint_rect.r;
  if (index < 0 && !hint) return false;

  const PPCContext call = ctx;
  const uint8_t* colour = base + call.r8.u32;
  // A colour that shows (an outline or shadow pass, or a fade, can draw the same text clear).
  const bool visible = LoadF(colour, 12) > 0.05f;
  if (index >= 0 && size_t(index) < s.colours.size() && visible) {
    std::memcpy(s.colours[index].data(), colour, 16);
    const Rect& r = sc.buttons[index].rect;
    s.looks[index] = {x - r.l, y - r.t, call.f3.f64, call.f4.f64, true};
    // The colour most buttons share (unfocused) and the odd one out (focused); buttons not
    // drawn visibly yet don't count.
    std::map<std::array<uint8_t, 16>, int> counts;
    for (const auto& c : s.colours) {
      if (LoadF(c.data(), 12) > 0.05f) counts[c]++;
    }
    const std::array<uint8_t, 16>* most = nullptr;
    const std::array<uint8_t, 16>* single = nullptr;
    int singles = 0;
    for (const auto& [c, n] : counts) {
      if (!most || n > counts[*most]) most = &c;
      if (n == 1) {
        single = &c;
        ++singles;
      }
    }
    if (most && counts[*most] >= 2) {
      std::memcpy(s.other_colour, most->data(), 16);
      s.have_other_colour = true;
    }
    if (single && singles == 1 && counts.size() == 2) {
      std::memcpy(s.focus_colour, single->data(), 16);
      s.have_focus_colour = true;
    }
    std::memcpy(base + s.guest, s.focus_colour, 16);
    std::memcpy(base + s.guest + 16, s.other_colour, 16);
    for (size_t i = 0; i < s.colours.size(); ++i) {
      if (!s.looks[i].set) continue;
      if (s.have_focus_colour && std::memcmp(s.colours[i].data(), s.focus_colour, 16) == 0) s.focus_look = s.looks[i];
      if (s.have_other_colour && std::memcmp(s.colours[i].data(), s.other_colour, 16) == 0) s.other_look = s.looks[i];
    }
    const bool changed = std::memcmp(s.logged_colours, s.focus_colour, 16) != 0 ||
                         std::memcmp(s.logged_colours + 16, s.other_colour, 16) != 0;
    if (s.logged < 20 && changed && s.have_focus_colour && s.have_other_colour) {
      ++s.logged;
      std::memcpy(s.logged_colours, s.focus_colour, 16);
      std::memcpy(s.logged_colours + 16, s.other_colour, 16);
      float f[8];
      for (int k = 0; k < 4; ++k) {
        f[k] = LoadF(s.focus_colour, 4 * k);
        f[4 + k] = LoadF(s.other_colour, 4 * k);
      }
      REXLOG_INFO("menu_quit: focused colour ({:.2f} {:.2f} {:.2f} {:.2f}), others ({:.2f} {:.2f} {:.2f} {:.2f})",
                  f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
  }
  // The button with the game's focus while Quit Game is selected: drawn as the others are.
  const bool focus_drawn = s.have_focus_colour && std::memcmp(colour, s.focus_colour, 16) == 0;
  // A pass of this label drawn as `to` instead of as it's drawn now (its main pass: `from`),
  // keeping the pass's own offset from the main one (a shadow stays a shadow).
  auto restyle = [&](const State::Look& from, const State::Look& to) {
    if (!from.set || !to.set || from.sx <= 0 || from.sy <= 0) return;
    ctx.f1.f64 += to.dx - from.dx;
    ctx.f2.f64 += to.dy - from.dy;
    ctx.f3.f64 *= to.sx / from.sx;
    ctx.f4.f64 *= to.sy / from.sy;
  };
  const State::Look main_look = index >= 0 && size_t(index) < s.looks.size() ? s.looks[index] : State::Look{};
  if (index >= 0 && s.selected && main_look.set && s.focus_look.set &&
      std::fabs(main_look.sx - s.focus_look.sx) < 1e-6 && std::fabs(main_look.dx - s.focus_look.dx) < 1e-6 &&
      s.have_other_colour && (focus_drawn || !visible)) {
    // The entry the game still has focus on, while Quit Game is selected: as an unfocused one.
    if (focus_drawn) ctx.r8.u64 = s.guest + 16;
    restyle(main_look, s.other_look);
  }
  if (hint) {
    // The hint line, right-aligned as the game lays it out.
    const double before = TextWidth(call, base, s, call.r7.u32, call.r6.u32);
    const double after = TextWidth(call, base, s, call.r7.u32, HintAddr(s));
    ctx.r6.u64 = HintAddr(s);
    if (before > 0 && after > 0) ctx.f1.f64 = x + (before - after) * call.f3.f64;
  }
  __imp__sub_82C49D88(ctx, base);
  const uint64_t result = ctx.r3.u64;
  if (last) {
    // Quit Game, one row under the last entry.
    ctx = call;
    ctx.r6.u64 = s.armed ? ConfirmAddr(s) : LabelAddr(s);
    ctx.f2.f64 = y + double(sc.last.b - sc.last.t);
    // Sized and placed as an unfocused entry, or as the focused one while it's selected.
    restyle(main_look, s.selected ? s.focus_look : s.other_look);
    const bool last_main = visible && (focus_drawn || (s.have_other_colour &&
                                                         std::memcmp(colour, s.other_colour, 16) == 0));
    if (last_main) {
      if (s.selected && s.have_focus_colour) ctx.r8.u64 = s.guest;
      else if (s.have_other_colour) ctx.r8.u64 = s.guest + 16;
    }
    __imp__sub_82C49D88(ctx, base);
  }
  ctx = call;
  ctx.r3.u64 = result;
  return true;
}

// Every controller poll (input_trace.cpp): moves on and off Quit Game, and quits.
void OnControllerState(const PPCContext& ctx, uint8_t* base, uint32_t state) {
  if (!REXCVAR_GET(menu_quit)) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  Refresh(s, ctx, base);
  const uint16_t buttons = uint16_t(base[state + 4] << 8 | base[state + 5]);
  const int16_t ly = int16_t(base[state + 10] << 8 | base[state + 11]);
  const int dir = ly > kStick ? -1 : ly < -kStick ? 1 : 0;  // stick up = positive Y
  const bool up = (buttons & kUp) || dir < 0, down = (buttons & kDown) || dir > 0, a = buttons & kA;
  const bool was_up = (s.prev & kUp) || s.prev_dir < 0, was_down = (s.prev & kDown) || s.prev_dir > 0;
  const bool up_edge = up && !was_up, down_edge = down && !was_down, a_edge = a && !(s.prev & kA);
  s.prev = buttons;
  s.prev_dir = dir;
  if (!up) s.swallow_up = false;
  if (!down) s.swallow_down = false;
  if (!a) s.swallow_a = false;

  const bool showing = s.active && std::chrono::steady_clock::now() - s.drawn < kRecent;
  if (!showing) {
    if (s.selected) REXLOG_INFO("menu_quit: menu closed");
    s.selected = false;
    return;
  }
  const Role focused = s.scenes[s.active].focused;
  if (down_edge) {
    if (!s.selected && focused == kLast) {
      s.selected = true;
      s.swallow_down = true;
    } else if (s.selected) {
      s.selected = false;
      // The game still has focus on the entry we came from: from the last its Down goes to the
      // first (the menus wrap); from the first we're there already.
      if (focused != kLast) s.swallow_down = true;
    }
  }
  if (up_edge) {
    if (!s.selected && focused == kFirst) {
      s.selected = true;
      s.swallow_up = true;
    } else if (s.selected) {
      s.selected = false;
      if (focused != kFirst) s.swallow_up = true;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (!s.selected || now - s.armed_at > std::chrono::seconds(4)) s.armed = false;
  const bool ask = s.scenes[s.active].kind != 0;  // not the main menu: progress can be lost
  if (s.selected && a_edge && ask && !s.armed) {
    s.armed = true;
    s.armed_at = now;
  } else if (s.selected && a_edge && !s.quit) {
    s.quit = true;
    RequestQuit();
  }
  if (s.selected && a) s.swallow_a = true;

  uint16_t out = buttons;
  int16_t out_ly = ly;
  if (s.swallow_down) {
    out &= ~kDown;
    if (out_ly < 0) out_ly = 0;
  }
  if (s.swallow_up) {
    out &= ~kUp;
    if (out_ly > 0) out_ly = 0;
  }
  if (s.swallow_a || s.selected) out &= ~(kA | kStart);
  if (out != buttons || out_ly != ly) {
    base[state + 4] = uint8_t(out >> 8);
    base[state + 5] = uint8_t(out);
    base[state + 10] = uint8_t(uint16_t(out_ly) >> 8);
    base[state + 11] = uint8_t(out_ly);
  }
}

}  // namespace tbc::menu_quit
