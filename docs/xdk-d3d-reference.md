# The XDK Direct3D library — reference

What XenosNative needs to know about the Direct3D library that Microsoft's Xbox 360 XDK links into every
game, and how to find its pieces in a new executable. Facts carry their source: the game where they were
seen and the XDK version. Facts from Dante's Inferno come from Hell's Gate's
[`NATIVE_D3D_MAP.md`](https://github.com/florinp93/hells-gate-recomp/blob/main/docs/NATIVE_D3D_MAP.md)
(credited in [`credits.md`](credits.md)).

## Contents

1. [The library in a recompiled executable](#the-library-in-a-recompiled-executable)
2. [XDK versions of our games](#xdk-versions-of-our-games)
3. [Entry points](#entry-points)
4. [The device structure](#the-device-structure)
5. [Command buffer, waits and swap](#command-buffer-waits-and-swap)
6. [Predicated tiling](#predicated-tiling)
7. [Occlusion queries](#occlusion-queries)
8. [Surfaces and EDRAM](#surfaces-and-edram)
9. [Shaders, vertex declarations, textures](#shaders-vertex-declarations-textures)
10. [Finding the entry points in a new executable](#finding-the-entry-points-in-a-new-executable)
11. [PM4 and register cheat sheet](#pm4-and-register-cheat-sheet)

## The library in a recompiled executable

- It is linked statically. There are no exports and no symbols, and the retail build has almost no
  strings.
- The code sits in one contiguous range, with a few functions elsewhere:
  - **POTCAWE:** `0x822A0000`–`0x822CFFFF`.
  - **Dante's Inferno TU2:** `0x827C8000`–`0x827EFFFF`, about 600 functions, plus copies of the draw
    functions at `0x8291xxxx`.
- Many setters in the XDK headers are inline. The game writes the device's register shadow and sets dirty
  bits itself, without calling anything. A dirty-state flush (one function) then turns dirty groups into
  PM4 packets before a draw or a resolve.
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

The roughly 45 functions XenosNative hooks or needs to understand. Names follow the XDK API. "Dante" is the
address in Dante's Inferno TU2, from Hell's Gate's map. "POTCAWE" is filled in during Phase 1; see
[`../../docs/xenos-native-potcawe.md`](../../docs/xenos-native-potcawe.md) for the evidence of each.

| Group | Function | Role | Dante (TU2) | POTCAWE |
|---|---|---|---|---|
| Draw | DrawIndexedVertices | Indexed draw from the current index buffer | `8291D328` | |
| Draw | DrawVertices | Non-indexed draw | `8291CF38` | |
| Draw | BeginVertices / EndVertices | Inline draw: returns a pointer into the ring for the vertices | `8291CA58` / `8291CA48` | |
| Draw | DrawVerticesUP | User-pointer draw (Begin/End inlined) | `8291CEF0` | |
| Targets | SetRenderTarget, SetDepthStencilSurface | Bind surfaces (copies the surface header into RB_COLOR_INFO / RB_DEPTH_INFO) | `827D2278`, `827D25C8` | |
| Targets | Clear | Clear the bound targets | `827EDC18` | |
| Targets | Resolve | EDRAM → texture (flags: 0 = RT0, 4 = depth; bit 24 swaps red/blue) | `827E5B18` | |
| Targets | BeginTiling / EndTiling | Predicated tiling bracket | `827DCBF8` / `827DCF68` | |
| Targets | SetPredication | Tile predication control | `827DCA88` | |
| Shaders | SetVertexShader, SetPixelShader | Bind shader objects | `827E8018`, `827E7E58` | |
| Shaders | CreateVertexDeclaration, SetVertexDeclaration | 12-byte elements, `0xFF` terminator | `827E82F0`, `827E81E8` | |
| Shaders | Program flush | Patches the vertex shader for the declaration and writes the program (IM_LOAD) | `827EA4A0` | |
| Constants | SetVertexShaderConstantF, SetPixelShaderConstantF | Float constants into the shadow | `827E7B10`, `827E7BE8` | |
| Constants | LoadShaderConstantsFromBuffer | Engine constant buffers, bypassing the shadow | `8291D778` | |
| Resources | SetStreamSource, SetIndices | Vertex fetch constants (slot 95 − stream) and index buffer | `827D1A10`, `827D1B30` | |
| Resources | SetTexture, sampler setters, MIPMAPLODBIAS | Texture fetch constants and filter state | `827D92A0`, `827D0EC8`, `827D1070`, `827D1310`, `827D1430` | |
| Resources | Vertex buffer Lock / Unlock | Dynamic buffers filled by worker threads | `827D71A8` / `827D6268` (likely) | |
| Resources | Release | Resource release | `827D7128` (likely) | |
| Sync | InsertFence, BlockOnFence | GPU fences | `827D4010`, `827D3178` | |
| Sync | Command buffer reserve | Space in the ring; may start a new segment | `827D3F10`, `827D4148` | |
| Queries | D3DQuery Issue, GetData | Occlusion queries | Issue `827DAA98` | Issue `822BA590`, GetData `822BA818` |
| Frame | Swap (and Synchronize), Present | End of frame, VdSwap | Swap `827D4EE0` | Swap `822B58B0`, Present `822B5D58` |
| Frame | SetRenderState_PresentInterval | Writes `device+13444` (POTCAWE) | | `822B3B40` |
| Device | Direct3D_CreateDevice, device reset | Device creation | `827E2FC0`, reset `827D59D8` | reset `822B6338` |
| Surfaces | XGSetSurfaceHeader | Fills a surface header (EDRAM size, tiles) | | `822ABE40` |
| Render states | Cull, fill, blend, alpha ref, depth/stencil, poly offset, color mask, HiZ | Each identified by the register it writes; many are inlined | about 17 addresses | |

## The device structure

The D3D device object in guest memory is the source of truth XenosNative reads at draw time. Its layout
depends on the XDK version.

| Field | Dante's Inferno (TU2) | POTCAWE (XDK 5426) |
|---|---|---|
| Fetch constants (32 slots × 24 bytes) | `+1152` | to find |
| Vertex shader float constants (256 vec4) | `+1920` | to find |
| Pixel shader float constants (256 vec4) | `+6016` | to find |
| Dirty bits: one per group of 4 vec4; 64-bit dirty masks per register group | yes | to find |
| Bool and loop constants | `+10112` | to find |
| Ring write pointer | | `+10780` |
| Pointer to the swap/vblank block (`+4`: Synchronize wait flag; `+16`/`+20`: last flip) | | `+10772` |
| Write-back slots | | `+11196`, `+11200` |
| Current vertex declaration | `+11992` | to find |
| D3DRS_PRESENTIMMEDIATETHRESHOLD | | `+11716` |
| Sampler filter state | `+12100` | to find |
| Current render target surface | `+12616` | to find |
| D3DRS_PRESENTINTERVAL | | `+13444` |
| Vblank count; last flip vblank; swap counters (pending swaps waited on while `[+16408] − [+16416] ≥ 15`) | | `+16396`; `+16404`; `+16408`/`+16416` |
| Refresh rate (from VdQueryVideoMode) | | `+21400` |
| GPU wait time counters | | `+21440`, `+21448` |

**Behavior worth knowing (Dante's Inferno, from Hell's Gate):**
- **Nothing reaches the ring until a draw or a resolve.** The flush writes the dirty groups at that point.
  So the shadow at draw time is the GPU state of that draw.
- **Engine constant buffers bypass the shadow.** `LoadShaderConstantsFromBuffer` writes the GPU directly,
  so a constant's value is whichever source wrote last. Shader literal constants also persist in GPU
  registers until something overwrites them.
- **Surface objects are reused** for different surfaces. Re-read the surface header at every bind instead of
  caching it per object.

## Command buffer, waits and swap

Seen in POTCAWE (XDK 5426), from POTCAWERecomp's CPU usage and 60 FPS work.

- **Ring.** The ring write pointer is at `device+10780`. `sub_822AE600` waits for ring space.
  `sub_822AE6C8(device, ring, 0, callback, argument)` inserts a GPU callback packet.
- **Waits.** Six wait loops in the library share one pattern:
  - `sub_822AF458(device, kind)` records the start of the wait (device, kind, timestamp, CP read pointer).
  - `sub_822AF608` runs on every iteration that the condition is still false: a short `db16cyc` pause,
    then a "GPU hung" check that calls `sub_822B1ED8` after 5 seconds without progress.
  - The loops: ring space `sub_822AE600`, ring range `sub_822ADA00`, fence `sub_822AD968`, pending swaps
    `sub_822B5D58`, write-back slots `sub_822C3388` and `sub_822C3628`.

  On a PC those loops spin a core. POTCAWERecomp hooks the two helpers so they sleep (`src/gpu_wait.cpp`).
- **Swap.** `sub_822B58B0(device, frontbuffer)` is Swap; `frontbuffer = 0` is Synchronize, which has an alias
  at `sub_822B5D50`. It:
  1. maps the present interval at `device+13444` (0/1 → 1, 2 → 2, 4 → 3, `0x80000000` → 0);
  2. builds the descriptor `frontbuffer | interval << 8 | threshold`;
  3. inserts a GPU callback to `sub_822B5718`, which flips at vblank.

  Synchronize also writes a WAIT_REG_MEM so that the GPU waits until the callback clears
  `[device+10772]+4`.
- **Present.** `sub_822B5D58` is Present: it calls `VdSwap`, then Swap. The game's present wrapper is
  `sub_823B1CB8`: Synchronize, two GPU timing callbacks, then Present.

## Predicated tiling

Seen in POTCAWE (XDK 5426).

- **The tiles.** The engine computes its tile rectangles (two vertical tiles of 416 and 368 rows at
  1280×720 with 2× MSAA) and renders the scene between BeginTiling and EndTiling.
- **The replay.** The library replays the command buffer once per tile under SET_BIN_MASK / SET_BIN_SELECT.
  Per-tile values are written with bin-predicated SET_CONSTANT packets (`0xC0012D01`, register 0x2325 for
  the query address).
- **The CPU side.** The CPU issues each draw once per frame. The emulator sees the 962 scene draws twice
  only because of the replay. To confirm in Phase 1 for every draw kind.
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
  (`sub_824D5750`), with the MSAA type as `r6`.
- **Aliasing.** Surfaces with different pitch or MSAA can alias the same EDRAM. POTCAWE clears its 2× MSAA
  scene at pitch 1280 through a 4× MSAA surface at pitch 640, which has the same tile layout.

## Shaders, vertex declarations, textures

Seen in Dante's Inferno, from Hell's Gate. Verify per XDK version.

- **Shader objects.** The pixel shader's microcode is at `+0x18` of its object, the vertex shader's at
  `+0x20`.
- **Vertex shader patching.** The vertex shader is patched per vertex declaration, and the flush binds the
  patched variant. Read the program the flush actually loads (the last IM_LOAD), not the object's default.
- **Textures.** A texture object holds its 6-dword fetch constant at `+0x1C`. The sampler setters patch
  dwords of that fetch constant.
- **Vertex buffers.** A vertex buffer object holds its fetch constant at `+0x18` and its CPU address at
  `+0x20`. SetStreamSource writes vertex fetch constant `95 − stream`.

## Finding the entry points in a new executable

Work from the cheapest method to the most expensive. Record each finding with its evidence in the game's
XenosNative document.

1. **Same XDK version as a mapped game?** Then the library's code is probably identical. Compare the bytes
   of a known function, or a signature of its first instructions, against the new image. Every match is a
   free mapping. Verify the device offsets anyway.
2. **Anchor functions** have unique behavior that is easy to spot in the generated C++. The generated C++
   keeps each PPC instruction as an `// asm` comment, and `loc_XXXXXXXX:` labels give addresses.
   - Swap and Present: Present calls the `VdSwap` import, and Swap reads the present interval.
   - The wait helpers: `db16cyc` loops, and reads of the `KeTimeStampBundle` milliseconds.
   - Query Issue: builds EVENT_WRITE_ZPD.
   - XGSetSurfaceHeader: the `+79`/`+15` tile math.
   - The callback insert: called with a function address as an argument.
3. **Call-site census.** Count the `bl` calls into the library's range from the engine's code. Setters are
   called from hundreds of sites. Draws are called from a few submission functions. Swap is called once per
   frame from the engine's present.
4. **PM4 signatures.** Type-3 packet headers appear as immediates:
   `0xC0000000 | (dword_count − 1) << 16 | opcode << 8`. A function that builds a DRAW_INDX (0x22) or
   DRAW_INDX_2 (0x36) header is a draw. EVENT_WRITE (0x46) with the resolve registers is a resolve. See the
   [cheat sheet](#pm4-and-register-cheat-sheet).
5. **Register IDs.** Type-0 writes (`(count − 1) << 16 | register`) name the register a setter writes. The
   RB_COPY_* registers (0x2318–0x231E) are written by the resolve, and RB_COLOR_CLEAR and RB_DEPTH_CLEAR by
   the clear.
6. **Write-pointer attribution** (Hell's Gate's method). Hook a candidate, read the ring write pointer before
   and after the original runs, and disassemble the packets it wrote with the SDK's packet disassembler.
   This turns "probably a resolve" into a fact.
7. **Engine wrappers.** Most engines wrap D3D in their own renderer (POTCAWE's renderer is around
   `0x823Fxxxx`). The wrapper's purpose is often clear from its arguments (surface sizes, MSAA levels), and
   it names the D3D function it calls.

## PM4 and register cheat sheet

From the SDK's `include/rex/graphics/xenos.h` and `register_table.inc`.

| Type-3 opcode | Value | Meaning |
|---|---|---|
| DRAW_INDX | `0x22` | Draw with an index buffer fetched from memory |
| DRAW_INDX_2 | `0x36` | Draw with indices in the packet |
| IM_LOAD / IM_LOAD_IMMEDIATE | `0x27` / `0x2B` | Load shader microcode |
| SET_CONSTANT / SET_CONSTANT2 | `0x2D` / `0x55` | Constants and registers |
| LOAD_ALU_CONSTANT | `0x2F` | Constants from memory (engine constant buffers) |
| SET_SHADER_CONSTANTS | `0x56` | Incremental constant update |
| INVALIDATE_STATE | `0x3B` | Selective state invalidation |
| WAIT_REG_MEM | `0x3C` | GPU waits on a register or memory value |
| MEM_WRITE | `0x3D` | Write words to memory |
| INDIRECT_BUFFER | `0x3F` | Call a command buffer segment |
| EVENT_WRITE / _SHD / _EXT / _ZPD | `0x46` / `0x58` / `0x5A` / `0x5B` | Events (resolve, done, extents, occlusion) |
| SET_BIN_MASK / SET_BIN_SELECT | `0x50` / `0x51` (`0x60`–`0x63` LO/HI) | Predicated tiling |
| INTERRUPT | `0x54` | CPU interrupt (swap and vblank callbacks) |

| Register | ID | Meaning |
|---|---|---|
| RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO | `0x2000`, `0x2001`, `0x2002` | Surface pitch/MSAA, color and depth EDRAM base and format |
| RB_COPY_CONTROL, RB_COPY_DEST_BASE … | `0x2318`, `0x2319` … | Resolve setup |
| RB_DEPTH_CLEAR, RB_COLOR_CLEAR | `0x231D`, `0x231E` | Clear values |
| RB_SAMPLE_COUNT_ADDR | `0x2325` | Occlusion query result address |
