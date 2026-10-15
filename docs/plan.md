# XenosNative — work plan

First game: Pirates of the Caribbean: At World's End (POTCAWE), in `POTCAWERecompVulkan`. Started
2026-10-13. Estimates are in working days, at the pace of the POTCAWERecomp work so far. Tick the boxes as
tasks are done, and add the date and a pointer to the notes that close each one.

| Phase | Goal | Estimate | Status |
|---|---|---|---|
| 0 | Scaffolding: project, build, documents, baseline | 0.5–1 day | done (2026-10-13) |
| 1 | Map the game's D3D layer and verify that every draw can be rebuilt from it | 1–2 days | done (2026-10-13): 100 % of the draws rebuilt; go (D14) |
| 2 | Core renderer: first frames drawn by XenosNative | 4–6 days | started (2026-10-13) |
| 3 | Parity in POTCAWE: every pass, every level, the launcher option | 4–8 days | not started |
| 4 | Second game (GRAW) and extraction of `xenos_native/` into its own repository | 3–5 days | not started |

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
      [`../../docs/xenos-native-potcawe.md`](../../docs/xenos-native-potcawe.md).
- [x] Library skeleton: `xenos_native/CMakeLists.txt` (static library `xenos_native`, linked into
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
[`xenos-native-potcawe.md`](../../docs/xenos-native-potcawe.md).

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

   Written into the profile, `potcawe::XenosNativeProfile()` in `src/xenos_native_potcawe.cpp`
   (2026-10-13). The layout is Dante's Inferno's shifted by −128 and −312 bytes in two blocks.
3. **Trace mode.** Hooks on every entry point, logging calls per frame (counts and arguments for one
   frame on a key press). Tracing is a feature of the library (cvar `xenos_native_trace`, off by default),
   because every new game goes through this step.
   - [x] Hook file `src/xenos_native_potcawe.cpp` (game side) with one line per entry point: 39 entry
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
open questions answered in [`xenos-native-potcawe.md`](../../docs/xenos-native-potcawe.md). Met on
2026-10-13.

**Decision: go or no-go for Phase 2.** Recommendation: **go**. Every draw is visible at the D3D layer, its
state can be rebuilt exactly, and the costs D1 targets are all there:
- the aliased clears are library calls a renderer can replace with host clears;
- the tiled scene is issued once by the CPU;
- the resolves are explicit calls with a destination texture.

**Decided: go** (Gustavo, 2026-10-13; [`decisions.md`](decisions.md), D14).

**What Phase 2 starts from:**
- the profile and the hooks (`src/xenos_native_potcawe.cpp`);
- the rebuild rules, which move from `src/observer.cpp` into the core;
- the observer, kept green as a regression test;
- two new items for the plan:
  - textures the CPU writes without Unlock (Bink): step 7;
  - skipping the SetShaderGPRAllocation draws in sync-only mode: step 3.

## Phase 2 — Core renderer

Goal: XenosNative draws the title screen, the main menu and a Port Royal frame. The images do not need to
be perfect yet.

Started on 2026-10-13. Turned on with `--xenos_native=true`; the Xenos path stays the default.

State on 2026-10-13: every draw of the three scenes goes through XenosNative (1,136 at the dock, none
failing) and the frame reaches the screen. Textures are placeholders, so the images show geometry, vertex
colors, fog and post-processing, but not the game's art yet. GPU time at the dock is 10.7 ms per frame at
1280×720 with Antialiasing Off (the options' default; corrected on 2026-10-14, this said 2× MSAA); the
Xenos path needs 8.5–9 ms at four times the pixels (2×2, AA Off; the baseline has no 1×1 run), so
XenosNative is far from its goal there: optimization is Phase 3.

2026-10-14: the Vulkan device was being lost on the first dock frames (out-of-range buffer reads without
`robustBufferAccess`); fixed with XenosNative's first DiligentCore patch (D22). 60 FPS at the dock, renderer
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
   - [x] BC1: DiligentCore's `BC1_UNORM` is Vulkan's BC1 without alpha, so DXT1 is decompressed to RGBA8
         by the SDK's load shader (D23, 2026-10-14). Native BC1 with alpha would need a DiligentCore patch.

   These five points are the integration problems Hell's Gate documented. Credit them in the commit.
2. **SDK graphics code in process.** The SDK's GPU backend is a plugin DLL (`rexgpu-xenosrd.dll`), and its
   translator and texture code are not exported. Either compile the needed SDK graphics sources into
   `xenos_native` (Hell's Gate's choice), or export them through an SDK patch.
   - [x] Decided: compiled into `xenos_native_sdk`, the cvars left to the plugin (D15, 2026-10-13).
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

**Exit:** the three scenes rendered recognizably by XenosNative, the fallback unaffected, and 10 minutes
without a crash. **Decision: continue to parity, or adjust the design.**

**Exit check, 2026-10-14:**
- the language menu, the title screen and the main menu (with their Bink backgrounds) and the Port Royal
  dock (HUD, character shadows) render as on the Xenos path; Gustavo played at the dock;
- the Xenos path is unchanged with `xenos_native` off (`potcawe_084`);
- 12 minutes at the dock, walking and turning the camera every minute (`potcawe_092`): no crash, no error
  in the log, every frame under 20 ms after the load;
- known differences, for Phase 3: the sun glare (fake occlusion query counts), AA forced off, 1280×720
  only, and the GPU time (12 ms facing the town against ~5 ms for the Xenos path; 4 ms facing the sea).

## Phase 3 — Performance and the renderer's own features

Goal: XenosNative faster than the Xenos path at the same settings, with the features that change how it
renders: real occlusion queries, compressed textures, no translation or upload stalls, and any render
resolution.

Agreed order (Gustavo, 2026-10-14): performance first (D29). Then, after a viability analysis, the list
and order below, with POTCAWE's polish moved to Phase 4 and the second game to Phase 5 (D31). Notes:
`local_docs/06-phase3-performance.md`.

- [x] GPU time: find why the town side of the dock costs 12 ms on XenosNative (4 ms facing the sea, ~5 ms
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
        `xenos_native_resolution_scale` override (D38).
  - [x] The launcher shows the resulting resolution with XenosNative in the render quality row, disabled
        ("2560 × 1440 (from the screen)", or "(original)" at 1280×720), computed as the renderer does
        (2026-10-14).
  - [x] Checked at exactly 2× against the Xenos path at 2×2 (closer than at 1×), and at 1.067, 1.25, 1.4,
        1.5 and 1.6 (D38).
- [x] Performance A/B against the Xenos path with `tools/gpu_ab.ps1`, same spot, the baseline's
      configurations, both paths in one session (2026-10-14; [Measured on XenosNative](#measured-on-xenosnative-phase-3)).
      At the same resolution without antialiasing XenosNative uses 38–56 % less of the GPU; at 3× it holds
      60 FPS where the Xenos path with 2× MSAA drops to 33.

**Exit:** the items above done, XenosNative's GPU time below the Xenos path's at the same resolution, and
no frame over 20 ms at the dock after the load.

Exit check (2026-10-14):
- every item above is done;
- the GPU time at the same resolution is 38–56 % lower than the Xenos path's;
- the longest frame at the dock after the load was 18.4–19.0 ms in every run (the A/B runs' frame
  intervals, the three-minute walk of D35).

**Closed on 2026-10-14 by Gustavo (D39).**

## Phase 4 — POTCAWE polish

Goal: play POTCAWE on XenosNative with no visible difference from the Xenos path, everywhere (D31).

Started on 2026-10-14 (D39). First, in this order (D40): antialiasing, the pipeline cache, aspect ratios.

- [x] Antialiasing of XenosNative's own: SMAA (default) or FXAA, applied to the copy of the finished scene
      before the HUD is composited, which the game's profile names (`scene_resolve_caller`). The launcher's
      Antialiasing row offers SMAA / FXAA / Off with XenosNative; the game's 2× MSAA and the tile hooks of
      `src/msaa.cpp` stay for the Xenos path (D41, 2026-10-14).
- [ ] Every level Gustavo can reach, compared with the Xenos path (screenshots at the same spots). What
      the dock already shows working (2026-10-14) is checked again on the way:
      - shadow maps (1024², pitch 1040), including the clears that alias them;
      - color and depth resolves;
      - full-screen passes, transparent objects, the bloom and luminance chain, the composite;
      - HUD, menus, loading screens, subtitles;
      - videos (Bink).
- [x] The display gamma ramp (found while checking D33). Present maps the front buffer through the
      guest's ramp, as the SDK's presenters do (SDK patch 0004). The tone curve now matches the Xenos path.
      Done early, during Phase 3, at Gustavo's request (D36, 2026-10-14).
- [ ] Known limits from Phase 2, when a scene needs them: partial clears, signed formats with separate data,
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
    - **Messages:** no warning from XenosNative, and no DiligentCore message after the start.
- [x] Any aspect ratio without bars: ultrawide, 16:10, the Steam Deck. The renderer fills the output with
      a scale per axis, POTCAWE's hooks widen its 16:9 cameras (Hor+ or Vert+, culling included), and the
      2D layers (HUD, menus, videos) are drawn in a centered 16:9 box (D43, 2026-10-14). Checked at 21:9,
      16:10 and 16:9 in the menus, the dock and the pause menu. Not yet: anchoring HUD elements to the
      screen's edges, other levels and the cutscenes.
- [x] Anisotropic filtering: XenosNative applies the SDK's `anisotropic_override` as the Xenos path does
      (it used only what the game asks for, plain trilinear on most of the dock). POTCAWE defaults to 16x
      on both paths, with a launcher row, at no measured cost (D44, 2026-10-14).
- [x] 60 FPS option (works with XenosNative, 2026-10-14).
- [x] Launcher row "Renderer: Xenos (emulated) / XenosNative (experimental)", added early in Phase 2 for
      testing (D25, 2026-10-14).
- [x] Launcher rows "Image format" (fill a wide screen, or 16:9 with bars) and "Internal resolution"
      (automatic, or a lower height in that format, in fullscreen and in a window) for XenosNative; the
      DLSS modes out of the antialiasing row; Save keeps the window open (D53, D54, 2026-10-15).

**Exit:** Gustavo plays the agreed levels without differences. **Decision: make XenosNative the default
for POTCAWE, or keep it optional.**

## Phase 5 — Temporal upscaling and frame generation

Goal: the temporal techniques modern PC games get, on a game that never had them (D40).

- [x] Motion vectors: camera motion from depth and the current and previous camera matrices, then moving
      objects (a draw matched with the previous frame's, its vertex shader run with the previous
      constants). Both done for DLAA (D47, D48, 2026-10-14).
- [x] Jitter of the scene's draws, the upscaler before the post-processing and the HUD. The jitter is a
      sub-pixel offset of the main 3D view's viewport rather than of each draw's NDC (D47); DLSS runs where
      the view's post-processing starts (D47, D49).
- [ ] DLSS / DLAA (DiligentCore's SuperResolution module has DLSS on Vulkan) and FSR 3, as quality options.
      Started on 2026-10-14 (D46), before Phase 4 closes, in this order:
  - [x] DLSS found and reported: DiligentCore's SuperResolution module with NVIDIA's NGX SDK, the runtime
        next to the executable; `xenos_native_antialiasing = dlaa` reports it (D46).
  - [x] Jitter of the main 3D view, through its draws' viewport (D47).
  - [x] Motion vectors from depth and the camera's matrices, with the view's depth range: POTCAWE's depth
        is reversed (D47).
  - [x] DLAA before the game's post-processing: `xenos_native_antialiasing = dlaa`, a launcher choice;
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
- [ ] Frame generation, for more than 60 FPS without changing the game's timing. The HUD is drawn in a
      surface of its own, which frame generation needs apart.

## Phase 6 — Second game and extraction

- [ ] GRAW (XDK 2.0.2417, the oldest XDK of our games so far): map its D3D layer with
      [`integrating-a-game.md`](integrating-a-game.md), write its profile, observer at 100 %, first frames.
- [ ] Update the core for whatever turned out to be POTCAWE-specific.
- [ ] `git subtree split --prefix=xenos_native` into its own repository. The games then use it as a
      submodule, and its SDK patches go with it.
- [ ] Kameo (XDK 2.0.2099) after that.

---

## Baseline to beat

### Measured with this build (2026-10-13)

The Xenos path of this build, in the state XenosNative starts from:
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

| Kind | Replay ms | Removed by XenosNative? |
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

Phase 3 replaces these estimates with the same nine runs on XenosNative.

### Measured on XenosNative (Phase 3)

2026-10-14, build `55b01f9` (D30–D38). The same spot and method as the baseline: `tools/gpu_ab.ps1`, 20 s
of `nvidia-smi` per configuration, in a 1280×720 window. Both paths ran in one session, alternating, so
that the machine's state is the same for both. Details: `local_docs/06-phase3-performance.md`.
- **Renderers.** The Xenos path on D3D12. XenosNative at a forced render scale
  (`xenos_native_resolution_scale`) equal to the Xenos path's whole scale.
- **No MSAA on XenosNative** (D31): its rows compare with the Xenos path's "Off" rows. The MSAA rows show
  what the game's original setting costs.
- **Extra columns.** The GPU clock and board power from `nvidia-smi`. For XenosNative, its own GPU time
  per frame (duration queries around each frame, 600-frame average).
- **The GPU stays at 2745–2880 MHz in every run,** so the utilization is not inflated by a lower clock.

| Scale | Target | Path | GPU % (min–max) | Power | FPS (RTSS) | GPU per frame ≈ (util. × interval) | XenosNative's own GPU time |
|---|---|---|---|---|---|---|---|
| 1× | 60 FPS | Xenos, AA Off | 28.9 (27–31) | 55 W | 60 | 4.8 ms | |
| 1× | 60 FPS | XenosNative | **17.8** (17–19) | 50 W | 60 | 3.0 ms | 1.69 ms |
| 2× | 60 FPS | Xenos, 2× MSAA | 81.4 (79–84) | 145 W | 60 | 13.6 ms | |
| 2× | 60 FPS | Xenos, AA Off | 51.3 (50–53); repeat 51.6 | 106 W | 60 | 8.6 ms | |
| 2× | 60 FPS | XenosNative | **26.8** (25–29); repeat 27.4 | 68 W | 60 | 4.5 ms | 3.30 ms |
| 3× | 60 FPS | Xenos, 2× MSAA | 93.2 (91–95) | 175 W | **33** / 30.3 ms | 28.2 ms | |
| 3× | 60 FPS | Xenos, AA Off | 96.9 (96–97) | 181 W | 59 / 16.7 ms | at the limit | |
| 3× | 60 FPS | XenosNative | **42.6** (41–44) | 118 W | **60** / 15.7 ms | 7.1 ms | 5.85 ms |
| 2× | 30 FPS | Xenos, 2× MSAA | 41.3 (39–44) | 99 W | 30 | 13.8 ms | |
| 2× | 30 FPS | Xenos, AA Off | 28.3 (26–30) | 66 W | 30 | 9.4 ms | |
| 2× | 30 FPS | XenosNative | **15.8** (14–17) | 55 W | 30 | 5.3 ms | 3.34 ms |
| 3× | 30 FPS | Xenos, 2× MSAA | 84.5 (81–86) | 162 W | 30 / 33.7 ms | 28.2 ms | |
| 3× | 30 FPS | Xenos, AA Off | 50.8 (47–53) | 116 W | 30 | 16.9 ms | |
| 3× | 30 FPS | XenosNative | **24.4** (23–25) | 70 W | 30 | 8.1 ms | 6.08 ms |

- **Without antialiasing on both sides,** XenosNative uses 38 % less of the GPU at 1×, 44–48 % less at
  2× and 52–56 % less at 3×. The gain grows with resolution.
- **The plan's estimate is beaten by far.** It was 2×2 Off from ~8.5–9 ms to ~6.5–7 ms, about −20–25 %.
  Measured: 8.6 → 4.5 ms by the same utilization method (−48 %), and 3.3 ms by XenosNative's own
  timing.
- **Against the original 2× MSAA,** XenosNative without antialiasing uses a third of the GPU, but that is
  not like for like until XenosNative has its own antialiasing (Phase 4).
- **The utilization estimate for XenosNative is above its own measurement** (4.5 against 3.3 ms at 2×).
  The utilization also counts what is not in XenosNative's command buffers: presentation, the desktop
  compositor, the overlay. That share is in the Xenos path's figures as well, so the comparison by
  utilization is like for like.
- **Repeats** differ by 0.3 (Xenos) and 0.6 (XenosNative) points, and the Xenos path's figures are within
  1–2 points of the 2026-10-13 baseline: the noise is small.

## Test protocol

- **Automated first.** `tools/game.ps1` and `tools/level2.ps1` drive the game to the Port Royal dock with
  the keyboard as the controller, and take screenshots and RenderDoc captures. `tools/gpu_ab.ps1` runs one
  configuration per line and samples `nvidia-smi`. Measure at 30 FPS when comparing GPU load: at 60 FPS
  the cap hides gains.
- **Same spot, both renderers.** Every visual check is a pair of screenshots: Xenos path and XenosNative,
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
| The SDK's SPIR-V translator always builds `loop { switch (pc) }`, the structure that produced the flat-colored objects in Port Royal on D3D12 (fixed there with `dxbc_switch = false`, which only affects DXBC). | The same corruption on Vulkan under load. | Check Port Royal at 2×2/60 FPS early in Phase 2. If it appears, add an "if blocks" mode to the SPIR-V translator (an SDK patch, like the DXBC one). |
| The engine bypasses the D3D library (direct PM4 writes, precompiled command buffers, inlined setters). | Draws or state XenosNative never sees. | **Closed for POTCAWE (Phase 1):** every draw packet comes from a hooked call; inlined setters are covered by the shadow; the observer rebuilds 100 % of the draws. Recheck per game. |
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
  other projects. SDK patches that only XenosNative needs live with the library (D10).
