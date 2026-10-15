# NativeXRenderer — decision log

One entry per decision: date, what was decided, why, and what would make us revisit it. Add entries at the
end, and do not rewrite old ones. If a decision changes, add a new entry that supersedes the old one.

The project was called **XenosNative** until D58. Entries D1–D57 keep that name: read `XenosNative` as
NativeXRenderer, and `xenos_native` (folders, cvars, namespace) as `native_x_renderer`.

---

## D1 — Replace the D3D layer, not the GPU (2026-10-13)

**Decision:** XenosNative hooks the game's XDK Direct3D library and draws on the host. It does not change
the SDK's Xenos emulation.

**Why:** the remaining GPU cost of POTCAWE on the Xenos path is largely emulation:
- EDRAM ownership transfers;
- resolves through the EDRAM buffer and guest memory;
- texture reloads of resolve results;
- the scene drawn twice because of predicated tiling.

SDK patches 0017–0019 showed how far optimizing the emulator goes: 0019 (lazy resolves) measured no gain.
A renderer that sees D3D calls has none of these costs.

**Revisit if:** Phase 1 shows that the engine bypasses the D3D library for a large part of its rendering.

## D2 — DiligentCore with its Vulkan backend (2026-10-13, Gustavo)

**Decision:** the host API layer is DiligentCore (Apache 2.0), Vulkan backend.

**Why:**
- Hell's Gate proved the combination with the same SDK version (v0.10.0) and the SDK's SPIR-V translator.
- Less of our own code for devices, swap chains, pipelines and resource binding than raw Vulkan.
- A D3D12 backend remains possible later.

**Cost accepted:** a large dependency, and its build integration (known issues, see the plan, Phase 2
step 1).

**Revisit if:** the integration costs more than writing the needed Vulkan code directly on the SDK's
Vulkan device.

## D3 — One repository, the library in its own folder (2026-10-13, Gustavo)

**Decision:** `POTCAWERecompVulkan` is a clone of POTCAWERecomp. XenosNative lives in `xenos_native/`, with
its own CMake target and documents, and with no game-specific code. Once a second game uses it, it is
extracted with `git subtree split --prefix=xenos_native` and used as a submodule.

**Why:** developing a library with one user in two repositories doubles every change. Keeping the folder
clean keeps the extraction a mechanical step.

## D4 — Reuse the SDK's translator and texture code (2026-10-13)

**Decision:** shaders are translated with the SDK's `SpirvShaderTranslator`, and textures are loaded with
the SDK's texture loaders. XenosNative does not write its own.

**Why:** they are proven on these games, through the SDK's Vulkan backend and through Hell's Gate.

**Revisit if:** the translator's `loop { switch }` control flow corrupts shaders on Vulkan (see the plan's
risks). The fix would then be an option in the translator, not a new translator.

## D5 — Keep the Xenos path as fallback and reference; sync-only command processor (2026-10-13)

**Decision:** the original D3D bodies keep running, and the SDK command processor keeps executing the ring
in a sync-only mode. A cvar selects the renderer at startup.

**Why:**
- Synchronization stays correct without reimplementing it: fences, swap and vblank callbacks, waits,
  queries.
- Every comparison has a reference.
- Users keep a working game while parity is incomplete.

**Revisit if:** frame pacing above 60 FPS needs the ring removed (as Hell's Gate found).

## D6 — Starting point: POTCAWERecomp `rt-to-texture` at `c6be8a3` (2026-10-13)

**Decision:** the clone starts from `rt-to-texture`, not `main`.

**Why:** SDK patch 0020 (occlusion queries, the sun flare) exists only there, and it is applied on top of
patch 0019. Patch 0019 (lazy resolves) is an option that is off and harmless.

## D7 — Hell's Gate as a design reference only (2026-10-13)

**Decision:** no code is copied from Hell's Gate. Its documents inform the design, and it is credited.

**Why:** its repository has no license (checked 2026-10-13 through the GitHub API: `license: null`), which
means all rights are reserved. Its documents are kept for personal reference in `local_docs/reference/`,
which is gitignored.

**Revisit if:** the author adds a license or gives permission. Then record which code was adopted, under
which terms.

## D8 — Name: XenosNative (2026-10-13, Gustavo)

**Decision:** folder `xenos_native/`, namespace `xenos_native`, CMake target `xenos_native`, cvars prefixed
`xenos_native`.

## D9 — The game files are copied (2026-10-13, Gustavo)

**Decision:** `assets/` holds a full copy of the game files (6.4 GB), so the project does not depend on
POTCAWERecomp's folder.

## D10 — XenosNative's SDK patches travel with the library (2026-10-13, proposed)

**Decision (to confirm with the first such patch):**
- SDK changes that only XenosNative needs go into `xenos_native/sdk_patches/` and are applied after the
  shared series. Examples: the sync-only command processor mode, the draw callback for the observer, and
  exports of translator and texture code if that route is chosen.
- `cmake/apply_sdk_patches.cmake` applies both folders.
- The shared `patches/` series stays identical to GRAWRecomp's and KameoRecomp's.

**Why:** when `xenos_native/` is extracted, its SDK requirements go with it. Games that do not use
XenosNative do not carry its patches.

**Note:** the observer and the trace mode are part of the library, not debug instrumentation. Every new
game goes through them, so project rule 2 (no debug instrumentation in the repository) does not apply to
them. They are off by default and cost nothing when off.

## D11 — D10 confirmed: the draw observer is XenosNative's first SDK patch (2026-10-13)

**Decision:**
- D10 holds. `xenos_native/sdk_patches/0001` adds `IGraphicsSystem::SetDrawObserver`: an optional
  callback the command processor calls for every draw packet it executes.
- `cmake/apply_sdk_patches.cmake` applies `xenos_native/sdk_patches/` after `patches/`, in one series: one
  stamp, and the same "start over" message if anything changed.
- The patch bumps `kGpuPluginAbiVersion` to 2, because the plugin interface gained a virtual function.

**Why:**
- The observer needs the command processor's register file at each draw, and the command processor lives
  in the GPU plugin DLL.
- A plain C callback with a plain-data structure crosses the DLL boundary safely.
- With no observer set, the patch costs one relaxed atomic load per type-3 packet.
- The shared series stays identical to GRAWRecomp's and KameoRecomp's.

**Revisit if:** the SDK gains its own draw hook upstream; then drop the patch.

## D12 — A draw's state: the device shadow, plus the packets the draw call itself wrote (2026-10-13)

**Decision:** XenosNative rebuilds a draw's GPU state from the device structure (register shadow,
constants, fetch constants, current objects) **and** from the packets the D3D call wrote. It reads those
packets right after the call returns, to get what the library writes only into the command buffer:
- the program control registers;
- the shader microcode actually loaded (`IM_LOAD`);
- the shaders' literal constants (`LOAD_ALU_CONSTANT`), kept in a small persistent model;
- `VGT_INDX_OFFSET`;
- fetch constants written outside the flush.

The rules are in [`xdk-d3d-reference.md`](xdk-d3d-reference.md#rebuilding-a-draws-gpu-state).

**Why:**
- The shadow alone matched 92 % of POTCAWE's draws in the observer. The shadow plus the call's own packets
  match 100 % in every verified scene.
- The alternative for the program registers and the literals was to reimplement the XDK's program flush
  from the shader objects. That is more code, more version-specific, and needs its own verification.
- Hell's Gate reached the same conclusion for the vertex shader variant: it reads the last `IM_LOAD`.
- The packet reader is small and follows only register writes, constant and shader loads, and indirect
  buffers. It is not a second command processor: no draws, predication, events or EDRAM.

**Revisit if:** a game writes the same state through a path the reader does not follow (for example an
engine constant buffer outside the call's range). The observer would show it as a mismatch.

## D13 — Bring-up features share one hook per entry point (2026-10-13)

**Decision:**
- The game declares each entry point once (`XENOS_NATIVE_HOOK(address, Call)`, in its own executable).
- The library dispatches every hooked call to its features (trace, observer, later the renderer) with
  whether it is nested in another hooked call, and ends the frame at Present.
- The list of calls is one table (`include/xenos_native/calls.inc`): name, argument names for the trace,
  flags.
- Features are off by default:
  - cvars `xenos_native_trace` and `xenos_native_verify`;
  - keys F9 and F10, through the SDK's rebindable keybinds.

  With all features off a hook is one branch around the original.

**Why:**
- A new game writes one line per function and gets the trace and the observer for free.
- The nesting flag tells the engine's calls from the library's internal ones (Clear → ClearF,
  DrawVerticesUP → BeginVertices, EndTiling → SetPredication).
- The renderer of Phase 2 will plug into the same dispatch.

## D14 — Go for Phase 2 (2026-10-13, Gustavo)

**Decision:** Phase 1 met its exit criterion, so XenosNative goes on to the core renderer (Phase 2).

**Why:** every draw of POTCAWE is visible at the D3D layer, and its GPU state is rebuilt exactly (100 % in
every verified scene). The costs D1 targets are all there and reachable:
- the aliased clears are library calls;
- the tiled scene is issued once by the CPU;
- the resolves are explicit calls with a destination texture.

**Also decided:** the analysis tools of Phase 1 move into the library (`xenos_native/tools/`), generic and
tracked, so the next game uses them as they are.

## D15 — The SDK's graphics code compiled into XenosNative, the cvars left to the plugin (2026-10-13)

**Decision (Phase 2, step 2):**
- XenosNative compiles the part of the SDK's graphics code it needs into its own static target
  `xenos_native_sdk`:
  - the SPIR-V shader translator and `SpirvShader`;
  - the register file;
  - the ucode, texture and draw utilities.
- The GPU plugin keeps its own copy, as it is.
- The cvars stay the plugin's: three cvars the code reads get accessors that forward to the plugin's
  registered value (`src/sdk_cvars.cpp`), and `util/draw.cpp`, which defines two cvars, is compiled
  through a wrapper (`src/sdk_draw.cpp`) that turns those definitions into forwarders.
- `util/draw_extent_estimator.cpp` (it defines `execute_unclipped_draw_vs_on_cpu`) and `flags.cpp` are
  not compiled.

**Why:**
- The plugin is a DLL with hidden symbols; exporting C++ classes from it would need a large SDK patch.
  Hell's Gate also compiles the SDK sources into its executable.
- The cvar registry ignores a second registration of a name. The executable's static initializers run
  before the plugin is loaded, so a second copy of the definitions would win, and the settings
  (`potcawe.toml`, the command line, `src/startup.cpp`'s defaults such as
  `execute_unclipped_draw_vs_on_cpu = true`) would stop reaching the plugin, which still runs the Xenos
  path. Forwarders keep one registration, the plugin's.

**Revisit if:** the SDK exports a shader translation interface from the plugin, or moves the cvar
definitions out of the sources XenosNative needs.

## D16 — XenosNative's utility shaders: GLSL, compiled at startup with the SDK's glslang (2026-10-13)

**Decision:**
- XenosNative's own shaders are written in GLSL: the copies of resolves, presenting, later clears of
  rectangles.
- They are compiled to SPIR-V when the renderer starts, with the glslang library the SDK already builds
  (`src/glsl.cpp`, with glslang's default resource limits).
- DiligentCore receives SPIR-V only, as for the game's shaders.

**Why:**
- DiligentCore is built without its compilers (`DILIGENT_NO_GLSLANG`, D15). Turning them on would bring
  a second glslang and SPIRV-Tools next to the SDK's.
- Writing the utility shaders with the SPIR-V builder, as the SDK does for its own, is much more code
  for a handful of small shaders.
- Precompiled SPIR-V would need a build step and a glslang executable the SDK does not build.

**Cost:** a few milliseconds at startup.

## D17 — Guest memory and primitives: the SDK's SharedMemory and PrimitiveProcessor on DiligentCore (2026-10-13)

**Decision:**
- Vertex and index data reach the GPU the way the SDK's backends do it: one 512 MB storage buffer mirrors
  guest physical memory, the translated vertex shaders fetch vertices from it, and indexed draws use it
  as their index buffer.
- XenosNative does not reimplement this. It subclasses the SDK's `SharedMemory` (`src/guest_memory.cpp`)
  and `PrimitiveProcessor` (`src/primitives.cpp`), which are backend-independent, and gives them
  DiligentCore buffers. Both base classes are compiled into `xenos_native_sdk` (D15).
- Each draw requests the ranges its vertex fetch constants and its index buffer cover. Pages already
  valid cost nothing. A page is uploaded again after the CPU writes it, which the base class learns
  through the memory's write watches.

**Why:**
- The page watches cover every CPU write, with or without Unlock. The planned alternative, hooking the
  Unlock cache flush, misses the Bink planes (written without Unlock) and the dynamic data written into
  the command buffer.
- The primitive processor already knows every conversion the host needs: triangle fans, quad lists and
  line loops to lists and strips, points and rectangles expanded in the vertex shader, primitive reset
  values. Index endianness is swapped by the vertex shader, so guest index buffers are used in place.
- The SDK's own `SharedMemory` stays idle on this path (host rendering is off, D5). The memory's
  invalidation callbacks support several listeners, so the two coexist.

**Cost:** about 170 small copies per frame at the dock (1.5 MB, mostly vertex data the game writes every
frame). Each copy ends the render pass in progress. To revisit in Phase 3.

**Rejected:** the Unlock hook as the only invalidation source (incomplete, above). It stays a possible
optimization: it names exact ranges where page watches need an access violation.

## D18 — Resources bound by name through DiligentCore's SPIR-V reflection (2026-10-13)

**Decision:**
- The translator's SPIR-V is given to DiligentCore unchanged except for one instruction (below).
  DiligentCore reflects it and remaps its descriptor sets to its own layout.
- XenosNative binds by the names DiligentCore reports:
  - storage and uniform buffers by their block type: `XeSharedMemory`, `XeSystemConstants`,
    `XeFloatConstants`, `XeBoolLoopConstants`, `XeFetchConstants`;
  - textures and samplers by their variable names, `xe_texture{N}_{2d|3d|cube}_{u|s}` and
    `xe_sampler{N}_...`, in the order of the shader's bindings.
- The buffers are static variables, one per stage; textures and samplers are dynamic variables, set per
  draw.
- The translator's `Features` describe the DiligentCore device: SPIR-V 1.5, one shared memory binding,
  full 32-bit indices, no clip or cull distances, swizzles applied in the shader.
- `OpSource Unknown 0` is added to every module.

**Why:**
- DiligentCore names a buffer by its block type unless the module says it is HLSL.
- `XeFloatConstants` exists in both stages with different contents, so it must not become one resource.
- DiligentCore refuses a module without `OpSource`, and glslang's builder writes none for an unknown
  source language.
- DiligentCore does not enable shader clip and cull distances. Image views with a swizzle would need one
  view per swizzle.

**Rejected:** declaring an explicit pipeline resource signature with the translator's set and binding
numbers. It is more code, and DiligentCore would remap the bindings anyway.

## D19 — Viewport: the translated shaders produce Direct3D-style Y (2026-10-13)

**Decision:** draws use `draw_util::GetHostViewportInfo` with `origin_bottom_left = true`, as the SDK's
D3D12 backend does, and pass the viewport to DiligentCore unchanged.

**Why:** DiligentCore's Vulkan backend flips Y with a negative viewport height, to behave like Direct3D.
The SDK's Vulkan backend expects an unflipped viewport. With the flag, the two flips cancel, and the pixels
(and so the winding rule) are those of the SDK's Vulkan path.

## D20 — D12 completed: the draw packet's registers, and microcode copied when loaded (2026-10-13)

**Decision:** two additions to the rebuild of D12, found when draws first ran:
- `VGT_DRAW_INITIATOR`, `VGT_DMA_BASE` and `VGT_DMA_SIZE` come from the draw packet itself, which the
  command processor decodes into those registers. The tracker reads every draw packet, including
  bin-predicated ones: the XDK predicates all draw packets, tiled or not.
- The tracker copies the vertex and pixel microcode when the call that loads it (`IM_LOAD`) returns, and
  keeps the copy.

**Why:**
- The observer compared the register shadow's groups only. These three registers are outside them, so
  Phase 1's 100 % did not cover them. Without them, every draw used the initiator of the last internal
  rectangle (a clear or a resolve).
- The GPU copies the microcode when it executes the `IM_LOAD`; later draws run the copy. While a level
  loads, the game frees and reuses that memory: reading it at draw time gave microcode the SDK's
  disassembler threw on, and the game crashed while the dock loaded. With the copy, no shader has failed
  analysis since.

## D21 — Packets read when each command buffer segment ends; D20's explanation corrected (2026-10-13)

**Decision:**
- The tracker reads a call's packets as soon as each command buffer segment ends (the nested
  `KickSegment`), and the rest when the call returns. Before, it read everything when the call returned.
- Shader microcode (`IM_LOAD`, `IM_LOAD_IMMEDIATE`) and `LOAD_ALU_CONSTANT` data are copied at the moment
  the packet is read.
- The shader cache protects the SDK's analysis against access violations as well as exceptions (some
  invalid microcode makes it dereference null), and logs where a rejected program came from.

**Why:** D20 said the vertex shader memory was reused while a level loads. The diagnostics showed
something more specific:
- the failing programs are vertex shaders loaded inline (`IM_LOAD_IMMEDIATE`, 105–192 dwords) by
  `DrawIndexedVertices`;
- our copy matches the shader the SDK dumps (`--dump_shaders`) for its first 16–22 dwords, then holds a
  coherency packet sequence (`COHER_STATUS_HOST`, `COHER_SIZE_HOST`, `COHER_BASE_HOST`, `WAIT_REG_MEM`, as
  `sub_822B0010` writes it) where the microcode should be.

Reading each finished segment early removed most of these, but not all: one load in three still shows a
few (3 in `potcawe_046`). Something writes that sequence into the command buffer over the inline
microcode. The Xenos path, which reads the microcode later (when the command processor executes the
packet), has never failed on these shaders, so the final contents are probably right, but that is not
verified packet by packet. The writer is not identified yet (open point in `local_docs/05-phase2.md`). Meanwhile the draws that get such a program are skipped and logged,
not crashed on.

## D22 — DiligentCore patched for robust buffer access; frames in flight bounded (2026-10-14)

**Decision:**
- XenosNative's changes to DiligentCore are a `git format-patch` series in `xenos_native/diligent_patches/`,
  applied to the submodule at configure time (`xenos_native/cmake/apply_diligent_patches.cmake`), as the
  SDK's patches are (D10). It is made on the submodule's branch `xenos-native`, in a worktree.
- Patch 0001 enables Vulkan's `robustBufferAccess` when the device supports it.
- `Device::Present` waits for the GPU to finish the frame two frames back (a DiligentCore fence).
- The renderer logs an error when DiligentCore's command queue fence falls more than 64 submissions behind
  while frames complete.

**Why:**
- Without the feature the device was lost (`VK_ERROR_DEVICE_LOST`) on the first frames of the Port Royal
  dock; with it, it is not. So some buffer access goes out of range. Which one is not identified: the
  translated shaders read guest memory (vertices, indices) at addresses taken from the game's fetch
  constants, and the bound views and buffers have fixed sizes. The SDK's Vulkan backend enables
  `robustBufferAccess` too; DiligentCore does not, and has no option for it.
- DiligentCore ignores Vulkan errors in release builds. After the loss, its waits returned at once and the
  game kept running at 60 FPS, but its command queue fence stopped advancing (it reads fence status, which
  reported the loss), so its dynamic heap was only reclaimed when it filled up and DiligentCore forced a
  GPU idle (every 1.4 s), the duration queries never completed, and the GPU sat at 8.6 %. That looked like
  a GPU that could not keep up; it was a lost device. The check above makes the next one visible.
- Without vertical sync DiligentCore's swap chain uses mailbox presentation, which never blocks: nothing
  bounded how far the CPU could run ahead of a slow GPU.

**Evidence:** `local_docs/05-phase2.md`, runs 20–26: `vkGetFenceStatus` returned -4 from the first dock
frames on; with the feature enabled, no loss, no heap exhaustion, the queue fence 3 submissions behind, GPU
utilization 69 % for 10.7 ms of GPU time per frame.

**Rejected:** creating the Vulkan device in XenosNative and attaching DiligentCore to it
(`AttachToVulkanDevice`): more code to keep in step with DiligentCore's own device setup, for one feature.

## D23 — Guest textures: the SDK's TextureCache on DiligentCore, unchanged data not loaded again (2026-10-14)

**Decision:**
- `GuestTextures` (`src/guest_textures.cpp`) is a subclass of the SDK's `TextureCache`, as `GuestMemory` is
  of `SharedMemory` (D17). The base class keeps doing what does not depend on the backend: texture keys from
  the fetch constants, guest layouts (tiling, packed mips, pitch), the memory a load needs, invalidation
  through the guest memory mirror's page watches, the LRU.
- The host side is ported from the SDK's Vulkan backend:
  - the host formats are the D3D12 backend's table (DiligentCore's formats are DXGI's), changed where
    DiligentCore's Vulkan mapping does not match the DXGI layout: 2_10_10_10 swaps red and blue in the
    host swizzle; 5_6_5 and 4_4_4_4 are expanded to RGBA8; DXT1 is decompressed to RGBA8, because
    DiligentCore's `BC1_UNORM` is Vulkan's BC1 without alpha; 1_5_5_5 and DXT3A_AS_1_1_1_1 are not
    supported yet;
  - the loads are the SDK's precompiled SPIR-V load shaders (untiling, endian swap, format conversion),
    run as DiligentCore compute pipelines. They read the guest memory mirror and write a scratch buffer,
    which `UpdateTexture` copies into the texture, level by level and slice by slice.
- A texture binding samples, in this order: the host copy of a resolve to its address (D11), the guest
  texture, or a placeholder.
- Each draw requests its textures before its vertex data. The tracker does not report fetch constant
  writes one by one, so the 32 bindings are checked again at each draw (cheap when nothing changed).
- When a texture is outdated, its guest data is hashed first (XXH3, base and mips separately). A part that
  hashes as at its last load is not loaded again.

**Why:**
- The SDK's texture code is what the Xenos path runs, so formats, layouts and invalidation behave the same.
  Only the host objects and the dispatches are XenosNative's.
- The hash: `SharedMemory` invalidates the whole 256 KB block around a CPU write, which costs fewer access
  violations. POTCAWE's engine writes reference counts in pages of those blocks every frame, so 31
  textures (16 MB of host data) were reloaded at the dock every frame with their data unchanged. Hashing
  their 5 MB of guest data per frame on the CPU replaces all of those loads. Textures whose data does
  change (the Bink planes) are still loaded.

**Evidence:** `local_docs/05-phase2.md`, runs 27–35:
- every reload at the dock had unchanged data (run 34);
- the writer is game code, not the D3D library (run 32);
- loads stopped at the dock once the hash was in place (run 35).

**Rejected:**
- A CPU path with `texture_util`: slower, and the load shaders already exist.
- Narrower invalidation in `SharedMemory`: the memory callbacks are shared with the Xenos path. It stays
  an option.
- Hooking the engine's writes: game-specific, and the hash covers every game.

## D24 — Float24 depth keeps the guest's values on the host (2026-10-14)

**Decision:** the draw path calls `draw_util::GetHostViewportInfo` with `full_float24_in_0_to_1` false, so
24-bit float depth (`kD24FS8`) keeps its guest value on the host.

**Why:**
- The SDK's Vulkan backend halves float24 depth, mapping its [0, 2) range to [0, 1). This lets EDRAM data
  move through depth attachments, and the backend doubles the values back when it resolves.
- XenosNative has no EDRAM, and its resolves copy host depth as it is. A halved shadow map was therefore
  compared with the full-range depth that the shader computes, and POTCAWE's character shadows disappeared
  (run 42).
- Guest depth above 1, which float24 can hold, is clipped or clamped on the host. It has not been seen.

**Evidence:** `local_docs/05-phase2.md`, run 42: the shadows match the Xenos path.

## D25 — The launcher offers XenosNative before parity; game settings it lacks are turned off (2026-10-14, Gustavo)

**Decision:**
- POTCAWE Launcher gets its "Renderer" row now, in Phase 2: "Xenos (emulated)" or "XenosNative
  (experimental)", saved as `xenos_native` in `potcawe_options.toml`. It is offered only when Vulkan is
  installed.
- With XenosNative, the render quality, antialiasing and graphics API rows show what XenosNative uses
  (1280×720, no MSAA, Vulkan) and are disabled; the choices for Xenos are kept. `draw_resolution_scale` is
  saved as 1, since the SDK's backend only keeps the GPU in sync then.
- The library gets `xenos_native::Renders()`. POTCAWE's MSAA hooks (`src/msaa.cpp`) apply when it is true,
  whatever `potcawe_no_msaa` says, so the command line gets the same result as the launcher.

**Why:**
- Gustavo wants to play with XenosNative to find what still differs from the Xenos path, which was planned
  for Phase 3. Playing through the launcher is the normal way to start the game.
- With 2× MSAA the engine renders the scene in two EDRAM tiles of a 1280×416 surface. XenosNative draws
  each pass once at the surface's size and does not join the tiles yet, so only the top 416 rows showed
  (run 46). Joining them is Phase 3.
- `Renders()` is a general query, not a POTCAWE feature: any game can turn off what XenosNative does not
  support yet.

**Evidence:** `local_docs/05-phase2.md`, runs 46–48.

## D26 — Every segment change read, NewSegment hooked; D21's cause corrected (2026-10-14)

**Decision:**
- A new hooked call, `NewSegment`: the library function that moves the write pointer to a newly
  allocated command buffer segment (POTCAWE `sub_822AE950`). The tracker treats it like `KickSegment`: it
  reads the packets written so far, then continues from the new segment.
- Whichever segment change comes first reads up to the write pointer; the next one reads only what was
  written since, so nothing is read twice when `KickSegment` calls `NewSegment`.
- The observer compares the programs too: the hash of the microcode the tracker copied against the
  command processor's (`ucode_data_hash`, the same XXH3 over the guest's bytes).

**Why:** `KickSegment` is not the only way to a new segment. The library's reservation (POTCAWE
`sub_822AEE50`) calls `KickSegment`, and if what it reserves still does not fit, calls `NewSegment` again
directly. Before this decision, such a call's packets continued in a segment the tracker did not know
about, and one of two things happened:
- The new segment was below the old one in memory: the call was "not fully read" and its packets were lost
  (one or two calls per frame). When those were a draw's program flush, that draw and the following ones
  kept the previous programs (up to 24 draws in a row). A vertex shader that does not match its vertex
  data draws stretched triangles. That is what Gustavo saw at the dock, for a few seconds at a time.
- The new segment was above it: the tracker read the memory in between as packets. That memory holds old
  packets from earlier frames, including stale `IM_LOAD_IMMEDIATE` headers whose microcode had since been
  partly overwritten by newer packets, such as `sub_822B0010`'s coherency sequence. That explains D21's
  "overwritten" inline vertex shaders: no writer overwrote them; the tracker read memory that was no longer
  part of the command buffer.

**Evidence:** `local_docs/05-phase2.md`, runs 49–52. At the dock, 30 observer verifications of 2 frames:
- before: 2–6 calls "not fully read" and up to 33 draws with the wrong vertex program per verification,
  each run of wrong draws starting at a call that was not fully read;
- after: every draw matches (about 68,000), 0 calls not fully read, 0 program mismatches.

With the renderer, 2.5 minutes at the dock after the load: no shader analysis or translation failures (they
appeared in one load in three, and during play), and the shader and pipeline counts stay flat.

## D27 — With XenosNative, the SDK runs its Vulkan backend (2026-10-14)

**Decision:** when `xenos_native` is on, POTCAWE's startup sets `gpu_backend` to `vulkan` before the SDK
loads its GPU plugin (`src/options.cpp`), whatever the launcher or `potcawe.toml` say. The launcher
already shows "Vulkan" in its API row with XenosNative (D25).

**Why:**
- With XenosNative the SDK's backend only keeps the GPU in sync (sync-only command processor), which both
  of its backends support. With D3D12 the process held an idle D3D12 device beside DiligentCore's Vulkan
  device.
- Overlays such as RTSS can attach to that D3D12 device and report "D3D12". Gustavo read that as
  XenosNative not being on, although his logs show it drawing every frame (`potcawe_085`–`087`).
- With one graphics API in the process the overlay reports what presents, and nothing is created that is
  not used.

**Evidence:** `local_docs/05-phase2.md`, run 54: the SDK's Vulkan backend in sync-only mode, XenosNative
drawing, the overlay reports Vulkan, 60 FPS, the same renderer CPU time as with D3D12 underneath.

## D28 — Pipelines compiled asynchronously; draws wait by being skipped (2026-10-14)

**Decision:**
- DiligentCore compiles XenosNative's pipelines in its thread pool (the `AsyncShaderCompilation` feature,
  4 threads, `PSO_CREATE_FLAG_ASYNCHRONOUS`).
- A draw whose pipeline is still compiling is skipped. The pipeline's resources (static variables, the
  resource binding) are set up on the first draw after it is ready.
- Shader analysis and translation (the SDK's) stay on the render thread.
- The renderer's log reports what was created on the render thread (count, time, the longest one) and how
  many draws were skipped while their pipeline compiled.

**Why:**
- A pipeline the driver has not compiled before took up to 205 ms on the render thread (run 56): a
  245 ms frame when the camera turned at the dock. Pipelines the driver has cached take under 1.5 ms.
- Skipping a draw for a frame or two is what the Xenos path does with asynchronous pipelines, which
  POTCAWE already uses (`gpu_async_pipelines_no_frame_wait`, `src/startup.cpp`). A new object appears a
  frame late instead of the whole image stopping.
- Translation costs at most 2.7 ms per shader and happens about a hundred times while a level loads, so
  moving it off the thread can wait.

**Evidence:** `local_docs/05-phase2.md`, runs 56–58. Walking and turning at the dock, the longest frame
went from 245 ms to 18.5 ms, and pipeline creation on the render thread from 300 ms to 0.3 ms per
600 frames. In 12 minutes at the dock, 409 draws were skipped during the load and a few during play, and no
frame took over 20 ms after the load.

## D29 — Phase 2 closed; go for Phase 3, performance first (2026-10-14, Gustavo)

**Decision:** Phase 2 is closed on its exit check (`plan.md`), and Phase 3 starts. Its order, agreed with
Gustavo: GPU time first, then the occlusion queries (the sun glare), then resolution, MSAA and the rest of
the game.

**Why:**
- The exit criteria hold: the three scenes match the Xenos path, the fallback is unchanged, and 12 minutes
  at the dock passed with no crash and no frame over 20 ms after the load.
- XenosNative exists to be faster than the emulation, and at the dock it is not yet: 12 ms of GPU time
  facing the town against about 5 ms for the Xenos path at the same settings. That has to be understood
  before more features are built on the current draw path.

## D30 — Render targets bound only when they change (2026-10-14)

**Decision:** the draw path remembers the render targets it bound last and calls `SetRenderTargets` only
when a draw's targets differ. The renderer tells it (`DrawPath::TargetsChanged`) when a clear, a resolve or
Present binds other targets.

**Why:** DiligentCore's Vulkan backend adds a barrier after every transition out of a writable state, even
to the same state (`TransitionTextureState`: "Always add barrier after writes"). Binding the same render
targets again therefore placed a barrier, and a barrier ends the render pass (dynamic rendering). Every one
of the dock's ~1,100 draws ran in its own render pass and stored and reloaded the RGBA16F scene target and
its depth buffer: about 10 µs per draw on an RTX 4070 Ti, whatever the draw. That was the "80 % of the GPU
facing the town" Gustavo saw.

**Evidence:** `local_docs/06-phase3-performance.md`. At the dock, 1280×720, AA Off:
- GPU time 12.1 → 3.3 ms per frame facing the town, 2.8 → 1.8 ms facing the sea;
- renderer CPU time 5.35 → 4.3 ms;
- render passes per frame 1,145 → 173, barriers 1,325 → 320.

The image is unchanged.

**Rejected:** a DiligentCore patch that skips same-state barriers for render targets. Not binding what is
already bound is enough, and keeps the patch series at one entry.

## D31 — Phase 3 holds the renderer's own work; POTCAWE's polish moves to Phase 4, the second game to Phase 5 (2026-10-14, Gustavo)

**Decision:**
- **Phase 3** is the work that changes how XenosNative renders, in this order:
  1. the occlusion queries (the sun glare);
  2. DXT1 textures kept compressed on the host (BC1);
  3. shader translation off the render thread;
  4. the guest memory uploads, step A (one batch at the start of the frame); step B (the draws of a pass
     recorded after all of their uploads) only if step A's measurements call for it;
  5. the render resolution chosen from the window, at any scale, not only whole multiples of 1280×720.
- **The game's MSAA is not brought to XenosNative.** The antialiasing XenosNative offers will be its own,
  a modern post-process AA (FXAA or SMAA) or host MSAA, applied to the scene before the HUD is composited.
  Until then the Antialiasing option stays forced off with XenosNative (D25).
- **Phase 4** is POTCAWE's polish: that antialiasing, every level compared with the Xenos path, the
  known limits from Phase 2 as scenes need them, a persistent pipeline cache, long sessions, ultrawide
  output. It ends with the decision to make XenosNative POTCAWE's default.
- **Phase 5** is the old Phase 4: a second game (GRAW) and the library's extraction.

**Why:**
- The game's 2× MSAA exists to fit EDRAM: it is what splits the scene into two tiles, and POTCAWERecomp
  needs hooks to make it work (`src/msaa.cpp`). XenosNative draws the scene once into a plain texture, so
  its antialiasing can be any host technique. Gustavo prefers a modern one of XenosNative's own.
- Whole multiples are a limit of the Xenos path's EDRAM emulation. XenosNative's render targets and
  resolve results are ordinary textures, so the scale can follow the window (a 1920×1080 window wants
  1.5×). The SDK's translator bakes the scale into shaders as an integer in three places, which a patch
  will have to turn into a constant.
- Resolution comes last because it touches almost every part of the renderer: the earlier items are
  validated at 1× first, and the occlusion queries already scale their counts (D32).
- The old Phase 3 mixed renderer work with walking the whole game; the old Phase 4 was the second game.
  With polish as its own phase, Phase 3 has a measurable end and Phase 5 starts from a finished POTCAWE.

Gustavo agreed to the list, the order and the split after a viability analysis (2026-10-14: "Me parece
bien ese orden y la división en Fase 4 y 5").

## D32 — Occlusion queries measured on the host and answered when the command processor ends them (2026-10-14)

**Decision:**
- **The tracker reads Query Issue's packets.** Issue's first argument is the query, not the device, but
  the library writes its packets into the device's command buffer. A new call flag, `kPackets`, makes the
  state tracker read such a call with the device of the last device call. The tracker also records the
  register writes of bin-predicated `SET_CONSTANT` packets (`PacketScan::predicated_writes`), because the
  library writes the query's `RB_SAMPLE_COUNT_ADDR` (0x2325) in them, one per tile. They are not applied
  to the register file.
- **Host queries bracket the guest's.** `Issue(BEGIN)` (flags & 2) begins a DiligentCore occlusion query.
  `Issue(END)` (flags & 1) ends it and keys it by the end event's sample count address: the last 0x2325
  write of the call.
- **Outside any render pass.** A Vulkan query begun outside a render pass must end outside it, and the
  draws in a guest query can span several render passes. DiligentCore's API has no call that only ends
  the render pass, so a state change on a 16-byte buffer forces a barrier, and a barrier ends it (D30).
- **No waiting.** Once per frame, after the submit, the results that arrived are collected in order
  without waiting. The latest count per sample count address is kept.
- **The command processor writes the answer.** A new SDK patch, `xenos_native/sdk_patches/0003`, adds
  `IGraphicsSystem::SetOcclusionQueryResolver`:
  - at an `EVENT_WRITE_ZPD` whose block holds the end marker (`0xFFFFFEED`), the command processor asks
    the resolver;
  - the answer becomes `Total_A` and `ZPass_A`, with every other count zeroed;
  - `ZPass_A` and `ZPass_B` are written last, in one 8-byte store after a release fence, so the D3D
    library never sees half a result: it takes a query as finished as soon as either word loses the
    marker.
  - Without a resolver, or if it declines, the fake count stays as before. The Xenos path never sets one.
- **The answer is the latest count that arrived** for that address, one or two frames old. Before the
  first result it is 0 samples (not visible).
- **Counts are reported at the guest's resolution:** divided by the render scale's area
  (`OcclusionQueries::SetSampleScale`). It is 1 until the resolution work (D31).

**Why:**
- POTCAWE's sun flare draws a quad at the sun inside a query (`sub_8244F4A8`), reads the previous frame's
  result (`sub_824521F8`), and sets the glare's strength from the visible fraction. In sync-only mode the
  command processor reported its fake 1000 samples, more than the quad can produce, so the glare was at
  full strength and drawn over the geometry in front of the sun. Gustavo saw this at the dock after D26.
- **The SDK has to write the answer.** The command processor executes the end event later than the
  game's Issue call, on its own thread, and overwrites the block with the fake count. If XenosNative
  wrote guest memory itself, the two writers would race. With the resolver, the command processor is the
  only writer, as on the Xenos path.
- **Waiting for the exact result would stall the game.** At the end event the host GPU has not run the
  query yet: XenosNative records on the game's thread and submits at the end of the frame, with up to two
  frames in flight. Waiting would hold the command processor, and with it the game, for up to two frames
  per query. The Xenos path's own host queries do a full GPU wait per query end (patch 0020); that is
  what XenosNative avoids. A result one or two frames late cannot be seen in a flare's strength, and
  POTCAWE reads last frame's result anyway.
- **Keyed by address.** The game reuses its query objects every frame, and the end event's address is
  what the command processor sees. At the dock the same two blocks come back every frame.
- **Several tiles.** XenosNative draws a tiled pass once, so the whole count belongs in one block: the
  last tile's, which is the one that holds the end marker. The other tiles' blocks get 0 from the
  command processor's unchanged path, and GetData adds the blocks. This is not exercised yet: with
  XenosNative the scene has one tile (Antialiasing forced off, D25).

**Evidence:** `local_docs/06-phase3-performance.md`, runs 101–108. At the dock, 1280×720, AA Off:
- every frame: 2 queries begun, ended, measured and answered; 1,200 per 600 frames, none unanswered once
  the first results arrived;
- samples: 0 with the sun hidden or off screen, up to 104 per query facing it;
- three views of the sun, compared with the Xenos path at the same spot: the glare matches, and the
  white-out over the geometry is gone;
- 10 observer verifications of 2 frames on the Xenos path: every draw matches, 0 calls not fully read;
  reading Issue's packets did not disturb the tracker.

**Rejected:**
- Writing the counts into guest memory from XenosNative: two writers, as above.
- Exact, same-frame results by waiting on the host GPU: the stall above.
- Ending the render pass through a DiligentCore patch: the barrier needs no patch, and keeps the patch
  series at one entry.

## D33 — DXT1 kept compressed as BC1 with 1-bit alpha; textures counted at their host size (2026-10-14)

**Decision:**
- **A second DiligentCore patch.** `xenos_native/diligent_patches/0002` maps `TEX_FORMAT_BC1_*` to
  `VK_FORMAT_BC1_RGBA_*`, and those back to `TEX_FORMAT_BC1_*`. DiligentCore's D3D11 and D3D12 backends
  get the same decoding from DXGI's BC1.
- **DXT1 is loaded as BC1.** `k_DXT1` and `k_DXT1_AS_16_16_16_16` load into `BC1_UNORM` through the
  SDK's 64bpb load shader, which only untiles and byte-swaps the 8-byte blocks. This is what both of the
  SDK's backends do.
- **Fallbacks.** A texture whose size is not a multiple of 4 is decompressed to RGBA8, as before and as
  for the other block formats. Every block format is decompressed when the device has no BC formats (the
  `TextureCompressionBC` feature is optional), as the SDK's Vulkan backend does.
- **Host sizes.** Host textures report their real size to the SDK's texture cache: every mip and slice,
  from DiligentCore's `GetMipLevelProperties`. Before, they reported the base level at the guest's bits
  per pixel. The log shows how many guest textures are on the host and their size.

**Why:**
- **Memory.** DXT1 is 4 bits per texel. Decompressed to RGBA8 it takes 8 times the memory, and its load
  writes 8 times the data.
- **Alpha.** Vulkan's BC1_RGB formats decode the fourth color of a three-color block (first color not
  greater than the second) as opaque black. DXT1, like Direct3D's BC1, decodes it as transparent:
  alpha-tested DXT1 textures would get black borders with BC1_RGB. That is why D23 decompressed DXT1.
- **The memory limits.** The SDK's texture cache destroys textures unused for 30 s above its soft limit
  (384 MB), and any unused texture above its hard limit (768 MB). Counting decompressed textures at
  4 bits per texel and without their mips hid most of their real size from those limits.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 109–113. At the dock, 1280×720, AA Off:

| | DXT1 decompressed (run 109) | DXT1 as BC1 (runs 110, 111, 113) |
|---|---|---|
| Guest textures on the host | 412–460, **276–318 MB** | 412–458, **128–139 MB** |
| Video memory in use (RTSS) | 3,968 MB | 3,763 MB |
| Data written by texture loads, from the start to the dock | 1,821 MB | 1,673 MB |
| GPU time facing the town | 3.14–3.30 ms (135–145 copies) | 3.14–3.17 ms (118–135 copies), 3.5–3.6 ms (159–170 copies) |

- The GPU time follows the number of guest memory copies of the run (the render pass breaks, D30), not
  the texture format.
- The data written by loads hardly changes: most of it is the menus' video planes, which are not DXT1.
- The image is the same: at the start view, 57.6 % of the pixels are identical before and after, and
  41.9 % differ by less than 16 levels. Two runs of the same build give 83.1 % and 16.6 %. The extra
  small differences are the hardware's BC1 interpolation against the SDK's decompression shader. The
  alpha-tested foliage has no black borders.

**Rejected:** keeping DXT1 decompressed until a DiligentCore release decodes BC1 with alpha. The patch
changes a few lines and decodes BC1 the way DiligentCore's D3D backends already do.

## D34 — Shaders translated on worker threads; draws wait by being skipped (2026-10-14)

**Decision:**
- **Translation leaves the render thread.** Two worker threads translate guest shaders to SPIR-V and
  create their DiligentCore shaders. Each worker has its own `SpirvShaderTranslator`, which keeps
  per-translation state. On a machine with fewer than four hardware threads there is one worker.
- **Analysis stays on the render thread.** The SDK's analysis runs when a draw first uses a microcode. It
  costs about 0.02 ms, and the draw needs its results right away to choose the shader modifications.
- **The handoff.** The render thread creates the `Translation` object (`GetOrCreateTranslation`) and
  queues it. The worker translates it and stores the shader, then publishes the state (pending, ready or
  failed) with a release store. The render thread reads that state with an acquire load.
- **Draws wait by being skipped.** Until both of its shaders are ready, a draw is skipped, as when its
  pipeline is still compiling (D28). The log counts the two cases apart. Analyses and pipelines are timed
  on the render thread, translations on the workers.
- **Shutdown.** The destructor sets an atomic flag, wakes the workers and joins them, without taking the
  queue's lock. Workers also wake every 50 ms on their own, so a missed notification cannot hang the exit.

**Why:**
- **The cost at a level load.** D28 measured translation at up to 2.7 ms per shader, about a hundred
  times per level load. At the dock that is 93 translations and 75 ms of the render thread, in the frames
  where the game is also loading.
- **Same treatment as pipelines.** Skipping draws for a few frames is already how new pipelines are
  handled (D28) and how the Xenos path handles asynchronous pipelines. An object appearing a frame or two
  late during a load cannot be seen.
- **Thread safety.** The translator only reads the analysis of the translation's shader, which no longer
  changes, and writes into the `Translation` it was given. The render thread may add translations to the
  same shader meanwhile, but the worker never touches that map. DiligentCore creates device objects from
  any thread.
- **Two workers.** The load's 93 translations take about 90 ms of CPU, so two workers finish within a
  few frames. More would compete with the game's threads and DiligentCore's four compilation threads,
  which work at the same moment.
- **Precedent in the SDK.** It gives each thread its own translator when it loads its shader storage
  (`d3d12/pipeline_cache.cpp`), and both of its backends compile pipelines on creation threads.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 109–116. At the dock, 1280×720:

| | Translation on the render thread (runs 109, 110, 113) | On worker threads (runs 114, 116) |
|---|---|---|
| Translations at the dock's load | 93, 75.3 ms on the render thread (max 2.6 ms) | 93, 84.7–87.6 ms on the workers (max 3.1 ms), none on the render thread |
| Longest frame in the 600 frames with the load | 79.6–83.2 ms | 51.9–53.9 ms |
| Draws skipped in those frames | 382 while their pipeline compiled | 1,368–1,496 while a shader was translated, 595–631 while their pipeline compiled |
| Longest frame during play at the dock | 18.4–19.0 ms | 18.4–20.0 ms |

- The image at the start view, 25 s after the load, differs from earlier runs no more than two runs of
  one build differ from each other.
- A normal window close exits in 0.5 s with the workers running (run 115).
- No translation failures.

**Rejected:**
- **Analysis on the workers too.** It would add a second wait for no gain: it is 1.7 ms for the whole
  load.
- **One translator behind a lock.** That serializes the translations, and the render thread would block
  on the lock whenever it needs the translator's default modifications.

## D35 — Guest memory pages are copied only when their data changed; this replaces upload step A (2026-10-14)

**Decision:**
- **A hash per page.** The guest memory mirror keeps a 64-bit hash (XXH3) of the data last copied into
  each 4 KB page (1 MB for the 512 MB mirror).
- **Unchanged pages are not copied.** When the SDK's `SharedMemory` uploads invalid pages, a page whose
  current data has the same hash is marked valid without a copy. The others are copied in contiguous
  runs.
- **Copies come from a snapshot.** The pages are copied into a CPU buffer first, hashed there, and
  uploaded from it. The CPU may write a page at any moment, so the recorded hash must describe exactly
  the bytes the mirror received.
- **The log**, every 600 frames: buffer copies per frame (average and maximum), the guest memory copied,
  and the guest memory requested again but unchanged.
- **This replaces step A** as D31 planned it (one batch of uploads at the start of the frame). Step B
  (the draws of a pass recorded after all of their uploads) is not needed for now.

**Why: the measurement that changed the plan.** A probe, since removed, logged every upload of frames
4000–4005 at the dock, with the draw and what requested it, and hashed every uploaded page (runs
117–118):
- **The uploads.** About 133–178 per frame, about 3 MB: 105 for index data, 33–47 for vertex data,
  25–26 for texture data.
- **The same ranges every frame.** 131 of about 150 ranges were uploaded again in the next frame, at the
  same draws.
- **Few real changes.** Only 6–7 uploads per frame (13–16 pages) held data that had changed. They are
  vertex data the CPU writes just before draws 41, 607 and 1062–1122, and one index range. Over 600
  frames at the dock, 2.5 % of the uploaded pages had changed.
- **The cause.** The pages are invalidated without being changed: `SharedMemory` invalidates more than
  the CPU writes. POTCAWE writes reference counts next to its data every frame (`sub_8242D218`), which
  D23 had already found for textures.
- **Step A would only have moved the copies.** It would have moved about 130 copies of unchanged data to
  the start of the frame, still 3 MB per frame. It could not have moved the few that change, because the
  CPU writes them during the frame, right before their draws.
- **Skipping them removes the copies**, and it needs no knowledge of when the game writes.

**Correctness:**
- **The mirror holds the last snapshot.** Nothing on the GPU writes the mirror (no memory export;
  resolves write host textures), so each page holds exactly the last snapshot copied into it.
- **Writes during the check.** Pages are made valid (write-protected) before they are hashed, as they
  were before being copied. A write during or after the hash invalidates the page again, and its next
  use checks it again.
- **First use.** A hash of 0 means unknown, so a page is always copied the first time.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 117–121. At the dock, 1280×720, AA Off:

| | Before (runs 109–116) | After (runs 119, 120) |
|---|---|---|
| Buffer copies per frame, start view | 118–170 | 7–9 (9.4–12.2 on average over 600 frames) |
| Guest memory copied per frame | 2,664–3,192 KB | 60–80 KB, and about 2,500 KB found unchanged |
| GPU time facing the town | 3.14–3.60 ms | 1.55–1.66 ms |
| Renderer CPU time per frame | 4.34–4.48 ms | 3.99–4.11 ms |
| Longest frame during play | 18.4–20.0 ms | 18.3–19.0 ms |

- **The image is unchanged.** The start view is within run-to-run variation.
- **A three-minute walk** around the dock (run 120): NPCs, subtitles, HUD and shadows are correct, no
  errors, 1.0–1.7 ms of GPU time.
- **The main menu's video** still plays (run 121).
- **Over the session**, XenosNative's GPU time at the dock went from 12.1 ms (Phase 2's close) to 3.3 ms
  (D30) to 1.65 ms. RTSS shows 14–19 % of the GPU, against about 30 % for the Xenos path at the same
  settings.

**Rejected:**
- **Step A as planned**, for the reasons above.
- **Finer invalidation in `SharedMemory`.** That is an SDK change to a design the Xenos path shares;
  hashing solves it in XenosNative without a patch.
- **Hashing the guest memory and copying it directly**, without a snapshot. It saves one memcpy, but a
  CPU write between the hash and the copy could leave the mirror and its recorded hash disagreeing.

## D36 — XenosNative applies the guest's display gamma ramp when presenting (2026-10-14, Gustavo)

**Decision:**
- **SDK patch 0004.** `xenos_native/sdk_patches/0004` adds `IGraphicsSystem::GetGammaRamp`. It copies
  the ramps the command processor tracks from the DC_LUT register writes: the 256-entry table and the
  piecewise linear (PWL) ramp, with a version that changes on every write. A mutex taken only where those
  writes change an entry makes the copy safe from the render thread.
- **A gamma pass before the scaling.** Present maps the front buffer through the ramp, texel by texel and
  at the front buffer's size, into an RGBA16F texture. That texture is then scaled into the window as
  before. This is the order of the SDK's presenters.
- **The SDK's arithmetic and choice.** The pass computes what the SDK's `apply_gamma` shaders compute:
  - the table maps each 8-bit channel to 10 bits;
  - the PWL takes the 10-bit value, picks an entry with its 7 high bits and interpolates with the low 3.
  As the SDK does, the table is used for 8_8_8_8 front buffers and the PWL for 2_10_10_10 ones.
- **Updates.** The ramp texture (R32_UINT, 256×4: the table, then the PWL's red, green and blue) is
  rewritten only when the version and the content changed. The log shows both ramps at a few levels
  whenever they change, and which ramp the front buffer's format selects.
- Done in Phase 3 rather than Phase 4, where the plan had put it, at Gustavo's request: he found the image
  flat compared with the Xenos path.

**Why:**
- **The image was too light.** Without the ramp the image was lighter in the darks than on the Xenos
  path. A level of 48 came out as 32 there, and 128 as 116 (the D33 checks). Gustavo also saw less
  contrast while playing.
- **POTCAWE's ramp.** It presents a `2_10_10_10_AS_16_16_16_16` front buffer (format 54), and writes a PWL
  ramp that darkens the low levels. Red at 32, 64, 128 and 192 of 255 gives 67, 196, 467 and 748 of 1023;
  a straight line gives 129, 259, 518 and 776. The 256-entry table stays at its default (linear).
- **The GPU applies it after the frame.** The display controller applies the ramp to the front buffer
  after the frame is finished. So it belongs to presenting, not to the draws, and only the command
  processor sees the register writes.
- **Thread safety.** The command processor writes the ramp on its own thread, and the SDK's presenters
  read it there. XenosNative presents on the game's render thread, hence the copy under a lock.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 123–125. The start view at the dock, compared
with the Xenos path:
- **Tone curve, XenosNative to the Xenos path** (green, average per level):
  - before: 24 → 10, 48 → 32, 128 → 116, 192 → 186;
  - after: 24 → 23.8, 48 → 47.1, 128 → 127.1, 192 → 191.8, the diagonal.
- **Pixels differing by 16 levels or more:** 30–31 % before, 1.9–2.4 % after.
- **Pixels within 6 levels:** 2.8 % before, 87 % after. The remaining small differences are the two
  renderers'; the same comparison between two runs of one build gives 97–98 %.
- **The Xenos path is unchanged** with the patch (run 125): no errors, 87.7 % of the pixels identical to
  its earlier screenshot.

**Rejected:**
- **Applying the ramp in the scaled copy**, after the bilinear filter. The ramp would act on filtered
  values, and the result would differ from the SDK's whenever the window is not 1:1.
- **Hooking the D3D library's gamma functions.** That needs addresses per game. The register writes reach
  the command processor in every game.

## D37 — The render resolution follows the screen, at any scale, with no selector (2026-10-14, Gustavo)

**Decision:**
- **The scale comes from the output.** XenosNative renders at the largest scale at which the game's 16:9
  image fits the output: the monitor in fullscreen, the window's client area otherwise. The scale is
  `min(output width / guest width, output height / guest height)`, and the guest size comes from the
  game's profile (POTCAWE: 1280×720). It need not be a whole number:

  | Output | Scale | Rendered |
  |---|---|---|
  | 1366×768 | 1.067 | 1365×768 |
  | 1920×1080, 1920×1200, 2560×1080 | 1.5 | 1920×1080 |
  | 2560×1440, 2560×1600, 3440×1440 | 2 | 2560×1440 |
  | 3840×2160 | 3 | 3840×2160 |

- **No scale selector with XenosNative.** The launcher shows the resulting resolution instead of the
  scale row, which stays for the Xenos path. A setting outside the launcher (`potcawe.toml`) can force a
  scale, for a weak GPU behind a large monitor.
- **Bounds.** The scale is at least 1: in a window smaller than the game's resolution the game still
  renders at 1280×720, and Present reduces the image. The scale is at most what the device's largest
  texture allows.
- **Chosen at startup.** A window resized during play scales the presented image; the render resolution
  changes at the next start.
- **Everything the game renders is scaled, shadow maps included,** as the Xenos path does at 2×2 and 3×3.
  Textures loaded from guest memory keep their size, except those that resolves produce.
- **Separate scales per axis** throughout the renderer, equal for now, so that a later change of aspect
  ratio only has to give them different values.
- **Other aspect ratios move to Phase 4.** Filling a screen that is not 16:9 (no bars on 16:10, ultrawide
  or the Steam Deck) needs the game's camera projection, its HUD and menus, and any effect that assumes
  16:9 to change. That is game-side work, done together for every aspect ratio. Until then the image is
  16:9 with bars.
- **The translator's scale.** SDK patch `xenos_native/sdk_patches/0005` lets the SPIR-V translator revert
  a fractional scale (`SetFractionalResolutionScale`). The translated shaders revert the scale in two
  places, both of which XenosNative needs:
  - the pixel position of parameter generation;
  - the texel offsets and weights of resolution-scaled textures.
  The whole-number scale keeps its uses in EDRAM emulation and memory export, which XenosNative does not
  use. The SDK's own backends do not call it, so the Xenos path is unchanged.

**Why:**
- **Gustavo's requests (2026-10-14).**
  - No fixed scales: "que todo se calcule en base a resoluciones". A 1366×768 or 1920×1200 monitor
    should get exactly its resolution, not the next multiple.
  - Shadows scaled like the rest, as on the Xenos path.
  - No bars, but together with the ultrawide work.
- **Integer scales waste GPU time.** They are a limit of the Xenos path's EDRAM emulation. For 1600×900
  or 1920×1200, its automatic scale renders 2560×1440: 2.6 and 1.8 times the pixels needed. For 1366×768
  it stays at 1× and stretches the image.
- **XenosNative can do any scale.** It has no EDRAM: its render targets and resolve results are plain
  textures of any size. Vulkan viewports take fractional positions and sizes, so the SDK's viewport
  helper can run at scale 1 and its result be multiplied.
- **A minimum of 1** keeps the game's smallest targets (its luminance and bloom chain) at least their
  guest size, for almost no saving below it.

**Evidence:** to come with the implementation (`local_docs/06-phase3-performance.md`). The first check is
at exactly 2× against the Xenos path at 2×2, then fractional scales (1.067, 1.25, 1.5).

## D38 — How the renderer applies the render scale (2026-10-14)

**Decision:**
- **One rounding rule.** `RenderScale` (`src/scale.h`) holds a float per axis, fixed at startup. A guest
  pixel coordinate becomes a host one as `floor(guest × scale + 0.5)`. The rule is the same for both edges
  of every rectangle and for sizes counted from 0.
- **Host sizes.** Render targets and resolve textures are created at that size, at least 1 pixel. They
  stay keyed by the guest's surface and texture.
- **Viewports.** The SDK's viewport helper runs at scale 1, in guest pixels. Its NDC transform is
  relative to the viewport, so XenosNative multiplies the viewport's offset and extent by the scale as
  floats. Vulkan takes fractional viewports.
- **Point sizes** keep `1 / guest extent`, which equals the SDK's `scale / host extent`.
- **Scissors** are converted edge by edge with the rule.
- **Resolves** are clipped in guest pixels, as before, and both rectangles are then converted. With a
  fractional scale, their rounded sizes can differ by one pixel when their offsets differ; the copy takes
  the smaller.
- **Textures from resolves.** The system constant `textures_resolution_scaled` has a bit set for every
  fetch constant bound to a resolve result, so that the translated shaders revert the scale in texel
  offsets and weights, as the SDK does.
- **The translators**, on the render thread and on the workers, get the scale through
  `SetFractionalResolutionScale` (`sdk_patches/0005`).
- **Occlusion queries** divide their counts by `scale x × scale y` (D32's sample scale).
- **The output and the guest size.** The output is the swap chain at startup: the monitor in
  fullscreen, the client area in a window. The guest size is the profile's `output_width/height`
  (POTCAWE: 1280×720).
- **Bounds and override.** The scale is clamped to 1 and to `MaxTexture2DDimension / max(guest width,
  height)`. The cvar `xenos_native_resolution_scale` (a double, 0 = from the output) forces a scale.
- **Present** is unchanged. The front buffer comes out at the render size, the gamma pass runs at that
  size (D36), and the copy to the window is 1:1 when the output's 16:9 rectangle is the render size.

**Why:**
- **No gaps or overlaps.** A single rounding rule for edges means two rectangles that touch in guest
  pixels touch on the host. That covers per-tile resolves of one texture, and viewports and scissors
  that tile a target.
- **Viewports.** Scaling them as floats keeps the SDK's half-pixel offset and its handling of the
  guest's viewport in guest units: one tested code path, at every scale.
- **Texel offsets in resolved textures are host texels in the SDK** (it divides them by the scale), so
  manual filtering in game shaders stays consistent with the larger textures. Doing the same keeps
  XenosNative equal to the Xenos path at 2×2.
- **Everything scales uniformly**, the smallest targets included (the 1×1 luminance targets become 2×2
  at 2×), as on the Xenos path. That is what the 2× comparison checks.

**Evidence:** `local_docs/06-phase3-performance.md`, runs 127–139:
- **Scales.** Automatic from windows of 1366×768 (1.067 → 1365×768), 1600×900 (1.25) and 1920×1080
  (1.5), and fullscreen on a 2560×1440 monitor (2.0). Forced 1, 1.4, 1.6 and 2 in other windows.
- **No errors in any run.** The render targets and resolve textures are logged at their host sizes.
- **Parity at exactly 2×.** XenosNative fullscreen at 2560×1440 against the Xenos path at 2×2 on the
  same monitor, 1:1, start view below the overlay:
  - the tone curve is the diagonal;
  - 70.0 % of the pixels are within 2 levels, 22.5 % within 3–6, and 7.5 % differ by 7 or more;
  - at 1× the same comparison gave 55.5 %, 31.3 % and 13.3 %.
- **GPU time facing the town:**

  | Scale | 1 | 1.067 | 1.25 | 1.4 | 1.5 | 1.6 | 2 |
  |---|---|---|---|---|---|---|---|
  | ms | 1.55–1.65 | 1.68–1.74 | 1.95–1.99 | 2.19–2.29 | 2.35–2.51 | 2.53–2.74 | 3.32–3.84 |

- **Occlusion queries:** at most 92–98 samples per query facing the sun at 1.067, 1.25 and 2, in the
  range of 1× (104, a different framing). The counts are at the guest's scale.
- **Not the renderer:** captures of a 1920×1080 window come out posterized on both paths, the Xenos path
  at 1×1 too. Other window sizes and fullscreen do not. It is in this machine's display or capture,
  and the tests avoid that window size.

**Rejected:**
- **Rounding sizes up and edges to nearest.** Two rules would disagree on where a rectangle that starts
  at 0 ends.
- **Keeping the smallest targets (1×1, 4×4) at 1×.** The Xenos path scales them too, and the 2×
  comparison matches.

## D39 — Phase 3 closed; Phase 4 (POTCAWE polish) next (2026-10-14, Gustavo)

**Decision:** Phase 3 is closed on its exit check (`plan.md`), and Phase 4 starts. Gustavo chooses the
order of its items.

**What Phase 3 changed**, at the dock:
- GPU time 12.1 → 1.65 ms per frame at 1×: render targets bound once (D30), guest memory copied only
  when it changed (D35);
- the sun's occlusion queries measured on the host (D32);
- DXT1 kept compressed (D33);
- shader translation off the render thread (D34);
- the display gamma ramp applied (D36);
- the render resolution following the screen at any scale (D37, D38).

**Why:**
- **Every item is done.** The last was the A/B against the Xenos path, with both paths in one session.
- **XenosNative is faster at the same resolution.** It uses 38–56 % less of the GPU without
  antialiasing, and holds 60 FPS at 3× where the Xenos path with 2× MSAA drops to 33.
- **No frame over 20 ms at the dock after the load:** 18.4–19.0 ms at most in every run.
- **What remains is polish, not renderer work:** antialiasing of its own, the rest of the game, other
  aspect ratios, a persistent pipeline cache and long sessions.

Gustavo: "Dala por cerrada!" (2026-10-14).

## D40 — Phase 4 starts with antialiasing, aspect ratios and a pipeline cache; temporal upscaling and frame generation become Phase 5 (2026-10-14, Gustavo)

**Decision:**
- **Phase 4 (POTCAWE polish) starts with three items:**
  - XenosNative's own antialiasing, FXAA and SMAA, on the scene before the HUD;
  - any aspect ratio without bars (ultrawide, 16:10, the Steam Deck);
  - a persistent pipeline cache.
  Its other items follow: every level compared with the Xenos path, long sessions, and the known limits
  from Phase 2.
- **The order of work:**
  1. antialiasing, which needs the frame's HUD composite located (the aspect ratio work needs it too);
  2. the pipeline cache, which is self-contained;
  3. aspect ratios, the largest and least certain item: it needs the game's camera, its HUD and its
     menus.
- **A new Phase 5: temporal upscaling and frame generation.** It covers:
  - the motion vectors these need (camera motion first, then moving objects);
  - DLSS / DLAA and FSR 3 as quality options (DiligentCore's SuperResolution module already has DLSS on
    Vulkan);
  - frame generation, to show more than 60 FPS without touching the game's timing.
- **The old Phase 5** (a second game and the library's extraction) **becomes Phase 6.**

**Why:**
- **Gustavo's request** (2026-10-14): "deja lo del DLSS y FrameGen para una Fase 5. De momento
  enfoquémonos en el Antialiasing propio (FXAA, SMAA), la proporción de pantalla sin bandas (ultrawide,
  16:10, Steam Deck) y la caché persistente de pipelines".
- **Temporal techniques need motion vectors,** which a 2007 game does not produce. Making them is
  research of its own, while FXAA and SMAA need only the final image.
- **Upscaling buys little speed here:** XenosNative already takes 3.3 ms at 1440p and 5.85 ms at 4K on
  an RTX 4070 Ti. Their value is image quality (DLAA) and frame rates above 60 (frame generation), both
  after the polish.

## D41 — XenosNative's antialiasing: SMAA or FXAA on the copy of the finished scene, named by the profile (2026-10-14)

**Decision:**
- **Two methods**, chosen at startup by the cvar `xenos_native_antialiasing` (`smaa` by default, `fxaa`,
  `off`), and by POTCAWE's launcher in its Antialiasing row when XenosNative is the renderer:
  - **SMAA 1x**, preset High, from its authors' reference implementation, vendored unchanged in
    `xenos_native/thirdparty/smaa` (iryoku/smaa `71c806a`, MIT). CMake embeds `SMAA.hlsl` as a string
    and it is compiled as GLSL (`SMAA_GLSL_4`) at startup. Three passes: luma edges, blending weights
    with the area and search lookup textures, neighborhood blending.
  - **FXAA**, the SDK's (its Vulkan presenter's normal quality), as one pixel shader that computes the luma
    itself.
- **Where: on the copy of the finished scene.** The game's profile names the Resolve that copies the scene
  after the post-processing and before the HUD (`Profile::scene_resolve_caller`, the call's return
  address); POTCAWE's is `0x824583EC`. Right after that copy, its texture is antialiased in place: a copy
  of it is the input, and the last pass writes the texture. Without a named resolve there is no
  antialiasing.
- **The trace now prints each call's caller** (" from 0x…", the return address), to find such a call in a
  new game.

**Why:**
- **The HUD must stay sharp.** The game's MSAA is not brought over (D31), so the antialiasing works on the
  image. Applied to the front buffer it would blur the HUD and the menus' text. POTCAWE copies the
  finished scene to `0x09B2E000`, then draws the HUD in the same EDRAM, then composites that copy and the
  HUD into the back buffer (traced frame below). The copy is the last point before the HUD.
- **A return address names the point without a game hook.** It identifies the engine's call, it lives in
  the game's profile with its other addresses (rule 1), and the library stays general: a callback would
  need a hook in the game's code for the same information.
- **SMAA is the default.** It smooths edges as well as FXAA with less blur on textures, at a cost of about
  0.15 ms at 2560×1440. FXAA stays as the cheaper choice.
- **The images are gamma-encoded,** which both methods expect.

**Evidence:** `local_docs/07-phase4.md`, runs 156–163:
- **The trace of a frame at the dock** (run 156): the copy to `0x09B2E000` from `0x824583EC` (the scene
  blended with the last frame's copy), then the HUD in its surface, then two full-screen draws: that copy,
  then the HUD texture.
- **Applied once per frame at the dock** (600 times in 600 frames). In the menus it is applied somewhat
  more often (717 in 600 frames), and the menus' text is as sharp as without antialiasing.
- **Edges:** the dock's ropes and railings lose their stair-steps with both methods. FXAA also softens
  the textures; SMAA keeps them.
- **GPU time facing the town:** at 1×, 1.56–1.65 ms off, 1.61–1.72 SMAA, 1.59–1.69 FXAA. At 2×, 3.30 ms
  off (the A/B), 3.46 SMAA, 3.39 FXAA.

**Rejected:**
- **Host MSAA,** rendering the scene's targets with samples. It costs much more GPU (the Xenos path's 2×
  MSAA added 60 % at 2×2), it does nothing for the alpha-tested foliage, and every target, resolve and
  shader with MSAA would have to be handled.
- **Antialiasing the front buffer,** which blurs the HUD.
- **A temporal method (TAA, DLAA):** it needs motion vectors, which is Phase 5 (D40).

## D42 — The shaders, translations and pipelines are kept between runs and prepared at startup (2026-10-14)

**Decision:**
- **A record of what the game used.** XenosNative appends every new guest shader (its microcode), every new
  translation (shader and modification) and every new pipeline (its shaders, their modifications, and the
  fixed-function state without the host shader pointers) to a file per title:
  `<runtime cache directory>/xenos_native/<title id>.pipelines` (`src/storage.cpp`).
  - The file is a versioned header followed by records.
  - New records are written at the end of the frame they appeared in.
  - A record cut short (the process killed while writing) ends the file, and another version starts it
    again.
- **Prepared at the next start.** At the first draw the stored shaders are analyzed (render thread, about
  1.3 ms) and the stored translations queued to the workers. Each stored pipeline is created, up to 64 per
  frame, as soon as its shaders are translated. The draws then find everything ready.
- **DiligentCore's pipeline cache** (Vulkan's `VkPipelineCache`, the driver's binaries) is kept next to it
  (`<title id>.vkcache`). It is given to every pipeline creation, and saved once no new pipeline appeared
  for 600 frames, so not in the middle of a load. Vulkan ignores data from another device or driver.
- **On by default:** the cvar `xenos_native_pipeline_storage`. The game passes the runtime's cache
  directory to `xenos_native::Initialize`.

**Why:**
- **First use still showed.** Since D28 and D34 nothing stalls the frame, but every shader and pipeline
  seen for the first time in a run made its draws wait a few frames. At the dock's load that was about
  1,350–1,600 draws skipped while translating and 570–1,700 while compiling: objects appearing late in
  every session, not only the first.
- **Not depending on the driver's cache.** The plan asked that a new run not depend on the driver's cache,
  which a driver update clears.
- **Translations are recorded on their own, not only through pipelines.** With pipelines only, the second
  and third runs still translated 13 and 6 shaders at the dock's load. Those were draws whose translation
  never led to a pipeline (no target bound, for one), and they were translated again every run.

**Evidence:** `local_docs/07-phase4.md`, runs 164–169. At the dock's load (the 600 frames that contain it):

| Run | Stored before | Translations at the load | Pipelines at the load | Draws skipped (translating + compiling) |
|---|---|---|---|---|
| 164 (pipelines only, empty) | nothing | 93 | 91 | 1,611 + 1,678 |
| 165 | 97 shaders, 112 pipelines | 13 | 4 | 638 + 128 |
| 166 | 117 pipelines | 6 | 8 | 347 + 231 |
| 167 (with translations, empty) | nothing | 93 | 104 | 1,356 + 569 |
| 168 | 97 shaders, 108 translations, 125 pipelines | 0 | 3 | 0 + 6 |
| 169 | 128 pipelines | 0 | 0 | **0 + 0** |

- **The cost of preparing them:** at startup, 47 ms after the first draw to create the 128 pipelines, of
  which 1.3 ms of analyses and 1.5–1.9 ms of creations are on the render thread. The 108 translations
  (about 88 ms) run on the workers while the menus show.
- **The files:** 77 KB of records, and 5.3 MB of pipeline cache.
- **The longest frame of the load is unchanged** (44–53 ms): that is the game's own loading, not
  XenosNative's.

**Rejected:**
- **The SDK's shader storage.** It belongs to the Xenos path's backends: it stores their pipeline
  descriptions and creates their pipelines, which XenosNative does not use.
- **Saving the pipeline cache at exit.** The game may be killed, and a save during play happens only
  after things are quiet.

## D43 — Screens of any aspect ratio are filled: wider 3D views, 2D layers in a 16:9 box (2026-10-14)

**Decision:**
- **The renderer fills the output.** When the game's profile says its hooks widen its cameras
  (`Profile::widens_cameras`) and the output is not the game's aspect ratio, the render scale is per axis
  (D37's `RenderScale` had an x and a y for this). The vertical scale is the one D37 computes from the
  output's height (or the override), the horizontal one follows from the output's aspect ratio, and the
  game's 1280×720 image becomes, for example, 3440×1440 or 1280×800. Neither scale goes below 1.
  - `xenos_native::AspectFactor()` is the result: the output's aspect ratio over the game's, above 1 for a
    wider output and below 1 for a taller one. It is 1 otherwise, and always on the Xenos path.
  - The cvar `xenos_native_fill_screen` (on by default, read at startup) turns it off: the 16:9 image with
    bars, as before.
- **The game widens its 3D views.** A game hook multiplies the tangent of the half angle of every 16:9
  perspective camera by the factor: the width on a wider output (Hor+), the height on a taller one (Vert+).
  It does so before the game builds both the projection and the culling frustum, so its culling follows.
- **2D layers go into a box.** `xenos_native::SetGameAspectBox(bool)` marks the draws that follow as a 2D
  layer laid out for the game's aspect ratio (a HUD, menus, a video). In a box, the draw path maps guest
  pixels to host pixels with the smaller of the two scales on both axes, plus an offset that centers the
  render target's guest area in its host size. That applies to viewports and scissors; clears and resolves
  are not boxed. The game's hooks set the box around its 2D views, and Present clears it.
- **Fill-rate rectangles are opened.** A 3D effect the game clips to a screen rectangle computed for 16:9
  gets the whole viewport on the wider axis (a game hook).
- **POTCAWE** (`src/aspect.cpp`): four midasm hooks.
  - The camera builder's perspective branch (`sub_823288C8`).
  - The view setup, which reads the view's matrix: a square parallel projection is a 2D view.
  - The start of a view's post-processing (box off, since the composite covers the screen).
  - The scissor of a draw's screen rectangle (opened on the wider axis outside 2D views).

**Why:**
- **Gustavo asked for no bars** on ultrawide, 16:10 and the Steam Deck (2026-10-14). His primary monitor is
  3440×1440.
- **The camera is the only reliable place for the 3D views.** The game's own culling then agrees with what
  is drawn. Changing the matrices in shader constants would leave the culling at 16:9 and pop objects at
  the edges. It also could not tell the 3D views from the 2D ones and the shadow maps.
- **The box is the renderer's job, not the camera's.** Narrowing the 2D views' parallel camera
  (`P00 / factor`) was tried first (run 177). The HUD and the menus were right, but the game's text layout
  measures in screen pixels through that camera. It kept the glyphs at their stretched width and dropped
  what no longer fit: the pause menu showed "Reiniciar desde punto de" without "control". In the box, the
  game lays everything out exactly as at 16:9.
- **A centered 16:9 box is the usual treatment** for HUDs, menus and videos on wide screens. Anchoring
  each HUD element to the screen's edges needs per-element knowledge of the game's layout. It remains
  possible later.
- **The rectangle is only an optimization in POTCAWE.** The sun's haze at Port Royal is a large mesh
  clipped to a screen rectangle the game computes for fill rate. At 21:9 the rectangle fell short of the
  mesh, and a vertical seam showed in the sky (RenderDoc pixel history, event 1433: a scissor 2340 pixels
  wide). At 16:9, removing the clipping changes nothing (runs 182, 183).

**Evidence:** `local_docs/07-phase4.md`, runs 170–188.

| Output | Render size | GPU at the dock | Draws per frame | Result |
|---|---|---|---|---|
| 1600×900 (16:9, reference) | 1600×900 | 2.02–2.05 ms | 1,128–1,143 | no change: no fill, no hook active |
| 2560×1080 (21:9) | 2560×1080 | 2.85–2.91 ms | 1,178–1,199 | 3D Hor+ (camera ×1.3333), HUD and menus in a 16:9 box, no seam |
| 1280×800 (16:10) | 1280×800 | 1.75 ms | 1,124–1,147 | 3D Vert+ (×0.9), HUD and menus in a 16:9 box |

- **The wider view draws about 5 % more objects** (the sides). Nothing pops at the edges: the game's
  culling uses the widened frustum.
- **The Xenos path is unchanged** (run 188). The hooks do nothing when `AspectFactor()` is 1.
- **The launcher's render quality row** shows the filled size, for example 3440×1440 on the ultrawide.

**Rejected:**
- **Widening the matrices in the shader constants** (in the renderer): culling stays at 16:9, and the
  views cannot be told apart.
- **Narrowing the 2D views' camera:** the game's text layout drops words (above).
- **Hooking `sub_822FC610` and `sub_82303D60`**, the camera class with the "Projection Angle
  Width/Height" properties. Neither runs in the menus or at the dock (run 171).

## D44 — XenosNative applies the SDK's anisotropic filtering override; POTCAWE defaults to 16x (2026-10-14)

**Decision:**
- **XenosNative honors `anisotropic_override`**, the SDK's cvar, as the SDK's texture caches do. It forces
  the level on textures with mipmaps, linear filtering and no base map, and keeps what the game asks
  elsewhere. The device's maximum caps the result (`textures.cpp`; the sampler cache key already holds
  the level, so a change at run time makes new samplers).
- **POTCAWE defaults to 16x on both paths.** The value is `anisotropic_override = 5` in `potcawe.toml`
  (`src/startup.cpp`), and the launcher has an "Anisotropic filtering" row (16×, 8×, 4× (original), 2×,
  off). That part is a game setting, so it was made in POTCAWERecomp first (`f3b67db`, branch
  `rt-to-texture`, not pushed) and merged here.

**Why:**
- **A parity gap.** XenosNative used only what the game asks for. POTCAWE asks for 4x on few textures (one
  sampler: a texture with 11 mip levels, only in the run that turns toward the sea). At the dock its textures (5 and 8 mip levels)
  are plain trilinear. The SDK forces 4x on those by default (`anisotropic_override = 3`), so on the Xenos
  path the dock's planks kept their grain at an angle, and on XenosNative they blurred to a gray smear.
- **16x costs nothing measurable**, on either path (below). Gustavo asked for 16x if it was cheap.

**Evidence:** `local_docs/07-phase4.md`, runs 189–198 (`tools/gpu_ab.ps1`, the dock, 60 FPS, RTX 4070 Ti).

| Configuration | XenosNative GPU per frame | GPU % | Xenos path GPU % |
|---|---|---|---|
| 2×, what the game asks | 3.46 ms | 28.7 | — |
| 2× (2×2), 4x | 3.50 ms | 28.0 | 51.9 |
| 2× (2×2), 16x | 3.46 ms | 28.1 | 50.4 |
| 3× (3×3), 4x | 6.22 ms | 44.1 | 96.5 |
| 3× (3×3), 16x | 6.22 ms | 44.7 | 96.6 |

- **The planks** (`local_docs/evidence/aniso_compare.png`, the default view enlarged): blurred with what the
  game asks, grain with 4x, slightly sharper far away with 16x. XenosNative at 4x matches the Xenos path at
  4x.

**Rejected:**
- **A XenosNative-only cvar.** It would duplicate the SDK's, and the two paths would need two settings
  for one option.
- **Keeping 4x as the default.** It matches the Xenos path as it was, but 16x is sharper at no measured
  cost. The other launcher options keep the console's behavior by default, and this one is the exception
  (POTCAWERecomp's README says why).

## D45 — The Vulkan pipeline cache is saved on a thread of its own (2026-10-14)

**Decision:** D42's save of DiligentCore's (Vulkan's) pipeline cache runs on a thread of its own:
- the driver's serialization (`GetData`) and the file write leave the render thread;
- the file is written next to the cache and renamed over it, so a process killed while writing leaves the
  last complete cache;
- a save that fails (a pipeline added between `GetData`'s two calls makes it return nothing) is tried again
  600 frames later;
- the log line gives the save's duration.

When it is saved does not change: 600 frames after the last new pipeline.

**Why:**
- **Gustavo noticed short stutters** every few minutes in a session of about 10 minutes (`potcawe_200`,
  3840×2160, 60 FPS).
- **Each one was a save.** The six saves in the session each fall in a 600-frame window whose longest
  frame is 32–83 ms, against 18.4–19 ms in the others. The three longest (78, 81 and 83 ms) are 2–3 minutes
  apart.
- **The cache had grown to 24–25 MB.** D42 measured 5.3 MB. It probably grew with the render scales tried
  since: the translator writes the scale into the shaders (D38), so each scale adds its own binaries.
- **A save follows every few new pipelines** (2–9 when an area shows something new). So in a new area a
  save came about 10 s later, in the middle of play.

**Evidence:** `local_docs/07-phase4.md`, runs 200–202.
- Run 201 started without the records, to provoke saves while playing:
  - five saves, 28–73 ms each on their own thread;
  - the windows after them at 18.4–18.9 ms.
- Run 202 is a normal start with the records:
  - the menus' window at 30.5 ms, as before;
  - the dock at 18.4–19.0 ms, its load at 52 ms.

**Open:** in run 201 (no records, so everything new at the title screen) one frame took 460 ms and the
window averaged 21 ms, against 43–149 ms in D42's runs without records. It came a second after the save had
ended, and the save thread does not touch the render thread. Not investigated yet. It only concerns the
first start, before the records exist.

**Rejected:**
- **Saving less often only.** It would make the stutter rarer, not shorter.
- **Saving at exit.** As in D42: the game may be killed.

## D46 — Phase 5 starts with DLSS before Phase 4 closes; DLSS through DiligentCore's SuperResolution module (2026-10-14)

**Decision:**
- **Phase 5 starts now, with DLSS,** at Gustavo's request. Phase 4's remaining items stay open: every level
  compared with the Xenos path, the hour-long session, and the known limits when a scene needs them.
- **DLSS comes through DiligentCore's SuperResolution module.** That is its NGX provider for Vulkan.
  - **NVIDIA's NGX SDK:** the headers, the loader library and the DLSS runtime `nvngx_dlss.dll` (310.5.3)
    come from DiligentGraphics/DLSS-Headers, which DiligentCore fetches when configuring.
  - **The runtime** is copied next to the game's executable (`xenos_native_copy_runtime`).
  - **Not built in:** DirectSR (D3D12) and FSR 1 (its shaders need the HLSL compiler, which is not built).
- **The build passes the Windows SDK version.** The module enables DLSS only with a Windows SDK of
  10.0.26100 or later, read from `WindowsSDKVersion`, which only Visual Studio's prompts set. XenosNative's
  CMake passes the newest installed SDK that way.
- **The order of work** (`docs/plan.md`, Phase 5):
  1. find DLSS and report it (this entry);
  2. jitter the 3D views;
  3. motion vectors from depth and the camera's matrices;
  4. DLAA (DLSS at the render resolution) as an antialiasing mode;
  5. the upscaling modes;
  6. then FSR and frame generation.
- **Where DLSS goes:** before the game's post-processing, at the start of the view's post-processing (the
  point POTCAWE's hook from D43 already marks). The game's bloom and its blend with the last frame then work
  on a clean image. The 3D views are known from their matrices, and only they are jittered.
- **The cvar:** `xenos_native_antialiasing = dlaa` looks for DLSS and reports it. Until DLAA draws, SMAA is
  used meanwhile.

**Why:**
- **DiligentCore already supports it.** Its module has a DLSS provider on Vulkan, the device extensions
  NGX needs (DiligentCore enables them whenever the GPU has them), and an interface for the inputs:
  color, depth, motion vectors, jitter, exposure and reset.
- **DLAA first:** it needs no second render scale. The upscaling modes need the 3D views rendered smaller
  than the HUD and the output, which XenosNative's single scale (D37) does not do yet.
- **Before post-processing:** that is where DLSS is meant to run, and the game's post-processing starts at
  one known point.

**Evidence:** `local_docs/08-phase5.md`, run 203 (1600×900, RTX 4070 Ti): "NVIDIA DLSS is available: NGX:
DLSS", native resolution supported. The input sizes for that output:
- quality 1067×600;
- balanced 928×522;
- performance 800×450;
- ultra performance 533×300.

"Max quality" (NGX's ultra quality) is not offered and falls back to quality.

**The license:** NVIDIA's RTX SDK license allows distributing the runtime in object form as part of an
application. This repository is private. The runtime is fetched, not committed, so a published repository
would not carry it.

## D47 — DLAA: the main 3D view jittered through its viewport, camera motion vectors, DLSS before the post-processing (2026-10-14)

**Decision:**
- **The game marks its views.** `xenos_native::BeginView(matrix)` starts a view, with its view-projection
  matrix for a 3D view and null otherwise. `xenos_native::EndViewGeometry()` is called where the view's
  post-processing starts. POTCAWE calls both from `src/views.cpp`: the view setup that uploads the matrix,
  and the start of the post-processing, the same points as D43's box.
- **The jitter.** Between the two calls, the draws into targets of the game's output size (the main view's;
  reflections and shadow maps are smaller) are shifted by a sub-pixel offset through their host viewport,
  in host pixels. The offset is a Halton (2, 3) sequence of 64, one per frame. The viewport also gives the
  view's depth range.
- **The motion vectors.** At `EndViewGeometry` a pixel pass computes them from the view's depth, its matrix
  this frame and the last, its depth range (reversed when the near plane is above the far one) and the
  jitter. They follow the camera's motion only.
- **DLSS at the native resolution.** Its preset (K) is chosen by NGX for DLAA. DLSS takes the color, the
  depth (with NGX's inverted-depth flag when reversed), the vectors and the jitter. Its result is copied
  over the color, and the game's post-processing then reads the antialiased image.
  - After NGX records its commands, the command buffer is submitted and DiligentCore's state cache cleared
    (NGX binds its own compute pipelines).
  - DLAA is skipped while an occlusion query is open.
  - History starts again after a frame without the view.
- **`xenos_native_antialiasing = dlaa`** replaces the antialiasing of the finished scene. SMAA is used
  where DLSS is not available. The launcher's antialiasing row with XenosNative offers SMAA, DLAA (NVIDIA
  RTX), FXAA and off; SMAA stays the default.
- **Two DiligentCore patches:**
  - `0003`: DLSS at the output resolution is created with NGX's DLAA quality value;
  - `0004`: NGX's messages are routed to DiligentCore's log when NGX logging is on (NVIDIA's registry
    settings). The minimum level stays off, because at ON NGX writes log files next to the executable.

**Why:**
- **The viewport, not the matrix.** The game builds its matrices on the CPU and the main view shares them
  with its reflection. A sub-pixel shift of the viewport moves the rasterized image exactly, for every draw
  of the view, without knowing which constant is the projection. It also leaves the matrix unjittered for
  the motion vectors.
- **Before the post-processing.** That is where DLSS is meant to run. POTCAWE's post-processing (bloom, a
  blend with the last frame) then works on a stable image.
- **The depth range is the key.** POTCAWE clears depth to 0 and tests `GreaterEqual`: reversed depth, which
  its viewport gives as z from 1 to 0.
  - **What it caused:** the first version read depth as 0 near, 1 far. The motion vectors were noise,
    DLSS took the depth upside down, and the result was blurred (dumps of DLSS's input, vectors and output,
    run 225).
  - **Ruled out on the way:** the jitter's sign (four combinations), a one-frame offset of the jitter, the
    model (preset M or K), the HDR and exposure flags, and jittered draws that re-composite a copy of the
    scene. None changed it (runs 206–224).
  - **With the depth range:** the image is sharp. With the vectors at zero or negated it is softer again
    (runs 226–229): the vectors are right and needed, because POTCAWE's camera moves a little every frame.
- **Why the submission:** DiligentCore does not know what NGX binds. Without clearing its cache the scene
  was black (run 204). Clearing it with commands pending makes DiligentCore warn and loses track of them,
  hence the submission first.

**Evidence:** `local_docs/08-phase5.md`, runs 204–233, and the image dumps.

| Run | 1600×900, the dock | Result |
|---|---|---|
| 204 | first version, no submission | the 3D scene black |
| 205 | submission and cache cleared | correct image, but blurred |
| 206–224 | the experiments above | all blurred, except without jitter |
| 225 | dumps | DLSS's input sharp, its output blurred; the motion vectors noise |
| 226 | the depth range (reversed) | sharp; softer than SMAA only where SMAA keeps the aliasing |
| 228, 229 | vectors at zero, negated | softer: the computed vectors are needed |

| Cost (`gpu_ab`, 60 FPS) | SMAA | DLAA |
|---|---|---|
| 2× (2560×1440) | 28.1 % of the GPU, 70 W | 35.2 %, 85 W (about +1.2 ms per frame) |
| 3× (3840×2160) | 43.9 %, 122 W | 58.8 %, 161 W (about +2.5 ms per frame) |

**The log while DLAA runs:**
- **NGX:** DLSS 310.9.16, from the driver's override (`C:\ProgramData\NVIDIA\NGX\models`), not the
  bundled 310.5.3. Preset K for DLAA.
- **XenosNative's GPU timer** is not reliable with DLAA: the mid-frame submission puts the GPU's idle time
  between the two halves of the frame into the measurement. GPU utilization is the measure.

**Limits:**
- **Objects moving on their own** (characters, animated objects, water) have only the camera's motion
  vectors, so they may ghost or soften while they move. Their vectors need the last frame's transforms per
  draw: a later step.
- **Draws that sample a copy of the scene** at screen positions (POTCAWE's haze, the water) are jittered
  with the rest. That shifts their copied content twice; it showed nothing in the tests.
- **The upscaling modes** are not built: they need the 3D view at a second, lower render scale.

**Rejected:**
- **Jitter in the projection matrix** (a game hook): it needs to know the projection constant, and the
  reflection and the main view share the matrix.
- **DLSS on the finished scene,** where SMAA runs: after the game's post-processing, with a blend of
  the last frame already in it.

## D48 — DLAA: motion vectors of their own for the objects that move on their own (2026-10-14)

**Decision:**
- **Draws matched across frames.** With DLAA, each draw of the main 3D view that writes depth is matched
  with the last frame's. The key is its vertex shader, its first vertex fetch address, its index address
  and type, and its vertex count. Several draws with one key (a mesh drawn in several places) are told
  apart by their constants: the closest of the last frame's is the same object.
- **Moved or not.** A draw moved if its vertex shader's float constants changed, apart from the registers
  the whole view shares. Those are the registers that every draw using them sees with one value, and that
  half of the view's draws or more use. They are found in each frame for the next. In POTCAWE they are
  c0–c3 (the view-projection), c8, c9 and c27.
- **A motion vertex shader per translation.** The translated SPIR-V is patched
  (`spirv::MakeMotionVertexShader`):
  - a new entry point calls the shader's `main` twice;
  - in the first call every load of a float constant reads a second uniform block, the last frame's
    constants (`XeFloatConstantsPrevious`); in the second, this frame's. A select on a private flag
    chooses between the two blocks;
  - the clip-space position of each call goes to two extra outputs, locations 30 and 31.
  It is made on the shader workers, once the draw's own vertex shader is ready.
- **The motion draws.** A draw that moved is kept: its constants of both frames, the system, bool/loop
  and fetch constants, the viewport, the scissor and the index buffer.
  - **When.** It is drawn again at `EndViewGeometry`, after the camera's motion pass and before DLSS.
  - **How.** Its own state, with the depth tested equal to the scene's and not written, no stencil and no
    blending, into the motion vectors. A pixel shader turns the two positions into host pixels.
  - **Result.** Where the draw is what the scene shows, its motion replaces the camera's.
- **Left to the camera's motion:**
  - vertex shaders that sample textures (the motion pipeline binds none);
  - shaders that use the float constants other than through loads;
  - SPIR-V before 1.4.
- **`xenos_native_object_motion`** (default true) turns it off.

**Why:**
- **The draw's own shader, not the game's matrices.** POTCAWE skins its characters on the GPU. Their
  vertex shaders take all 256 constant registers: the bones in c31–c219, the world matrix in c4–c7. Running
  the draw's own vertex shader with the last frame's constants gives each vertex's exact motion, bones
  included, without knowing the game's constant layout. So the pass is a library feature, with nothing
  game-specific.
- **One function, called twice.** Cloning the function would mean telling every operand id from a literal,
  opcode by opcode. The select only rewrites loads. The driver inlines both calls and folds the flag, so
  each call reads one block.
- **Drawn at the end, depth equal.** The translator marks the position invariant and its arithmetic
  NoContraction (for multipass rendering). So the second call rebuilds, bit for bit, the depth the draw
  wrote. Testing equal against the final depth keeps only what the scene shows: what a later draw covered,
  and what alpha test discarded, keep the camera's motion. This needs no second depth target, and the
  targets change once per frame, not twice per moving draw.
- **The shared registers.** With the camera moving, every draw's constants change in c0–c3 and c9 (probe,
  run 236). Without the shared registers, every draw would be a motion draw. With them, about 150 of ~960
  draws are: Jack, the NPCs and animated props, whether the camera moves or not.

**Evidence:** `local_docs/08-phase5.md`, runs 234–245.
- **Constants probe (run 236).**
  - Camera still: 811 of ~1,019 draws per frame unchanged. About 150 changed: the skinned draws (189–191
    registers each, the bones and the world) and about 30 props (c4–c7).
  - Camera moving: every draw differs in c0–c3 and c9.
- **Reprojection test (run 239).** Frame B's color was compared with frame A's, fetched where the motion
  vectors point. Over the 81,006 pixels the objects' motion changed (5.6 %):

  | Vectors | Mean error | Median |
  |---|---|---|
  | zero | 0.093 | 0.045 |
  | camera only | 0.064 | 0.022 |
  | camera and objects | 0.032 | 0.007 |

- **Screenshots (runs 244, 245).** Jack runs toward the camera. With the camera's motion only, his hair
  leaves a trail over the pier and the dreadlocks smear; with the objects' motion they do not
  (`local_docs/evidence/object_motion_dlaa.png`).

| Cost (`gpu_ab`, 60 FPS, the dock) | Camera's motion only | With the objects' motion |
|---|---|---|
| 2× (2560×1440) | 35.5 % of the GPU | 34.9 % |
| 3× (3840×2160) | 59.3 % | 59.3 % |
| Render thread (CPU) | 4.8 ms per frame | 5.5 ms per frame (~150 motion draws) |

**Limits:**
- **Vertex data the CPU rewrites** (CPU skinning, particles in a dynamic buffer) is not followed. With the
  same constants it looks still; at a new address it is a new draw.
- **Not followed:**
  - transparent draws (no depth write), the water among them;
  - objects new this frame;
  - the frame after a history reset.
  All of them keep the camera's motion.
- **The motion pipelines are not in the stored records (D42).** They compile when a moving object first
  shows, so it has the camera's motion for a few frames. DiligentCore's pipeline cache keeps them for the
  next run.
- **A view with many moving objects** costs render thread time in proportion: about 4 µs per motion draw.

**Rejected:**
- **A motion output on every draw** (a second render target on every pipeline): it changes every pipeline
  and every guest pixel shader, and most draws stand still.
- **Comparing world matrices:** the library does not know which constants they are. The shared registers
  come from the data.
- **Cloning the vertex shader's function:** see above.

## D49 — The DLSS modes: the main 3D view at a reduced scale, upscaled before the post-processing (2026-10-14)

**Decision:**
- **The modes.** `xenos_native_antialiasing` takes `dlss_quality`, `dlss_balanced`, `dlss_performance` and
  `dlss_ultra_performance` besides `dlaa`. The launcher's antialiasing row with XenosNative offers them as
  DLSS Quality, Balanced, Performance and Ultra Performance. The input resolution is NGX's recommendation
  for the mode and the output: at 1600×900, 1067×600, 928×522, 800×450 and 533×300; at 3840×2160,
  2560×1440, 2227×1253, 1920×1080 and 1280×720.
- **The reduced targets.** While a 3D view is open (`BeginView` with a matrix, until `EndViewGeometry`), the
  targets of the game's output size get a second host texture at the reduced scale: the input size over
  the guest size, per axis. That view's draws, clears and resolves use it. Every other target (shadow
  maps, the reflection, the post-processing's) keeps the render scale. The same surfaces are used at the
  render scale after the view.
- **Resolves from a reduced target scale** into resolved textures, which stay at the render scale: color
  filtered, depth nearest.
- **At `EndViewGeometry`:**
  - DLSS takes the reduced color, depth and motion vectors, and writes its result into the surface's
    target at the render scale;
  - the view's other reduced targets are copied to theirs: the depth nearest, through a depth-writing pass;
  - if DLSS cannot run (an occlusion query open), the color is copied, filtered.
- **The view's draws:**
  - their viewports and scissors use their target's scale;
  - their samplers take the mip bias `log2(reduced / render scale)` (NVIDIA's guide);
  - the jitter keeps its Halton sequence, with `max(64, 8 × output pixels / input pixels)` phases.
- **The occlusion queries** report their samples at the game's scale from the scale of the phase they began
  in (each query keeps its own).
- **DiligentCore patch `0005`:** `SuperResolutionDesc::OptimizationType`, passed to NGX as the quality value
  when the input is smaller than the output. NGX picks the model (render preset) by it.

**Why:**
- **Resolved textures at one scale.** The translator bakes the render scale into the shaders where they
  revert it (D37, SDK patch 0005): in resolved textures' texel offsets and weights, and in the pixel position
  of parameter generation.
  - With every resolved texture at the render scale, the first stays right for every draw. The resolves
    inside the view (the scene copy and its depth, for the haze and the water) are upscaled on the way.
  - The second (VPOS) would be off by the ratio in a reduced view. No draw at the dock reads it (probe,
    run 248); a one-time warning names it if a game does.
  - So one translation, one pipeline and one stored record still serve every scale.
- **By phase, not by surface.** The scene's surfaces (`0x40243E70`, `0x40243EB0`) are drawn in the view,
  and resolved and drawn again by the post-processing after it (run 248). A second host texture per surface,
  chosen by the phase, keeps both right.
- **The depth is brought back.** The post-processing resolves the scene's depth again after the view (from
  `0x82457ED8`, for its blend with the last frame), so the target at the render scale needs it.
- **The quality value matters.** Without it, NGX created every mode as MaxPerf. With it, NGX picks its
  default model for each mode (DLSS 310: K for Quality and Balanced, M for Performance, L for Ultra
  Performance).

**Evidence:** `local_docs/08-phase5.md`, runs 248–266.
- **The frame (run 248)**, logged by a probe:
  - the reflection view (128²);
  - in the main view, the shadow maps (1024²), then the scene's clear (inside the view), 994 draws, the color
    and depth copies, and 25 more draws;
  - after `EndViewGeometry`, the post-processing's resolves of the scene's depth and color;
  - then the HUD view, which clears its own depth;
  - no draw with parameter generation.
- **Images (runs 249–253, 1600×900):** every mode gives the dock's whole image, haze, water, sun glare and
  HUD as DLAA does. The detail drops gradually down to Ultra Performance.
- **The sun's occlusion query:** its sample counts stay comparable in every mode (92–114 at the game's
  scale). Without the per-query scale, Ultra Performance would report about a tenth of them.

| Cost (`gpu_ab`, 60 FPS, the dock) | 2× (2560×1440) | 3× (3840×2160) |
|---|---|---|
| SMAA | 28.0 %, 70 W | 45.9 %, 122 W |
| DLAA | 36.8 %, 86 W | 60.5 %, 161 W |
| DLSS Quality | 32.2 %, 78 W | 51.4 %, 140 W |
| DLSS Balanced | 31.1 %, 76 W | 49.4 %, 136 W |
| DLSS Performance | 31.3 %, 81 W | 52.2 %, 146 W |
| DLSS Ultra Performance | 29.4 %, 75 W | 46.3 %, 130 W |

- **The saving is moderate.** POTCAWE's own rendering is light (about 6 ms at 3×, D38). Part of it does not
  shrink: the shadow maps, the reflection, the post-processing and the HUD at the output resolution. DLSS
  itself costs more at a larger output.
- **Performance costs more than Balanced:** NGX's default for it is the heavier model M.

**Limits:**
- **Only the main 3D view's output-sized targets are reduced.** The shadow maps stay at the render scale,
  3072² at 3×.
- **The pixel position of parameter generation** is not reverted for the reduced scale (warned once).
- **The scene copy inside the view is upscaled**, so the haze and the water sample it at the render scale
  from data at the reduced one: softer, as everything in the view.
- **The stencil of the reduced depth is not copied** to the target at the render scale. POTCAWE's HUD view
  clears it.

**Rejected:**
- **Resolved textures at the source's scale:** the baked scale would be wrong for the reduced ones.
- **A per-draw scale in the system constants** (a change to SDK patch 0005): not needed while no draw in
  the view reads its pixel position.
- **A shader per scale:** twice the translations and pipelines for the view's shaders, for the same reason
  as above.

## D50 — Resolves of 2_10_10_10_FLOAT targets take the guest format's range; SMAA unbinds its image (2026-10-15)

**Decision:**
- **The range at the resolve.** When a resolve copies a `2_10_10_10_FLOAT` render target, which is
  RGBA16F on the host, the copy first brings each value to what the guest format holds:
  - RGB clamped to the 7e3 range, 0 to 31.875;
  - alpha clamped to 0 to 1 and rounded to its 2 bits.

  The blitter's color sources (texel, MSAA and filtered) apply it when the resolve asks
  (`BlitOptions::float_7e3`).
- **SMAA unbinds the image it antialiases** before copying it and after writing it. The resolve's copy
  leaves the image bound as the render target, and SMAA's `CopyTexture` made DiligentCore unbind it with an
  info message every frame.

**Why:**
- **POTCAWE's journal** (Back), found by Gustavo in his long session: the pages showed the paused scene
  through them, in negative.
- **How the journal is drawn.** The journal is a 3D view drawn into the scene's surface (`2_10_10_10_FLOAT`),
  cleared to alpha 0.627. It is copied to a texture, and the frame's last composite blends it over the
  scene and HUD by that alpha.
- **What went wrong.** The pages' draws leave alpha above 1, which the guest's 2-bit alpha cannot hold. On
  the Xenos path the resolve packs the target into its EDRAM format, which clamps it. XenosNative copied
  the RGBA16F as it was, and the composite's `(1 - alpha)` subtracted the scene from the pages.
- **The SDK's backends do the same.** They draw `2_10_10_10_FLOAT` targets as RGBA16F with unclamped
  outputs (its "host render target path"), and their resolves read through the EDRAM format. Clamping at
  XenosNative's resolve matches what the Xenos path shows. RGB keeps its 16-bit precision (no 7e3 rounding),
  and only its range is the guest's.
- **The message flood** filled 5 MB of log in five minutes of play with SMAA and made the log rotate. It
  is older than D49: the runs that showed none had SMAA off.

**Evidence:** `local_docs/07-phase4.md`, runs 267–277.
- **The page textures are right:** dumps of the page's composition and of its 1024×512 target, before
  their resolve, show the parchment, the map and the text, nothing else.
- **The trace of the journal frame:**
  - the two pages composed and resolved;
  - the reflection, the shadows, the scene, the post-processing and the HUD;
  - the journal view (clear alpha 0.627, 46 draws, resolved as `k_16_16_16_16_EXPAND`);
  - the final composite.
- **After the change:** the journal matches the Xenos path. The dock with SMAA and with DLSS Quality
  differs from before only by run-to-run noise. No DiligentCore message with SMAA.

**Limits:**
- **Draws still blend with the unclamped values inside the target,** as on the SDK's host render target
  path. A draw whose result depends on alpha clamped before blending would still differ from the console,
  on both paths.
- **Other formats whose range the host format exceeds** (`16_16` and `16_16_16_16`, fixed point from -32 to
  32) are not brought to their range at the resolve. None has shown a problem.

## D51 — Resolves into cube textures (2026-10-15)

**Decision:**
- **The resolve.** A resolve whose destination is a cube texture writes one face: the call's `slice`
  argument. The face goes to that slice of a host cube texture, a six-slice 2D array created as a cube.
  The destination's size is decoded as a 2D texture's: width and height, and a stack depth of 6.
- **The registry.** Resolved textures are keyed by cube or not as well, and a lookup asks for one kind:
  - a cube fetch is given the newest cube texture resolved to its address, through its cube view;
  - a flat fetch is given the newest texture of the other kind, through its 2D array view.

  Presenting and the guest texture cache's "already resolved" check still take the newest of either kind.
- **Still skipped:** resolves into 3D textures. A 3D texture's size is encoded differently.

**Why:**
- **Gustavo's long session** (run 278) met a cube in an area of level 3 (01:54–02:00). There the game
  renders a 256×256 target (`k_32_FLOAT`) and resolves it into the six faces of a cube texture every frame,
  likely a point light's shadow map. XenosNative skipped all six resolves, so whatever sampled the cube got
  no data.
- **The face order** is the same in D3D and Vulkan (+X, −X, +Y, −Y, +Z, −Z). Cubes loaded from guest memory
  already reach the translated shaders as host cubes in that order.

**Evidence:**
- At the dock (run 279) nothing changed. The comparison against the last run differs only by an NPC that
  walked through the view, and there is no warning.
- The cube's area has not been checked yet: Gustavo is to compare it with the Xenos path.

**Limits:**
- **Resolves into 3D textures** are still skipped (logged once).
- **A cube texture is not presented.** A front buffer is never one.

## D52 — Unused render targets and resolved textures are freed after a minute; the pipeline cache is saved at most every 5 minutes (2026-10-15)

**Decision:**
- **Freeing.** Every host render target and resolved texture records the last frame it was used:
  - a render target when it is drawn to, cleared or resolved from;
  - a resolved texture when it is resolved to, bound to a draw or presented.

  Every 600 frames, those unused for 3600 frames (a minute at 60 FPS) are freed, and the log says how many
  and how much. A later use creates them again. Entries without a texture (an unsupported format) stay, so
  their warning is not repeated.
- **The pipeline cache.** It is saved 10 s after new pipelines, as in D45, and then at most every
  5 minutes. A failed save is tried again 10 s later. What is not saved yet is in the pipeline records
  (D42), compiled again at the next start.

**Why:**
- **Gustavo's long session** (run 278, 54 minutes, levels 1 to 3, 3840×2160).
  - **Nothing freed these.** Resolved textures grew from 44 to 101 (about 470 MB at 3×) and render targets
    from 12 to 19 (about 200 MB). New areas bring surfaces and resolve destinations of their own: the
    journal's page textures are at a new address in each level, 18 MB each at 3×.
  - **About 50 pipeline cache saves** of 32–43 MB were written while new areas brought pipelines every few
    seconds: some 2 GB. They were harmless to the frame rate (D45).
- **A minute is long enough.** What the game samples or presents stays used every frame. What it resolves
  only now and then (the journal's pages, a cube map) is resolved again before it is sampled.

**Evidence:** `local_docs/07-phase4.md`, run 280.
- The journal was opened, closed for 80 s and opened again. A minute after closing, 2 render targets
  (9 MB) and 8 resolved textures (6 MB) were freed.
- On reopening they were created again and the journal was right; the dock after it was right too.
- The pipeline cache was saved right after the first new pipelines.

**Limits:**
- **Device memory still grew in the session:** 1.42 GB after the first load, 2.16 GB at the end, 65 pages
  of DiligentCore's memory manager created and 6 freed.
  - The targets explain only part of it. The guest textures the SDK's cache keeps stay at about 383 MB,
    while 21 GB of them were loaded over the session.
  - Two possible causes, not measured yet:
    - fragmentation of the memory manager's pages;
    - textures the SDK's cache freed but that the pipelines' resource bindings still reference until those
      pipelines draw again.
- **A process killed between saves** loses the unsaved pipelines' binaries, not their records.

## D53 — The DLSS modes leave the launcher: experimental, from potcawe_options.toml only (2026-10-15)

**Decision:**
- **The launcher's antialiasing row** with XenosNative offers SMAA, DLAA (NVIDIA RTX), FXAA and off. DLSS
  Quality, Balanced, Performance and Ultra Performance (D49) are no longer in it.
- **`xenos_native_antialiasing` still takes the four DLSS values**, written by hand in
  `potcawe_options.toml`:
  - the file says so, in a comment the launcher writes above the key: experimental, for testing only, not
    finished;
  - when the file names one, the row shows it last, as "DLSS Quality (experimental)" (the word in the
    launcher's five languages), so that saving keeps it;
  - the cvar's description and the renderer's log line call them experimental.
- **Merged from POTCAWERecomp** (`b782265`): the launcher's Save writes the options at once and keeps the
  window open, its button reading "Saved" for a moment.

**Why:**
- **Gustavo's call:** the modes are not finished.
  - The image is right, but the saving against DLAA is moderate (D49) and Performance costs more than
    Balanced.
  - The resolution choices are being redesigned ("internal resolution").
- **Keeping them reachable** lets them be tested without a launcher that silently replaces them: before
  this change, an unknown value in the file became SMAA at the next save.

**Evidence:** a test copy of the launcher, in a folder of its own with a test options file (Gustavo's
launcher was open):
- with `dlss_quality`, the row shows five entries, "DLSS Quality (experimental)" selected, and Save keeps
  `dlss_quality`, the window staying open;
- with `dlaa`, four entries, and Save keeps `dlaa`.

## D54 — The launcher chooses XenosNative's image format and internal resolution (2026-10-15)

**Decision:**
- **The internal resolution row.** "Render quality" is now "Internal resolution" (made in POTCAWERecomp,
  `594be5e`, and merged). With XenosNative it is no longer disabled:
  - "Automatic", the screen's or the window's size as before;
  - then the usual heights below it (2160, 1800, 1440, 1080, 900, 720), down to the lowest the image
    allows: 720 lines, or 800 for an output narrower than 16:9, where the width keeps at least 1280;
  - nothing above the output (Gustavo: no supersampling);
  - a height the options file names is offered as well when it is in range. A height no longer offered
    (another monitor, another window size) becomes automatic.
- **A new "Image format" row**, enabled with XenosNative when the screen or the window is not 16:9: "Fill
  the screen" (D43) or "16:9 with bars". Otherwise it shows "16:9", disabled; for Xenos, "16:9 with bars".
- **The internal resolution follows the format.** Its widths are computed as the renderer computes them, by
  `NativeRenderSize(output, fill, height)`:
  - on a 3440×1440 screen, filling: 3440×1440, 2580×1080, 2150×900 and 1720×720;
  - with bars: 2560×1440, 1920×1080, 1600×900 and 1280×720.
- **The same in a window**, relative to the window's size.
- **The options file:** the launcher writes `xenos_native_fill_screen` and `xenos_native_resolution_scale`
  (the height over 720; 0 for automatic), with a comment saying so. The renderer already took both (D38,
  D43), and needs no change.

**Why:**
- **Gustavo's request.** A 4K screen should be able to render at 1440p. His ultrawide should be able to show
  the game at 2560×1440, which is a 16:9 image with bars: the image format and the internal resolution are
  two choices, hence two rows (his choice among three proposals).
- **Heights, not percentages.** They name the resolutions people know (1440p, 1080p), and give the exact
  1:1 case: 2560×1440 with bars on a 3440×1440 screen is not scaled at all.

**Evidence:** `local_docs/07-phase4.md`, runs 281 and 282, and a test copy of the launcher (Gustavo's was
open):
- **The rows** for Gustavo's 3440×1440 monitor, filling and with bars; for a 1600×900 window (format
  disabled); and for Xenos. Selecting 2150×900 saves `xenos_native_resolution_scale = 1.25`; with bars and
  1.5, 1920×1080 is selected.
- **The renderer in a 1600×1000 window:**
  - with bars and 1.25, it renders 1600×900 with bars above and below;
  - filling and 1.111111, 1280×800, the lowest for 16:10, as the launcher offers it.

**Limits:**
- **A lower internal resolution is scaled to the screen with a bilinear filter**, the HUD included (the game
  composes it into the same image): softer text. A sharpening filter could come later.
- **Outputs that are not 16:9 need `Profile::widens_cameras`** to fill (D43). Without it, the format row is
  only a label.

## D55 — Pipelines not drawn for 10 s let go of their textures; device memory in the log (2026-10-15)

**Decision:**
- **Idle pipelines let go of their textures.**
  - Every pipeline records the frame it last drew in.
  - Every 600 frames, the pipelines not drawn in those 600 frames set their texture variables to null.
    This happens at the end of the frame, after the object motion draws, which commit the frame's bindings
    again (D48).
  - Nothing needs those bindings again: every draw sets all of its pipeline's textures before it commits
    them.
  - The log says how many pipelines let go, and how many textures only they held were freed, with their
    size (watched with weak pointers through the reset).
- **Device memory in the log.**
  - DiligentCore patch `0006` adds `IRenderDeviceVk::GetMemoryUsage`. It reports what the device's global
    memory manager has in use and in its pages, device-local and host-visible, and the page count.
  - Every 600 frames the renderer logs it next to what XenosNative knows it holds: guest textures, render
    targets and resolved textures.
- **Texture sizes count mips and compressed blocks** (`TextureBytes`, from DiligentCore's mip level
  properties). Render targets and resolved textures have one mip, so D52's figures do not change.

**Why:**
- **The long session's growth** (run 278, D52's limits): 1.42 → 2.16 GB.
  - That figure was the memory manager's pages, not what is in them. A page is freed only when it is empty,
    so the pages follow the highest use.
  - **The parts at the end, at 3840×2160:**
    - guest textures at the SDK cache's soft limit, about 383 MB (`texture_cache_memory_limit_soft`, 384 MB,
      the same budget as the Xenos path);
    - resolved textures never freed, about 470 MB (D52 frees them now);
    - render targets, about 200 MB;
    - the fixed part: the 512 MB guest memory mirror, the 32 MB converted index buffer, and the output-size
      textures of the gamma ramp and the antialiasing.
  - **The rest could not be told apart in that log:** partly empty pages, or textures still held.
- **A resource binding holds its views, and a view holds its texture.**
  - In DiligentCore, a texture's default view shares the texture's reference counters, and any other view
    holds the texture.
  - So a pipeline that is not drawn again keeps the textures of its last draw alive. The SDK's texture
    cache may have evicted them, or D52 freed them: their memory stays taken while the game is elsewhere,
    and in a new level that is for good.

**Evidence:** `local_docs/07-phase4.md`, runs 283–287. All at the dock, in a 1600×900 window, turning the
camera for 3–8 minutes:
- **Default limits (283):** flat.
  - 851 MB in use in 880 MB of pages (28 pages);
  - of it: guest textures 142 MB, render targets 41 MB, resolved textures 64 MB, and about 604 MB fixed.
- **The SDK's cache evicting hard** (soft limit 64 MB, 2 s; runs 284, 285 and 287):
  - the guest textures swing between 74 and 128 MB, and the memory in use follows them;
  - the pages stay at 864–928 MB: no growth, and little fragmentation.
- **The journal (286):** opened, closed for 80 s, opened again.
  - Its pipelines let go of their textures once it closed.
  - A minute later D52 freed 2 render targets (9 MB) and 8 resolved textures (6 MB), and the memory in
    use dropped by those 15 MB (865 → 850 MB).
  - Reopened, it was right.
- **What the release frees by itself (287, the cache evicting hard):**
  - 16 textures (2 MB) of the menus' pipelines when the dock loaded;
  - then 0–10 textures (up to 1 MB) every 10 s at the dock.

  At the default limits (286) none: the SDK's cache still held them.

**Limits:**
- **At the dock few textures were held.** The release matters where the game leaves an area for good: a
  new level, which the automated drivers cannot reach. Gustavo's next long session will show it in the new
  log lines.
- **Partly empty pages are not compacted.** DiligentCore frees a page only when it is empty; at the dock
  the pages exceed the use by 30–100 MB.
- **The guest textures may still take up to the SDK cache's limits**: 384 MB soft, 768 MB hard, as on the
  Xenos path.

## D56 — XenosNative is POTCAWE's default renderer; the Xenos path stays as a choice and as the fallback (2026-10-15)

**Decision:**
- **POTCAWE draws with XenosNative by default.**
  - `src/startup.cpp` writes `xenos_native = true` into `potcawe.toml`. An existing file that lacks the key
    gets it appended, with its comment, as with every default there.
  - The launcher selects XenosNative when no file names a renderer.
  - The launcher no longer marks it "(experimental)". Its hint now offers Xenos for when something looks
    wrong, in the five languages.
  - Without `vulkan-1.dll` the launcher offers only Xenos, as before.
  - The library keeps `xenos_native` off: a game turns it on in its own configuration once it is ready.
- **The Xenos path stays**, through the launcher's Renderer row or `xenos_native = false`.
- **A fallback when XenosNative cannot start.**
  - XenosNative takes the window from the SDK's presenter before it creates its device.
  - If the device, the blitter or the switch that turns host rendering off fails, it now releases what it
    created and gives the window back to the SDK's presenter (`IGraphicsSystem::presenter()`). The Xenos
    path then draws.
  - Before, the window was left without a presenter.
- **Anchoring the HUD to the screen's edges moves to a later phase.** It means anchoring HUD elements to
  the screen's edges at wide aspect ratios, where the 2D layers are now drawn in a centered 16:9 box (D43).
  Gustavo's call; `plan.md`, "Later".

**Why:**
- **Gustavo's call:** Phase 4's exit question, "make XenosNative the default for POTCAWE, or keep it
  optional".
- **Performance:** at the same resolution without antialiasing, XenosNative uses 38–56 % less of the GPU
  (D39). At 3× it holds 60 FPS where the Xenos path with 2× MSAA drops to 33.
- **Features the Xenos path does not have:**
  - any internal resolution (D37, D54) and any aspect ratio (D43);
  - SMAA or FXAA (D41), and NVIDIA DLAA (D47);
  - a persistent pipeline cache (D42).
- **Four long sessions** through the first levels (runs 278, 288 and 291), the last one 64 minutes without
  any problem:
  - the memory in use stays bounded (D52, D55);
  - 60 FPS;
  - no message from the renderer.

  The only crash, in run 288, was the game's: the setjmp/longjmp fix.

**Evidence:** `local_docs/07-phase4.md`, run 292 and a clean copy.
- **The defaults, from an empty folder:** the exe, the launcher and their DLLs, with no toml file.
  - The launcher shows "Renderizador: XenosNative", SMAA and Vulkan, and the new hint.
  - The game logs `xenos_native=true` and `gpu_backend=vulkan`, renders with SMAA, and its new
    `potcawe.toml` ends with `xenos_native = true`.
- **An existing `potcawe.toml`** (the build's) got the key appended with its comment.
- **The fallback (run 292):** a temporary probe made the start fail after XenosNative had created its
  device. The log says "no host device; the Xenos path draws", the SDK's presenter creates its swap chain
  again, and the menus and the dock are drawn by the Xenos path at 60 FPS. The probe was removed.

**Limits:**
- **Not every level has been compared side by side with the Xenos path** (Phase 4's remaining item).
  Gustavo has played the first levels on XenosNative without finding a difference.
- **The fallback covers XenosNative's own start, not a machine without Vulkan.** With XenosNative on, the
  game runs the SDK's Vulkan backend (D27), which then has to work too. The launcher offers XenosNative
  only where `vulkan-1.dll` exists.
- **The known limits from Phase 2 stand** (`plan.md`).

## D57 — Phase 4 closed; its open checks move to "Later" (2026-10-15, Gustavo)

**Decision:** Phase 4 (POTCAWE polish) is closed. Phase 5 (temporal techniques) continues. Four checks were
not done as written, and move to the plan's "Later" section, to be done when a scene needs them or a
difference is reported:
- **Every level compared side by side with the Xenos path.** Gustavo played the first levels on
  XenosNative in four long sessions without seeing a difference. What those sessions found was fixed: the
  journal (D50), a cube map (D51), and the memory (D52, D55). The script error was the game's: the
  setjmp/longjmp fix.
- **The known limits from Phase 2:** partial clears, signed formats with separate data, 1_5_5_5 and
  DXT3A_AS_1_1_1_1, and resolves into 3D textures. Only the cube resolves were met (D51).
- **Other levels and the cutscenes at 21:9.** D43 was checked in the menus, the dock and the pause menu.
  The long sessions ran at 3840×2160 (16:9).
- **HUD elements anchored to the screen's edges**, already moved there by D56.

**What Phase 4 changed:**
- **Antialiasing:**
  - its own: SMAA (the default) or FXAA on the finished scene (D41);
  - NVIDIA DLAA (D47, from Phase 5).
- **A persistent pipeline cache** (D42): saved on a thread of its own (D45), and at most every 5 minutes
  (D52).
- **Any aspect ratio** without bars (D43).
- **Anisotropic filtering** at 16x (D44).
- **The launcher:**
  - the image format and the internal resolution (D54);
  - the DLSS modes made experimental (D53).
- **Long sessions:**
  - resolves of 2_10_10_10_FLOAT targets (D50) and into cube textures (D51);
  - unused targets freed (D52);
  - idle pipelines' textures released, and device memory in the log (D55).
- **XenosNative is POTCAWE's default renderer**, with a fallback to the Xenos path (D56).

**Why:**
- **The hour-long session is done:** 64 minutes with no problem, the memory in use bounded, 60 FPS, and no
  message from the renderer (run 291).
- **XenosNative is the default** (D56).
- **What remains are checks,** not known problems.

Gustavo: "Demos por cerrada la Fase 4." (2026-10-15).

## D58 — XenosNative becomes NativeXRenderer, a public project in a repository of its own (2026-10-15, Gustavo)

**Decision:**
- **The name.** XenosNative is now **NativeXRenderer**.
  - The identifiers are the name in snake_case, as D8 did: the folder, CMake target, namespace and include
    path `native_x_renderer`, the cvars `native_x_renderer` and `native_x_renderer_*`, and the log prefix
    `[native_x_renderer]`.
  - The submodules' branches are `native-x-renderer`.
  - POTCAWE's side follows: `src/native_x_renderer_potcawe.cpp` and `docs/native-x-renderer-potcawe.md`.
  - The trace reader is `tools/frame_trace.py`, and `ppc_index.py`'s variable is `PPC_INDEX`.
- **The decision log keeps the old name** in D1–D57, as its rule says (old entries are not rewritten). Read
  `XenosNative` there as NativeXRenderer, and `xenos_native` as `native_x_renderer`.
- **Not renamed:**
  - "the Xenos path", which is the SDK's emulation of the Xbox 360's GPU, Xenos;
  - the pipeline storage's internal magic (`XNPS`).
- **The project is public,** in a repository of its own (`ElGuslml/NativeXRenderer`), with DiligentCore as
  its own submodule. POTCAWE uses it as a submodule. Its history is kept: 49 commits, split out of this
  repository with `git subtree split` before the rename. This brings forward the extraction that Phase 6
  planned.
- **POTCAWE's first release** is published on the public POTCAWERecomp repository, built from public
  sources.

**Why:**
- **Gustavo's call: no "Xenos" in the name,** to avoid confusion with the SDK's Xenos emulation and with
  other projects. XenosRecomp, the Xbox 360 shader recompiler of Unleashed Recompiled, is one.
- **Public, so that the release can be built from the published sources.**

**What changes for a user:**
- The old keys in `potcawe_options.toml` (`xenos_native*`) are no longer read. The launcher writes the new
  ones at its next Save, and until then the defaults apply: NativeXRenderer, SMAA, filling the screen,
  automatic resolution.
- The pipeline storage moves to `userdata/cache/native_x_renderer/`. It is prepared again once, at the first
  run.
