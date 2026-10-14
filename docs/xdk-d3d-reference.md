# The XDK Direct3D library — reference

What XenosNative needs to know about the Direct3D library that Microsoft's Xbox 360 XDK links into every
game, and how to find its pieces in a new executable. Facts carry their source: the game where they were
seen and the XDK version. Facts from Dante's Inferno come from Hell's Gate's
[`NATIVE_D3D_MAP.md`](https://github.com/florinp93/hells-gate-recomp/blob/main/docs/NATIVE_D3D_MAP.md)
(credited in [`credits.md`](credits.md)). Facts from POTCAWE (XDK 2.0.5426) were established in Phase 1
(2026-10-13): their evidence is in [`../../docs/xenos-native-potcawe.md`](../../docs/xenos-native-potcawe.md).

## Contents

1. [The library in a recompiled executable](#the-library-in-a-recompiled-executable)
2. [XDK versions of our games](#xdk-versions-of-our-games)
3. [Entry points](#entry-points)
4. [The device structure](#the-device-structure)
5. [Rebuilding a draw's GPU state](#rebuilding-a-draws-gpu-state)
6. [The library's own draws](#the-librarys-own-draws)
7. [Command buffer, waits and swap](#command-buffer-waits-and-swap)
8. [Predicated tiling](#predicated-tiling)
9. [Occlusion queries](#occlusion-queries)
10. [Surfaces and EDRAM](#surfaces-and-edram)
11. [Shaders, vertex declarations, textures](#shaders-vertex-declarations-textures)
12. [Finding the entry points in a new executable](#finding-the-entry-points-in-a-new-executable)
13. [PM4 and register cheat sheet](#pm4-and-register-cheat-sheet)

## The library in a recompiled executable

- It is linked statically. There are no exports and no symbols, and the retail build has almost no
  strings.
- The code sits in one range, but not alone, and the draw functions are elsewhere:
  - **POTCAWE:** the device code at `0x822AD000`–`0x822C7000`, between the CRT (`0x822A0000`…) and the
    XGraphics helpers (`0x822AA000`–`0x822AD000`) below it; the draws apart, at `0x824ABAD8`–`0x824AC818`.
  - **Dante's Inferno TU2:** `0x827C8000`–`0x827EFFFF`, about 600 functions, plus the draws at
    `0x8291xxxx`.
- Many setters in the XDK headers are inline. The game writes the device's register shadow and sets dirty
  bits itself, without calling anything. The flush that turns dirty groups into PM4 packets runs inside
  each draw (and resolve, clear).
- Different games use different XDK versions. The library's code, and the layout of its device structure,
  can change between versions. Treat every offset as valid only for the version where it was seen.

## XDK versions of our games

Read from the XEX import table: the version that `xboxkrnl.exe` and `xam.xex` were linked against. Use
`node tools/xdkver.mjs <default.xex>`, a personal tool that parses optional header `0x000103FF`.

| Game | XEX | XDK (linked against) |
|---|---|---|
| Kameo: Elements of Power | KameoRecomp | 2.0.2099.0 |
| Ghost Recon Advanced Warfighter | GRAWRecomp | 2.0.2417.0 |
| Pirates of the Caribbean: At World's End | POTCAWERecomp | 2.0.5426.0 |
| Dante's Inferno (Hell's Gate, reference only) | — | not recorded; the game is from 2010 |

## Entry points

The functions XenosNative hooks or needs to understand. Names follow the XDK API; signatures are
`(device = r3, …)`. "Dante" is the address in Dante's Inferno TU2, from Hell's Gate's map.

| Group | Function | Role | Dante (TU2) | POTCAWE (5426) |
|---|---|---|---|---|
| Draw | DrawIndexedVertices | `(prim, baseVertex, startIndex, indexCount)` | `8291D328` | `824AC3C0` |
| Draw | DrawVertices | `(prim, startVertex, vertexCount)` | `8291CF38` | `824ABFD8` |
| Draw | BeginVertices / EndVertices | Inline draw: returns a pointer into the command buffer | `8291CA58` / `8291CA48` | `824ABAD8` / none |
| Draw | DrawVerticesUP | `(prim, vertexCount, data, stride)`: Begin, copy, inlined End | `8291CEF0` | `824ABF78` |
| Targets | SetRenderTarget, SetDepthStencilSurface | Bind surfaces (copy the surface header into RB_COLOR_INFO / RB_DEPTH_INFO) | `827D2278`, `827D25C8` | `822B4F60`, `822B52C8` |
| Targets | Clear, ClearF | `(count, rects, flags, D3DCOLOR, Z, stencil)`, `(flags, rect, float4*, Z, stencil)` | Clear `827EDC18` | `822C6AA0`, `822C6978` |
| Targets | Resolve | `(flags, srcRect, destTexture, destPoint, level, slice, clearColor, Z, …)`; flags 0 = RT0, 4/0x10/0x14 = depth; bit 24 swaps red/blue | `827E5B18` | `822C2048` |
| Targets | BeginTiling / EndTiling | Predicated tiling bracket | `827DCBF8` / `827DCF68` | `822BCC58` / `822BD1E0` |
| Targets | BeginZPass / EndZPass | Z prepass for tiling | — | `822BD698` / `822BD9E0` (likely; unused in the traced scenes) |
| Targets | SetPredication | Tile predication mask | `827DCA88` | `822BCAE8` |
| Targets | SetViewport, SetScissorRect | | — | `822B4E28`, `822B4550` |
| Shaders | SetVertexShader, SetPixelShader | Bind shader objects | `827E8018`, `827E7E58` | `822B7E18`, `822B7B60` |
| Shaders | CreateVertexDeclaration, SetVertexDeclaration | 12-byte elements, `0xFF` terminator | `827E82F0`, `827E81E8` | —, `822B7FE8` |
| Shaders | Program flush | Patches the vertex shader for the declaration, loads the programs and their literal constants | `827EA4A0` | `822B1090` (internal) |
| Shaders | SetShaderGPRAllocation | `(flags, vertexGprs, pixelGprs)`; runs prebuilt one-point draws | — | `822B8120` |
| Constants | SetVertexShaderConstantF, SetPixelShaderConstantF | Float constants into the shadow (also inlined by engines) | `827E7B10`, `827E7BE8` | `822B7848`, `822B7950` |
| Constants | LoadShaderConstantsFromBuffer | Engine constant buffers, bypassing the shadow | `8291D778` | not used |
| Resources | SetStreamSource, SetIndices | Vertex fetch constant `95 − stream`, index buffer | `827D1A10`, `827D1B30` | `822B46A0`, `822B47C0` |
| Resources | SetTexture, sampler setters | Texture fetch constants and filter state | `827D92A0`, `827D0EC8`, `827D1070`, `827D1310`, `827D1430` | `822AC668`, `822B3B70`, `822B3CF8`… |
| Resources | Vertex buffer Lock / Unlock | | `827D71A8` / `827D6268` (likely) | `822ACEE0` / `822ACF30` |
| Resources | Index buffer Lock / Unlock, texture Unlock | | — | `822ACF40` / `822ACF88`, `822AA700` |
| Resources | Release | | `827D7128` (likely) | `822ACE10` |
| Sync | InsertFence, BlockOnFence | GPU fences | `827D4010`, `827D3178` | not used |
| Sync | New command buffer segment | Ends the segment with a fence, starts the next | `827D3F10`, `827D4148` | `822AEC50` |
| Queries | D3DQuery Issue, GetData | Occlusion queries | Issue `827DAA98` | `822BA590`, `822BA818` |
| Frame | Swap (and Synchronize), Present | End of frame, VdSwap | Swap `827D4EE0` | Swap `822B58B0`, Synchronize `822B5D50`, Present `822B5D58` |
| Frame | SetRenderState_PresentInterval | Writes the present interval | | `822B3B40` |
| Device | Direct3D_CreateDevice, device reset | | `827E2FC0`, reset `827D59D8` | `822AF860`, reset `822B6338` |
| Surfaces | XGSetSurfaceHeader | Fills a surface header (EDRAM size, tiles) | | `822ABE40` |
| Render states | Cull, fill, blend, alpha ref, depth/stencil, poly offset, color mask, HiZ… | Each identified by the register it writes; many are inlined | about 17 addresses | about 90 addresses (`local_docs/02`) |

## The device structure

The D3D device object in guest memory is the source of truth XenosNative reads at draw time. Its layout
depends on the XDK version.

| Field | Dante's Inferno (TU2) | POTCAWE (XDK 5426) |
|---|---|---|
| Dirty masks: six u64 (VS constants, PS constants, register groups, fetch, misc, tiling-dependent) | `+0`…`+40` | `+0`…`+40` |
| Command buffer write pointer / limit / kick threshold | `+48` / `+52` / `+56` | `+48` / `+52` / `+56` |
| Fetch constants (32 slots × 24 bytes) | `+1152` | `+1024` |
| Vertex shader float constants (256 vec4) | `+1920` | `+1792` |
| Pixel shader float constants (256 vec4) | `+6016` | `+5888` |
| Bool and loop constants (8 + 32 dwords) | `+10112` | `+9984` |
| Register shadow `0x2000`/`0x2100`/`0x2180`/`0x2200`/`0x2280`/`0x2300`/`0x2380` | `+10368`/`+10444`/`+10528`/`+10548`/`+10596`/`+10680`/`+10832` | `+10240`/`+10316`/`+10400`/`+10420`/`+10468`/`+10552`/`+10704` |
| Write-back block pointer (`[+0]` last fence reached) | | `+10768` |
| Pointer to the swap/vblank block | | `+10772` |
| Fence counter (steps by 2) | `+10908` | `+10780` |
| Write-back slots | | `+11196`, `+11200` |
| Current vertex declaration | `+11992` | `+11684` |
| D3DRS_PRESENTIMMEDIATETHRESHOLD, SCISSORTESTENABLE | | `+11716`, `+11720` |
| Sampler filter state | `+12100` | `+11788`, `+11866` (per-sampler bytes) |
| Current index buffer | `+12612` | `+12300` |
| Current render targets RT0–3, depth | `+12616`…`+12632` | `+12304`…`+12320` |
| Current vertex buffers, textures | `+12636 + 4·s`, — | `+12324 + 4·s`, `+12408 + 4·i` |
| Viewport, scissor | | `+12512`, `+12540` |
| Current pixel shader, vertex shader | `+12868`, `+12872` | `+12556`, `+12560` |
| Predication mask | `+12884` | `+12572` |
| Targets saved by BeginTiling | `+12904`…`+12920` | `+12592`…`+12608` |
| Tile count, tile rects | `+12924`, `+12928` | `+12612`, `+12616` |
| Command buffer pointer saved by BeginVertices | `+13652` | `+13300` |
| D3DRS_PRESENTINTERVAL | | `+13444` |
| Vblank count; last flip vblank; swap counters | | `+16396`; `+16404`; `+16408`/`+16416` |
| Refresh rate (from VdQueryVideoMode) | | `+21400` |
| GPU wait time counters | | `+21440`, `+21448` |

**Between XDK versions** (POTCAWE 5426 against Dante's Inferno): everything up to the end of the register
shadow is 128 bytes lower in POTCAWE. The block of current objects (targets, shaders, predication, tiling)
is 312 bytes lower. Other fields move by other amounts (BeginVertices' pointer: −352). So a new version
inserts or removes a few blocks: start from the nearest known layout and confirm each field in the code
that uses it.

**Behavior worth knowing:**
- **The write pointer holds the last dword written**, not the next free one: the library writes packets
  with `stwu` (pre-increment). A call's packets are `(pointer before, pointer after]`. (POTCAWE.)
- **Nothing reaches the command buffer until a draw, clear or resolve.** The flush writes the dirty groups
  at that point. (Dante's Inferno, POTCAWE.)
- **Dirty bit order.** Float constant group g (4 vec4) is bit `63 − g` of its mask; fetch slot s is bit
  `31 − s` of the low half of the fetch mask (the flush shifts it up and walks it with `cntlzd`).
  (POTCAWE.)
- **Engine constant buffers bypass the shadow** in Dante's Inferno (`LoadShaderConstantsFromBuffer`).
  POTCAWE does not use them: its only `LOAD_ALU_CONSTANT` is the program flush's.
- **Surface objects are reused** for different surfaces. Re-read the surface header at every bind instead of
  caching it per object. (Dante's Inferno.)

## Rebuilding a draw's GPU state

The rules that rebuild the full GPU state of a D3D draw, as checked by XenosNative's observer against the
SDK command processor's register file: **100 % of the draws** of POTCAWE's title screen, main menu and
Port Royal dock, with and without predicated tiling (`local_docs/04-observer.md`). Hell's Gate reached the
same with Dante's Inferno with rules 1–5 in their Dante form.

1. **Render state** is the register shadow **after the draw call returns**. The flush adjusts a few
   registers in the shadow itself (`RB_MODECONTROL`, `RB_HIZCONTROL`, `RB_COLOR_INFO` from the bound
   shaders and targets), so a snapshot before the call is wrong for them.
2. **Registers written outside the shadow**, straight into the command buffer: `SQ_PROGRAM_CNTL`,
   `SQ_CONTEXT_MISC`, `SQ_INTERPOLATOR_CNTL` (the program flush, from the shader objects) and
   `VGT_INDX_OFFSET` (DrawIndexedVertices' base vertex). Their value is the last one written in the
   command buffer, by this call or an earlier one.
3. **Fetch constants** are the shadow, except what packets wrote outside the flush:
   - the inline draws' stream (slot 31, vertex fetch 95, pointing into the command buffer);
   - the library's own clears and resolves (slots 0–3).

   Such a value stays until the slot is flushed from the shadow again (its dirty bit).
4. **Float constants** need a model of the GPU's constant file, because shader **literal constants**
   persist:
   - a group that is dirty when the draw call starts takes the shadow's values;
   - the program flush then loads the bound shaders' literal constants with `LOAD_ALU_CONSTANT` (POTCAWE:
     PS c251–c255, VS c254–c255). They stay in the GPU until a later flush or literal overwrites them;
   - the library's own draws (Present's copy) write constants too.

   One warm-up frame is enough to know every literal in use.
5. **Bool and loop constants** are the shadow.
6. **Not used by draws**, so not part of the state: the resolve's copy registers (`RB_COPY_*`, written by
   Resolve), and `RB_SAMPLE_COUNT_ADDR` (written by Query Issue).
7. **Per tile**, only `PA_SC_WINDOW_OFFSET`, `PA_SC_WINDOW_SCISSOR_TL/BR` and the query's
   `RB_SAMPLE_COUNT_ADDR` differ (bin-predicated packets). A renderer that draws the tiled pass once at full
   size ignores them.

8. **The draw packet's own registers.** `VGT_DRAW_INITIATOR` (primitive type, index source and size,
   count), `VGT_DMA_BASE` and `VGT_DMA_SIZE` (index buffer address, count and endianness) are not
   register writes: the command processor decodes them from the `DRAW_INDX`/`DRAW_INDX_2` payload. The XDK
   **bin-predicates every draw packet**, tiled or not, so a reader that skips predicated packets misses
   them. (Found in Phase 2: the observer compares the shadow's register groups, which do not include
   these three, so rules 1–7 at 100 % did not cover them. D20.)
9. **Microcode** is copied by the GPU when it executes `IM_LOAD`; later draws run that copy. A renderer
   copies it when it reads the packet, not when a later draw needs it, and reads each command buffer
   segment as soon as the library ends it (`KickSegment`). Even then, POTCAWE's inline vertex shaders
   (`IM_LOAD_IMMEDIATE`) are sometimes found overwritten by a coherency packet sequence while a level
   loads (D20, D21; cause open).

Rules 2–4 and 8 read the packets the D3D call itself wrote, right after it wrote them. That is a few
hundred bytes per draw, and no emulation: only register writes, constant loads, shader loads, indirect
buffers and draw packets are followed.

## The library's own draws

The command processor runs draw packets that no game draw call issued. In POTCAWE every one of them is the
library's:

| Source | What the GPU draws | For a native renderer |
|---|---|---|
| Clear / ClearF | A 3-vertex `RECTLIST` per cleared surface, through a surface the library configures in the packet: half the pitch at 4× MSAA for a 2× MSAA target (POTCAWE: the 1280-pitch scene as 640 at 4×, the 1040-pitch shadow maps as 520). Shaders loaded with `IM_LOAD_IMMEDIATE`. | A host clear of the bound targets. |
| Resolve | A 3-vertex `RECTLIST` in copy mode (`RB_MODECONTROL` 6). | A copy or a texture reference. |
| SetShaderGPRAllocation | `INDIRECT_BUFFER` to three buffers prebuilt at device creation, each with its own shaders and 8 one-point draws (POTCAWE: at `0x1FCA1000` physical, 24 points per call). They flush the shader pipe after the GPR split changes. | Nothing: the host has no GPR split. |
| Present | Constant writes and a copy to the front buffer. | Present. |

## Command buffer, waits and swap

Seen in POTCAWE (XDK 5426).

- **Command buffer.** One ring of several MB in physical memory (POTCAWE: around `0x0769xxxx`–`0x07ABxxxx`),
  written through `device+48`. When the pointer passes the threshold at `+56`, `sub_822AEC50` ends the
  segment and starts a new one: 8–17 times per frame, always from inside another call. A segment ends with
  `INVALIDATE_STATE` and two `EVENT_WRITE_SHD`: the fence (`[+10780]`, then `+2`) and the ring position go to
  the write-back block `[+10768]`. `sub_822AE6C8(device, ring, 0, callback, argument)` inserts a GPU callback
  packet.
- **Waits.** Six wait loops in the library share one pattern:
  - `sub_822AF458(device, kind)` records the start of the wait (device, kind, timestamp, CP read pointer).
  - `sub_822AF608` runs on every iteration that the condition is still false: a short `db16cyc` pause,
    then a "GPU hung" check that calls `sub_822B1ED8` after 5 seconds without progress.
  - The loops: ring space `sub_822AE600`, ring range `sub_822ADA00`, fence `sub_822AD968`, pending swaps
    `sub_822B5D58`, write-back slots `sub_822C3388` and `sub_822C3628`.

  On a PC those loops spin a core. POTCAWERecomp hooks the two helpers so they sleep (`src/gpu_wait.cpp`).
- **Fences.** The game never calls InsertFence or BlockOnFence. Resources carry the fence of their last
  use at `+8` (stamped by SetIndices and SetStreamSource when they are unbound), and Lock waits for it.
- **Swap.** `sub_822B58B0(device, frontbuffer)` is Swap; `frontbuffer = 0` is Synchronize, which has an alias
  at `sub_822B5D50`. It:
  1. maps the present interval at `device+13444` (0/1 → 1, 2 → 2, 4 → 3, `0x80000000` → 0);
  2. builds the descriptor `frontbuffer | interval << 8 | threshold`;
  3. inserts a GPU callback to `sub_822B5718`, which flips at vblank.

  Synchronize also writes a WAIT_REG_MEM so that the GPU waits until the callback clears
  `[device+10772]+4`. In POTCAWE, Swap runs once per frame, as Synchronize, from the engine's present.
- **Present.** `sub_822B5D58` calls `VdSwap`; it does not call Swap. The game's present wrapper is
  `sub_823B1CB8`: SetShaderGPRAllocation(0, 0, 0), a GPU callback, Synchronize, timing, Present.

## Predicated tiling

Seen in POTCAWE (XDK 5426).

- **The tiles.** The engine computes its tile rectangles (two tiles, rows 0–416 and 352–720, at 1280×720
  with 2× MSAA) and renders a pass between BeginTiling and EndTiling. POTCAWE has three tiled passes per
  frame at the dock.
- **The replay.** The library replays the pass's command buffer once per tile under SET_BIN_MASK /
  SET_BIN_SELECT. Per-tile values are bin-predicated packets: the window offset and scissor
  (`PA_SC_WINDOW_*`) and the query address (`RB_SAMPLE_COUNT_ADDR`). Nothing else differs between the tiles
  (observer).
- **The CPU side.** The CPU issues each draw once per frame; only the GPU runs it per tile (observer: 1021
  draw calls executed twice, from one call each).
- **Resolves.** POTCAWE does not let EndTiling resolve. It resolves each tile itself: SetPredication(tile
  mask: `0x3`, `0xC`), then Resolve with that tile's rectangle, into the same texture. EndTiling then runs
  with no resolve flags.
- **Dante's Inferno** uses a single BeginTiling/EndTiling per frame in gameplay, with one tile.

## Occlusion queries

Seen in POTCAWE (XDK 5426), from POTCAWERecomp's `local_docs/06-sun-glow.md`.

- **Issue.** Issue (`sub_822BA590`) reaches `sub_822C4848`, which emits EVENT_WRITE_ZPD after setting
  RB_SAMPLE_COUNT_ADDR (0x2325):
  - BEGIN writes the address as `block + 32`;
  - END writes it as `block + 0`.
- **The marker.** Before END the library writes the marker `0xFFFFFEED` into ZPass_A/ZPass_B (`+16`/`+20`)
  of the **last tile's** block only.
- **Tiles.** With several tiles (`query+148` > 1), each tile's pass uses its own block, through
  bin-predicated SET_CONSTANTs.
- **GetData** (`sub_822BA818`) sums `ZPass_A + ZPass_B` of each block's end struct, minus the begin struct
  (`+48`/`+52`). The result is pending while the last block's ZPass_A and ZPass_B both still hold the
  marker.

## Surfaces and EDRAM

- **EDRAM in tiles.** EDRAM is addressed in tiles of 80×16 samples (5120 bytes), 2048 tiles in total. The
  tile math shows up as `+79` / `+15` roundings and as a `2048` budget check.
- **Surface headers.** XGSetSurfaceHeader (POTCAWE `sub_822ABE40`) fills a surface header and calls the
  tile-size routine (`sub_822AA730`). POTCAWE's engine creates its surfaces through its own wrapper
  (`sub_824D5750`), with the MSAA type as `r6`. A surface keeps RB_SURFACE_INFO at `+24`,
  RB_COLOR_INFO/RB_DEPTH_INFO at `+28`, and its size at `+36`.
- **Aliasing.** Surfaces with different pitch or MSAA can alias the same EDRAM. The library's ClearF uses
  this itself (see [the library's own draws](#the-librarys-own-draws)): the "engine trick" seen from the
  Xenos path was the XDK's fast clear.
- **A surface's format changes between its draws and its resolve.** The same surface object reads
  `2_10_10_10_FLOAT_AS_16_16_16_16` in RB_COLOR_INFO at its draws and `2_10_10_10_FLOAT` at the Resolve that
  follows (POTCAWE's scene and back buffer surfaces, XDK 5426). Both are one EDRAM storage format; the
  "_AS_" variant only changes what a resolve converts to. A renderer that keys host targets by the
  surface's format must use the storage format (`xenos::GetStorageColorFormat`), or the draws and the
  resolve land on different targets. (Phase 2.)

## Shaders, vertex declarations, textures

**Texture addresses.** A texture header in memory (`XGSetTextureHeader`, the fetch constant at `+28` in the
object) holds the CPU (virtual) base address, for example `0xA84D2000`. The fetch constant the device
writes for the GPU holds the physical address, `(address & 0x1FFFFFFF)` plus 4 KB for the `0xE0000000`
view (`0x084D2000` here). A renderer that keys textures by address (resolve destinations, sampled
textures) must convert the header's address the same way. (POTCAWE, XDK 5426; found in Phase 2.)

**Resolve arguments.** `Resolve(device, flags, srcRect*, destTexture, destPoint*, level, slice, clearColor*,
Z, stencil, params)`: `flags` bits 0–2 pick RT0–3 or depth (4), bits 4–7 one sample (fragment 0–3) instead of
the average. `slice` is the destination slice of a stacked texture: POTCAWE resolves its two shadow cascades
into slices 0 and 1 of one stacked `k_24_8_FLOAT` texture. The destination's fetch constant gives the
dimension (dword 5, bits 9–10) and, for 2D and stacked textures, the size (dword 2: width − 1 in bits 0–12,
height − 1 in 13–25, slices − 1 in 26–31). (Phase 2.)

**The front buffer is read through its fetch constant.** A resolve with RB_COPY_DEST_INFO's red/blue swap
writes BGRA, and the front buffer texture's swizzle swaps it back; scan-out (the SDK's presenter) applies
that swizzle. A renderer that presents a host copy of the resolve must apply it too. (POTCAWE, Phase 2.)

Seen in Dante's Inferno (Hell's Gate) and confirmed in POTCAWE (XDK 5426).

- **Shader objects.**
  - **Pixel shader:** program block at `PS + [PS+0x40]`. Microcode at `[PS+0x18] + [block+40]`, size
    `[block+44]` bytes. POTCAWE's `SQ_INTERPOLATOR_CNTL` comes from `[block+64]`.
  - **Vertex shader:** microcode base `[VS+0x20]`, and per-declaration variants. The active variant's
    block is at `VS + [VS+896] + 872` in POTCAWE (`+892` in Dante's Inferno).
  - **Literal constants:** loaded by the program flush from `PS+40` and `VS+872` with `LOAD_ALU_CONSTANT`.
- **Vertex shader patching.** The vertex shader is patched per vertex declaration, and the flush binds the
  patched variant. Read the program the flush actually loads (the last IM_LOAD of the draw's packets), not
  the object's default.
- **Textures.** A texture object holds its 6-dword fetch constant at `+0x1C`. The sampler setters patch
  dwords of that fetch constant.
- **Vertex and index buffers.** A vertex buffer object holds its fetch constant at `+0x18` (base, size) and
  an index buffer its base and size there. SetStreamSource writes vertex fetch constant `95 − stream`.
  Resource type: the low 4 bits of `+0` (1 VB, 2 IB, 3 texture, 4 surface); reference count at `+4`; fence
  at `+8`.
- **Unlock flushes the CPU cache** over the range it unlocks (`dcbf` loop, POTCAWE `sub_822AF398`): that
  call names exactly the memory the CPU just wrote.
- **Not every CPU write goes through Unlock.** POTCAWE's Bink videos write their texture planes directly.

## Finding the entry points in a new executable

Work from the cheapest method to the most expensive. Record each finding with its evidence in the game's
XenosNative document. For POTCAWE, steps 2–4 with the tools in [`../tools/`](../tools/README.md), then the
trace and the observer at 100 %, took one day (2026-10-13).

1. **Same XDK version as a mapped game?** Then the library's code is probably identical. Compare the bytes
   of a known function, or a signature of its first instructions, against the new image. Every match is a
   free mapping. Verify the device offsets anyway.
2. **PM4 signatures, first.** Type-3 packet headers appear as `lis`+`ori` immediates:
   `0xC0000000 | (dword_count − 1) << 16 | opcode << 8 | predicate`. A scan of every function for them finds
   the draws (`DRAW_INDX` 0x22, `DRAW_INDX_2` 0x36), wherever they are, the resolve (`EVENT_WRITE` with the
   RB_COPY registers), the clear (`IM_LOAD_IMMEDIATE` + `DRAW_INDX_2`), the tiling functions (`SET_BIN_*`),
   the queries (`EVENT_WRITE_ZPD`) and the segment end (`EVENT_WRITE_SHD`). In POTCAWE this alone found the
   draw functions, which sit away from the rest of the library.
3. **The draw's flush names the device layout.** Every draw calls the flush helpers with
   `(device, dirty mask, first register, device + offset)`: the float constants, fetch constants and every
   register shadow group come out of one function. Then summarize every function by the device offsets it
   reads and writes: a setter is a function that writes one known field.
4. **Register IDs.** Type-0 writes (`(count − 1) << 16 | register`) and shadow offsets name the register a
   setter writes. The RB_COPY_* registers (0x2318–0x231E) are written by the resolve, and RB_COLOR_CLEAR and
   RB_DEPTH_CLEAR by the clear.
5. **Anchor functions** have unique behavior that is easy to spot in the generated C++. The generated C++
   keeps each PPC instruction as an `// asm` comment, and `loc_XXXXXXXX:` labels give addresses (blocks are
   not always emitted in address order).
   - Swap and Present: Present calls the `VdSwap` import, and Swap reads the present interval.
   - The wait helpers: `db16cyc` loops, and reads of the `KeTimeStampBundle` milliseconds.
   - XGSetSurfaceHeader: the `+79`/`+15` tile math.
   - The callback insert: called with a function address as an argument.
6. **Call-site census.** Count the `bl` calls into the library from the engine's code. Draws are called
   from a few submission functions. Many setters are inlined, so a low count is normal.
7. **Engine wrappers.** Most engines wrap D3D in their own renderer (POTCAWE's renderer is around
   `0x823Fxxxx`). The wrapper's purpose is often clear from its arguments (surface sizes, MSAA levels), and
   it names the D3D function it calls.
8. **Confirm at run time with the trace** (`--xenos_native_trace`, F9): call counts per frame, argument
   values, the order of calls in a pass. Then the observer (`--xenos_native_verify`, F10) proves the
   device layout: every draw's rebuilt state against the command processor's.

## PM4 and register cheat sheet

From the SDK's `include/rex/graphics/xenos.h`, `register_table.inc` and `command_processor.cpp`.

| Type-3 opcode | Value | Meaning |
|---|---|---|
| DRAW_INDX | `0x22` | Draw with an index buffer fetched from memory |
| DRAW_INDX_2 | `0x36` | Draw with indices in the packet |
| IM_LOAD / IM_LOAD_IMMEDIATE | `0x27` / `0x2B` | Load shader microcode |
| SET_CONSTANT | `0x2D` | Constants by type: `(offset_type >> 16)` 0 ALU (`0x4000`), 1 fetch (`0x4800`), 2 bool (`0x4900`), 3 loop (`0x4908`), 4 registers (`0x2000`); index `offset_type & 0x7FF` |
| SET_CONSTANT2, SET_SHADER_CONSTANTS | `0x55`, `0x56` | Registers from `offset_type & 0xFFFF` |
| LOAD_ALU_CONSTANT | `0x2F` | `(address, offset_type, size)`: constants from memory (shader literals, engine constant buffers) |
| INVALIDATE_STATE | `0x3B` | Selective state invalidation |
| WAIT_REG_MEM | `0x3C` | GPU waits on a register or memory value |
| MEM_WRITE | `0x3D` | Write words to memory |
| INDIRECT_BUFFER | `0x3F` | `(address, length)`: run another command buffer |
| EVENT_WRITE / _SHD / _EXT / _ZPD | `0x46` / `0x58` / `0x5A` / `0x5B` | Events (resolve, done/fence, extents, occlusion) |
| SET_BIN_MASK / SET_BIN_SELECT | `0x50` / `0x51` (`0x60`–`0x63` LO/HI) | Predicated tiling |
| INTERRUPT | `0x54` | CPU interrupt (swap and vblank callbacks) |

Type-0 packets: `count = ((header >> 16) & 0x3FFF) + 1`, first register `header & 0x7FFF`, bit 15 = all
values to one register. Type-1: two registers (`header & 0x7FF`, `(header >> 11) & 0x7FF`). Type-2: no-op.
Bit 0 of a type-3 header: bin-predicated.

| Register | ID | Meaning |
|---|---|---|
| RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO | `0x2000`, `0x2001`, `0x2002` | Surface pitch/MSAA, color and depth EDRAM base and format |
| PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL/BR | `0x2080`–`0x2082` | Per tile with predicated tiling |
| VGT_INDX_OFFSET | `0x2102` | Base vertex, written by DrawIndexedVertices |
| SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, SQ_INTERPOLATOR_CNTL | `0x2180`–`0x2182` | Written by the program flush, outside the shadow |
| RB_MODECONTROL | `0x2208` | 4 color, 5 depth only, 6 copy (resolve) |
| SQ_VS_CONST, SQ_PS_CONST | `0x2307`, `0x2308` | Float constant windows (POTCAWE: VS base 0, PS base 256) |
| RB_COPY_CONTROL, RB_COPY_DEST_BASE … | `0x2318`, `0x2319` … | Resolve setup |
| RB_DEPTH_CLEAR, RB_COLOR_CLEAR | `0x231D`, `0x231E` | Clear values |
| RB_SAMPLE_COUNT_ADDR | `0x2325` | Occlusion query result address |
