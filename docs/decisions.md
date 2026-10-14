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
