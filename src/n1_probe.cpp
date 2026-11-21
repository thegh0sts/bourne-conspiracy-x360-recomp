// tbc - n1 probe (--n1_probe): what Skullburglar's n1 renderer would need to read Bourne's live scene.
//
// n1's scene feed (skullburglar src/scene_feed.cpp, written for 50 Cent: Blood on the Sand) reads every primitive
// component from the object table and finds each field by name through UE3's reflection data. This probe finds,
// in Bourne's running game, the addresses and offsets that feed needs and logs them, 30 s after the first script
// event and then every 60 s, six times (so some runs are in a level):
//   - FName::Names: the global TArray<FNameEntry*> (Data, Num, Max), and where an entry's text starts;
//   - the UObject header: Name (+0x2C), Class (+0x30), Outer (+0x28), checked against the name table;
//   - UField::Next, UStruct::Children and UProperty::Offset, found on StaticMeshComponent's reflection data and
//     checked on live components (their StaticMesh pointers must name StaticMesh objects);
//   - a census of the live primitive components by class.
// Read-only; off by default.

#include "generated/default/tbc_pch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(n1_probe, false, "Debug",
                    "Development: log the name table, object layout and reflection offsets n1's scene feed needs");

namespace {

constexpr uint32_t kObjects = 0x83655774;  // GObjObjects {Data, Num, Max}
constexpr uint32_t kOuter = 0x28, kName = 0x2C, kClass = 0x30;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}

bool Readable(uint32_t addr) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return false;
  auto* heap = runtime->memory()->LookupHeap(addr);
  uint32_t protect = 0;
  return heap && heap->QueryProtect(addr, &protect) && (protect & rex::memory::kMemoryProtectRead);
}

bool Ptr(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && (a & 3) == 0; }

// The name table, once found.
struct Names {
  uint32_t array = 0, data = 0, count = 0, text = 0;  // array: the TArray; text: the characters' offset in an entry
  bool wide = false;
} g_names;

std::string EntryText(const uint8_t* base, uint32_t entry, uint32_t text, bool wide) {
  std::string s;
  // Entries are packed back to back (UTF-16 text, so only 2-byte aligned).
  if (entry < 0x40000000 || entry >= 0xA0000000 || (entry & 1) || !Readable(entry + text)) return s;
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t c = wide ? (uint32_t(base[entry + text + 2 * i]) << 8 | base[entry + text + 2 * i + 1])
                            : base[entry + text + i];
    if (!c) break;
    if (c < 0x20 || c > 0x7E) return {};
    s.push_back(char(c));
  }
  return s;
}

// Bourne's FName is one word: name index << 14 | number (FName::AppendString, sub_8290CCB8: the entry is
// Names.Data[word >> 14], its text at +0x10; a number n > 0 adds "_<n - 1>").
std::string Name(const uint8_t* base, uint32_t word) {
  const uint32_t index = word >> 14, number = word & 0x3FFF;
  if (!g_names.data || index >= g_names.count) return "<name " + std::to_string(index) + ">";
  std::string s = EntryText(base, Load32(base, g_names.data + 4 * index), g_names.text, g_names.wide);
  if (number) s += "_" + std::to_string(number - 1);
  return s;
}

std::string ObjName(const uint8_t* base, uint32_t obj) {
  if (!Ptr(obj) || !Readable(obj + kClass)) return "<bad>";
  return Name(base, Load32(base, obj + kName));
}

std::string ClassName(const uint8_t* base, uint32_t obj) {
  if (!Ptr(obj) || !Readable(obj + kClass)) return "<bad>";
  return ObjName(base, Load32(base, obj + kClass));
}

// FName::Names: a TArray in the image's data whose first entries are UE3's hard-coded names.
constexpr uint32_t kNamesArray = 0x836556E0;  // FName::Names {Data, Num, Max}, from FName::AppendString

void FindNames(const uint8_t* base) {
  if (g_names.array) {  // found in an earlier run: the table grows, so read it again
    g_names.data = Load32(base, g_names.array), g_names.count = Load32(base, g_names.array + 4);
    return;
  }
  {
    const uint32_t data = Load32(base, kNamesArray), num = Load32(base, kNamesArray + 4);
    REXLOG_INFO("n1_probe: Names at {:08X}: data {:08X}, num {}, max {}", kNamesArray, data, num,
                Load32(base, kNamesArray + 8));
    for (uint32_t i = 0; i < 4 && Ptr(data) && Readable(data + 4 * i); ++i) {
      const uint32_t e = Load32(base, data + 4 * i);
      std::string hex;
      for (uint32_t k = 0; Ptr(e) && Readable(e) && k < 40; ++k) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02X", base[e + k]);
        hex += b;
        if (k % 4 == 3) hex += ' ';
      }
      REXLOG_INFO("n1_probe:   entry {} at {:08X}: {}", i, e, hex);
    }
    for (bool wide : {false, true}) {
      if (Ptr(data) && EntryText(base, Load32(base, data), 16, wide) == "None") {
        g_names = {kNamesArray, data, num, 16, wide};
        REXLOG_INFO("n1_probe: name 0 is \"None\" ({}); name 1 \"{}\"", wide ? "UTF-16" : "ANSI",
                    EntryText(base, Load32(base, data + 4), 16, wide));
        return;
      }
    }
  }
  for (uint32_t a = 0x82000000; a < 0x84000000; a += 4) {
    if ((a & 0xFFF) == 0 && !Readable(a)) {
      a += 0xFFC;
      continue;
    }
    const uint32_t data = Load32(base, a), num = Load32(base, a + 4), max = Load32(base, a + 8);
    if (!Ptr(data) || num < 1000 || num > 2000000 || max < num || max > 4000000 || !Readable(data + 8)) continue;
    const uint32_t e0 = Load32(base, data), e1 = Load32(base, data + 4);
    for (uint32_t text : {8u, 12u, 16u, 4u, 0u}) {
      for (bool wide : {false, true}) {
        if (EntryText(base, e0, text, wide) == "None" && EntryText(base, e1, text, wide) == "ByteProperty") {
          g_names = {a, data, num, text, wide};
          REXLOG_INFO("n1_probe: FName::Names at {:08X}: data {:08X}, {} names (max {}), text at entry +{:X} ({})", a,
                      data, num, max, text, wide ? "UTF-16" : "ANSI");
          return;
        }
      }
    }
  }
  REXLOG_INFO("n1_probe: FName::Names not found in 82000000-84000000");
}

std::vector<uint32_t> AllObjects(const uint8_t* base) {
  std::vector<uint32_t> out;
  const uint32_t data = Load32(base, kObjects), count = std::min<uint32_t>(Load32(base, kObjects + 4), 3000000);
  for (uint32_t i = 0; i < count; ++i) {
    if ((i & 1023) == 0 && !Readable(data + 4 * i)) break;
    out.push_back(Load32(base, data + 4 * i));
  }
  return out;
}

// The class object whose name is `name` (and whose own class is "Class").
uint32_t FindClass(const uint8_t* base, const std::vector<uint32_t>& objs, const std::string& name) {
  for (uint32_t o : objs) {
    if (!Ptr(o) || !Readable(o + kClass)) continue;
    if (ObjName(base, o) == name && ClassName(base, o) == "Class") return o;
  }
  return 0;
}

bool IsSubclass(const uint8_t* base, uint32_t cls, uint32_t of, uint32_t super_off) {
  for (int i = 0; cls && i < 32; ++i) {
    if (cls == of) return true;
    if (!Ptr(cls) || !Readable(cls + super_off)) return false;
    cls = Load32(base, cls + super_off);
  }
  return false;
}

void Run(const uint8_t* base) {
  REXLOG_INFO("n1_probe: GObjObjects {:08X}: data {:08X}, {} objects", kObjects, Load32(base, kObjects),
              Load32(base, kObjects + 4));
  FindNames(base);
  if (!g_names.data) return;
  const std::vector<uint32_t> objs = AllObjects(base);

  // UObject header: each object's +0x04 should be its own index, +0x2C a name, +0x30 a class named "Class"'s child.
  uint32_t index_ok = 0, named = 0, checked = 0;
  for (uint32_t i = 0; i < objs.size() && checked < 5000; i += std::max<uint32_t>(1, uint32_t(objs.size() / 5000))) {
    const uint32_t o = objs[i];
    if (!Ptr(o) || !Readable(o + kClass)) continue;
    ++checked;
    index_ok += Load32(base, o + 4) == i;
    if (checked <= 3) REXLOG_INFO("n1_probe:   sample object {:08X} name word {:08X}: {}", o, Load32(base, o + kName), ObjName(base, o));
    named += !ObjName(base, o).empty() && !ClassName(base, o).empty();
  }
  REXLOG_INFO("n1_probe: header check on {} objects: +04 is the index for {}, +2C/+30 name a named class for {}",
              checked, index_ok, named);
  for (uint32_t i = 0; i < 8 && i < objs.size(); ++i) {
    REXLOG_INFO("n1_probe:   object {} {:08X}: {} ({}), outer {}", i, objs[i], ObjName(base, objs[i]),
                ClassName(base, objs[i]), ObjName(base, Load32(base, objs[i] + kOuter)));
  }

  // Reflection. Candidates are checked on StaticMeshComponent: its Children chain (UStruct::Children, then
  // UField::Next) must hold a property named "StaticMesh"; the property's Offset field must point, in live
  // components, at objects of class StaticMesh.
  const uint32_t smc = FindClass(base, objs, "StaticMeshComponent");
  const uint32_t sm_class = FindClass(base, objs, "StaticMesh");
  const uint32_t prim = FindClass(base, objs, "PrimitiveComponent");
  REXLOG_INFO("n1_probe: classes StaticMeshComponent {:08X}, StaticMesh {:08X}, PrimitiveComponent {:08X}", smc,
              sm_class, prim);
  if (!smc || !sm_class) return;
  std::string words;
  for (uint32_t k = 0; k < 0x100; k += 4) {
    char b[16];
    std::snprintf(b, sizeof(b), "%s%08X", k % 32 ? " " : "\n    ", Load32(base, smc + k));
    words += b;
  }
  REXLOG_INFO("n1_probe: StaticMeshComponent class object:{}", words);
  // SuperField: a word in the class pointing at another class object (MeshComponent).
  uint32_t super_off = 0;
  for (uint32_t k = 0x34; k < 0x100; k += 4) {
    const uint32_t v = Load32(base, smc + k);
    if (Ptr(v) && Readable(v + kClass) && ClassName(base, v) == "Class" && ObjName(base, v) == "MeshComponent") {
      super_off = k;
      break;
    }
  }
  REXLOG_INFO("n1_probe: UStruct::SuperField +{:X}", super_off);
  // Children: a word in the class pointing at an object whose outer is the class; Next: the offset in that object
  // that walks to further children with the same outer.
  uint32_t children_off = 0, next_off = 0, prop = 0;
  for (uint32_t k = 0x34; k < 0x100 && !prop; k += 4) {
    const uint32_t first = Load32(base, smc + k);
    if (!Ptr(first) || !Readable(first + kClass) || Load32(base, first + kOuter) != smc) continue;
    for (uint32_t n = 0x34; n < 0x60 && !prop; n += 4) {
      uint32_t f = first;
      int steps = 0;
      for (; f && steps < 200; ++steps) {
        if (!Ptr(f) || !Readable(f + n) || Load32(base, f + kOuter) != smc) break;
        if (ObjName(base, f) == "StaticMesh" && ClassName(base, f) == "ObjectProperty") {
          prop = f, children_off = k, next_off = n;
          break;
        }
        f = Load32(base, f + n);
      }
    }
  }
  REXLOG_INFO("n1_probe: UStruct::Children +{:X}, UField::Next +{:X}, StaticMesh property {:08X}", children_off,
              next_off, prop);
  if (!prop) return;
  words.clear();
  for (uint32_t k = 0; k < 0x90; k += 4) {
    char b[16];
    std::snprintf(b, sizeof(b), "%s%08X", k % 32 ? " " : "\n    ", Load32(base, prop + k));
    words += b;
  }
  REXLOG_INFO("n1_probe: StaticMesh property object:{}", words);
  // Live components (StaticMeshComponent and subclasses).
  std::vector<uint32_t> comps;
  for (uint32_t o : objs) {
    if (Ptr(o) && Readable(o + kClass) && super_off && IsSubclass(base, Load32(base, o + kClass), smc, super_off)) {
      comps.push_back(o);
    }
  }
  // UProperty::Offset: the property word that, as an offset into live components, gives StaticMesh objects.
  uint32_t offset_field = 0, best = 0;
  for (uint32_t k = 0x40; k < 0x90; k += 4) {
    const uint32_t off = Load32(base, prop + k);
    if (off < 0x40 || off > 0x1000 || (off & 3)) continue;
    uint32_t hits = 0;
    for (size_t c = 0; c < comps.size() && c < 400; ++c) {
      const uint32_t v = Load32(base, comps[c] + off);
      hits += Ptr(v) && Readable(v + kClass) && Load32(base, v + kClass) == sm_class;
    }
    if (hits > best) best = hits, offset_field = k;
  }
  REXLOG_INFO("n1_probe: {} live StaticMeshComponents; UProperty::Offset +{:X} (value {:X}): {} of the first {} "
              "point at StaticMesh objects",
              comps.size(), offset_field, offset_field ? Load32(base, prop + offset_field) : 0, best,
              std::min<size_t>(comps.size(), 400));
  if (offset_field) {
    const uint32_t off = Load32(base, prop + offset_field);
    for (size_t c = 0; c < comps.size() && c < 6; ++c) {
      REXLOG_INFO("n1_probe:   {} {} -> {}", ClassName(base, comps[c]), ObjName(base, comps[c]),
                  ObjName(base, Load32(base, comps[c] + off)));
    }
  }
  // Census: live objects by class, for the primitive-component classes.
  if (prim && super_off) {
    std::map<std::string, uint32_t> census;
    for (uint32_t o : objs) {
      if (!Ptr(o) || !Readable(o + kClass)) continue;
      const uint32_t cls = Load32(base, o + kClass);
      if (IsSubclass(base, cls, prim, super_off)) ++census[ObjName(base, cls)];
    }
    std::string s;
    for (const auto& [k, v] : census) s += " " + k + " " + std::to_string(v) + ",";
    REXLOG_INFO("n1_probe: primitive components by class:{}", s);
  }
}

}  // namespace

namespace tbc::n1probe {

// From the ProcessEvent hook (script_trace.cpp), on the game thread.
void OnProcessEvent(const PPCContext&, uint8_t* base) {
  if (!REXCVAR_GET(n1_probe)) return;
  // At 30 s, then every 60 s, six times: the later runs are in a level.
  static const auto first = std::chrono::steady_clock::now();
  static int runs = 0;
  if (runs >= 6 || std::chrono::steady_clock::now() - first < std::chrono::seconds(30 + 60 * runs)) return;
  ++runs;
  REXLOG_INFO("n1_probe: run {} of 6", runs);
  Run(base);
}

}  // namespace tbc::n1probe
