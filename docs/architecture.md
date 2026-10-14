# XenosNative — architecture

Status: design, 2026-10-13. Sections marked **(to confirm in Phase N)** describe the intended design before
the evidence that will settle it. Update this document when a phase confirms or changes them.

## Contents

1. [The problem](#the-problem)
2. [The approach](#the-approach)
3. [A draw, step by step](#a-draw-step-by-step)
4. [Components](#components)
5. [XDK profile](#xdk-profile)
6. [Game integration](#game-integration)
7. [Threading](#threading)
8. [Resolution model](#resolution-model)
9. [Selection and fallback](#selection-and-fallback)
10. [The hard parts](#the-hard-parts)
11. [Expected gains](#expected-gains)
12. [Non-goals for now](#non-goals-for-now)
13. [Glossary](#glossary)

## The problem

On the Xbox 360, the game's Direct3D library (from the XDK, linked into the executable) turns API calls into
PM4 packets in a ring buffer. The Xenos GPU executes them. Rendering goes into 10 MB of EDRAM, and
*resolves* copy EDRAM into textures in main memory.

ReXGlue's Xenos backend (from Xenia) emulates that hardware on D3D12 or Vulkan. That is general, because it
works for any game without knowing anything about it, but it pays for hardware details that a PC does not
have:

- **EDRAM aliasing.** Two surfaces with different pitch or MSAA can occupy the same EDRAM. The emulator
  keeps one host render target per configuration and copies between them whenever the game switches
  ("ownership transfers"). POTCAWE clears its scene 19 times per frame through such an alias. SDK patches
  0017 and 0018 removed most of that cost, but the mechanism is still there.
- **Resolves.** A resolve is emulated as a dump of the host render target into an EDRAM buffer, then a copy
  into guest memory (the "shared memory" buffer). Every texture that samples the result is then reloaded
  from guest memory. POTCAWE does about 40 resolves and about 55 such texture reloads per frame.
- **Tiling.** When a render target does not fit in 10 MB, the game draws the scene once per tile
  (predicated tiling). POTCAWE at 1280×720 with 2× MSAA draws its 962 scene draws twice. The emulator
  replays both passes.
- **Resolution.** The EDRAM layout is emulated, so the host resolution is the game's times a whole number
  (2×, 3×).

## The approach

Replace the game's D3D layer, not the GPU. A recompiled game calls the D3D library through ordinary
functions (`sub_XXXXXXXX`), and the SDK makes every generated function overridable: `sub_X` is a weak alias
of `__imp__sub_X`. A strong definition elsewhere (`REX_HOOK_RAW(sub_X)`) replaces it for direct calls and
for the indirect-call table, and `__imp__sub_X` stays callable as the original body. XenosNative hooks the
D3D entry points and draws on the host with what it learns there.

This is the model of [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp), which replaced
Sonic Unleashed's D3D layer entirely, with no Xenos emulation at all. It is also the model of
[Hell's Gate](https://github.com/florinp93/hells-gate-recomp), which did it on ReXGlue while keeping the
emulator. XenosNative follows Hell's Gate's hybrid, for three reasons:

1. **The original D3D body keeps running.** The game's command buffer is written exactly as on the console,
   so every side effect the game relies on still happens: fences, swap and vblank callbacks, the D3D
   library's waits, query markers.
2. **The SDK command processor still executes the ring**, but in a *sync-only* mode that skips host
   rendering. Synchronization stays emulated and correct, and only drawing is replaced.
3. **The Xenos path stays one switch away**, as the fallback and as the reference for comparisons.

Removing the command processor entirely, as Unleashed did and as Hell's Gate plans for frame rates above
60 FPS, is a possible later step, not a goal.

## A draw, step by step

1. The game calls, for example, `DrawIndexedVertices(device, type, base, start, count)`, an XDK function at
   a game-specific address.
2. The hook (game side, one line in the game's hook file) saves the arguments and calls the original
   `__imp__sub_X`. The D3D library flushes dirty state and writes the draw packets into the ring.
3. After the original returns, the hook calls `xenos_native::OnDraw(kind, args)`.
4. XenosNative reads the device structure through the XDK profile: register shadow, float/bool/loop
   constants, fetch constants, current vertex and pixel shaders, render targets and depth surface. This is
   the complete GPU state of the draw. Reading the shadow at draw time also covers state the game sets with
   inlined setters, which never call a function.
5. It looks up or creates:
   - the SPIR-V shaders, from the SDK's translator, keyed by microcode hash;
   - the pipeline, keyed by shaders and state;
   - the textures, from the fetch constants, resolve-registry first;
   - the render targets, from the guest surfaces.
6. It uploads what changed (constants, vertex and index ranges) and records the draw on the DiligentCore
   immediate context.
7. Later, the SDK command processor reaches the same packets in the ring. In sync-only mode it updates its
   register file and handles fences and events, but it does not draw.

Clears, resolves and Swap work the same way:
- **Clear** clears the host render target.
- **Resolve** registers the host render target, or an MSAA-resolved copy of it, as the texture at the
  destination address.
- **Swap** presents.

## Components

All of these live in `xenos_native/` and contain no game-specific code.

| Component | Responsibility | Built on |
|---|---|---|
| Hook interface | Entry points the game's hook file calls (`OnDraw`, `OnClear`, `OnResolve`, `OnSwap`, `OnSetRenderTarget`…) plus pass-through when XenosNative is off. | `REX_HOOK_RAW`, `__imp__` originals |
| Device state reader | Reads the D3D device structure of the current XDK version from guest memory (big-endian). | XDK profile |
| Shader cache | Microcode from the shader object, hashed; SPIR-V from the SDK's `SpirvShaderTranslator`; Diligent shader objects. | SDK translator |
| Pipeline cache | Pipelines keyed by shader pair, primitive type, render target formats, MSAA, blend/depth/stencil/raster state. Created on worker threads. | DiligentCore |
| Guest memory mirror | The GPU-side copy of guest memory that the translated shaders fetch vertices from, kept current for the ranges draws use. Index data uploaded with endian swap. | Diligent buffers |
| Texture cache | Host textures from fetch constants: address, format, size, tiling, mips. Invalidated when the CPU writes their pages. | SDK texture loaders |
| Render target cache | Host render targets keyed by the guest surface (EDRAM base, pitch, format, MSAA) at the render resolution. No EDRAM. | DiligentCore |
| Resolve registry | Guest address range → host texture produced by a resolve. Texture lookups hit it first. A CPU read of a registered range triggers a readback. | — |
| Presenter | Present at Swap, scaling to the window. Coexistence with the SDK presenter is decided in Phase 2. | DiligentCore swap chain |
| Sync-only command processor | Executes the ring for synchronization and skips host rendering. | SDK patch (D10) |
| Trace and observer | Per-frame call trace; per-draw comparison of the rebuilt state with the command processor's register file. Used to bring up every new game. | SDK draw callback (D10) |

### Binding contract (to confirm in Phase 2)

The SDK's SPIR-V translator expects certain resources (guest memory as storage buffers, constant buffers,
textures and samplers) in descriptor sets laid out by the SDK's Vulkan backend. XenosNative binds the
same layout through DiligentCore's named resource binding, so the translator's output is used unchanged.
Hell's Gate used: guest memory in set 0, constants in set 1, textures and samplers in sets 2 and 3.

### SDK graphics code (to confirm in Phase 2)

The SDK builds its GPU backend as a plugin DLL (`rexgpu-xenos*.dll`). The translator, the texture
loaders and the register definitions are not exported from it. Hell's Gate compiles the needed SDK graphics
sources into its own static library. The alternative is an SDK patch that exports them. Decide in
Phase 2, step 2.

## XDK profile

Everything that depends on the executable or on the XDK version, gathered in one place per game:

- **Entry points**: the address of each hooked D3D function. These have to be compile-time symbols
  (`sub_822B58B0`), so the profile is a C++ file of one macro line per function, not a runtime table:

  ```cpp
  // src/xenos_native_potcawe.cpp (game side) — sketch, to confirm in Phase 1
  XENOS_NATIVE_HOOK_DRAW_INDEXED(8229XXXX)
  XENOS_NATIVE_HOOK_RESOLVE(822BXXXX)
  XENOS_NATIVE_HOOK_SWAP(822B58B0)
  ```
- **Device layout**: offsets in the D3D device structure. These are runtime data passed to the core at
  startup:
  - the register shadow and its dirty masks;
  - the float constants (vertex and pixel), fetch constants, and bool/loop constants;
  - the current shaders, vertex declaration, render targets and depth surface;
  - the present interval;
  - the ring write pointer.

  Games built with the same XDK version should share these values. Games with different versions need
  their own (POTCAWE 5426, GRAW 2417 and Kameo 2099 are three different versions).
- **Object layouts**: where the shader, texture, surface and vertex buffer objects keep their microcode
  pointer, fetch constant and header. These are runtime data too.

## Game integration

What only one game or engine does lives in the game's repository, next to its other hooks:

- **Engine tricks:** POTCAWE clears EDRAM through a surface with another pitch and MSAA, which is an alias
  that only exists because of EDRAM.
- **Behavior that depends on the console's limits:** POTCAWE computes its tile count from the size of EDRAM
  (`src/msaa.cpp`).
- **Things the engine computes from GPU results:** the sun flare's expected occlusion sample count.
- **Videos,** if the engine draws them outside the usual path.

The core offers callbacks for these (for example, a hook before a clear, or a surface-alias policy) once
Phase 3 shows which ones are needed. They are not designed up front.

## Threading

- The game's render thread calls the D3D library, so the hooks and all recording run there, on
  DiligentCore's immediate context. That is the same thread order as the game's calls, with no locking in
  the common path.
- Pipeline creation runs on worker threads. A draw whose pipeline is not ready yet is skipped for that
  frame instead of stalling it, which is the lesson of the SDK's `gpu_async_pipelines_no_frame_wait`.
- The SDK command processor keeps its own thread and executes the ring in sync-only mode.
- Present happens at Swap, on the render thread.

If the per-draw work turns out to bound the frame on the CPU (to measure in Phase 3 with
`tools/threadcpu.ps1`), recording can move to a worker that consumes a queue of draw snapshots.

## Resolution model

- The game believes it renders at its own size (POTCAWE: 1280×720, and 1024² shadow maps).
- XenosNative creates each host render target at `guest size × scale`. It scales:
  - viewports, scissors and resolve rectangles;
  - the shader constants the SDK translator uses for pixel-position math.
- **Phase 2** uses whole-number scales, like the SDK, which keeps half-pixel offsets and texel-exact
  post-processing correct.
- **Phase 3** adds a scale chosen from the window (for example 3440×1440 → 2560×1440 for the 16:9 image),
  which is not a whole number. Post-processing that samples by texel offsets has to be checked then.
- Wider aspect ratios also need the game's projection changed. That is per game (like GRAW's
  `src/widescreen.cpp`), not part of the renderer.

## Selection and fallback

- An init-only cvar, `xenos_native` (`false` by default during development), selects the renderer at
  startup.
- When it is off, every hook is a pure pass-through to `__imp__sub_X`, the command processor draws as
  usual, and nothing of XenosNative is initialized.
- Per game, the launcher gets a "Renderer" row once Phase 3 reaches parity.
- Switching needs a restart.

## The hard parts

| Problem | Approach |
|---|---|
| Predicated tiling (POTCAWE: 2 tiles with 2× MSAA) | The CPU issues each draw once and the D3D library replays the command buffer per tile with bin predication. XenosNative draws once at full size, and merges the per-tile resolves into one. Phase 1 lists what differs per tile. |
| Surfaces aliasing the same EDRAM | No EDRAM. A clear through an alias clears the target that holds that memory. Other aliases are handled when they appear, and logged. |
| Resolves to the same address at different sizes within a frame (seen by Hell's Gate) | The registry is keyed by address plus size and format, and the newest entry shadows older ones. That is also what patch 0019 found for POTCAWE's bloom buffer. |
| Gamma and float render target formats (8_8_8_8_GAMMA, 7e3 floats) | Use the SDK translator's existing handling, and match the formats the SDK's Vulkan backend picks. |
| Shader control flow on NVIDIA (`loop { switch }`) | See the plan's risks: an "if blocks" mode in the SPIR-V translator if Port Royal shows the flat-color corruption. |
| Occlusion queries | Host queries begun and ended at the D3D library's Issue calls, results written where the library expects them. The Xenos path's patch 0020 documents the layout. |
| CPU reads of GPU results, memexport | A readback when a registered range is read. Memexport only if Phase 1 finds it in use. |
| ImGui overlay and SDK presenter | Decided in Phase 2. The overlay may stay Xenos-only at first. |

## Expected gains

From POTCAWE's per-event timing at 2×2 with Antialiasing Off (one tile, no MSAA), measured in POTCAWERecomp
before `clear_memory_page_state = false`:
- the real frame was about 9.8 ms of GPU time;
- RenderDoc's replay shows 17.2 ms, of which about 5.5 ms are emulation work: transfers ~1.0, resolves
  ~1.5, resolve texture loads ~1.0, uploads ~1.0, clears ~0.4.

Replay inflates times by about 1.75× (a floor of ~3 µs per event), so those 5.5 ms are about 3 ms of real
GPU time. Since then, `clear_memory_page_state = false` has removed about 1 ms of uploads. XenosNative
removes most of what remains, but keeps the uploads it still needs and adds a small amount of work of its
own.

Applied to the baseline measured in Phase 0 ([plan](plan.md#baseline-to-beat), 2026-10-13):
- **2×2, Antialiasing Off:** ~8.5–9 → ~6.5–7 ms (about −20–25 %).
- **2×2, 2× MSAA (original look):** ~14 ms today (82.6 % at 60 FPS). The scene is drawn once instead of
  twice (about 4 ms of real GPU time at 2×2, from the replay difference between two tiles and one), plus the
  savings above: ~9–9.5 ms (about −30–35 %).
- **3×3, 2× MSAA:** 33 FPS today → about 50–55 FPS.
- **1×1:** no visible difference. The game already has headroom there.

The game's own draws are the floor. They run the same translated shaders on both paths. These numbers are
estimates from replay timings, and Phase 3 replaces them with A/B measurements.

## Non-goals for now

- **A D3D12 backend.** DiligentCore supports one, but the SDK's translator for D3D12 produces DXBC with its
  own root signature. Revisit after Phase 4.
- **Linux or macOS.** Vulkan through DiligentCore keeps Linux possible, but the game projects are Windows
  only today.
- **Our own shader translator,** or offline shader compilation as in Unleashed. The SDK translator is
  proven on these games.
- **Removing the command processor.**
- **Changing how the game looks,** beyond resolution and antialiasing. Hell's Gate's SMAA and higher shadow
  resolution are ideas for after parity.

## Glossary

| Term | Meaning |
|---|---|
| XDK D3D library | Microsoft's Direct3D implementation for the Xbox 360, statically linked into each game. |
| Device structure | The D3D device object in guest memory: register shadow, constants, current resources, ring pointers. |
| PM4 | The packet format of the Xenos command ring (type-0 register writes, type-3 commands). |
| EDRAM | The Xenos GPU's 10 MB of embedded render memory; render targets live there. |
| Resolve | Copying a render target from EDRAM into a texture in main memory. |
| Predicated tiling | Rendering a target larger than EDRAM in tiles, replaying the command buffer per tile with bin predication. |
| Fetch constant | A 6-dword (texture) or 2-dword (vertex) descriptor the shaders read resources through. |
| Shared memory | The SDK's GPU-side mirror of guest physical memory. |
| Sync-only mode | The SDK command processor executing the ring without drawing (fences, events, queries). |
| Xenos path | ReXGlue's GPU emulation, the fallback. |
