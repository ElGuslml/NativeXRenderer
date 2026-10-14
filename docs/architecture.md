# XenosNative — architecture

Status: design, 2026-10-13; Phase 1 settled the XDK profile, the hook interface and the rebuild of a
draw's state (2026-10-13). Sections marked **(to confirm in Phase N)** describe the intended design before
the evidence that will settle it. Update this document when a phase confirms or changes them.

## Contents

1. [The problem](#the-problem)
2. [The approach](#the-approach)
3. [A draw, step by step](#a-draw-step-by-step)
4. [Rebuilding a draw's GPU state](#rebuilding-a-draws-gpu-state)
5. [Draws, clears, resolves and Swap](#draws-clears-resolves-and-swap)
6. [Components](#components)
7. [XDK profile](#xdk-profile)
8. [Bring-up tools: trace and observer](#bring-up-tools-trace-and-observer)
9. [Game integration](#game-integration)
10. [Threading](#threading)
11. [Resolution model](#resolution-model)
12. [Selection and fallback](#selection-and-fallback)
13. [The hard parts](#the-hard-parts)
14. [Opportunities found in Phase 1](#opportunities-found-in-phase-1)
15. [Expected gains](#expected-gains)
16. [Non-goals for now](#non-goals-for-now)
17. [Glossary](#glossary)

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
2. The hook (game side, one line in the game's hook file) saves the arguments and the dirty masks, and
   calls the original `__imp__sub_X`. The D3D library flushes dirty state and writes the draw packets into
   the ring.
3. After the original returns, the hook calls into XenosNative with the call, its arguments and the range of
   the command buffer it wrote.
4. XenosNative rebuilds the draw's complete GPU state (next section): the device structure through the
   XDK profile (register shadow, constants, fetch constants, current shaders and surfaces), plus the few
   things the D3D library writes only into the command buffer, read from the packets this call wrote.
   Reading the shadow at draw time also covers state the game sets with inlined setters, which never call
   a function.
5. It looks up or creates:
   - the SPIR-V shaders, from the SDK's translator, keyed by microcode hash and modification;
   - the pipeline, keyed by shaders and state;
   - the textures, from the fetch constants, resolve-registry first;
   - the render targets, from the guest surfaces.
6. It uploads what changed (constants, vertex and index ranges) and records the draw on the DiligentCore
   immediate context.
7. Later, the SDK command processor reaches the same packets in the ring. In sync-only mode it updates its
   register file and handles fences and events, but it does not draw.

### The draw path, as built in Phase 2

`src/draw.cpp` is a port of the SDK Vulkan backend's `IssueDraw`, with the SDK's backend-independent
pieces doing the work wherever they exist:

| Step | What it does | Code |
|---|---|---|
| Shaders | The tracked microcode (copied at `IM_LOAD`, D20) → `SpirvShader`, analyzed once; the modification from the draw state (`GetCurrent*ShaderModification`, for a device without clip and cull distances); SPIR-V → DiligentCore shader. | `shaders.cpp` |
| Primitives | The SDK's `PrimitiveProcessor`: host primitive type, host vertex shader type (points and rectangles expanded in the vertex shader), index buffer (guest in place, converted, or built-in). | `primitives.cpp` (D17) |
| Render targets | The colors the pixel shader writes with a non-empty mask (`GetNormalizedColorMask`), and depth if tested, from the surfaces the device has bound. | `draw.cpp`, `targets.cpp` |
| Pipeline | `GetCurrentStateDescription` in DiligentCore terms: topology, cull and fill, depth and stencil (one mask for both faces), depth bias (static), blending (the D3D12 backend's factor tables). | `pipelines.cpp` |
| Viewport, scissor | `draw_util::GetHostViewportInfo` with a bottom-left NDC origin (D19) and float24 depth in its guest range (D24), `draw_util::GetScissor`. In tiled passes the window offset and window scissor are neutralized for the draw: it covers the whole surface once. | `draw.cpp` |
| Textures (loading) | The SDK's `TextureCache::RequestTextures` for the textures both shaders use, before the vertex data: outdated textures are loaded by the SDK's load shaders, unless their guest data hashes as at their last load. | `guest_textures.cpp` (D23) |
| Vertex data | Every vertex fetch constant the vertex shader uses: its range requested in the guest memory mirror. Then the mirror is put in its read state once, after any upload. | `guest_memory.cpp` (D17) |
| Constants | System constants (`UpdateSystemConstantValues`, host render target path; texture swizzles and signs from the texture cache), float constants packed by the shader's constant map, bool/loop and fetch constants: dynamic buffers, mapped per draw. | `draw.cpp` |
| Textures (binding) | Per binding: a resolve result at the fetch constant's address, the guest texture, or a placeholder (also for the binding of the other dimension: stacked and 3D textures share fetch instructions, and the shader picks one of the two bindings at run time); samplers from the fetch constant and the instruction's overrides. | `textures.cpp` |
| Draw | `SetRenderTargets` when the targets changed (D30: binding them again ends DiligentCore's render pass), pipeline, `CommitShaderResources`, viewport, scissor, stencil reference, blend factors, then `Draw` or `DrawIndexed`. | `draw.cpp` |

## Rebuilding a draw's GPU state

Settled in Phase 1: the observer checks this rebuild against the SDK command processor's register file, and
it matches 100 % of POTCAWE's draws in every verified scene, with and without predicated tiling. The rules
and their evidence are in [`xdk-d3d-reference.md`](xdk-d3d-reference.md#rebuilding-a-draws-gpu-state); in
short:

| State | Source |
|---|---|
| Render state | The register shadow **after** the draw call (the flush adjusts a few registers). |
| `SQ_PROGRAM_CNTL`, `SQ_CONTEXT_MISC`, `SQ_INTERPOLATOR_CNTL`, `VGT_INDX_OFFSET` | The last value written in the command buffer: the library writes them only there. |
| Fetch constants | The shadow, plus what packets wrote outside the flush (inline draws, the library's clears) until the slot is flushed again. |
| Float constants | A model: dirty groups from the shadow, then the shaders' literal constants (`LOAD_ALU_CONSTANT`), which persist. |
| Bool and loop constants | The shadow. |
| Shader microcode | The programs the flush loaded (`IM_LOAD` in the draw's packets): this is the vertex shader variant patched for the declaration. Copied when the packet is read; packets are read as each command buffer segment ends (D21). |
| `VGT_DRAW_INITIATOR`, `VGT_DMA_BASE`, `VGT_DMA_SIZE` | The draw packet's payload, bin-predicated or not (D20). |

The packet reader follows only register writes, constant loads, shader loads, indirect buffers and draw
packets, in the few hundred bytes a draw writes. It is not an emulation of the command processor: events
and EDRAM are ignored, and bin-predicated packets other than draws (the per-tile registers) are not
applied; their register writes are kept apart for the calls that need them (the query's sample count
address, D32). Since Phase 2 it lives in the core (`src/state.cpp`), and the renderer and the observer
share one rebuild.

The tracker reads the packets of every call whose first argument is the device. A call flagged `kPackets`
in `calls.inc` (Query Issue, whose first argument is the query) is read with the device of the last device
call.

## Draws, clears, resolves and Swap

Clears, resolves and Swap work the same way as draws:
- **Clear** clears the host render target.
- **Resolve** copies the host render target (MSAA averaged or one sample, red/blue swap and exponent bias
  applied) into the host texture registered at the destination address, into the slice the call names.
  Resolved textures are 2D arrays: a stacked destination has one slice per guest slice.
- **Swap** presents the texture resolved to the front buffer, through the front buffer's fetch constant
  swizzle (which undoes a resolve's red/blue swap).

## Components

All of these live in `xenos_native/` and contain no game-specific code.

| Component | Responsibility | Built on |
|---|---|---|
| Hook interface | Entry points the game's hook file calls (`OnDraw`, `OnClear`, `OnResolve`, `OnSwap`, `OnSetRenderTarget`…) plus pass-through when XenosNative is off. | `REX_HOOK_RAW`, `__imp__` originals |
| Device state reader | Reads the D3D device structure of the current XDK version from guest memory (big-endian). | XDK profile |
| Shader cache | Microcode copied at load, hashed and analyzed on the render thread; SPIR-V from the SDK's `SpirvShaderTranslator` and the Diligent shader object, per modification, made on two worker threads with a translator each (D34). A draw whose shaders are not ready is skipped. | SDK translator |
| Pipeline cache | Pipelines keyed by shader pair, primitive type, render target formats, MSAA, blend/depth/stencil/raster state. Compiled in DiligentCore's thread pool (D28); a draw whose pipeline is not ready is skipped. | DiligentCore |
| Guest memory mirror | The GPU-side copy of guest memory that the translated shaders fetch vertices from and indexed draws take indices from, kept current page by page for the ranges draws use. Indices are swapped by the vertex shader. A page invalidated by a CPU write is copied again only if its data hash changed (D35): the invalidation is coarser than the writes. | SDK `SharedMemory` on a Diligent buffer (D17) |
| Primitive processing | Host primitive and vertex shader types, index conversions. | SDK `PrimitiveProcessor` (D17) |
| Texture cache | Host textures from fetch constants: address, format, size, tiling, mips, loaded by the SDK's load shaders as DiligentCore compute pipelines. Block formats (DXT1 to DXN) stay compressed on the host when their size is a multiple of the block (D33). Invalidated when the CPU writes their pages (the mirror's page watches); an outdated texture whose guest data hashes the same is not loaded again. Each texture reports its host size, mips included, to the SDK's memory limits. | SDK `TextureCache` on DiligentCore textures (D23) |
| Render target cache | Host render targets keyed by the guest surface (EDRAM base, pitch, EDRAM storage format, MSAA, size), created at the render scale (D38). No EDRAM. | DiligentCore |
| Resolve registry | Guest address (plus format, size and slice count) → host 2D array texture at the render scale, produced by resolves. Texture lookups hit it first; the guest texture cache skips loading what it covers. A CPU read of a registered range would need a readback (none seen). | — |
| Antialiasing | SMAA (vendored reference, compiled as GLSL at startup) or FXAA (the SDK's), on the texture of the resolve the game's profile names as the finished scene (`scene_resolve_caller`), right after the copy and before the HUD composite reads it (D41, `src/aa.cpp`). | DiligentCore |
| Presenter | Present at the Present call: the front buffer through the guest's display gamma ramp at its own size (D36), then scaled to the window. XenosNative's own swap chain; the SDK's presenter is detached from the window. At most two frames in flight (a fence wait after Present, D22). | DiligentCore swap chain; the ramp from the command processor (`sdk_patches/0004`) |
| Host device | DiligentCore's Vulkan device, with XenosNative's patches (`diligent_patches/`, D22): `robustBufferAccess`, which the translated shaders need (0001), and BC1 with 1-bit alpha, as in Direct3D (0002, D33). DiligentCore ignores Vulkan errors in release builds; the renderer watches its queue fence to report a lost device. | DiligentCore |
| Sync-only command processor | Executes the ring for synchronization and skips host rendering. | SDK patch (D10) |
| Occlusion queries | A host occlusion query between the guest's Query Issue(BEGIN) and Issue(END), begun and ended outside any render pass, keyed by the end event's sample count address. Results are collected once per frame without waiting; when the command processor executes the end event, it asks for the latest count of that address and writes it where the D3D library reads it (D32, `src/queries.cpp`). | SDK occlusion query resolver (`sdk_patches/0003`) |
| Trace and observer | Per-frame call trace; per-draw comparison of the rebuilt state with the command processor's register file. Used to bring up every new game. Done in Phase 1 (`src/trace.cpp`, `src/observer.cpp`). | SDK draw observer (`sdk_patches/0001`) |

### Binding contract (settled in Phase 2, D18)

The SDK's SPIR-V translator declares guest memory in set 0, the constant buffers in set 1, and the vertex
and pixel textures and samplers in sets 2 and 3. XenosNative gives the SPIR-V to DiligentCore unchanged
(plus an `OpSource` instruction) and binds by the names DiligentCore's reflection reports: the block types
`XeSharedMemory`, `XeSystemConstants`, `XeFloatConstants` (one per stage), `XeBoolLoopConstants` and
`XeFetchConstants`, and the variables `xe_texture…` and `xe_sampler…`. DiligentCore remaps the sets and
bindings to its own layout.

### SDK graphics code (settled in Phase 2, D15)

The SDK builds its GPU backend as a plugin DLL (`rexgpu-xenos*.dll`) and exports none of it. XenosNative
compiles the sources it needs into `xenos_native_sdk`, as Hell's Gate does: the translator, the shader
analysis, the register file, the draw and texture utilities, `SharedMemory` and `PrimitiveProcessor`.
Their cvars stay registered by the plugin; XenosNative's copies forward to it.

## XDK profile

Everything that depends on the executable or on the XDK version, gathered in one place per game:

- **Entry points**: the address of each hooked D3D function. These have to be compile-time symbols
  (`sub_822B58B0`), so the profile is a C++ file of one macro line per function, not a runtime table.
  Settled in Phase 1 ([`include/xenos_native/hooks.h`](../include/xenos_native/hooks.h)); the calls a game
  can report are listed in [`include/xenos_native/calls.inc`](../include/xenos_native/calls.inc):

  ```cpp
  // src/xenos_native_potcawe.cpp (game side)
  #include <xenos_native/hooks.h>
  XENOS_NATIVE_HOOK(824AC3C0, DrawIndexedVertices)
  XENOS_NATIVE_HOOK(822C2048, Resolve)
  XENOS_NATIVE_HOOK(822B5D58, Present)
  ```

  Each hook runs the original. With no feature on, it is one predictable branch around the call.
- **Device layout**: offsets in the D3D device structure. These are runtime data, a
  `xenos_native::DeviceLayout` the game fills and passes to `xenos_native::Initialize()` before the guest
  starts:
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

- **Behavior that depends on the console's limits:** POTCAWE computes its tile count from the size of EDRAM
  (`src/msaa.cpp`), and resolves each tile itself.
- **Things the engine computes from GPU results:** the sun flare's expected occlusion sample count.
- **Videos,** if the engine draws them outside the usual path. POTCAWE's Bink videos use the usual path,
  with planes the CPU writes without Unlock.

The core offers callbacks for these (for example, a hook before a clear, or a surface-alias policy) once
Phase 3 shows which ones are needed. They are not designed up front.

Phase 1 moved one item out of this list: the clears through a surface with another pitch and MSAA, which
looked like an engine trick from the Xenos path, are the XDK library's own ClearF. They are core behavior,
and every XDK game has them.

## Bring-up tools: trace and observer

Both are library features, off by default and free when off. Every new game goes through them
([`integrating-a-game.md`](integrating-a-game.md)).

- **Trace** (`--xenos_native_trace`): call counts per frame (outermost, nested, other threads) in the log
  every `xenos_native_trace_interval` frames. F9 (`bind_xenos_native_trace`) or `xenos_native_trace_dump_at`
  dumps whole frames call by call, with:
  - arguments, surfaces, textures and rects;
  - the draw state read from the device;
  - the range of the command buffer each call wrote.

  [`tools/xn_trace.py`](../tools/README.md) prints a dump's passes.
- **Observer** (`--xenos_native_verify`): F10 (`bind_xenos_native_verify`) or `xenos_native_verify_at`
  verifies `xenos_native_verify_frames` frames after a warm-up frame. It:
  - registers the SDK's draw observer;
  - matches every draw packet the command processor executes to the call that wrote it, by address;
  - compares the rebuilt state with the register file, and the programs (microcode hashes) with the
    shaders the command processor loaded (D26).

  The report in the log gives:
  - the draws that fully match;
  - the mismatches by register, by constant and by program;
  - what differs per tile;
  - the library's own draws;
  - memexport use.

  The command buffer is a ring, so later frames reuse the verified frames' addresses. The observer only
  matches packets from the first one the GPU runs inside the verified range until the first one outside
  it.

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

Settled in Phase 3 (D37, D38).

- The game believes it renders at its own size (POTCAWE: 1280×720, and 1024² shadow maps).
- **The scale follows the screen.** It is the largest at which the game's image (the profile's
  `output_width/height`) fits the output: the swap chain at startup, which is the monitor in fullscreen
  and the client area in a window. It is at least 1 and need not be a whole number, for example
  1366×768 → 1.067, 1920×1200 → 1.5, 3440×1440 → 2. The cvar `xenos_native_resolution_scale` overrides
  it.
- **Everything the game renders is scaled.** XenosNative creates each host render target and resolve
  texture at `guest size × scale` (`src/scale.h`: `floor(guest × scale + 0.5)` for every edge and size).
  It also scales:
  - viewports (as floats; the SDK helper runs at scale 1);
  - scissors and resolve rectangles;
  - occlusion query counts (divided back).
- **The translator** reverts the fractional scale where shaders read pixel positions, and in texel
  offsets and weights of resolved textures (`sdk_patches/0005`, the `textures_resolution_scaled` bits).
- **Checked against the Xenos path at exactly 2×.** It matches more closely than at 1×. Fractional
  scales (1.067 to 1.6) show no seams or broken effects at the dock.
- **Other aspect ratios** need the game's projection, HUD and menus changed. That is per game (like
  GRAW's `src/widescreen.cpp`), and Phase 4 for POTCAWE. The renderer's scale has an x and a y for it.

## Selection and fallback

- An init-only cvar, `xenos_native` (`false` by default during development), selects the renderer at
  startup.
- When it is off, every hook is a pure pass-through to `__imp__sub_X`, the command processor draws as
  usual, and nothing of XenosNative is initialized.
- Per game, the launcher gets a "Renderer" row. POTCAWE's has it since Phase 2 (D25), marked
  experimental. A game setting XenosNative does not support yet is turned off by the game's own hooks when
  `xenos_native::Renders()` says XenosNative draws (POTCAWE: 2× MSAA), and its launcher shows it disabled.
- Switching needs a restart.
- With XenosNative on, the SDK's backend only keeps the GPU in sync. The game selects the SDK's Vulkan
  backend then (POTCAWE: D27), so the process uses one graphics API.

## The hard parts

| Problem | Approach |
|---|---|
| Predicated tiling (POTCAWE: 2 tiles with 2× MSAA) | Confirmed in Phase 1: the CPU issues each draw once, and only the window offset, the scissor and the query address differ per tile. XenosNative draws the pass once at full size. POTCAWE resolves each tile itself (SetPredication + Resolve per tile rectangle, same texture): those become one resolve. |
| Surfaces aliasing the same EDRAM | No EDRAM. The aliased clears are the library's ClearF: a host clear of the bound targets. Other aliases are handled when they appear, and logged. |
| Shader literal constants and program registers | Not in the shadow: read from the packets the draw call wrote (rebuild rules 2–4). |
| The library's own draws | ClearF and Resolve rectangles, and SetShaderGPRAllocation's prebuilt one-point draws. The first two become host clears and resolves; the last are skipped. |
| Textures the CPU writes without Unlock (POTCAWE's Bink video planes) | Settled in Phase 2 (D23): the SDK's page watches mark the textures outdated whatever wrote them, and a hash of the guest data skips loads whose data did not change (the watches cover 256 KB blocks, so they also fire for unrelated writes next to a texture). |
| A surface's format rewritten between its draws and its resolve | Host render targets are keyed by the EDRAM storage format, as the SDK's are: the "_AS_" color formats only change what a resolve converts to. |
| Depth sampled after a resolve (shadow maps) | Host depth keeps the guest's range (D24); the resolve copies it as it is, so shaders compare it with the depth they compute. |
| Resolves to the same address at different sizes within a frame (seen by Hell's Gate) | The registry is keyed by address plus size and format, and the newest entry shadows older ones. That is also what patch 0019 found for POTCAWE's bloom buffer. |
| Gamma and float render target formats (8_8_8_8_GAMMA, 7e3 floats) | Use the SDK translator's existing handling, and match the formats the SDK's Vulkan backend picks. |
| Shader control flow on NVIDIA (`loop { switch }`) | See the plan's risks: an "if blocks" mode in the SPIR-V translator if Port Royal shows the flat-color corruption. |
| Occlusion queries | Settled in Phase 3 (D32): host queries begun and ended at the D3D library's Issue calls; the sync-only command processor writes their counts where the library expects them, at the end event, from the latest result (one or two frames late, never waiting for the host GPU). With tiling, the whole count goes to the last tile's block. |
| CPU reads of GPU results, memexport | Phase 1 found neither in POTCAWE (no texture lock per frame, no memexport shader). A readback path only if Phase 3 shows stale data where the CPU reads. |
| ImGui overlay and SDK presenter | Decided in Phase 2. The overlay may stay Xenos-only at first. |

## Opportunities found in Phase 1

Things the mapping showed that make the renderer simpler or faster than the design assumed. They are ideas
for Phases 2–3, not commitments.

- **Unlock names the memory the CPU wrote.** The XDK's Unlock flushes the CPU cache over exactly the locked
  range (`dcbf` loop; POTCAWE `sub_822AF398`). Hooking that flush gives dirty ranges for vertex, index and
  texture data with no hashing and no page protection. It does not cover CPU writes without Unlock (Bink).
- **The vertex shader variant comes for free.** The draw's packets hold the `IM_LOAD` of the vertex shader
  variant the flush patched for the declaration, so XenosNative does not need to reimplement the XDK's
  patching (the problem Hell's Gate solved the same way).
- **Clears and resolves need no aliasing logic.** They are recognizable library calls with their arguments:
  flags, rectangle, color, destination texture.
- **The GPR allocation draws can be skipped.** 144–192 of POTCAWE's draw packets per frame are pipe flushes
  for a GPR split the host does not have.
- **One draw submitter.** All of POTCAWE's scene meshes go through `sub_823DF3A8`. If the per-draw CPU cost
  matters in Phase 3, that is the place to batch.
- **The observer as a regression test.** It runs in a few seconds through `tools/level2.ps1`. Phase 2 can
  keep it green while it replaces the command processor's drawing, and compare its own state with the
  rebuild.

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
  own root signature. Revisit after Phase 6.
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
