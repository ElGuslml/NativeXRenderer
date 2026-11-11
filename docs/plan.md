# NativeXRenderer — work plan

First game: Pirates of the Caribbean: At World's End (POTCAWE), in `POTCAWERecompVulkan`. Started
2026-10-13. Estimates are in working days, at the pace of the POTCAWERecomp work so far. Tick the boxes as
tasks are done, and add the date and a pointer to the notes that close each one.

| Phase | Goal | Status |
|---|---|---|
| 0 | Scaffolding: project, build, documents, baseline | done (2026-10-13) |
| 1 | Map the game's D3D layer and verify that every draw can be rebuilt from it | done (2026-10-13): 100 % of the draws rebuilt; go (D14) |
| 2 | Core renderer: first frames drawn by NativeXRenderer | done (2026-10-14) |
| 3 | Performance and the renderer's own features | done (2026-10-14) |
| 4 | POTCAWE polish | closed (2026-10-15, D57): the default renderer there (D56); open checks in "Later" |
| 5 | Temporal upscaling and frame generation | in progress: DLAA and the DLSS modes in POTCAWE; FSR 3 and frame generation not started |
| 6 | Second game (GRAW), extraction, third game (Kameo) | closed (2026-10-21): NativeXRenderer is the default renderer of the three games; GRAW's DLAA and Kameo's further levels in "Later" |
| 7 | More games: the infrastructure to keep several in step, then the next game | in progress: the infrastructure done (the SDK's changes in one branch, D89; recomp-tools: a test of every game, a project template); next, the game |

The first plan had five phases with estimates (scaffolding, mapping, the core renderer, POTCAWE's parity, the second
game); they were split as the work went. The sections below keep each phase's items and dates.

The phases are sequential, and each one ends with an explicit decision. Phase 1 is cheap and answers the
questions that decide whether Phases 2–3 are worth their cost. Stop there if the answers are bad: for
example, if the engine writes most of its GPU commands without going through the D3D library.

---

## Phase 0 — Scaffolding

- [x] Project created from POTCAWERecomp `rt-to-texture` at `c6be8a3` (SDK patches 0001–0020), with its
      history. The remote `potcawe` points to the local POTCAWERecomp and is fetch-only (2026-10-13).
- [x] Game files copied into `assets/` (26 files, 6,835,718,678 bytes, identical to the source).
- [x] Personal tools copied into `tools/` (gitignored), with their hardcoded paths repointed to this
      repository.
- [x] Baseline build (RelWithDebInfo) passes, unchanged from POTCAWERecomp.
- [x] Documents: this plan, [`architecture.md`](architecture.md), [`xdk-d3d-reference.md`](xdk-d3d-reference.md),
      [`integrating-a-game.md`](integrating-a-game.md), [`decisions.md`](decisions.md),
      [`credits.md`](credits.md), and the game side
      [`../../docs/native-x-renderer-potcawe.md`](../../docs/native-x-renderer-potcawe.md).
- [x] Library skeleton: `native_x_renderer/CMakeLists.txt` (static library `native_x_renderer`, linked into
      `potcawerecomp.exe`, no behavior yet).
- [x] Gustavo: one run of the baseline build to confirm it matches POTCAWERecomp (title screen, main menu,
      one minute at the Port Royal dock). "Runs perfectly" (2026-10-13, logs `potcawe_001`–`002`).
- [x] Baseline re-measured with this build (see [Baseline to beat](#baseline-to-beat)): `tools/gpu_ab.ps1`
      at the Port Royal dock. Configurations: 2×2 and 3×3, with Antialiasing On and Off, at 30 and 60 FPS,
      plus one repeat (2026-10-13, `local_docs/01-baseline.md`).

**Phase 0 done (2026-10-13).** The DiligentCore submodule is part of Phase 2, step 1, because Phase 1 does
not need it.

## Phase 1 — Map the D3D layer and observe

Goal: know every XDK D3D entry point the game uses, and prove that the full GPU state of every draw can be
rebuilt from the D3D device structure at the moment the draw is called. No rendering yet.

Started and finished on 2026-10-13. Notes: `local_docs/02-d3d-map.md` (static mapping),
`local_docs/03-trace.md` (trace runs), `local_docs/04-observer.md` (observer runs). Results:
[`native-x-renderer-potcawe.md`](../../docs/native-x-renderer-potcawe.md).

1. **Entry points.** Identify the functions listed in
   [`xdk-d3d-reference.md`](xdk-d3d-reference.md#entry-points). Methods are in
   [`xdk-d3d-reference.md`](xdk-d3d-reference.md#finding-the-entry-points-in-a-new-executable).
   - [x] Draws: DrawIndexedVertices `824AC3C0`, DrawVertices `824ABFD8`, BeginVertices `824ABAD8`,
         DrawVerticesUP `824ABF78`. No separate EndVertices, no DrawIndexedVerticesUP. Found by their
         `DRAW_INDX` packets, away from the rest of the library (2026-10-13).
   - [x] Clear `822C6AA0`, ClearF `822C6978`, Resolve `822C2048`, BeginTiling `822BCC58`, EndTiling
         `822BD1E0`, SetPredication `822BCAE8`, SetRenderTarget `822B4F60`, SetDepthStencilSurface
         `822B52C8`; also SetViewport, SetScissorRect, and BeginZPass/EndZPass (likely, unused) (2026-10-13).
   - [x] SetVertexShader `822B7E18`, SetPixelShader `822B7B60`, SetVertexDeclaration `822B7FE8`,
         SetStreamSource `822B46A0`, SetIndices `822B47C0`, SetTexture `822AC668`, sampler setters,
         SetVertexShaderConstantF `822B7848`, SetPixelShaderConstantF `822B7950`. About 44 engine functions
         also write constants inline. No constant buffers: the only `LOAD_ALU_CONSTANT` is the program
         flush's. CreateVertexDeclaration is not needed: the flush reads the bound declaration (2026-10-13).
   - [x] The flush: inline in each draw, through helpers for constants, register groups, fetch constants
         and the program (`822B1090`) (2026-10-13).
   - [x] Command buffer: new segment `822AEC50` (with its fence). InsertFence and BlockOnFence are not
         used by the game (2026-10-13).
   - [x] Swap, Present, SetRenderState_PresentInterval, query Issue / GetData, the GPU wait helpers,
         XGSetSurfaceHeader (already found while working on POTCAWERecomp).
   - [x] Render state setters, by register: about 90, almost all called only by the renderer's init
         (`local_docs/02-d3d-map.md`). Not hooked: the shadow covers them (2026-10-13).
   - [x] Also found: Lock/Unlock thunks, the cache flush Unlock calls, SetShaderGPRAllocation `822B8120`
         and its prebuilt buffers (2026-10-13).
2. **Device layout** for XDK 2.0.5426. The offsets of:
   - [x] the register shadow (seven groups from `+10240`);
   - [x] the vertex and pixel float constants (`+1792`, `+5888`);
   - [x] the fetch constants (`+1024`);
   - [x] the bool/loop constants (`+9984`);
   - [x] the current shaders, vertex declaration, render targets and depth surface;
   - [x] the dirty masks (`+0`…`+40`, and their bit order).

   Written into the profile, `potcawe::NativeXRendererProfile()` in `src/native_x_renderer_potcawe.cpp`
   (2026-10-13). The layout is Dante's Inferno's shifted by −128 and −312 bytes in two blocks.
3. **Trace mode.** Hooks on every entry point, logging calls per frame (counts and arguments for one
   frame on a key press). Tracing is a feature of the library (cvar `native_x_renderer_trace`, off by default),
   because every new game goes through this step.
   - [x] Hook file `src/native_x_renderer_potcawe.cpp` (game side) with one line per entry point: 39 entry
         points (2026-10-13); 40 with `NewSegment` (D26, 2026-10-14).
   - [x] Traced frames of: language menu, title screen, main menu, Port Royal dock, with and without
         2× MSAA (logs `potcawe_012`, `013`, `020`, `021`; 2026-10-13).
4. **Observer.** At each draw, rebuild the draw's GPU state from the device structure and compare it with
   the register file the SDK's command processor has when it executes that draw. The comparison needs a
   generic draw callback in the SDK: a small patch, off and free when unset (see
   [`decisions.md`](decisions.md), D10 and D11).
   - [x] 100 % of the draws match over one frame of each scene (2026-10-13, logs `potcawe_019`–`021`).
         Hell's Gate reached 975/975 on Dante's Inferno.

         | Scene | Draws matching |
         |---|---|
         | Title screen (Bink background) | 15/15, 18/18 |
         | Main menu | 21/21 |
         | Port Royal dock, Antialiasing Off | 1123/1123 |
         | Port Royal dock, 2× MSAA (two tiles) | 1127/1127 (2148 executions) |

         The rebuild needs the call's own packets besides the shadow (decision D12). The shadow alone
         reached 92 %.
5. **Open questions.** Answer each with evidence:
   - [x] Predicated tiling. The CPU calls each draw once; per tile only the window offset, the scissor
         and the query address differ. POTCAWE resolves each tile itself under SetPredication.
   - [x] The 19 EDRAM clears per frame are the library's ClearF/Clear, drawn through a surface the library
         aliases itself.
   - [x] The engine does not write PM4 directly, run precompiled command buffers, or patch the command
         buffer: every draw packet comes from a hooked call, or from the library's prebuilt
         SetShaderGPRAllocation buffers.
   - [x] Memexport: no shader uses it in the verified scenes.
   - [x] CPU reads of resolve results: no sign (no texture lock per frame). A raw-pointer read is not
         excluded; Phase 3 watches for stale data.
   - [x] Videos: Bink draws through the same D3D calls (three `k_8` planes and a YUV pixel shader); the CPU
         writes the planes without Unlock.

**Exit:** all entry points mapped (or shown unused), the observer at 100 % in the three scenes, and the
open questions answered in [`native-x-renderer-potcawe.md`](../../docs/native-x-renderer-potcawe.md). Met on
2026-10-13.

**Decision: go or no-go for Phase 2.** Recommendation: **go**. Every draw is visible at the D3D layer, its
state can be rebuilt exactly, and the costs D1 targets are all there:
- the aliased clears are library calls a renderer can replace with host clears;
- the tiled scene is issued once by the CPU;
- the resolves are explicit calls with a destination texture.

**Decided: go** (Gustavo, 2026-10-13; [`decisions.md`](decisions.md), D14).

**What Phase 2 starts from:**
- the profile and the hooks (`src/native_x_renderer_potcawe.cpp`);
- the rebuild rules, which move from `src/observer.cpp` into the core;
- the observer, kept green as a regression test;
- two new items for the plan:
  - textures the CPU writes without Unlock (Bink): step 7;
  - skipping the SetShaderGPRAllocation draws in sync-only mode: step 3.

## Phase 2 — Core renderer

Goal: NativeXRenderer draws the title screen, the main menu and a Port Royal frame. The images do not need to
be perfect yet.

Started on 2026-10-13. Turned on with `--native_x_renderer=true`; the Xenos path stays the default.

State on 2026-10-13: every draw of the three scenes goes through NativeXRenderer (1,136 at the dock, none
failing) and the frame reaches the screen. Textures are placeholders, so the images show geometry, vertex
colors, fog and post-processing, but not the game's art yet. GPU time at the dock is 10.7 ms per frame at
1280×720 with Antialiasing Off (the options' default; corrected on 2026-10-14, this said 2× MSAA); the
Xenos path needs 8.5–9 ms at four times the pixels (2×2, AA Off; the baseline has no 1×1 run), so
NativeXRenderer is far from its goal there: optimization is Phase 3.

2026-10-14: the Vulkan device was being lost on the first dock frames (out-of-range buffer reads without
`robustBufferAccess`); fixed with NativeXRenderer's first DiligentCore patch (D22). 60 FPS at the dock, renderer
CPU 4.7 ms per frame, no stalls.

2026-10-14, step 7: the game's textures are loaded through the SDK's texture cache (D23). The language menu,
the title screen and its Bink background, the main menu and the Port Royal dock (HUD and character shadows
included) match the Xenos path. GPU time at the dock is 11.7–12.0 ms per frame, renderer CPU 5.4 ms.

1. **DiligentCore.**
   - [x] Submodule `thirdparty/diligent-core`, pinned to master `074060c` (2026-10-04). The last release,
         v2.5.6, is from 2024-09, and Hell's Gate builds master (2026-10-13).
   - [x] CMake: `DILIGENT_NO_GLSLANG` and `DILIGENT_NO_HLSL`, because their glslang/SPIRV targets collide
         with the SDK's. Only five of Diligent's submodules are needed; it reuses the SDK's Vulkan-Headers,
         SPIRV-Headers and xxHash targets (2026-10-13).
   - [x] No AVX2 build flags: `-mavx2` removed from `Diligent-BuildSettings` (2026-10-13).
   - [x] VSync off by default in the swap chain (`Present(0)`, 2026-10-13).
         The present mode an option since D106 (`native_x_renderer_present_mode`, DiligentCore patch 0009,
         2026-10-29): V-Sync without tearing (MAILBOX, else FIFO) or immediate; the launchers' V-Sync row.
         D107 (2026-10-30): V-Sync is FIFO, with the SDK's vblank_follows_display (NVIDIA's MAILBOX queued three
         frames: 16-19 ms at 160 Hz; FIFO and the display's vertical blank 4 ms).
   - [x] BC1: DiligentCore's `BC1_UNORM` is Vulkan's BC1 without alpha, so DXT1 is decompressed to RGBA8
         by the SDK's load shader (D23, 2026-10-14). Native BC1 with alpha would need a DiligentCore patch.

   These five points are the integration problems Hell's Gate documented. Credit them in the commit.
2. **SDK graphics code in process.** The SDK's GPU backend is a plugin DLL (`rexgpu-xenosrd.dll`), and its
   translator and texture code are not exported. Either compile the needed SDK graphics sources into
   `native_x_renderer` (Hell's Gate's choice), or export them through an SDK patch.
   - [x] Decided: compiled into `native_x_renderer_sdk`, the cvars left to the plugin (D15, 2026-10-13).
3. **Command processor sync-only mode** (SDK patch, see D10): packets are still executed for fences,
   interrupts, waits and queries, but nothing is drawn on the host.
   - [x] Selected by the same cvar as the renderer: `sdk_patches/0002`, `SetHostRendering(false)`
         (2026-10-13).
   - [x] The library's own draws (ClearF and Resolve rectangles, SetShaderGPRAllocation points) are
         skipped like the game's: every draw packet is (2026-10-13).
4. **Device and presentation.**
   - [x] A Diligent Vulkan device on the game window (2026-10-13).
   - [x] Decided: its own swap chain; the SDK's presenter is detached from the window, and its ImGui
         overlays are not shown on this path yet (2026-10-13).
5. **Shaders and pipelines.**
   - [x] The rebuild of D12 in the core (`src/state.cpp`): one register file plus the `IM_LOAD`ed
         microcode, shared with the observer, which stays at 100 % (2026-10-13).
   - [x] Every command buffer segment change followed (`NewSegment`), so no call's packets are lost or
         read from stale memory; the observer compares the programs too: 100 % of the draws at the dock,
         programs included (D26, 2026-10-14). This removed the stretched triangles at the dock.
   - [x] Xenos microcode → SPIR-V with the SDK's `SpirvShaderTranslator`, modifications as the SDK's Vulkan
         backend computes them (`src/shaders.cpp`, 2026-10-13).
   - [x] Fixed binding contract: shared memory, constants, textures and samplers, bound by name through
         DiligentCore's SPIR-V reflection (D18, `src/pipelines.cpp`, 2026-10-13).
   - [x] Pipeline cache keyed by the host shaders and the fixed-function state (2026-10-13).
   - [x] Pipelines created off the render thread, so new pipelines do not stall the frame (the lesson of
         `gpu_async_pipelines_no_frame_wait`): DiligentCore's asynchronous pipelines; a draw whose pipeline
         is compiling is skipped (D28, 2026-10-14). Shader translation stays on the render thread (at most
         2.7 ms per shader).
6. **Buffers.**
   - [x] Index and vertex data: the SDK's shared memory design, on a DiligentCore buffer (D17). Indices are
         used in place and swapped by the vertex shader; fans, quads, loops and reset indices are
         converted by the SDK's PrimitiveProcessor (2026-10-13).
   - [x] Dynamic buffers: their pages are uploaded again after a CPU write (page watches, D17), with or
         without Unlock (2026-10-13).
   - [x] Unchanged ranges skipped: by page validity, not by hash (2026-10-13).
7. **Textures.**
   - [x] Textures written by resolves bound by address (2026-10-13).
   - [x] Fetch constants → host textures: the SDK's `TextureCache` with DiligentCore textures and the SDK's
         load shaders as compute pipelines (untiling, endianness, formats, packed mips; D23, 2026-10-14).
   - [x] Invalidated on CPU writes, with or without Unlock (the Bink planes included): the guest memory
         mirror's page watches (D17). Outdated textures whose data hashes the same are not loaded again
         (D23, 2026-10-14).
   - [x] Swizzles and signs from the texture cache; a resolved texture repeats its last component as the
         guest does (one component: RRRR, two: RGGG) (2026-10-14).
   - [x] Resolves into stacked textures: one host slice per guest slice (POTCAWE's two shadow cascades,
         2026-10-14).
   - Moved to Phase 3 (none seen in POTCAWE so far): signed formats whose signed data differs
     (`IsSignedVersionSeparateForFormat`; the signed binding samples the unsigned data for now); 1_5_5_5 and
     DXT3A_AS_1_1_1_1 (no host format yet); resolves into 3D and cube textures (skipped and logged once).
8. **Render targets and resolves.**
   - [x] Host render targets keyed by the guest surface (EDRAM base, pitch, MSAA, format, size from the
         surface object), at scale 1 for now (2026-10-13).
   - [x] Keyed by the EDRAM storage format: the XDK rewrites a surface's format between its draws and its
         resolve (2_10_10_10_FLOAT_AS_16_16_16_16 and 2_10_10_10_FLOAT), and the two used to get separate
         targets, which lost POTCAWE's HUD (2026-10-14).
   - [x] Float24 depth in its guest range on the host, so sampled depth compares right (D24, 2026-10-14).
   - [x] Clears of whole targets (2026-10-13). Partial clears (a rectangle): none in POTCAWE's traced
         frames; skipped and logged once (2026-10-14), implemented when a scene needs them (Phase 3).
   - [x] A resolve registers its host texture at the destination address (and size and format: the same
         address is resolved at two sizes in one frame). Guest memory is not written (2026-10-13).
9. **Swap.**
   - [x] Present at Present: the newest texture resolved to the front buffer, letterboxed (2026-10-13).
   - [x] Read through the front buffer's fetch constant swizzle, which undoes the resolve's red/blue swap;
         the scaled blit's vertical orientation fixed (2026-10-14).
   - [x] Keep the existing 60 FPS hook (`src/framerate.cpp`) and the GPU wait hooks (`src/gpu_wait.cpp`)
         working: the SDK still runs the vblank and swap callbacks. At the dock: 16.67 ms per frame with the
         60 FPS option, 33.33 ms without (2026-10-14).

**Exit:** the three scenes rendered recognizably by NativeXRenderer, the fallback unaffected, and 10 minutes
without a crash. **Decision: continue to parity, or adjust the design.**

**Exit check, 2026-10-14:**
- the language menu, the title screen and the main menu (with their Bink backgrounds) and the Port Royal
  dock (HUD, character shadows) render as on the Xenos path; Gustavo played at the dock;
- the Xenos path is unchanged with `native_x_renderer` off (`potcawe_084`);
- 12 minutes at the dock, walking and turning the camera every minute (`potcawe_092`): no crash, no error
  in the log, every frame under 20 ms after the load;
- known differences, for Phase 3: the sun glare (fake occlusion query counts), AA forced off, 1280×720
  only, and the GPU time (12 ms facing the town against ~5 ms for the Xenos path; 4 ms facing the sea).

## Phase 3 — Performance and the renderer's own features

Goal: NativeXRenderer faster than the Xenos path at the same settings, with the features that change how it
renders: real occlusion queries, compressed textures, no translation or upload stalls, and any render
resolution.

Agreed order (Gustavo, 2026-10-14): performance first (D29). Then, after a viability analysis, the list
and order below, with POTCAWE's polish moved to Phase 4 and the second game to Phase 5 (D31). Notes:
`local_docs/06-phase3-performance.md`.

- [x] GPU time: find why the town side of the dock costs 12 ms on NativeXRenderer (4 ms facing the sea, ~5 ms
      for the Xenos path at the same settings). Every draw was in its own render pass; 3.3 ms now (D30,
      2026-10-14).
- [x] Occlusion queries (the sun flare): host queries between the guest's Issue calls, answered to the
      sync-only command processor through SDK patch 0003, at most two frames late. The glare matches the
      Xenos path (D32, 2026-10-14).
- [x] DXT1 textures uploaded as BC1, not decompressed: DiligentCore patch 0002 for the format with 1-bit
      alpha. Texture memory at the dock 276–318 → 128–139 MB, same image (D33, 2026-10-14).
- [x] Shader translation on worker threads; a draw waits by being skipped, as for pipelines. The analysis
      stays on the render thread (0.02 ms). At the dock's load the render thread no longer spends 75 ms
      translating, and its longest frame went from ~80 to 52–54 ms (D34, 2026-10-14).
- [x] Guest memory uploads. Measured first: 97.5 % of the pages uploaded again at the dock had not
      changed (invalidated by the engine's writes next to them). So pages are copied only when their data
      hash changed, instead of batching the copies (the planned step A). 7–9 copies per frame instead of
      ~145, GPU time 3.3 → 1.65 ms (D35, 2026-10-14). Step B is not needed for now.
- [x] Render resolution from the screen, at any scale, with no selector (D37, D38, 2026-10-14): the 16:9 rectangle that
      fits the monitor or the window, at least 1×; separate scales per axis, equal for now; shadow maps
      scaled like the rest.
  - [x] SDK patch 0005: the translator reverts a fractional scale (2026-10-14).
  - [x] Render targets, viewports, scissors, clears, resolves, resolved textures, point sizes, occlusion
        query counts and Present at the scale (D38, 2026-10-14).
  - [x] The scale from the output (the swap chain) and the profile's guest size; the
        `native_x_renderer_resolution_scale` override (D38).
  - [x] The launcher shows the resulting resolution with NativeXRenderer in the render quality row, disabled
        ("2560 × 1440 (from the screen)", or "(original)" at 1280×720), computed as the renderer does
        (2026-10-14).
  - [x] Checked at exactly 2× against the Xenos path at 2×2 (closer than at 1×), and at 1.067, 1.25, 1.4,
        1.5 and 1.6 (D38).
- [x] Performance A/B against the Xenos path with `tools/gpu_ab.ps1`, same spot, the baseline's
      configurations, both paths in one session (2026-10-14; [Measured on NativeXRenderer](#measured-on-nativexrenderer-phase-3)).
      At the same resolution without antialiasing NativeXRenderer uses 38–56 % less of the GPU; at 3× it holds
      60 FPS where the Xenos path with 2× MSAA drops to 33.

**Exit:** the items above done, NativeXRenderer's GPU time below the Xenos path's at the same resolution, and
no frame over 20 ms at the dock after the load.

Exit check (2026-10-14):
- every item above is done;
- the GPU time at the same resolution is 38–56 % lower than the Xenos path's;
- the longest frame at the dock after the load was 18.4–19.0 ms in every run (the A/B runs' frame
  intervals, the three-minute walk of D35).

**Closed on 2026-10-14 by Gustavo (D39).**

## Phase 4 — POTCAWE polish

Goal: play POTCAWE on NativeXRenderer with no visible difference from the Xenos path, everywhere (D31).

Started on 2026-10-14 (D39). First, in this order (D40): antialiasing, the pipeline cache, aspect ratios.
**Closed on 2026-10-15 (D57).** Its open checks moved to "Later".

- [x] Antialiasing of NativeXRenderer's own: SMAA (default) or FXAA, applied to the copy of the finished scene
      before the HUD is composited, which the game's profile names (`scene_resolve_caller`). The launcher's
      Antialiasing row offers SMAA / FXAA / Off with NativeXRenderer; the game's 2× MSAA and the tile hooks of
      `src/msaa.cpp` stay for the Xenos path (D41, 2026-10-14).
- [ ] Moved to "Later" (D57); the first levels were played on NativeXRenderer without a visible difference.
      Every level Gustavo can reach, compared with the Xenos path (screenshots at the same spots). What
      the dock already shows working (2026-10-14) is checked again on the way:
      - shadow maps (1024², pitch 1040), including the clears that alias them;
      - color and depth resolves;
      - full-screen passes, transparent objects, the bloom and luminance chain, the composite;
      - HUD, menus, loading screens, subtitles;
      - videos (Bink).
- [x] The display gamma ramp (found while checking D33). Present maps the front buffer through the
      guest's ramp, as the SDK's presenters do (SDK patch 0004). The tone curve now matches the Xenos path.
      Done early, during Phase 3, at Gustavo's request (D36, 2026-10-14).
- [ ] Moved to "Later" (D57). Known limits from Phase 2, when a scene needs them: partial clears, signed
      formats with separate data,
      1_5_5_5 and DXT3A_AS_1_1_1_1, resolves into 3D and cube textures.
  - [x] Resolves into cube textures: met in level 3, a cube map rendered every frame (D51, 2026-10-15).
- [x] A persistent pipeline cache, so a new run does not depend on the driver's cache. The shaders,
      translations and pipelines used are recorded per title and prepared at the next start, with
      DiligentCore's (Vulkan's) pipeline cache next to them: from the second run, no draw waits at the
      dock's load (D42, 2026-10-14). The Vulkan cache is saved on a thread of its own: its saves on the
      render thread were the 30–80 ms stutters Gustavo noticed (D45, 2026-10-14).
- [x] Long sessions: an hour of play with no growth in memory, shaders or pipelines. Done by Gustavo's
      fourth session (2026-10-15, 64 minutes, run 291): the memory in use stayed bounded, and shaders and
      pipelines grow only with new content.
  - Gustavo's first session (2026-10-15) found the journal (Back) showing the scene through its pages: the
    2_10_10_10_FLOAT range at resolves, fixed (D50). Also fixed: SMAA's DiligentCore message every frame.
  - His second session (54 minutes, levels 1 to 3) showed:
    - 60 FPS throughout, with hitches only at new content and video transitions;
    - a cube map resolve, now supported (D51);
    - render targets and resolved textures never freed, and the pipeline cache saved about 50 times: now
      freed after a minute unused, and saved at most every 5 minutes (D52);
    - device memory growing 1.42 → 2.16 GB over the session, more than those explain. Measured: the
      figure was the memory manager's pages. The use is the guest textures at the SDK cache's limit, the
      targets D52 now frees, the fixed buffers, and textures that idle pipelines kept alive: those now let
      go after 10 s, and the log shows the memory in use (D55). To confirm in another long session.
  - His third session (30 minutes, two levels, run 288), with D55's log:
    - memory flat within a level, 1.63–1.64 GB in use for the last 13 minutes in 1.84 GB of pages;
    - D52 freed about 200 MB at the level change.

    It ended when the game closed itself: a Lua script error, which the recompiled `longjmp` turned into
    `exit()`. That is a game fix, not the renderer's: `setjmp_address` / `longjmp_address`, made in
    POTCAWERecomp and merged (`docs/script-errors.md`). A fourth session should run past it.
  - His fourth session (64 minutes, three level ends, run 291) had no problem:
    - **Script errors:** the game caught four and went on. Each one would have closed it before the fix.
    - **Memory in use:** 1.64–1.77 GB for the last 50 minutes, with guest textures at the SDK cache's
      limit. Pages peaked at 1.92 GB; 281 were created and 247 freed.
    - **Freed by D52:** 19 render targets (208 MB) and 90 resolved textures (551 MB).
    - **Shaders and pipelines:** 381 → 478 and 656 → 861, all new content, kept for the next run.
    - **Pacing:** 60 FPS in 383 windows of 600 frames.
    - **Slow frames:** 22 windows have a frame above 25 ms:
      - at the start (401 ms);
      - at loads and videos;
      - with new pipelines;
      - with 400–950 texture loads as new areas stream in.

      Two, of 46 and 56 ms, have no visible cause in the log.
    - **Messages:** no warning from NativeXRenderer, and no DiligentCore message after the start.
- [x] Any aspect ratio without bars: ultrawide, 16:10, the Steam Deck. The renderer fills the output with
      a scale per axis, POTCAWE's hooks widen its 16:9 cameras (Hor+ or Vert+, culling included), and the
      2D layers (HUD, menus, videos) are drawn in a centered 16:9 box (D43, 2026-10-14). Checked at 21:9,
      16:10 and 16:9 in the menus, the dock and the pause menu. Not yet: other levels and the cutscenes.
      Anchoring HUD elements to the screen's edges moved to a later phase (Gustavo, 2026-10-15; see
      "Later").
- [x] Another aspect ratio than the screen's (`native_x_renderer_aspect`, D104, 2026-10-28): 21:9 on a 16:9
      screen or 32:9 on a 21:9 one, centered with bars, for a player whose screen is not the one they want to
      see (or to preview a wider one). The launchers' Format row offers 16:9, 21:9, 32:9 and the screen's.
      Checked in GH Metallica (menus and a song, 32:9 in a 1720×720 window and 21:9 in a 1280×720 one).
- [x] Anisotropic filtering: NativeXRenderer applies the SDK's `anisotropic_override` as the Xenos path does
      (it used only what the game asks for, plain trilinear on most of the dock). POTCAWE defaults to 16x
      on both paths, with a launcher row, at no measured cost (D44, 2026-10-14).
- [x] 60 FPS option (works with NativeXRenderer, 2026-10-14).
- [x] Launcher row "Renderer: Xenos (emulated) / NativeXRenderer (experimental)", added early in Phase 2 for
      testing (D25, 2026-10-14).
- [x] Launcher rows "Image format" (fill a wide screen, or 16:9 with bars) and "Internal resolution"
      (automatic, or a lower height in that format, in fullscreen and in a window) for NativeXRenderer; the
      DLSS modes out of the antialiasing row; Save keeps the window open (D53, D54, 2026-10-15).
- [x] NativeXRenderer is POTCAWE's default renderer. The Xenos path stays in the launcher, and as the fallback
      when NativeXRenderer cannot start (D56, 2026-10-15).

**Exit:** Gustavo plays the agreed levels without differences. **Decision: make NativeXRenderer the default
for POTCAWE, or keep it optional.** Decided on 2026-10-15: the default (D56). Phase 4 closed the same day
(D57).

## Phase 5 — Temporal upscaling and frame generation

Goal: the temporal techniques modern PC games get, on a game that never had them (D40).

- [x] Motion vectors: camera motion from depth and the current and previous camera matrices, then moving
      objects (a draw matched with the previous frame's, its vertex shader run with the previous
      constants). Both done for DLAA (D47, D48, 2026-10-14).
- [x] Jitter of the scene's draws, the upscaler before the post-processing and the HUD. The jitter is a
      sub-pixel offset of the main 3D view's viewport rather than of each draw's NDC (D47); DLSS runs where
      the view's post-processing starts (D47, D49).
- [x] DLSS / DLAA (DiligentCore's SuperResolution module has DLSS on Vulkan) and FSR 3 (D116), as quality options.
      Started on 2026-10-14 (D46), before Phase 4 closes, in this order:
  - [x] DLSS found and reported: DiligentCore's SuperResolution module with NVIDIA's NGX SDK, the runtime
        next to the executable; `native_x_renderer_antialiasing = dlaa` reports it (D46).
  - [x] Jitter of the main 3D view, through its draws' viewport (D47).
  - [x] Motion vectors from depth and the camera's matrices, with the view's depth range: POTCAWE's depth
        is reversed (D47).
  - [x] DLAA before the game's post-processing: `native_x_renderer_antialiasing = dlaa`, a launcher choice;
        +1.2 ms at 2×, +2.5 ms at 3× on an RTX 4070 Ti (D47, 2026-10-14).
  - [x] Motion vectors of objects moving on their own (characters, animated props): the main view's
        draws matched with the last frame's, those that moved drawn again with their vertex shader run on
        both frames' constants, depth equal. About 150 of ~960 draws at the dock; no measurable GPU cost,
        +0.65 ms on the render thread (D48, 2026-10-14).
  - [x] Upscaling modes (the 3D views at a second, lower render scale): DLSS Quality, Balanced,
        Performance and Ultra Performance, a launcher choice. The main 3D view's output-sized targets at the
        reduced scale; resolved textures and everything else at the render scale; DLSS's result and the
        depth brought back at the view's end. At 3×: DLAA 60.5 %, Quality 51.4 %, Ultra Performance 46.3 %
        of the GPU (D49, 2026-10-14). Taken out of the launcher as experimental, still available from
        `potcawe_options.toml` (D53, 2026-10-15).
- [x] Spatial upscaling and sharpening when presenting: AMD FSR 1, NVIDIA Image Scaling, AMD CAS on the bilinear
      stretch, for a lower internal resolution; a launcher choice (RecompLauncher 3.8). FSR 1 +0.14 ms, NIS +0.20 ms a
      frame at 3440x1440 (D115, 2026-11-03).
- [x] FSR 3.1 beside DLSS (AMD's FidelityFX API on Vulkan), for AMD, Intel and GTX: `fsr_native` in place of DLAA,
      and the upscaling modes (experimental). POTCAWE at 3440x1440: Native AA 6.05 ms of renderer GPU time against
      DLAA's 6.15, Quality 5.35 against DLSS Quality's 5.7 (D116, 2026-11-03).
- [x] The camera's motion vectors through one reprojection matrix made in double precision: a still camera's were a
      fraction of a pixel off at random, which DLSS and FSR blurred. At the dock DLAA's texture detail about twice as
      sharp, FSR 3.1 Native AA's 2.5 times (D117, 2026-11-03).
- [x] The camera's motion by a vote of the main view's draws (King Kong Recompiled's method):
      `native_x_renderer_camera_vote`; in POTCAWE the same reprojection as its hook's, 0.000 px apart (D118,
      2026-11-03).
- [x] The main 3D view without hooks: from its first depth-writing perspective draw of the output size to the copy
      the profile names (`upscale_resolve_caller`); POTCAWE as with its hooks, GH Metallica's songs with DLAA
      (D119, 2026-11-03).
- [x] The motion draws of objects that moved with the camera only skipped: GH Metallica's 287-306 a frame -> 3-8, DLAA
      at 160 FPS 8.8-9.7 -> 6.50-6.77 ms a frame against SMAA's 6.25 (D120, 2026-11-03).
- [x] The upscaling modes without hooks: reduced targets from the frame's start after a frame that reached the
      upscale point. GH Metallica's songs with DLSS / FSR 3.1 Quality; POTCAWE the same with its hooks or without
      (D121, 2026-11-03).
- [x] DLSS's CPU cost cut again: no submission before the upscaler (DiligentCore patch 0010), cheaper vote and
      matching, the color-over-depth search by key: GH Metallica's busiest venue with DLAA 5.69 -> 5.3 ms of renderer
      CPU a frame, SMAA 4.5 (D122, 2026-11-03). DLSS itself is 1.2 ms (K) / 1.55 ms (M) of GPU at 3440x1440.
- [x] DLSS's model a choice: Transformer (render preset 0, NVIDIA's default for each mode, as before) or CNN (preset
      F), NGX's render preset hint (DiligentCore patch 0013), `native_x_renderer_dlss_model` and a launcher row
      (RecompLauncher 3.20.0); NGX's own log in the game's on request (patch 0014, `native_x_renderer_dlss_log`), to see
      the preset NGX used (D143, 2026-11-10). Run in GH Warriors of Rock: DLSS 310 no longer has E, CNN is F.
- [x] The DLSS runtime NVIDIA's 310.9.16, from NVIDIA's DLSS repository and checked by its hash, in place of
      DLSS-Headers' 310.5.3; the NGX headers and library stay DLSS-Headers' (D144, 2026-11-10). In GH Warriors of Rock,
      put beside the game by hand: 310.9.1 with the Transformer defaults (K, M) and preset F taken, 310.9.16 with preset
      F taken; 310.9.16 with the defaults (hint 0): K for DLAA, Quality and Balanced, M for Performance, L for Ultra
      Performance.
- [x] `native_x_renderer::ResetTemporalHistory()`: the game resets DLSS's and FSR 3.1's history at its camera cuts
      (D145, 2026-11-10), for GH Metallica's ghosting at the cuts and GH Warriors of Rock's. Run in GH Warriors of
      Rock: two resets a cut, all counted.
- [x] A view over two frames: DLSS and FSR 3.1 once a pair for a game that splits its scene (GH Warriors of Rock's
      split render, the console's motion): the view paused at the first frame's copy and taken up in the next, one
      jitter and one application a pair, D48 and the vote from pair to pair, the first frame's copies into reduced
      variants of their textures (`Profile::view_pause_outer_caller`, `upscale_resolve_outer_caller`; D146,
      2026-11-10). Run in a GH Warriors of Rock song with DLSS Quality and Performance: one application a pair, smooth,
      no ghosting. A flicker of small bright details behind the game's depth of field with DLSS, also with the split
      off: open.
- [ ] The temporal upscalers without the game's view hooks: the camera found by a vote of the draws, the upscale point
      by shader hash (`GHMetalRecomp/local_docs/27-upscalers-plan.md`, from King Kong Recompiled's renderer).
- [x] Frame generation, for more than 60 FPS without changing the game's timing: AMD FSR 3.1's frame interpolation
      without its swap chain, the HUD from the scene resolve's picture, V-Sync on. POTCAWE at 120 FPS from its 60
      (D123, 2026-11-03).
- [x] Frame generation in GH Metallica, its note highway interpolated with the scene: the 2D layer's motion from its
      vertices, the board's from its beat lines; 160 FPS from 80 (D124, 2026-11-03).
- [ ] NVIDIA's frame generation (Streamline, needs a variable refresh rate display on Vulkan); a launcher row.
- [x] A minimized window's frames ended without being shown: Alt+Tab from exclusive fullscreen had DiligentCore's
      dynamic heap run out within seconds in a GH Metallica song, and the game crash (D125, 2026-11-04).
- [x] A pass drawn frame after frame does not wait for its pipelines: a pipeline new to the driver stopped a GH5 song for
      most of a second (D126, 2026-11-07).
- [x] The reduced copies of the screen (640x360, 320x180) at the game's resolution: GH Metallica's bloom and depth of
      field showed a lattice at high render scales, their blurs' offsets being constants for 320x180 texels (D127,
      2026-11-07).
- [ ] D127 checked in every game (Gustavo, 2026-11-07: every recomp made and to come): at 1440p or above, the bloom
      and the blurred backgrounds looked at for a grid or a doubled glow, and `Profile::unscaled_screen_fractions` set
      where the post-processing blurs reduced copies of the screen. Set: GH Metallica, GH Van Halen (the same targets),
      GH Warriors of Rock (2026-11-08: its bloom showed no grid, but its depth of field's blurred background did, at
      3440x1440 in the rocker creator; a frame dump showed the 320x180 blurred copy striped; Gustavo checked it), GH5 and
      Band Hero (the same engine, set the same day; GH5 had been marked not needed after a look at its title screen's
      bloom only). Left: GH Smash Hits (the World Tour engine: needed, the issue's author), GH Aerosmith
      (GH III's engine), Kameo, GRAW, POTCAWE; each new game at integration (`integrating-a-game.md`, step 4).
- [x] Alpha to coverage on 1x targets dithers in 2x2 as on the console: the translator's sample mask needs the GPU's
      sample rate shading, which DiligentCore patch 0012 enables (D138, 2026-11-11; D114's dither had never run, alpha
      to coverage was off there). Kameo's grass: no dark outlines and not see-through.
- [ ] D138 checked in every game (Gustavo, 2026-11-11: every recomp): with NativeXRenderer, what the game draws with
      alpha to coverage at 1x (foliage, hair, fences) compared with the Xenos path, now that it dithers instead of
      being off. GH Metallica's and GH5's hair (D114) first. Checked: Kameo.

## Phase 6 — Second game and extraction

- [x] GRAW (XDK 2.0.2417, three years older than POTCAWE's): map its D3D layer with
      [`integrating-a-game.md`](integrating-a-game.md), write its profile, observer at 100 %, first frames.
  - [x] Step 0: GRAWRecomp on the shared SDK series (0001–0020), the library as a submodule
        (2026-10-15).
  - [x] Map, hooks and profile (`src/native_x_renderer_graw.cpp`); the observer at 100 % in the training
        mission, 3888 draws over three frames (D59, 2026-10-15).
  - [x] First frames. The scene is 4× MSAA in three predicated tiles of 1280×256: NativeXRenderer has to
        draw a tiled pass once into a target of the tiles' full size, without the game's MSAA (POTCAWE
        turned its MSAA off instead, D25, D31).
    - [x] The tiled pass drawn once at the tiles' size, host targets without MSAA, the SDK's shader storage
          off (D60); the hooks' calls from other threads kept out of the tracker (D61); the occlusion query
          pool for GRAW's 367 queries a frame (D62). The training mission draws at 60 FPS (2026-10-16).
    - [x] False colors and 2×2 dot patterns on many surfaces (green walls and props): NVIDIA's Vulkan driver
          samples cube maps with gradients wrongly in one pixel of each quad. The SDK's translator samples
          them at a LOD it computes (shared patch 0021), which also fixes the Xenos path's Vulkan `fbo` mode
          in GRAW; the "if blocks" mode was tried and dropped (D63, 2026-10-16).
    - [x] Large yellow outlines in the HUD that the Xenos path does not show, and no objective arrow: the
          game reads a depth-stencil resolve as k_8_8_8_8 for the stencil (D64, 2026-10-16).
    - [x] Antialiasing: SMAA on the scene resolve, named by two return addresses (D65, 2026-10-16).
    - [x] Alpha to coverage dithers at 1× (fences, vegetation): the game counts on its 4× MSAA. Blended by
          the alpha, depth where the alpha is at least 0.5 (D69, 2026-10-17).
  - [x] GRAW Launcher offers NativeXRenderer (experimental), its internal resolution and antialiasing, and
        anisotropic filtering; ultrawide screens filled through GRAW's own hooks (D66, 2026-10-16).
  - [x] Objects and the objective's arrow vanishing for a few frames: D32's answers mixed up GRAW's pooled
        query objects. Queries are measured only with `occlusion_query_enable` on; GRAW keeps the fake count
        (D67, 2026-10-16).
  - [x] The render thread's per-call costs: clock reads, cvar lookups by name, shader hashes per draw, the
        shadow copy (D68, 2026-10-17): about 1.9 ms less per frame in GRAW's training mission.
  - [x] Resolved textures bound after their memory held other data (a DXT3 texture read as a depth resolve;
        likely the green Cross-Com video): invalidated when their guest memory is allocated again or written
        (D70, 2026-10-17).
  - [x] 120 FPS in a mission: the renderer on a thread of its own, the command processor waiting for it at
        its signals (D71, SDK patch 0006, 2026-10-17): 79 -> 117 FPS at 120 Hz, 131 at 144 Hz.
  - [x] Less tracker and capture work on the game's render thread: setters that write no packet skipped, a
        draw's constant writes left out, dirty fetch slots, runs of changes, programs by pointer (D72,
        2026-10-17): 133.5 -> 147.7 FPS at 240 Hz.
  - [x] The register file on the renderer's thread: the tracker records what changed (D73, 2026-10-17): GRAW's
        training 216 -> 220.7 FPS at 240 Hz; the mission's frame is now set by the main thread as well.
  - [x] Resolved textures kept over an allocation or release of their memory when its data is unchanged, freed
        by wall time; fetches that read guest memory a resolve had written are counted (D74, 2026-10-18).
- [x] Update the core for whatever turned out to be POTCAWE-specific.
  - [x] The dirty masks and the resource objects described by the profile; inline draws that the game
        fills between BeginVertices and EndVertices; the fetch and segment rules of the state tracker
        (D59).
  - [x] Checked 2026-10-20, with three games on three XDKs (5426, 2417, 2099): what differs is in the profile
        (device and object layouts, the inline draws' end `inline_draw_end` (D77), the scene resolve's callers,
        `widens_cameras`), guest memory is read through the SDK's translation (D76), and POTCAWE is named only
        in the core's comments, as an example. What no game needs yet stays in "Later" (the known limits of
        Phase 2); a game's own MSAA is turned off (POTCAWE, D25) or drawn without it (GRAW, D60).
- [x] `git subtree split` into its own repository. The games then use it as a submodule, and its SDK
      patches go with it. Done early, with the rename, for POTCAWE's first release (D58, 2026-10-15).
- [x] Kameo (XDK 2.0.2099) after that. Started 2026-10-18 (KameoRecomp `docs/native-x-renderer-kameo.md`); the default
      renderer of Kameo and GRAW since 2026-10-21 (Gustavo), as in POTCAWE (D56).
  - [x] Steps 0-3: the library linked, the D3D library mapped (GRAW's layout up to +12964, a smaller tiling block;
        LTCG inlining), the hooks and the profile; the observer at 100 % in the title video, the menu, a video and
        the first level, after reading guest memory through the SDK's translation (D76) and tracking the inline
        draws the engine commits itself (D77).
  - [x] Step 4: first frames. The title video, the menu, the videos and the first level draw as on the Xenos path,
        after clears given the target's own rectangle (D78) and the samples of a query's always-pass draws (D79,
        the light glows) (2026-10-18). D79 withdrawn by D87 (2026-10-20): counted, those glows washed out and
        flickered over the Enchanted Kingdom's forest; not counted, as before D79.
  - [x] Kameo Launcher offers NativeXRenderer (experimental), its internal resolution, SMAA / FXAA / off (the
        profile names the scene resolve) and anisotropic filtering (2026-10-18); ultrawide through Kameo's own
        hooks, which take `AspectFactor()` (`widens_cameras`, 2026-10-19).
  - [x] Performance A/B at the same outputs (1280×720, 2560×1440, 3440×1440, 30 and 60 FPS, 2026-10-19): the GPU
        nearly equal at 1280×720, about a third less at the higher outputs (3440×1440 at 60 FPS: 36 % / 86 W
        against 23 % / 64 W); more CPU (the renderer thread about 10 ms a frame in the level, 3,850 draws); isolated
        frames of 30-50 ms in the Bink videos (KameoRecomp `docs/native-x-renderer-kameo.md`, section 8).
  - [x] The flying dragons' billboards in two or three wing positions at once: a resolve's clears (`0x300`) were
        ignored; done after the copy, and partial clears filled (D80, 2026-10-20).
  - [x] The menu's and loading screen's smoke cut into hard-edged shapes: signed texture bindings got the unsigned
        view, so a signed distortion map jumped at its zero crossings; a signed view now (D85, 2026-10-20).
  - [x] The Bink videos' isolated 30-50 ms frames: NativeXRenderer presents when the game calls Present, while the
        command processor executes the game's swaps paced by the vblank; the game's own waits (its Bink texture lock)
        delay its CPU by a vblank now and then, which the Xenos path absorbs and NativeXRenderer shows (KameoRecomp
        `local_docs/15`). `native_x_renderer_present_with_swaps` (D86, SDK patch 0008, 2026-10-20): frames shown
        with the guest's swaps, no frame over 20 ms; off in the library by default, on in Kameo's default
        configuration since Gustavo compared both (2026-10-20).
  - [x] The renderer thread's CPU time: about 10 ms a frame in the first level, 4,000 draws (the profile in
        [The renderer thread's CPU time](#the-renderer-threads-cpu-time-kameo-2026-10-20)); four steps, done
        2026-10-20: 9.6–9.9 → 6.4–6.5 ms (about −33 %).
    - [x] Step 1, caches and redundant state (D81, 2026-10-20): 9.6–9.9 → 9.2–9.4 ms; GRAW 3.05 → 2.90 ms,
          POTCAWE 4.5 → 4.4 ms.
    - [x] Step 2, binding textures and samplers only when they change (D82, 2026-10-20): 9.2–9.4 → 7.2–7.5 ms;
          GRAW unchanged (2.8–2.9 ms), POTCAWE 4.4 → 4.3 ms.
    - [x] Step 3, views and samplers cached by fetch constant (D83, 2026-10-20): 7.2–7.5 → 6.6–6.7 ms; with it
          only the fetch constants that changed recompute their SDK binding.
    - [x] Step 4, the shared memory's vector per call (D84, SDK patch 0007, 2026-10-20): 6.6–6.7 → 6.4–6.5 ms.
          GRAW and POTCAWE with D83 and D84: GRAW 2.8–2.9 → 2.75–2.85 ms (observer 3,843/3,843), POTCAWE
          4.3 → 3.9–4.0 ms (1,128/1,128); the same images.
  - [x] D77's dropped inline draws (twice in Gustavo's sessions): committed draws followed by the game's own cache
        coherency packets, dropped by mistake; drawn now (D88, 2026-10-21).
  - [x] Parity: the title, the menus, the videos, the first level and the Enchanted Kingdom played on NativeXRenderer
        by Gustavo (2026-10-18..21); what he plays next is followed in "Later".

## Phase 7 — More games

With three games, keeping them in step costs more than bringing up a game: the SDK's changes, the library, the
launchers and the tools were copied from one repository to the next. First the infrastructure, then the next game
(Gustavo, 2026-10-21).

- [x] Infrastructure.
  - [x] One place for the SDK's changes: the `recomp` branch of ElGuslml/rexglue-sdk, the games' SDK submodule;
        the 25 shared fixes and the library's 8 changes as commits (D89, 2026-10-21).
  - [x] A script that tests every game: each one's route (GRAW's training, POTCAWE's dock, Kameo's first level)
        with each renderer, the observer, the threads' CPU, the log's new errors and warnings, screenshots compared
        with baselines, one report: `Test-Games.ps1` in a repository of its own, recomp-tools (2026-10-21; the six
        runs in about 10 minutes).
  - [x] A project template: a new game's repository from KameoRecomp (CMake, launcher, startup, options, tools,
        release) by a script: recomp-tools' `New-RecompProject.ps1` and `template\` (2026-10-21). The launcher's
        optional rows (60 FPS, ultrawide, NativeXRenderer) are turned on in the game's `src/project.h`; the library
        is built and linked from the start, unreferenced until the game's side exists. The tools each game copied
        moved to recomp-tools (`tools\`, `Game.ps1`). Tested by setting Kameo up again from its executable: the
        same first codegen as on 2026-10-12 (15,276 functions, the same 74 gap functions, 439 switch tables
        without differences, after the same tail-call target by hand) and the title screen on D3D12.
  - [x] The SDK's branch on upstream's nightly of 2026-10-02 (v0.10.0 and 24 later commits; D90, 2026-10-21):
        the library's changes unchanged, the three games tested.
- [ ] The next game: chosen by its XDK version (one already mapped is cheaper), whether it uses memory export or
      tessellation (not supported yet) or predicated tiling with MSAA (D60), and its state in Xenia Canary.

## Later

Work left for after the planned phases.

- [ ] Kameo: the rest of the game on NativeXRenderer, as Gustavo plays it; a difference he reports is compared with
      the Xenos path at that spot. Not seen yet: the tessellated draw `sub_820CF228`, predicated tiling and a present
      from the tiling thread.
- [ ] GRAW: DLAA. GRAW's views marked (`BeginView` / `EndViewGeometry`) and the jitter in its tiled pass. Moved
      out of Phase 6 by Gustavo, 2026-10-20 (GRAW draws as on the Xenos path, with SMAA).
- [ ] POTCAWE: anchor HUD elements to the screen's edges when the output is wider than 16:9. The 2D layers
      are drawn in a centered 16:9 box (D43). Moved out of Phase 4 by Gustavo, 2026-10-15.
- [ ] POTCAWE: every level compared side by side with the Xenos path, as Phase 4 listed it (D57). Until
      then, a difference Gustavo reports is compared at that spot.
- [ ] POTCAWE: other levels and the cutscenes at 21:9 (D43 checked the menus, the dock and the pause menu).
- [ ] POTCAWE: the depth of field's crosshatch above 720 lines (found 2026-11-10; left for later by Gustavo the same
      day, the depth of field's switch turns it off meanwhile). Its usual mode (pixel shader `EC40189F32F953BD`) blends
      the full-size scene's center sample with four diagonal taps one guest texel away (the offsets are constants the
      game computes for 1280x720); at a render scale above 1 the taps skip host texels and every detail shows as four
      diagonal copies. Not D127's kind: there is no reduced copy (the flag is set and changes nothing). The fix planned:
      a profile entry naming the shader and its tap fetches (instructions 22-29); the copy of the microcode given to the
      translator has those fetches on a spare fetch constant, bound to a guest-resolution copy of fetch 0's resolved
      texture, box-filtered as D127's `kColorBox`, made when that texture is resolved; the center keeps the host's
      resolution. One 1280x720 downsample a frame; the pipeline list names the original hash, so the patched
      translation is made at its first use. The game's "Strong Blur" mode (320x180, clean with D127) was tried forced:
      no lattice, but far blurrier than the original. Details: POTCAWE's `docs/depth-of-field.md` and
      `local_docs/26-depth-of-field.md`.
- [x] Shadow quality: the shadow maps at a scale of their own, at least twice or four times the game's resolution
      (D92, 2026-10-22, Gustavo; the launchers' "Shadow quality" row). POTCAWE's and Kameo's are found; GRAW's
      shadow pass binds a color target and is not. POTCAWE's shaders filter the map on a size the game gives in c38:
      the host size there (D93), without which no size showed a difference.
- [ ] The known limits from Phase 2 that no scene has needed yet, when one does (partial clears done by D80; 1_5_5_5
      by D105, 2026-10-28, Guitar Hero: Van Halen's instruments): signed formats with separate data, DXT3A_AS_1_1_1_1,
      resolves into 3D textures.
- [x] The SDK's dialogs (its ImGui overlays) on NativeXRenderer's swap chain: the keyboard (GRAW asks for a new
      game's save name), message boxes (D75, 2026-10-18).
- [ ] Guitar Hero: Metallica (XDK 2.0.7978, 2026-10-24; the map and the profile in GHMetalRecomp's
      `docs/native-x-renderer-ghmetallica.md`): the songs' scene resolved through a 4x MSAA surface at its EDRAM
      (D95), and its post-processing mask resolved once and sampled minutes later (D96). Left: Gustavo's full play
      test; the crowd atlas's mip levels 1-5, which NativeXRenderer skips (it resolves level 0 only).
- [x] The renderer thread's CPU in GH Metallica's songs, 4.8 ms a frame (29 % of a core at 60 FPS, 56 % at 120):
      over half of it protecting pages again, hashing pages requested again unchanged and waiting for the lock the
      game's faults held. Pages rewritten every frame compared by hash, not watched, and texture pages no longer
      invalidated by writes next to them (D97, 2026-10-25): 2.9 ms a frame.
- [x] Uploads in batches (D98, 2026-10-25): the queued draws' vertex and index data uploaded with the first's, the
      render pass ended once per batch: GH Metallica's GPU 4.5–5.6 → 2.7–2.9 ms a frame at 3440×1440 / 160 FPS, the
      renderer thread 2.86 → 2.59 ms at 120 FPS.
- [x] DiligentCore's `Present` at 3440×1440 (D99, DiligentCore patch `0007`, 2026-10-25): the next image acquired
      when the back buffer is first needed, and waited for asleep instead of spinning in NVIDIA's driver until the
      display frees one: GH Metallica's songs at 160 FPS, the process 2.17 → 1.68 cores, the renderer thread 0.78 →
      0.39 cores, the pacing slightly better.
- [x] A shader's float constants copied a run of consecutive registers at a time (D100, 2026-10-26): Kameo's meadow
      (about 3,850 draws a frame, 120 FPS), the renderer thread 6.0 → 5.0 ms a frame. What is left of a draw's CPU:
      [performance.md](performance.md).
- [x] A first run without garbage (D101, 2026-10-27): draws into offscreen targets wait for their shaders and pipeline
      instead of being skipped, so a texture the game renders once (GH Metallica's post-processing mask) is not left
      undefined; the scene's draws are still skipped while theirs are created.
- [ ] Guitar Hero: Aerosmith (XDK 2.0.6683, 2026-10-27; GHAeroRecomp, GH Metallica's profile ported). The stage dark
      for a frame now and then in songs: a segment change outside any call read as packets, old ones from the ring's
      last lap among them (D102, done). Left: the crowd's sheets, resolved into mip levels 1-3 (18 resolves a frame,
      skipped as in GH Metallica).
- [x] New screens' data uploaded faster (D108, 2026-10-30, Guitar Hero 5): the guest memory's pages mapped ahead on
      a thread of their own (each page's first read through the mirror's view took 1.1 µs), pages never copied not
      hashed, one scratch buffer for every texture load: Party Play's song start 56–61 → 38 ms of renderer time.
- [x] D97's frame counts scaled by the frame rate (D109, 2026-10-30): at 160 FPS GH5's ring buffer, rewritten every
      17–32 frames, was never left unwatched: 1,400 → 5,800 pages unwatched, the renderer thread 4.05 → 3.72 ms a
      frame, the process 3.29 → 3.11 cores in a song.
- [x] Unwatched pages compared with a copy on the CPU, not by hash (D110, 2026-10-30): "hash only what a draw reads"
      measured first and not worth it (the draws request 83-87 % of the bytes hashed in GH5 and GH Metallica); most
      unwatched pages change at each request, and each was hashed twice. The uploads' time −6 % (GH5) and −10 % (GH
      Metallica); the copy 28 MB.
- [x] Binding sets (D111, 2026-10-30): each set of textures and samplers a pipeline draws with keeps a shader
      resource binding of its own, written once, instead of a descriptor set allocated and written at every commit.
      The renderer thread −5 % in GH5's songs at 160 FPS, −6 % in Kameo's first level. What bindless descriptors
      would have removed (profiled first: 6–7 % of the thread); bindless itself left for later.
- [x] Depth resolves through an aliased MSAA surface (D112, 2026-10-31): GH5's depth of field and motion blur read
      a depth downsampled through a 640×360 4x MSAA surface over the 1280×720 depth; D95 did it for color only, and
      the image was blurred all over.
- [x] A depth restored through a color surface at its EDRAM (D113, 2026-10-31): GH5 draws its scene over two frames
      and restores the first frame's depth by drawing its depth resolve's words into a k_8_8_8_8 surface at the depth
      surface's EDRAM; the depth stayed cleared, and its band and drum kit looked transparent.
- [x] Alpha to coverage blended only on surfaces with MSAA in the game (D114, 2026-11-01): at 1x the console
      dithers, and GH Metallica's hair blended by its alpha looked see-through.
- [x] Less CPU per draw on both threads (D139, 2026-11-11, Kameo's troll army at 120 FPS): the renderer -14.5 to -17 %
      per draw (texture look-ups, targets and pipeline kept from the last draw; fewer clock reads; the SDK's recomp
      change 82, valid pages requested without the global lock); the capture's microcode hashing halved.
- [x] The capture's per-call cost on the game's render thread (D140, 2026-11-11): setters followed lean, draws
      published without the capture's lock or a fence, the shadow runs and bool/loop constants compared with their
      last image. Kameo at 160 FPS in ultrawide: the game's render
      thread about -90 ns a draw (-0.5 ms a frame at ~5,900 draws); the observer at 100 %.
- [ ] Kameo's dense areas at 160 FPS in ultrawide (130-150 after D139, 142-153 with D140's lean setters): the
      renderer's driver costs (pipeline binds, descriptor sets for the constants' dynamic offsets), what is left of the
      capture; see [performance.md](performance.md#cpu-the-renderer-thread).
- [ ] The next performance work, in the order of [performance.md](performance.md#what-to-do-next-in-order):
      what specializing the translated shaders' per-draw state would gain (measured first in a capture), bindless
      descriptors (after D111, for what binding sets do not remove), the ring (D5).
- [x] GPUs without `D24_UNORM_S8_UINT` (AMD; D103, 2026-10-27): the guest's D24S8 depth falls back to
      `D32_FLOAT_S8X24_UINT` where the device does not offer it as a depth target, as the SDK's Vulkan backend
      does; D64's packed words take the guest's format. Checked on NVIDIA with the Khronos profiles layer hiding
      the format. Left: a player with an AMD GPU (the depth bias with a float depth, D103).
- [x] A whole AMD device simulated (2026-11-08, before the other games' move to D128-D133): the Khronos profiles layer
      with gpuinfo.org's RX 7800 XT and RX 6700 XT reports, intersected with the NVIDIA device's limits (AMD's own
      looser alignments made the NVIDIA GPU read constants wrong: false device losses), and the validation layer. The
      pipeline tool, the title and a song in GH Metallica: only the known 08737, and viewports past AMD's 16384 at
      3440x1440 (D134, fixed). AMD's Windows driver has graphics pipeline libraries with fast linking (D132's `auto` is
      NVIDIA only for now). Left: a real AMD GPU (its driver and shader compiler).
- [ ] What is left to improve, planned on 2026-11-07 from Gustavo's questions (nothing measured or changed yet; the details,
      King Kong Recompiled's figures and the order in GHMetalRecomp's `local_docs/37-renderer-next.md`):
  - [x] **The render scale as a system constant, not part of the translation.** `SetFractionalResolutionScale` writes the
        scale into the SPIR-V, so every window size, internal resolution or FSR 1 / NIS setting not used before gives the
        driver new shaders (D126's evidence; GH Metallica's driver cache 69 MB for 473 pipelines, King Kong's 37.9 MB for
        3,214). One translation for every scale: the driver's cache kept across sizes, a shipped list valid at any scale,
        and D127's native draws given scale 1 (their pixel position, wrong today). An SDK commit (the translator, behind
        the library's setter) with the library's side. Done (2026-11-08): D128, the SDK's recomp change 73; checked on
        the ten games' stored translations, Gustavo played most of the games with it.
  - [x] **A pipeline list shipped beside the game,** as King Kong Recompiled's `kkpipelines.list`: D42's records from
        Gustavo's own play (his choice, 2026-11-08: no bot; an unattended tour as King Kong's, timed inputs through every
        chapter, 35 minutes, 3,214 pipelines, stays an option; its players' uploads later took the list to 17,652),
        merged into the player's storage at start. Done (2026-11-08): D129, by hash only (the microcode from the player's
        own `MaterialLibrary.bin.xen` in the Guitar Hero games), compiled by the launcher's "Compiling shaders" (the
        game's `--native_x_renderer_prepare_pipelines`, the SDK's recomp change 74); GH Metallica's list from Gustavo's
        storages (473 pipelines). Next: the other Guitar Hero games (their profiles' `title_id` and `shader_files`, a
        list from their storages); other games need their shaders found another way (D129's limits). The Guitar Hero
        games and GRAW done on 2026-11-08 (D133). POTCAWE (2026-11-09, D136): its EDL-compressed level packages read by
        the game's own reader (`Profile::read_shader_archives`), and its vertex shaders' exports, patched by its D3D
        library for their pixel shader, carried in the list as ISA fields; 225 of 225 shaders, 327 pipelines (GRAW's list
        made again would have 572 of 588, 2 more). Left: Kameo (CAFF archives, the XEX image; exports and control flow
        patched).
  - [x] **Vulkan graphics pipeline libraries** (King Kong Recompiled's task 183): a missing pipeline linked from parts made
        ahead (about 6 µs), the optimized one made behind it; on an RTX 4070 Ti a missing pipeline's mean 26 ms -> under
        1 ms for 98 %, at 70-190 MB of video memory. A DiligentCore patch (its Vulkan backend does not use the extension);
        NVIDIA first (an RX 9070 XT lost the device with it). After the two items above, if first-use stutter is left.
        Done (2026-11-08): D130 (dynamic rendering), D131 (one resource signature), D132 (DiligentCore patch 0011,
        NVIDIA by default); GH Metallica's 473 pipelines from 266 parts.

  - [x] **The critical threads on the performance cores** of hybrid CPUs (this library's, the command processor's, the
        vertical blank timer's, the audio's, and each game's main, render and job threads), EcoQoS off, priority above
        normal, as King Kong Recompiled's `ScheduleCriticalThread`. Measured first: which threads are on the frame's chain
        (WPR / WPA's ready times), on which cores they run, and an A/B moving one at a time onto the efficiency cores. An SDK
        mechanism with each game's list of thread entry points. Measured (2026-11-08, GH Metallica's song at 160 FPS): every
        thread on a performance core, woken within 5 us: not needed on this CPU. What the trace found instead: the job
        queue's lock (the SDK's change 75) and its pulse (GH Metallica's `ghmetallica_job_wake_one`).
  - [x] **The command processor's thread measured** at 160 FPS (never done in GH Metallica). Then, before removing the
        ring (D5): a lighter ring reader that handles only the synchronizing packets (King Kong Recompiled's ring
        skimmer), the same for every game; and the D3D library's GPU waits woken by an SDK event at each fence instead of
        each game's polling slices. Measured (2026-11-08): the thread 4-13 % of a core in GH Metallica's song, 7-9 % in
        Kameo; a WPR trace in Kameo found a third of it in work nobody used, removed instead of a skimmer (D135, the SDK's
        change 76 `sync_only_lean`: 7.0 -> 4.4 % in Kameo). The GPU waits by events: GH Metallica's render thread, ~2 % of a
        core in them, not worth it.
  - [ ] **A D3D12 backend: not now.** The gain over the Xenos path is not emulating the GPU, the same on either API.
        Revisit for AMD FSR 4 (D3D12 only) or for players whose Vulkan driver fails; its cost: the shaders (the SDK's
        D3D12 translator writes DXBC with its own root signature), the Vulkan-only DiligentCore patches, the present modes,
        frame generation, every game tested twice.

---

## Baseline to beat

### Measured with this build (2026-10-13)

The Xenos path of this build, in the state NativeXRenderer starts from:
- **Spot:** the Port Royal dock, default camera after loading (`tools/level2.ps1`).
- **Window:** 1280×720 (`tools/game.ps1`).
- **Hardware and API:** RTX 4070 Ti, D3D12.
- **Sampling:** `tools/gpu_ab.ps1`, 20 s of `nvidia-smi` per configuration.
- **FPS and frame time:** the RTSS overlay in each run's screenshot.

Details, logs (`potcawe_003`–`011`) and screenshots: `local_docs/01-baseline.md`.

| Render scale | Antialiasing | Target | GPU % (min–max) | FPS / frame time | GPU time per frame ≈ |
|---|---|---|---|---|---|
| 2×2 | 2× MSAA (original) | 30 FPS | 42.5 (41–44); repeat 41.1 (40–42) | 30 | 14 ms |
| 2×2 | Off (no MSAA, one tile) | 30 FPS | 27.1 (24–30) | 30 | 9 ms |
| 3×3 | 2× MSAA | 30 FPS | 82.7 (79–86) | 30 / 32.8 ms | 27.6 ms |
| 3×3 | Off | 30 FPS | 51.2 (50–52) | 30 | 17 ms |
| 2×2 | 2× MSAA | 60 FPS | 82.6 (81–84) | 60 / 16.2 ms | 13.8 ms |
| 2×2 | Off | 60 FPS | 50.8 (49–52) | 60 / 16.5 ms | 8.5 ms |
| 3×3 | 2× MSAA | 60 FPS | 90.5 (83–95) | **33** / 29.1 ms | — (the GPU is the limit; the queries' CPU waits keep it under 100 %) |
| 3×3 | Off | 60 FPS | 96.9 (96–97) | 60 / 17.1 ms | at the limit |

"GPU time per frame" is utilization × frame interval. It is an approximation: the GPU clocks down at low
load, which inflates utilization. Use it to compare configurations, not as an exact time. The repeat of
the first configuration at the end differs by 1.4 points, which is the noise level.

Earlier numbers from POTCAWERecomp (`local_docs/05-gpu-load.md`, `06-sun-glow.md`) were higher because
they were measured before `clear_memory_page_state = false`: 2×2 MSAA at 60 FPS 89 %, Antialiasing Off 59 %.
POTCAWERecomp's 3×3 MSAA result of 37 FPS was measured toward the sun, not at the default camera.

### Where the time goes

GPU time at 2×2 with Antialiasing Off, by kind of event, from RenderDoc's per-event timing in POTCAWERecomp
(before `clear_memory_page_state = false`). Replay times are inflated (~3 µs floor per event), about 1.75×
the real frame:

| Kind | Replay ms | Removed by NativeXRenderer? |
|---|---|---|
| The game's draws | 10.8 | no: the same translated shaders |
| The game's full-screen quads | ~0.9 | no |
| EDRAM ownership transfers (shadow map region) | ~1.0 | yes |
| Resolves (dump into the EDRAM buffer and copy to shared memory) | ~1.5 | mostly: a resolve becomes a texture reference, or an MSAA resolve |
| Texture loads of resolve results | ~1.0 | yes |
| Shared memory uploads | ~1.0 | partly: vertex and index uploads remain |
| Clears (draw clears of patch 0018) | ~0.4 | becomes native clears |

### Estimate

From [`architecture.md`](architecture.md#expected-gains), applied to this baseline:
- **2×2, Antialiasing Off:** ~8.5–9 → ~6.5–7 ms (about −20–25 %).
- **2×2, 2× MSAA:** ~14 → ~9–9.5 ms (about −30–35 %), because the scene is drawn once instead of twice.
- **3×3, 2× MSAA:** 33 FPS → about 50–55 FPS.

Phase 3 replaces these estimates with the same nine runs on NativeXRenderer.

### Measured on NativeXRenderer (Phase 3)

2026-10-14, build `55b01f9` (D30–D38). The same spot and method as the baseline: `tools/gpu_ab.ps1`, 20 s
of `nvidia-smi` per configuration, in a 1280×720 window. Both paths ran in one session, alternating, so
that the machine's state is the same for both. Details: `local_docs/06-phase3-performance.md`.
- **Renderers.** The Xenos path on D3D12. NativeXRenderer at a forced render scale
  (`native_x_renderer_resolution_scale`) equal to the Xenos path's whole scale.
- **No MSAA on NativeXRenderer** (D31): its rows compare with the Xenos path's "Off" rows. The MSAA rows show
  what the game's original setting costs.
- **Extra columns.** The GPU clock and board power from `nvidia-smi`. For NativeXRenderer, its own GPU time
  per frame (duration queries around each frame, 600-frame average).
- **The GPU stays at 2745–2880 MHz in every run,** so the utilization is not inflated by a lower clock.

| Scale | Target | Path | GPU % (min–max) | Power | FPS (RTSS) | GPU per frame ≈ (util. × interval) | NativeXRenderer's own GPU time |
|---|---|---|---|---|---|---|---|
| 1× | 60 FPS | Xenos, AA Off | 28.9 (27–31) | 55 W | 60 | 4.8 ms | |
| 1× | 60 FPS | NativeXRenderer | **17.8** (17–19) | 50 W | 60 | 3.0 ms | 1.69 ms |
| 2× | 60 FPS | Xenos, 2× MSAA | 81.4 (79–84) | 145 W | 60 | 13.6 ms | |
| 2× | 60 FPS | Xenos, AA Off | 51.3 (50–53); repeat 51.6 | 106 W | 60 | 8.6 ms | |
| 2× | 60 FPS | NativeXRenderer | **26.8** (25–29); repeat 27.4 | 68 W | 60 | 4.5 ms | 3.30 ms |
| 3× | 60 FPS | Xenos, 2× MSAA | 93.2 (91–95) | 175 W | **33** / 30.3 ms | 28.2 ms | |
| 3× | 60 FPS | Xenos, AA Off | 96.9 (96–97) | 181 W | 59 / 16.7 ms | at the limit | |
| 3× | 60 FPS | NativeXRenderer | **42.6** (41–44) | 118 W | **60** / 15.7 ms | 7.1 ms | 5.85 ms |
| 2× | 30 FPS | Xenos, 2× MSAA | 41.3 (39–44) | 99 W | 30 | 13.8 ms | |
| 2× | 30 FPS | Xenos, AA Off | 28.3 (26–30) | 66 W | 30 | 9.4 ms | |
| 2× | 30 FPS | NativeXRenderer | **15.8** (14–17) | 55 W | 30 | 5.3 ms | 3.34 ms |
| 3× | 30 FPS | Xenos, 2× MSAA | 84.5 (81–86) | 162 W | 30 / 33.7 ms | 28.2 ms | |
| 3× | 30 FPS | Xenos, AA Off | 50.8 (47–53) | 116 W | 30 | 16.9 ms | |
| 3× | 30 FPS | NativeXRenderer | **24.4** (23–25) | 70 W | 30 | 8.1 ms | 6.08 ms |

- **Without antialiasing on both sides,** NativeXRenderer uses 38 % less of the GPU at 1×, 44–48 % less at
  2× and 52–56 % less at 3×. The gain grows with resolution.
- **The plan's estimate is beaten by far.** It was 2×2 Off from ~8.5–9 ms to ~6.5–7 ms, about −20–25 %.
  Measured: 8.6 → 4.5 ms by the same utilization method (−48 %), and 3.3 ms by NativeXRenderer's own
  timing.
- **Against the original 2× MSAA,** NativeXRenderer without antialiasing uses a third of the GPU, but that is
  not like for like until NativeXRenderer has its own antialiasing (Phase 4).
- **The utilization estimate for NativeXRenderer is above its own measurement** (4.5 against 3.3 ms at 2×).
  The utilization also counts what is not in NativeXRenderer's command buffers: presentation, the desktop
  compositor, the overlay. That share is in the Xenos path's figures as well, so the comparison by
  utilization is like for like.
- **Repeats** differ by 0.3 (Xenos) and 0.6 (NativeXRenderer) points, and the Xenos path's figures are within
  1–2 points of the 2026-10-13 baseline: the noise is small.

### The renderer thread's CPU time (Kameo, 2026-10-20)

Before any change, as a record to continue from (KameoRecomp `local_docs/13-renderer-cpu.md`, its scripts and
reports in `local_docs/evidence/13_renderer_cpu`). Kameo's first level, standing, 1280×720, 60 FPS, library
`2ac9e4e`: the renderer thread at 59–62 % of a core, **9.6–9.9 ms of CPU a frame** (the log's "renderer CPU"), for
4,030–4,140 draws a frame binding about 9.3 textures and as many samplers each. A sampling profile of the thread (its
stack every 2 ms through StackWalk64 and the PDBs; ETW was not available), as shares of its busy samples:

| Group | Share | What |
|---|---|---|
| The draw path | 83 % inclusive | everything per draw |
| `DrawInternal`'s own code | 11 % | register reads, the fetch and constant bitmaps, building the state |
| Binding textures and samplers (DiligentCore) | ~15 % | `ShaderVariable::Set` for every texture and sampler of every draw, unchanged or not; `CommitShaderResources` allocating and writing a dynamic descriptor set per draw |
| The driver | ~16 % | mostly under the binding (descriptor set allocation 3 %, writes 2.6 %), then `SetPipelineState` 2.8 %, `DrawIndexed` 1.9 %, `SetViewports` 1.3 % |
| Look-ups per draw | ~10 % | the shader and its host variant (4.5 %), the pipeline (an XXH3 of the state, 1.8 %), samplers (3.5 %), views (2.4 %), `ResolvedTextures::FindNewest` twice per texture (2.4 %) |
| The SDK's texture cache | ~5 % | `TextureCache::RequestTextures` |
| The SDK's shared memory | ~7 % | `SharedMemory::RequestRanges`: a `std::vector` allocated and freed per call, its lock, `VirtualProtect` |
| Constants | ~3 % | five buffers mapped per draw |
| `ApplyRecord` | 3.8 % | D73 |
| Present | ~6 % | mostly RTSS's hooks and timing inside `vkQueuePresentKHR` |

The game's render thread spent about 1.5 ms a frame in the capture (D72, D73). Planned, in order of risk: (1) caches
and redundant state (the shader with its program, viewport / scissor / blend / stencil / index buffer only when they
change, fetch and bool/loop constants only when they change); (2) textures and samplers set only when they change,
the resources committed only when something changed; (3) views and samplers cached by fetch constant; (4) the shared
memory's vector per call (an SDK patch).

After the four steps (D81–D84, the same spot): **6.4–6.5 ms a frame**. The largest parts left, as
shares of the busy time: the draw path's own code 17.5 %; `DrawIndexed` 9.3 % (the descriptor sets bound again in
every draw for the dynamic buffers' offsets, 4.2 %); Present 8.6 % (RTSS); `SetPipelineState` 7.0 %; the SDK's range
requests 6.9 % (their lock, `VirtualProtect` for uploaded pages); `ApplyRecord` 6.4 %; `CommitShaderResources` 5.6 %
(a quarter of the draws); `GuestMemory::WriteIfChanged`'s comparison 3.3 %; the texture look-ups about 9 %.

## Test protocol

- **Automated first.** `tools/game.ps1` and `tools/level2.ps1` drive the game to the Port Royal dock with
  the keyboard as the controller, and take screenshots and RenderDoc captures. `tools/gpu_ab.ps1` runs one
  configuration per line and samples `nvidia-smi`. Measure at 30 FPS when comparing GPU load: at 60 FPS
  the cap hides gains.
- **Same spot, both renderers.** Every visual check is a pair of screenshots: Xenos path and NativeXRenderer,
  same save, same camera input.
- **Gustavo's runs** are short, concrete and grouped: several questions per run, and the logs are checked
  first, before asking for another run (project rule 4).
- **Correctness tools:**
  - the observer (Phase 1), for state;
  - RenderDoc on the Vulkan device, for draws (`tools/renderdoc/`, adapted to Vulkan captures);
  - the trace mode, for call sequences.

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| The SDK's SPIR-V translator always builds `loop { switch (pc) }`, the structure that produced the flat-colored objects in Port Royal on D3D12 (fixed there with `dxbc_switch = false`, which only affects DXBC). | The same corruption on Vulkan under load. | Check Port Royal at 2×2/60 FPS early in Phase 2. If it appears, add an "if blocks" mode to the SPIR-V translator (an SDK patch, like the DXBC one). GRAW's false colors (2026-10-16) looked like it, but came from cube maps sampled with gradients (D63); an "if blocks" mode was tried and did not help. |
| The engine bypasses the D3D library (direct PM4 writes, precompiled command buffers, inlined setters). | Draws or state NativeXRenderer never sees. | **Closed for POTCAWE (Phase 1):** every draw packet comes from a hooked call; inlined setters are covered by the shadow; the observer rebuilds 100 % of the draws. Recheck per game. |
| Predicated tiling hides per-tile differences. | Wrong constants or resolves when two tiles are collapsed into one pass. | **Closed (Phase 1):** only the window offset, the scissor and the query address differ per tile. Treat BeginTiling…EndTiling as one pass, and merge the engine's per-tile resolves. |
| Occlusion queries per tile (the sun flare). | Wrong glare strength. | Host queries; compare with the Xenos path (patch 0020) at the same spot. |
| The CPU reads resolve results, or shaders use memexport. | Missing data in guest memory. | Phase 1: no memexport shader, no texture lock per frame. A readback path only if Phase 3 shows stale data where the CPU reads. |
| Textures the CPU writes without Unlock (Bink video planes). | Stale video frames. | Settled in step 7: the guest memory mirror's page watches mark them outdated, and a hash skips the loads whose data did not change (D23). |
| Per-draw work on the game's render thread. | CPU-bound frames. | Measure with `tools/threadcpu.ps1`. Move recording to a worker if needed. |
| DiligentCore build integration (target name collisions with the SDK, AVX2, VSync, BC1). | Build or runtime problems. | Known fixes, documented by Hell's Gate (Phase 2, step 1). |
| SDK presenter and ImGui overlay versus a second swap chain. | Overlay missing, or present conflicts. | Decide in Phase 2, step 4. The overlay can stay on the Xenos path at first. |
| Parity long tail. | Phase 3 takes longer than estimated. | The Xenos path stays as the fallback, and Native stays optional until parity is reached. |

## Keeping in sync

- **POTCAWE fixes:** `git fetch potcawe`, then merge `potcawe/main` (or the branch that has the fix). Game
  fixes keep being made in POTCAWERecomp first.
- **The shared SDK series** (`patches/`, shared with GRAWRecomp and KameoRecomp) stays identical to the
  other projects. SDK patches that only NativeXRenderer needs live with the library (D10).
