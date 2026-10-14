# XenosNative — decision log

One entry per decision: date, what was decided, why, and what would make us revisit it. Add entries at the
end, and do not rewrite old ones. If a decision changes, add a new entry that supersedes the old one.

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
