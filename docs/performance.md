# NativeXRenderer — performance: where the time goes and what could be done

Status: 2026-10-26, after D97, D98, D99 and D100. Written after Gustavo asked whether the renderers of his recompilations are limited
by ReXGlue's architecture, what D5's "ring removal" means, and whether the techniques of other recompilations
(Unleashed Recompiled's bindless textures and shader specialization, Hell's Gate's hybrid renderer) would help.
The measurements are GH Metallica's (GHMetalRecomp), the game with the highest frame rates so far: 120 to 240 FPS,
on a 3440×1440 ultrawide at 160 Hz. Update this document as the items below are done or measured again.

## Contents

1. [What NativeXRenderer replaced, and what it still takes from the SDK](#what-nativexrenderer-replaced-and-what-it-still-takes-from-the-sdk)
2. [CPU: the renderer thread](#cpu-the-renderer-thread)
3. [GPU: a profile at 3440×1440, 160 FPS](#gpu-a-profile-at-34401440-160-fps)
4. [The translated shaders](#the-translated-shaders)
5. [The ring (D5)](#the-ring-d5)
6. [Other recompilations](#other-recompilations)
7. [What to do next, in order](#what-to-do-next-in-order)
8. [How to measure](#how-to-measure)

## What NativeXRenderer replaced, and what it still takes from the SDK

The SDK's Xenos path emulates the GPU: the EDRAM, resolves through it, predicated tiling replayed pass by pass, and
the host resolution as a whole multiple of the game's ([architecture.md](architecture.md#the-problem)). Those were
the larger GPU costs, and NativeXRenderer does none of them: it draws the game's draws on host render targets at the
output's resolution, at any scale (D37, D38). That is why it takes about half the GPU of the Xenos path at 1440p and in
ultrawide (GHMetalRecomp's `local_docs/04`, the A/B series).

What it still takes from the SDK, and what each costs:

| Taken from the SDK | Costs | Where |
|---|---|---|
| The shader translator (Xenos microcode to SPIR-V) | GPU: the shaders emulate the Xenos (below) | [The translated shaders](#the-translated-shaders) |
| The shared memory (guest memory mirrored in a 512 MB buffer, write-watched) | CPU: protections and faults (D97 removed most); GPU: every upload is a copy that ends the render pass | [CPU](#cpu-the-renderer-thread), [GPU](#gpu-a-profile-at-34401440-160-fps) |
| The texture cache and its load shaders | Little per frame: loads only when guest data changes (D23, D35) | — |
| The primitive processor (index conversion) | Little | — |
| The command processor, in sync-only mode (D5) | CPU: a thread, the game's packets, the waits between it and the renderer thread; no GPU | [The ring](#the-ring-d5) |

None of these is a fixed limit of ReXGlue: NativeXRenderer is a library compiled into each game with the SDK's
graphics sources it needs (D15), and it already replaced the SDK's drawing. Each piece can be replaced in turn when a
measurement shows it is worth it.

## CPU: the renderer thread

GH Metallica, 1280×720 window, the career's first song (the bot), after D97: 2.9 ms of CPU per frame on the
renderer thread (4.8 before D97), 36 % of a core at 120 FPS. By function, of its busy time (D97's profile):

| Part | Share | Notes |
|---|---|---|
| Guest memory requests (`SharedMemory::RequestRanges`) | 43 % | Hashing 21 %, uploads 17 % (`UpdateBuffer`), VirtualProtect 1.5 % |
| The driver (vkCmd*, descriptor sets) | ~30 % | Draws, pipeline binds, descriptor sets |
| Render pass and barriers | ~18 % | `CommitRenderPassAndFramebuffer` 9.6 %, `FlushBarriers` 8.9 %: each upload ends the render pass |
| Textures (`RequestTextures`) | 4 % | After D82/D83 |

Done since: **the uploads in batches (D98).** A draw whose own data copied requests the data of the draws queued
behind it too, so their copies end the render pass once (4.8 batches a frame of about 100 draws each, in place of a
render pass ended at 35 % of the draws), and an unwatched page is hashed once per batch: 2.86 → 2.59 ms a frame at
120 FPS, the hashing 10.4 → 5.3 MB a frame.

Done since: **DiligentCore's `Present` (D99).** At 3440×1440 in a borderless window the renderer thread spent 2.1–3.5
ms of every frame in `vkAcquireNextImageKHR`, spinning in NVIDIA's driver until the display's next refresh freed an
image (not waiting for the GPU: acquired a frame later, it waited as long). DiligentCore patch `0007` acquires the
image when the back buffer is first needed and waits for it asleep: in GH Metallica's songs at 160 FPS the process
2.17 → 1.68 cores, the renderer thread 0.78 → 0.39 cores, the frame interval's worst per 600 frames 7.31 → 7.04 ms.

Done since: **a shader's float constants copied a run at a time (D100).** Kameo at Gustavo's spot (the meadow of the
Enchanted Kingdom, about 3,850 draws a frame, 120 FPS): 6.0 → 5.0 ms a frame on the renderer thread. What is left of a
draw there, by share of the thread's samples:

| Part | Share | What could lower it |
|---|---|---|
| DiligentCore's `SetPipelineState`, `DrawIndexed`, `CommitShaderResources` (the driver included) | 7.8, 6.8, 4.6 % | Fewer descriptor set writes: a cache of resource bindings per pipeline and textures, or bindless descriptors; the constant buffers' dynamic offsets bound in one call |
| The textures: views, requests, samplers | 6 % | Remember the last fetch constant per binding: a draw with the same textures skips the lookups |
| The register records applied (outside the draw) | 5 % | — |
| `FillSystemConstants` | 3 % | Filled only when the registers it reads changed |
| The vertex data's ranges, the primitive processor, the pipeline lookup | 2.2, 1.8, 1.3 % | The last pipeline's state compared before hashing |
| The timing of every command (two clock reads each) | 2 % | Timed per batch of commands |

What is left to try:
- **Hashing only what a draw reads.** An unwatched page (D97) is hashed whole, once per batch since D98 (5.3 MB a
  frame in GH Metallica), where a draw reads a few hundred bytes of it; comparing the requested bytes with a CPU copy
  of what was uploaded would read less.
- **Bindless descriptors.** One descriptor set of every texture and sampler, indexed by the shader, instead of a
  set per draw: less `CommitShaderResources` and descriptor allocation. Mostly CPU; it needs the shaders to index
  (a translator change).

## GPU: a profile at 3440×1440, 160 FPS

Gustavo's settings: NativeXRenderer, 3440×1440 borderless, ultrawide (Hor+), 160 FPS, SMAA. The renderer's own GPU
timer: 4.5–5.6 ms per frame in a song, of the 6.25 ms a frame lasts at 160 Hz (Gustavo saw up to 90 % GPU use at
some angles). Three RenderDoc captures (two in the song, one in the stage introduction with pyrotechnics), each
event timed with RenderDoc's GPU duration counter (`gpu_time_tsv.py`):

| Part | Song, highway | Song, drummer | Introduction, pyro |
|---|---|---|---|
| Scene draws at the output's resolution | 1.75 ms (40 %) | 1.62 ms (38 %) | 1.74 ms (36 %) |
| Full-screen passes (the game's post-processing, SMAA, the present) | 0.89 ms (20 %) | 0.88 ms (21 %) | 1.18 ms (24 %) |
| Copies (guest memory uploads) | 0.84 ms (19 %), 509 | 0.72 ms (17 %), 387 | 0.85 ms (18 %), 451 |
| Smaller targets (the game's quarter-resolution post-processing) | 0.52 ms | 0.60 ms | 0.65 ms |
| The shadow map (2752×2048) | 0.34 ms | 0.35 ms | 0.35 ms |
| Total | 4.41 ms | 4.23 ms | 4.84 ms |

What the profile said:
- **The uploads cost the GPU too:** 17–19 % of the frame in copies, and more in the draws: each copy ended the
  render pass, so the 3440×1440 targets were stored and loaded again about 200 times a frame. Batching them was the
  clearest GPU gain, and D98 did it (below).
- **The scene at the output's resolution is the largest part,** as in any game at 5 megapixels: the game's own draws
  through the translated shaders.
- **One full-screen pass of the game's costs 0.25–0.28 ms** (the most expensive event in all three captures).
- **The shadow map**, 2752×2048 on the host: 0.35 ms.

### After D98: the uploads in batches

The same settings and route, two captures: the draws interrupted by copies 223 / 193 → 9 / 6 a frame; every event's
GPU time 4.41 / 4.23 → 2.94 / 3.12 ms, the scene's draws 1.75 / 1.62 → 0.78 / 1.00 ms, the copies 0.84 / 0.72 → 0.52 /
0.43 ms. NativeXRenderer's frame timer: 4.5–5.6 → **2.7–2.9 ms a frame** of the 6.25 at 160 Hz.

### Would a SPIR-V optimizer help? Measured: no

In the song capture with the highway, every shader module the frame's draws use (169) was taken from RenderDoc,
optimized with SPIRV-Tools' `spirv-opt -O` (built from the SDK's `thirdparty/spirv-tools`), put back in place of
the original (`BuildTargetShader`, `ReplaceResource`) and the frame timed again:
- all 169 replaced, none failed; their SPIR-V 5.1 → 3.7 MB;
- the frame 4.47 / 4.64 ms before (two runs), 4.51 / 4.49 ms optimized; event by event the same to the microsecond
  (the most expensive, 0.252 ms before and after);
- the replacement itself checked: that event's pixel shader replaced by one that discards everything took
  0.014 ms, 0.252 again once restored.

The driver (NVIDIA, RTX 4070 Ti) already optimizes the translated SPIR-V as well as `spirv-opt` does: running an
optimizer over the translator's output would gain nothing here.

The most expensive event, a full-screen pass of the game's (12 texture samples): its SPIR-V has 16 switches and 29
conditional branches, nearly all of them the translator deciding at run time, per texture component, how to read
it (unsigned, signed, biased), from the fetch constant's bits, with both an unsigned and a signed view bound. They
are uniform across the draw, which GPUs branch on cheaply; what fixing them per pipeline (specialization) would gain
is not measured yet. It can be, the same way: one shader specialized by hand to the draw's fetch constants,
replaced in the capture and timed.

## The translated shaders

The SDK's translator (`spirv_translator*.cpp`) turns each Xenos shader into SPIR-V that emulates the Xenos
instruction by instruction. Read in its source:
- **Control flow:** every shader is a main loop, and one with jumps (labels) is a state machine, a switch on an
  emulated program counter, marked not to unroll or flatten; predicates, loop counters and the address register
  are variables.
- **Vertex fetch:** each vertex word is loaded from the 512 MB guest memory buffer, its endianness read at run time
  from the fetch constant, then unpacked by format in the shader; the hardware's vertex input does none of it.
- **Per-draw state** (alpha test, gamma, sign, the shader modifications' remaining switches) is read from system
  constants at run time and branched on, not fixed when the pipeline is created.
- **No SPIR-V optimizer runs:** the SDK's SPIRV-Tools context only validates. That costs nothing measurable: the
  driver's own compiler does as well ([measured](#would-a-spir-v-optimizer-help-measured-no)).

Unleashed Recompiled took the other road (XenosRecomp): each shader translated ahead of time to structured HLSL,
the per-draw state as specialization constants, so the driver's compiler sees straight-line code with the state
fixed.

## The ring (D5)

On the console the D3D library writes PM4 packets into a ring buffer that the GPU executes. With NativeXRenderer:
1. The game's D3D functions still run and write those packets.
2. The SDK's command processor executes the ring on its own thread in a sync-only mode: it draws nothing, but it
   emulates fences, the swap and vertical blank callbacks, the waits and queries.
3. NativeXRenderer draws from what its hooks capture at the D3D calls, on its own thread (D71); before every signal
   to the game the command processor waits until that thread has run what was queued (SDK patch 0006), so the game
   reuses no memory the renderer still reads.

"Removing the ring" means not writing nor executing those packets: NativeXRenderer would emulate the fences, swaps,
vertical blank and queries itself, as Unleashed does and as Hell's Gate plans for frame rates above 60.
- **It would save CPU and latency:** the command processor's thread, the packets the game's render thread writes,
  the waits between the command processor and the renderer thread, and one step between a frame and its
  presentation.
- **It would save no GPU time:** the ring never reaches the GPU.
- **Its cost:** each game's synchronization reimplemented and verified (the fences its D3D library waits on, its
  swap and vertical blank callbacks, its queries), and the Xenos path no longer a reference for those.
- **GH Metallica does not need it yet:** at 120 and 160 FPS its frames are even (8.33 and 6.27 ms on average,
  Gustavo's session), and the renderer thread and the GPU are below a frame.

Revisit D5 if a game's pacing above 60 FPS is uneven because of the command processor, or if its CPU (measure the
command processor's thread first) becomes the limit at 240 Hz.

## Other recompilations

- **Unleashed Recompiled** (Sonic Unleashed): no Xenos emulation at all, the renderer written for one game; shaders
  translated ahead of time (XenosRecomp) with specialization constants, bindless textures, pipelines precompiled.
  The techniques carry over; the per-game rewrite does not fit a library for several games.
- **Hell's Gate** (Dante's Inferno): the hybrid NativeXRenderer follows (D5, D7): the D3D layer replaced, the
  command processor kept for synchronization, its removal planned for frame rates above 60, and a further hybrid
  renderer in progress.

NativeXRenderer serves Kameo, GRAW, POTCAWE and GH Metallica: each technique is worth bringing in when a profile
shows its cost, as a decision with its measurement, so every game gains from it.

## What to do next, in order

1. ~~Batch the uploads~~: done, D98 (the GPU 4.5–5.6 → 2.7–2.9 ms a frame at 3440×1440 / 160 FPS).
2. ~~`Present` at high resolutions~~: done, D99 (the process 2.17 → 1.68 cores at 3440×1440 / 160 FPS).
3. **The translated shaders**, by measurement: a SPIR-V optimizer is ruled out (above); next, measure what
   specializing the per-draw state would gain on the most expensive shaders (one by hand, replaced in a capture),
   then decide on specialization constants in the translator and a direct vertex fetch where the format allows.
   One change there helps every game.
4. **Hash only what a draw reads** (CPU).
5. **Bindless descriptors** (CPU; needs the translator).
6. **The ring**, only if pacing or CPU at 240 Hz needs it.

## How to measure

- **CPU:** recomp-tools' `measure\threadcpu.ps1` and `stacksample.ps1`; with every stack kept (`-Top 5000`) the
  stacks can be grouped by function (inclusive shares of the busy samples), as for D97. The log's "renderer CPU"
  per frame is the time the thread spends running commands, waits inside them included (the swap chain's
  acquisition sleeps since D99 and still counts), not its CPU time. To compare variants, take the threads' CPU in
  short windows aligned with the log's times, so that phases of different load (a song, the failed-song screen) are
  not mixed (D99).
- **GPU:** the log's "GPU x ms average" (NativeXRenderer's frame timer); RenderDoc captures under recomp-tools'
  `Game.ps1 ... -RenderDoc`, then `tools\renderdoc\gpu_time_tsv.py` (every event's GPU duration with its target and
  shaders) and `gpu_time_compare.py`.
- **A shader change before writing it:** RenderDoc can replace a capture's shader modules (`BuildTargetShader`,
  `ReplaceResource`) and time the frame again, as the experiment above did with spirv-opt.
