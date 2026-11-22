// tbc - scene feed, from Skullburglar's src/scene_feed.cpp (BSD 3-Clause, LICENSE.skullburglar in tools/n1probe;
// commit 67aacb28, written for 50 Cent: Blood on the Sand), ported to The Bourne Conspiracy for the n1 renderer work
// (claude/bourne-n1-feasibility.md). Only the research part (JSON dumps, timing, census); n1's binary live feed is not
// here yet. Bourne's differences, found with src/n1_probe.cpp:
//   - FName is one word, name index << 14 | number (High Moon); FName::Names is at 0x836556E0, an entry's text is
//     UTF-16 at +0x10 (50 Cent: two words, ANSI at +0x08, Names at 0x837F98A0);
//   - UObject +0x30 Class (50 Cent +0x34); UStruct +0x44 Children (+0x50); UProperty +0x60 Offset (+0x64);
//   - GObjObjects is at 0x83655774; GEngine is found in the object table (the GameEngine instance).
// Commands come from --scenefeed ("dump <frames> [every]", "time <frames> [cached]", "census", "emitters <frames>"),
// run --scenefeed_delay seconds after the first frame; output under --scenefeed_dir.
//
// Original description:
//
// Scene feed (research spike for a renderer that draws the UE3 scene itself instead of replaying the GPU stream).
//
// Reads, on the game thread once per frame, every primitive component attached to the renderer's scene (UE3's
// UPrimitiveComponent with a non-null SceneInfo) from the live object table, plus the player's camera, and writes
// one JSON file per frame. Every field is found by name through UE3's own reflection data (UProperty +0x64 Offset),
// so nothing below depends on a class layout except the object header and the reflection records themselves.
//
// Object layout (this build, as used elsewhere in src/): UObject +0x04 Index, +0x28 Outer, +0x2C Name (FName: index,
// number), +0x34 Class; UField +0x3C SuperField, +0x40 Next; UStruct +0x50 Children; UProperty +0x64 Offset;
// UBoolProperty +0x84 BitMask. FName::Names is a TArray<FNameEntry*> at 0x837F98A0, an entry's text at +8.
// The object table (UObject::GObjObjects, a TArray<UObject*>) is found once by scanning the image's data for an
// array whose entries' Index fields count up from 0.
//
// Commands (BOTS_EXEC / BOTS_EXEC_FILE; output under BOTS_SCENEFEED_DIR, default "scenefeed"):
//   scenefeed dump <frames> [every]  write frame_<n>.json for the next <frames> frames (every <every>th frame)
//   scenefeed time <frames> [cached] walk without writing and log the cost per stage; "cached" re-reads a primitive
//                                    list found by a full table walk every 64 frames (the incremental-tracking bound)
//   scenefeed census                 log every primitive component class: instances, attached, hidden, rendered
//   scenefeed draws <frames>         ask the native renderer to dump its next <frames> frames of draws (VS constants
//                                    and pass info) next to the JSON, for matching draws to primitives offline
//   scenefeed emitters <frames>      research: write each particle component's emitter instances raw (see DumpEmitters)
//
// JSON (one object per file):
//   frame, time (WorldInfo.TimeSeconds), real_time, host_us (steady clock, microseconds; the renderer's draw dumps
//   use the same clock), camera {location[3], rotation[3] (UE3 units, 65536 = 360 degrees: pitch, yaw, roll), fov
//   (horizontal, degrees), view[16], proj[16]}, primitives [ {kind, class, name (component path), owner, owner_class,
//   mesh, transform[16], materials[], bounds {origin[3], extent[3], radius}, hidden, rendered, last_render_time,
//   lod, bones[[16]...], emitters[...]} ], skeletons {mesh: {names[], parents[]}}, lights [...], fog [...], post {...}.
//   emitters (particle primitives only; one per FParticleEmitterInstance): {index, type (sprite, mesh, beam, trail or
//   other, from the LOD level's TypeDataModule class), type_class, vtable, emitter (ParticleEmitter path), material,
//   screen_alignment (EParticleScreenAlignment), local_space, sub_images [h, v], sub_uv_method, mesh, mesh_alignment,
//   override_material (mesh emitters only), sub_uv_offset (byte offset of a particle's [ImageH, ImageV, Interp,
//   Image2H, Image2V], sub-UV emitters), location[3], active, max, stride, particles [[location 3, old_location 3,
//   size 3, rotation (radians), color 4 (linear RGBA), relative_time (0..1 of the lifetime), payload words...]]}.
//   A sprite particle's quad is centred on location (or old_location, depending on the material's motion blur);
//   a mesh particle's transform is translation(location) with the mesh emitter's rotation and size.
//   lights (every attached LightComponent: world lights and the ones DynamicLightEnvironments make for characters
//   and movers), fog (attached HeightFogComponent and FogVolumeDensityComponent): {class, name, props}, where props
//   holds every script property below ActorComponent, by name, as reflection describes it (see Members): for lights
//   LightToWorld (row 2, its Z axis, is the direction light travels: the owner's forward axis), Brightness, LightColor [R, G, B, A],
//   Radius, FalloffExponent, CastShadows, CastDynamicShadows, LightEnvironment, LightShadowMode, Inner/OuterConeAngle
//   (spot), LowerBrightness/LowerColor (sky), WorldSpaceIncidentLighting (spherical-harmonic environment lights).
//   post: {chain [{class, name, props}] (the local player's PlayerPostProcess effects, e.g. SfUberPostProcessEffect,
//   SfGodRaysEffect, MaterialEffect), chain_name, current (LocalPlayer.CurrentPPInfo: LastSettings, the blended
//   PostProcessSettings the renderer uses, and LastVolumeUsed), world_default (WorldInfo.DefaultPostProcessSettings),
//   camera_settings / camera_override_alpha when the camera class has them}. Enum values are written by name.
//   Matrices are UE3 FMatrix as stored: 16 floats, row-major, row vectors (v' = v * M, translation in row 3).
//   World space is UE3's: left-handed, Z up, 1 unit = 1 cm by convention.
#include "generated/default/tbc_pch.h"

#include "scene_feed.h"
#include "hybrid_state.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>


REXCVAR_DEFINE_STRING(scenefeed, "", "Debug",
                      "Development (n1): a scene feed command, e.g. \"dump 5 30\" (5 frames, every 30th) or \"census\"");
REXCVAR_DEFINE_INT32(scenefeed_delay, 120, "Debug", "Development (n1): seconds after the first frame to run --scenefeed");
REXCVAR_DEFINE_STRING(n1_feed, "lights,fog,post,chars", "Debug",
                      "Development (n1): what fill 4 sends besides meshes: any of lights, fog, post, chars (each "
                      "character lit by the level lights near it), envs (the game's own light environments; Bourne's "
                      "hold zero-brightness lights, so off by default), or none");
REXCVAR_DEFINE_DOUBLE(n1_fog_density, 1.5, "Debug",
                      "Development (n1): scale on the height fog's Density sent to n1 (n1's formula is 50 Cent's)");
REXCVAR_DEFINE_INT32(n1_fog_color, 2, "Debug",
                     "Development (n1): the fog colour n1 draws: 0 as 50 Cent (pow(LightColor / 255, 2.2)), 1 LightColor / "
                     "255, 2 LightColor / 255 * LightBrightness");
REXCVAR_DEFINE_STRING(n1_trace, "", "Debug",
                      "Development (n1): log once what the scene feed sends for each static mesh whose mesh or "
                      "baked-light path contains this text (case ignored; * for all): mesh, baked-light path, "
                      "materials, position");
REXCVAR_DEFINE_STRING(scenefeed_dir, "scenefeed", "Debug", "Development (n1): where --scenefeed writes its files");

namespace tbc::scenefeed {
namespace {
uint8_t* g_base = nullptr;

uint8_t* Raw(uint32_t address) { return g_base + (uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u)); }
uint32_t U32(uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Raw(address), 4);
  return __builtin_bswap32(v);
}
float F32(uint32_t address) { return std::bit_cast<float>(U32(address)); }
bool Readable(uint32_t address) { return address >= 0x40000000u && address < 0xA0000000u && !(address & 3); }
// Whether [address, address + size) is committed guest memory. Readable() only checks the address range; a pointer taken
// from a structure the engine is freeing (a chapter change) can point at unmapped pages, and reading those kills the
// process. Used for the pointers the live material path follows (mesh render data, parameter arrays).
bool Mapped(uint32_t address, uint32_t size) {
  if (!Readable(address) || uint64_t(address) + size > 0xA0000000ull) return false;
  auto* heap = rex::system::kernel_memory()->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(address, &info)) return false;
  return (info.state & rex::memory::kMemoryAllocationCommit) && info.protect &&
         uint64_t(address) + size <= uint64_t(info.base_address) + info.region_size;
}

constexpr uint32_t kNames = 0x836556E0;           // FName::Names {Data, Num, Max}
constexpr uint32_t kObjectTable = 0x83655774;     // UObject::GObjObjects {Data, Num, Max}
constexpr uint32_t kClass = 0x30, kChildren = 0x44, kPropOffset = 0x60;

// Bourne's FName: one word, index << 14 | number. Name text is UTF-16 in the entry; kept here as ASCII, per index.
const char* NameText(uint32_t word) {
  static std::unordered_map<uint32_t, std::string> cache;
  const uint32_t index = word >> 14;
  if (auto it = cache.find(index); it != cache.end()) return it->second.c_str();
  const uint32_t names = U32(kNames);
  if (!Readable(names) || index >= U32(kNames + 4)) return "?";
  const uint32_t entry = U32(names + 4 * index);
  if (entry < 0x40000000u || entry >= 0xA0000000u || (entry & 1)) return "?";
  std::string s;
  for (uint32_t i = 0; i < 256; ++i) {
    const uint8_t* p = Raw(entry + 0x10 + 2 * i);
    const uint16_t c = uint16_t(p[0] << 8 | p[1]);
    if (!c) break;
    s.push_back(c < 0x80 ? char(c) : '?');
  }
  return cache.emplace(index, std::move(s)).first->second.c_str();
}

std::string ObjName(uint32_t object) {
  if (!Readable(object)) return "None";
  const uint32_t word = U32(object + 0x2C);
  std::string out = NameText(word);
  if (const uint32_t number = word & 0x3FFF) out += "_" + std::to_string(number - 1);
  return out;
}

const char* ClassName(uint32_t object) {
  const uint32_t cls = Readable(object) ? U32(object + kClass) : 0;
  return Readable(cls) ? NameText(U32(cls + 0x2C)) : "None";
}

// Package.Group.Object
std::string Path(uint32_t object) {
  if (!Readable(object)) return "";
  uint32_t chain[16];
  int n = 0;
  for (uint32_t o = object; Readable(o) && n < 16; o = U32(o + 0x28)) chain[n++] = o;
  std::string out;
  for (int i = n - 1; i >= 0; --i) {
    if (!out.empty()) out += '.';
    out += ObjName(chain[i]);
  }
  return out;
}

constexpr uint32_t kBoolMask = 0x80;  // UBoolProperty BitMask (Bourne: 4 below 50 Cent's +0x84; checked by the census)
// A property subclass's first own field: UBoolProperty BitMask, UArrayProperty Inner, UStructProperty Struct,
// UByteProperty Enum (50 Cent +0x84; Bourne's UProperty is 4 bytes shorter).
constexpr uint32_t kSubFirst = 0x80;

struct Field {
  uint32_t offset = 0xFFFFFFFF, mask = 0;
  bool ok() const { return offset != 0xFFFFFFFF; }
};

// A property of the class or its superclasses, by name.
Field FindProp(uint32_t cls, const char* name) {
  for (int depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    for (uint32_t f = U32(cls + kChildren), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n)
      if (!std::strcmp(NameText(U32(f + 0x2C)), name)) {
        Field out{U32(f + kPropOffset), 0};
        if (!std::strcmp(ClassName(f), "BoolProperty")) {
          out.mask = U32(f + kBoolMask);
        }
        return out;
      }
  return {};
}

bool ClassChainHas(uint32_t cls, const char* name) {
  for (int depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    if (!std::strcmp(NameText(U32(cls + 0x2C)), name)) return true;
  return false;
}

enum class Kind : uint8_t { kNone, kStatic, kSkeletal, kBsp, kTerrain, kParticle, kDecal, kSpeedTree, kSprite, kOther };
const char* KindName(Kind k) {
  switch (k) {
    case Kind::kStatic: return "static";
    case Kind::kSkeletal: return "skeletal";
    case Kind::kBsp: return "bsp";
    case Kind::kTerrain: return "terrain";
    case Kind::kParticle: return "particle";
    case Kind::kDecal: return "decal";
    case Kind::kSpeedTree: return "speedtree";
    case Kind::kSprite: return "sprite";
    case Kind::kOther: return "other";
    default: return "none";
  }
}

// Non-primitive components the feed also reports: lights and fog.
enum class Extra : uint8_t { kNone, kLight, kFog };

struct ClassInfo {
  Kind kind = Kind::kNone;  // kNone: not a PrimitiveComponent
  Extra extra = Extra::kNone;
  const char* name = "";
  // Per-class property offsets the per-frame walk reads, looked up on first use (the same FindProp a Prop() call does).
  mutable Field attached, light_env;
  mutable bool attached_done = false, light_env_done = false;
};

// Offsets, resolved from the first instance of each class family seen.
struct Offsets {
  bool primitive = false, mesh = false, static_mesh = false, skeletal = false, particle = false, decal = false;
  Field scene_info, local_to_world, bounds, hidden_game, last_render_time, owner, cull_dist, cached_cull, materials,
      static_mesh_ref,
      forced_lod, skeletal_mesh_ref, space_bases, predicted_lod, template_ref, decal_material,
      special_highlight;  // B-277(e): SkeletalMeshComponent.bSpecialHighlight (the enemy outline)
  Field owner_no_see, only_owner_see;  // suspect detector: view-owner-dependent hide flags of a primitive
  bool actor = false;
  Field actor_hidden;
  bool skel_asset = false;
  Field ref_skeleton, skel_materials;
};
Offsets g_off;
// Suspect detector state (see n1's suspect.rs): off with BOTS_N1_SUSPECT=0.
const bool g_suspect_on = [] {
  const char* v = std::getenv("BOTS_N1_SUSPECT");
  return !(v && v[0] == '0');
}();
uint32_t g_suspect_lines = 0;
std::unordered_set<uint32_t> g_suspect_seen;

std::unordered_map<uint32_t, ClassInfo> g_classes;

const ClassInfo& ClassifySlow(uint32_t cls);
const ClassInfo kNotAClass{};  // what ClassifySlow returns for a pointer that is not a live class (never cached)
// The class map lookup sits on the per-frame walk (every tracked component, every frame, plus the safety-net slice):
// a small direct-mapped memo in front of it. Map entries never move or go away, so the pointers stay valid.
const ClassInfo& Classify(uint32_t cls) {
  struct Slot {
    uint32_t cls = 0;
    const ClassInfo* info = nullptr;
  };
  static Slot memo[4096];
  Slot& slot = memo[((cls >> 2) ^ (cls >> 14)) & 4095];
  if (slot.info && slot.cls == cls) return *slot.info;
  const ClassInfo& info = ClassifySlow(cls);
  if (&info != &kNotAClass) slot.cls = cls, slot.info = &info;
  return info;
}

bool NotAClass(uint32_t cls);

const ClassInfo& ClassifySlow(uint32_t cls) {
  auto it = g_classes.find(cls);
  if (it != g_classes.end()) return it->second;
  // The per-frame checks read +0x34 of components that may have been freed since the last frame, so <cls> can be
  // garbage. Only a live UClass is classified (and cached); anything else is "not a component" (T480 runs crashed in
  // NameText and ClassChainHas on such pointers after chapter loads, with tracking off and on).
  if (NotAClass(cls)) return kNotAClass;
  ClassInfo info;
  info.name = Readable(cls) ? NameText(U32(cls + 0x2C)) : "None";
  if (ClassChainHas(cls, "LightComponent")) info.extra = Extra::kLight;
  else if (ClassChainHas(cls, "HeightFogComponent") || ClassChainHas(cls, "FogVolumeDensityComponent"))
    info.extra = Extra::kFog;
  if (ClassChainHas(cls, "PrimitiveComponent")) {
    if (ClassChainHas(cls, "SkeletalMeshComponent")) info.kind = Kind::kSkeletal;
    else if (ClassChainHas(cls, "StaticMeshComponent")) info.kind = Kind::kStatic;
    else if (ClassChainHas(cls, "ModelComponent")) info.kind = Kind::kBsp;
    else if (ClassChainHas(cls, "TerrainComponent")) info.kind = Kind::kTerrain;
    else if (ClassChainHas(cls, "ParticleSystemComponent")) info.kind = Kind::kParticle;
    else if (ClassChainHas(cls, "DecalComponent")) info.kind = Kind::kDecal;
    else if (ClassChainHas(cls, "SpeedTreeComponent")) info.kind = Kind::kSpeedTree;
    else if (ClassChainHas(cls, "SpriteComponent")) info.kind = Kind::kSprite;
    else info.kind = Kind::kOther;
    Offsets& o = g_off;
    if (!o.primitive) {
      o.primitive = true;
      o.scene_info = FindProp(cls, "SceneInfo");
      o.local_to_world = FindProp(cls, "LocalToWorld");
      o.bounds = FindProp(cls, "Bounds");
      o.hidden_game = FindProp(cls, "HiddenGame");
      o.last_render_time = FindProp(cls, "LastRenderTime");
      o.owner = FindProp(cls, "Owner");
      o.cull_dist = FindProp(cls, "CullDistance");
      o.cached_cull = FindProp(cls, "CachedCullDistance");
      o.owner_no_see = FindProp(cls, "bOwnerNoSee");
      o.only_owner_see = FindProp(cls, "bOnlyOwnerSee");
      REXLOG_INFO("scenefeed: PrimitiveComponent CullDistance +{:X} CachedCullDistance +{:X}", o.cull_dist.offset,
                  o.cached_cull.offset);
      REXLOG_INFO("scenefeed: PrimitiveComponent SceneInfo +{:X} LocalToWorld +{:X} Bounds +{:X} HiddenGame +{:X}/{:08X} "
                  "LastRenderTime +{:X} Owner +{:X}",
                  o.scene_info.offset, o.local_to_world.offset, o.bounds.offset, o.hidden_game.offset,
                  o.hidden_game.mask, o.last_render_time.offset, o.owner.offset);
    }
    if (!o.mesh && ClassChainHas(cls, "MeshComponent")) {
      o.mesh = true;
      o.materials = FindProp(cls, "Materials");
      REXLOG_INFO("scenefeed: MeshComponent Materials +{:X}", o.materials.offset);
    }
    if (!o.static_mesh && info.kind == Kind::kStatic) {
      o.static_mesh = true;
      o.static_mesh_ref = FindProp(cls, "StaticMesh");
      o.forced_lod = FindProp(cls, "ForcedLodModel");
      REXLOG_INFO("scenefeed: StaticMeshComponent StaticMesh +{:X} ForcedLodModel +{:X}", o.static_mesh_ref.offset,
                  o.forced_lod.offset);
    }
    if (!o.skeletal && info.kind == Kind::kSkeletal) {
      o.skeletal = true;
      o.skeletal_mesh_ref = FindProp(cls, "SkeletalMesh");
      o.space_bases = FindProp(cls, "SpaceBases");
      o.predicted_lod = FindProp(cls, "PredictedLODLevel");
      o.special_highlight = FindProp(cls, "bSpecialHighlight");
      REXLOG_INFO("scenefeed: SkeletalMeshComponent SkeletalMesh +{:X} SpaceBases +{:X} PredictedLODLevel +{:X}",
                  o.skeletal_mesh_ref.offset, o.space_bases.offset, o.predicted_lod.offset);
    }
    if (!o.particle && info.kind == Kind::kParticle) {
      o.particle = true;
      o.template_ref = FindProp(cls, "Template");
    }
    if (!o.decal && info.kind == Kind::kDecal) {
      o.decal = true;
      o.decal_material = FindProp(cls, "DecalMaterial");
    }
  }
  return g_classes.emplace(cls, info).first->second;
}

// UObject::GObjObjects: {data, count, max} whose entries' Index (+0x04) equal their position.
uint32_t g_objects = 0;
bool Mapped(uint32_t address) { return address >= 0x40000000u && !(address & 3); }
bool FindObjectTable() {
  if (g_objects) return true;
  if (Readable(U32(kObjectTable)) && U32(kObjectTable + 4) > 1000) {
    g_objects = kObjectTable;
    REXLOG_INFO("scenefeed: object table at {:08X} ({} objects)", g_objects, U32(g_objects + 4));
  }
  return g_objects != 0;
}

struct Camera {
  float location[3] = {};
  int32_t rotation[3] = {};
  float fov = 0;
  float view[16] = {}, proj[16] = {};
  bool ok = false;
};

void Mul(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = s;
    }
}

// UE3's view and projection for a POV (FSceneView as ULocalPlayer::CalcSceneView builds it): ViewMatrix =
// Translation(-Location) * InverseRotation(Rotation) * axis swap (UE3 X forward, Y right, Z up -> view Z forward,
// X right, Y up); ProjectionMatrix = FPerspectiveMatrix(FOV/2, 1280, 720, near 10) with an infinite far plane.
void CameraMatrices(Camera& cam) {
  constexpr double k = 3.14159265358979323846 * 2 / 65536;
  const double p = cam.rotation[0] * k, y = cam.rotation[1] * k, r = cam.rotation[2] * k;
  const double sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
  // FRotationMatrix rows: X, Y, Z axes.
  const double m[3][3] = {{cp * cy, cp * sy, sp},
                          {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
                          {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
  float t[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -cam.location[0], -cam.location[1], -cam.location[2], 1};
  float inv[16] = {};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) inv[i * 4 + j] = float(m[j][i]);
  inv[15] = 1;
  const float swap[16] = {0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
  float tmp[16];
  Mul(t, inv, tmp);
  Mul(tmp, swap, cam.view);
  const float half = float(cam.fov * 3.14159265358979323846 / 360), near_z = 10;
  std::memset(cam.proj, 0, sizeof(cam.proj));
  cam.proj[0] = 1 / std::tan(half);
  cam.proj[5] = 1280.0f / 720.0f / std::tan(half);
  cam.proj[10] = 1;  // infinite far plane (UE3 uses 1 - Z_PRECISION)
  cam.proj[11] = 1;
  cam.proj[14] = -near_z;
}

// GEngine: the live GameEngine object (not its class default object), found once in the object table.
uint32_t Engine() {
  static uint32_t engine = 0;
  if (engine && Readable(engine) && ClassChainHas(U32(engine + kClass), "GameEngine")) return engine;
  engine = 0;
  if (!FindObjectTable()) return 0;
  const uint32_t data = U32(g_objects), count = U32(g_objects + 4);
  for (uint32_t i = 0; i < count && !engine; ++i) {
    const uint32_t o = U32(data + 4 * i);
    if (!Readable(o) || !Readable(U32(o + kClass))) continue;
    if (!std::strcmp(ClassName(o), "Class")) continue;
    if (ClassChainHas(U32(o + kClass), "GameEngine") && ObjName(o).rfind("Default__", 0) != 0) engine = o;
  }
  if (engine) REXLOG_INFO("scenefeed: GEngine {:08X} ({})", engine, ClassName(engine));
  return engine;
}

bool ReadCamera(Camera& cam, uint32_t& world_info_out) {
  static Field players, actor, player_camera, camera_cache;
  const uint32_t engine = Engine();
  if (!Readable(engine)) return false;
  if (!players.ok()) players = FindProp(U32(engine + kClass), "GamePlayers");
  if (!players.ok() || !U32(engine + players.offset + 4)) return false;
  const uint32_t lp = U32(U32(engine + players.offset));
  if (!Readable(lp)) return false;
  if (!actor.ok()) actor = FindProp(U32(lp + kClass), "Actor");
  const uint32_t pc = actor.ok() ? U32(lp + actor.offset) : 0;
  if (!Readable(pc)) return false;
  if (!player_camera.ok()) player_camera = FindProp(U32(pc + kClass), "PlayerCamera");
  const uint32_t camera = player_camera.ok() ? U32(pc + player_camera.offset) : 0;
  static Field world_info;
  if (!world_info.ok()) world_info = FindProp(U32(pc + kClass), "WorldInfo");
  world_info_out = world_info.ok() ? U32(pc + world_info.offset) : 0;
  if (!Readable(camera)) return false;
  if (!camera_cache.ok()) camera_cache = FindProp(U32(camera + kClass), "CameraCache");
  if (!camera_cache.ok()) return false;
  const uint32_t pov = camera + camera_cache.offset + 4;  // TCameraCache {float TimeStamp; TPOV POV}
  for (int i = 0; i < 3; ++i) cam.location[i] = F32(pov + 4 * i);
  for (int i = 0; i < 3; ++i) cam.rotation[i] = int32_t(U32(pov + 12 + 4 * i));
  cam.fov = F32(pov + 24);
  CameraMatrices(cam);
  cam.ok = true;
  return true;
}

struct LiveComponent;
struct Primitive {
  uint32_t component;
  Kind kind;
  uint32_t index = 0;  // the component's UObject Index when found (AliveAt)
  // Caches for the per-frame walk: the class last seen on the component and its ClassInfo (Classify), and the
  // component's LiveState::components entry (that map is never erased from, so the pointer stays valid).
  uint32_t cls = 0;
  const ClassInfo* info = nullptr;
  LiveComponent* live = nullptr;
};

// The component's class, refreshed when the class pointer changed (a freed component's memory reused).
const ClassInfo& PrimClass(Primitive& p) {
  const uint32_t cls = U32(p.component + kClass);
  if (cls != p.cls || !p.info) p.cls = cls, p.info = &Classify(cls);
  return *p.info;
}

// B-234: the per-frame walks touch about a thousand components, each a handful of cache lines scattered through guest
// memory, and the loop bodies are long enough that the CPU cannot overlap one component's misses with the next's.
// Prefetching a few components ahead hides most of that. BOTS_FEED_PREFETCH=0 (or BOTS_FEED_LEGACY=1) turns it off.
const bool g_prefetch = [] {
  for (const char* name : {"BOTS_FEED_PREFETCH", "BOTS_FEED_LEGACY"}) {
    const char* v = std::getenv(name);
    if (v && *v && (name[10] == 'P' ? *v == '0' : *v != '0')) return false;
  }
  return true;
}();
inline void Prefetch(uint32_t address) { __builtin_prefetch(Raw(address)); }
// The lines the walks read from a component: header (class, name, outer, index), and the primitive fields.
void PrefetchComponent(uint32_t c) {
  Prefetch(c);
  Prefetch(c + kClass);
  for (const Field* f : {&g_off.scene_info, &g_off.hidden_game, &g_off.bounds, &g_off.local_to_world, &g_off.cull_dist,
                         &g_off.cached_cull, &g_off.materials, &g_off.owner, &g_off.static_mesh_ref,
                         &g_off.skeletal_mesh_ref})
    if (f->ok()) Prefetch(c + f->offset);
  if (g_off.local_to_world.ok()) Prefetch(c + g_off.local_to_world.offset + 60);
  if (g_off.bounds.ok()) Prefetch(c + g_off.bounds.offset + 27);
}
// The object table's entry for the component (AliveAt reads it).
void PrefetchEntry(uint32_t index) {
  if (g_objects) Prefetch(U32(g_objects) + 4 * index);
}
// The owning actor's bHidden word, once the component's own lines have arrived.
void PrefetchOwner(uint32_t c) {
  if (!g_off.owner.ok() || !g_off.actor_hidden.ok()) return;
  const uint32_t owner = U32(c + g_off.owner.offset);
  if (Readable(owner)) Prefetch(owner + g_off.actor_hidden.offset);
}

// Remove the entries <gone> says to, keeping order (std::erase_if, but the predicate may update the entry's caches).
// <ahead>(entry) runs on the entry kAhead places later before each test, to prefetch.
template <typename T, typename F, typename A>
void DropIf(std::vector<T>& v, F gone, A ahead) {
  constexpr size_t kAhead = 8;
  size_t n = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    if (g_prefetch && i + kAhead < v.size()) ahead(v[i + kAhead]);
    if (!gone(v[i])) {
      if (n != i) v[n] = v[i];
      ++n;
    }
  }
  v.resize(n);
}
template <typename T, typename F>
void DropIf(std::vector<T>& v, F gone) {
  size_t n = 0;
  for (size_t i = 0; i < v.size(); ++i)
    if (!gone(v[i])) {
      if (n != i) v[n] = v[i];
      ++n;
    }
  v.resize(n);
}

struct Times {
  double enumerate = 0, extract = 0, json = 0, write = 0;
  uint32_t primitives = 0, objects = 0;
};

// Lights and fog components attached to the world (ActorComponent.bAttached), found by the same walk.
struct Extras {
  std::vector<uint32_t> lights, fog;
  std::vector<uint32_t> decals;  // B-228: every DecalComponent, attached or not (they never get a SceneInfo)
};
Extras g_extras;

bool Attached(uint32_t component, const ClassInfo& info, uint32_t cls) {
  if (!info.attached_done) info.attached_done = true, info.attached = FindProp(cls, "bAttached");
  return info.attached.ok() && (U32(component + info.attached.offset) & info.attached.mask);
}
bool Attached(uint32_t component) {
  const uint32_t cls = U32(component + kClass);
  return Attached(component, Classify(cls), cls);
}

// Every attached primitive component in the object table.
void Enumerate(std::vector<Primitive>& out, Times& t) {
  out.clear();
  g_extras.lights.clear();
  g_extras.fog.clear();
  g_extras.decals.clear();
  if (!FindObjectTable()) return;
  const uint32_t data = U32(g_objects), count = U32(g_objects + 4);
  t.objects = count;
  uint32_t last_cls = 0;
  const ClassInfo* last = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = U32(data + 4 * i);
    if (!o) continue;
    const uint32_t cls = U32(o + kClass);
    if (cls != last_cls) last_cls = cls, last = &Classify(cls);
    if (last->extra != Extra::kNone) {
      if (Attached(o)) (last->extra == Extra::kLight ? g_extras.lights : g_extras.fog).push_back(o);
      continue;
    }
    if (last->kind == Kind::kNone) continue;
    if (last->kind == Kind::kDecal) {
      g_extras.decals.push_back(o);
      continue;
    }
    if (!g_off.scene_info.ok() || !U32(o + g_off.scene_info.offset)) continue;  // not attached to a scene
    out.push_back({o, last->kind, i});
  }
}

// JSON has no NaN or infinity (skeletal SpaceBases hold NaN for bones a LOD doesn't update): write null.
void Float(std::string& s, bool comma, float v) {
  if (comma) s += ',';
  if (std::isfinite(v))
    fmt::format_to(std::back_inserter(s), "{:.7g}", v);
  else
    s += "null";
}
void Floats(std::string& s, uint32_t address, int n) {
  s += '[';
  for (int i = 0; i < n; ++i) Float(s, i, F32(address + 4 * i));
  s += ']';
}
void FloatsHost(std::string& s, const float* v, int n) {
  s += '[';
  for (int i = 0; i < n; ++i) Float(s, i, v[i]);
  s += ']';
}
void Str(std::string& s, std::string_view v) {
  s += '"';
  for (char c : v) {
    if (c == '"' || c == '\\') s += '\\';
    if (uint8_t(c) >= 0x20) s += c;
  }
  s += '"';
}

// FMeshBone stride in SkeletalMesh.RefSkeleton (declared array<int> in script; the native element is larger).
// Found once: the stride at which every entry's name is a valid FName index and its parent index is in range.
uint32_t g_bone_stride = 0, g_bone_parent = 0;

void Skeleton(std::string& s, uint32_t mesh) {
  if (!g_off.ref_skeleton.ok()) return;
  const uint32_t data = U32(mesh + g_off.ref_skeleton.offset), count = U32(mesh + g_off.ref_skeleton.offset + 4);
  if (!Readable(data) || !count || count > 1024) return;
  const uint32_t name_count = U32(kNames + 4);
  static bool searched = false;
  if (!g_bone_stride && (searched || count < 16)) return;  // small rigs (props) can have every bone on the root
  if (!g_bone_stride) {
    searched = true;
    // Raw dump of the first bones, so the layout can be read off the log if the search fails.
    for (uint32_t row = 0; row < 12 && row * 16 < count * 128; ++row) {
      const uint32_t a = data + row * 16;
      REXLOG_INFO("scenefeed: bones +{:03X}: {:08X} {:08X} {:08X} {:08X}", row * 16, U32(a), U32(a + 4), U32(a + 8), U32(a + 12));
    }
    REXLOG_INFO("scenefeed: RefSkeleton TArray {:08X} {:08X} {:08X} names {}", data, count, U32(mesh + g_off.ref_skeleton.offset + 8), name_count);
    for (uint32_t stride = 16; stride <= 128 && !g_bone_stride; stride += 4)
      for (uint32_t parent = 8; parent + 4 <= stride && !g_bone_stride; parent += 4) {  // +4 is Flags (all 0)
        bool ok = U32(data + parent) == 0;  // the root is its own parent (0)
        uint32_t nonzero = 0;
        for (uint32_t b = 1; b < count && ok; ++b) {
          const uint32_t p = U32(data + b * stride + parent);
          ok = (U32(data + b * stride) >> 14) < name_count && p < b;  // FName: index << 14
          nonzero += p != 0;
        }
        if (ok && count > 4 && nonzero) g_bone_stride = stride, g_bone_parent = parent;  // a real tree has non-root parents
      }
    REXLOG_INFO("scenefeed: FMeshBone stride {} parent +{} (from {} bones)", g_bone_stride, g_bone_parent, count);
    if (!g_bone_stride) return;
  }
  s += "{\"names\":[";
  for (uint32_t b = 0; b < count; ++b) {
    if (b) s += ',';
    Str(s, NameText(U32(data + b * g_bone_stride)));
  }
  s += "],\"parents\":[";
  for (uint32_t b = 0; b < count; ++b)
    fmt::format_to(std::back_inserter(s), "{}{}", b ? "," : "", int32_t(U32(data + b * g_bone_stride + g_bone_parent)));
  s += "]}";
}

bool IsObject(uint32_t p);
bool AliveAt(uint32_t o, uint32_t index);

// A property of <object>'s class by name, cached per (class, name).
Field Prop(uint32_t object, const char* name) {
  static std::unordered_map<uint64_t, Field> cache;
  const uint32_t cls = U32(object + kClass);
  const uint64_t key = (uint64_t(cls) << 32) ^ std::hash<std::string_view>{}(name);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  return cache.emplace(key, FindProp(cls, name)).first->second;
}
uint32_t PropU32(uint32_t object, const char* name, uint32_t fallback = 0) {
  const Field f = Prop(object, name);
  return f.ok() ? U32(object + f.offset) : fallback;
}
// Prop() for a per-frame call site that nearly always sees one class: a one-entry cache in front of the map (which
// hashes the name string on every call). Declare the site static next to the call.
struct PropSite {
  uint32_t cls = 0;
  bool set = false;
  Field field;
};
Field Prop(PropSite& site, uint32_t object, const char* name) {
  const uint32_t cls = U32(object + kClass);
  if (!site.set || site.cls != cls) site.field = Prop(object, name), site.cls = cls, site.set = true;
  return site.field;
}
uint32_t PropU32(PropSite& site, uint32_t object, const char* name) {
  const Field f = Prop(site, object, name);
  return f.ok() ? U32(object + f.offset) : 0;
}
bool PropBool(uint32_t object, const char* name) {
  const Field f = Prop(object, name);
  return f.ok() && (U32(object + f.offset) & f.mask);
}
uint8_t PropByte(uint32_t object, const char* name) {
  const Field f = Prop(object, name);
  return f.ok() ? *Raw(object + f.offset) : 0;
}

// Every script property of an object (or struct) as JSON, from the reflection records alone. UProperty: +0x44
// ArrayDim, +0x48 ElementSize, +0x60 Offset; the subclass's first field (+0x80) is UBoolProperty BitMask,
// UStructProperty Struct, UObjectProperty PropertyClass, UArrayProperty Inner or UByteProperty Enum (UEnum: +0x44
// Names, a TArray<FName>). Vector, Vector2D, Rotator, Color, LinearColor and Matrix are written as arrays (Color as
// [R, G, B, A] bytes); other structs as objects; dynamic arrays up to 64 elements. Properties of <stop> and its
// superclasses are left out.
void Value(std::string& s, uint32_t prop, uint32_t address, int depth);

void Members(std::string& s, uint32_t structure, uint32_t address, int depth, const char* stop = "Object") {
  s += '{';
  bool first = true;
  for (int level = 0; Readable(structure) && level < 16; structure = U32(structure + 0x3C), ++level) {
    if (!std::strcmp(NameText(U32(structure + 0x2C)), stop)) break;
    for (uint32_t f = U32(structure + kChildren), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n) {
      if (!std::strstr(ClassName(f), "Property")) continue;
      if (!first) s += ',';
      first = false;
      Str(s, NameText(U32(f + 0x2C)));
      s += ':';
      const uint32_t dim = U32(f + 0x44), size = U32(f + 0x48), at = address + U32(f + kPropOffset);
      if (dim > 1 && dim <= 64) {
        s += '[';
        for (uint32_t k = 0; k < dim; ++k) {
          if (k) s += ',';
          Value(s, f, at + k * size, depth);
        }
        s += ']';
      } else {
        Value(s, f, at, depth);
      }
    }
  }
  s += '}';
}

void Value(std::string& s, uint32_t prop, uint32_t address, int depth) {
  const std::string_view type = ClassName(prop);
  const uint32_t sub = U32(prop + kSubFirst);
  if (type == "FloatProperty") {
    Float(s, false, F32(address));
  } else if (type == "IntProperty") {
    fmt::format_to(std::back_inserter(s), "{}", int32_t(U32(address)));
  } else if (type == "BoolProperty") {
    s += (U32(address) & sub) ? "true" : "false";
  } else if (type == "ByteProperty") {
    const uint8_t v = *Raw(address);
    const uint32_t names = IsObject(sub) ? U32(sub + 0x44) : 0;
    if (Readable(names) && v < U32(sub + 0x48)) Str(s, NameText(U32(names + 4 * v)));  // enum value name (Bourne's FName: one word)
    else fmt::format_to(std::back_inserter(s), "{}", v);
  } else if (type == "NameProperty") {
    Str(s, NameText(U32(address)));
  } else if (type == "ObjectProperty" || type == "ComponentProperty" || type == "ClassProperty") {
    const uint32_t o = U32(address);
    if (IsObject(o)) Str(s, Path(o));
    else s += "null";
  } else if (type == "StrProperty") {
    const uint32_t data = U32(address), n = U32(address + 4);
    std::string text;
    for (uint32_t k = 0; Readable(data & ~3u) && k + 1 < n && k < 256; ++k) {
      const uint16_t c = uint16_t(__builtin_bswap16(*reinterpret_cast<const uint16_t*>(Raw(data + 2 * k))));
      text += c < 0x80 ? char(c) : '?';
    }
    Str(s, text);
  } else if (type == "StructProperty" && Readable(sub) && depth < 6) {
    const std::string_view name = NameText(U32(sub + 0x2C));
    auto members = [&](std::initializer_list<const char*> names) {
      s += '[';
      bool first = true;
      for (const char* m : names) {
        const Field f = FindProp(sub, m);
        if (!first) s += ',';
        first = false;
        if (!f.ok()) s += "null";
        else if (name == "Color") fmt::format_to(std::back_inserter(s), "{}", *Raw(address + f.offset));
        else if (name == "Rotator") fmt::format_to(std::back_inserter(s), "{}", int32_t(U32(address + f.offset)));
        else Float(s, false, F32(address + f.offset));
      }
      s += ']';
    };
    if (name == "Vector") members({"X", "Y", "Z"});
    else if (name == "Vector2D") members({"X", "Y"});
    else if (name == "Rotator") members({"Pitch", "Yaw", "Roll"});
    else if (name == "Color" || name == "LinearColor") members({"R", "G", "B", "A"});
    else if (name == "Matrix") Floats(s, address, 16);
    else Members(s, sub, address, depth + 1);
  } else if (type == "ArrayProperty" && Readable(sub) && depth < 6) {
    const uint32_t data = U32(address), n = U32(address + 4), size = U32(sub + 0x48);
    s += '[';
    for (uint32_t k = 0; Readable(data) && k < n && k < 64 && size && size <= 0x1000; ++k) {
      if (k) s += ',';
      Value(s, sub, data + k * size, depth + 1);
    }
    s += ']';
  } else {
    s += "null";  // delegates, interfaces, native pointers, maps
  }
}

// One object: {class, name (path), props {...}}.
void ObjectJson(std::string& s, uint32_t object, const char* stop) {
  fmt::format_to(std::back_inserter(s), "{{\"class\":\"{}\",\"name\":", ClassName(object));
  Str(s, Path(object));
  s += ",\"props\":";
  Members(s, U32(object + kClass), object, 0, stop);
  s += '}';
}

// The UProperty record of <object>'s property <name> (0 if none).
uint32_t PropRecord(uint32_t object, const char* name) {
  for (uint32_t cls = U32(object + kClass), depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    for (uint32_t f = U32(cls + kChildren), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n)
      if (!std::strcmp(NameText(U32(f + 0x2C)), name)) return f;
  return 0;
}

// ,"<key>":<object.name> when the object has that property.
void NamedValue(std::string& s, const char* key, uint32_t object, const char* name) {
  if (!IsObject(object)) return;
  const uint32_t f = PropRecord(object, name);
  if (!f) return;
  fmt::format_to(std::back_inserter(s), ",\"{}\":", key);
  Value(s, f, object + U32(f + kPropOffset), 0);
}

// The light properties the live renderer reads; dumping every property of every light costs milliseconds a frame.
constexpr const char* kLiveLightProps[] = {
    "LightToWorld", "Brightness", "LightColor", "LightAffectsClassification", "LightingChannels",
    "UseDirectLightMap", "LowerBrightness", "LowerColor", "WorldSpaceIncidentLighting", "LightEnvironment",
    "bEnabled", "Radius", "FalloffExponent", "InnerConeAngle", "OuterConeAngle", "CastShadows",
    "CastDynamicShadows", "LightShadowMode", "bUseVolumes",
    // B-205: the rest of the shadow light (B-189 modulated shadows).
    "ModShadowColor", "ModShadowFadeoutExponent", "ShadowFalloffExponent", "MinShadowFalloffRadius"};

// One light as ObjectJson writes it, with only kLiveLightProps (property records cached per class).
void CompactLightJson(std::string& s, uint32_t light) {
  static std::unordered_map<uint32_t, std::vector<uint32_t>> records;
  auto [it, fresh] = records.try_emplace(U32(light + kClass));
  if (fresh)
    for (const char* name : kLiveLightProps)
      if (const uint32_t f = PropRecord(light, name)) it->second.push_back(f);
  // No "name": the renderer keys lights by their LightEnvironment, and Path() is the costly part.
  fmt::format_to(std::back_inserter(s), "{{\"class\":\"{}\",\"props\":{{", ClassName(light));
  bool first = true;
  for (const uint32_t f : it->second) {
    if (!first) s += ',';
    first = false;
    const char* name = NameText(U32(f + 0x2C));
    Str(s, name);
    s += ':';
    if (!std::strcmp(name, "LightingChannels")) {  // a struct of ~30 bools; the renderer reads two
      static const Field st = FindProp(U32(f + kSubFirst), "Static"), dy = FindProp(U32(f + kSubFirst), "Dynamic");
      const uint32_t at = light + U32(f + kPropOffset);
      fmt::format_to(std::back_inserter(s), "{{\"Static\":{},\"Dynamic\":{}}}",
                     st.ok() && (U32(at + st.offset) & st.mask), dy.ok() && (U32(at + dy.offset) & dy.mask));
    } else {
      Value(s, f, light + U32(f + kPropOffset), 0);
    }
  }
  s += "}}";
}

// Lights, fog and post-processing for the frame (see the format notes at the top). `live`: the renderer's cut: only
// the light properties it reads, and fog and post only when `fog_post` (they rarely change).
void LightsPostJson(std::string& s, uint32_t world_info, bool live = false, bool fog_post = true) {
  s += ",\"lights\":[";
  bool first = true;
  for (uint32_t l : g_extras.lights) {
    if (!IsObject(l)) continue;
    // The renderer skips baked or disabled level point and spot lights (the lightmaps have them): leave them out.
    if (live && !IsObject(PropU32(l, "LightEnvironment"))) {
      const std::string_view cls = ClassName(l);
      if ((cls == "PointLightComponent" || cls == "SpotLightComponent") &&
          (!PropBool(l, "bEnabled") || PropBool(l, "UseDirectLightMap") ||
           PropByte(l, "LightAffectsClassification") == 2 /* LAC_STATIC_AFFECTING */))
        continue;
    }
    if (!first) s += ",\n";
    first = false;
    if (live) CompactLightJson(s, l);
    else ObjectJson(s, l, "ActorComponent");
  }
  s += ']';
  if (!fog_post) return;
  s += ",\"fog\":[";
  first = true;
  for (uint32_t f : g_extras.fog) {
    if (!IsObject(f)) continue;
    if (!first) s += ",\n";
    first = false;
    ObjectJson(s, f, "ActorComponent");
  }
  s += "],\"post\":{\"chain\":[";
  const uint32_t engine = Engine();
  const uint32_t players = IsObject(engine) ? PropRecord(engine, "GamePlayers") : 0;
  const uint32_t lp = players && U32(engine + U32(players + kPropOffset) + 4) ? U32(U32(engine + U32(players + kPropOffset))) : 0;
  const uint32_t chain = IsObject(lp) ? PropU32(lp, "PlayerPostProcess") : 0;
  if (IsObject(chain)) {
    const Field effects = Prop(chain, "Effects");
    const uint32_t data = effects.ok() ? U32(chain + effects.offset) : 0, n = effects.ok() ? U32(chain + effects.offset + 4) : 0;
    first = true;
    for (uint32_t i = 0; Readable(data) && i < n && i < 32; ++i) {
      const uint32_t e = U32(data + 4 * i);
      if (!IsObject(e)) continue;
      if (!first) s += ",\n";
      first = false;
      ObjectJson(s, e, "Object");
    }
  }
  s += ']';
  if (IsObject(chain)) {
    s += ",\"chain_name\":";
    Str(s, Path(chain));
  }
  NamedValue(s, "current", lp, "CurrentPPInfo");
  const uint32_t pc = IsObject(lp) ? PropU32(lp, "Actor") : 0;
  const uint32_t camera = IsObject(pc) ? PropU32(pc, "PlayerCamera") : 0;
  NamedValue(s, "camera_override_alpha", camera, "CamOverridePostProcessAlpha");
  NamedValue(s, "camera_settings", camera, "CamPostProcessSettings");
  NamedValue(s, "world_default", world_info, "DefaultPostProcessSettings");
  s += '}';
}

// FParticleEmitterInstance (native, this build; found by dumping instances and matching the sprite vertex data):
//   +0x00 vtable, +0x10 CurrentLODLevel (UParticleLODLevel), +0x20 Location (FVector), +0x30 ParticleData,
//   +0x34 ParticleIndices (WORD[]), +0xC0 ParticleStride, +0xC4 ActiveParticles, +0xC8 MaxActiveParticles,
//   +0x130 the material the emitter draws with. Particles: ParticleData + ParticleIndices[i] * ParticleStride for
//   i < ActiveParticles. FBaseParticle (0x80 bytes): +0x00 OldLocation, +0x0C RelativeTime, +0x10 Location,
//   +0x1C OneOverMaxLifetime, +0x20 BaseVelocity, +0x2C Rotation, +0x30 Velocity, +0x3C BaseRotationRate,
//   +0x40 BaseSize, +0x4C RotationRate, +0x50 Size, +0x5C Flags, +0x60 Color (FLinearColor), +0x70 BaseColor;
//   module payloads follow from +0x80.
//   +0xB8 SubUVDataOffset: byte offset in a particle of the sub-UV payload [RandomImageTime, ImageH, ImageV, Interp,
//   Image2H, Image2V] (found by matching each sub-UV emitter's payload words; it sits after the module payloads, so
//   its place differs per emitter).
constexpr uint32_t kEmitLod = 0x10, kEmitLocation = 0x20, kEmitData = 0x30, kEmitIndices = 0x34, kEmitSubUV = 0xB8,
                   kEmitStride = 0xC0, kEmitActive = 0xC4, kEmitMax = 0xC8, kEmitMaterial = 0x130;

// Byte offset in a particle of its sub-UV frame data [ImageH, ImageV, Interp, Image2H, Image2V], or 0 when the
// instance's SubUVDataOffset is out of range.
uint32_t SubUVFrameOffset(uint32_t inst, uint32_t stride) {
  const uint32_t o = U32(inst + kEmitSubUV);
  return o >= 0x80 && !(o & 3) && o + 4 + 5 * 4 <= stride ? o + 4 : 0;
}

// EParticleAxisLock of an LOD level's OrientationAxisLock module (the engine keeps the last enabled one it finds in
// Modules), 0 (EPAL_NONE) without one. Locked sprites lie in a fixed plane (EPAL_X..EPAL_NEGATIVE_Z: flat glows and
// scorch cards) or turn only about an axis (EPAL_ROTATE_*), instead of facing the camera.
uint32_t LockAxis(uint32_t lod) {
  const Field modules = Prop(lod, "Modules");
  if (!modules.ok()) return 0;
  const uint32_t data = U32(lod + modules.offset), n = U32(lod + modules.offset + 4);
  uint32_t lock = 0;
  for (uint32_t i = 0; Readable(data) && i < n && i < 64; ++i) {
    const uint32_t m = U32(data + 4 * i);
    if (!IsObject(m) || std::strcmp(ClassName(m), "ParticleModuleOrientationAxisLock")) continue;
    const Field enabled = Prop(m, "bEnabled");
    if (enabled.ok() && !(U32(m + enabled.offset) & enabled.mask)) continue;
    lock = PropByte(m, "LockAxisFlags");
  }
  return lock;
}

void Emitters(std::string& s, uint32_t component) {
  static Field instances;
  if (!instances.ok()) instances = FindProp(U32(component + kClass), "EmitterInstances");
  if (!instances.ok()) return;
  const uint32_t data = U32(component + instances.offset), n = U32(component + instances.offset + 4);
  s += ",\"emitters\":[";
  bool first = true;
  for (uint32_t i = 0; Readable(data) && i < n && i < 64; ++i) {
    const uint32_t inst = U32(data + 4 * i);
    if (!Readable(inst)) continue;
    const uint32_t lod = U32(inst + kEmitLod);
    if (!IsObject(lod)) continue;
    const uint32_t required = PropU32(lod, "RequiredModule"), type_data = PropU32(lod, "TypeDataModule");
    const char* type = "sprite";
    std::string type_class = IsObject(type_data) ? ClassName(type_data) : "";
    if (type_class.find("Mesh") != std::string::npos) type = "mesh";
    else if (type_class.find("Beam") != std::string::npos) type = "beam";
    else if (type_class.find("Trail") != std::string::npos) type = "trail";
    else if (!type_class.empty()) type = "other";
    const uint32_t active = U32(inst + kEmitActive), stride = U32(inst + kEmitStride);
    if (!first) s += ',';
    first = false;
    fmt::format_to(std::back_inserter(s), "{{\"index\":{},\"type\":\"{}\",\"type_class\":\"{}\",\"vtable\":{},"
                   "\"emitter\":", i, type, type_class, U32(inst));
    Str(s, Path(U32(lod + 0x28)));  // the LOD level's outer: the ParticleEmitter
    const uint32_t material = U32(inst + kEmitMaterial);
    s += ",\"material\":";
    Str(s, IsObject(material) ? Path(material) : (IsObject(required) ? Path(PropU32(required, "Material")) : ""));
    if (IsObject(required))
      fmt::format_to(std::back_inserter(s),
                     ",\"screen_alignment\":{},\"local_space\":{},\"sub_images\":[{},{}],\"sub_uv_method\":{}",
                     PropByte(required, "ScreenAlignment"), PropBool(required, "bUseLocalSpace"),
                     int32_t(PropU32(required, "SubImages_Horizontal", 1)),
                     int32_t(PropU32(required, "SubImages_Vertical", 1)), PropByte(required, "InterpolationMethod"));
    if (const uint32_t lock = LockAxis(lod)) fmt::format_to(std::back_inserter(s), ",\"lock_axis\":{}", lock);
    if (IsObject(required) && PropBool(required, "bUseMaxDrawCount"))
      fmt::format_to(std::back_inserter(s), ",\"max_draw_count\":{}", int32_t(PropU32(required, "MaxDrawCount")));
    if (IsObject(type_data) && std::string_view(type) == "mesh") {
      s += ",\"mesh\":";
      Str(s, Path(PropU32(type_data, "Mesh")));
      fmt::format_to(std::back_inserter(s), ",\"mesh_alignment\":{},\"override_material\":{}",
                     PropByte(type_data, "MeshAlignment"), PropBool(type_data, "bOverrideMaterial"));
    }
    s += ",\"location\":";
    Floats(s, inst + kEmitLocation, 3);
    if (const uint32_t sub = stride >= 0x80 && stride <= 0x1000 ? SubUVFrameOffset(inst, stride) : 0)
      fmt::format_to(std::back_inserter(s), ",\"sub_uv_offset\":{}", sub);
    fmt::format_to(std::back_inserter(s), ",\"active\":{},\"max\":{},\"stride\":{},\"particles\":[", active,
                   U32(inst + kEmitMax), stride);
    const uint32_t pdata = U32(inst + kEmitData), pindex = U32(inst + kEmitIndices);
    if (Readable(pdata) && Readable(pindex & ~3u) && stride >= 0x80 && stride <= 0x1000 && active <= 4096) {
      for (uint32_t k = 0; k < active; ++k) {
        const uint16_t slot = uint16_t(__builtin_bswap16(*reinterpret_cast<const uint16_t*>(Raw(pindex + 2 * k))));
        const uint32_t p = pdata + slot * stride;
        if (k) s += ',';
        // [location 3, old_location 3, size 3, rotation, color 4, relative_time, payload...]
        s += '[';
        for (uint32_t off : {0x10u, 0x14u, 0x18u, 0x00u, 0x04u, 0x08u, 0x50u, 0x54u, 0x58u, 0x2Cu, 0x60u, 0x64u, 0x68u,
                             0x6Cu, 0x0Cu})
          Float(s, off != 0x10u, F32(p + off));
        for (uint32_t off = 0x80; off < stride; off += 4) Float(s, true, F32(p + off));
        s += ']';
      }
    }
    s += "]}";
  }
  s += "]";
}

// A material as n1 names it: the first cooked material up its Parent chain, plus the run-time instances' parameters.
std::string MaterialPath(uint32_t m, bool params = false, bool* runtime = nullptr);

std::string FrameJson(uint32_t frame, const Camera& cam, uint32_t world_info, const std::vector<Primitive>& prims,
                      int64_t host_us) {
  static Field time_seconds, real_time;
  if (Readable(world_info) && !time_seconds.ok()) {
    time_seconds = FindProp(U32(world_info + kClass), "TimeSeconds");
    real_time = FindProp(U32(world_info + kClass), "RealTimeSeconds");
  }
  const float now = time_seconds.ok() && Readable(world_info) ? F32(world_info + time_seconds.offset) : 0;
  std::string s;
  s.reserve(1 << 20);
  fmt::format_to(std::back_inserter(s), "{{\"frame\":{},\"time\":{:.4f},\"real_time\":{:.4f},\"host_us\":{},", frame,
                 now, real_time.ok() && Readable(world_info) ? F32(world_info + real_time.offset) : 0.f, host_us);
  s += "\"camera\":{";
  if (cam.ok) {
    s += "\"location\":";
    FloatsHost(s, cam.location, 3);
    fmt::format_to(std::back_inserter(s), ",\"rotation\":[{},{},{}],\"fov\":{:.5g},\"view\":", cam.rotation[0],
                   cam.rotation[1], cam.rotation[2], cam.fov);
    FloatsHost(s, cam.view, 16);
    s += ",\"proj\":";
    FloatsHost(s, cam.proj, 16);
  }
  s += "},\"primitives\":[\n";
  std::unordered_map<uint32_t, bool> skeletons;
  bool first = true;
  for (const Primitive& p : prims) {
    const uint32_t c = p.component;
    if (!first) s += ",\n";
    first = false;
    fmt::format_to(std::back_inserter(s), "{{\"kind\":\"{}\",\"class\":\"{}\",\"name\":", KindName(p.kind),
                   ClassName(c));
    Str(s, Path(c));
    const uint32_t owner = g_off.owner.ok() ? U32(c + g_off.owner.offset) : 0;
    s += ",\"owner\":";
    Str(s, Readable(owner) ? ObjName(owner) : "");
    s += ",\"owner_class\":";
    Str(s, Readable(owner) ? ClassName(owner) : "");
    uint32_t mesh = 0;
    if (p.kind == Kind::kStatic && g_off.static_mesh_ref.ok()) mesh = U32(c + g_off.static_mesh_ref.offset);
    if (p.kind == Kind::kSkeletal && g_off.skeletal_mesh_ref.ok()) mesh = U32(c + g_off.skeletal_mesh_ref.offset);
    if (p.kind == Kind::kParticle && g_off.template_ref.ok()) mesh = U32(c + g_off.template_ref.offset);
    s += ",\"mesh\":";
    Str(s, Path(mesh));
    s += ",\"transform\":";
    Floats(s, c + g_off.local_to_world.offset, 16);
    s += ",\"materials\":[";
    if (g_off.materials.ok() && (p.kind == Kind::kStatic || p.kind == Kind::kSkeletal)) {
      const uint32_t data = U32(c + g_off.materials.offset), n = U32(c + g_off.materials.offset + 4);
      for (uint32_t i = 0; Readable(data) && i < n && i < 64; ++i) {
        if (i) s += ',';
        Str(s, MaterialPath(U32(data + 4 * i), true));  // as the live feed passes it
      }
    } else if (p.kind == Kind::kDecal && g_off.decal_material.ok()) {
      Str(s, Path(U32(c + g_off.decal_material.offset)));
    }
    s += "]";
    if (g_off.bounds.ok()) {  // FBoxSphereBounds {Origin, BoxExtent, SphereRadius}
      const uint32_t b = c + g_off.bounds.offset;
      s += ",\"bounds\":{\"origin\":";
      Floats(s, b, 3);
      s += ",\"extent\":";
      Floats(s, b + 12, 3);
      fmt::format_to(std::back_inserter(s), ",\"radius\":{:.6g}}}", F32(b + 24));
    }
    bool hidden = g_off.hidden_game.ok() && (U32(c + g_off.hidden_game.offset) & g_off.hidden_game.mask);
    if (Readable(owner)) {
      if (!g_off.actor) {
        g_off.actor = true;
        g_off.actor_hidden = FindProp(U32(owner + kClass), "bHidden");
      }
      if (g_off.actor_hidden.ok()) hidden |= (U32(owner + g_off.actor_hidden.offset) & g_off.actor_hidden.mask) != 0;
    }
    const float lrt = g_off.last_render_time.ok() ? F32(c + g_off.last_render_time.offset) : -1;
    fmt::format_to(std::back_inserter(s), ",\"hidden\":{},\"last_render_time\":{:.4f},\"rendered\":{}", hidden, lrt,
                   lrt >= now - 0.05f);
    if (p.kind == Kind::kParticle) Emitters(s, c);
    if (p.kind == Kind::kStatic && g_off.forced_lod.ok())
      fmt::format_to(std::back_inserter(s), ",\"lod\":{}", int32_t(U32(c + g_off.forced_lod.offset)));
    if (p.kind == Kind::kSkeletal) {
      if (g_off.predicted_lod.ok())
        fmt::format_to(std::back_inserter(s), ",\"lod\":{}", int32_t(U32(c + g_off.predicted_lod.offset)));
      if (g_off.space_bases.ok()) {
        const uint32_t data = U32(c + g_off.space_bases.offset), n = U32(c + g_off.space_bases.offset + 4);
        s += ",\"bones\":[";
        for (uint32_t i = 0; Readable(data) && i < n && i < 1024; ++i) {
          if (i) s += ',';
          Floats(s, data + 64 * i, 16);
        }
        s += "]";
      }
      if (Readable(mesh)) skeletons[mesh] = true;
    }
    s += "}";
  }
  s += "\n],\"skeletons\":{";
  first = true;
  for (const auto& [mesh, _] : skeletons) {
    if (!g_off.skel_asset) {
      g_off.skel_asset = true;
      g_off.ref_skeleton = FindProp(U32(mesh + kClass), "RefSkeleton");
      g_off.skel_materials = FindProp(U32(mesh + kClass), "Materials");
      REXLOG_INFO("scenefeed: SkeletalMesh RefSkeleton +{:X} Materials +{:X}", g_off.ref_skeleton.offset,
                  g_off.skel_materials.offset);
    }
    if (!first) s += ",\n";
    first = false;
    Str(s, Path(mesh));
    s += ':';
    const size_t before = s.size();
    Skeleton(s, mesh);
    if (s.size() == before) s += "null";
  }
  s += "}";
  LightsPostJson(s, world_info);
  s += "}\n";
  return s;
}

struct State {
  int dump_left = 0, dump_every = 1, time_left = 0, emitters_left = 0;
  bool cached = false;
  uint32_t frame = 0, written = 0;
  float last_real_time = -1;
  std::vector<Primitive> prims;
  uint32_t prims_age = 1u << 30;
  std::vector<Times> times;
  std::string dir = [] {
    const char* v = std::getenv("BOTS_SCENEFEED_DIR");
    return std::string(v && *v ? v : "scenefeed");
  }();
};
State g_state;

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

void ReportTimes() {
  auto& v = g_state.times;
  if (v.empty()) return;
  auto stat = [&](double Times::*m, const char* name) {
    std::vector<double> x;
    for (auto& t : v) x.push_back(t.*m);
    std::sort(x.begin(), x.end());
    REXLOG_INFO("scenefeed time: {:<9} median {:.3f} ms  p90 {:.3f}  max {:.3f}  ({} frames)", name, x[x.size() / 2],
                x[x.size() * 9 / 10], x.back(), x.size());
  };
  stat(&Times::enumerate, "enumerate");
  stat(&Times::extract, "extract");
  stat(&Times::json, "json");
  stat(&Times::write, "write");
  REXLOG_INFO("scenefeed time: {} primitives of {} objects (last frame)", v.back().primitives, v.back().objects);
  v.clear();
}

void Census() {
  if (!FindObjectTable()) {
    REXLOG_WARN("scenefeed census: object table not found");
    return;
  }
  struct Count {
    uint32_t instances = 0, attached = 0, hidden = 0, rendered = 0;
    Kind kind;
  };
  std::unordered_map<std::string, Count> by_class;
  uint32_t world_info = 0;
  Camera cam;
  ReadCamera(cam, world_info);
  static Field time_seconds;
  if (Readable(world_info) && !time_seconds.ok()) time_seconds = FindProp(U32(world_info + kClass), "TimeSeconds");
  const float now = time_seconds.ok() ? F32(world_info + time_seconds.offset) : 0;
  const uint32_t data = U32(g_objects), count = U32(g_objects + 4);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = U32(data + 4 * i);
    if (!o) continue;
    const ClassInfo& ci = Classify(U32(o + kClass));
    if (ci.kind == Kind::kNone) continue;
    Count& c = by_class[ci.name];
    c.kind = ci.kind;
    ++c.instances;
    if (!U32(o + g_off.scene_info.offset)) continue;
    ++c.attached;
    if (U32(o + g_off.hidden_game.offset) & g_off.hidden_game.mask) ++c.hidden;
    if (F32(o + g_off.last_render_time.offset) >= now - 0.05f) ++c.rendered;
  }
  REXLOG_INFO("scenefeed census: {} objects, time {:.3f}", count, now);
  for (const auto& [name, c] : by_class)
    REXLOG_INFO("scenefeed census: {:<34} {:<9} instances {:>6} attached {:>6} hidden {:>5} rendered {:>5}", name,
                KindName(c.kind), c.instances, c.attached, c.hidden, c.rendered);
}

// Is <p> a live UObject (its Index points back at it in the object table)?
bool IsObject(uint32_t p) {
  if (!Readable(p) || !g_objects) return false;
  const uint32_t index = U32(p + 0x04);
  return index < U32(g_objects + 4) && U32(U32(g_objects) + 4 * index) == p;
}
// A live UClass: an object whose class is the class object UClass, the one object that is its own class. (A freed
// component's +0x34 can hold any pointer, including a live object that is not a class; walking its "superclass" chain
// read garbage and crashed a T480 run.)
bool NotAClass(uint32_t cls) {
  if (!g_objects) return !Readable(cls);
  if (!IsObject(cls)) return true;
  const uint32_t meta = U32(cls + kClass);
  return !IsObject(meta) || U32(meta + kClass) != meta;
}

// <o> is still the live object at object-table position <index>. Unlike IsObject this reads only the table, never
// <o>'s own memory: a component kept from an earlier frame may have been freed by a level unload into memory that is
// no longer mapped, and reading it there faults (chapter loads crashed in Track's per-frame checks).
bool AliveAt(uint32_t o, uint32_t index) {
  return g_objects && index < U32(g_objects + 4) && U32(U32(g_objects) + 4 * index) == o;
}

// Research dump for the native particle structures: every attached ParticleSystemComponent's EmitterInstances,
// raw (guest memory, big-endian), plus whatever their first 0x200 bytes point at. emitters_<frame>.bin holds
// regions {u32 address, u32 length, bytes}; emitters_<frame>.json indexes components and names the UObjects those
// pointers hit.
void DumpEmitters(uint32_t frame, const std::vector<Primitive>& prims) {
  static Field instances;
  std::string json = fmt::format("{{\"frame\":{},\"components\":[\n", frame);
  std::string bin;
  std::unordered_map<uint32_t, bool> done;
  auto region = [&](uint32_t a, uint32_t len) {
    if (!Readable(a) || done.count(a)) return;
    done[a] = true;
    const uint32_t h[2] = {a, len};
    bin.append(reinterpret_cast<const char*>(h), 8);
    bin.append(reinterpret_cast<const char*>(Raw(a)), len);
  };
  std::unordered_map<uint32_t, std::string> objects;
  bool first = true;
  for (const Primitive& p : prims) {
    if (p.kind != Kind::kParticle) continue;
    const uint32_t c = p.component;
    if (!instances.ok()) instances = FindProp(U32(c + kClass), "EmitterInstances");
    if (!instances.ok()) return;
    const uint32_t data = U32(c + instances.offset), n = U32(c + instances.offset + 4);
    if (!first) json += ",\n";
    first = false;
    json += fmt::format("{{\"component\":{},\"template\":", c);
    Str(json, Path(g_off.template_ref.ok() ? U32(c + g_off.template_ref.offset) : 0));
    json += ",\"name\":";
    Str(json, Path(c));
    json += ",\"transform\":";
    Floats(json, c + g_off.local_to_world.offset, 16);
    json += ",\"instances\":[";
    for (uint32_t i = 0; Readable(data) && i < n && i < 64; ++i) {
      const uint32_t inst = U32(data + 4 * i);
      json += fmt::format("{}{}", i ? "," : "", inst);
      if (!Readable(inst)) continue;
      region(inst, 0x400);
      for (uint32_t k = 0; k < 0x200; k += 4) {
        const uint32_t q = U32(inst + k);
        if (!Readable(q) || q == inst) continue;
        if (IsObject(q)) {
          objects[q] = Path(q) + " (" + ClassName(q) + ")";
          continue;
        }
        region(q, 0x2000);
      }
    }
    json += "]}";
  }
  json += "\n],\"objects\":{";
  first = true;
  for (const auto& [a, name] : objects) {
    json += fmt::format("{}\"{}\":", first ? "" : ",", a);
    first = false;
    Str(json, name);
  }
  json += "}}\n";
  char stem[64];
  std::snprintf(stem, sizeof(stem), "/emitters_%06u", frame);
  std::ofstream(g_state.dir + stem + ".json", std::ios::binary) << json;
  std::ofstream(g_state.dir + stem + ".bin", std::ios::binary) << bin;
}

void RequestDraws(int frames) {
  std::error_code ec;
  std::filesystem::create_directories(g_state.dir, ec);
  std::ofstream(g_state.dir + "/draws.req") << frames << "\n";
  REXLOG_INFO("scenefeed: asked the renderer for {} frames of draws ({}/draws.req)", frames, g_state.dir);
}
}  // namespace

bool Command(uint8_t* base, const std::string& cmd) {
  if (cmd.rfind("scenefeed", 0) != 0) return false;
  g_base = base;
  char what[32] = {}, extra[32] = {};
  int n = 0, every = 1;
  std::sscanf(cmd.c_str(), "scenefeed %31s %d %31s", what, &n, extra);
  const std::string w = what;
  if (w == "dump") {
    if (std::isdigit(uint8_t(extra[0]))) every = std::max(1, std::atoi(extra));
    g_state.dump_left = std::max(1, n), g_state.dump_every = every;
    std::error_code ec;
    std::filesystem::create_directories(g_state.dir, ec);
    REXLOG_INFO("scenefeed: dumping {} frames (every {}) to {}", g_state.dump_left, every, g_state.dir);
  } else if (w == "time") {
    g_state.time_left = std::max(1, n), g_state.cached = std::string_view(extra) == "cached";
  } else if (w == "census") {
    Census();
  } else if (w == "emitters") {
    g_state.emitters_left = std::max(1, n);
    std::error_code ec;
    std::filesystem::create_directories(g_state.dir, ec);
  } else if (w == "draws") {
    RequestDraws(std::max(1, n));
  } else {
    REXLOG_WARN("scenefeed: unknown command '{}'", cmd);
  }
  return true;
}

void Tick(uint8_t* base) {
  State& st = g_state;
  if (!st.dump_left && !st.time_left && !st.emitters_left) return;
  g_base = base;
  // Once per game frame: the input poll runs more than once a frame, so only a new RealTimeSeconds counts.
  Camera cam;
  uint32_t world_info = 0;
  const auto t0 = std::chrono::steady_clock::now();
  ReadCamera(cam, world_info);
  static Field real_time;
  if (Readable(world_info) && !real_time.ok()) real_time = FindProp(U32(world_info + kClass), "RealTimeSeconds");
  if (!real_time.ok() || !Readable(world_info)) return;
  const float rt = F32(world_info + real_time.offset);
  if (rt == st.last_real_time) return;
  st.last_real_time = rt;
  ++st.frame;

  Times t;
  const bool dumping = st.dump_left > 0;
  const bool full = dumping || !st.cached || ++st.prims_age >= 64;
  if (full) {
    Enumerate(st.prims, t);
    st.prims_age = 0;
  } else {
    // Incremental-tracking stand-in: drop entries that were detached or reused since the last full walk.
    std::erase_if(st.prims, [](const Primitive& p) {
      return !Readable(p.component) || Classify(U32(p.component + kClass)).kind != p.kind ||
             !U32(p.component + g_off.scene_info.offset);
    });
  }
  const auto t1 = std::chrono::steady_clock::now();
  t.enumerate = Ms(t0, t1);
  t.primitives = uint32_t(st.prims.size());
  if (!t.objects && g_objects) t.objects = U32(g_objects + 4);
  const int64_t host_us =
      std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();

  if (st.emitters_left > 0) {
    if (!full) Enumerate(st.prims, t);
    DumpEmitters(st.frame, st.prims);
    if (!--st.emitters_left) REXLOG_INFO("scenefeed: emitter dump finished (frame {})", st.frame);
    if (!dumping && !st.time_left) return;
  }
  if (dumping) {
    if ((st.frame % st.dump_every) == 0) {
      const auto j0 = std::chrono::steady_clock::now();
      const std::string json = FrameJson(st.frame, cam, world_info, st.prims, host_us);
      const auto j1 = std::chrono::steady_clock::now();
      char name[64];
      std::snprintf(name, sizeof(name), "/frame_%06u.json", st.frame);
      std::ofstream(st.dir + name, std::ios::binary) << json;
      const auto j2 = std::chrono::steady_clock::now();
      t.json = Ms(j0, j1), t.write = Ms(j1, j2);
      ++st.written;
      --st.dump_left;
      st.times.push_back(t);
      if (!st.dump_left) {
        REXLOG_INFO("scenefeed: wrote {} frames to {} (last: {} primitives)", st.written, st.dir, t.primitives);
        ReportTimes();
      }
    }
    return;
  }
  // Timing only: read every field the JSON would carry, without formatting.
  const auto e0 = std::chrono::steady_clock::now();
  volatile float sink = 0;
  for (const Primitive& p : st.prims) {
    const uint32_t c = p.component;
    for (int i = 0; i < 16; ++i) sink = sink + F32(c + g_off.local_to_world.offset + 4 * i);
    for (int i = 0; i < 7; ++i) sink = sink + F32(c + g_off.bounds.offset + 4 * i);
    sink = sink + float(U32(c + g_off.hidden_game.offset) & 1) + F32(c + g_off.last_render_time.offset);
    if (g_off.owner.ok()) sink = sink + float(U32(c + g_off.owner.offset) & 1);
    if (p.kind == Kind::kSkeletal && g_off.space_bases.ok()) {
      const uint32_t data = U32(c + g_off.space_bases.offset), n = U32(c + g_off.space_bases.offset + 4);
      for (uint32_t i = 0; Readable(data) && i < n && i < 1024; ++i)
        for (int k = 0; k < 16; ++k) sink = sink + F32(data + 64 * i + 4 * k);
    }
  }
  t.extract = Ms(e0, std::chrono::steady_clock::now());
  st.times.push_back(t);
  if (!--st.time_left) {
    REXLOG_INFO("scenefeed time ({}):", st.cached ? "cached list, full walk every 64 frames" : "full walk every frame");
    ReportTimes();
  }
}


// Hybrid fill 4: publish this frame's static and skeletal meshes to the n1 renderer (hybrid_state.h HybridScene). A cached
// primitive list, re-walked every BOTS_N1_RESCAN frames (default 16; the walk costs about 1.5 ms), each entry checked
// every frame as the timing mode does. Paths are interned per component and rebuilt when its mesh or materials change.

namespace {
// From the original's live-feed part (FrameJson writes material paths with it).

bool RuntimeObject(const std::string& path) {
  return path.starts_with("Transient.") || path.find(".TheWorld.") != std::string::npos;
}

// One parameter array of a MaterialInstanceConstant (ScalarParameterValues, VectorParameterValues,
// TextureParameterValues), appended as "&<kind>:<name>=<value>" unless an outer instance already set that name.
void InstanceParams(std::string& out, std::vector<uint32_t>& seen, uint32_t mic, const char* array, char kind) {
  const uint32_t rec = PropRecord(mic, array);
  if (!rec) return;
  const uint32_t inner = U32(rec + kSubFirst);  // UArrayProperty Inner: a StructProperty
  const uint32_t st = Readable(inner) ? U32(inner + kSubFirst) : 0;  // UStructProperty Struct
  if (!Readable(st)) return;
  const uint32_t size = U32(inner + 0x48);
  const Field name = FindProp(st, "ParameterName"), value = FindProp(st, "ParameterValue");
  if (!name.ok() || !value.ok() || !size || size > 0x200) return;
  const uint32_t at = mic + U32(rec + kPropOffset), data = U32(at), n = U32(at + 4);
  for (uint32_t k = 0; Readable(data) && k < n && k < 32; ++k) {
    const uint32_t e = data + k * size, nm = U32(e + name.offset);
    const uint32_t key = (uint32_t(kind) << 24) ^ nm;
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
    seen.push_back(key);
    fmt::format_to(std::back_inserter(out), "&{}:{}=", kind, NameText(nm));
    if (kind == 's') {
      fmt::format_to(std::back_inserter(out), "{:.4g}", F32(e + value.offset));
    } else if (kind == 'v') {
      const uint32_t v = e + value.offset;
      fmt::format_to(std::back_inserter(out), "{:.4g},{:.4g},{:.4g},{:.4g}", F32(v), F32(v + 4), F32(v + 8), F32(v + 12));
    } else {
      const uint32_t t = U32(e + value.offset);
      if (IsObject(t)) out += Path(t);
    }
  }
}

// A material's path for the renderer: the first cooked material up the Parent chain. With `params`, a run-time
// instance's own parameter values follow after '?' ("<path>?&s:Name=v&v:Name=r,g,b,a&t:Name=<texture>"; n1's
// matir applies them over the cooked chain), so per-instance looks (paint, tint, fades) survive. `runtime` is set
// when the material was made at run time, as its parameters can change from frame to frame.
std::string MaterialPath(uint32_t m, bool params, bool* runtime) {
  std::string extra;
  std::vector<uint32_t> seen;
  for (int depth = 0; depth < 8 && IsObject(m); ++depth) {
    std::string p = Path(m);
    if (!RuntimeObject(p)) return extra.empty() ? p : p + "?" + extra;
    if (runtime) *runtime = true;
    if (params && !std::strcmp(ClassName(m), "MaterialInstanceConstant")) {
      InstanceParams(extra, seen, m, "ScalarParameterValues", 's');
      InstanceParams(extra, seen, m, "VectorParameterValues", 'v');
      InstanceParams(extra, seen, m, "TextureParameterValues", 't');
    }
    m = PropU32(m, "Parent");
  }
  return {};
}

}  // namespace

// ---- Hybrid fill 4: this frame's static and skeletal meshes for the n1 renderer (hybrid_state.h HybridScene) ----
// The first cut of skullburglar's Live(): meshes, transforms, materials, bones and light environments only (no
// lights/fog/post feed, particles, trees or decals yet). A cached primitive list, re-walked every 16 frames (the walk
// costs ~1.5 ms); each entry is checked every frame. Paths are interned per component and rebuilt when its mesh or
// materials change.
namespace {
struct LiveComponent {
  uint32_t name = 0, outer = 0, mesh = 0, mats = 0, mat_count = 0, dle = 0;
  const char* mesh_path = nullptr;
  const char* light_path = nullptr;
  const char* env_path = nullptr;
  std::vector<const char*> materials;
};

// Interned strings live for the process (the renderer keys its caches on the pointers).
const char* Intern(const std::string& v) {
  if (v.empty()) return nullptr;
  static std::mutex mutex;
  static std::unordered_map<std::string, const char*> table;
  std::lock_guard lock(mutex);
  auto [it, added] = table.try_emplace(v, nullptr);
  if (added) {
    char* copy = new char[v.size() + 1];
    std::memcpy(copy, v.c_str(), v.size() + 1);
    it->second = copy;
  }
  return it->second;
}

// ---- Lights, fog and post for n1 (skullburglar's LightsBin / FogBin / PostBin, B-188; hybrid_state.h HybridLighting) ----
// Read from the same guest objects the JSON dump writes, with the renderer's defaults for a missing property.
// Property records are looked up once per class. Bourne's reflection offsets: Children +0x44, Offset +0x60, the
// subclass's first field (BitMask / Struct / Enum) +0x80, 4-byte FNames in an enum's names.
struct FieldEx {
  uint32_t offset = 0xFFFFFFFF, mask = 0, sub = 0;  // sub: a struct property's struct, a byte property's enum
  bool ok() const { return offset != 0xFFFFFFFF; }
};

FieldEx FindEx(uint32_t cls, const char* name) {
  for (int depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    for (uint32_t f = U32(cls + kChildren), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n)
      if (!std::strcmp(NameText(U32(f + 0x2C)), name)) {
        FieldEx out{U32(f + kPropOffset), 0, U32(f + kSubFirst)};
        if (!std::strcmp(ClassName(f), "BoolProperty")) out.mask = out.sub;
        return out;
      }
  return {};
}
FieldEx Member(const FieldEx& prop, const char* name) { return prop.ok() ? FindEx(prop.sub, name) : FieldEx{}; }
float FieldF(uint32_t base, const FieldEx& f, float fallback) { return f.ok() ? F32(base + f.offset) : fallback; }
bool FieldB(uint32_t base, const FieldEx& f) { return f.ok() && (U32(base + f.offset) & f.mask); }

struct ColorLayout {
  FieldEx r, g, b;
  void Init(const FieldEx& prop) { r = Member(prop, "R"), g = Member(prop, "G"), b = Member(prop, "B"); }
  void Bytes(uint32_t base, const FieldEx& prop, float out[3]) const {
    const uint32_t at = base + prop.offset;
    out[0] = r.ok() ? float(*Raw(at + r.offset)) : 0.f;
    out[1] = g.ok() ? float(*Raw(at + g.offset)) : 0.f;
    out[2] = b.ok() ? float(*Raw(at + b.offset)) : 0.f;
  }
};

constexpr uint32_t kLfEnabled = 1, kLfBaked = 2, kLfCast = 4, kLfStatic = 8, kLfDynamic = 16, kLfCastShadows = 32,
                   kLfModulated = 64, kLfSkyRaw = 128, kLfUseVolumes = 256;

struct LightLayout {
  int kind = -1;
  FieldEx l2w, brightness, color, affects, channels, direct, lower_b, lower_c, wsil, env, enabled, radius, falloff,
      inner, outer, cast, ch_static, ch_dynamic, use_volumes;
  FieldEx cast_shadows, shadow_mode, mod_color, mod_r, mod_g, mod_b, mod_fade, shadow_falloff, min_falloff;
  int modulate = -1;
  ColorLayout color_l, lower_l;
  FieldEx sh_r, sh_g, sh_b, sh_rv, sh_gv, sh_bv;
};

const LightLayout& LayoutFor(uint32_t light) {
  static std::unordered_map<uint32_t, LightLayout> layouts;
  const uint32_t cls = U32(light + kClass);
  auto [it, fresh] = layouts.try_emplace(cls);
  LightLayout& L = it->second;
  if (!fresh) return L;
  const auto is = [&](const char* n) { return ClassChainHas(cls, n); };
  L.kind = is("SpotLightComponent")                ? 1
           : is("PointLightComponent")             ? 0
           : is("DirectionalLightComponent")       ? 2
           : is("SphericalHarmonicLightComponent") ? 3
           : is("SkyLightComponent")               ? 4
                                                   : -1;
  if (L.kind < 0) return L;
  L.l2w = FindEx(cls, "LightToWorld"), L.brightness = FindEx(cls, "Brightness"), L.color = FindEx(cls, "LightColor");
  L.affects = FindEx(cls, "LightAffectsClassification"), L.channels = FindEx(cls, "LightingChannels");
  L.direct = FindEx(cls, "UseDirectLightMap"), L.lower_b = FindEx(cls, "LowerBrightness");
  L.lower_c = FindEx(cls, "LowerColor"), L.wsil = FindEx(cls, "WorldSpaceIncidentLighting");
  L.env = FindEx(cls, "LightEnvironment"), L.enabled = FindEx(cls, "bEnabled"), L.radius = FindEx(cls, "Radius");
  L.falloff = FindEx(cls, "FalloffExponent"), L.inner = FindEx(cls, "InnerConeAngle");
  L.outer = FindEx(cls, "OuterConeAngle"), L.cast = FindEx(cls, "CastDynamicShadows");
  L.ch_static = Member(L.channels, "Static"), L.ch_dynamic = Member(L.channels, "Dynamic");
  L.use_volumes = FindEx(cls, "bUseVolumes");
  L.cast_shadows = FindEx(cls, "CastShadows"), L.shadow_mode = FindEx(cls, "LightShadowMode");
  L.mod_color = FindEx(cls, "ModShadowColor"), L.mod_fade = FindEx(cls, "ModShadowFadeoutExponent");
  L.shadow_falloff = FindEx(cls, "ShadowFalloffExponent"), L.min_falloff = FindEx(cls, "MinShadowFalloffRadius");
  L.mod_r = Member(L.mod_color, "R"), L.mod_g = Member(L.mod_color, "G"), L.mod_b = Member(L.mod_color, "B");
  if (L.shadow_mode.ok() && IsObject(L.shadow_mode.sub)) {
    const uint32_t names = U32(L.shadow_mode.sub + 0x44), count = U32(L.shadow_mode.sub + 0x48);
    for (uint32_t k = 0; Readable(names) && k < count && k < 256; ++k)
      if (!std::strcmp(NameText(U32(names + 4 * k)), "LightShadow_Modulate")) L.modulate = int(k);
  }
  L.color_l.Init(L.color), L.lower_l.Init(L.lower_c);
  L.sh_r = Member(L.wsil, "R"), L.sh_g = Member(L.wsil, "G"), L.sh_b = Member(L.wsil, "B");
  L.sh_rv = Member(L.sh_r, "V"), L.sh_gv = Member(L.sh_g, "V"), L.sh_bv = Member(L.sh_b, "V");
  REXLOG_INFO("scenefeed lights: {} kind {}: color +{:X} brightness +{:X} l2w +{:X} env +{:X} enabled +{:X}/{:X} radius +{:X}",
              ClassName(light), L.kind, L.color.offset, L.brightness.offset, L.l2w.offset, L.env.offset,
              L.enabled.offset, L.enabled.mask, L.radius.offset);
  return L;
}

struct DleInfo {
  uint32_t name = 0, outer = 0;
  const char* path = nullptr;
  uint32_t stamp = 0, index = 0;
};
std::unordered_map<uint32_t, DleInfo> g_dle;

uint32_t EnvIndex(bots::HybridLighting& out, uint32_t dle, uint32_t stamp) {
  DleInfo& d = g_dle[dle];
  if (!d.path || d.name != U32(dle + 0x2C) || d.outer != U32(dle + 0x28)) {
    d.name = U32(dle + 0x2C), d.outer = U32(dle + 0x28);
    d.path = Intern(Path(dle));
    d.stamp = 0;
  }
  if (d.stamp != stamp) {
    d.stamp = stamp;
    d.index = uint32_t(out.paths.size());
    out.paths.push_back(d.path);
  }
  return d.index;
}

// Baked or disabled level point and spot lights are the lightmaps' business and left out.
void LightsBin(bots::HybridLighting& out, uint32_t stamp, bool envs) {
  for (uint32_t l : g_extras.lights) {
    if (!IsObject(l)) continue;
    const LightLayout& L = LayoutFor(l);
    if (L.kind < 0) continue;
    const uint32_t dle = L.env.ok() ? U32(l + L.env.offset) : 0;
    const bool has_env = IsObject(dle);
    if (has_env && !envs) continue;
    const bool baked = FieldB(l, L.direct) || (L.affects.ok() && *Raw(l + L.affects.offset) == 2);
    if (!has_env && (L.kind == 0 || L.kind == 1) && (!FieldB(l, L.enabled) || baked)) continue;
    bots::HybridLight& h = out.lights.emplace_back();
    std::memset(&h, 0, sizeof(h));
    h.kind = uint32_t(L.kind);
    h.flags = (FieldB(l, L.enabled) ? kLfEnabled : 0) | (baked ? kLfBaked : 0) | (FieldB(l, L.cast) ? kLfCast : 0) |
              (!L.channels.ok() || (L.ch_static.ok() && (U32(l + L.channels.offset + L.ch_static.offset) & L.ch_static.mask))
                   ? kLfStatic
                   : 0) |
              (!L.channels.ok() || (L.ch_dynamic.ok() && (U32(l + L.channels.offset + L.ch_dynamic.offset) & L.ch_dynamic.mask))
                   ? kLfDynamic
                   : 0);
    if (FieldB(l, L.cast_shadows)) h.flags |= kLfCastShadows;
    if (FieldB(l, L.use_volumes)) h.flags |= kLfUseVolumes;
    if (L.modulate >= 0 && *Raw(l + L.shadow_mode.offset) == L.modulate) h.flags |= kLfModulated;
    for (int k = 0; k < 3; ++k) {
      const FieldEx& m = k == 0 ? L.mod_r : k == 1 ? L.mod_g : L.mod_b;
      h.mod_shadow[k] = L.mod_color.ok() && m.ok() ? F32(l + L.mod_color.offset + m.offset) : 1.f;
    }
    h.mod_fade_exp = FieldF(l, L.mod_fade, 3.f), h.shadow_falloff_exp = FieldF(l, L.shadow_falloff, 2.f);
    h.min_shadow_falloff_radius = FieldF(l, L.min_falloff, 0.f);
    h.env = has_env ? EnvIndex(out, dle, stamp) : bots::kHybridNone;
    h.radius = FieldF(l, L.radius, 0.f), h.falloff = FieldF(l, L.falloff, 2.f);
    h.inner_deg = FieldF(l, L.inner, 0.f), h.outer_deg = FieldF(l, L.outer, 44.f);
    if (L.l2w.ok()) {
      const uint32_t m = l + L.l2w.offset;
      for (int k = 0; k < 3; ++k) h.pos[k] = F32(m + 4 * (12 + k)), h.axis[k] = F32(m + 4 * (8 + k));
    }
    const float brightness = FieldF(l, L.brightness, 1.f);
    float c[3] = {0, 0, 0};
    if (L.color.ok()) L.color_l.Bytes(l, L.color, c);
    for (int k = 0; k < 3; ++k) h.color[k] = (c[k] / 255.f) * brightness;
    const float lower_b = FieldF(l, L.lower_b, 0.f);
    float lc[3] = {0, 0, 0};
    if (L.lower_c.ok()) L.lower_l.Bytes(l, L.lower_c, lc);
    for (int k = 0; k < 3; ++k) h.lower[k] = (lc[k] / 255.f) * lower_b;
    if (L.kind >= 0 && L.kind <= 3)
      for (int k = 0; k < 3; ++k) h.lower[k] = c[k] / 255.f;
    if (L.kind == 4) {
      for (int k = 0; k < 3; ++k) h.pos[k] = c[k] / 255.f, h.axis[k] = lc[k] / 255.f;
      h.flags |= kLfSkyRaw;
    }
    h.sh = -1;
    if (L.wsil.ok() && L.sh_rv.ok() && L.sh_gv.ok() && L.sh_bv.ok()) {
      const uint32_t w = l + L.wsil.offset;
      h.sh = int32_t(out.sh.size() / 27);
      for (const auto& [m, v] : {std::pair{&L.sh_r, &L.sh_rv}, {&L.sh_g, &L.sh_gv}, {&L.sh_b, &L.sh_bv}})
        for (int k = 0; k < 9; ++k) out.sh.push_back(F32(w + m->offset + v->offset + 4 * k));
    }
  }
}

void FogBin(bots::HybridFog& fog) {
  std::memset(&fog, 0, sizeof(fog));
  for (uint32_t f : g_extras.fog) {
    if (!IsObject(f) || std::strcmp(ClassName(f), "HeightFogComponent")) continue;
    const uint32_t cls = U32(f + kClass);
    static std::unordered_map<uint32_t, std::array<FieldEx, 7>> layouts;
    auto [it, fresh] = layouts.try_emplace(cls);
    auto& L = it->second;
    if (fresh) {
      const char* names[7] = {"bEnabled", "LightColor", "LightBrightness", "Density", "Height", "StartDistance",
                              "ExtinctionDistance"};
      for (int i = 0; i < 7; ++i) L[i] = FindEx(cls, names[i]);
    }
    if (!FieldB(f, L[0])) return;  // the first height fog decides, enabled or not
    fog.present = 1;
    float c[3] = {0, 0, 0};
    if (L[1].ok()) {
      ColorLayout cl;
      cl.Init(L[1]);
      cl.Bytes(f, L[1], c);
    }
    const float k = FieldF(f, L[2], 1.f);
    // n1 draws the fog in pow(light_color, 2.2) (50 Cent's port). Bourne's haze is lighter: send the colour so that
    // n1's curve gives the chosen one (--n1_fog_color).
    const int mode = REXCVAR_GET(n1_fog_color);
    for (int i = 0; i < 3; ++i) {
      fog.color[i] = (c[i] / 255.f) * k;
      const float want = mode == 1 ? c[i] / 255.f : mode == 2 ? c[i] / 255.f * k : -1.f;
      fog.light_color[i] = want < 0 ? c[i] / 255.f : std::pow(std::max(want, 0.f), 1.f / 2.2f);
    }
    fog.density = FieldF(f, L[3], 0.f) * float(REXCVAR_GET(n1_fog_density));
    fog.height = FieldF(f, L[4], 0.f), fog.start = FieldF(f, L[5], 0.f);
    fog.extinction = FieldF(f, L[6], 1e8f);
    return;
  }
}

// Three numbers of a Color (bytes), Vector or LinearColor struct property.
void Triple(uint32_t base, const FieldEx& prop, float out[3]) {
  out[0] = out[1] = out[2] = std::nanf("");
  if (!prop.ok()) return;
  const std::string_view type = NameText(U32(prop.sub + 0x2C));
  const uint32_t at = base + prop.offset;
  if (type == "Color") {
    const char* m[3] = {"R", "G", "B"};
    for (int i = 0; i < 3; ++i)
      if (const FieldEx f = FindEx(prop.sub, m[i]); f.ok()) out[i] = float(*Raw(at + f.offset));
  } else {
    const bool vec = type == "Vector";
    const char* m[3] = {vec ? "X" : "R", vec ? "Y" : "G", vec ? "Z" : "B"};
    for (int i = 0; i < 3; ++i)
      if (const FieldEx f = FindEx(prop.sub, m[i]); f.ok()) out[i] = F32(at + f.offset);
  }
}

enum : uint32_t { kPfUber = 1, kPfGod = 2, kPfLsBloom = 4, kPfLsBalance = 8, kPfLsScene = 16, kPfLsTone = 32, kPfLast = 64 };

// Bourne's post: the stock UberPostProcessEffect with bUseWorldSettings, so what it draws is the local player's
// CurrentPPInfo.LastSettings (the world's blended post settings: bloom, scene shadows/midtones/highlights,
// desaturation). Swordfish's SfUber and god rays (50 Cent) don't exist here; MaterialEffects in the chain are passed
// on as 50 Cent's are.
void PostBin(bots::HybridLighting& out) {
  bots::HybridPost& p = out.post;
  std::memset(&p, 0, sizeof(p));
  const float nan = std::nanf("");
  p.material_effect = bots::kHybridNone;
  p.uber_bloom_scale = p.uber_bloom_threshold = p.ls_bloom_scale = p.ls_bloom_threshold = nan;
  p.desaturation = p.tone_min = p.tone_max = p.tone_scale = nan;
  for (float& v : p.balance) v = 255.f;
  for (int i = 0; i < 3; ++i) p.shadows[i] = p.midtones[i] = p.highlights[i] = nan;
  for (float& v : p.god) v = nan;
  for (float& v : p.uber_dof) v = nan;
  p.present = 1;
  out.post_materials.clear();
  out.post_params.clear();
  const uint32_t engine = Engine();
  const uint32_t players = IsObject(engine) ? PropRecord(engine, "GamePlayers") : 0;
  const uint32_t lp =
      players && U32(engine + U32(players + kPropOffset) + 4) ? U32(U32(engine + U32(players + kPropOffset))) : 0;
  if (!IsObject(lp)) return;
  const uint32_t chain = PropU32(lp, "PlayerPostProcess");
  if (IsObject(chain)) {
    const Field effects = Prop(chain, "Effects");
    const uint32_t data = effects.ok() ? U32(chain + effects.offset) : 0, n = effects.ok() ? U32(chain + effects.offset + 4) : 0;
    for (uint32_t i = 0; Readable(data) && i < n && i < 32; ++i) {
      const uint32_t e = U32(data + 4 * i);
      if (!IsObject(e) || std::strcmp(ClassName(e), "MaterialEffect")) continue;
      const uint32_t cls = U32(e + kClass);
      if (!FieldB(e, FindEx(cls, "bShowInGame"))) continue;
      const FieldEx m = FindEx(cls, "Material");
      const uint32_t mat = m.ok() ? U32(e + m.offset) : 0;
      if (IsObject(mat) && out.post_materials.size() < 15)
        if (const char* path = Intern(Path(mat))) out.post_materials.push_back(path);
    }
  }
  const FieldEx cur = FindEx(U32(lp + kClass), "CurrentPPInfo");
  const FieldEx last = Member(cur, "LastSettings");
  if (!last.ok()) return;
  const uint32_t s = lp + cur.offset + last.offset, sc = last.sub;
  p.flags |= kPfLast;
  if (FieldB(s, FindEx(sc, "bEnableBloom"))) p.flags |= kPfLsBloom;
  p.ls_bloom_scale = FieldF(s, FindEx(sc, "Bloom_Scale"), nan);
  p.ls_bloom_threshold = FieldF(s, FindEx(sc, "Bloom_Threshold"), nan);
  if (FieldB(s, FindEx(sc, "bEnableSceneEffect"))) {
    p.flags |= kPfLsScene;
    p.desaturation = FieldF(s, FindEx(sc, "Scene_Desaturation"), nan);
    Triple(s, FindEx(sc, "Scene_Shadows"), p.shadows);
    Triple(s, FindEx(sc, "Scene_MidTones"), p.midtones);
    Triple(s, FindEx(sc, "Scene_HighLights"), p.highlights);
  }
}

bool SameLights(const bots::HybridLighting& a, const bots::HybridLighting& b) {
  return a.lights.size() == b.lights.size() && a.sh == b.sh && a.paths == b.paths &&
         (a.lights.empty() || !std::memcmp(a.lights.data(), b.lights.data(), a.lights.size() * sizeof(bots::HybridLight)));
}

// n1 finds a component's baked light by the package in its path, matched against the cooked file names exactly. UE3
// names are case-insensitive and keep the first spelling the game met, which can differ from the file's
// (MAR_ST_Courtyard_ART_m vs MAR_ST_Courtyard_Art_m.xxx, mar_st_base_m vs MAR_ST_Base_m.xxx): give the path the file's
// spelling. The files are listed once from BOTS_N1_COOKED.
std::string FilePackageCase(std::string path) {
  static const std::unordered_map<std::string, std::string> files = [] {
    std::unordered_map<std::string, std::string> m;
    const char* dir = std::getenv("BOTS_N1_COOKED");
    std::error_code ec;
    if (dir && *dir)
      for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".xxx") continue;
        std::string stem = e.path().stem().string(), low = stem;
        for (char& c : low) c = char(std::tolower(uint8_t(c)));
        m.emplace(low, stem);
      }
    REXLOG_INFO("scenefeed: {} cooked package names from {}", m.size(), dir ? dir : "(BOTS_N1_COOKED unset)");
    return m;
  }();
  const size_t dot = path.find('.');
  if (dot == std::string::npos) return path;
  std::string low = path.substr(0, dot);
  for (char& c : low) c = char(std::tolower(uint8_t(c)));
  if (const auto it = files.find(low); it != files.end()) path.replace(0, dot, it->second);
  return path;
}

// --n1_trace=<text>: one log line per traced component (the first time it is fed).
void TraceInstance(uint32_t c, const bots::HybridScene::Instance& inst, const std::vector<const char*>& mats) {
  const std::string& want = REXCVAR_GET(n1_trace);
  if (want.empty() || !inst.mesh) return;
  static std::unordered_set<uint32_t> seen;
  auto lower = [](std::string v) {
    for (char& ch : v) ch = char(std::tolower(uint8_t(ch)));
    return v;
  };
  const std::string w = lower(want);
  const bool all = w == "*";
  if (!all && lower(inst.mesh).find(w) == std::string::npos &&
      (!inst.light || lower(inst.light).find(w) == std::string::npos))
    return;
  if (!seen.insert(c).second) return;
  std::string m;
  for (const char* x : mats) m += std::string(m.empty() ? "" : ", ") + (x ? x : "(none)");
  REXLOG_INFO("n1_trace: component {:08X} mesh {} light {} env {} at ({:.0f}, {:.0f}, {:.0f}) materials [{}]", c,
              inst.mesh, inst.light ? inst.light : "(none)", inst.env ? inst.env : "(none)", inst.transform[12],
              inst.transform[13], inst.transform[14], m);
}

struct FeedParts {
  bool lights = false, fog = false, post = false, envs = false, chars = false;
};
FeedParts Parts() {
  const std::string& v = REXCVAR_GET(n1_feed);
  return {v.find("lights") != std::string::npos, v.find("fog") != std::string::npos,
          v.find("post") != std::string::npos, v.find("envs") != std::string::npos,
          v.find("chars") != std::string::npos};
}

// Character lighting (n1_feed "chars"). Bourne's DynamicLightEnvironments keep their lighting inside the engine (their
// light components are zero-brightness shadow lights), so each character's environment is built here from the level
// lights near it, in the shape n1 expects (UE3's composite: one directional, one point, one SH light per environment):
// the strongest light reaching the character becomes the directional (from that light towards the character), the
// next one the point light, and the sky light plus a share of the rest the SH ambient (band 0 only).
struct CharEnv {
  uint32_t dle;
  float center[3];
};
struct LevelLight {
  float pos[3], axis[3], raw[3];
  float radius, falloff, brightness, cos_outer;
  bool spot;
};

void CharLightsBin(bots::HybridLighting& out, uint32_t stamp, const std::vector<CharEnv>& chars) {
  if (chars.empty()) return;
  std::vector<LevelLight> lights;
  float sky[3] = {0, 0, 0};
  for (uint32_t l : g_extras.lights) {
    if (!IsObject(l)) continue;
    const LightLayout& L = LayoutFor(l);
    if (L.kind < 0 || (L.env.ok() && IsObject(U32(l + L.env.offset))) || !FieldB(l, L.enabled)) continue;
    float c[3] = {0, 0, 0};
    if (L.color.ok()) L.color_l.Bytes(l, L.color, c);
    const float b = FieldF(l, L.brightness, 1.f);
    if (L.kind == 4) {  // sky: UE3's pow(c / 255, 2.2) * brightness
      for (int k = 0; k < 3; ++k) sky[k] += std::pow(c[k] / 255.f, 2.2f) * b;
      continue;
    }
    if (L.kind > 1 || b <= 0 || !L.l2w.ok()) continue;
    LevelLight& x = lights.emplace_back();
    const uint32_t m = l + L.l2w.offset;
    for (int k = 0; k < 3; ++k) x.pos[k] = F32(m + 4 * (12 + k)), x.axis[k] = F32(m + 4 * (8 + k)), x.raw[k] = c[k] / 255.f;
    x.radius = FieldF(l, L.radius, 0.f), x.falloff = FieldF(l, L.falloff, 2.f), x.brightness = b;
    x.spot = L.kind == 1;
    x.cos_outer = std::cos(FieldF(l, L.outer, 44.f) * 3.14159265f / 180.f);
    const float n = std::sqrt(x.axis[0] * x.axis[0] + x.axis[1] * x.axis[1] + x.axis[2] * x.axis[2]);
    if (n > 0) for (float& a : x.axis) a /= n;
  }
  struct Hit {
    float weight;
    const LevelLight* l;
    float att;
  };
  std::vector<Hit> hits;
  for (const CharEnv& ce : chars) {
    hits.clear();
    for (const LevelLight& x : lights) {
      const float d[3] = {ce.center[0] - x.pos[0], ce.center[1] - x.pos[1], ce.center[2] - x.pos[2]};
      const float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
      if (x.radius <= 0 || d2 >= x.radius * x.radius) continue;
      float att = std::pow(std::max(0.f, 1.f - d2 / (x.radius * x.radius)), x.falloff);  // UE3 radial attenuation
      if (x.spot) {
        const float len = std::sqrt(d2);
        const float cosang = len > 0 ? (d[0] * x.axis[0] + d[1] * x.axis[1] + d[2] * x.axis[2]) / len : 1.f;
        if (cosang < x.cos_outer) continue;
      }
      const float luma = 0.3f * x.raw[0] + 0.59f * x.raw[1] + 0.11f * x.raw[2];
      hits.push_back({att * x.brightness * luma, &x, att});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.weight > b.weight; });
    const uint32_t env = EnvIndex(out, ce.dle, stamp);
    auto base = [&](uint32_t kind) -> bots::HybridLight& {
      bots::HybridLight& h = out.lights.emplace_back();
      std::memset(&h, 0, sizeof(h));
      h.kind = kind;
      h.flags = kLfEnabled | kLfDynamic | kLfStatic;
      h.env = env;
      h.sh = -1;
      h.radius = 0, h.falloff = 2, h.outer_deg = 44;
      for (float& v : h.mod_shadow) v = 1;
      h.mod_fade_exp = 3, h.shadow_falloff_exp = 2;
      return h;
    };
    if (!hits.empty()) {  // key: directional from the strongest light towards the character
      const Hit& k = hits[0];
      bots::HybridLight& h = base(2);
      float dir[3] = {ce.center[0] - k.l->pos[0], ce.center[1] - k.l->pos[1], ce.center[2] - k.l->pos[2]};
      const float n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      for (int i = 0; i < 3; ++i) {
        h.axis[i] = n > 0 ? dir[i] / n : (i == 2 ? -1.f : 0.f);
        h.color[i] = k.l->raw[i] * k.l->brightness * k.att;
        h.lower[i] = k.l->raw[i];
      }
    }
    if (hits.size() > 1) {  // the second light as the environment's point light, where it is
      const Hit& p = hits[1];
      bots::HybridLight& h = base(0);
      for (int i = 0; i < 3; ++i) {
        h.pos[i] = p.l->pos[i];
        h.color[i] = p.l->raw[i] * p.l->brightness;
        h.lower[i] = p.l->raw[i];
      }
      h.radius = p.l->radius, h.falloff = p.l->falloff;
    }
    // Ambient: the sky plus a quarter of the other lights (UE3 folds the lights it doesn't pick into the SH).
    float amb[3] = {sky[0], sky[1], sky[2]};
    for (size_t i = 2; i < hits.size() && i < 16; ++i)
      for (int k = 0; k < 3; ++k) amb[k] += 0.25f * std::pow(hits[i].l->raw[k], 2.2f) * hits[i].l->brightness * hits[i].att;
    bots::HybridLight& h = base(3);
    for (int k = 0; k < 3; ++k) h.color[k] = 1.f, h.lower[k] = 1.f;
    h.sh = int32_t(out.sh.size() / 27);
    for (int ch = 0; ch < 3; ++ch)
      for (int i = 0; i < 9; ++i) out.sh.push_back(i == 0 ? amb[ch] / 0.886228f : 0.f);  // band 0: irradiance = c0 * 0.886
  }
}

struct LiveState {
  std::vector<Primitive> prims;
  std::unordered_map<uint32_t, LiveComponent> components;
  uint32_t age = 1u << 30;
  uint64_t frame = 0;
  bots::HybridScene building;
  bool logged = false;
  // lighting feed
  uint32_t env_stamp = 0, lighting_builds = 0;
  bool have_fog_post = false;
  bots::HybridLighting next_light, last_light;
  bots::HybridFog last_fog{};
  bots::HybridPost last_post{};
  std::vector<const char*> last_materials;
  std::vector<CharEnv> char_envs;
};
LiveState g_live;
}  // namespace

void Live(uint8_t* base) {
  bots::HybridState* hs = bots::FindHybridState();
  if (!hs || hs->fill.load(std::memory_order_relaxed) != 4) return;
  g_base = base;
  LiveState& st = g_live;
  const auto t0 = std::chrono::steady_clock::now();
  if (++st.age >= 16) {
    Times unused;
    Enumerate(st.prims, unused);
    std::erase_if(st.prims, [](const Primitive& p) { return p.kind != Kind::kStatic && p.kind != Kind::kSkeletal; });
    st.age = 0;
  } else {
    DropIf(st.prims, [](Primitive& p) {
      return !AliveAt(p.component, p.index) || PrimClass(p).kind != p.kind || !U32(p.component + g_off.scene_info.offset);
    });
  }
  const FeedParts walk_parts = Parts();
  const bool envs = walk_parts.envs, chars = walk_parts.chars && !walk_parts.envs;
  st.char_envs.clear();
  bots::HybridScene& out = st.building;
  out.instances.clear();
  out.skeletal.clear();
  out.bones.clear();
  out.materials.clear();
  out.live_materials.clear();
  out.decals.clear();
  out.decal_receivers.clear();
  out.trees.clear();
  out.emitters.clear();
  out.particle_rows.clear();
  out.lighting.clear();
  out.light_pending = false;
  for (Primitive& p : st.prims) {
    const uint32_t c = p.component;
    const bool skel = p.kind == Kind::kSkeletal;
    const Field& ref = skel ? g_off.skeletal_mesh_ref : g_off.static_mesh_ref;
    if (!ref.ok() || !g_off.local_to_world.ok()) continue;
    const uint32_t mesh = U32(c + ref.offset);
    if (!Readable(mesh)) continue;
    bool hidden = g_off.hidden_game.ok() && (U32(c + g_off.hidden_game.offset) & g_off.hidden_game.mask);
    const uint32_t owner = g_off.owner.ok() ? U32(c + g_off.owner.offset) : 0;
    if (Readable(owner)) {
      if (!g_off.actor) {
        g_off.actor = true;
        g_off.actor_hidden = FindProp(U32(owner + kClass), "bHidden");
      }
      if (g_off.actor_hidden.ok()) hidden |= (U32(owner + g_off.actor_hidden.offset) & g_off.actor_hidden.mask) != 0;
    }
    if (hidden) continue;
    uint32_t mats = 0, mat_count = 0;
    if (g_off.materials.ok()) {
      mats = U32(c + g_off.materials.offset);
      mat_count = std::min<uint32_t>(U32(c + g_off.materials.offset + 4), 64);
      if (!Readable(mats)) mats = 0, mat_count = 0;
    }
    if (!p.live) p.live = &st.components[c];
    LiveComponent& lc = *p.live;
    if (lc.name != U32(c + 0x2C) || lc.outer != U32(c + 0x28) || lc.mesh != mesh || lc.mats != mats ||
        lc.mat_count != mat_count) {
      lc.name = U32(c + 0x2C), lc.outer = U32(c + 0x28), lc.mesh = mesh, lc.mats = mats, lc.mat_count = mat_count;
      lc.mesh_path = Intern(Path(mesh));
      lc.light_path = Intern(FilePackageCase(Path(c)));
      lc.materials.clear();
      for (uint32_t i = 0; i < mat_count; ++i) lc.materials.push_back(Intern(MaterialPath(U32(mats + 4 * i), true, nullptr)));
    }
    const ClassInfo& info = PrimClass(p);
    if (!info.light_env_done) info.light_env_done = true, info.light_env = FindProp(p.cls, "LightEnvironment");
    const uint32_t dle = info.light_env.ok() ? U32(c + info.light_env.offset) : 0;
    if (dle != lc.dle) lc.dle = dle, lc.env_path = IsObject(dle) ? Intern(Path(dle)) : nullptr;
    if (!lc.mesh_path) continue;
    const char* env_path = envs ? lc.env_path : nullptr;
    if (skel) {
      if (chars && lc.env_path) {
        env_path = lc.env_path;
        CharEnv& ce = st.char_envs.emplace_back();
        ce.dle = lc.dle;
        const uint32_t b = c + g_off.bounds.offset;
        for (int i = 0; i < 3; ++i)
          ce.center[i] = g_off.bounds.ok() ? F32(b + 4 * i) : F32(c + g_off.local_to_world.offset + 4 * (12 + i));
      }
      bots::HybridScene::Skel& sk = out.skeletal.emplace_back();
      for (int i = 0; i < 16; ++i) sk.transform[i] = F32(c + g_off.local_to_world.offset + 4 * i);
      sk.mesh = lc.mesh_path;
      sk.bone_offset = uint32_t(out.bones.size() / 16);
      sk.bone_count = 0;
      if (g_off.space_bases.ok()) {
        const uint32_t data = U32(c + g_off.space_bases.offset), n = U32(c + g_off.space_bases.offset + 4);
        if (Readable(data) && n <= 1024) {
          for (uint32_t i = 0; i < 16 * n; ++i) out.bones.push_back(F32(data + 4 * i));
          sk.bone_count = n;
        }
      }
      sk.lod = g_off.predicted_lod.ok() ? uint32_t(std::max(0, int32_t(U32(c + g_off.predicted_lod.offset)))) : 0;
      sk.flags = 0;
      if (g_off.bounds.ok()) {  // FBoxSphereBounds {Origin, BoxExtent, SphereRadius}
        const uint32_t b = c + g_off.bounds.offset;
        sk.bounds[0] = F32(b), sk.bounds[1] = F32(b + 4), sk.bounds[2] = F32(b + 8), sk.bounds[3] = F32(b + 24);
      } else {
        sk.bounds[0] = sk.bounds[1] = sk.bounds[2] = 0, sk.bounds[3] = 1e30f;
      }
      sk.env = env_path;
      sk.mat_offset = uint32_t(out.materials.size());
      sk.mat_count = uint32_t(lc.materials.size());
      out.materials.insert(out.materials.end(), lc.materials.begin(), lc.materials.end());
      continue;
    }
    bots::HybridScene::Instance& inst = out.instances.emplace_back();
    for (int i = 0; i < 16; ++i) inst.transform[i] = F32(c + g_off.local_to_world.offset + 4 * i);
    inst.mesh = lc.mesh_path;
    inst.light = lc.light_path;
    inst.env = env_path;
    inst.mat_offset = uint32_t(out.materials.size());
    inst.mat_count = uint32_t(lc.materials.size());
    out.materials.insert(out.materials.end(), lc.materials.begin(), lc.materials.end());
    TraceInstance(c, inst, lc.materials);
  }
  out.walked = uint32_t(st.prims.size());
  // Lights every frame (sent only when they changed), fog and post every 16th build.
  {
    const auto tl = std::chrono::steady_clock::now();
    const FeedParts parts = Parts();
    bots::HybridLighting& nl = st.next_light;
    nl.Clear();
    ++st.env_stamp;
    if (parts.lights || parts.envs) LightsBin(nl, st.env_stamp, parts.envs);
    if (parts.chars && !parts.envs) CharLightsBin(nl, st.env_stamp, st.char_envs);
    bool post_changed = false;
    if (st.lighting_builds++ % 16 == 0) {
      if (parts.fog) FogBin(nl.fog);
      else std::memset(&nl.fog, 0, sizeof(nl.fog));
      nl.fogvols.clear();
      if (parts.post) PostBin(nl);
      else std::memset(&nl.post, 0, sizeof(nl.post)), nl.post.material_effect = bots::kHybridNone, nl.post_materials.clear();
      post_changed = !st.have_fog_post || std::memcmp(&nl.fog, &st.last_fog, sizeof(bots::HybridFog)) ||
                     std::memcmp(&nl.post, &st.last_post, sizeof(bots::HybridPost)) ||
                     nl.post_materials != st.last_materials;
    }
    if (!SameLights(nl, st.last_light) || post_changed) {
      nl.has_fog_post = post_changed;
      if (post_changed) st.last_fog = nl.fog, st.last_post = nl.post, st.last_materials = nl.post_materials;
      st.have_fog_post |= post_changed;
      st.last_light.lights = nl.lights, st.last_light.sh = nl.sh, st.last_light.paths = nl.paths;
      out.light = nl;
      out.light_pending = true;
    }
    out.lighting_ms = float(Ms(tl, std::chrono::steady_clock::now()));
  }
  out.frame = ++st.frame;
  out.cam_seq = hs->published.load(std::memory_order_acquire);
  out.build_ms = float(Ms(t0, std::chrono::steady_clock::now()));
  if (!st.logged || st.frame % 900 == 0) {
    st.logged = true;
    REXLOG_INFO("scenefeed live: frame {}: {} static, {} skeletal ({} bones) of {} primitives in {:.2f} ms; {} lights "
                "({} environments), fog {}, post flags {:X} ({} materials), lighting {:.2f} ms", st.frame,
                out.instances.size(), out.skeletal.size(), out.bones.size() / 16, out.walked, out.build_ms,
                st.last_light.lights.size(), st.last_light.paths.size(), st.last_fog.present, st.last_post.flags,
                st.last_materials.size(), out.lighting_ms);
  }
  std::lock_guard lock(hs->scene_mutex);
  // Keep a lighting update the renderer hasn't taken yet when this frame brings none, and fold an unconsumed fog and
  // post update into a newer lights-only one (as skullburglar's Live does).
  if (hs->scene.light_pending) {
    if (!out.light_pending) {
      std::swap(out.light, hs->scene.light);
      out.light_pending = true;
    } else if (hs->scene.light.has_fog_post && !out.light.has_fog_post) {
      out.light.has_fog_post = true;
      out.light.fog = hs->scene.light.fog, out.light.post = hs->scene.light.post;
      out.light.post_materials = hs->scene.light.post_materials;
      out.light.post_params = hs->scene.light.post_params;
      out.light.fogvols = hs->scene.light.fogvols;
    }
  }
  std::swap(hs->scene, out);
  hs->scene_cv.notify_all();
}

void Poll(uint8_t* base) {
  const std::string& cmd = REXCVAR_GET(scenefeed);
  if (cmd.empty()) return;
  static const auto first = std::chrono::steady_clock::now();
  static bool sent = false;
  if (!sent) {
    if (std::chrono::steady_clock::now() - first < std::chrono::seconds(REXCVAR_GET(scenefeed_delay))) return;
    sent = true;
    g_state.dir = REXCVAR_GET(scenefeed_dir);
    REXLOG_INFO("scenefeed: running \"{}\"", cmd);
    Command(base, "scenefeed " + cmd);
  }
  Tick(base);
}

}  // namespace tbc::scenefeed
