// tbc - "Quit Game" under "Cheats" on the main menu (--menu_quit, on by default).
//
// The main menu is an Unreal Engine 3 UI scene (Transient.MainMenuUI_<n>) of UILabelButtons:
// Campaign, Options, Achievements, Extras, Cheats, one under the other, the focused one drawn in
// another colour, and the button bar's hint line (ButtonBarUI_<n>:BlackBar.MenuHintText)
// describing it. The game knows nothing of a sixth entry, so this fakes one:
//
// - Drawing (DrawString, sub_82C49D88, hooked in keyboard_prompts.cpp): when Cheats' label is
//   drawn, "Quit Game" is drawn one row lower, same font, size and the colour of an unfocused
//   entry, or of the focused one while it's selected. Then the button the game still has focus
//   on is drawn unfocused, and the hint line reads "Exit to the desktop" (right-aligned as the
//   game's is). The colour is the FLinearColor DrawString takes in r8.
// - Selecting (the controller state the game reads, sub_823213D8, hooked in input_trace.cpp):
//   Down on Cheats, or Up on Campaign (the menu wraps), selects Quit Game; that press doesn't
//   reach the game. From there Down goes to Campaign and Up to Cheats, and A (Enter) closes the
//   game as the window's close button does. B / Esc goes back to the title as usual.
//
// The widgets are found by name, so it works in every language: their rectangles
// (UIObject.RenderBounds, +0x21C) tell which label is which, their StateStack (+0x9C) which has
// focus (a UIState_Focused in it). Offsets from the development ui_probe (git history).

#include "generated/default/tbc_pch.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <array>
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

REXCVAR_DEFINE_BOOL(menu_quit, true, "Input", "A Quit Game entry under Cheats on the main menu");

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

// Answers are kept per 4 KB page, but only until the next object table scan (Scan clears them):
// kept for good, a page that wasn't readable when first asked (the object table before a level
// grew it, a heap page not yet in use) stayed "unreadable" after the game moved things there, and
// back on the main menu after playing, its buttons were never found again (no Quit Game). The
// same mistake once made qte.cpp's keys stop working.
std::unordered_map<uint32_t, bool> g_pages;

bool Readable(uint32_t addr) {
  auto& pages = g_pages;
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
  bool at(double x, double y) const { return std::fabs(x - l) < 1.5 && std::fabs(y - t) < 1.5; }
};

enum Role { kOther, kCampaign, kCheats };

struct Button {
  uint32_t object = 0;
  Role role = kOther;
  Rect rect;
  bool focused = false;
};

struct Scene {
  std::vector<Button> buttons;
  bool any_focused = false;
};

struct State {
  std::mutex mutex;
  std::chrono::steady_clock::time_point next_scan{};
  std::unordered_map<uint32_t, std::string> class_names;  // class -> path name
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::string> names;  // (object, name, outer)
  std::map<uint32_t, Scene> scenes;  // by scene (the buttons' outer)
  uint32_t hint = 0;                 // MenuHintText label
  Rect hint_rect;
  // From the last poll: the live main menu.
  Rect cheats, campaign;
  std::vector<Rect> rects;  // every button of the live menu
  uint32_t live_scene = 0;
  Role focused = kOther;
  bool found = false;
  // Drawing. Each button's colour as last drawn: the colour most of them share is the
  // unfocused one, the one only one of them has the focused one (whatever the game's focus
  // animation does, the menu's own colours are copied).
  std::chrono::steady_clock::time_point drawn{};  // a main-menu label was drawn
  std::vector<std::array<uint8_t, 16>> colours;
  uint8_t focus_colour[16] = {};
  uint8_t other_colour[16] = {};
  bool have_focus_colour = false, have_other_colour = false;
  int logged = 0;
  // The main menu's rows (Campaign to Cheats), and when a label was last drawn in each and in the
  // row under Cheats.
  std::vector<float> rows;
  std::vector<std::chrono::steady_clock::time_point> row_hit;
  std::chrono::steady_clock::time_point below_hit{};
  // Text drawn left of the labels in their rows: Replay Chapter's city names ("Marseilles",
  // "Zurich"). The main menu has nothing there.
  std::chrono::steady_clock::time_point left_hit{};
  // What the live menu's Campaign and Cheats labels say, learnt from the first draws at their
  // places once the menu's scene appears (other menus reuse the same places for their own
  // entries, and the main menu's scene stays around underneath them).
  struct Texts {
    std::chrono::steady_clock::time_point first_seen;
    std::u16string cheats, campaign;
  };
  std::map<uint32_t, Texts> texts;
  uint8_t logged_colours[32] = {};
  uint32_t guest = 0;
  uint32_t scratch = 0;  // guest memory: the width and height the engine measures  // guest memory: two colours (32 bytes) then the strings
  std::u16string label_text, hint_text;
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

// Every half second: the main menu's buttons and the hint line, from the object table.
void Scan(State& s, const PPCContext& ctx, uint8_t* base) {
  g_pages.clear();
  const uint32_t data = Load32(base, kObjects), count = Load32(base, kObjects + 4);
  if (!Readable(data) || count > 2000000) return;
  s.scenes.clear();
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
    if (button && name.find("MainMenuUI_") != std::string::npos) {
      Button b;
      b.object = o;
      b.role = name.ends_with(":Cheats") ? kCheats : name.ends_with(":Campaign") ? kCampaign : kOther;
      s.scenes[Load32(base, o + 0x28)].buttons.push_back(b);
    } else if (!button && name.find("ButtonBarUI_") != std::string::npos && name.ends_with(".MenuHintText")) {
      s.hint = o;
    }
  }
}

// Every controller poll: rectangles and focus of the live main menu (the scene with a focused
// button; an old one lingers until garbage collection).
void Refresh(State& s, const PPCContext& ctx, uint8_t* base) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= s.next_scan) {
    // Quickly while a menu is up and the main menu isn't found yet, so Quit Game shows with the
    // rest of it; otherwise every half second (a full pass over the object table).
    const bool soon = !s.found && tbc::prompts::MenuUp();
    s.next_scan = now + std::chrono::milliseconds(soon ? 50 : 500);
    Scan(s, ctx, base);
  }
  s.found = false;
  s.focused = kOther;
  for (auto& [scene, sc] : s.scenes) {
    sc.any_focused = false;
    Rect cheats, campaign;
    Role focused = kOther;
    for (auto& b : sc.buttons) {
      if (!IsObject(b.object)) continue;
      b.rect = Bounds(base, b.object);
      b.focused = Focused(s, ctx, base, b.object);
      if (b.focused) {
        sc.any_focused = true;
        focused = b.role;
      }
      if (b.role == kCheats) cheats = b.rect;
      if (b.role == kCampaign) campaign = b.rect;
    }
    if (sc.any_focused && cheats.valid() && campaign.valid()) {
      s.found = true;
      if (!s.texts.count(scene)) {
        if (s.texts.size() > 16) s.texts.clear();
        s.texts[scene].first_seen = std::chrono::steady_clock::now();
      }
      if (s.live_scene != scene || s.rects.size() != sc.buttons.size()) {
        s.live_scene = scene;
        s.colours.assign(sc.buttons.size(), {});
        s.have_focus_colour = s.have_other_colour = false;
      }
      s.rects.clear();
      for (const auto& b : sc.buttons) s.rects.push_back(b.rect);
      std::vector<float> rows;
      for (const auto& b : sc.buttons) {
        if (!b.rect.valid() || std::fabs(b.rect.l - cheats.l) > 4.0f) continue;
        if (b.rect.t < campaign.t - 1.0f || b.rect.t > cheats.t + 1.0f) continue;
        bool known = false;
        for (float r : rows) known = known || std::fabs(r - b.rect.t) < 2.0f;
        if (!known) rows.push_back(b.rect.t);
      }
      if (rows.size() != s.rows.size()) s.row_hit.assign(rows.size(), {});
      s.rows = rows;
      s.cheats = cheats;
      s.campaign = campaign;
      s.focused = focused;
      break;
    }
  }
  s.hint_rect = s.hint && IsObject(s.hint) ? Bounds(base, s.hint) : Rect{};
  // In the log: each time the live main menu appears or goes (the first 40 times).
  static bool was_found = false;
  static int found_logged = 0;
  if (s.found != was_found && found_logged < 40) {
    ++found_logged;
    size_t buttons = 0;
    for (const auto& [scene, sc] : s.scenes) buttons += sc.buttons.size();
    REXLOG_INFO("menu_quit: main menu {} ({} scene(s), {} button(s) in the object table)",
                s.found ? "found" : "gone", s.scenes.size(), buttons);
  }
  was_found = s.found;
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
    case 3: s.label_text = u"Spiel beenden"; s.hint_text = u"Zum Desktop zurückkehren"; break;
    case 4: s.label_text = u"Quitter le jeu"; s.hint_text = u"Retourner au bureau"; break;
    case 5: s.label_text = u"Salir del juego"; s.hint_text = u"Volver al escritorio"; break;
    case 6: s.label_text = u"Esci dal gioco"; s.hint_text = u"Torna al desktop"; break;
    default: s.label_text = u"Quit Game"; s.hint_text = u"Exit to the desktop"; break;
  }
}

// Guest memory: [0] focus colour, [16] other colour, [32] label text, then hint text (UTF-16BE).
bool Guest(State& s, uint8_t* base) {
  if (s.guest) return true;
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return false;
  Texts(s);
  s.guest = runtime->memory()->SystemHeapAlloc(32 + 2 * uint32_t(s.label_text.size() + s.hint_text.size() + 2));
  if (!s.guest) return false;
  uint32_t p = s.guest + 32;
  for (const auto* t : {&s.label_text, &s.hint_text}) {
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
  REXLOG_INFO("menu_quit: Quit Game selected on the main menu");
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
  if (!s.found || !ctx.r8.u32 || !Guest(s, base)) return false;
  const double x = ctx.f1.f64, y = ctx.f2.f64;
  int index = -1;
  for (size_t i = 0; i < s.rects.size(); ++i) {
    if (s.rects[i].at(x, y)) index = int(i);
  }
  const bool cheats = s.cheats.at(x, y);
  const auto now = std::chrono::steady_clock::now();
  // The main menu is on screen when every one of its rows (Campaign to Cheats) had a label drawn
  // in the last moment, nothing was drawn in the row under Cheats, and nothing left of the labels
  // in their rows. Its submenus reuse the same places and its scene stays around under them, so
  // its own places alone can't tell: Replay Chapter lists one chapter per row from the bottom up
  // (with five unlocked it fills exactly Campaign to Cheats) with each chapter's city to its left,
  // and Extras fills all five rows and a sixth.
  const double row_h = s.cheats.b - s.cheats.t;
  if (std::fabs(x - s.cheats.l) < 4.0) {
    for (size_t i = 0; i < s.rows.size(); ++i) {
      if (std::fabs(y - s.rows[i]) < 3.0) s.row_hit[i] = now;
    }
    if (std::fabs(y - (s.cheats.t + row_h)) < 3.0) s.below_hit = now;
  } else if (x < s.cheats.l - 16.0 && y >= s.campaign.t - 3.0 && y <= s.cheats.b + 3.0) {
    s.left_hit = now;
  }
  if (cheats || s.campaign.at(x, y)) {
    // Rows never drawn in at all (a hidden button's) don't count.
    bool all = true;
    int drawn_rows = 0;
    for (const auto& t : s.row_hit) {
      if (t == std::chrono::steady_clock::time_point{}) continue;
      ++drawn_rows;
      all = all && now - t < std::chrono::milliseconds(100);
    }
    all = all && drawn_rows >= 3;
    if (!all || now - s.below_hit < std::chrono::milliseconds(150) ||
        now - s.left_hit < std::chrono::milliseconds(150)) {
      return false;
    }
    s.drawn = now;
  }
  const bool showing = now - s.drawn < kRecent;
  if (index >= 0 && !showing) return false;
  const bool hint = showing && s.selected && s.hint_rect.valid() && std::fabs(y - s.hint_rect.t) < 1.5 &&
                    x >= s.hint_rect.l - 1 && x < s.hint_rect.r;
  if (index < 0 && !hint) return false;

  const PPCContext call = ctx;
  const uint8_t* colour = base + call.r8.u32;
  // A colour that shows (an outline or shadow pass, or a fade, can draw the same text clear).
  const bool visible = LoadF(colour, 12) > 0.05f;
  if (index >= 0 && size_t(index) < s.colours.size() && visible) {
    std::memcpy(s.colours[index].data(), colour, 16);
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
  if (index >= 0 && s.selected && focus_drawn && s.have_other_colour) ctx.r8.u64 = s.guest + 16;
  if (hint) {
    // The hint line, right-aligned as the game lays it out.
    const double before = TextWidth(call, base, s, call.r7.u32, call.r6.u32);
    const double after = TextWidth(call, base, s, call.r7.u32, HintAddr(s));
    ctx.r6.u64 = HintAddr(s);
    if (before > 0 && after > 0) ctx.f1.f64 = x + (before - after) * call.f3.f64;
  }
  __imp__sub_82C49D88(ctx, base);
  const uint64_t result = ctx.r3.u64;
  if (cheats) {
    // Quit Game, one row under Cheats.
    ctx = call;
    ctx.r6.u64 = LabelAddr(s);
    ctx.f2.f64 = y + double(s.cheats.b - s.cheats.t);
    const bool cheats_main = visible && (focus_drawn || (s.have_other_colour &&
                                                         std::memcmp(colour, s.other_colour, 16) == 0));
    if (cheats_main) {
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

  const bool showing = s.found && std::chrono::steady_clock::now() - s.drawn < kRecent;
  if (!showing) {
    if (s.selected) REXLOG_INFO("menu_quit: main menu closed");
    s.selected = false;
    return;
  }
  if (down_edge) {
    if (!s.selected && s.focused == kCheats) {
      s.selected = true;
      s.swallow_down = true;
    } else if (s.selected) {
      s.selected = false;
      // The game still has focus on the button we came from: from Cheats its Down already goes
      // to Campaign (the menu wraps); from Campaign we're there already.
      if (s.focused != kCheats) s.swallow_down = true;
    }
  }
  if (up_edge) {
    if (!s.selected && s.focused == kCampaign) {
      s.selected = true;
      s.swallow_up = true;
    } else if (s.selected) {
      s.selected = false;
      if (s.focused != kCampaign) s.swallow_up = true;
    }
  }
  if (s.selected && a_edge && !s.quit) {
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
