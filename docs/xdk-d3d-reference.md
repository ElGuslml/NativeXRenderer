# The XDK Direct3D library — reference

What NativeXRenderer needs to know about the Direct3D library that Microsoft's Xbox 360 XDK links into every
game, and how to find its pieces in a new executable. Facts carry their source: the game where they were
seen and the XDK version. Facts from Dante's Inferno come from Hell's Gate's
[`NATIVE_D3D_MAP.md`](https://github.com/florinp93/hells-gate-recomp/blob/main/docs/NATIVE_D3D_MAP.md)
(credited in [`credits.md`](credits.md)). Facts from POTCAWE (XDK 2.0.5426) were established in Phase 1
(2026-10-13): their evidence is in [`../../docs/native-x-renderer-potcawe.md`](../../docs/native-x-renderer-potcawe.md).
Facts from GRAW (Ghost Recon Advanced Warfighter, XDK 2.0.2417) were established on 2026-10-15 (D59): their
evidence is in GRAWRecomp's `docs/native-x-renderer-graw.md`.

## Contents

1. [The library in a recompiled executable](#the-library-in-a-recompiled-executable)
2. [XDK versions of our games](#xdk-versions-of-our-games)
3. [Entry points](#entry-points)
4. [The device structure](#the-device-structure)
5. [Rebuilding a draw's GPU state](#rebuilding-a-draws-gpu-state)
6. [The library's own draws](#the-librarys-own-draws)
7. [Command buffer, waits and swap](#command-buffer-waits-and-swap)
8. [Predicated tiling](#predicated-tiling)
9. [Threads](#threads)
10. [Occlusion queries](#occlusion-queries)
11. [Surfaces and EDRAM](#surfaces-and-edram)
12. [Shaders, vertex declarations, textures](#shaders-vertex-declarations-textures)
13. [Finding the entry points in a new executable](#finding-the-entry-points-in-a-new-executable)
14. [PM4 and register cheat sheet](#pm4-and-register-cheat-sheet)

## The library in a recompiled executable

- It is linked statically. There are no exports and no symbols, and the retail build has almost no
  strings.
- The code sits in one range, but not alone, and the draw functions are elsewhere:
  - **POTCAWE:** the device code at `0x822AD000`–`0x822C7000`, between the CRT (`0x822A0000`…) and the
    XGraphics helpers (`0x822AA000`–`0x822AD000`) below it; the draws apart, at `0x824ABAD8`–`0x824AC818`.
  - **Dante's Inferno TU2:** `0x827C8000`–`0x827EFFFF`, about 600 functions, plus the draws at
    `0x8291xxxx`.
  - **GRAW:** `0x824C4578`–`0x824E0A40`, the draws first, inside it. XGraphics-style copy and tiling helpers
    follow at `0x824E0B00`. A bare `blr` with 513 callers (`sub_824D4868`, the PIX markers compiled out)
    sits in the middle: never hook it.
- Many setters in the XDK headers are inline. The game writes the device's register shadow and sets dirty
  bits itself, without calling anything. The flush that turns dirty groups into PM4 packets runs inside
  each draw (and resolve, clear).
- Different games use different XDK versions. The library's code, and the layout of its device structure,
  can change between versions. Treat every offset as valid only for the version where it was seen.
- **Link-time code generation (LTCG)** can fold the library into the engine (Kameo, XDK 2099): EndVertices, the
  typed Lock/Unlock thunks and parts of SetVertexShader and SetPixelShader inlined into the engine, and functions
  specialized for their callers' constant arguments (Clear without a stencil, BeginTiling always clearing to Z 1.0,
  EndTiling with only the device, BeginIndexedVertices for one primitive type). Callers still pass every argument
  in the usual registers. Look for an inlined EndVertices: `lwz rX,<inline end>(dev); stw rX,0(dev)` in the engine
  (the profile's `inline_draw_end`, D77).
- **Memory above 0xE0000000** (physical memory in 4 KB pages) is where Kameo's command buffer lives; on Windows the
  SDK maps that view 4 KB lower, and every read by virtual address must go through its translation (D76).

## XDK versions of our games

Read from the XEX import table: the version that `xboxkrnl.exe` and `xam.xex` were linked against. Use
recomp-tools' `node tools/xex/xdkver.mjs <default.xex>` (or `xex_summary.mjs`), which parses optional header
`0x000103FF`.

| Game | XEX | XDK (linked against) |
|---|---|---|
| Kameo: Elements of Power | KameoRecomp | 2.0.2099.0 |
| Ghost Recon Advanced Warfighter | GRAWRecomp | 2.0.2417.0 |
| Pirates of the Caribbean: At World's End | POTCAWERecomp | 2.0.5426.0 |
| Dante's Inferno (Hell's Gate, reference only) | — | not recorded; the game is from 2010 |

## Entry points

The functions NativeXRenderer hooks or needs to understand. Names follow the XDK API; signatures are
`(device = r3, …)`. "Dante" is the address in Dante's Inferno TU2, from Hell's Gate's map.

| Group | Function | Role | Dante (TU2) | POTCAWE (5426) | GRAW (2417) |
|---|---|---|---|---|---|
| Draw | DrawIndexedVertices | `(prim, baseVertex, startIndex, indexCount)` | `8291D328` | `824AC3C0` | `824C4E38` |
| Draw | DrawVertices | `(prim, startVertex, vertexCount)` | `8291CF38` | `824ABFD8` | `824C4BF8` |
| Draw | BeginVertices / EndVertices | Inline draw: returns a pointer into the command buffer | `8291CA58` / `8291CA48` | `824ABAD8` / none | `824C4578` / `824C4818`; BeginIndexedVertices `824C48C0` |
| Draw | DrawVerticesUP | `(prim, vertexCount, data, stride)`: Begin, copy, inlined End | `8291CEF0` | `824ABF78` | `824C4838` |
| Targets | SetRenderTarget, SetDepthStencilSurface | Bind surfaces (copy the surface header into RB_COLOR_INFO / RB_DEPTH_INFO) | `827D2278`, `827D25C8` | `822B4F60`, `822B52C8` | `824D0C50` (engine alias `824D1188`), `824D0FB8` |
| Targets | Clear, ClearF | `(count, rects, flags, D3DCOLOR, Z, stencil)`, `(flags, rect, float4*, Z, stencil)` | Clear `827EDC18` | `822C6AA0`, `822C6978` | `824CDCC8`, `824CD598` |
| Targets | Resolve | `(flags, srcRect, destTexture, destPoint, level, slice, clearColor, Z, …)`; flags 0 = RT0, 4/0x10/0x14 = depth; bit 24 swaps red/blue | `827E5B18` | `822C2048` | `824D1960` |
| Targets | BeginTiling / EndTiling | Predicated tiling bracket | `827DCBF8` / `827DCF68` | `822BCC58` / `822BD1E0` | `824D2868` / `824D2CA0` |
| Targets | BeginZPass / EndZPass | Z prepass for tiling | — | `822BD698` / `822BD9E0` (likely; unused in the traced scenes) | not present |
| Targets | SetPredication | Tile predication mask | `827DCA88` | `822BCAE8` | `824D2730` |
| Targets | SetViewport, SetScissorRect | | — | `822B4E28`, `822B4550` | `824D0488`, `824D03C0` (library-internal) |
| Shaders | SetVertexShader, SetPixelShader | Bind shader objects | `827E8018`, `827E7E58` | `822B7E18`, `822B7B60` | `824C9098`, `824C8D28` |
| Shaders | CreateVertexDeclaration, SetVertexDeclaration | 12-byte elements, `0xFF` terminator | `827E82F0`, `827E81E8` | —, `822B7FE8` | —, `824CA600` |
| Shaders | Program flush | Patches the vertex shader for the declaration, loads the programs and their literal constants | `827EA4A0` | `822B1090` (internal) | `824D98C8` (internal; no literal loads) |
| Shaders | SetShaderGPRAllocation | `(flags, vertexGprs, pixelGprs)`; runs prebuilt one-point draws | — | `822B8120` | `824C9200` (its draws inline) |
| Constants | SetVertexShaderConstantF, SetPixelShaderConstantF | Float constants into the shadow (also inlined by engines) | `827E7B10`, `827E7BE8` | `822B7848`, `822B7950` | `824C8788`, `824C88D0` |
| Constants | LoadShaderConstantsFromBuffer | Engine constant buffers, bypassing the shadow | `8291D778` | not used | not used |
| Resources | SetStreamSource, SetIndices | Vertex fetch constant `95 − stream`, index buffer | `827D1A10`, `827D1B30` | `822B46A0`, `822B47C0` | `824D06D0` (no fetch constant: the flush writes it), `824D0770` |
| Resources | SetTexture, sampler setters | Texture fetch constants and filter state | `827D92A0`, `827D0EC8`, `827D1070`, `827D1310`, `827D1430` | `822AC668`, `822B3B70`, `822B3CF8`… | `824C6F88`; the render-state table |
| Resources | Vertex buffer Lock / Unlock | | `827D71A8` / `827D6268` (likely) | `822ACEE0` / `822ACF30` | `824D6690` / `824D6700` |
| Resources | Index buffer Lock / Unlock, texture Unlock | | — | `822ACF40` / `822ACF88`, `822AA700` | `824D67C0` / `824D6828`, `824C5210` |
| Resources | Release | | `827D7128` (likely) | `822ACE10` | `824D64F8` (likely) |
| Sync | InsertFence, BlockOnFence | GPU fences | `827D4010`, `827D3178` | not used | not needed |
| Sync | New command buffer segment | Ends the segment with a fence, starts the next | `827D3F10`, `827D4148` | `822AEC50` | kick `824D7E80`, new segment `824D7C08` |
| Queries | D3DQuery Issue, GetData | Occlusion queries | Issue `827DAA98` | `822BA590`, `822BA818` | `824D3B48`, `824D3CF0` |
| Frame | Swap (and Synchronize), Present | End of frame, VdSwap | Swap `827D4EE0` | Swap `822B58B0`, Synchronize `822B5D50`, Present `822B5D58` | vblank sync `824CC858`, Present (VdSwap) `824CC9C8` |
| Frame | SetRenderState_PresentInterval | Writes the present interval | | `822B3B40` | `824CF9A0` |
| Device | Direct3D_CreateDevice, device reset | | `827E2FC0`, reset `827D59D8` | `822AF860`, reset `822B6338` | `824D69A0` (likely) |
| Surfaces | XGSetSurfaceHeader | Fills a surface header (EDRAM size, tiles) | | `822ABE40` | surface creation `824C6BB8` |
| Render states | Cull, fill, blend, alpha ref, depth/stencil, poly offset, color mask, HiZ… | Each identified by the register it writes; many are inlined | about 17 addresses | about 90 addresses (`local_docs/02`) | about 100 handlers at `824CDFF8`–`824D0360`, called through the device's table |

## The device structure

The D3D device object in guest memory is the source of truth NativeXRenderer reads at draw time. Its layout
depends on the XDK version.

| Field | Dante's Inferno (TU2) | POTCAWE (XDK 5426) | GRAW (XDK 2417) |
|---|---|---|---|
| Dirty masks (u64) | six, `+0`…`+40`: VS constants, PS constants, register groups, fetch, misc, tiling-dependent | the same | five `+16`…`+48`: VS, PS, then fetch + bool/loop + `0x2000`, the groups `0x2100`–`0x2280`, misc (`0x2300`, `0x2380`, program, streams) |
| Command buffer write pointer / limit / kick threshold | `+48` / `+52` / `+56` | `+48` / `+52` / `+56` | `+0` / `+4` / `+8` |
| Fetch constants (32 slots × 24 bytes) | `+1152` | `+1024` | `+1152` |
| Vertex shader float constants (256 vec4) | `+1920` | `+1792` | `+1920` |
| Pixel shader float constants (256 vec4) | `+6016` | `+5888` | `+6016` |
| Bool and loop constants (8 + 32 dwords) | `+10112` | `+9984` | `+10112` |
| Register shadow `0x2000`/`0x2100`/`0x2180`/`0x2200`/`0x2280`/`0x2300`/`0x2380` | `+10368`/`+10444`/`+10528`/`+10548`/`+10596`/`+10680`/`+10832` | `+10240`/`+10316`/`+10400`/`+10420`/`+10468`/`+10552`/`+10704` | `+11456`/`+11532`/`+11616`/`+11636`/`+11684`/`+11768`/`+11920` |
| Write-back block pointer (`[+0]` last fence reached) | | `+10768` | `+10384` |
| Pointer to the swap/vblank block | | `+10772` | `+10388` |
| Fence counter (steps by 2) | `+10908` | `+10780` | `+10396` |
| Write-back slots | | `+11196`, `+11200` | |
| Current vertex declaration | `+11992` | `+11684` | `+11408` |
| D3DRS_PRESENTIMMEDIATETHRESHOLD, SCISSORTESTENABLE | | `+11716`, `+11720` | `+11452` (bits 23–29), `+11952` |
| Sampler filter state | `+12100` | `+11788`, `+11866` (per-sampler bytes) | |
| Current index buffer | `+12612` | `+12300` | `+12532` |
| Current render targets RT0–3, depth | `+12616`…`+12632` | `+12304`…`+12320` | `+12536`…`+12552` |
| Current vertex buffers, textures | `+12636 + 4·s`, — | `+12324 + 4·s`, `+12408 + 4·i` | `+12556 + 8·s` ({offset, buffer}), `+12704 + 4·i` |
| Viewport, scissor | | `+12512`, `+12540` | `+12808` (x, y, w, h as integers), `+12832` |
| Current pixel shader, vertex shader | `+12868`, `+12872` | `+12556`, `+12560` | `+12944`, `+12948` |
| Predication mask | `+12884` | `+12572` | `+12960` |
| Targets saved by BeginTiling | `+12904`…`+12920` | `+12592`…`+12608` | `+12972`…`+12992` |
| Tile count, tile rects | `+12924`, `+12928` | `+12612`, `+12616` | `+12996`, `+13000` |
| Command buffer pointer saved by BeginVertices | `+13652` | `+13300` | `+13652` |
| D3DRS_PRESENTINTERVAL | | `+13444` | `+13788` |
| Vblank count; last flip vblank; swap counters | | `+16396`; `+16404`; `+16408`/`+16416` | |
| Refresh rate (from VdQueryVideoMode) | | `+21400` | |
| GPU wait time counters | | `+21440`, `+21448` | |

**Between XDK versions** (POTCAWE 5426 against Dante's Inferno): everything up to the end of the register
shadow is 128 bytes lower in POTCAWE. The block of current objects (targets, shaders, predication, tiling)
is 312 bytes lower. Other fields move by other amounts (BeginVertices' pointer: −352). So a new version
inserts or removes a few blocks: start from the nearest known layout and confirm each field in the code
that uses it.

**XDK 2417 against 5426** (GRAW against POTCAWE): the command buffer fields come first (`+0`/`+4`/`+8`), then
five dirty masks instead of six, in another order (the fetch slots share a mask with the bool/loop constants
and the `0x2000` group). The constants are where Dante's Inferno has them; the register shadow is 1216
bytes higher than POTCAWE's, with the same internal layout. A profile names each mask NativeXRenderer reads
(`DeviceLayout`): the VS and PS constant masks, the fetch mask and the bit of slot 0.

**XDK 2099 against 2417** (Kameo against GRAW, 2026-10-18): the same offsets up to `+12964` (the command buffer
fields, the five masks with the same bits, the constants, the register shadow, the current objects, viewport,
scissor, shaders, predication). The tiling block is smaller: BeginTiling saves only a reference surface (`+12968`),
not RT0–3 and the depth; the tile count is at `+12972`, the rects at `+12976` (−24). Everything after it is 32 bytes
lower (BeginVertices' saved end `+13620`, the present interval `+13756`, the segment start `+13996`). The resource
objects are the same as 2417's. Evidence in KameoRecomp's `docs/native-x-renderer-kameo.md`.

**The resource objects** change too (NativeXRenderer's `ObjectLayout`):

| Object field | POTCAWE (XDK 5426) | GRAW (XDK 2417) |
|---|---|---|
| Texture: its fetch constant (6 dwords) | `+28` | `+16` |
| Surface: RB_SURFACE_INFO, RB_COLOR_INFO / RB_DEPTH_INFO | `+24`, `+28` | `+48`, `+52` |
| Surface: size (width − 1, height − 1) | `+36`, bits 18–31 and 3–17 | `+24`, bits 0–12 and 13–25 (a 2D fetch dword 2) |
| Vertex / index buffer: base address, size | `+24`, `+28` | `+12`, `+16` |
| Resource fence (stamped when unbound) | `+8` | `+4` |
| Query: tile count | `+148` | `+144` |

**Behavior worth knowing:**
- **The write pointer holds the last dword written**, not the next free one: the library writes packets
  with `stwu` (pre-increment). A call's packets are `(pointer before, pointer after]`. (POTCAWE.)
- **Nothing reaches the command buffer until a draw, clear or resolve.** The flush writes the dirty groups
  at that point. (Dante's Inferno, POTCAWE.)
- **Dirty bit order.** Float constant group g (4 vec4) is bit `63 − g` of its mask; fetch slot s is bit
  `31 − s` of the low half of the fetch mask (the flush shifts it up and walks it with `cntlzd`).
  (POTCAWE.) In GRAW the fetch slots are the high half of the `+32` mask: slot s is bit `63 − s`.
- **Engine constant buffers bypass the shadow** in Dante's Inferno (`LoadShaderConstantsFromBuffer`).
  POTCAWE does not use them: its only `LOAD_ALU_CONSTANT` is the program flush's.
- **Surface objects are reused** for different surfaces. Re-read the surface header at every bind instead of
  caching it per object. (Dante's Inferno.)

## Rebuilding a draw's GPU state

The rules that rebuild the full GPU state of a D3D draw, as checked by NativeXRenderer's observer against the
SDK command processor's register file: **100 % of the draws** of POTCAWE's title screen, main menu and
Port Royal dock, with and without predicated tiling (`local_docs/04-observer.md`), and of GRAW's training
mission (3888 of 3888 draws over three frames, with its tiled scene; D59). Hell's Gate reached the same with
Dante's Inferno with rules 1–5 in their Dante form.

1. **Render state** is the register shadow **after the draw call returns**. The flush adjusts a few
   registers in the shadow itself (`RB_MODECONTROL`, `RB_HIZCONTROL`, `RB_COLOR_INFO` from the bound
   shaders and targets), so a snapshot before the call is wrong for them.
2. **Registers written outside the shadow**, straight into the command buffer: `SQ_PROGRAM_CNTL`,
   `SQ_CONTEXT_MISC`, `SQ_INTERPOLATOR_CNTL` (the program flush, from the shader objects) and
   `VGT_INDX_OFFSET` (DrawIndexedVertices' base vertex). Their value is the last one written in the
   command buffer, by this call or an earlier one. In XDK 2417 they are in the shadow and flushed with
   their group, but ClearF and SetShaderGPRAllocation's draws still write `SQ_PROGRAM_CNTL` straight into
   the command buffer: the same rule covers both.
3. **Fetch constants** are the shadow, except what packets wrote outside the flush:
   - the inline draws' stream (slot 31, vertex fetch 95, pointing into the command buffer);
   - the library's own clears and resolves (slots 0–3).

   Such a value stays until the slot is flushed from the shadow again (its dirty bit), or until a draw's
   own packets write it with the shadow's value. That second case is XDK 2417's: SetStreamSource only
   records the stream, and the flush writes the stream's vertex fetch constant into the shadow and sets
   its dirty bit during the draw, after the masks were read.
4. **Float constants** need a model of the GPU's constant file, because shader **literal constants**
   persist:
   - a group that is dirty when the draw call starts takes the shadow's values;
   - the program flush then loads the bound shaders' literal constants with `LOAD_ALU_CONSTANT` (POTCAWE:
     PS c251–c255, VS c254–c255). They stay in the GPU until a later flush or literal overwrites them;
   - the library's own draws (Present's copy) write constants too.

   One warm-up frame is enough to know every literal in use. In XDK 2417 there is no `LOAD_ALU_CONSTANT`
   at all: SetVertexShader and SetPixelShader copy the literals into the shadow and dirty their groups.
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
   segment as soon as the library leaves it.
10. **Every segment change.** A call's packets can continue in another command buffer segment anywhere in
    memory. The library moves there in its "kick" (end the segment, start a new one) and also in its
    reservation, which starts yet another segment when what it reserves does not fit after the kick. A
    reader that follows only the kick reads stale memory as packets, or loses the call's packets. The
    symptoms are draws that keep the previous call's programs, and inline vertex shaders that look
    "overwritten" (D20, D21, corrected by D26). A segment change inside another one (the kick calling the
    new-segment function) reads nothing more: in XDK 2417 what lies between is the kick's fence and a
    submission record past the submitted range, not the call's packets (D59).
11. **Inline draws.** BeginVertices (and XDK 2417's BeginIndexedVertices) runs the flush, which commits its
    packets as any call does, then writes the vertex fetch constant and the draw packet after the write
    pointer without moving it, and returns pointers into memory the game fills. EndVertices moves the
    write pointer past them. The draw is EndVertices, with the dirty masks the begin's flush saw. The
    vertex and index data come from the command buffer's allocator, not from the packet stream. POTCAWE
    calls BeginVertices only from DrawVerticesUP, which inlines the end; GRAW calls the pair itself.

Rules 2–4 and 8 read the packets the D3D call itself wrote, right after it wrote them. That is a few
hundred bytes per draw, and no emulation: only register writes, constant loads, shader loads, indirect
buffers and draw packets are followed.

**Most calls write no packets.** In GRAW (XDK 2417) a frame of a mission has about 1,800 draws and 17,500
setters, and over 600 frames the setters never wrote a packet: SetVertexShader, SetPixelShader,
SetVertexDeclaration, SetStreamSource, SetIndices, SetTexture, SetVertexShaderConstantF,
SetPixelShaderConstantF and SetDepthStencilSurface write only the shadow and its dirty masks; SetRenderTarget,
SetViewport, SetPredication, Clear, Resolve, the tiling calls, queries and Present do write packets. A draw's
own packets are mostly its flush of the dirty constants: of about 200 register writes, 147 float constants and
18 bool and loop constants, which the shadow holds too (D72).

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

- **Command buffer.** Segments allocated in physical memory (POTCAWE: around `0x0769xxxx`–`0x07ABxxxx`),
  written through `device+48`; `+52` is the segment's end and `+56` the threshold. When the pointer passes
  the threshold, `sub_822AEC50` ends the segment and starts a new one: 8–17 times per frame, always from
  inside another call. The new segment comes from `sub_822AE950(device, dwords)`, which sets `+48/+52/+56`.
  The reservation `sub_822AEE50(device, dwords)` calls `sub_822AEC50` when the reservation does not fit,
  and `sub_822AE950` directly if it still does not fit; Present calls `sub_822AE950` too. The caller writes
  its packets with `stwu` from `[+48]` and stores the new pointer at the end. A segment ends with
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
- **XDK 2417 (GRAW).** The fields are `+0` (write pointer), `+4` (segment end), `+8` (threshold).
  - **The kick** (`sub_824D7E80`) writes the segment's fence after the write pointer, submits the
    segment, and continues in the same memory with the pointer aligned up to 32 bytes, leaving a gap. Only
    past the threshold does it call the new-segment function (`sub_824D7C08`). The flush helpers call the
    kick themselves when a constant block does not fit, after writing what fits.
  - **Side allocations.** The command buffer's allocator also gives out blocks that are not packets:
    inline draws' vertices and indices, the tiling header and extents records, the data of clears and
    resolves. Present asks the new-segment function for 0x11208 dwords (about 280 KB) and starts writing
    after them.
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
- **Display gamma ramp.** The display controller maps every front buffer pixel through a ramp the library
  writes into its DC_LUT registers.
  - **Two ramps.** A 256-entry table (10 bits per channel) and a piecewise linear one (128 entries per
    channel, base and delta).
  - **Which one is used.** According to the SDK, `D3D::InitializePresentationParameters` sets up the table
    for 8_8_8_8 front buffers and the PWL ramp for 2_10_10_10 ones, and the SDK's presenters choose by the
    front buffer's format.
  - **In POTCAWE.** The front buffer is `2_10_10_10_AS_16_16_16_16`. The library writes the PWL once, early
    on: 384 register writes, 128 entries × 3 channels. The ramp darkens the low levels: 32 of 255 gives
    67 of 1023, where a straight line gives 129.
  - **For a renderer that presents itself.** It has to apply the ramp, after the frame and before any
    scaling; NativeXRenderer does (D36).

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
- **GRAW (XDK 2417)** renders its scene in three tiles of 1280×256 (rows 0–256, 256–512, 512–720) with 4×
  MSAA into a `2_10_10_10_FLOAT` target, once per frame, and resolves each tile itself as POTCAWE does
  (SetPredication `0x3`, `0xC`, `0x30`, then a depth and a color resolve per tile). The library records the
  pass differently: inside the bracket the kick appends each chunk's address to a tiling header instead of
  submitting it, and EndTiling submits the replay with a GPU interrupt callback. Draws into the tiling
  targets are wrapped in SET_BIN_MASK and an `EVENT_WRITE_EXT` that records their screen extents into
  `0xF00D` records. The CPU still issues each draw once (observer: 746 of 1276 draw calls executed three
  times). Its tile resolves pass the tile's rectangle and a destination point at the tile's corner
  (`x1`, `y1`), into textures of the whole image's size (1280×720).
- **The surfaces are one tile's size.** A tiled pass's surfaces hold one tile (GRAW: 1280×256, POTCAWE:
  1280×416), and the game's viewport covers the whole image; the window offset moves each tile into the
  surface. A renderer that draws the pass once needs targets as large as the tiles together (D60).

## Threads

- The D3D device is used from one thread at a time (the render thread in POTCAWE and GRAW), but resource
  functions are not tied to it: GRAW locks and unlocks vertex and index buffers from another thread
  (`sub_821A1000`) while the render thread is inside a device call. Their unlock flushes the CPU cache over
  the range (the hooked cache flush) on that thread too (D61).

## Occlusion queries

Seen in POTCAWE (XDK 5426), from POTCAWERecomp's `local_docs/06-sun-glow.md` and NativeXRenderer's D32.

- **Issue.** Issue (`sub_822BA590`) reaches `sub_822C4848`, which emits EVENT_WRITE_ZPD after setting
  RB_SAMPLE_COUNT_ADDR (0x2325):
  - BEGIN writes the address as `block + 32`;
  - END writes it as `block + 0`.
- **Arguments.** `Issue(query, flags)`: `D3DISSUE_END` = 1, `D3DISSUE_BEGIN` = 2. The first argument is
  the query object, not the device, but the packets go into the device's command buffer like any device
  call's. A hook that reads them needs the device from an earlier call.
- **Blocks.** A query owns one 64-byte block per tile: the end counts at `+0`, the begin counts at `+32`.
  The blocks of one query, and the queries, follow each other. In POTCAWE with one tile the dock's two
  queries use `0x06F2D000` and `0x06F2D040`. With two tiles the first query uses `0x06F2D000` and
  `0x06F2D040`, the second `0x06F2D080` and `0x06F2D0C0`.
- **Packets.** With one tile, Issue writes RB_SAMPLE_COUNT_ADDR in a normal SET_CONSTANT. In POTCAWE it
  writes RB_MODE_CONTROL, RB_SURFACE_INFO and RB_COLOR_INFO beside it. With several tiles each tile's
  address is in a bin-predicated SET_CONSTANT.
- **The marker.** Before END the library writes the marker `0xFFFFFEED` into ZPass_A/ZPass_B (`+16`/`+20`)
  of the **last tile's** block only.
- **Tiles.** With several tiles (`query+148` > 1), each tile's pass uses its own block, through
  bin-predicated SET_CONSTANTs.
- **GetData** (`sub_822BA818`) sums `ZPass_A + ZPass_B` of each block's end struct, minus the begin struct
  (`+48`/`+52`). The result is pending while the last block's ZPass_A and ZPass_B both still hold the
  marker.
- **For a renderer that draws instead of the GPU** (D32):
  - measure the draws between BEGIN and END with one host query, even across tiles;
  - write the count into the last tile's end struct (ZPass_A, with ZPass_B = 0, as one store, written
    last), and 0 into the other blocks;
  - leave the begin structs at 0.
  GetData then returns the count. The game reads its result in a later frame, so the count may be a frame
  or two old.

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
height − 1 in 13–25, slices − 1 in 26–31). (Phase 2.) Flags `0x100` and `0x200` clear the resolved color target
and the depth-stencil surface after the copy, over the copied rectangle: `clearColor` is a `D3DVECTOR4` (`r10`), Z
is in `f1`, the stencil argument is on the stack and the call writes it into `RB_DEPTH_CLEAR` (bits 0–7, the depth
in 8–31) together with `RB_COPY_CONTROL`'s clear enables. A game can render several pictures one after another into
one target this way, each resolved into its own texture and cleared for the next (Kameo's dragon billboards, D80;
GRAW's tiles). (Kameo, XDK 2099.)

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
- **Depth-stencil resolves read as color.** GRAW resolves its depth-stencil surface into a `k_24_8_FLOAT`
  texture and reads the same memory as `k_8_8_8_8` to get the stencil (R; G, B, A are the depth's bytes, low
  first): the 32-bit word holds the depth in bits 8–31 and the stencil in 0–7. A renderer that keeps resolves
  as host textures per format must give such a fetch the words (D64).
- **Resolve destinations are reused.** A game frees the memory it resolved into and uses it for textures it
  fills with the CPU. GRAW: a DXT3 texture later occupies a 64×64 depth resolve's memory, and the Cross-Com
  video's block (its Bink planes, written by the CPU, and its 160×120 resolve) is freed when the video ends
  and allocated again for the next. A renderer that keeps resolves as host textures by address must drop them
  when their memory is allocated again or written (D70).
- **Register LODs far past the mip range.** GRAW's ambient light sets a texture LOD of 150 (`setTexLOD`) to
  read a cube map's smallest mip, on fetches that also use the computed LOD. A translation that turns the
  LOD into a scale of the gradients (2^lod) overflows; the SDK's translator clamps it and samples cube maps
  at a LOD it computes (D63).

## Finding the entry points in a new executable

Work from the cheapest method to the most expensive. Record each finding with its evidence in the game's
NativeXRenderer document. For POTCAWE, steps 2–4 with the tools in [`../tools/`](../tools/README.md), then the
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
8. **Confirm at run time with the trace** (`--native_x_renderer_trace`, F9): call counts per frame, argument
   values, the order of calls in a pass. Then the observer (`--native_x_renderer_verify`, F10) proves the
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
