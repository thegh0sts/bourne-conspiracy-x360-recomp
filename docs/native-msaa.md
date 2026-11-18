# Real MSAA in the native renderer: plan

Status: preparation done (both traces and the GPU's capabilities are in); implementation for 0.3.4. Nothing here changes rendering yet. The only code so far is
`native-plugin/patches/0011-msaa-support-log.patch`, which logs what MSAA the GPU supports
(`gfx native: MSAA support: ...`).

## Why

The native renderer draws everything at one sample per pixel (`CreateImage`: `VK_SAMPLE_COUNT_1_BIT`).
The only ways to smooth edges are a higher render scale (supersampling: every pixel shaded N times)
and FXAA (patch 0010: blurs anything with contrast, the HUD included). Hardware MSAA tests coverage
and depth at 2, 4 or 8 points per pixel but shades each pixel once, so it smooths geometry edges
for much less than supersampling, and leaves textures, text and the HUD alone.

This is not the game's own 2x MSAA (`src/msaa.cpp`, off by default). That one draws the scene in
two strips (predicated tiling), because 720p with 2x MSAA doesn't fit in the 360's 10 MB of EDRAM.
The game keeps running at one sample per pixel and never knows about the host's MSAA. Nothing about
tiling comes back.

## What a frame looks like

From a native-renderer trace (`BOTS_DRAW_TRACE=1`, `tbc-test/trace_tail.txt`, an ultrawide
1720x720 run on 2026-11-14; 16:9 is the same with 1280). The same EDRAM is reused all frame:
colour surfaces at base `0x3DE` in several formats, depth at base `0`. `transfer:` lines are the
renderer moving a surface's contents between formats at the same base (`Own()`).

| Step | Draws | Target | Depth | Then |
|---|---|---|---|---|
| Bloom / luminance downsamples | 6 | 480x800 fmt 5 | none | 3 small resolves |
| Full-screen pass | 25 | base 3DE fmt 0 | none | resolve to `1F4B6000` |
| Depth clear + **z-prepass** | 2 + 90 | (colour masked) | base 0, write, GEQUAL | **depth resolve** to `19BD6000` |
| Occlusion queries (bounding boxes) | 61 | (colour masked) | base 0, test only | |
| Shadow map | 8 | 880x880 | base 5A0 | resolve 512x512 |
| Screen-space shadow mask (stencil) | ~8 | base 3DE fmt 0 | base 0, stencil | 3 resolves to `1A0B0000` |
| Depth clear (`src/msaa.cpp`'s ClearZ) + **main colour pass** | 1 + 90 | base 3DE fmt 3 (HDR) | base 0, write, GEQUAL | **colour resolve** to `19210000`, **depth resolve** to `19BD6000` |
| Post (DOF etc.) | ~14 | base 3DE fmt 10 / A / 2 | some test depth | resolves to `19210000`, `18D36000` |
| **Translucents** (particles, glass) | 28 | base 3DE fmt C (HDR) | base 0, test only | colour resolve to `19210000` |

What this means for MSAA:

- The edges worth anti-aliasing are drawn in three places: the z-prepass, the main colour pass and
  the translucents. All of them use **colour base 3DE and depth base 0, full-screen pitch**.
- **The main pass doesn't depend on the z-prepass's depth.** `src/msaa.cpp` clears depth right
  before it, and the main pass writes depth itself (GEQUAL, write on). So the depth surface can
  go through a resolve and back between the two without holes at edges.
- The game reads the scene back only through resolves (`Renderer::Resolve`): scene colour,
  scene depth (twice), the shadow mask. Each one is a point where the multisampled image has to
  be resolved to one sample per pixel first.
- Six or so `transfer:` hops per frame move base 3DE between formats (0 → 3, depth → 3 for the
  XDK's colour clears, 3 → 2, 2 → 3). Those need the multisampled contents too.
- **Occlusion queries count samples.** The renderer passes the GPU's counts to the game as they
  are (`occlusion.cpp`, `ResolveOcclusion`). With 4x MSAA they come out 4x, so they should be
  divided by the sample count, to keep the numbers the game's visibility checks see the same
  as without MSAA. (The render scale already inflates them by its area today, and the game
  copes, so this is about keeping behaviour unchanged, not a known bug.)

## 16:9 trace (2026-11-18)

`BOTS_DRAW_TRACE=1` at 1280x720, render scale 1, standing in a mission: 41 frames, about 920 draws,
17 resolves and 7 transfers each. Same structure as the ultrawide trace, with these corrections:

| Step | Draws | Target (pitch) | Depth | Then |
|---|---|---|---|---|
| Depth clear + z-prepass | 1 + ~297 | (colour masked) | base 0 (1280), write, GEQUAL | depth resolve |
| **Occlusion queries** | 177 | (colour masked) | **base 388, pitch 640** (half-resolution depth), test only | |
| Shadow maps (3 cascades) | ~20 | 880x880 | base 5A0 | 3 depth resolves |
| **Shadow mask** | ~10 | base 2D0 fmt 0, **pitch 640** | base 388 (640), stencil | 3 resolves 640x360 |
| Stencil clear, `src/msaa.cpp`'s ClearZ, colour clear (depth trick) | 4 | base 2D0 fmt 3 (1280) | base 0 | transfer depth -> fmt 3 |
| **Main colour pass** | 298 | base 2D0 fmt 3 (1280) | base 0, write, GEQUAL | colour + depth resolves |
| Distortion / decal buffer | 1 + 34 | base 2D0 fmt 2 / A (1280) | base 0, test only | resolve |
| Composite, **translucents** | 1 + 38 | base 2D0 fmt 3 / C (1280) | base 0, test only | colour resolve |
| Bloom | 5 | 400x960 fmt 5 | none | 3 resolves |
| Tone map + HUD | 25 | base 2D0 fmt 0 (1280) | none | resolve to the front buffer |

- **The colour base moves with the resolution** (`0x2D0` at 1280x720: depth takes the first 720
  tiles, 1280x720 / (80x16)). Surfaces are picked by pitch (the screen width), not by base.
- **Occlusion queries and the shadow mask run at half resolution** (pitch 640) with their own
  depth buffer. Multisampling only full-width surfaces leaves both alone: no occlusion count
  scaling, and no stencil work inside the multisampled depth.
- **Full-resolution stencil is only ever cleared** (the quarter-size clears, ClearZ); no full-width
  draw tests or writes it. A clear can clear the multisampled twin directly, so stencil never has
  to be copied up, and the GPU's lack of shader stencil export (RTX 4080 SUPER:
  `shader stencil export no`) doesn't matter.
- The z-prepass's depth is resolved for the shadow mask, then cleared (ClearZ) before the main
  pass, which writes its own depth. It doesn't need multisampling itself; it gets it anyway because
  it draws into the same depth surface, at the cost of some depth bandwidth.
- Edges come from three runs of draws: the main pass (fmt 3), the decal buffer (fmt A) and the
  translucents (fmt C). Everything else at full width is full-screen quads, which multisample
  correctly but gain nothing.

GPU (from 0011's log line on the RTX 4080 SUPER): colour and depth 2/4/8 samples, sampling them
2/4/8, per-sample shading yes, shader stencil export no.

## Design

**A multisampled twin per surface.** `Image` gets an optional multisampled companion (same format,
N samples) and a note of which of the two holds the newest contents.

- **Draws** to an MSAA-enabled surface render into the twin. If the one-sample image is newer
  (a transfer, a resolve's clear, an upload), it is first copied up into the twin: a full-screen
  pass writing every sample, depth through `gl_FragDepth`.
- **Every other access** (resolve copies, transfers, `Own()`, sampling as a texture, readbacks)
  uses the one-sample image. If the twin is newer, it is resolved down first: colour with
  `vkCmdResolveImage` (average), depth and stencil through a dynamic-rendering resolve
  (`VK_RESOLVE_MODE_SAMPLE_ZERO_BIT`, core in Vulkan 1.2).
- **Clears** done at a resolve (`copy.color_clear_enable` / `depth_clear_enable`) clear both.

This keeps every existing path (resolve, transfer, texture cache) working on one-sample images,
as now. MSAA only lives between a run of draws and the next time anything reads the result.

**Which surfaces.** Version 1: surfaces with the scene's pitch (the full-screen 1280 / 1720 /
render-scaled width), colour and depth. That covers base 3DE in every format and depth base 0, and
leaves shadow maps (880) and bloom (480) alone. Post passes into base 3DE then run multisampled
too, which is correct but wastes bandwidth; narrowing it to the geometry formats (3, C and depth)
is a later optimisation once it works.

**Pipelines.** `PipelineKey` gets the sample count (`rasterizationSamples`). To keep the pipeline
lists people have built (`Documents\tbc\cache\native\*.manifest.bin`, and the master list we plan
to ship), the count goes into spare bits of an existing field: a new field changes the record size,
and a manifest with another record size **starts over**. The warm-up list then needs the MSAA
variants recorded too, so the master list should be made with MSAA on, or include both.

**Alpha to coverage.** Xenos's alpha-to-mask is emulated in the shader now (`sc.alpha_to_mask`).
With MSAA it can map to `alphaToCoverageEnable`, which smooths foliage and fences properly. Later step.

**Settings.** `BOTS_GFX_MSAA=2|4|8` (off when unset), from a launcher **Anti-aliasing** entry
(MSAA 2x / 4x, possibly combined with FXAA). Clamped to what the GPU supports.

## Steps

Each step builds, runs and is checked before the next.

1. **Capability log** (done: patch 0011). One run gives the GPU's sample counts, per-sample
   shading and shader stencil export.
2. **Fresh 16:9 trace** to confirm the table above at 1280x720 and render scale 1.5:
   `BOTS_DRAW_TRACE=1` and `BOTS_TRACE_RENDERING=1`, about 10 seconds in a mission, stderr to a file.
3. **Twin images and the two copies** (resolve down, copy up), with the setting off by default.
   Test: MSAA on, render scale 1, a static scene: no change apart from smoother edges.
4. **Pipelines**: sample count in the key, manifest kept compatible.
5. **Occlusion counts** divided by the sample count.
6. **Depth and stencil**: resolve with sample zero, copy up (see risks).
7. **Launcher option**, then a full mission at 1x, 1.5x and 2x, comparing frame time with
   render scale and FXAA.
8. Optional: alpha to coverage; MSAA only for the geometry formats.

## Risks and open questions

- **Copying stencil back up.** Copying a resolved depth/stencil into the multisampled twin needs
  stencil written from a shader (`VK_EXT_shader_stencil_export`). Step 1's log says whether the
  GPU has it. Without it: eight passes, one per stencil bit, or keep stencil-only work
  (the shadow mask) away from the twin. The shadow mask passes use stencil, so this matters.
- **The number of copies up.** About six transfers a frame each force a full-screen copy into the
  twin. At 1440p and 4x that's around 0.2 ms each on an RTX 4080 class card. Skip the copy when the
  next draw overwrites the whole surface (the XDK's clears do).
- **Depth resolved at sample zero** is what DOF, fog and the shadow mask then read. It matches what
  the game saw with its own MSAA, so it should be fine, but an edge may show a one-pixel halo in DOF.
- **Render scale and MSAA together** multiply memory: 2560x1440 at 4x is about 135 MB of colour
  and depth. Fine on the cards that can run render scale 2.
- **Transfers between formats** (`Transfer()`) stay on one-sample images in this design. If a
  sequence like "format 0 → format 3 → draw" loses edges because the copy up happens after the
  edges were drawn, those hops would need a per-sample transfer (sample-rate shading).
