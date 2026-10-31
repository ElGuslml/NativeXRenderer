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

## D59 — GRAW is the second game; the profile describes what changes between XDK versions (2026-10-15, Gustavo)

**Decision:**
- **GRAW goes second, then Kameo,** as Phase 6 planned. Gustavo first asked for Kameo (XDK 2.0.2099), then
  chose to follow the plan.
- **The profile describes what NativeXRenderer had taken from POTCAWE's XDK (5426).** GRAW's XDK (2417)
  differs in the device and in the resource objects:
  - **The dirty masks** (`DeviceLayout`): `dirty_masks`, six masks from one offset, becomes the three masks
    the state tracker reads, each with its offset (VS constants, PS constants, fetch slots), and
    `fetch_dirty_slot0_bit`, the bit of fetch slot 0. POTCAWE: `+0`, `+8`, `+24`, bit 31. GRAW: `+16`,
    `+24`, `+32`, bit 63.
  - **The resource objects** (`ObjectLayout`, new in `Profile`): a texture's fetch constant, a surface's
    RB_SURFACE_INFO, its color or depth info and its size (a u32 with two bit fields), a buffer's address
    and size. They were constants in `targets.cpp`, `renderer.cpp` and `trace.cpp`.
  - **An incomplete profile turns a feature off.** The observer and the renderer check that the profile has
    what they read; if not, they log what is missing, and the Xenos path draws.
- **Inline draws the game fills itself.** New flags in `calls.inc`: `kInlineBegin` for BeginVertices and
  the new `BeginIndexedVertices`, `kInlineEnd` for EndVertices, which is now the draw (`kDraw`). The begin is
  tracked as an ordinary call (its flush commits the state and the programs) and keeps its dirty masks for
  the end, which reads the fetch constant and the draw packet the begin left after the write pointer.
- **Two state tracker rules, generalized:**
  - a fetch dword that a draw's own packets write with the shadow's value comes from the shadow: the
    override from an earlier packet ends there;
  - a segment change inside another one (the kick calling the new-segment function) reads nothing.
- Interface version 0.5 (`kVersionMinor`). POTCAWE's profile names the masks and its object layout, which
  were the library's constants.

**Why:**
- **The masks.** In XDK 2417 the fetch slots are the high half of the third mask, slot s in bit 63 − s;
  the library read the low half of the fourth (POTCAWE). The first observer run on GRAW gave 78.5 % of the
  draws matching.
- **The objects.** A surface's info is at `+48`/`+52` in GRAW, `+24`/`+28` in POTCAWE, and its size is
  encoded differently; a texture's fetch constant is at `+16`, not `+28`. The renderer would have read wrong
  surfaces and front buffers.
- **Inline draws.** GRAW's engine calls BeginVertices, BeginIndexedVertices and EndVertices itself (7 per
  frame in training: the present's quads and particles). POTCAWE only reaches BeginVertices from
  DrawVerticesUP, so a draw at BeginVertices' return never mattered there. In GRAW it saw no draw packet,
  and taking only the end missed the programs the begin's flush loaded.
- **The fetch rule.** GRAW's SetStreamSource records the stream only; the draw's flush writes the vertex
  fetch constant into the shadow and sets its dirty bit after the tracker read the masks. The override an
  inline draw's packets left on slot 31 survived every later draw: 1346 mismatches on vertex fetch 95.
- **The segment rule.** GRAW's kick writes the segment's fence and leaves a submission record past the
  submitted range before it calls the new-segment function. Reading those as packets made the scan stop
  (or misparse) in the middle of draws, Present and BeginTiling.

**Results:**
- **GRAW, training mission** (`tools/nav_training.ps1`, frames 2100–2102): 3888 of 3888 draw calls match
  the rebuilt state (100 %), no call read incompletely, no program, register, constant or fetch mismatch.
  The 95 draw packets per frame without a call are the library's own (clears, resolves,
  SetShaderGPRAllocation's one-point draws).
- **POTCAWE, Port Royal dock**: 1132 of 1132 (100 %), as before; with NativeXRenderer drawing, the same
  image at 60 FPS.

**Revisit if:** a third XDK needs another mask (the bool and loop constants, the register groups): the
tracker reads them from the shadow at every draw today, so it has not needed their masks.

## D60 — A tiled pass is drawn once, at the size of all its tiles, without the game's MSAA (2026-10-16)

**Decision:**
- **A tiled pass** (BeginTiling…EndTiling) is drawn once into host targets of the size its tiles cover
  together. BeginTiling's rectangles (D3DRECTs in the whole image's coordinates) give that size; every
  surface bound inside the bracket gets a host target at least that large. Each tile's resolve copies its
  own rectangle from there, so the per-tile resolves need no merging.
- **Host render targets have one sample**, whatever MSAA the guest surface has (D31: the game's MSAA is not
  brought over). Each draw shows the translated shaders a surface without MSAA (RB_SURFACE_INFO's MSAA field
  cleared for the draw): alpha to coverage dithers as it does at 1× on the console, and no interpolator is
  centroid.
- An EndTiling that resolves into a texture is logged once and not supported (no game has one yet).
- **The SDK's shader storage is turned off** (`store_shaders = false`) when NativeXRenderer draws: it holds
  the Xenos path's pipelines, and the runtime creates them all before the game's first instruction.

**Why:**
- GRAW renders its scene in three tiles of 1280×256 with 4× MSAA (`2_10_10_10_FLOAT`). The library drew the
  pass once at the surface's size, so only the top 256 rows would show (POTCAWE's D25, with its 2× MSAA).
  POTCAWE avoided it with its own hooks, which turn its MSAA off and leave one tile; the tiles' size makes that
  unnecessary for any game.
- GRAW resolves each tile with its rectangle and a destination point at the tile's corner (the engine's
  `sub_821959F8`), as POTCAWE does: copying each rectangle from one full-size target rebuilds the image.
- With one sample per pixel the shaders must not compute coverage for four.
- GRAW waited more than 14 s at startup while the runtime created 400 Vulkan pipelines of the Xenos path's
  `fsi` mode, which NativeXRenderer never uses.

**Results:** GRAW's training mission (`tools/nav_training.ps1`, 1280×720): the three bands form one image with
no seam; 1272–1346 draws per frame, all drawn; 60 FPS, 1.7–1.8 ms of GPU time per frame. The game starts in
about a second. Evidence: GRAWRecomp `local_docs/03-native-x-renderer-first-frames.md`.

**Revisit if:** a game reads MSAA samples itself, or host MSAA becomes one of NativeXRenderer's antialiasing
choices.

## D61 — Only device calls, and what they call, reach the state tracker and the renderer (2026-10-16)

**Decision:** whether a call goes to the state tracker and the renderer is decided per thread, at its
outermost call: a device call (first argument the device) or a call that writes into the device's command
buffer (`kPackets`) does, with every call nested in it. Other outermost calls (the resource Lock and Unlock,
the cache flush) only reach the trace.

**Why:**
- GRAW locks and unlocks vertex and index buffers from another thread (the engine's `sub_821A1000`), while
  the render thread is inside a D3D call. The tracker pushed an inactive entry for every call onto its call
  stack, one `std::vector` for all threads: two threads pushing and popping at once corrupted the heap. The
  renderer's record of the outermost call's arguments was overwritten too.
- The crashes had many faces, all within seconds of the first frame: heap corruption found by SPIRV-Cross and
  the SPIR-V builder on the shader workers, an access violation in the runtime's `WaitMultiple`, a packet scan
  of unmapped memory, and the tracker's own stack. 6 runs in 7 crashed, with and without D60's changes.
- On one thread nothing changes: neither the tracker nor the renderer used those calls.

**Results:** 3 runs of 45 s with no crash, then the training mission (D60's results).

**Revisit if:** a game makes device calls from two threads; the tracker assumes one thread at a time inside the
device, which the XDK requires anyway.

## D62 — The occlusion query pool holds GRAW's queries; a frame has a budget (2026-10-16)

**Decision:**
- DiligentCore's occlusion query pool has 4096 slots (its default is 128).
- A frame begins at most 2048 host queries. Past that, a guest query gets none, and the command processor
  answers it with its address's latest count.
- At most 1024 queries wait for their results (32 before). A query dropped from the wait, or abandoned, is
  kept for reuse instead of destroyed.
- The log reports the most queries begun in a frame and the ones over the budget, and lists at most 8
  addresses.

**Why:**
- GRAW begins up to 367 occlusion queries in a frame, for 300–370 addresses (POTCAWE: two).
- DiligentCore takes a pool slot at each begin and gets the slots back only when the command buffer is
  submitted, at Present. With 128 the pool ran out in the middle of a frame; the query had no slot, and
  reading its result crashed (`QueryVkImpl::GetData`).
- With 32 waiting, the results of all but the last 32 queries of each frame were dropped, and those addresses
  would have answered 0 samples: not visible.
- The queries alive (at most 1025) and the slots a frame takes (at most 2048) fit in the pool together.

**Results:** GRAW's training mission: 31095 queries in 600 frames, 367 at most in a frame, none over the budget,
results for 367 addresses, no errors.

**Revisit if:** a game begins more than 2048 queries in a frame.

## D63 — GRAW's false colors: cube maps sampled with gradients on NVIDIA; fixed in the SDK, not with "if blocks" (2026-10-16, Gustavo)

**Decision:**
- **The SDK's SPIR-V translator samples cube maps at a LOD it computes** (shared SDK patch 0021, in the games'
  `patches/`, since the SDK's own Vulkan path needs it too): `log2(max(|ddx|, |ddy|) * width / 2)` from the
  gradients of the direction, which include the LOD bias as a scale, sampled with an explicit LOD instead of
  the gradients. The bias scale's LOD is clamped to ±32.
- **The "if blocks" mode** that the plan's risk proposed for the translator's `loop { switch (pc) }` was
  written and tried at Gustavo's request, and dropped: it changed the false colors without removing them,
  and the switch turned out not to be their cause. Its diff is kept in GRAWRecomp's
  `local_docs/evidence/03_first_frames/`.

**Why:**
- In a RenderDoc capture of GRAW, the shader debugger computes the right color for a green pixel from the
  same SPIR-V and inputs. Replacing the draw's pixel shader with patched SPIR-V and reading the GPU's
  intermediate values (GRAWRecomp `local_docs/03`) showed every value right but one: the cube map sample of
  the ambient light, garbage in the first pixel of each 2×2 quad (negative values from an RGBA16 UNORM
  texture). It stays garbage with the gradients scaled by 1, and becomes right with an explicit LOD.
- The guest asks for LOD 150 (the cube's smallest mip) through `setTexLOD`; the translator applies it as a
  gradient scale, 2^150, which is infinite: zero derivatives became NaN gradients.
- The Xenos path's Vulkan `fbo` mode showed the same false colors in GRAW; with the patch it draws the
  training mission with the D3D12 path's colors. NativeXRenderer translates with the same code.

**Results:** GRAW's training mission with NativeXRenderer has the Xenos path's colors (no green surfaces, no
dot pattern but the fence's alpha-to-coverage dither); POTCAWE's dock is unchanged.

**Revisit if:** a driver gets cube map gradients right (anisotropic filtering then comes back to cube maps),
or a game's cube coordinates are unnormalized (the LOD assumes the face spans 2 units).

## D64 — A depth resolve read as k_8_8_8_8 binds its depth and stencil words (2026-10-16)

**Decision:**
- A resolve of a depth-stencil surface into a `k_24_8` or `k_24_8_FLOAT` texture writes, in the guest's
  memory, 32-bit words: the depth in bits 8–31 (float24 for D24FS8, unorm24 for D24S8) and the stencil in
  0–7. When a fetch reads that texture's address as `k_8_8_8_8`, NativeXRenderer binds those words as an RGBA8
  texture (R the stencil, G, B, A the depth's bytes, low first) instead of the resolve's depth (R32_FLOAT).
- The words are written on demand: the first such fetch marks the resolved texture, and from then on every
  depth resolve into it also packs the host depth and stencil into the words (a blit,
  `Blitter::Source::kDepthStencilPacked`, through a stencil view of the depth target). Until then the fetch
  gets zero words (no stencil). Depth resolves nobody reads that way (shadow maps) cost nothing more.
- Other reinterpretations of a resolve (another format than the one resolved to) are not handled yet.

**Why:** GRAW marks its objective in the stencil, resolves the depth-stencil surface to memory as
`k_24_8_FLOAT` and reads that memory as `k_8_8_8_8` to get the stencil, whose edges a full-screen pass
draws in yellow: the objective outline, and through the same mask, whether the reticle shows the objective's
arrow. The resolve registry found the texture by address only and bound the depth, so the pass read the
depth the last frame's HUD had left as "stencil": large yellow outlines around the HUD, and no arrow. The
Xenos path's capture showed the pass reading an RGBA8 texture with the stencil in R.

**Results:** GRAW's training mission: no yellow outlines, the objective's arrow and line in the reticle, as
on the Xenos path.

**Revisit if:** a game reads a resolve with another format than these two (the words would have to be
written for that format too).

## D65 — The scene resolve can be named by two return addresses; it is of the game's output size (2026-10-16)

**Decision:**
- The profile's `scene_resolve_outer_caller` (interface 0.6): the return address one level above
  `scene_resolve_caller`, for games whose resolves all go through a function of their own. It is read from
  the guest stack: the back chain gives the caller's stack pointer, 8 bytes below which the Xbox 360
  compiler saves the return address (`__savegprlr`, `internal.h`'s `OuterReturnAddress`). 0: not checked.
- The trace prints both addresses for every call: `from <caller> < <its caller>`.
- The scene resolve must also be of the game's output size (`output_width` × `output_height`).

**Why:** GRAW resolves through one engine function, `sub_82114588`, reached through function pointers:
its return address into Resolve is the same for 25 of the frame's 31 resolves. One level up, the post-processing
passes share one (`sub_82194CC0`): among them only the scene copy is 1280×720, and the other 1280×720 resolve
through `sub_82114588`, the front buffer's, comes from the Present. The size rule only states what the
finished scene is; POTCAWE's scene resolve already is of its output size.

**Results:** GRAW: SMAA applied once per frame in the mission, before the HUD (its text stays sharp), 0.1 ms of
GPU time at 1280×720.

**Revisit if:** a game's finished scene is not of its output size (a game that renders at a lower size and
upscales itself), or the compiler saved the return address elsewhere.

## D66 — GRAW's launcher offers NativeXRenderer as experimental; GRAW fills ultrawide screens with its own hooks (2026-10-16, Gustavo)

**Decision:**
- GRAW Launcher gets POTCAWE's rows (D25, D54): "Renderer" (Xenos, or NativeXRenderer marked experimental,
  offered when Vulkan is installed), NativeXRenderer's internal resolution, its antialiasing (SMAA, FXAA or
  off) in place of the game's 4× MSAA, anisotropic filtering (D44, 16× by default), and Save keeps the window
  open. Xenos stays the default.
- DLAA is not offered: GRAW does not mark its views yet (`BeginView` / `EndViewGeometry`). Chosen without
  them, DLAA leaves the image without antialiasing: the scene resolve's SMAA is off while DLSS is on, and
  DLSS runs at `EndViewGeometry`.
- GRAW sets `Profile::widens_cameras`. Its ultrawide hooks for the Xenos path widen the 16:9 projections and
  narrow the HUD around its center, for an image the presenter stretches. When NativeXRenderer draws they
  take `AspectFactor()`, and the renderer stretches instead of the presenter. `SetGameAspectBox` is not used.
- GRAW's defaults keep `native_x_renderer_fill_screen` off. The launcher turns it on with "Ultrawide" on an
  output wider than 16:9 only: the HUD hook narrows on x only, so a taller output would distort the HUD
  (GRAW warns once).

**Why:**
- Gustavo asked for POTCAWE's launcher in GRAW: the renderer, the resolution, the antialiasing and 16×
  anisotropic filtering. He plays on a 3440×1440 monitor, where the Xenos path already offers ultrawide.
- GRAW's hooks already do what D43 asks of a game for wider outputs: the camera is widened before the
  culling, and the HUD stays at 16:9. Reusing them gives NativeXRenderer the Xenos path's ultrawide image,
  menus included (the hook narrows them too, with black at the sides).

**Results:** the training mission and the campaign menu in a 2580×1080 window: NativeXRenderer filling it
(rendered at 2580×1080, cameras ×1.3438) against the Xenos path's ultrawide on D3D12 at 2×: the same framing,
HUD and menus, 60 FPS in both. GRAWRecomp `local_docs/06-launcher.md`.

**Revisit if:** GRAW's HUD should anchor to the screen's edges, or a taller output (16:10, the Steam Deck)
should fill: the HUD hook would then need the vertical axis, or the box.

## D67 — Occlusion queries are measured only with the SDK's occlusion_query_enable on; GRAW keeps the fake count (2026-10-16, Gustavo)

**Decision:** the renderer measures the guest's occlusion queries (D32) only when the SDK's
`occlusion_query_enable` is on, its default. With it off, the resolver is not set, and the command processor
answers its fake sample count, as on the Xenos path. GRAW's defaults turn it off (`yeti.toml`); POTCAWE keeps
it on, for its sun's glare.

**Why:**
- Gustavo saw elements vanish for a few frames in GRAW's training mission, with NativeXRenderer only, the
  camera still: the objective's arrow and its line in the reticle, a barred window across the street. A
  burst of 80 screenshots caught the line missing in 4 of them, the arrow in 3.
- D32 answers a query with the latest count that arrived for its sample count address, one or two frames
  old. That holds in POTCAWE, whose two query objects measure the same thing every frame. GRAW keeps a pool
  of about 400 query objects (300–430 addresses in 600 frames, 150–430 begun per frame) and gives them to
  different objects from frame to frame: an object can get the count of another one that was hidden, or 0
  before its address has a result, and the game skips it until its query says it is visible again.
- The Xenos path never measured GRAW's queries: its host queries make the game wait (44–50 FPS, GRAWRecomp's
  step 0), so GRAW's defaults already turned them off there. One cvar now means the same for both paths.

**Results:** GRAW's training mission, 1280×720: a burst of 120 screenshots with the arrow and the line in
every one (before: missing in 4 of 80). GPU time 1.83 → 1.50 ms per frame at 1×, with the same draws: the
queries no longer end the render pass (D32's barrier). GRAWRecomp `local_docs/07-flicker-occlusion-queries.md`.

**Revisit if:** a game needs measured counts and reuses its query objects for different things. Each answer
would then have to come from the same use of the query, by waiting for it (the cost D32 avoided) or by the
game reading its results later than it does.

## D68 — The render thread's per-call costs cut: clock reads, cvar lookups, shader hashes, the shadow copy (2026-10-17, Gustavo)

**Decision:** five changes on the render thread, none of them visible in the image.
- **Only the calls that do work are timed.** `RendererTime` (the log's "renderer CPU") read the clock on entry
  and exit of every hooked call. It now covers ClearF, the draws, BeginTiling, EndTiling, Resolve, Query Issue
  and Present. The other calls only feed the state tracker.
- **The SDK cvars are looked up once per frame.** The forwarding accessors (D15, `sdk_cvars.h`) looked the
  plugin's value up by name on every read: the registry's lock, a `std::string` key, a conversion from text.
  `anisotropic_override` was read per sampler and `half_pixel_offset` per draw. Each accessor now keeps its own
  value per thread and looks it up again when the frame changes (`sdk::NextFrame`, at the renderer's frame end).
  A cvar changed at run time reaches the renderer from the next frame on.
- **A program's hash is computed once per load.** The state tracker hashes the microcode (XXH3) when it applies
  an `IM_LOAD`, and the draw path passes it to `ShaderCache::Get`, which hashed both programs on every draw.
- **The shadow copy is precomputed.** A draw copies the device's register groups, fetch constants and
  bool/loop constants from the shadow. The groups become runs without the packet registers when the tracker
  is created, and every run is one byte-swapping loop the compiler vectorizes; before, each register was
  checked against the packet register list with `std::find`. The fetch constants are copied whole and the
  overridden ones patched after.
- **The shader loads' microcode goes into one buffer** of the packet scan, which keeps its capacity, instead
  of a `std::vector` per load.

**Why:** a sampling profile of GRAW's render thread in the training mission (GRAWRecomp
`local_docs/05-performance.md`): NativeXRenderer took 62 % of the thread's busy time. The clock reads were
6.2 %, the cvar lookups 3.5 % with their allocations, `Apply` 8.9 %, the shader hashes 2.6 %. The game's
D3D calls are mostly state calls that do nothing in the renderer, and every one of them paid for two clock
reads.

**Results:** GRAW's training mission, the same spot, two runs each, library D67 against D68: the render
thread 55.8 / 57.6 % of a core → 46.4 / 44.4 % (about 1.9 ms less per frame at 60 FPS); the process 0.91 /
0.92 → 0.81 / 0.79 cores; the same 1327 draws, the same GPU time (5.8 ms at 3×) and the same image. The
observer on the Xenos path: 3750 of 3750 draws fully match, 1,961,208 float constants compared, the programs
equal. POTCAWE at the dock: the same image.

**Left:** the guest memory hashing of D35 (about 6 % of the render thread: every range a draw reads is
hashed to find what changed), DiligentCore's and the driver's per-draw work (descriptor sets, pipeline
binds, about 15 %), and the packet scan's per-dword record of the flush's constant writes.

## D69 — Alpha to coverage without MSAA: blended by the alpha, depth where the alpha is at least 0.5 (2026-10-17, Gustavo)

**Decision:** a draw with alpha to coverage (`RB_COLORCONTROL.alpha_to_mask_enable`) on NativeXRenderer's host
targets, which have one sample (D60), no longer uses the translated shader's 2×2 dither:
- a color target that does not blend blends by the shader's alpha (`SRC_ALPHA`, `INV_SRC_ALPHA`, for color and
  alpha); a target that blends already keeps its blend;
- a draw that writes depth tests the alpha against 0.5 (`>=`), or keeps the game's test where it is a
  `>`/`>=` test with a higher reference;
- the cvar `native_x_renderer_alpha_to_coverage` chooses: `blend` (the default) or `dither` (as before).

**Why:**
- With the game's MSAA, alpha to coverage covers a fraction of each pixel's samples equal to the alpha, and the
  resolve blends them with what is behind. With one sample the shader dithers it in 2×2 pixels: GRAW's
  chain-link fences turned into scattered dark dots, the trees' edges and the cables dotted (Gustavo, the
  open item of D60). Blending by the alpha is what the resolve gives, without the pattern.
- **Depth.** GRAW draws these objects twice (about 125 draws a frame in the training mission): a depth-only
  pass with alpha to coverage and an alpha test (`>` 0.235, 0.3, 0.8...), then the color pass with the depth
  test `>=` against it (reversed depth: GRAW clears depth to 0) and no depth write. Written where the alpha
  is at least 0.5, the depth keeps the color pass to the front layer where the object is mostly covered, and
  later geometry behind it hidden there. Where the alpha is lower, the color pass still passes against what
  is behind and blends over it; geometry drawn behind it later is drawn over it, as through the uncovered
  samples of the MSAA.
- **Draws that blend already.** GRAW fades some objects in with alpha blending and alpha to coverage together.
  With MSAA each covered sample is blended too, so the result is closer to alpha × alpha; a fixed-function
  blend cannot square the alpha, so they keep their blend, and come out somewhat more opaque.

**Results:** GRAW's training mission at 1× (SMAA off for the comparison), against the Xenos path at 1× with
the game's 4× MSAA (D3D12 and Vulkan): the fence a faint mesh as on the Xenos path instead of dots, the tree's
and the cables' edges without the pattern; the same GPU time (1.43–1.50 ms). POTCAWE at the dock: the same
image (its palms are alpha tested, without coverage). GRAWRecomp `local_docs/08-alpha-to-coverage.md`.

**Not solved by it:** thin geometry and thin alpha-tested lines narrower than a pixel (GRAW's barred windows)
stay aliased and dotted at 1×: the Xenos path's MSAA smooths their edges. A higher internal resolution,
host MSAA or DLAA would.

## D70 — Resolved textures are dropped when their guest memory is allocated again or written (2026-10-17, Gustavo)

**Decision:** a resolved texture is invalidated when the guest memory it stands for changes, and fetches read
the guest's data there again, as on the Xenos path:
- each resolve records the bytes its destination covers in guest memory (the base level and every slice or
  face, laid out as its fetch constant says), and marks them in the guest memory mirror as valid and written
  by the GPU (`GuestMemory::ResolveWritten`, the SDK's `MakeRangeValid`, as `RangeWrittenByGpu` does for the
  Xenos path without firing the watches): the pages become write-watched;
- `ResolvedTextures` registers a global watch on the mirror. The mirror fires it on guest threads for a CPU
  write to a watched page, a new physical allocation (shared SDK patch, `invalidate_physical_on_alloc`) and a
  protection change; the callback queues the range, and the render thread applies the queue before each
  lookup and resolve. A texture whose range it overlaps is invalidated; the next resolve into it makes it
  valid again;
- `FindNewest` returns nothing when the newest texture at the address is invalidated, so the fetch binds the
  guest texture and the texture cache loads it (its own watch has marked it outdated);
- a fetch of a block-compressed format (DXT, DXN, CTX1: 4×4 blocks) never binds a resolved texture: a resolve
  does not write them.

**Why:**
- Resolved textures were found by exact address forever. Games free a resolve destination's memory and use it
  for textures they fill with the CPU. Seen in GRAW: a DXT3 128×32 texture at `0x1E5D4000` was bound to a
  stale 64×64 `k_24_8_FLOAT` depth resolve every run; the Cross-Com video (Bink planes `k_8` written by the
  CPU, drawn into a 160×120 target and resolved) gets a block of its own for each video, freed when it ends
  (the resolve at `0xE102000`, invalidated with the block `0xE102000`–`0xE125FFF`). A plane fetched at the
  address of the last video's resolve read that RGBA texture instead of the plane: the likely cause of the
  green-tinted video Gustavo saw at 120 FPS (not reproduced in four loads of his save before or after).
- **The mirror's watches.** They are what the Xenos path's caches use, and they see every CPU write (Unlock or
  not) and every allocation. Marking the pages GPU-written is what keeps a resolve from being invalidated by
  writes next to it: the mirror widens the invalidation around a CPU write up to 256 KB, but stops at
  GPU-written pages. The mirror's data in those pages stays what was last uploaded; a draw reading them as
  vertices read the guest's memory before, which a resolve did not write either.

**Results:** GRAW, the save Gustavo loaded (`yeti_158`) and the training mission (`yeti_159`): three
invalidations each, all at reallocations: the menus' 1280×720 resolve while the mission loads, the 64×64 depth
resolve, and the Cross-Com's 160×120 resolve when its video ends. The Cross-Com video, the HUD's objective
outline (D64) and the scene as before; the frame time and the renderer's CPU time unchanged (12.9 ms and
5.6 ms per frame in the mission at 120 Hz, as before). GRAWRecomp `local_docs/10-crosscom-stale-resolve.md`.

**Rejected:** watching the resolve destinations with a memory invalidation callback of NativeXRenderer's
own: it would get the exact pages of a CPU write, but its unwatch range would either narrow the mirror's
256 KB unprotection (many more access violations, which the SDK's comment measures) or let the mirror's
widening unprotect a resolve's pages without notice.

## D71 — The renderer on a thread of its own; the command processor waits for it before it signals the game (2026-10-17, Gustavo)

**Decision:**
- Every hooked call is captured on the game's render thread into a command (`src/commands.h`): the register
  changes since the last command, the programs (shared copies, kept by the hash of their microcode), the
  surfaces bound as their objects say, the call's arguments and what they point to (a clear's color, a
  resolve's rectangles, its texture's fetch constant and RB_COPY_DEST_INFO, the size of a tiled pass, a query's
  sample count address, the front buffer's fetch constant), and an inline draw's vertices and indices.
- A thread of its own (`NativeXRenderer`) runs the commands in order with the code that ran inside the calls
  before: render targets, clears, resolves, the draw path, tiling, queries, the views (BeginView,
  EndViewGeometry, the aspect box), Present and the frame's end. It owns its copy of the register file,
  DiligentCore's context and the driver. The queue holds 4096 commands, which keep their memory from one use
  to the next; the game's thread waits when it is full.
- SDK patch 0006 (`IGraphicsSystem::SetSignalGate`): the command processor calls the renderer before every
  signal to the guest: the packets that write guest memory (EVENT_WRITE_SHD fences, MEM_WRITE, COND_WRITE,
  REG_TO_MEM, EVENT_WRITE_EXT and _ZPD), the scratch register and read pointer write-backs, and the interrupts
  (INTERRUPT, XE_SWAP). The renderer returns once every command queued so far has run.
- When an inline draw is drawn, its copy is compared with guest memory; if the game reused that memory, the
  copy goes into the mirror and those pages are invalidated.
- The tracker notes the registers whose value changes, comparing as it copies (in blocks of 16, which one
  vectorized pass finds unchanged); a command takes exactly those.
- `native_x_renderer_thread` (on by default, read at startup). Off, every command runs as soon as it is
  captured, on the game's thread. Without patch 0006 the renderer stays on the game's thread.

**Why:**
- GRAW, a mission from Gustavo's save at 120 Hz: 79 FPS with the GPU mostly idle. The game's render thread was
  93 % busy, 11.6 ms a frame: 4.2 ms of the game's own code, about 7 ms of NativeXRenderer inside its calls,
  spread over 1800-2000 draws with no single hot spot. The Xenos path, whose work runs on the command
  processor's thread, ran the same spot at 118 FPS. Per-draw cuts on the game's thread were estimated at
  90-95 FPS.
- **Reading guest memory later is safe because of the gate.** The game reuses vertex buffers, index buffers,
  textures and command buffer memory only after the GPU has signalled that it is done: a resource carries the
  fence of its last use (stamped when it is unbound), segments end with fences, swaps and callbacks are
  interrupts. With the gate the game never sees the GPU further along than the renderer's thread. A resource is
  stamped by a later call than the draws that used it, so its fence comes after them. The one gap: a segment
  kicked from inside a call, whose packets the command processor can reach before that call is queued; only
  command buffer memory is at risk there, which is why inline draws' data is copied.
- **Deadlocks.** The renderer's thread waits only for the queue and the GPU, never for a game thread or the
  command processor; the command processor holds no lock while it waits; the CPU's own register writes (MMIO)
  do not reach the gate.

**Results:**
- GRAW, the same mission (RTSS's frame rate, 1280×720 window): 120 Hz: 79 → 117 FPS. 144 Hz: 119 FPS with the
  first version, 128 once programs were shared and the registers taken in one pass, 131 with the vectorized
  comparison. The training mission at 120 Hz: 120 FPS (8.33 ms), 112 with the thread off.
- At 144 Hz the game's thread is the limit again (92 % busy, the tracker 18 % of it); the renderer's thread is
  about 60 % busy (the rest is its spin while the queue is empty). The command processor waits about 1.2 ms a
  frame at its signals in that mission; in POTCAWE's dock at 60 FPS, never.
- The images as before: GRAW's training, the mission, the Cross-Com video, the menus, the team screen;
  POTCAWE's dock with SMAA and with DLAA (applied every frame, object motion as before). No inline draw's data
  needed restoring in these runs. The game exits cleanly.
- Not a hang: loads that stopped on a black screen at 120-144 Hz were the SDK's keyboard dialog (a new game's
  save name, `XamShowKeyboardUI`), which the test's keys opened at those frame rates and which NativeXRenderer
  does not show (the SDK's ImGui overlays, plan "Later"). The same happened before this decision.
- GRAWRecomp `local_docs/11-cpu-120fps.md`.

**Rejected:**
- Copying everything a draw reads from guest memory on the game's thread: the texture cache and the primitive
  processor read guest memory inside the SDK's code, and the gate makes it unnecessary.
- Holding the command processor until the game's call in progress returns, to close the kicked-segment gap: that
  call can itself be waiting for the command processor (ring space).

**Revisit if:** a game reads GPU results back with the CPU without a signal in between, or polls GPU progress
other than through the command processor's signals.

## D72 — Less of the renderer's work on the game's render thread: the tracker skips what it would not use (2026-10-17, Gustavo)

**Decision:** on the game's render thread, after D71:
- A call that is not a draw and wrote nothing into the command buffer leaves the tracker at once: no scan, no
  Apply, no tracked call (`StateTracker::set_skip_empty_calls`, on unless the observer verifies). The dirty
  masks are read for draws and inline begins only; the renderer keeps the arguments of the calls it acts on only.
- A draw's scan leaves out its float, bool and loop constant writes: the tracker takes those from the shadow
  anyway (float constants by dirty group, bool and loop all forty).
- Fetch constants are copied from the shadow for the slots the draw's flush uploads (dirty when the call
  started), not all 32; the others keep what the GPU has. A slot the flush dirties as it goes is in the draw's
  packets.
- The tracker notes its changes as runs of registers (a changed block of the shadow is one run), and a command
  carries them as runs, copied in bulk on both sides.
- Programs: kept by the tracker for its lifetime (one replaced by the same hash loaded otherwise too) and passed
  to the renderer's thread as plain pointers; a direct-mapped cache of 256 finds most loads before the map; a
  load read when its call returns is hashed where it is in guest memory instead of being copied (a load read
  when a segment ends is still copied, D21).
- The queue's two positions sit on cache lines of their own; the producer reads the consumer's position only
  when its last reading says the queue is full.

**Why:**
- At 240 Hz GRAW's mission ran at 133.5 FPS: the game's render thread was the limit again, 2.1 ms of its 7.5 ms
  frame in the tracker and the capture, and 1.3 ms waiting for the game's main thread.
- A trace of one frame: 1,800 draws and about 17,500 setters (SetVertexShaderConstantF 5,500, SetTexture 3,700,
  the shader, declaration, stream and index setters); a count over 600 frames: those setters never wrote a
  packet (0 of millions), only the shadow.
- Counted per draw: 203 packet writes, 147 of them float constants and 18 bool and loop constants, which Apply
  skipped (float) or overwrote with the shadow (bool, loop); 9.4 dirty constant groups, 1.6 shader loads of
  245 dwords; 45 registers changed. Timed with the TSC: Apply's constants 37 %, shader loads 28 %, the shadow's
  render state and fetch constants 17 %, packet writes 15 %.

**Results:**
- GRAW's mission at 240 Hz (RTSS): 133.5 -> 135.3 (setters) -> 143.2 (constant writes skipped; the fetch slots
  alone changed nothing measurable) -> 146 (runs, plain program pointers, the queue) -> 147.7 FPS (program cache,
  no microcode copies). The training mission: 214 FPS.
- The observer: 100 % of GRAW's draws in the mission (5,178 over three frames), fetch slots and programs
  included, after each step; POTCAWE's dock 100 % (3,384), and its image as before with the fast path on.
- The game's render thread at 147.7 FPS: its own code 3.75 ms, NativeXRenderer about 1.4 ms, 1.4 ms waiting for
  the main thread (whose own code is about 4.2 ms). Further gains are in the game's code.

**Revisit if:** a game's setters write packets (they then take the normal path: the fast path only skips calls
that wrote nothing), or the observer finds fetch constants that differ from the shadow in a slot that was not
dirty.

## D73 — The register file on the renderer's thread: the tracker records what changed (2026-10-17, Gustavo)

**Decision:**
- The tracker no longer keeps a register file of its own for the renderer. For each call it records the
  registers that change (`RegisterRecord`): runs of the shadow's dwords, big-endian as the guest stores them,
  and runs of packet values. It finds them by comparing the shadow with a big-endian image of the file the
  records build, in blocks of 16 dwords, and skips packet writes that change nothing.
- It resolves the fetch constant overrides (D59's rule 3) itself, so a record holds only those two kinds of
  runs.
- A draw or a Present takes the record into its command. The renderer's thread writes it into its own file,
  byte-swapping the shadow's runs.
- The tracker keeps a file of its own only for the observer (`set_apply_locally`). Without a renderer it drops
  the records (`set_keep_record`).
- An inline draw's stream (fetch 95) and its initiator and DMA base come from the draw's own scan, where rule 11
  puts them, not from a register file.

**Why:**
- After D72 and the SDK's counters (151.4 FPS at 240 Hz in GRAW's mission), the game's render thread spent
  about 1.15 ms a frame in the tracker's Apply and the capture of the changed registers: two passes over the
  same data, one on each side of the command.

**Results:**
- The observer: 100 % of GRAW's draws in the mission (5,311 over three frames) and in training (4,080); POTCAWE's
  dock 100 % (3,357, with 66 DrawVerticesUP); POTCAWE's image as with D72.
- A temporary comparison over 276,480 of GRAW's inline draws: the values from the scan and from the register
  file never differed.
- GRAW at 240 Hz: training 216 -> 220.7 FPS; the mission 151.4 -> 151.4-153.0 FPS.
- The game's render thread does about 0.25 ms less: the capture is lighter. Comparing the shadow still costs
  what Apply cost (0.76 ms), because the shadow can only be read on the game's thread. A first version that
  copied the shadow into the record without comparing cost the same: it wrote about 2 KB a draw into memory
  that was not in the cache.
- The mission gains little because its frame is no longer set by the render thread alone. The main thread waits
  for five events of its worker threads, the render thread among them (`sub_82244EB8`, 2.4 ms a frame), while
  the render thread waits for the main thread's frame (1.4 ms). Only the render thread's work while the main
  thread waits shortens the frame.

**Revisit if:** a consumer needs the register file on the game's thread (it can use `set_apply_locally`, as the
observer does), or a renderer wants to drop a record (the image assumes every record reaches the file in
order).

## D74 — A resolved texture outlives an allocation or release of its memory; unused textures are freed by time (2026-10-18, Gustavo)

**Decision:**
- The resolved textures watch guest memory through the memory's physical invalidation callbacks instead of the
  mirror's global watch. The callback says whether the range is a CPU write (the page written, widened) or an exact
  range (an allocation, a release, a protection change, a file read). A CPU write invalidates the texture as before
  (D70). For an exact range the guest bytes the texture covers are hashed and compared with the hash taken at the
  resolve: unchanged, the texture is kept and its memory watched again; changed, invalidated. The log counts both.
- A fetch that finds no valid resolved texture where a resolve once wrote is counted ("fetches in 600 frames read
  guest memory a resolve had written") and the first such addresses are logged with when and why the texture went.
- Unused render targets and resolved textures are freed after a minute of wall time, not after 3600 frames.

**Why:**
- GRAW's checkpoint reload releases and re-allocates the memory of its Cross-Com pictures (five 128x128 textures)
  without writing it; D70 invalidated them at the allocation, so later draws read guest memory that holds the data
  of a resolve the host never wrote (the console keeps the resolve in memory across an allocation). The 3600-frame
  limit was half a minute at 120 FPS, so a texture resolved once and sampled for a minute was freed as well.
- Investigated for the scene fading to black at the Angel (GRAW, local_docs/13); not its cause, the bug reproduced
  with this in place. Right on its own.

**Results:** GRAW: at a reload the five Cross-Com textures are kept ("kept: ... allocated, released or reprotected with
its data unchanged"); the observer 100 % (5,311 draws in the mission; 2,259 in the Angel run). Cost: a hash of the
texture's guest bytes at each resolve that starts watching it, and at each exact-range event.

**Revisit if:** a game allocates a resolve destination and expects it cleared without writing it (the hash then
matches a stale resolve: the game would see the old image where the console would see the allocator's contents).

## D75 — The SDK's dialogs are drawn over NativeXRenderer's frames (2026-10-18, Gustavo)

**Decision:**
- NativeXRenderer hands the runtime's ImGuiDrawer a drawer of its own (`Overlay`, `src/overlay.cpp`), with no
  presenter: the SDK's "bring your own renderer" contract (`ui::ImmediateDrawer`, `ui::AppUIDrawContext`).
- Dear ImGui stays on the UI thread, where the window's input reaches it, as in the SDK's own mode: once per
  presented frame the renderer's thread asks the UI thread (fire and forget, one request in flight) to run
  `ImGuiDrawer::Draw` if a dialog is open. The drawer only records what Dear ImGui draws: vertices, indices, draws,
  scissors and textures (the font atlas, kept as data until the renderer's thread makes its texture).
- The renderer's thread draws the latest record over the back buffer before Present: one pipeline (position,
  texture coordinates, RGBA8 color; alpha blending; scissor), dynamic vertex and index buffers, a texture per
  ImmediateTexture. While no dialog is open nothing is drawn.

**Why:**
- The SDK draws its dialogs (XamShowKeyboardUI's keyboard, message boxes, its F3 overlay) with ImGui from its
  presenter, which NativeXRenderer detaches from the window (plan, Phase 2 step 4). A game that asks for text
  waited for an answer on a black screen: GRAW's new game asks for the save's name (D71's results).
- Calling ImGuiDrawer::Draw on the renderer's thread would race the UI thread's input events on Dear ImGui's
  state; recording on the UI thread and drawing the record keeps every ImGui call where the SDK makes them.

**Results:** GRAW, new game: the save-name dialog shows at the same size and place as on the Xenos path; the
name typed in it is taken (Enter) and the game goes on to the difficulty menu. No cost without a dialog: one
deferred call on the UI thread per frame.

**Revisit if:** a game stops presenting while a dialog is open (the record would not be drawn: the renderer would
have to present by itself), or a dialog draws with textures of its own in formats other than RGBA8.

## D76 — Guest memory read by its virtual address goes through the SDK's translation (2026-10-18, Gustavo)

**Decision:** every read of guest memory by a virtual address (`Load32`, `Load64`, `LoadFloat`, a shader load's
microcode, the device's shadow) goes through `GuestPointer` (`src/internal.h`), the SDK's `rex::memory::GuestPtr`:
above 0xE0000000 it adds the 4 KB the SDK's mapping takes off on hosts whose allocation granularity is coarser
than 4 KB (Windows). Physical addresses read through the 0xA0000000 view or `TranslatePhysical` are unchanged.

**Why:**
- The SDK maps the 0xE0000000 view (physical memory in 4 KB pages) 4 KB lower on Windows, and the recompiled
  code adds the 4 KB back on every access (`PhysicalHostOffset`). `base + address` read 4 KB away from what the
  game wrote there.
- Kameo (XDK 2099) writes its command buffer at 0xEF0Axxxx. The first observer runs found 0 % of the draws
  matching: the tracker read other packets than the calls wrote (SetViewport's 16 bytes held the gamma table
  that Present writes further on), so the shader loads, the SQ registers and the fetch constants were wrong, and
  3,199 of a level frame's 22,905 calls were read incompletely. POTCAWE and GRAW keep their command buffers and
  devices below 0xE0000000, which is why it never showed.

**Results:** Kameo, the same four spots: 100 % of the draws a hooked call writes match (the title video 174, the
menu 329, a video 1, the first level 3,035); no call read incompletely in the level. Addresses below 0xE0000000
read as before: with this decision and D77, GRAW's training mission (3,843 draws over three frames at 120 Hz) and
POTCAWE's dock (1,137 draws) stay at 100 %, their images unchanged.

**Revisit if:** the SDK changes how it maps the 0xE0000000 view.

## D77 — A game that inlines EndVertices: the begin is the draw, handed on when the game commits it (2026-10-18, Gustavo)

**Decision:**
- `DeviceLayout::inline_draw_end`: where BeginVertices and BeginIndexedVertices save the last dword of their packets
  (Kameo: +13620), for a game whose engine commits inline draws itself. kUnknown otherwise (POTCAWE, GRAW).
- With it, a top-level BeginVertices is tracked as the draw when it returns: its scan reaches the fetch constant
  and draw packets it left after the write pointer, and the state is the one its flush uploaded. The tracked call
  is kept, not handed on.
- Before the next device call, if the write pointer is at that saved dword, the game has committed the draw: the
  kept call goes to the observer and the renderer as an EndVertices (the renderer copies the vertices then, filled
  by the game). If the write pointer is elsewhere, the game dropped it: it is discarded (logged once).

**Why:**
- Kameo's XDK library was linked with link-time code generation: EndVertices does not exist as a function, the
  engine does `[dev+0] = [dev+13620]` itself at 43 sites. Its particles and sprites are BeginVertices quads: about
  860 a frame in the first level. Without this, the observer counted them as draw packets written outside any
  hooked call, and the renderer would not draw them.
- A first version tracked the EndVertices at the next device call: 8 of the level's inline draws then took render
  states the engine had already set for the next draw (RB_DEPTHCONTROL, RB_BLENDCONTROL, PA_CL_CLIP_CNTL), through
  render-state handlers that only write the shadow. The begin's own state is the one the GPU draws with.
- One rule in the core instead of 43 `[[midasm_hook]]`s in the game: any game with the same inlining needs only the
  offset. The engine fills and commits an inline draw before its next D3D call (Kameo: in the same function).

**Results:** Kameo's observer at 100 % with the inline draws: the title video 177 of 177 (3 inline), the menu 330 of
330 (1), a video 3 of 3 (2), the first level 3,851 of 3,851 (818 inline); none dropped. GRAW and POTCAWE leave the
field unset: unchanged (D76's results).

**Revisit if:** a game makes another D3D call between a BeginVertices and its inlined end (the draw is then
dropped and the log says so), or inlines EndVertices without the begin saving its end in the device.

## D78 — A clear given a rectangle that covers its target clears it whole (2026-10-18, Gustavo)

**Decision:** a Clear/ClearF with a rectangle is no longer skipped as a whole: the rectangle is captured with the
call, and each target it covers (x1 ≤ 0, y1 ≤ 0, x2 ≥ the guest width, y2 ≥ the guest height) is cleared as before.
A target the rectangle covers only in part is still skipped, logged once with the rectangle and the size.

**Why:** Kameo clears its scene with `Clear(1, &rect, 0x3F, ...)`, the rectangle being the target's own 1280×720. The
skipped clear left the scene's depth as the previous frame left it: in the first level the geometry failed the depth
test and only a sky gradient drawn without it showed, and every occlusion query got 0 samples. POTCAWE and GRAW pass
no rectangle, which is why it had not shown.

**Results:** Kameo's first level draws as on the Xenos path (no more "partial clear" in the log). None of GRAW's 415
and POTCAWE's 328 logs had a partial clear; with this decision and D79 GRAW's training mission (3,843 draws over three
frames) and POTCAWE's dock (1,128) stay at 100 %, their images unchanged.

**Revisit if:** a game clears part of a target (a viewport, a split screen): that needs a scissored clear or a quad.

## D79 — Inside an occlusion query, a depth test that always passes is kept (2026-10-18, Gustavo)

**Decision:** while an occlusion query is open, a draw whose depth test always passes without writing keeps that test
(`DrawPath::SetCountingSamples`), instead of the SDK's normalization that turns it off: the draw then rasterizes into
the depth target, writing nothing, and the host query counts its samples.

**Why:** the SDK's `GetNormalizedDepthControl` turns off a depth test that is ALWAYS without writes (to skip depth
target work), and a draw that then writes no color has no target and is skipped. The guest GPU rasterizes it and
counts its samples. Kameo's light glows measure how much of each glow is on screen with such a quad (no color writes,
depth always, alpha test off, inside a query) and draw the glow by the count: skipped, every count was 0 and the
glows never showed (the Xenos path shows the same: its backends skip the draw the same way).

**Results:** Kameo's first level: the queries count 25-49 samples per glow at the traced spot and the torches glow;
without the queries' answers (`occlusion_query_enable = false`, every glow "visible") the same glows show, plus a few
that the measured counts hide. POTCAWE's sun flare queries count what they counted before at its dock; GRAW measures
none (`occlusion_query_enable = false`).

**Revisit if:** a game counts samples of draws with depth disabled altogether (the guest GPU may count those too; here
only a test that is enabled and always passes is kept).

## D80 — A resolve's clears are done after its copy, over the rectangle it copied; partial clears are filled (2026-10-20, Gustavo)

**Decision:**
- **Resolve.** Flags `0x100` (clear the render target) and `0x200` (clear the depth and stencil) are done after the
  copy, as the EDRAM copy does them (the SDK's resolve, `GetResolveInfo`): the color one on the resolved color
  target with the call's color (a `D3DVECTOR4` at `r10`); the depth one on the depth-stencil surface bound (the
  source itself for a depth resolve) with Z (`f1`) and the stencil in `RB_DEPTH_CLEAR` bits 0-7, which the call
  writes (its own stencil argument is on the stack). Both cover the rectangle the copy read, narrowed to what it
  copied, and run even when the copy itself is skipped.
- **Clears of a part of a target** are filled by the blitter (`Blitter::FillColor`, `FillDepthStencil`: a
  triangle over the rectangle, a constant color or `gl_FragDepth` and the stencil reference), since DiligentCore's
  clears take no rectangle. A rectangle that covers the target is still a DiligentCore clear (D78). This closes
  "partial clears", a known limit since Phase 2.

**Why:** Kameo's flying dragons in the distance are billboards of a 128×128 picture the game renders every frame:
twelve dragons at different moments of their wing beat drawn one after the other into the same 512×512 target,
each resolved into its own texture with `0x300`, which clears the target for the next one. The clears were
ignored, each picture held every dragon drawn before it, and the billboards showed the wings in two or three
positions at once (Gustavo). GRAW's tiled pass resolves each tile with `0x300` too; since D60 draws the pass once,
those clears had not mattered there. No game had logged a partial clear, apart from Kameo before D78.

**Results:** Kameo's first cinematic and the spot after it: one dragon per billboard, as on the Xenos path (RenderDoc:
the impostor target held three wing poses before). GRAW's training mission: the observer 3,843/3,843, the same image
(its tiles' clears now run, on rectangles the copies have already read). POTCAWE's dock: 1,128/1,128, the same image.

**Revisit if:** a game clears part of a multisampled target (the fills are single-sampled: such a clear is skipped and
logged once), or depends on the region outside a resolve's copy being cleared as well.

## D81 — The draws' repeated look-ups and state cut: shaders cached by draw, viewport and scissor when they change (2026-10-20, Gustavo)

**Decision:** step 1 of the renderer thread's CPU work (plan, "The renderer thread's CPU time"):
- **Shaders.** The draw path keeps two direct-mapped caches of what it found: the analyzed shader by its
  microcode's hash and type (256 entries, before `ShaderCache::Get`'s map) and the host shader of a translation by
  the shader and its modification (1,024 entries, before the shader's translations and `GetHostShader`'s map).
  Only shaders found are kept, and host shaders only once ready, which they stay.
- **Viewport and scissor** are set when they differ from the last draw's: DiligentCore records both into the
  command buffer on every call. The cache is dropped where the render targets are (D30's `TargetsChanged`, and
  the draw path's own binds: DiligentCore resets the viewport with the targets).
- **The bool/loop constants** are uploaded when they differ from the last upload of the frame (a dynamic buffer
  keeps its data until the frame ends; object motion's draws upload their own and drop the cache). The fetch
  constants are uploaded every draw: they change with nearly every draw, and comparing them cost more (1.3 % of
  the thread) than the upload it saved.
- Not done: stencil reference, blend factors and an unchanged pipeline, which DiligentCore already filters; the index
  buffer, which shares the guest memory buffer with the vertex fetches (its state transitions go with step 2).

**Why:** the profile of Kameo's first level (plan, the table "Group / Share / What"): the shader look-ups were about
4.5 % of the renderer thread's busy time, `SetViewports` 1.7 %, the constants' maps 1.9 %.

**Results:** Kameo's first level, the same spot and protocol as the profile: the renderer's CPU 9.6–9.9 → 9.2–9.4 ms a
frame (about −5 %); in the profile the shader look-ups 4.5 → 0.7 % and `SetViewports` gone. GRAW's training mission
at 120 FPS: 3.05 → 2.90 ms, the observer 3,843/3,843, the same image. POTCAWE's dock: 4.5 → 4.4 ms, 1,128/1,128, the
same image.

## D82 — Textures and samplers set when they change, shader resources committed when something changed (2026-10-20, Gustavo)

**Decision:** step 2 of the renderer thread's CPU work (plan, "The renderer thread's CPU time"):
- **Bindings.** Each pipeline keeps, next to its texture and sampler variables, the object each one holds; a draw
  sets a variable only when its view or sampler is another object. The variable holds a reference to its object, so
  the pointer cannot belong to another object while it is kept. D55's release forgets them with the variables.
- **The commit.** The draw path keeps the pipeline whose shader resources the last draw committed, and a draw with
  that pipeline draws with them when none of its variables changed and every texture they hold is readable in the
  state it is in (DiligentCore's own test: shader resource, depth read for depth textures; a texture loaded, copied or
  drawn into since is not). The kept pipeline is dropped where the render targets change (D30's `TargetsChanged`,
  and the draw path's own binds, after which a commit transitions what was a target): all the other work on the
  context (clears, resolves, the blitter, antialiasing, DLSS and its object motion draws, Present and its submission)
  goes through there. The guest texture loads inside a draw's view look-ups are compute work, on the other bind point,
  and leave their textures' states for the test above.
- The log's draw line counts both: texture and sampler bindings set, and the draws that committed.

**Why:** after D81 the binding was the largest part of the renderer thread in Kameo's first level: `ShaderVariable::Set`
11.3 % (AddRef, Release and DiligentCore's cache for every texture and sampler of every draw) and
`CommitShaderResources` 12.9 % (a descriptor set allocated and written per draw). Measured with a temporary probe at
the same spot: of about 57,000 variables a frame 1.7 % change, and three draws in four have the last draw's pipeline
and resources.

**Results:** Kameo's first level: the renderer's CPU 9.2–9.4 → 7.2–7.5 ms a frame (about −20 %; 9.6–9.9 before D81),
about 950 bindings set and 1,080 commits for 4,100 draws a frame; in the profile `ShaderVariable::Set` 11.3 → 0.2 %,
`CommitShaderResources` 12.9 → 4.3 %, the readable test 1.0 %. The image, the title and level videos, the menu and the
cinematic's dragons unchanged. GRAW's training mission at 120 FPS: 2.78–2.98 → 2.82–2.89 ms (865 of 1,300 draws
commit), the observer 3,843/3,843, the same image. POTCAWE's dock: 4.35–4.40 → 4.28–4.33 ms, 1,128/1,128, the same
image.

**Revisit if:** something binds a pipeline or commits shader resources on the graphics bind point, or submits the
command buffer, without going through `TargetsChanged`; or a texture is written without a state change DiligentCore
tracks (a state set with `STATE_TRANSITION_FLAG_UPDATE_STATE` off).

## D83 — A draw's texture look-ups kept per fetch constant; only changed fetch constants recompute their binding (2026-10-20, Gustavo)

**Decision:** step 3 of the renderer thread's CPU work (plan, "The renderer thread's CPU time"):
- **The SDK's bindings.** `GuestTextures::RequestTextures` marked all 32 fetch constants written before every draw,
  so the SDK's texture cache computed every used binding again (`BindingInfoFromFetchConstant`, the host format's
  swizzle) in every draw. It now keeps the fetch constants as it last saw them and marks written only the used
  ones whose contents differ, as the SDK's command processors mark the ones the guest writes. A binding of
  unchanged contents is the one computed from them; the SDK still resets all bindings at every frame and when a
  texture became outdated or was destroyed.
- **The look-ups.** `Textures` keeps, per fetch constant, what `View` (per binding dimension and sign),
  `HostSwizzle` and `IsResolved` found for its six words, while a generation is unchanged. The generation changes
  when the resolved textures may answer differently (`ResolvedTextures::generation`: a texture created, resolved to,
  invalidated or freed), when the guest texture bindings change (`GuestTextures::generation`: the SDK's
  `UpdateTextureBindingsImpl`, called for every binding computed again or reset, and every frame), and at every
  frame, so that the textures found are marked used again in the new frame (the SDK's LRU, D52). The resolved
  textures' queued invalidations are applied before each look-up as before (`ApplyInvalidations`, now public).
  The look-ups' side effects stay per call: the stats counters, and `NoteGuestRead` for a fetch that reads guest
  memory a resolve had written.
- **Samplers.** A direct-mapped cache of 128 entries before the derivation and the map: keyed by everything the
  derivation reads (the fetch constant's words, the binding's slot and filters, the bias, the anisotropic
  override). Samplers are never freed.

**Why:** after D82, in Kameo's first level: the SDK's `RequestTextures` 7.6 % of the renderer thread's busy time,
`Textures::View` 3.5 %, `Sampler` 3.5 %, `HostSwizzle` 2.4 %, with `ResolvedTextures::FindNewest` (a walk of a map)
twice or three times per texture. About 38,000 views and as many samplers a frame, nearly all the same as the
draw before.

**Results:** Kameo's first level: the renderer's CPU 7.2–7.5 → 6.6–6.7 ms a frame; the texture look-ups about 17 →
9 % of the busy time. The log's texture counts unchanged; the level, the videos (the same frames as the Xenos path),
the menu, the cinematic's dragons, and at a render scale of 2 (where `IsResolved` is used) the level and the menu as
on the Xenos path at 2x. GRAW and POTCAWE: with D84 below.

**Revisit if:** a guest texture's view changes without a binding change (a texture recreated in place), or a
resolved texture changes without `Touch`, `GetOrCreate`, an invalidation or `EndFrame`.

## D84 — The SDK's shared memory keeps its merged ranges between calls (SDK patch 0007, 2026-10-20, Gustavo)

**Decision:** step 4. `SharedMemory::RequestRanges` sorted and merged the requested ranges in a local `std::vector`
reserved on entry: a heap allocation and a free in every call, also for `RequestRange`'s single range, which the
draw path and the SDK's primitive processor make for every vertex and index buffer of every draw. SDK patch
`sdk_patches/0007` makes it a member, cleared on entry and kept with its capacity, as `upload_ranges_` already was
(`RequestRanges` was not reentrant before either). It changes the SDK's own backends the same way.

**Why:** in Kameo's first level the allocation, the free and the vector's growth were about 5 % of the renderer
thread's busy time (`RtlAllocateHeap`, `RtlFreeHeap`, `vector::_Reallocate` under `RequestRanges`), the largest part
of its 13 %.

**Results:** Kameo's first level: 6.6–6.7 → 6.4–6.5 ms a frame; `RequestRanges` 13.2 → 6.9 % of the busy time (the
rest: the lock, and `VirtualProtect` for the pages uploaded, which the watches need). The four steps together:
9.6–9.9 → 6.4–6.5 ms (about −33 %). With D83 and D84: GRAW's training mission at 120 FPS 2.8–2.9 → 2.75–2.85 ms,
the observer 3,843/3,843, the same image; POTCAWE's dock 4.3 → 3.9–4.0 ms, 1,128/1,128, the same image.

## D85 — Signed texture bindings read the bits as signed (2026-10-20, Gustavo)

**Decision:** the translated shaders declare two bindings for a texture whose sign is decided at run time,
`xe_textureN_..._u` and `..._s`, sample both, and pick per component from the fetch constant's signs (the system
constant `texture_swizzled_signs`). The signed binding expects the texture's bits read as signed normalized values.
NativeXRenderer bound the unsigned view to both (`GuestTextures::GetView` ignored `is_signed`). Now:
- the host formats whose bits the SDK's Vulkan backend reads as signed through a view of another format (8- and
  16-bit normalized: `k_8`, `k_8_A`, `k_8_8`, `k_8_8_8_8` and its `_AS_16_16_16_16`, `k_16`, `k_16_16`,
  `k_16_16_16_16`) name their signed format in the format table;
- such textures are created with the typeless format (DiligentCore creates the image with a mutable format and its
  default view of the unsigned format) and get a second view of the signed format;
- a signed binding gets that view; formats without one (block compressed, float, the 10_11_11 family the SDK loads
  separately as signed) keep the unsigned view, as before.

**Why:** Kameo's menu background and loading screen are purple smoke and flames distorted by a 128×128 `k_8_8`
signed map (Gustavo: "las flamas se ven raras, como cortadas"). Read unsigned, a small negative value (0xFF, −0.01)
became +1.0: the distortion jumped at every zero crossing of the map, which cut the smoke into hard-edged shapes
and kept it from fading at the top of the loading screen. Found in a RenderDoc capture of the menu: the draws sample
the same image through `xe_texture0_2d_u` and `xe_texture0_2d_s`.

**Results:** Kameo's menu and loading screen draw the smoke and flames as on the Xenos path (KameoRecomp
`local_docs/15`). GRAW's training mission: the observer 3,847/3,847, the same image; POTCAWE's dock: 1,119/1,119, the
same image (no signed texture at their spots changed visibly).

**Revisit if:** a signed fetch of a format without a signed view shows wrong values (the 10_11_11 family needs the
SDK's separate signed load), or a resolved texture is fetched as signed (resolved textures have one view).

## D86 — Frames shown with the guest's swaps (an option; SDK patch 0008, 2026-10-20, Gustavo)

**Decision:** `native_x_renderer_present_with_swaps` (off by default for now, read at startup, needs the renderer's
own thread): a frame is shown when the command processor executes the game's swap for it, as the SDK's presenters
show it, instead of when the game calls Present.
- SDK patch `sdk_patches/0008`: `IGraphicsSystem::NoteSwapIssued` (VdSwap, after it wrote the `PM4_XE_SWAP`),
  `GetSwapCounts(issued, executed)` and `SetSwapObserver` (called on the GPU thread after every executed swap).
- The Present command carries the number of its swap: the swaps issued when the game's Present returned (it has
  called VdSwap by then; a game that calls it later gets the next number all the same).
- The renderer composes the frame at the Present command, as before, but into a frame image of the back buffer's
  size and format (three, round robin), and queues it. A queued frame is shown (copied into the back buffer, then
  the swap chain's Present) once its swap has executed: checked before every command, and when the queue wakes the
  renderer's thread (the swap observer wakes it; while it waits for commands it also looks every 5 ms). When several
  are ready the newest is shown; with two queued, the next Present shows the oldest first; one queued 100 ms is shown.
- Nothing waits on the renderer's thread: the command processor executes a swap only after the renderer has run
  the commands captured so far (the swap passes D71's signal gate), and those may include the next frame's Present.
- Presenting now ends DiligentCore's command buffer between the next frame's commands: the draw path's state caches
  are dropped then (`TargetsChanged`).

**Why:** Kameo's Bink videos showed isolated 30-50 ms frames on NativeXRenderer and none on the Xenos path. The game's
Bink code locks a video texture and now and then waits a vertical blank for the GPU (~16 ms, 30 times in 30 s at the
title screen, on both renderers: a probe in the game's GPU wait hook with its host stack). Its CPU runs a frame ahead of
the command processor, whose swaps kept an exact 60 Hz during those seconds (a probe in `PM4_XE_SWAP`). The SDK shows
frames at the swaps, so the delay never reaches the screen; NativeXRenderer showed them at the CPU's Present, 30 ms
apart. A first version with one composed frame waiting in the back buffer did not help: every frame's swap executed
after the next frame's Present had run (the CPU a full frame ahead), so every frame was shown early.

**Results:** Kameo's title screen video, 30 s of RTSS frame times: off 9-11 frames over 20 ms (max 33-35 ms), on none
(max 18.4 ms, as the Xenos path: 18.9); every frame shown at its swap, none early, late or skipped. The first level at
60 FPS: max 18.4 ms; at 120 FPS (`video_mode_refresh_rate=120`): max 9.8 ms; Gustavo's options in fullscreen 3440×1440
(ultrawide, fill, SMAA): every frame at its swap. The images as the Xenos path's, the videos' frames now at the same
moments. The cost: a frame is shown up to one frame later than at the Present call, as on the Xenos path. GRAW and
POTCAWE: with the option off, the observer at 3,843/3,843 and 1,128/1,128 and the same images; on (GRAW's training
mission at 120 FPS, POTCAWE's dock at 60), every frame at its swap, the same images.

**Revisit if:** a game presents without VdSwap, or calls VdSwap more than once per Present (the numbering would drift:
frames shown late or by the timeout); the option would then need the numbering per game.

## D87 — D79 withdrawn: a query's draws that write nothing are not counted (2026-10-20, Gustavo)

**Decision:** D79 is undone. Inside an occlusion query, a draw whose depth test always passes without writing is
normalized like any other (the SDK turns the test off); if it writes no color it has no target, is skipped, and
the query counts none of its samples. `DrawPath::SetCountingSamples` is gone. The Xenos path's equivalent, shared
SDK patch 0026, is removed from the series (25 shared patches again).

**Why:** Kameo has two features measured that way, each with two queries over one quad: a "total" query whose quad
writes no color and no depth and is never occluded, and a "visible" one with the scene's depth test. The light glows
(`sub_823550D8`) take their opacity as visible / total × the light's own (depth ALWAYS for the total); the sun
(`sub_8213C240`) takes visible / (0.75 × total) for a lens flare and a dimming of the sunlight (depth disabled
altogether for the total). With D79 and patch 0026 the glows' totals were counted, and in the Enchanted Kingdom's
forest the glows around the sun washed the image out and flickered on both renderers, at any scale and frame rate
(Gustavo). In one session at that spot on the Xenos path, with a temporary switch for 0026's behavior: counted, the
image was overexposed and flickering; not counted, both gone. Counting the sun's total (depth disabled, a temporary
switch too) changed nothing visible there. Xenia Canary's notes for its occlusion queries name Kameo (4D5307D2) among
the titles that look more correct when unresolved results stay zero, and Gustavo recalls the original's torches
without a glow around the flame. The guest GPU evidently counts no samples for
these quads (or the game expects none): both features stay off, as they were before D79.

**Results:** Kameo's first level: the torches without the glow quads (as before D79), the image otherwise unchanged
on both renderers, no new warnings. Gustavo at the forest spot with NativeXRenderer without D79: right, no haze and no
flicker. The sun's total stays 0 (the sun's dimming and flare are not computed, as before);
measured in the first level with a temporary probe of the game's variables: total 0, visible about 4,200 samples.

**Revisit if:** a game's effect measured by a quad that writes nothing is known to show on the console (a capture or
footage of the hardware); then count such draws for that game only.

## D88 — An inline draw followed by the game's own packets is still drawn (2026-10-21, Gustavo)

**Decision:** for a game that commits its inline draws itself (D77), the draw is handed on when the device's write
pointer is at the draw's end, as before, or past it within the same segment (less than 64 KB further): the game
committed the draw and then wrote packets of its own before the next device call. Those packets are not read. A
write pointer short of the end, or in another segment, still drops the draw (the warning of D77).

**Why:** in Kameo, D77's warning showed in two of Gustavo's sessions, once each (it logs once). A temporary probe
logged every case in a session of his: 20 in about 13 minutes, all from the same engine function (`sub_82351F88`, a
quad-list effect begun at `0x82351FE0`), the write pointer always 88 bytes past the end, the next call a SetTexture.
The 88 bytes are a cache coherency sequence the game's inlined D3D code writes right after committing the draw for a
buffer the CPU has just written (`COHER_STATUS_HOST` 0x03000000, `COHER_SIZE_HOST` / `COHER_BASE_HOST` with a 16-20 KB
range such as `0x1F987000`, then a `WAIT_REG_MEM` on the status). The draw was committed; NativeXRenderer dropped it,
and the effect was missing for that frame.

**Results:** Kameo, Gustavo playing the same places and missions again: the case logged once ("followed by 88 bytes
... drawn (D88)"), no draw dropped, no new warning. GRAW and POTCAWE have no such sequence at their spots.

**Revisit if:** a game writes packets that change the state of the next draw (register writes, constants) between
committing an inline draw and its next device call: they would have to be read then.

## D89 — The library's SDK changes are commits of the SDK's `recomp` branch (2026-10-21, Gustavo)

**Decision:** NativeXRenderer's eight changes to the ReXGlue SDK are no longer patch files in `sdk_patches/`: they are
commits 26-33 of the `recomp` branch of [ElGuslml/rexglue-sdk](https://github.com/ElGuslml/rexglue-sdk/tree/recomp),
after the 25 fixes the games share, on top of the official v0.10.0. The games use that branch as their
`thirdparty/rexglue-sdk` submodule; nothing is applied at configure time. The branch's `RECOMP.md` lists every
change. A library change that needs the SDK changed is a commit on `recomp` too, and the games move both submodules.

**Why:** with three games every SDK change was made by hand in several places: the shared series copied byte for
byte into each game's `patches/`, the library's series applied after it, the submodule left modified, and a
`checkout -- .` and a new configure whenever a patch changed. Removing patch 0026 (D87) took a commit, a configure and
a test in each game. One branch is one place: a change is one commit, a new upstream release is one rebase, and the
submodule is a plain checkout. The library gives up working with the official SDK plus its own patches; no project
uses it that way (`git format-patch c94f5eb..recomp` gives patch files back if one does).

**Results:** the branch's tree is the one the games built and tested with (the 54 files the 33 patches touch are
identical). KameoRecomp, GRAWRecomp and POTCAWERecompVulkan on the branch: built from a clean submodule, the same
images, the observers at 100 % (KameoRecomp `local_docs/18`).

**Revisit if:** another project wants NativeXRenderer with a different SDK.

## D90 — The SDK's `recomp` branch on upstream's nightly of 2026-10-02 (2026-10-21, Gustavo)

**Decision:** the `recomp` branch of ElGuslml/rexglue-sdk is rebased from v0.10.0 onto upstream's `development` at
`bd833a2` (the nightly `nightly-20261002-bd833a2a`, v0.10.0 and 24 later commits). The library's eight changes
(26-33) apply without conflicts and unchanged, and the library's code needs no change. The branch as it was on
v0.10.0 is kept as the tag `recomp-v0.10.0`, so the games' older commits still build.

Two shared changes are different on the nightly:
- **Change 5 is dropped** (`fullscreen_borderless_window`). Upstream is borderless at the desktop mode by default, and
  its `fullscreen_exclusive` switches the display mode.
- **Change 34 (the quit prompt) uses upstream's UI input blockers.** Before, it blocked the controller with its own
  code.

**Why:** the next work builds on upstream's new input code: the launchers on SDL3, then remapping the controller. That
code has vibration, dead zones, UI input blockers, hotplug notices and the real device subtype. The nightly also brings:
- **Display:** a fullscreen that switches the display mode; the monitor, window size and fullscreen applied without a
  restart.
- **Codegen:** `stwcx.` as an atomic compare-and-swap, and sync fences.
- **Logging:** the log is configured through `LogConfig`.

Every upstream commit left out makes the next rebase larger.

**Results** (KameoRecomp `local_docs/21`):
- **Codegen:** in KameoRecomp, GRAWRecomp and POTCAWERecompVulkan the same function lists and switch tables, and the
  gap finder finds nothing new. Kameo's reads of uninitialized registers are the same 93 benign cases.
- **The test of every game:** all six runs OK, the observers at 100 %, the CPU and the images within the runs before
  it.
- **The quit prompt and fullscreen:** the quit prompt behaves as before in Kameo and POTCAWE. Borderless and
  exclusive fullscreen work with this library's swap chain and with the Xenos path.

**Revisit if:** the next upstream release or nightly conflicts with one of the library's changes: rebase again, the
same way.

## D91 — The SDK's dialogs make their textures with the drawer they are drawn with (2026-10-22, Gustavo)

**Decision:** no change in the library for the achievement icons. The SDK's `recomp` branch (changes 37 and 38) makes
every texture of its ImGui dialogs with the `ImGuiDrawer`'s current immediate drawer, which is this library's
`Overlay` while NativeXRenderer draws (D75):
- the font atlas, now made on Dear ImGui's request (`ImGuiBackendFlags_RendererHasTextures`): `Overlay::CreateTexture`
  is called again whenever the atlas changes (new glyphs or sizes), and the records keep the old texture alive until
  they are drawn;
- the achievement icons (`AchievementIconCache`), which were made with the SDK presenter's drawer: `Overlay::Draw`
  skipped them (it draws only the textures it made), so the toast and the F7 list had no icons with NativeXRenderer
  (seen in Kameo, 2026-10-22).

**Why:** the plan for the achievements (POTCAWERecompVulkan `local_docs/10`) expected the library to draw
`ImmediateTexture` images of the SDK's drawers on its swap chain. The textures it could not draw belonged to the
presenter's drawer, which NativeXRenderer detaches; making them with the drawer the dialogs are drawn with is the
SDK's contract for "bring your own renderer", and keeps the library free of the SDK's D3D12 and Vulkan objects.

**Results** (POTCAWERecompVulkan `local_docs/14`): the achievement toast and the F7 list with the game's icons and
Barlow text on NativeXRenderer and on the Xenos path; the quit prompt in the same style; the test of every game
(recomp-tools, 2026-10-22): all six runs OK, the observers at 100 % (Kameo 4,129/4,129, GRAW 1,292/1,292, POTCAWE
1,128/1,128), the images within their limits.

**Revisit if:** a dialog draws a texture made outside the `ImGuiDrawer` (for example a game's own `ImmediateDrawer`
calls): it would still be skipped.

## D92 — Shadow maps at a scale of their own: the launcher's shadow quality (2026-10-22, Gustavo)

**Decision:**
- **`native_x_renderer_shadow_scale`** (read at startup; 0 by default): when it is above the render scale, the shadow
  maps are drawn at that scale on both axes. The launchers' "Shadow quality" row writes 0 (Original: the render scale,
  as everything else), 2 (High) or 4 (Ultra): at least twice or four times the game's resolution, never below the
  render scale (Gustavo's choice of fixed sizes over a multiplier: at 1440p POTCAWE's 1024² maps are 2048² in Original
  and High and 4096² in Ultra; the memory stays bounded). It is capped where a 2048² map would exceed the largest
  texture.
- **Which surfaces are shadow maps, with no game code:** a depth surface drawn or cleared with no color surface bound,
  which is not of the game's output size. The first such use marks the surface (`RenderTargets::MarkShadow`, keyed as
  targets are); from then on:
  - a draw with no color surface, a clear (whatever is bound) and a resolve from it use its target at the shadow
    scale;
  - a draw that binds a color surface with it uses its target at the render scale, so that both attachments are at one
    scale.
- **Resolves from a shadow map** go to a resolved texture at the shadow scale (`ResolvedTexture::shadow`, part of its
  key), and the destination rectangle is scaled by that texture's scale. Fetches sample it with normalized
  coordinates, so its size does not matter to them.
- **The slope-scaled depth bias** is multiplied by the target's scale (the larger axis), as the SDK's Xenos path
  multiplies it by its resolution scale: the slope is per host pixel, so at a higher scale the same factor would give
  less bias. At the render scale this changes nothing at 1×; POTCAWE sets no polygon offset at the dock (probe, below).

**Why:**
- **Gustavo's request** (the launcher's row, shown as "coming soon" since Launcher 2's phase 1; his choice of fixed
  sizes).
- **No game hooks needed.** Shadow maps are the passes that draw depth only: POTCAWE's two cascades (a 1024² float24
  surface, pitch 1040, bound with no color target: trace `potcawe_013`, `rt0=0x0`) and Kameo's 768² and 1024² maps. The
  scene, a depth prepass or the HUD are of the output size or bind a color target.
- **The clear goes to the shadow target whatever is bound.** POTCAWE clears its shadow maps with the XDK's ClearF,
  which binds an aliased color surface for its rectangle (pitch 520, 4× MSAA). Taken by the binding, that clear went
  to the target at the render scale; the shadow target was never cleared, the second cascade was drawn over the
  first, and characters got large dark shapes instead of their shadows (runs 398 and 399, 4× and 2×; run 400, a forced
  4× render scale, right; run 401, the clear fixed, right).
- **The translator's baked scale** (D37) reverts the render scale, not the shadow scale, in the texel offsets of
  resolved textures: a fetch offset into a shadow map spans the render scale's host texels. POTCAWE's shadow fetches
  are point samples (`tfetch3D`, four per pixel), whose footprint stays within a guest texel either way; the image at
  Ultra matches a forced 4× render scale (below).

**Results** (POTCAWERecompVulkan `local_docs/19-shadow-quality.md`):
- **Image:** at the dock in a 1280×720 window, Ultra (4096²) gives Jack's and the sailors' shadows smoother edges than
  Original (1024²), and the same shadows as a forced 4× render scale.
- **Cost** (`gpu_ab`, the dock, 60 FPS): at 1×, 1.73 → 1.91 ms of GPU time per frame (17.8 → 19.0 %); at 2×, 3.50 →
  3.64 ms (28.5 → 29.0 %). Memory: +128 MB for the 4096² depth target and +128 MB for its resolved texture (two
  slices).
- **Polygon offset:** a temporary probe saw none at the dock (no draw with a slope or constant bias).
- **The test of every game** (recomp-tools, defaults): all six runs OK, observers at 100 % (Kameo 4,114/4,114, GRAW
  1,381/1,381, POTCAWE 1,128/1,128), the images within their limits. With the shadow scale at 4: Kameo's two maps found
  and at 3072² and 4096², its image within its limit; GRAW's none.

**Limits:**
- **GRAW's shadow pass binds a color target** (a 1024² `8_8_8_8` surface with its 1024² depth): it is not found, and
  GRAW's launcher has no row (`game.h`'s `kShadowQuality`).
- **A shadow map drawn into a color surface** (POTCAWE's point light cube in level 3, `k_32_FLOAT`, D51) keeps the
  render scale.
- **The first frame's clear** of a shadow map, before its first draw marks it, goes to the render scale's target, which
  is then unused and freed after a minute (D52).

**Revisit if:** a game draws depth only into a surface that is not a shadow map and that something else of the render
scale then reads by pixel position, or a game's shadow fetches use offsets of more than a texel.

## D93 — A texture's size in a game's constant: the host's, for a resolve result (2026-10-22, Gustavo)

**Decision:**
- **`Profile::texture_size_constants`:** a game lists the float constants in which it gives its shaders the size of a
  texture they sample, `(width, height, 1 / width, 1 / height)`, with the stage and the texture's fetch constant.
- **At each draw** whose shader of that stage reads the constant and samples that fetch constant: when the texture
  bound there is a resolve result whose host size differs from its guest size (`Textures::Resolved`), and the constant
  holds exactly the guest size, the uploaded constant gets the host size instead (the register file is unchanged). The
  first time, the log says so.
- **POTCAWE's profile:** the pixel shaders' c38, fetch constant 8 (its shadow maps).

**Why:**
- **D92's shadow scale changed nothing to see** (Gustavo, 2026-10-22, at 3440×1440: Original 2752×2048 and Ultra
  4096² looked the same, with the same jagged edges on Jack's shadow). POTCAWE's shadow-receiving pixel shaders (the nine
  dumped at the dock, `6B954BAB187F64D2` and the others with `tfetch3D … tf8`) filter the map themselves:
  `floor(uv × c38.xy) × c38.zw`, four point samples at that texel's corners, blended by `frac(uv × c38.xy)`, with
  c38 = (1024, 1024, 1/1024, 1/1024). On a larger map those samples land on the 1024 grid: one host texel of each block
  is read, and the shadow is the 1024² one whatever the map's size, at the render scale (D37) as with the shadow scale.
- **With the host size in c38** the same filter runs on the host's texels: Original gets the render scale's map (2048²
  at 1440p, which was drawn and wasted until now), High and Ultra theirs.
- **Matched by value and by use:** three other pixel shaders read c38 without sampling fetch constant 8, with another
  meaning; they are left alone, and so would be a constant that does not hold the guest size.
- **The game's knowledge stays in its profile** (rule 1): which constant holds which texture's size is POTCAWE's; the
  library only swaps a size it can check.

**Results** (POTCAWERecompVulkan `local_docs/19-shadow-quality.md`, the dock):
- **At 1×:** Ultra's shadows sharp (Jack's hair, sword hilt and hat drawn), Original's as before (1024², nothing to
  change).
- **At a forced 2×:** Original sharper than before (2048² used), Ultra sharper still.
- **The test of every game** (recomp-tools, 1280×720 windows, where the render scale is 1 and nothing is swapped): all
  six runs OK, observers at 100 % (Kameo 4,160/4,160, GRAW 1,285/1,285, POTCAWE 1,128/1,128), the images within
  their limits. Kameo and GRAW set no entries: D93 does nothing there.

**Limits:** the first frame at a level may sample a map resolved at the render scale before the shadow pass is marked
(D92), with the size it has: right for that frame.

**Revisit if:** a game passes a texture's size in another layout (only the width, texel size first): a field for the
layout.

## D94 — The scene copied twice a frame: antialiased once (2026-10-23, Gustavo)

**Decision:**
- **`Profile::scene_resolve_once`:** when true, only the first resolve of each frame that D41 / D65 take for the
  finished scene (the profile's callers, the game's output size) is antialiased; the flag is reset at each Present.
  False (the default): every one, as before.
- **Kameo's profile sets it.**

**Why:**
- **Kameo copies its scene twice a frame through the same calls:** the first post-processing pass (the scene with its
  fog) and the bloom composite, both from `0x820DC4B0 < 0x8227AE6C`, both 1280×720 into the same texture
  (`0x82758880`; the level trace of KameoRecomp `local_docs/08`). The callers and the destination cannot tell them
  apart, so FXAA or SMAA ran twice in every frame of play.
- **Measured** (KameoRecomp `local_docs/23-gpu-cost-4k.md`, Gustavo's garden at 3840×2160 and 120 FPS, RenderDoc's GPU
  duration per event): the frame 6.31 ms, SMAA 0.91 ms of it in two runs of a copy, the edges, the weights and the
  blending. With the option: one run (~0.32 ms in the first level's frame), about 0.45 ms a frame less at 4K.
- **The first copy rather than the last:** the last one cannot be known when it comes, and between the two Kameo draws
  only the bloom's downsampled passes and one full-screen composite, with no geometry: the edges are the first copy's.
- **Off by default:** GRAW and POTCAWE copy their scene once a frame.

**Results:** the test of every game (recomp-tools), all six runs OK: Kameo with its profile setting it, the observer at
100 % (4,154/4,154) and the image within its limit (the same difference to the baseline as before, 10.35); GRAW
(1,285/1,285) and POTCAWE (1,137/1,137) unchanged. A RenderDoc capture of Kameo's first level at 3840×2160 shows one
SMAA run.

**Revisit if:** a game whose first copy of the frame is not the one with the edges (geometry drawn between two
copies): the copy's index in the frame as the profile's choice.

## D95 — A resolve through an MSAA surface reads the single-sampled target drawn at its EDRAM (2026-10-24)

**Decision:**
- **Each host target stamps its last write** (`HostTarget::written`): every draw stamps the targets bound to it,
  every clear and fill the one it clears, the promotion of a reduced target (D49) the full one, in one increasing
  order (`g_target_writes`).
- **At a resolve from a multisampled color surface**, `RenderTargets::FindMsaaAlias` looks for a single-sampled color
  target at the render scale with the same EDRAM base and storage format and the pitch the samples take: twice the
  surface's for 4x MSAA, the same for 2x. A 4x pixel is 2×2 samples and a 2x one 1×2 (one above the other), stored as
  that many pixels of the 1x surface, as the SDK's EDRAM lays them out. When that target was written after the
  surface's own, the resolve copies from it: the source rectangle times the samples, filtered to the destination's
  size (`kColorScaled`, a factor of 2 per axis that a linear sample at each destination pixel's center averages
  exactly), and the resolve's clears (0x100, 0x200, D80) go to that target too. The first time, the log says so.
- **A resolve of one sample** (fragment bits) through such an alias is averaged, with a warning once.

**Why:**
- **Guitar Hero: Metallica's songs were covered in noise and stripes** with NativeXRenderer (2026-10-24): the engine
  draws its 1280×720 scene into a 1x surface at EDRAM base 0 (pitch 1280), binds a 640×360 4x MSAA surface at base 0
  (pitch 640) and resolves it with no draw in between, a 2×2 average of the scene for its post-processing (the
  RenderDoc capture: that resolve's host target, never drawn, held garbage, which the post-processing spread over the
  scene). On the console both surfaces are the same EDRAM; NativeXRenderer keys host targets by the surface's shape
  (D60), so the 4x surface had a target of its own that nothing drew into.
- **Ordered by writes, not by frames**: the same EDRAM can be drawn through either surface; the target written last
  holds what the EDRAM holds.
- **Color only**: depth through an aliased MSAA surface has not been seen; a resolve averages depth samples, which
  no game reads as a downsample yet.

**Results** (GHMetalRecomp, 1280×720 window): the log names the alias (640×360 4x reads the 1280×720 target, base 0);
the career's opening cinematic and its first song in the arena are clean, at 60 FPS. The observer is unaffected (it
compares the guest's draws, 654 of 654 in a song).

**Limits:** a 1x target at a reduced scale (D49) is found only after its promotion; a surface partly covered by the
alias reads what the alias has there.

**Revisit if:** a game resolves depth through an aliased MSAA surface, or draws through a multisampled surface and
resolves through a single-sampled one (the opposite direction: a copy of each sample).

## D96 — A resolved texture that still holds its resolve is freed by time only above a memory budget (2026-10-24, Gustavo)

**Decision:**
- **D52's check stays** (every 600 frames, the resolved textures unused for a minute), with two outcomes:
  - **an invalidated texture is freed** as before: its guest memory holds newer data (D70, D74), so no fetch finds
    it again;
  - **one that still holds its resolve is kept** while all the resolved textures take at most
    `native_x_renderer_resolved_memory_mb` (384 MB by default, the soft limit of the SDK's cache for guest
    textures). Above it, the least recently used are freed first, until the total is under it.
- **0 restores D52's behavior**: freed after a minute unused.
- **Render targets are unchanged.**

**Why:**
- **Guitar Hero: Metallica's stage under a blocky pattern.** In Gustavo's play with NativeXRenderer (SMAA,
  3440×1440) the pattern appeared now and then, in songs and in the pause menu.
  - The RenderDoc capture: the post-processing's full-screen composite blends the sharp and the blurred scene
    through a 128×64 k_8 mask at 0x76ED000, and read that mask as a guest texture, which held garbage.
  - The engine resolves the mask once, when the career's first song loads. It samples it only in the pause menu,
    on the failed-song screen and in some moments of a song.
  - After a minute without those, D52 freed the mask, which the console's memory would still hold. The log has
    "a draw reads guest memory at 0x76ed000 ... freed unused", and the reads ("fetches ... read guest memory a
    resolve had written") at each of the five moments he dumped a frame.
- **D52's premise does not hold for every game.** D52 assumed that what a game resolves only now and then it
  resolves again before sampling it. That holds for Kameo's journal pages; GRAW's Cross-Com pictures (D74) and
  this mask are resolved once and sampled for minutes.
- **A budget keeps D52's bound.** Kameo's 64-minute session at 3840×2160 freed 90 resolved textures (551 MB).
  GH Metallica's 22 take 117 MB at 3440×1440.

**Results** (GHMetalRecomp, 1280×720 window, the career's first song): reproduced with D52's minute shortened to
120 frames, a local change for the test only.
- **With the budget at 0:**
  - the stage under the blocky pattern in the song and in the pause menu, as in Gustavo's screenshot;
  - the mask and two 512×64 textures read from guest memory: 500 to 3,600 such reads every 600 frames.
- **With 384 MB:** clean, no such read, no resolved texture freed (19 kept), 60 FPS.
- **Gustavo's play** (3440×1440, SMAA): no blocky stage.
- **The test of every game** (recomp-tools): all six runs OK, the observer at 100 % (Kameo 4,114/4,114, GRAW
  1,381/1,381, POTCAWE 1,128/1,128), Kameo's image within its limit (10.01, as before). GRAW and POTCAWE also took
  D94 and D95 with it.

**Limits:** above the budget the least recently used texture that still holds its resolve may be one the game
samples later; the log's line names it when a draw reads there.

**Revisit if:** a game's resolved textures pass the budget in normal play (the log's device memory line), or a
game resolves into ever new addresses without its memory being written in between (they would stay up to the
budget).

## D97 — Guest memory pages rewritten every frame are compared by hash, not watched (2026-10-25)

**Decision:**
- **A page found written again 8 times, each within 16 frames of the last, is left unwatched.** It stays invalid:
  every request hashes it and copies it if it changed (D35's hash), and nothing protects it, so the game's writes
  there no longer fault. After 120 frames unchanged it is watched again. `native_x_renderer_unwatch_rewritten`
  (on) turns it off.
- **Texture data and resolve destinations stay watched**: the texture cache and the resolved textures (D70) learn
  of the game's writes through the watches. Pages the texture cache requests, or a resolve wrote, are always
  watched; one that was unwatched fires the invalidation for its page when it becomes so.
- **Those pages are made valid as written by the GPU.** The SDK widens the invalidation around a CPU write up to
  256 KB, but not across GPU-written pages; the flag has no other use there. Texture pages then stop the widening:
  a write to one texture page no longer invalidates the textures around it.

**Why:**
- **The renderer thread in GH Metallica's songs** (`native_x_renderer_thread`): 4.8 ms of CPU per frame, 56 % of a
  core at 120 FPS (the frame rate GHMetalRecomp now offers), and more than one core at 240. Sampled by function,
  `SharedMemory::RequestRanges` took 59 % of its busy time:
  - VirtualProtect, protecting the requested pages again (`MakeRangeValid`), 26 %;
  - XXH3, hashing pages requested again unchanged, 19 %;
  - the shared memory's lock, held by the game's thread while it handled its faults, 10 %.
- **What was rewritten.** Counted per frame:
  - a ring buffer of about 18 MB, rewritten about every 10 frames (450 pages a frame);
  - 20 single pages of small textures, rewritten every frame (a few of them three times).
  - 35 faults a frame, each invalidating up to 64 pages around it, so about 1,000 pages (450 of them texture pages)
    were protected again and hashed again every frame.
- **Hashing costs less than watching** for such pages: a 4 KB page hashes in a fraction of a microsecond; a
  protection is a system call, and a fault an exception, a system call and the lock.
- **Exact invalidation everywhere was worse:** with every page marked as GPU-written, the ring buffer faulted
  once per page (470 faults a frame) until its pages were unwatched. The widening stays for the other pages.

**Results** (GHMetalRecomp, 1280×720 window, the career's first song, the bot):
- **The renderer thread**: 4.8 → 2.9 ms of CPU per frame; at 120 FPS 56 → 36 % of a core, at 60 FPS 29 → 18 %.
  VirtualProtect 26 → 1.5 % of its busy time, the lock 10 → 3 %.
- **Per frame:** about 6,100 pages unwatched (the ring buffer), 8.5 MB of them hashed; 27 faults (25 on the small
  textures' pages), 33 pages protected again.
- **The whole process:** 2.06 → 1.65 cores at 120 FPS, 1.17 → 0.90 at 60. GHMetalRecomp's own change to its
  D3D library's waits for the GPU is in that count too.
- **The images:** the same (the stage, the band, the highway, the failed-song screen).

**Limits:**
- An unwatched page is hashed at every request, several times a frame when several draws read it.
- A page that is both a vertex buffer and a texture is unwatched until the texture cache requests it: a texture
  there can miss one change (the invalidation is fired when the page becomes watched again).

**Revisit if:** a game rewrites texture data every frame in large amounts (it stays watched: the faults remain), or
its uploads per frame (each ending the render pass) become the larger cost: GH Metallica has about 450 a frame.

## D98 — Uploads ahead: the queued draws' vertex and index data uploaded in one batch (2026-10-25)

**Decision:**
- **A draw whose own requests copied** (its vertices, indices or textures; the render pass is ended for them anyway)
  **looks at the draws queued behind it before it draws** (`CommandQueue::Peek`, the renderer's thread only): their
  register records applied to a copy of the registers the ranges come from (the fetch constants, `VGT_DRAW_INITIATOR`,
  `VGT_DMA_BASE`), the vertex fetch ranges their vertex shaders use and their index ranges gathered
  (`DrawPath::AddDrawRanges`), up to `native_x_renderer_upload_ahead` draws (256) or the frame's end, and requested at
  once (`DrawPath::UploadAhead`): their copies are recorded with the draw's own.
- **Each draw still requests its own ranges when it runs**, finding them uploaded; a draw whose vertex shader was
  not analyzed yet is not looked at (it is analyzed at its own draw).
- **Within a window, an unwatched page (D97) is hashed once**: the draws' requests skip a page the window already
  hashed (`GuestMemory::BeginWindow` / `EndWindow`); an inline draw's restore clears that for its pages.
- **Resolves do not end a window**: a resolve writes the mirror on the GPU after the window's copies, and the pages it
  made valid are not uploaded again. Only the frame's end does.

**Why:**
- **Every upload ended the render pass.** GH Metallica's songs at 3440×1440, 160 FPS: about 440 copies a frame, and
  the draws interrupted by copies 193–223 times a frame (RenderDoc). Each interruption stores and loads the render
  targets again (3440×1440 color and depth), and waits behind barriers.
- **What the copies were** (a temporary count): per 100 draws, 46–51 copies of guest memory read as vertices and
  indices, about 1 of converted indices, about 1.4 of texture data, none of inline data restored.
- **The queue is deep enough:** at a draw, on average 200–250 commands were queued behind it, more than 64 for over
  90 % of the draws (a temporary count).
- **It is safe by the queue's design (D71):** the game's thread writes a draw's data before queueing it and reuses
  it only after the command processor's next signal, which waits until this thread has run every queued command. So
  the data of every queued draw is as it will be when the draw runs. Inline draws (whose memory the D3D library
  reuses on its own schedule) still restore their copies at their draw, as before.

**Results** (GHMetalRecomp, the career's first song, the bot):
- **The GPU, at 3440×1440, 160 FPS, ultrawide, SMAA:** NativeXRenderer's frame timer 4.5–5.6 → 2.7–2.9 ms a frame.
  Two RenderDoc captures: draws interrupted by copies 223 / 193 → 9 / 6 a frame; every event's GPU time 4.41 / 4.23
  → 2.94 / 3.12 ms; the scene's draws at the output's resolution 1.75 / 1.62 → 0.78 / 1.00 ms; the copies 0.84 / 0.72
  → 0.52 / 0.43 ms.
- **The renderer thread, at 1280×720, 120 FPS** (the same session with `native_x_renderer_upload_ahead = 0`): 2.86 →
  2.59 ms of CPU a frame; unwatched pages hashed 10.4 → 5.3 MB a frame; 4.8 windows a frame, about 100 draws each.
- **Earlier versions:**
  - windows ended at resolves (31 a frame in GH Metallica): 37 windows of 13.5 draws, 2.65 ms; without the window's
    hashing rule the unwatched pages were hashed twice (20 MB a frame);
  - a window opened by every draw, copying the whole register file (80 KB): in the test of every game GRAW's renderer
    thread went 2.56 → 3.09 ms and POTCAWE's 3.05 → 3.77 ms, with 607 and 397 windows a frame of 2–3 draws (their
    queues are short, and they upload little: 81 and 66 KB a frame). Opened only by a draw that copied, with the few
    registers it reads: GH Metallica 4.8 windows a frame of about 100 draws, 2.86 → 2.59 ms.
- **The images:** the same (the stage, the band, the highway, the HUD).
- **The CPU at 3440×1440:** at 120 FPS the process 1.54 → 1.45 cores, the renderer thread 34 → 30 % of a core
  (60 s of the song). At 160 FPS the renderer thread rose (57–66 → 86–87 % of a core): without D98 the GPU was nearly
  saturated (4.9–6.0 ms of the 6.25), and the thread was blocked behind it part of the time; per frame it took
  4.4–5.3 ms of CPU without D98 and 5.0–5.6 ms with it. Of that, 2.3–3.4 ms a frame is spent in DiligentCore's
  `Present` at this size, with D98 or without (a temporary timer; not at 1280×720): [performance.md](performance.md).
- **The test of every game** (recomp-tools, run 2026-10-25_133912): all OK, observers 4146/4146 (Kameo), 1285/1285
  (GRAW), 1124/1124 (POTCAWE); renderer CPU Kameo 5.94 → 5.80 ms, GRAW 2.56 → 2.41, POTCAWE 3.05 → 3.14 (within its
  runs' 3.05–3.65); 5.1, 6.6 and 5.4 windows a frame.

**Limits:** a draw whose vertex shader is new is not looked at; texture loads still end the render pass where they
happen (about 1 % of the draws in GH Metallica).

**Revisit if:** a game is found to rewrite a queued draw's data before the command processor's next signal (the draw
would show the data as the window found it), or its queue is shallow (fewer draws per window).

## D99 — The swap chain acquires the next image when it is needed, and waits for it asleep (2026-10-25)

**Decision:**
- **DiligentCore patch `0007`** changes the Vulkan swap chain's image acquisition:
  - **`Present` no longer acquires the next image at its end.** The first `GetCurrentBackBufferRTV()` after it does,
    or the next `Present` (or `Resize`, when the swap chain is minimized). NativeXRenderer draws a frame into its own
    render targets and asks for the back buffer only to compose the frame at its `Present` command, about a frame
    later.
  - **`AcquireNextImage` asks with a timeout of 0.** While no image is free it sleeps 200 µs at a time (a
    high-resolution waitable timer, the way the games' waits sleep) and asks again.
- NativeXRenderer itself is unchanged: `Present(0)`, 3 images, at most 2 frames in flight (D22).

**Why:**
- **Where the time went.** GH Metallica's songs at 3440×1440 in a borderless window, 160 FPS (the display at 160 Hz):
  the renderer thread spent 2.1–3.5 ms of every frame in `vkAcquireNextImageKHR` at the end of `Present` (a temporary
  timer; the flush took 0.06 ms, `vkQueuePresentKHR` 0.09 ms), and it was CPU time: NVIDIA's driver spins inside the
  call while it waits. In a 1280×720 window the call took 0.002 ms.
- **It waits for the display, not for the GPU.** Acquired at the end of the next frame instead, about 6 ms later
  and long after the GPU had finished (2.7–3.2 ms a frame since D98), the call still took 2.1–3.5 ms (4.3 at most).
  With frames presented as fast as the display shows them, the presentation engine holds the images until the
  display's next refresh frees one.
- **The deferral alone saves no CPU, but improves the pacing:** the frame interval's worst per 600 frames went
  7.31 → 6.73 ms (median of the song's windows). The sleeping alone saves the CPU but worsens it (7.50 ms). Together
  they save the CPU and pace better than before (7.04–7.12 ms).

**Results** (GHMetalRecomp, the career's first song, the bot; 3440×1440, 160 FPS, ultrawide, SMAA; per 5 s window
during the song, 19–23 windows a run; the variants compared in one build through a temporary switch):

| Variant | Process | Renderer thread | Interval, worst per 600 frames (median) |
|---|---|---|---|
| Before (acquired at `Present`'s end, waiting in the driver) | 2.17 cores | 0.78 | 7.31 ms |
| Deferred, waiting in the driver | 2.14 | 0.79 | 6.73 ms |
| Acquired at `Present`'s end, sleeping | 1.71 | 0.38 | 7.50 ms |
| **Deferred, sleeping (patch `0007`)** | **1.60, 1.67** (two runs) | **0.37, 0.38** | **7.12, 7.11 ms** |
| Patch `0007`, the clean build | 1.68 | 0.39 | 7.04 ms |

- The average interval stayed at 6.26 ms in every variant; the images are the same.
- After it, 60 % of the renderer thread's stack samples wait on the queue's condition variable (idle), and no stack
  sample is in the acquisition.
- **Earlier tries:**
  - 5 swap chain images: the acquisition still waited in the songs (process 2.05 cores).
  - A sleeping wait for the GPU before `Present` (on the frame fence): 1.67 cores, but the pacing worse (the average
    interval 6.29–6.31 ms, the worst 8.8–9.7 ms): it waited for the wrong thing.
  - Sleeping 100 µs or 50 µs at a time: no change (1.71 and 1.67 cores, 7.23 and 7.14 ms).
- **The test of every game** (recomp-tools, 1280×720 windows, run 2026-10-25_163922): all OK, observers 4171/4171
  (Kameo), 1288/1288 (GRAW), 1128/1128 (POTCAWE); renderer CPU Kameo 5.80 → 5.75 ms, GRAW 2.41 → 2.35, POTCAWE
  3.14 → 3.15.

**Limits:** the composition starts up to 200 µs (and the timer's wake-up) after the display frees an image; the
measured pacing is still better than before.

**Revisit if:** a driver blocks in `vkAcquireNextImageKHR` with a timeout of 0 (the specification says it returns
`VK_NOT_READY`), or presentation moves to a thread of its own or to `VK_KHR_present_wait`.

## D100 — A shader's float constants copied a run at a time (2026-10-26)

**Decision:** before each draw, the float constants a shader uses are copied into its constant buffer one run of
consecutive registers at a time (one `memcpy` per run), not one register at a time. The constant that D93 swaps for a
texture's host size is overwritten in the copy; the object motion's copy (D48) takes the same runs.

**Why:**
- **Where the time went.** Kameo, Gustavo's spot (the save "Kameo" in the Enchanted Kingdom, the camera turned with R3
  twice to the meadow; 3440×1440, 120 FPS), about 3,850 draws a frame: the renderer thread took 5.8–6.0 ms a frame,
  70 % of a frame at 120 FPS. Its stack samples resolved to source lines (a sampler with `SymGetLineFromAddr64`):
  the loop that gathered the float constants register by register (a bit scan and a 16-byte copy for each) took
  about 14 % of the thread's samples, more than any other single part; DiligentCore's `SetPipelineState`,
  `DrawIndexed` and `CommitShaderResources` together 19 %.
- **Shaders use ranges.** Matrices, bones and lights are consecutive registers, which the register file keeps in
  order: a run is copied as it is.

**Results** (the same spot, one session, the old and the new copy switched by a temporary flag file every 18 s,
three rounds; the log's 600-frame windows inside each phase):

| Copy | Renderer CPU per frame | Draws per frame |
|---|---|---|
| Register by register | 6.03, 6.08, 6.01, 6.08, 6.06, 5.93, 6.01 ms | 3,754–3,946 |
| **A run at a time** | **5.10, 5.04, 4.97, 4.95, 5.06, 4.98 ms** | 3,819–3,871 |

After it the copy is 3 % of the thread's samples. What is left of a draw's CPU, by share of the thread's samples:
DiligentCore's pipeline switch, draw and descriptor sets (7.8, 6.8, 4.6 %), the system constants (3.0 %), the
textures' views, requests and samplers (6 %), the vertex data's ranges (2.2 %), the primitive processor (1.8 %), the
pipeline lookup (1.3 %); outside the draw, the register records (5 %) and the timing of every command (2 %).

**Revisit if:** a shader's constants are mostly isolated registers (the runs are then single registers: no worse).

## D101 — Draws into offscreen targets wait for their pipelines (2026-10-27)

**Decision:** a draw whose first target (the first color target, else depth) is not of the game's output size waits
for its shaders' translations (D34) and its pipeline (D28) instead of being skipped, up to 3 s
(`native_x_renderer_wait_offscreen_pipelines`, on). The scene's draws, into targets of the output's size, are still
skipped while theirs are created. The log's creation line counts the draws that waited and for how long.

**Why:**
- **A skipped draw can leave a target undefined for good.** A target is created with undefined contents; the game
  draws or clears it before resolving it. When the draw that fills it is skipped, the resolve copies garbage, and a
  texture the game renders once keeps it: GH Metallica's post-processing mask (128x64, D96), drawn at the career's
  first song load, and its 320x180 blurred copies of the scene. Gustavo's first run of the released beta (an empty
  pipeline storage) showed the song's intro in blocks; the same with an empty storage here covered the whole opening
  cinematic in stripes, for about 25 s.
- **Only a first run, or a new scene, has pipelines to create** (D42 prepares the stored ones at startup), so the
  waits are short and rare; offscreen passes are few next to the scene's draws, which keep D28's "a frame late".

**Evidence:** GHMetalRecomp `local_docs/16-first-run-garbage.md`. GH Metallica, the career's first song at
3440x1440, the pipeline storage deleted before the run: without D101 (`ghmetallica_293`) stripes over the opening
cinematic; with it (`ghmetallica_294`) a clean image, and 1 to 12 offscreen draws waited per 600 frames, 1.5 to 32 ms
in all, at most 7.7 ms for one draw.

**Revisit if:** a game's scene is drawn into a target of another size than its output (the scene's draws would wait),
or a game draws offscreen passes every frame with new shaders often enough for the waits to show.

## D102 — A segment change outside any tracked call is not read as packets (2026-10-27)

**Decision:** the tracker reads nothing for a `KickSegment` or `NewSegment` called outside any tracked call, as it
already did for one inside a call (rule 10 of xdk-d3d-reference.md).

**Why:**
- **The kick's gap.** GH Aerosmith (XDK 2.0.6683) kicks command buffer segments outside the D3D calls the tracker
  hooks, several times a frame. The kick writes ten dwords, `INVALIDATE_STATE` and two `EVENT_WRITE_SHD` marks, and
  continues with the write pointer aligned up to 32 bytes, as XDK 2417's kick does (GRAW). The 4 to 7 dwords in
  between are never written: they hold packets from an earlier lap of the ring, which the GPU does not execute. The
  tracker read such a kick like any call, from the write pointer at its entry to the one at its return, so it read
  them as packets: most often a header whose length runs past the end ("not fully" read calls in the observer's
  report), now and then a complete old register write.
- **The stage dark for a frame.** Such writes were the window offset and scissor (`PA_SC_WINDOW_OFFSET`,
  `PA_SC_WINDOW_SCISSOR_TL`, `PA_SC_WINDOW_SCISSOR_BR`) of an earlier pass: the bloom's 320x180, the crowd sheets'
  256x512, a 128x64 one. The scene's draws up to the next write of the scissor were clipped to that size, so most of
  the stage stayed black, and what came after it (the HUD, part of the scene) was drawn. Gustavo saw the stage flicker
  in songs, with NativeXRenderer only, at 60 and 160 FPS: about one frame in 450.

**Evidence:** GHAeroRecomp, a Quickplay song at 160 FPS, with temporary probes (removed). The scene's brightness read
back at its first resolve: 25 to 43 single dark frames per song. The constants, fetch constants and render state of
every draw into the scene's target, kept for the last 12 frames: in 23 of 25 dark frames, 28 to 212 scene draws had
an earlier pass's `PA_SC_WINDOW_SCISSOR_BR` (0x00B40140, 320x180, in the first) where both neighbors had 0x02D00500,
and nothing else differed. The window registers' writes the tracker read, per frame: the dark frames' came from a
kick's scan, whose bytes were the two marks followed by an old `00022080 00000000 00000000 00B40140` (or 0x02000100,
0x00800040). Not the GPU: with the GPU finishing each frame
before the next, the dark frames stayed. With D102: no dark frame in a whole song, none in 1,000 screenshots (3 to 7
before), and the observer at 100 % over 7 frames of a song, no call "not fully" read.

**Revisit if:** a game writes state packets inside a kick it calls on its own: those would be lost (the observer's
render state mismatches would show them).

## D103 — D24S8 depth on GPUs without D24_UNORM_S8_UINT (2026-10-27)

**Decision:**
- The guest's D24S8 depth targets are `D24_UNORM_S8_UINT` where DiligentCore reports that format as a depth target
  that can also be sampled (`GetTextureFormatInfoExt`), and `D32_FLOAT_S8X24_UINT` otherwise, as D24FS8's always
  are; the SDK's Vulkan backend chooses the same way. The log says so once when it takes the fallback.
- What read the guest's depth format from the host format reads it from the guest's: the packed depth words of D64
  (`HostTarget::depth_float24`), which are unorm24 for D24S8 whatever the host stores.
- Stored pipelines (D42) whose depth format is `D24_UNORM_S8_UINT` are prepared with the fallback on such a device
  (a storage written on another GPU): the same state its draws ask for.
- The depth bias stays the guest format's, as on the SDK's Vulkan backend. With a float depth target Vulkan scales
  the constant factor by the primitive's exponent, `2^(e-23)`: unorm24's step `2^-24` for depths from 0.5 to 1,
  smaller nearer the camera.

**Why:** AMD's Vulkan drivers do not offer `D24_UNORM_S8_UINT` as a depth attachment (the specification requires
it or `D32_SFLOAT_S8_UINT`; Mesa's bug 106528 was closed WONTFIX, and vkd3d falls back the same way).
NativeXRenderer created it without asking, so on AMD the depth targets of the games that use D24S8 (GH Metallica,
GH Van Halen, GH Aerosmith, Kameo; GRAW and POTCAWE use D24FS8 only) would not be created, or would be created by an
invalid call. Found by reading the code; not seen on an AMD machine.

**Evidence:** GHMetalRecomp `local_docs/18-amd-depth-format.md`. On the RTX 4070 Ti with the Khronos profiles layer
hiding `D24_UNORM_S8_UINT` (`exclude_formats`, profile emulation off): GH Metallica's career, its first song drawn
as without the layer (the scene, the band, the highway), the observer 452 of 452 draws; with the validation layer
above it, no error over five depth targets taking the fallback (300 to 470 draws a frame), only the two known ones
(a uniform block's layout in shader modules, a queue family index). Test-Games all OK as it was
(`runs/2026-10-27_185200`), and with the layer hiding the format (`2026-10-27_190525`: Kameo 4152/4152, GRAW
1285/1285, POTCAWE 1128/1128, GH Metallica and GH Van Halen 7/7, the images within their limits).

**Revisit if:** a player with an AMD GPU sees depth problems, the bias first (decals, shadows fighting nearer the
camera), or a game reads a D24S8 depth resolve another way than D64's.

## D104 — The image's aspect ratio can be another than the output's (2026-10-28)

**Decision:**
- The cvar `native_x_renderer_aspect` (0 by default, read at startup) asks for the image's aspect ratio while
  `native_x_renderer_fill_screen` is on: 0 is the output's (D43 as it was); another, such as 2.3889 (21:9) or
  3.5556 (32:9), is rendered at that ratio. The scale is per axis as in D43, its factor that ratio over the game's;
  the vertical scale is the largest at which the image fits the output on both axes,
  `min(output height / 720, output width / (1280 * factor))`, or the resolution scale forced, never below 1.
- The presentation does not change: it already scales the front buffer to fit the swap chain and centers it, so the
  bars are above and below when the image is wider than the output, at the sides when it is narrower.
- `AspectFactor()` is that factor, so a game's hooks widen its cameras and keep its 2D layer at 16:9 for the image,
  not the screen. A game's own overlay that covers parts of the image (GH Metallica's menu bars) computes the image's
  rectangle in the output the same way.
- The launchers' Format row (RecompLauncher 3.0) offers 16:9, 21:9, 32:9 and the output's own; it writes
  `native_x_renderer_fill_screen = false` for 16:9 and the ratio otherwise (0 for the screen's own, so the image
  follows another monitor), and computes the render size the same way.

**Why:** Gustavo asked for the format by name (21:9, 32:9) instead of "fill the screen", for a 32:9 screen to choose
16:9, 21:9 or 32:9, and for a 16:9 screen to show 21:9 with bars (and his 21:9 screen to preview 32:9). Everything
after the render scale already worked for any factor: the targets, the resolves and the presentation take their sizes
from it.

**Evidence:** GH Metallica in a 1720×720 window at 3.5556: render scale 2.0 × 1.0 (2560×720, the vertical scale held
at 1), the image 1720×484 with bars above and below, the boot video and the main menu in their 16:9 box in its
center with the menu bars at its sides, the career's first song with the stage across the image and the notes on
their lanes. In a 1280×720 window at 2.3889: 1720×720 rendered (factor 1.3438), shown at 1280×536; menus and a song
as expected. No error in either log.

**Revisit if:** a game draws something the output's size rather than the image's (a full-screen overlay of its own
outside the hooks would cover the bars), or the ratio needs changing while the game runs (it is read at startup, as
`native_x_renderer_fill_screen`).

## D105 — 1_5_5_5 textures (2026-10-28)

**Decision:**
- Guest textures in k_1_5_5_5 are loaded as the SDK's D3D12 backend does: the load shader
  `texture_load_r5g5b5a1_b5g5r5a1_cs` swaps red and blue into `TEX_FORMAT_B5G5R5A1_UNORM` (DXGI's layout: blue in bits
  0-4, green 5-9, red 10-14, alpha 15), sampled with the RGBA swizzle. Until now the format had no host format (D23)
  and its bindings sampled the white placeholder.
- DiligentCore patch 0008 maps `TEX_FORMAT_B5G5R5A1_UNORM` to `VK_FORMAT_A1R5G5B5_UNORM_PACK16`, which is that layout
  (and the SDK's Vulkan backend's format for 1_5_5_5), instead of `VK_FORMAT_B5G5R5A1_UNORM_PACK16` (alpha in bit 0,
  blue in the top bits). A1R5G5B5 is one of the formats every Vulkan device samples; B5G5R5A1 is optional. Nothing
  else in NativeXRenderer used the format (resolves of 1_5_5_5 surfaces stay RGBA8, `HostTextureFormat`).

**Why:** Guitar Hero: Van Halen's career intro drew Eddie Van Halen's guitar white (lavender under the stage lights)
with only its relief: the instrument's color textures are 1_5_5_5 (2D, tiled, packed mips, 256×128 to 512×256), and
the Xenos path showed the striped finish. Patching the mapping keeps the 16-bit data as it is, like patch 0002 did for
BC1, rather than a new load shader that expands it to RGBA8.

**Evidence:** GHVanHRecomp, Career > Single > Guitar > Easy, the intro: 6 to 10 placeholder bindings a frame, all
fetch constants of format 3 (a temporary log in `Textures::FindView`, removed: base addresses 0x0F250000,
0x117AD000, 0x11845000…, none of them a resolve), the guitar white (`ghvanhalen_029`, `_030`); with the change
0 placeholders a frame and the guitar as on the Xenos path (`ghvanhalen_031`). DXT3A_AS_1_1_1_1 is still not
supported.

**Revisit if:** a 1_5_5_5 texture's alpha looks inverted or its colors swapped (the load shader and the mapping are
the SDK's D3D12 and Vulkan ones), or a device fails to create the format (it must not: it is mandatory).

## D106 — The present mode: V-Sync without tearing, or immediate (2026-10-29)

**Decision:**
- `native_x_renderer_present_mode` (empty by default): how frames reach the screen. Empty keeps D99's: MAILBOX, else
  IMMEDIATE (DiligentCore's order for a sync interval of 0). `"vsync"`: MAILBOX, else FIFO: never torn; FIFO, the
  mode that waits for the display, only on a driver that offers no MAILBOX. `"immediate"`: IMMEDIATE, else MAILBOX: each
  frame shown as it is ready (tearing; what a G-Sync or FreeSync display wants). FIFO where the surface offers none of
  them. `Device::Present` still presents with a sync interval of 0: the game paces itself.
- DiligentCore patch 0009: `ISwapChainVk::SetPresentModes` takes the modes to try, in order, in place of the ones the
  sync interval picks (FIFO_RELAXED, FIFO with VSync; MAILBOX, IMMEDIATE, FIFO without); a change creates the swap
  chain again at the next Present. `Device::ApplyPresentMode` gives it the option's list before each Present when the
  option changed (the log says which: "present mode: ..." then DiligentCore's "Using VK_PRESENT_MODE_... swap chain
  present mode").
- The games' launchers write it: a V-Sync row (RecompLauncher 3.4, on by default: `"vsync"`, off: `"immediate"`), with
  the SDK's presenters' own options for the Xenos path (`d3d12_allow_variable_refresh_rate_and_tearing`,
  `vulkan_allow_present_mode_immediate`, `vulkan_allow_present_mode_fifo_relaxed`: false with V-Sync).

**Why:** a Guitar Hero: Metallica player asked for a V-Sync option. The SDK's `vsync` is the emulated
console's vertical blank (the game's pacing), not the display's. NativeXRenderer presented MAILBOX on NVIDIA (no
tearing), but IMMEDIATE on a driver without MAILBOX; the Xenos path tears by design (Xenia's presenters). DiligentCore's
VSync gives FIFO_RELAXED first, which shows a late frame at once, torn: hence the patch rather than a sync interval of
1.

**Evidence:** GH Metallica at 1280x720 on an RTX 4070 Ti (`ghmetallica_386`-`388`): empty and `"vsync"` MAILBOX,
`"immediate"` IMMEDIATE (the intro drawn). The FIFO fallback is DiligentCore's own preference loop (not seen: NVIDIA
offers MAILBOX).

**Revisit if:** a driver without MAILBOX shows the game's frame rate held below its own with V-Sync (FIFO waits for
the display: a game set above the display's rate is held to it), or a display's variable refresh rate needs the
present interval rather than IMMEDIATE.

## D107 — V-Sync is FIFO (2026-10-30)

**Decision:** `native_x_renderer_present_mode = "vsync"` presents with FIFO only: each frame shown once, at a vertical
blank, never torn. D106's order (MAILBOX, else FIFO) is `"mailbox"`. The launchers' V-Sync row writes `"vsync"` with
the SDK's `vblank_follows_display` (recomp change 61), which marks the game's vertical blank on the monitor's own, so
a game that paces itself by vertical blanks never produces frames faster than the display shows them.

**Why:** measured with PresentMon 2.6.0 in Guitar Hero: Metallica, 30 s of a song at 3440x1440 on a 159.96 Hz display,
RTX 4070 Ti (GHMetalRecomp's `local_docs/24-frame-pacing.md`). NVIDIA presents Vulkan through a DXGI swap chain, and
its MAILBOX behaves as a three-image queue: with the game's timer at 160.000 Hz, a hair faster than the display, the
queue filled and every frame waited about three refreshes, 16-19 ms from present to display (none dropped). FIFO:
5.8 ms; FIFO with the vertical blank following the display: 4.0 ms, no frame dropped or shown twice, presents within
0.5 ms of the period 92 % of the time. MAILBOX's lower latency assumes a game that renders several frames a refresh,
which these games, paced by their own vertical blank, never do.

**Evidence:** runs m160 / m160b (MAILBOX), f160 (FIFO), l160 (MAILBOX + vblank following), lf160 (FIFO + vblank
following) in GHMetalRecomp's `local_docs/evidence/14_frame_pacing`.

**Revisit if:** a driver's FIFO queues frames when the game runs at the display's rate without
`vblank_follows_display` (a game whose launcher does not write it), or a variable refresh rate display with V-Sync on
shows the game held below its frame rate.

## D108 — New data uploaded faster: the guest memory mapped ahead, one scratch buffer for the texture loads (2026-10-30, Gustavo)

**Decision:**
- **The guest memory's pages are read once on a thread of their own when the draw path starts** (`GuestMemory::
  MapAhead`, `native_x_renderer_map_guest_memory`, on): every page of the view the mirror copies from
  (`TranslatePhysical`), 64 KB at a time with `ReadProcessMemory`, below normal priority. A page the SDK keeps
  without access fails the read instead of raising the exception its handlers take for the game's own accesses.
- **A page never copied (or dropped) is not hashed** before its copy: it differs whatever its data. The copy still
  hashes the snapshot it uploads.
- **One scratch buffer for every texture load**, grown to the largest a texture needs (a power of two, at least
  4 MB), instead of one created per load. The loads run one after another; DiligentCore's transitions (the load
  shader's writes, the copies into the texture that read it, the next load's writes) put the barriers between them.

**Why** (Guitar Hero 5, 3440×1440, 160 FPS; GH5Recomp's `local_docs/02-port-from-gh-van-halen.md`):
- **The longest frames outside the start were new screens' data.** When Party Play's song began, one frame took the
  renderer 56–61 ms: 466 draws, 33–36 ms of them in the texture requests, 24.8–27.6 ms of that copying 37.6 MB of
  guest memory into the mirror. Split further with temporary timers: the first read of each page through this view
  14.5 ms (the game wrote the pages through another view, so the system maps each on first touch: reading one byte
  per page alone took 10.9 ms, 1.1 µs a page; copying them all after that 3.1 ms), protecting the pages again
  3.8 ms, DiligentCore's `UpdateBuffer` 3.3 ms. `PrefetchVirtualMemory` and `VirtualLock` did not map them faster
  (15 ms with the latter). Of the texture loads themselves: the buffer created per load 1.1 ms in all (and 54 ms
  once, when DiligentCore took a new block of device memory), the textures created 3.0–4.1 ms, the load shaders
  2.9–3.2 ms, the copies into the textures 2.1–2.3 ms, the textures' own hashes 2.1–2.2 ms.
- **Mapping ahead costs about 130 ms on a thread of its own, once** (16 pages without access in GH5), and memory:
  the pages the game has not used yet become resident then instead of when it fills them (the venue's textures are
  such pages). At GH5's first screen the game took 120–300 MB more of the system's memory (1.42–1.54 GB, 1.25–1.30 GB
  without it; two runs each); its working set shows 380 MB more, as it counts twice a page mapped both through the
  game's views and through this one.

**Results** (GH5, the same route, the slow frame logged by temporary timers): the first read of the new pages
10.9 → 0.2 ms; that frame's renderer time 56–61 → 38 ms, its interval 62–67 → 44 ms. In the song, unchanged. The
images as before (the menus, the song, 0 placeholders).

**Revisit if:** a game's guest memory is larger than the mirror's 512 MB, or a system with little memory trims the
mapped pages (they would be mapped again on first read, as before).

## D109 — D97's frame counts scaled by the frame rate (2026-10-30, Gustavo)

**Decision:** a page is left unwatched when it is found written again 8 times, each within 16 frames **at 60 FPS**,
and watched again after 120 frames **at 60 FPS** unchanged: both counts are multiplied by the frame rate over 60
(from 1 to 4), the frame's time smoothed over about 64 frames (`GuestMemory::EndFrame`).

**Why:** at 160 FPS GH5 made about 275 watched pages valid again every frame, in about 44 calls (protections), and its
threads faulted on them as they rewrote them. Counted by how soon each came back: 180–345 a frame after 17–32 frames,
40–70 within 4, 30–70 after 9–16. The pages a ring buffer rewrites every 17–32 frames at 160 FPS (106–200 ms) were
never left unwatched with D97's 16 frames, which at 60 FPS were 267 ms.

**Results** (GH5, 3440×1440, 160 FPS): pages left unwatched 1,400 → 5,800; the renderer thread in the song about
4.05 → 3.72 ms a frame; the process 3.27 → 3.06 cores in the main menu, 3.29 → 3.11 in the song. The pages are
hashed instead (4.3–5.1 MB a frame, from 0.6) and copied in more runs (about 280 copies a frame, from 131): what
"hash only what a draw reads" ([performance.md](performance.md#what-to-do-next-in-order)) would cut.

**In the other games** (D108 and D109 together, against the same builds without them, the same routes; the renderer
thread's time a frame, and the longest frames of the run):
- GH Metallica (160 FPS, 3440×1440, Ace of Spades): 4.77 → 4.83 ms, frame interval 6.52 → 6.56 ms (the song is
  heavier than 160 FPS allows, with or without them), hashed 4.3 → 5.2 MB a frame. Within the runs' variation.
- GH Van Halen (160 FPS, the career's first song): 4.41 → 4.19 ms; the start's longest frame 254 → 230 ms.
- GH Aerosmith (160 FPS, the first screen): 2.80 → 2.88 ms; the start's 208 → 181 ms and 34 → 23 ms.
- Kameo (the first level, about 85 FPS): 9.84 → 9.59 ms; the opening video's longest frames 31–38 → 32–39 ms.
- GRAW (about 118 FPS): 4.10 → 4.01 ms, hashed 6.1 → 3.0 MB a frame; the loading's 381 → 215 ms, 162 ms → none
  over 20, 127 → 67 ms.
- POTCAWE (60 FPS: D109 changes nothing): 5.80 → 5.86 ms; the loading's 823 → 728, 139 → 133, 108 → 90 ms.
- The same images at each game's spot, no new errors or warnings.

## D110 — Unwatched pages compared with a copy on the CPU, not by hash (2026-10-30, Gustavo)

**Decision:** a page left unwatched (D97) is compared with a copy of what the mirror holds, kept on the CPU, instead
of by hash (`native_x_renderer_compare_unwatched`, on). The copy is an address range of the mirror's size, reserved,
and committed 64 KB at a time as the unwatched pages are first copied through it; each page at its own offset, so a
run of pages is uploaded from it in one `UpdateBuffer`. A changed page's compare stops at its first difference, and
the copy uploaded needs no hash. A page's copy stops counting when the page is watched again, uploaded by the hash
path (`Upload`) or restored by an inline draw (`WriteIfChanged`); a page uploaded through it has no hash, so once
watched again it is copied at its first request. If committing fails, the pages are compared by hash from then on.
The log's uploads line gives the copy's size.

**Why:** Gustavo chose performance.md's "hash only what a draw reads" next. Measured first (the SDK's range request
callback gives every requested byte range): in GH5's and GH Metallica's songs at 160 FPS the draws requested 86–87 %
and 83 % of the bytes hashed (500–800 of 900–1,400 pages whole), so that would have saved a few hundredths of a ms.
The same measurement showed most unwatched pages changed at each request (about 45 % in GH5, 75 % in GH Metallica),
and every changed page was hashed twice: to compare it, then the snapshot uploaded. Per frame in GH5's song, of
`UploadRanges`' 1.0–1.2 ms: the compare hashes 0.34–0.42 ms, `UpdateBuffer` 0.24–0.29, the snapshot hashes 0.12–0.14,
the snapshot copies 0.05, `MakeValid` 0.03.

**Results** (the same build, the option on and off, two runs each, `UploadRanges` timed, every song window):
- GH5, Party Play's song, 3440×1440 / 160 FPS: 1.024 → 0.957 ms a frame (44 windows each), per MB compared 0.219 →
  0.205 ms (−6 %).
- GH Metallica, Ace of Spades, 160 FPS: 1.477 → 1.338 ms (20 windows each), per MB 0.332 → 0.298 ms (−10 %).
- About 1.5 % and 3 % of the renderer thread: less than the hashes' time suggested, as the compare reads the copy
  (not in the cache) as well as the page the game just wrote. The copy took 28 MB in GH5's song.
- The images as before; the other NativeXRenderer games' Test-Games as before.

**Revisit if:** a game leaves unwatched many pages that seldom change (the compare reads twice the memory a hash did:
`native_x_renderer_compare_unwatched=false` to compare), or memory is short: the copy is never decommitted, so it
holds every page ever left unwatched (a ring buffer's, in these games).
