// Hybrid renderer spike (n1 for Bourne): where a separate renderer for the 3D scene plugs into UE3, and what turning
// the game's own scene rendering off saves. Off unless asked for (--hybrid_suppress / --hybrid_ab / --hybrid_report /
// --hybrid_fill). Port of skullburglar's hybrid.cpp (50 Cent); addresses found by structure in Bourne:
//
//   sub_82A51FA0  UGameViewportClient::Draw candidate (calls the "ScreenShot"/"MovieFrame" helper sub_82A2FE10 and
//                 BeginRenderingViewFamily; returns at 0x82A52180). Three more callers: 0x82A30378, 0x82A306B8,
//                 0x82A30BAC (sub_82A30398 / sub_82A30250 / sub_82A30928; never seen in gameplay).
//   sub_82E63C88  BeginRenderingViewFamily (game thread): builds the FSceneRenderer (ctor sub_82E61DB0) and enqueues
//                 FDrawSceneCommand (vtable 0x821F46E4: dtor sub_82A5C9A0, Execute sub_82E63C58, DescribeCommand
//                 sub_82E304A0 -> L"FDrawSceneCommand" at 0x821F46F0), or FDrawSceneCommandPP (vtable 0x821F4714,
//                 Execute sub_82E63B00). GIsThreadedRendering 0x8365CE08; without it, runs inline (0x82E63E00).
//   sub_82E63B80  RenderViewFamily_RenderThread(FSceneRenderer*): show flags at renderer +0x18 (64-bit); bit 51 set
//                 skips the scene, bit 23 runs sub_82E4C180 first, then
//   sub_82E639B0  FSceneRenderer::Render (returns to 0x82E63C08 from here; scene captures also call it from
//                 0x82CAD180/550/B48, 0x82CAE220 - those are never suppressed). Then ~FSceneRenderer sub_82E62280 and
//                 GMalloc (0x835A8190) ->Free.
// D3D device 0x8359F774 (msaa.cpp); its ring make-room function is sub_82305DE0 (dev +0x30 write ptr, +0x38 limit).
// NtOpenFile / NtCreateFile import thunks: 0x833F612C / 0x833F613C (for n1's package preload, not hooked yet).
//
// Fill (--hybrid_fill, with suppression): at the skipped scene's place in the GPU stream the game writes a marker (a
// type-0 write of the frame's camera seq to register 0x0030); the native renderer plugin finds the shared state
// (BOTS_HYBRID_STATE, hybrid_state.h) and draws there: 1 a flat test colour, 2 a sky gradient and ground grid through
// the frame's camera. The camera comes from the view family's first FSceneView; its layout is found on the first
// frame (logged as "hybrid: FSceneView ...").
#include "generated/default/tbc_pch.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "hybrid_state.h"
#include "scene_feed.h"

REXCVAR_DEFINE_INT32(hybrid_suppress, 0, "Debug",
                     "Development (n1): 1 = skip the game's 3D scene rendering (FSceneRenderer::Render); HUD, menus "
                     "and movies still draw");
REXCVAR_DEFINE_INT32(hybrid_ab, 0, "Debug",
                     "Development (n1): toggle --hybrid_suppress on and off every N seconds (A/B timing in one run)");
REXCVAR_DEFINE_INT32(hybrid_report, 0, "Debug",
                     "Development (n1): log scene timings every N seconds (0 = off unless suppress/ab is set: 10)");
REXCVAR_DEFINE_INT32(hybrid_fill, 0, "Debug",
                     "Development (n1): what the native renderer draws where the scene was skipped: 0 nothing, 1 a "
                     "test colour, 2 a test grid through the game's camera, 4 the n1 renderer (needs the plugin built "
                     "with n1; BOTS_N1_COOKED names the CookedXenon folder)");

REX_EXTERN(__imp__sub_82E63C88);
REX_EXTERN(__imp__sub_82E63B80);
REX_EXTERN(__imp__sub_82E639B0);
REX_EXTERN(sub_82305DE0);  // D3D: make room in the device's ring, returns the write pointer

namespace {
using Clock = std::chrono::steady_clock;
using bots::HybridCamera;
using bots::HybridState;

// The one HybridState, published for the renderer plugin before main (hybrid_state.h).
HybridState g_state;
const bool g_state_published = [] {
  char value[32];
  std::snprintf(value, sizeof(value), "%llx", static_cast<unsigned long long>(uintptr_t(&g_state)));
#ifdef _WIN32
  SetEnvironmentVariableA(bots::kHybridStateEnv, value);
#else
  setenv(bots::kHybridStateEnv, value, 1);
#endif
  return true;
}();

constexpr uint32_t kDevice = 0x8359F774;
constexpr uint32_t kRenderWidth = 0x8342402C;  // ultrawide.cpp: the width the game renders at (height 720)
constexpr uint32_t kRenderFromRenderThread = 0x82E63C08;  // return address of Render's call in sub_82E63B80

uint8_t* Raw(uint8_t* base, uint32_t a) { return base + (uint64_t(a) + (a >= 0xE0000000u ? 0x1000u : 0u)); }
uint32_t U32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return __builtin_bswap32(v);
}
float F32(uint8_t* base, uint32_t a) {
  const uint32_t v = U32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void Store(uint8_t* base, uint32_t a, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(Raw(base, a), &value, 4);
}
bool Readable(uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u && !(a & 3); }

// ---- timing ----
struct Stats {
  uint64_t families = 0, by_site[5] = {};
  uint64_t rt_frames = 0, renders = 0, suppressed = 0, captures = 0, markers = 0, cameras = 0;
  double rt_ms = 0, render_ms = 0, gt_ms = 0;
};
std::mutex g_mutex;
Stats g_stats;
Clock::time_point g_start, g_report;
std::atomic<bool> g_started{false};

bool Enabled() {
  return REXCVAR_GET(hybrid_suppress) || REXCVAR_GET(hybrid_ab) || REXCVAR_GET(hybrid_report) ||
         REXCVAR_GET(hybrid_fill);
}
int ReportSeconds() {
  const int r = REXCVAR_GET(hybrid_report);
  return r > 0 ? r : 10;
}
bool Suppressing(Clock::time_point now) {
  const int ab = REXCVAR_GET(hybrid_ab);
  if (ab > 0) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - g_start).count();
    return (secs / ab) % 2 == 1;  // first period draws normally
  }
  return REXCVAR_GET(hybrid_suppress) != 0;
}
double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

void MaybeReport(Clock::time_point now) {
  std::lock_guard lock(g_mutex);
  if (!g_started.exchange(true)) g_start = g_report = now;
  const double secs = std::chrono::duration<double>(now - g_report).count();
  if (secs < ReportSeconds()) return;
  const Stats& s = g_stats;
  REXLOG_INFO(
      "hybrid: {:.1f} s, suppress {}, fill {}: {:.1f} fps (view families), game thread BeginRenderingViewFamily "
      "{:.2f} ms, render thread {:.2f} ms/frame, Render {:.2f} ms ({} run, {} skipped, {} captures); cameras {}, "
      "markers {} written {} seen {} filled; n1 ready {}; sites {}/{}/{}/{}/{}",
      secs, s.suppressed > s.renders ? 1 : 0, REXCVAR_GET(hybrid_fill), double(s.families) / secs,
      s.families ? s.gt_ms / double(s.families) : 0.0, s.rt_frames ? s.rt_ms / double(s.rt_frames) : 0.0,
      s.renders ? s.render_ms / double(s.renders) : 0.0, s.renders, s.suppressed, s.captures, s.cameras, s.markers,
      g_state.markers_seen.exchange(0), g_state.fills_drawn.exchange(0), g_state.n1_ready.load(), s.by_site[0], s.by_site[1], s.by_site[2],
      s.by_site[3], s.by_site[4]);
  g_stats = {};
  g_report = now;
}

// ---- camera (FSceneView) ----
void Mul(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = s;
    }
}
bool Invert(const float* m, float* out) {  // Gauss-Jordan, double precision
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (std::fabs(a[p][c]) < 1e-30) return false;
    if (p != c)
      for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
    const double d = a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = a[r][c];
        for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = float(a[r][c + 4]);
  return true;
}
void ReadMatrix(uint8_t* base, uint32_t at, float* m) {
  for (int i = 0; i < 16; ++i) m[i] = F32(base, at + 4 * i);
}
// A UE3 perspective projection (row vectors, z' = w).
bool LooksLikeProjection(const float* m) {
  return m[0] > 0.1f && m[0] < 20 && m[5] > 0.1f && m[5] < 40 && std::fabs(m[11] - 1) < 1e-4f &&
         std::fabs(m[15]) < 1e-4f && std::fabs(m[1]) < 1e-4f && std::fabs(m[4]) < 1e-4f;
}
// A view matrix: rotation rows orthonormal, last column (0, 0, 0, 1).
bool LooksLikeView(const float* m) {
  if (std::fabs(m[3]) > 1e-4f || std::fabs(m[7]) > 1e-4f || std::fabs(m[11]) > 1e-4f || std::fabs(m[15] - 1) > 1e-4f)
    return false;
  for (int r = 0; r < 3; ++r) {
    const float n = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
    if (std::fabs(n - 1) > 1e-3f) return false;
  }
  return true;
}

// FSceneView offsets, found on the first view (50 Cent: rect +0x24, view +0x50, projection +0x90).
struct ViewLayout {
  bool tried = false, ok = false;
  uint32_t proj = 0, rect = 0;  // view matrix = proj - 0x40; rect 0: use the render width x 720
};
ViewLayout g_layout;

void FindLayout(uint8_t* base, uint32_t view) {
  g_layout.tried = true;
  float m[16], v[16];
  for (uint32_t o = 0x40; o <= 0x300 && !g_layout.ok; o += 4) {
    ReadMatrix(base, view + o, m);
    if (!LooksLikeProjection(m)) continue;
    ReadMatrix(base, view + o - 0x40, v);
    if (LooksLikeView(v)) g_layout.ok = true, g_layout.proj = o;
  }
  // The view rectangle: X, Y, SizeX, SizeY floats somewhere before the view matrix.
  if (g_layout.ok)
    for (uint32_t o = 0x08; o + 16 <= g_layout.proj - 0x40; o += 4) {
      const float x = F32(base, view + o), y = F32(base, view + o + 4), w = F32(base, view + o + 8),
                  h = F32(base, view + o + 12);
      if (x >= 0 && x < 2000 && y >= 0 && y < 2000 && w >= 320 && w <= 4096 && h >= 200 && h <= 4096 &&
          w == std::floor(w) && h == std::floor(h)) {
        g_layout.rect = o;
        break;
      }
    }
  REXLOG_INFO("hybrid: FSceneView {:08X}: {} projection +{:X}, view matrix +{:X}, rect +{:X}", view,
              g_layout.ok ? "found" : "NOT found", g_layout.proj, g_layout.proj ? g_layout.proj - 0x40 : 0,
              g_layout.rect);
  for (uint32_t o = 0; o < 0x200; o += 16)
    REXLOG_INFO("hybrid:   view +{:03X}: {:08X} {:08X} {:08X} {:08X} | {:.5g} {:.5g} {:.5g} {:.5g}", o,
                U32(base, view + o), U32(base, view + o + 4), U32(base, view + o + 8), U32(base, view + o + 12),
                F32(base, view + o), F32(base, view + o + 4), F32(base, view + o + 8), F32(base, view + o + 12));
}

bool ReadCamera(uint8_t* base, uint32_t family, uint32_t view, HybridCamera& cam) {
  if (!g_layout.tried) FindLayout(base, view);
  if (!g_layout.ok) return false;
  ReadMatrix(base, view + g_layout.proj - 0x40, cam.view);
  ReadMatrix(base, view + g_layout.proj, cam.proj);
  if (!LooksLikeProjection(cam.proj)) return false;
  Mul(cam.view, cam.proj, cam.view_proj);
  if (!Invert(cam.view_proj, cam.inv_view_proj)) return false;
  if (g_layout.rect) {
    for (int i = 0; i < 4; ++i) cam.rect[i] = F32(base, view + g_layout.rect + 4 * i);
  } else {
    const uint32_t w = U32(base, kRenderWidth);
    cam.rect[0] = cam.rect[1] = 0, cam.rect[2] = float(w >= 640 && w <= 4096 ? w : 1280), cam.rect[3] = 720;
  }
  const float* v = cam.view;  // View = Translation(-L) * R: L = -row3 * R^T
  for (int i = 0; i < 3; ++i) cam.location[i] = -(v[12] * v[i * 4] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
  cam.fov = float(2 * std::atan(1.0 / cam.proj[0]) * 180 / 3.14159265358979323846);
  cam.time = F32(base, family + 0x1C);
  return true;
}

// Seqs of view families with a camera, not yet rendered (game thread -> render thread).
std::mutex g_fifo_mutex;
std::deque<uint32_t> g_fifo;
thread_local uint32_t t_render_seq = 0;

// A type-0 register write of `value` to the marker register, appended to the D3D device's ring (as the runtime's own
// inline packet writers do).
void EmitMarker(PPCContext& ctx, uint8_t* base, uint32_t value) {
  const uint32_t dev = U32(base, kDevice);
  if (!Readable(dev)) return;
  uint32_t ring = U32(base, dev + 0x30);
  if (ring > U32(base, dev + 0x38)) {
    PPCContext saved = ctx;
    ctx.r3.u64 = dev;
    sub_82305DE0(ctx, base);
    ring = ctx.r3.u32;
    ctx = saved;
  }
  Store(base, ring + 4, bots::kHybridMarkerRegister);  // type 0, one register
  Store(base, ring + 8, value);
  Store(base, dev + 0x30, ring + 8);
}
}  // namespace

// BeginRenderingViewFamily(FCanvas* r3, FSceneViewFamily* r4), game thread.
extern "C" REX_FUNC(sub_82E63C88) {
  if (!Enabled()) return __imp__sub_82E63C88(ctx, base);
  const uint32_t lr = uint32_t(ctx.lr);
  const int site = lr == 0x82A52180 ? 0 : lr == 0x82A30378 ? 1 : lr == 0x82A306B8 ? 2 : lr == 0x82A30BAC ? 3 : 4;
  g_state.fill.store(uint32_t(REXCVAR_GET(hybrid_fill)), std::memory_order_relaxed);
  bool camera = false;
  if (REXCVAR_GET(hybrid_fill)) {
    // FSceneViewFamily: +0x00 Views (data, count), +0x10 Scene, +0x1C CurrentWorldTime (50 Cent's layout)
    const uint32_t family = ctx.r4.u32, views = U32(base, family), count = U32(base, family + 4),
                   scene = U32(base, family + 0x10);
    if (scene && count && Readable(views) && Readable(U32(base, views))) {
      const uint32_t seq = g_state.published.load(std::memory_order_relaxed) + 1;
      HybridCamera& cam = g_state.cameras[seq % HybridState::kCameras];
      if (ReadCamera(base, family, U32(base, views), cam)) {
        cam.seq = seq;
        g_state.published.store(seq, std::memory_order_release);
        camera = true;
        {
          std::lock_guard lock(g_fifo_mutex);
          g_fifo.push_back(seq);
          if (g_fifo.size() > 16) g_fifo.pop_front();
        }
        // The frame's scene, published before the render command that draws it exists (as skullburglar does), so
        // the render thread can't reach this camera's marker first and draw the previous frame's scene.
        if (REXCVAR_GET(hybrid_fill) == 4) tbc::scenefeed::Live(base);
      }
    }
  }
  const auto t0 = Clock::now();
  __imp__sub_82E63C88(ctx, base);
  const auto t1 = Clock::now();
  {
    std::lock_guard lock(g_mutex);
    ++g_stats.families, ++g_stats.by_site[site];
    g_stats.cameras += camera;
    g_stats.gt_ms += Ms(t0, t1);
  }
  MaybeReport(t1);
}

// RenderViewFamily_RenderThread(FSceneRenderer*), render thread.
extern "C" REX_FUNC(sub_82E63B80) {
  if (!Enabled()) return __imp__sub_82E63B80(ctx, base);
  {
    std::lock_guard lock(g_fifo_mutex);
    t_render_seq = g_fifo.empty() ? 0 : g_fifo.front();
    if (!g_fifo.empty()) g_fifo.pop_front();
  }
  const auto t0 = Clock::now();
  __imp__sub_82E63B80(ctx, base);
  const auto t1 = Clock::now();
  std::lock_guard lock(g_mutex);
  ++g_stats.rt_frames;
  g_stats.rt_ms += Ms(t0, t1);
}

// FSceneRenderer::Render(), render thread (and scene captures).
extern "C" REX_FUNC(sub_82E639B0) {
  if (!Enabled()) return __imp__sub_82E639B0(ctx, base);
  const bool main_view = uint32_t(ctx.lr) == kRenderFromRenderThread;
  const auto t0 = Clock::now();
  // Fill 4: until n1 has built its shaders and pipelines (its startup step), the game draws its own scene.
  const bool n1_waiting = REXCVAR_GET(hybrid_fill) == 4 && !g_state.n1_ready.load(std::memory_order_acquire);
  if (main_view && !n1_waiting && g_started.load(std::memory_order_relaxed) && Suppressing(t0)) {
    const bool marker = REXCVAR_GET(hybrid_fill) != 0 && t_render_seq != 0;
    if (marker) EmitMarker(ctx, base, t_render_seq);
    std::lock_guard lock(g_mutex);
    ++g_stats.suppressed;
    g_stats.markers += marker;
    return;
  }
  __imp__sub_82E639B0(ctx, base);
  const auto t1 = Clock::now();
  std::lock_guard lock(g_mutex);
  if (main_view) ++g_stats.renders, g_stats.render_ms += Ms(t0, t1);
  else ++g_stats.captures;
}
