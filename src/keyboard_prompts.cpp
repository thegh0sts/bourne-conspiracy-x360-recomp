// tbc - keyboard prompts: show the player's own keys where the game shows Xbox buttons.
//
// Bourne draws every button prompt as text: the button is one character of the font
// BourneUIPlatformSpecific.ControllerButtonFont, drawn on its own between the words around it
// ("Press " + "Y" + " to use Bourne Instinct..."). Found with --prompt_trace (prompt_trace.cpp).
// All text goes through one engine function, DrawString (sub_82C49D88): r6 = the text
// (UTF-16, ends with 0), r7 = the font, f1/f2 = X/Y, f3/f4 = X/Y scale. To centre a label on
// the button it replaces, both are measured from their font's glyph table, as DrawString
// lays them out: UFont +60/+64 = the glyphs (24 bytes each, width at +8), +144 = kerning,
// +140 = non-zero when characters go through a remap table. A font that can't be measured
// that way is estimated (9 px a character for the HUD font, 28 px for a button).
// DrawString's return value is not the width.
//
// With keyboard and mouse on (--mnk_mode, ticked in the launcher), each button character is
// drawn instead as the key bound to that button, in the game's own HUD font, e.g. "[F]". The
// binds are the runtime's keybind_* settings, the ones the launcher's Edit keys... writes, so
// a rebind shows up in the prompts at once. Characters we don't know stay as they are (the
// trace logs them), and so does everything with a controller.
//
// The keys themselves are fixed: they are the player's binds (keybind_*, from the launcher)
// and this file never changes them. Each prompt shows the key bound to its button. Menus are
// told apart by text that only they draw (" Accept", " Back", "Confirm", "Cancel", the pause
// menu's "Paused", in the last half second; all text comes from one caller, so the caller
// can't tell), and there a button shows its menu key when it has one: Enter for A,
// Backspace for B (the launcher's defaults are A = Space or Enter, B = F or Backspace,
// Start = Esc). That choice only changes the label; a wrong guess shows the other key of the
// same button, which works as well.
//
//   --keyboard_prompts=false               always the Xbox buttons
//   --keyboard_prompts_in_menus=false      Xbox buttons in menus
//   --keyboard_prompt_centre=false          labels start where the button did
//   --keyboard_mouse_attacks=true           fight prompts: light / heavy attack on fire's and
//                                           aim's keys ([LMB] / [RMB]), for a button on a line
//                                           about "Attack" or "Combo"
//   --keyboard_prompt_scale/dx/dy           size and nudge of gameplay labels
//   --keyboard_menu_prompt_scale/dx/dy      the same for menus

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <rex/input/pad_style.h>

#include "prompts.h"

REXCVAR_DEFINE_STRING(pad_prompts, "auto", "Input",
                      "Controller button prompts: auto (PlayStation symbols when a PlayStation pad is "
                      "connected), xbox, playstation");
REXCVAR_DEFINE_BOOL(keyboard_prompts, true, "UI",
                    "With keyboard and mouse on, show the bound keys in button prompts");
REXCVAR_DEFINE_BOOL(keyboard_prompts_in_menus, true, "UI",
                    "Menus show keys too, Enter / Backspace for A / B where bound (off: Xbox "
                    "buttons in menus)");
REXCVAR_DEFINE_BOOL(keyboard_mouse_attacks, false, "UI",
                    "Fight prompts show light / heavy attack as [LMB] / [RMB] (the launcher sets "
                    "this with its matching option, which binds them in the game's input config)");
REXCVAR_DEFINE_BOOL(keyboard_prompt_centre, true, "UI",
                    "Centre key labels on the button they replace (off: start where it started)");
REXCVAR_DEFINE_DOUBLE(keyboard_prompt_scale, 0.85, "UI", "Size of the key labels in play")
    .range(0.3, 2.0);
REXCVAR_DEFINE_DOUBLE(keyboard_prompt_dx, 0.0, "UI", "Moves the key labels in play right (px)")
    .range(-100.0, 100.0);
REXCVAR_DEFINE_DOUBLE(keyboard_prompt_dy, 4.0, "UI", "Moves the key labels in play down (px)")
    .range(-100.0, 100.0);
REXCVAR_DEFINE_DOUBLE(keyboard_menu_prompt_scale, 0.85, "UI", "Size of the key labels in menus")
    .range(0.3, 2.0);
REXCVAR_DEFINE_DOUBLE(keyboard_menu_prompt_dx, 0.0, "UI", "Moves the key labels in menus right")
    .range(-100.0, 100.0);
REXCVAR_DEFINE_DOUBLE(keyboard_menu_prompt_dy, 4.0, "UI", "Moves the key labels in menus down")
    .range(-100.0, 100.0);

REX_EXTERN(__imp__sub_82C49D88);  // DrawString(FCanvas*, X, Y, ..., text, font, ...)

namespace {

constexpr const char* kButtonFont = "BourneUIPlatformSpecific.ControllerButtonFont";
constexpr const char* kLabelFont = "BourneFonts.HelveticaCondBold_17";

// ControllerButtonFont character -> the runtime's bind setting for that button. From the
// prompts seen so far: "Press Y to use Bourne Instinct", "Press and hold E while moving to
// Sprint" (RB), "W to draw your gun and T to shoot" (W is LB: Bourne's input config has
// LB = WeaponStateButtonPress, the draw / holster), "PRESS START (.)", and A/B/X with Accept/
// Back/Change Storage Device in menus, a/b in small dialogs. Not seen yet: the left
// shoulder, the stick clicks, Back and the D-pad. "o" and "O" are the radar's marker icons,
// not buttons.
const char* BindFor(char16_t c) {
  switch (c) {
    case u'A': case u'a': return "keybind_a";
    case u'B': case u'b': return "keybind_b";
    case u'X': case u'x': return "keybind_x";
    case u'Y': case u'y': return "keybind_y";
    // ControllerButtonFont's other buttons (from its page, logs/font_page_before.tga): the
    // upper-case letter is the large badge, lower-case the small one.
    case u'E': case u'e': return "keybind_right_shoulder";  // RB: sprint
    case u'W': case u'w': return "keybind_left_shoulder";   // LB: draw / holster the gun
    case u'T': case u't': return "keybind_right_trigger";   // RT: shoot
    case u'R': case u'r': return "keybind_left_trigger";    // LT: aim, pop out of cover
    case u'9': return "keybind_lstick_press";                // LS
    case u'(': return "keybind_rstick_press";                // RS
    case u's': return "keybind_dpad_up";
    case u'h': return "keybind_dpad_down";
    case u'k': return "keybind_dpad_left";
    case u'f': return "keybind_dpad_right";
    case u',': return "keybind_back";
    case u'.': return "keybind_start";
    default: return nullptr;
  }
}

// The keys of a bind, in order ("Space,Return" -> Space, Return).
std::vector<std::string> Keys(const std::string& keys) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= keys.size()) {
    const size_t end = std::min(keys.find(',', start), keys.size());
    if (end > start) out.push_back(keys.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

// The key a prompt shows for a bind: the first one, or in menus a menu key (Enter, then
// Backspace, then Esc) when the bind has one.
std::string PickKey(const std::string& keys, bool menu) {
  const std::vector<std::string> list = Keys(keys);
  if (menu) {
    for (const char* menu_key : {"Return", "Backspace", "Escape"}) {
      for (const std::string& k : list) {
        if (k == menu_key) return k;
      }
    }
  }
  return list.empty() ? std::string() : list.front();
}

// A key's label ("Control" -> "[Ctrl]", "Return" -> "[Ent]").
std::string KeyLabel(std::string key) {
  static const std::unordered_map<std::string, std::string> kShort = {
      {"Return", "Ent"},   {"Space", "Spc"},  {"Escape", "Esc"}, {"Control", "Ctrl"}, {"Backspace", "Bksp"},
      {"Backtick", "`"},   {"LBracket", "["}, {"RBracket", "]"},   {"Quote", "'"},
      {"Semicolon", ";"},  {"Comma", ","},    {"Period", "."},     {"Slash", "/"},
      {"Backslash", "\\"}, {"Minus", "-"},    {"Plus", "="},       {"PageUp", "PgUp"},
      {"PageDown", "PgDn"}, {"Delete", "Del"}, {"Insert", "Ins"},  {"CapsLock", "Caps"}};
  if (const auto it = kShort.find(key); it != kShort.end()) {
    key = it->second;
  }
  return key.empty() ? "[?]" : "[" + key + "]";
}

struct State {
  std::mutex mutex;
  std::unordered_map<uint32_t, std::string> font_names;  // UFont -> full name
  uint32_t label_font = 0;                               // the HUD font, once seen
  std::unordered_map<std::string, uint32_t> strings;     // label -> UTF-16 copy in guest memory
  std::unordered_map<std::string, std::string> keynames;  // like labels: the key's own name
  std::unordered_map<std::string, std::string> labels;   // bind setting + screen -> label
  bool keyboard_mode = false;                            // --mnk_mode
  std::chrono::steady_clock::time_point settings_read{};
  std::chrono::steady_clock::time_point menu_seen{};
  std::chrono::steady_clock::time_point title_seen{};  // "PRESS START (" drawn
  // Text drawn lately, with its height on screen: tells what a button's line is about.
  struct Recent {
    double y;
    std::string text;
    std::chrono::steady_clock::time_point when;
  };
  std::vector<Recent> recent;
};

State& S() {
  static State s;
  return s;
}

const std::string& FontName(State& s, const PPCContext& ctx, uint8_t* base, uint32_t font) {
  auto it = s.font_names.find(font);
  if (it == s.font_names.end()) {
    it = s.font_names.emplace(font, tbc::prompts::ObjectPathName(ctx, base, font)).first;
    if (it->second == kLabelFont) {
      s.label_font = font;
    }
  }
  return it->second;
}

// A guest UTF-16 string as ASCII (other characters as '?'), at most 120 characters.
std::string Ascii(const uint8_t* base, uint32_t text) {
  std::string out;
  for (size_t i = 0; i < 120; ++i, text += 2) {
    const uint16_t c = uint16_t(base[text] << 8 | base[text + 1]);
    if (c == 0) {
      break;
    }
    out.push_back(c < 0x80 ? char(c) : '?');
  }
  return out;
}

// Whether the words drawn on the same line as a button, this frame or the last, are about
// fight attacks ("X for Light Attack.", "Chain X and Y together to perform Combos").
bool FightLine(State& s, double y, std::chrono::steady_clock::time_point now) {
  for (const auto& r : s.recent) {
    if (now - r.when < std::chrono::milliseconds(300) && std::abs(r.y - y) <= 14.0 &&
        (r.text.find("Attack") != std::string::npos || r.text.find("Combo") != std::string::npos)) {
      return true;
    }
  }
  return false;
}

// Whether a guest UTF-16 string is exactly this ASCII text.
bool Is(const uint8_t* base, uint32_t text, const char* ascii) {
  for (size_t i = 0;; ++i, text += 2) {
    const uint16_t c = uint16_t(base[text] << 8 | base[text + 1]);
    if (c != uint8_t(ascii[i])) {
      return false;
    }
    if (c == 0) {
      return true;
    }
  }
}

// A label as a 0-terminated UTF-16 (big-endian) string in guest memory, made once and kept.
uint32_t GuestString(State& s, uint8_t* base, const std::string& text) {
  if (const auto it = s.strings.find(text); it != s.strings.end()) {
    return it->second;
  }
  auto* runtime = rex::Runtime::instance();
  const uint32_t addr =
      runtime ? runtime->memory()->SystemHeapAlloc(uint32_t(text.size() + 1) * 2) : 0;
  if (addr) {
    for (size_t i = 0; i <= text.size(); ++i) {
      base[addr + 2 * i] = 0;
      base[addr + 2 * i + 1] = i < text.size() ? uint8_t(text[i]) : 0;
    }
  }
  s.strings.emplace(text, addr);
  return addr;
}

constexpr auto kRecent = std::chrono::milliseconds(500);
std::atomic<int64_t> g_title_until{0};
std::atomic<int64_t> g_menu_until{0};  // a menu's prompts (Accept, Back, Paused...) drawn until then  // steady_clock ticks: PRESS START on screen until then

// Binds and --mnk_mode are read again every half second, so a change made while the game
// runs (the runtime's settings overlay) shows up in the labels.
void Refresh(State& s, std::chrono::steady_clock::time_point now) {
  if (now - s.settings_read > kRecent) {
    s.labels.clear();
    s.keynames.clear();
    s.keyboard_mode = rex::cvar::Query<bool>("mnk_mode");
    s.settings_read = now;
  }
}

// The key a prompt shows for this button ("F", "LMB", "Return").
const std::string& KeyFor(State& s, const char* bind, bool menu, bool fight) {
  const std::string key = std::string(bind) + (menu ? "@menu" : "") + (fight ? "@fight" : "");
  auto it = s.keynames.find(key);
  if (it == s.keynames.end()) {
    const std::string name = bind;
    std::string keys;
    if (fight && name == "keybind_x") {
      // In fights the triggers attack (launcher option): light on fire's key, heavy on aim's.
      keys = rex::cvar::GetFlagByName("keybind_right_trigger");
    } else if (fight && name == "keybind_y") {
      keys = rex::cvar::GetFlagByName("keybind_left_trigger");
    } else {
      keys = rex::cvar::GetFlagByName(bind);
    }
    it = s.keynames.emplace(key, PickKey(keys, menu)).first;
  }
  return it->second;
}

const std::string& LabelFor(State& s, const char* bind, bool menu, bool fight) {
  const std::string key = std::string(bind) + (menu ? "@menu" : "") + (fight ? "@fight" : "");
  auto it = s.labels.find(key);
  if (it == s.labels.end()) {
    it = s.labels.emplace(key, KeyLabel(KeyFor(s, bind, menu, fight))).first;
  }
  return it->second;
}

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

// The width of a 0-terminated UTF-16 string in a font, at scale 1: the sum of each glyph's
// width plus the font's kerning. Negative if the font can't be measured this way.
double TextWidth(const uint8_t* base, uint32_t font, uint32_t text) {
  if (Load32(base, font + 140) != 0) {
    return -1.0;
  }
  const uint32_t glyphs = Load32(base, font + 60);
  const uint32_t count = Load32(base, font + 64);
  const int32_t kerning = int32_t(Load32(base, font + 144));
  if (!glyphs || count == 0 || count > 65536) {
    return -1.0;
  }
  double width = 0.0;
  for (uint32_t i = 0; i < 64; ++i, text += 2) {
    const uint32_t c = uint32_t(base[text] << 8 | base[text + 1]);
    if (c == 0) {
      break;
    }
    const uint32_t index = c < count ? c : (127 < count ? 127 : 0);
    width += double(int32_t(Load32(base, glyphs + index * 24 + 8)) + kerning);
  }
  return width;
}

size_t Length(const uint8_t* base, uint32_t text) {
  size_t n = 0;
  while (n < 64 && (base[text + 2 * n] | base[text + 2 * n + 1]) != 0) {
    ++n;
  }
  return n;
}

// DrawString with other text and font, the rest of the call as the game made it. The
// caller's registers are left as they were.
uint32_t Draw(PPCContext& ctx, uint8_t* base, const PPCContext& call, uint32_t text,
              uint32_t font, double x, double y, double scale) {
  ctx = call;
  ctx.r6.u64 = text;
  ctx.r7.u64 = font;
  ctx.f1.f64 = x;
  ctx.f2.f64 = y;
  ctx.f3.f64 = call.f3.f64 * scale;
  ctx.f4.f64 = call.f4.f64 * scale;
  __imp__sub_82C49D88(ctx, base);
  return ctx.r3.u32;
}

}  // namespace

namespace tbc::glyphs {
void ProbeFont(const PPCContext& ctx, uint8_t* base, uint32_t font);  // glyph_probe.cpp
void Install(uint8_t* base, uint32_t font);                           // key_glyphs.cpp
uint32_t CharFor(const std::string& key);                             // key_glyphs.cpp
void PatchHud(const PPCContext& ctx, uint8_t* base,
              const std::function<std::string(const char*)>& key_for);  // key_glyphs.cpp
void PatchPadPrompts(const PPCContext& ctx, uint8_t* base, bool playstation);  // key_glyphs.cpp
}

extern "C" REX_FUNC(sub_82C49D88) {
  tbc::prompts::TraceText(ctx, base);
  if (ctx.r7.u32) tbc::glyphs::ProbeFont(ctx, base, ctx.r7.u32);

  const uint32_t text = ctx.r6.u32;
  const uint32_t font = ctx.r7.u32;
  if (!text || !font || !REXCVAR_GET(keyboard_prompts)) {
    __imp__sub_82C49D88(ctx, base);
    return;
  }

  State& s = S();
  uint32_t label_text = 0;
  uint32_t label_font = 0;
  bool glyph_label = false;
  bool menu = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (Is(base, text, " Accept") || Is(base, text, " Back") || Is(base, text, "Confirm") ||
        Is(base, text, "Cancel") || Is(base, text, "Paused")) {
      if (now - s.menu_seen >= kRecent) {
        REXLOG_INFO("keyboard_prompts: menu, from the text \"{}\"", Ascii(base, text));
      }
      s.menu_seen = now;
      g_menu_until.store((now + kRecent).time_since_epoch().count());
    }
    menu = now - s.menu_seen < kRecent;
    // The title screen's "PRESS START (Start)": A takes it too, so it shows A's menu key
    // (Enter) rather than Start's (Esc, which reads like "quit").
    if (Is(base, text, "PRESS START (")) {
      s.title_seen = now;
      g_title_until.store((now + kRecent).time_since_epoch().count());
    }
    const bool title = now - s.title_seen < kRecent;
    if (REXCVAR_GET(keyboard_mouse_attacks)) {
      // Remember the words on screen and where (not the buttons themselves).
      if (base[text + 2] != 0 || base[text + 3] != 0) {
        if (s.recent.size() >= 48) {
          s.recent.erase(s.recent.begin());
        }
        s.recent.push_back({ctx.f2.f64, Ascii(base, text), now});
      }
    }
    Refresh(s, now);
    // Every font is looked up (once each), so the HUD font is known by the first button.
    const bool button_font = FontName(s, ctx, base, font) == kButtonFont;
    if (button_font) {
      tbc::glyphs::Install(base, font);  // once: key glyphs into the font (key_glyphs.cpp)
    }
    if (s.keyboard_mode && !(menu && !REXCVAR_GET(keyboard_prompts_in_menus)) && button_font &&
        s.label_font) {
      // A button is one character: anything longer is left alone.
      const char16_t c = char16_t(base[text] << 8 | base[text + 1]);
      const bool single = base[text + 2] == 0 && base[text + 3] == 0;
      const char* bind = single ? BindFor(c) : nullptr;
      if (bind && title && c == u'.') {
        bind = "keybind_a";
      }
      if (bind) {
        const bool fight = REXCVAR_GET(keyboard_mouse_attacks) && !menu &&
                           (c == u'X' || c == u'Y') && FightLine(s, ctx.f2.f64, now);
        const bool as_menu = menu || title;
        if (const uint32_t glyph = tbc::glyphs::CharFor(KeyFor(s, bind, as_menu, fight))) {
          // The key's own glyph, one character of the button font itself.
          label_text = GuestString(s, base, std::string(1, char(glyph)));
          label_font = font;
          glyph_label = true;
        } else {
          label_text = GuestString(s, base, LabelFor(s, bind, as_menu, fight));
          label_font = s.label_font;
        }
      }
    }
  }
  if (!label_text) {
    __imp__sub_82C49D88(ctx, base);
    return;
  }

  // Draw the label instead, centred where the button would have been.
  const PPCContext call = ctx;
  // A glyph is drawn like the button it replaces: same font, size and place.
  const double scale = glyph_label ? 1.0
                       : menu      ? REXCVAR_GET(keyboard_menu_prompt_scale)
                                   : REXCVAR_GET(keyboard_prompt_scale);
  const double dx = glyph_label ? 0.0
                    : menu      ? REXCVAR_GET(keyboard_menu_prompt_dx)
                                : REXCVAR_GET(keyboard_prompt_dx);
  const double dy = glyph_label ? 0.0
                    : menu      ? REXCVAR_GET(keyboard_menu_prompt_dy)
                                : REXCVAR_GET(keyboard_prompt_dy);
  constexpr double kTypicalButton = 28.0;  // px, when the button font can't be measured
  constexpr double kHudCharacter = 9.0;    // px per character of the HUD font, likewise
  double button_width = TextWidth(base, font, text);
  if (button_width <= 0.0) {
    button_width = kTypicalButton;
  }
  double label_width = TextWidth(base, label_font, label_text);
  if (label_width <= 0.0) {
    label_width = kHudCharacter * double(Length(base, label_text));
  }
  button_width *= call.f3.f64;
  label_width *= call.f3.f64 * scale;
  static int logged = 0;
  if (logged < 8) {
    ++logged;
    REXLOG_INFO("keyboard_prompts: {} label, button {:.1f} px wide, label {:.1f} px wide",
                menu ? "menu" : "gameplay", button_width, label_width);
  }
  double centre = 0.0;
  if (REXCVAR_GET(keyboard_prompt_centre) && label_width > 0.0 && label_width < 1024.0 &&
      button_width < 1024.0) {
    centre = (button_width - label_width) / 2.0;
  }
  const uint32_t result = Draw(ctx, base, call, label_text, label_font,
                               call.f1.f64 + centre + dx, call.f2.f64 + dy, scale);
  // The caller gets DrawString's own result back (it is the same whatever the text).
  ctx = call;
  ctx.r3.u64 = result;
}

namespace tbc::prompts {
// HUD textures with button badges (door, takedown meter, QTE) get the keys' glyphs
// (key_glyphs.cpp). Called on every controller poll (input_trace.cpp), which happens every
// frame on the game's thread; the work itself is done at most once a second. "@qte" badges show
// the W / A / S / D directions that answer QTEs (qte.cpp).
void PatchHudTextures(const PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(keyboard_prompts)) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  Refresh(s, std::chrono::steady_clock::now());
  if (!s.keyboard_mode) {
    // A controller: its own kind of button prompts (--pad_prompts: auto follows the pad).
    const std::string mode = REXCVAR_GET(pad_prompts);
    const bool ps = mode == "playstation" ||
                    (mode == "auto" && rex::input::ConnectedPadStyle() == rex::input::PadStyle::kPlayStation);
    tbc::glyphs::PatchPadPrompts(ctx, base, ps);
    return;
  }
  tbc::glyphs::PatchHud(ctx, base, [&s](const char* bind) -> std::string {
    static const std::unordered_map<std::string, const char*> kQte = {
        {"keybind_a@qte", "keybind_lstick_down"}, {"keybind_b@qte", "keybind_lstick_right"},
        {"keybind_x@qte", "keybind_lstick_left"}, {"keybind_y@qte", "keybind_lstick_up"}};
    std::string name = bind;
    if (const auto it = kQte.find(name); it != kQte.end()) {
      name = rex::cvar::Query<bool>("qte_wasd") ? it->second : name.substr(0, name.size() - 4);
    }
    return KeyFor(s, name.c_str(), false, false);
  });
}

bool MenuUp() {
  return std::chrono::steady_clock::now().time_since_epoch().count() < g_menu_until.load();
}

bool TitleScreenUp() {
  return std::chrono::steady_clock::now().time_since_epoch().count() < g_title_until.load() &&
         rex::cvar::Query<bool>("mnk_mode");
}
}  // namespace tbc::prompts
