# XenosNative — work plan

First game: Pirates of the Caribbean: At World's End (POTCAWE), in `POTCAWERecompVulkan`. Started
2026-10-13. Estimates are in working days, at the pace of the POTCAWERecomp work so far. Tick the boxes as
tasks are done, and add the date and a pointer to the notes that close each one.

| Phase | Goal | Estimate | Status |
|---|---|---|---|
| 0 | Scaffolding: project, build, documents, baseline | 0.5–1 day | done (2026-10-13) |
| 1 | Map the game's D3D layer and verify that every draw can be rebuilt from it | 1–2 days | not started |
| 2 | Core renderer: first frames drawn by XenosNative | 4–6 days | not started |
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

1. **Entry points.** Identify the roughly 45 functions listed in
   [`xdk-d3d-reference.md`](xdk-d3d-reference.md#entry-points). The ones already known for POTCAWE are in
   [`xenos-native-potcawe.md`](../../docs/xenos-native-potcawe.md#known-d3d-functions). Methods are in
   [`xdk-d3d-reference.md`](xdk-d3d-reference.md#finding-the-entry-points-in-a-new-executable).
   - [ ] Draws: DrawIndexedVertices, DrawVertices, BeginVertices/EndVertices, DrawVerticesUP (and
         DrawIndexedVerticesUP if present).
   - [ ] Clear, Resolve, BeginTiling, EndTiling, SetPredication, SetRenderTarget, SetDepthStencilSurface.
   - [ ] SetVertexShader, SetPixelShader, CreateVertexDeclaration, SetVertexDeclaration, SetStreamSource,
         SetIndices, SetTexture, the sampler state setters, SetVertexShaderConstantF,
         SetPixelShaderConstantF, any constant buffer loads.
   - [ ] The function that flushes dirty state into the command buffer before a draw.
   - [ ] Command buffer reservation (segment switches), InsertFence, BlockOnFence.
   - [x] Swap, Present, SetRenderState_PresentInterval, query Issue / GetData, the GPU wait helpers,
         XGSetSurfaceHeader (already found while working on POTCAWERecomp).
   - [ ] Render state setters, by register, as far as the game calls them as functions. Inlined setters
         are covered by reading the shadow at draw time (step 2).
2. **Device layout** for XDK 2.0.5426: the offsets of:
   - [ ] the register shadow;
   - [ ] the vertex and pixel float constants;
   - [ ] the fetch constants (textures and vertex streams);
   - [ ] the bool/loop constants;
   - [ ] the current shaders, vertex declaration, render targets and depth surface;
   - [ ] the dirty masks.

   Write them into the profile table described in [`architecture.md`](architecture.md#xdk-profile).
3. **Trace mode.** Hooks on every entry point, logging calls per frame (counts and arguments for one
   frame on a key press). Tracing is a feature of the library (cvar `xenos_native_trace`, off by default),
   because every new game goes through this step.
   - [ ] Hook file `src/xenos_native_potcawe.cpp` (game side) with one line per entry point.
   - [ ] One traced frame each of: title screen, main menu, Port Royal dock (`tools/level2.ps1`).
4. **Observer.** At each draw, rebuild the draw's GPU state from the device structure and compare it with
   the register file the SDK's command processor has when it executes that draw. The comparison needs a
   generic draw callback in the SDK: a small patch, off and free when unset (see
   [`decisions.md`](decisions.md), D10).
   - [ ] 100 % of the draws match over one frame of each scene above. Hell's Gate reached 975/975 on
         Dante's Inferno.
5. **Open questions.** Answer each with evidence:
   - [ ] Predicated tiling. The occlusion query work showed that the command buffer is replayed per tile
         with bin-predicated constants. Confirm that the CPU calls each draw once per frame, and list what
         differs per tile: constants, resolves, queries.
   - [ ] The 19 EDRAM clears per frame: are they D3D Clear calls, or engine draws through an aliased
         surface?
   - [ ] Does the engine write PM4 packets directly, run precompiled command buffers, or patch the
         command buffer after the fact?
   - [ ] Memexport: does any shader use it?
   - [ ] Does the CPU read any resolve result (screenshots, save thumbnails, luminance read back)?
   - [ ] How are the videos drawn (Bink: textures plus a YUV shader through the same D3D calls)?

**Exit:** all entry points mapped (or shown unused), the observer at 100 % in the three scenes, and the
open questions answered in [`xenos-native-potcawe.md`](../../docs/xenos-native-potcawe.md).
**Decision: go or no-go for Phase 2.**

## Phase 2 — Core renderer

Goal: XenosNative draws the title screen, the main menu and a Port Royal frame. The images do not need to
be perfect yet.

1. **DiligentCore.**
   - [ ] Submodule `thirdparty/diligent-core`, pinned to a recent master commit. The last release, v2.5.6,
         is from 2024-09, and Hell's Gate builds master.
   - [ ] CMake: `DILIGENT_NO_GLSLANG` and `DILIGENT_NO_HLSL`, because their glslang/SPIRV targets collide
         with the SDK's.
   - [ ] No AVX2 build flags.
   - [ ] VSync off by default in the swap chain.
   - [ ] BC1 mapped to the RGBA (1-bit alpha) Vulkan formats.

   These five points are the integration problems Hell's Gate documented. Credit them in the commit.
2. **SDK graphics code in process.** The SDK's GPU backend is a plugin DLL (`rexgpu-xenosrd.dll`), and its
   translator and texture code are not exported. Either compile the needed SDK graphics sources into
   `xenos_native` (Hell's Gate's choice), or export them through an SDK patch.
   - [ ] Decide and record it in [`decisions.md`](decisions.md).
3. **Command processor sync-only mode** (SDK patch, see D10): packets are still executed for fences,
   interrupts, waits and queries, but nothing is drawn on the host.
   - [ ] Selected by the same cvar as the renderer.
4. **Device and presentation.**
   - [ ] A Diligent Vulkan device on the game window.
   - [ ] Decide how it coexists with the SDK presenter and its ImGui overlay: its own swap chain, or
         drawing into the SDK presenter's image.
5. **Shaders and pipelines.**
   - [ ] Xenos microcode → SPIR-V with the SDK's `SpirvShaderTranslator`.
   - [ ] Fixed binding contract: shared memory, constants, textures and samplers.
   - [ ] Pipeline cache keyed by shader hashes and state.
   - [ ] Pipelines created off the render thread, so new pipelines do not stall the frame (the lesson of
         `gpu_async_pipelines_no_frame_wait`).
6. **Buffers.**
   - [ ] Index and vertex data uploaded with endian swap.
   - [ ] Dynamic buffers (lock/unlock) uploaded on unlock.
   - [ ] Unchanged ranges skipped by hash.
7. **Textures.**
   - [ ] Fetch constants → host textures, through the SDK's texture loaders (untiling, endianness, formats,
         packed mips).
   - [ ] Invalidated on CPU writes.
8. **Render targets and resolves.**
   - [ ] Host render targets keyed by the guest surface, at the render resolution.
   - [ ] Clears.
   - [ ] A resolve registers its host texture at the destination address. Guest memory is written only if
         the CPU reads it.
9. **Swap.**
   - [ ] Present at Swap.
   - [ ] Keep the existing 60 FPS hook (`src/framerate.cpp`) and the GPU wait hooks (`src/gpu_wait.cpp`)
         working: the SDK still runs the vblank and swap callbacks.

**Exit:** the three scenes rendered recognizably by XenosNative, the fallback unaffected, and 10 minutes
without a crash. **Decision: continue to parity, or adjust the design.**

## Phase 3 — Parity in POTCAWE

Goal: play POTCAWE on XenosNative with no visible difference from the Xenos path, and measure the gain.

- [ ] Shadow maps (1024², pitch 1040), including the clears that alias them.
- [ ] Scene with 2× MSAA drawn once: the two tiles collapsed into one pass, and the per-tile resolves
      merged.
- [ ] Color resolves (2× MSAA averaged to RGBA16F) and depth resolves.
- [ ] Full-screen passes, transparent objects, the bloom and luminance chain, the composite.
- [ ] HUD, menus, loading screens, subtitles.
- [ ] Occlusion queries (the sun flare): host queries, with the expected sample count matching what the
      game computes. See `PotcaweFlareAaLevelHook` in `src/msaa.cpp`.
- [ ] Videos (Bink).
- [ ] Every level Gustavo can reach, compared with the Xenos path (screenshots at the same spots).
- [ ] 60 FPS option, Antialiasing option. With one pass, the tile hooks of `src/msaa.cpp` may no longer be
      needed.
- [ ] Render resolution equal to the window, not a whole multiple of 1280×720.
- [ ] Launcher row "Renderer: Emulated (Xenos) / Native (XenosNative)".
- [ ] Performance A/B against the Xenos path with `tools/gpu_ab.ps1`, same spots, same configurations as
      the baseline.

**Exit:** Gustavo plays the agreed levels without differences, and the GPU time is at or below the
estimate. **Decision: make Native the default for POTCAWE, or keep it optional.**

## Phase 4 — Second game and extraction

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
| The engine bypasses the D3D library (direct PM4 writes, precompiled command buffers, inlined setters). | Draws or state XenosNative never sees. | Read the state from the device shadow at draw time, which covers inlined setters. The observer finds the rest in Phase 1, before any rendering work. |
| Predicated tiling hides per-tile differences. | Wrong constants or resolves when two tiles are collapsed into one pass. | Phase 1 lists what differs per tile. Treat BeginTiling…EndTiling as one pass. |
| Occlusion queries per tile (the sun flare). | Wrong glare strength. | Host queries; compare with the Xenos path (patch 0020) at the same spot. |
| The CPU reads resolve results, or shaders use memexport. | Missing data in guest memory. | A readback path, used only when a CPU read of that range is detected. |
| Per-draw work on the game's render thread. | CPU-bound frames. | Measure with `tools/threadcpu.ps1`. Move recording to a worker if needed. |
| DiligentCore build integration (target name collisions with the SDK, AVX2, VSync, BC1). | Build or runtime problems. | Known fixes, documented by Hell's Gate (Phase 2, step 1). |
| SDK presenter and ImGui overlay versus a second swap chain. | Overlay missing, or present conflicts. | Decide in Phase 2, step 4. The overlay can stay on the Xenos path at first. |
| Parity long tail. | Phase 3 takes longer than estimated. | The Xenos path stays as the fallback, and Native stays optional until parity is reached. |

## Keeping in sync

- **POTCAWE fixes:** `git fetch potcawe`, then merge `potcawe/main` (or the branch that has the fix). Game
  fixes keep being made in POTCAWERecomp first.
- **The shared SDK series** (`patches/`, shared with GRAWRecomp and KameoRecomp) stays identical to the
  other projects. SDK patches that only XenosNative needs live with the library (D10).
