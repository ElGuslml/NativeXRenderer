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
