# NativeXRenderer

<p align="center">
  <a href="https://discord.gg/XQtM5TmNs8"><img alt="Discord: join the server" src="https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?style=for-the-badge&logo=discord&logoColor=white"></a>
</p>

A native renderer for Xbox 360 games statically recompiled with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

ReXGlue normally emulates the Xbox 360 GPU (Xenos): it executes the PM4 command stream that the game's
Direct3D library writes, and emulates the 10 MB of EDRAM, its resolves and its tiling on the host GPU.
NativeXRenderer works one level higher. Every Xbox 360 game links Microsoft's XDK Direct3D library into its
executable. NativeXRenderer hooks that library's entry points (draws, clears, resolves, swap…) and draws each
call directly on the host GPU through [DiligentCore](https://github.com/DiligentGraphics/DiligentCore)
(Vulkan). The game's code stays unchanged, and the SDK's Xenos emulation remains available as the fallback.

> [!NOTE]
> **Status, 2026-10-15: POTCAWE's default renderer (D56).** Phase 4 (POTCAWE polish) is done, and
> Phase 5 (temporal techniques) is in progress.
> - **What it draws:** the game on DiligentCore (Vulkan), with the game's textures, occlusion queries and
>   display gamma.
> - **Beyond the console:** any resolution and aspect ratio, SMAA, FXAA or NVIDIA DLAA, and a persistent
>   pipeline cache.
> - **Cost:** 38–56 % less of the GPU than the Xenos path at the same resolution.
> - **Long sessions:** an hour at 60 FPS, with the memory in use bounded.
> - **The Xenos path** stays as the alternative and the fallback.
>
> The game's D3D layer is mapped, and the observer rebuilds the GPU state of 100 % of the draws from the D3D
> device. The first game is Pirates of the Caribbean: At World's End
> ([PiratesAtWorldsEndRecomp](https://github.com/ElGuslml/PiratesAtWorldsEndRecomp), which uses this
> repository as a submodule). The next planned games are Ghost Recon Advanced Warfighter (GRAWRecomp) and
> Kameo (KameoRecomp). The work plan is [`docs/plan.md`](docs/plan.md).

## Why

Emulating Xenos faithfully costs host GPU time that the game never asked for. Measured in POTCAWE at a
2×2 render scale on an RTX 4070 Ti ([`docs/plan.md`](docs/plan.md#baseline-to-beat)):

- About a third of the remaining frame is the emulation itself:
  - copies between host render targets that alias the same EDRAM;
  - resolves dumped into an EDRAM buffer and copied back to guest memory;
  - textures reloaded from those resolves.
- The game draws its scene twice per frame (two EDRAM tiles), because 1280×720 with 2× MSAA does not fit
  in 10 MB. On the PC that limit does not exist, but an emulator has to replay what the console did.
- The render resolution can only be a whole multiple of the game's (2×, 3×), because the EDRAM layout is
  emulated.

A renderer that sees the game's Direct3D calls instead of the GPU's commands has none of these costs:
- a render target is a host texture;
- a resolve is a reference to that texture;
- a tiled scene is drawn once;
- the resolution is whatever the window needs.

The estimate for POTCAWE at 2×2 is 20–35 % less GPU time per frame: the most with the original 2× MSAA,
the least with antialiasing off. See
[`docs/architecture.md`](docs/architecture.md#expected-gains) for how that number was derived.

## How it works

```
 game code (recompiled PPC)
   │  calls
   ▼
 XDK D3D entry point (sub_XXXXXXXX)  ── hooked with REX_HOOK_RAW
   │                                  │
   │ original body still runs         │ after it returns:
   ▼                                  ▼
 PM4 packets into the ring          NativeXRenderer reads the D3D device's state
   │                                (register shadow, constants, fetch constants,
   ▼                                 shaders, surfaces) and records a host draw
 SDK command processor in                │
 "sync-only" mode: fences,               ▼
 interrupts and queries,            DiligentCore (Vulkan): pipelines from the SDK's
 no host rendering                  SPIR-V translator, host textures and render
                                    targets, present
```

The game keeps writing its command buffer exactly as on the console. The GPU-side work that only exists
for synchronization still runs in the SDK: fences, vblank and swap callbacks, the waits of the D3D library.
Only the drawing moves to NativeXRenderer.

## Bring-up tools

Two library features, off by default and free when off, take a new game from "mapped" to "verified" before
any rendering. See [`docs/architecture.md`](docs/architecture.md#bring-up-tools-trace-and-observer).

| Feature | Turn on | Does |
|---|---|---|
| Trace | `--native_x_renderer_trace=true`; F9 dumps a frame | Counts the game's D3D calls per frame; dumps frames call by call with arguments and draw state. |
| Observer | `--native_x_renderer_verify=true`; F10 verifies a frame | Rebuilds every draw's GPU state from the D3D device and compares it with the SDK command processor's register file (the SDK's draw observer, change 26 of its `recomp` branch). |

## Layers

| Layer | Where it lives | Changes per… |
|---|---|---|
| **Core**: device, pipelines, shaders (SDK translator), buffers, textures, render targets, resolves, present | this repository | never; shared by all games |
| **XDK profile**: addresses of the D3D entry points (`NATIVE_X_RENDERER_HOOK`, [`include/native_x_renderer/hooks.h`](include/native_x_renderer/hooks.h)), layout of the D3D device structure and of the resource objects (`native_x_renderer::DeviceLayout`, `ObjectLayout`) | the game's repository (a hook file and a profile table) | executable, and XDK version |
| **Game integration**: engine-specific behavior (tiling, surface aliasing tricks, queries, video) | the game's repository | game / engine |

This repository contains no game-specific code or addresses, so that every game can share it.

## Using it in a game

- **Add this repository as a submodule**, for example at `native_x_renderer/`, with its own submodules:
  `git submodule update --init --recursive`. DiligentCore is one of them, in `thirdparty/diligent-core`.
- **In the game's CMake,** after the ReXGlue SDK: `add_subdirectory(native_x_renderer)`, link the
  `native_x_renderer` target, and call `native_x_renderer_copy_runtime(<game target>)`, which copies the
  DLLs the renderer needs next to the executable.
- **Use the SDK's [`recomp`](https://github.com/ElGuslml/rexglue-sdk/tree/recomp) branch** of ElGuslml/rexglue-sdk as the game's
  `thirdparty/rexglue-sdk` submodule: the official SDK's nightly of 2026-10-02 (after v0.10.0, D90) with the shared
  fixes and the changes this library needs (changes 26-33 in its `RECOMP.md`), one commit each. They were patches
  in `sdk_patches/` until D89.
  Its DiligentCore patches ([`diligent_patches/`](diligent_patches/)) are applied by this repository's CMake.
- **The rest is per game:** the hook file, the profile, the launcher row. See
  [`docs/integrating-a-game.md`](docs/integrating-a-game.md).

Requirements: a GPU with a Vulkan driver. NVIDIA DLAA needs an RTX GPU.

## Documents

| Document | Contents |
|---|---|
| [`docs/plan.md`](docs/plan.md) | Work plan: phases, tasks with checkboxes, exit criteria, the baseline to beat, test protocol, risks. |
| [`docs/architecture.md`](docs/architecture.md) | Design: data flow, components, threading, resolution model, fallback, the hard parts. |
| [`docs/xdk-d3d-reference.md`](docs/xdk-d3d-reference.md) | What is known about the XDK Direct3D library across games, and how to find its entry points in a new executable. |
| [`docs/integrating-a-game.md`](docs/integrating-a-game.md) | Checklist for adding NativeXRenderer to another ReXGlue game. |
| [`docs/decisions.md`](docs/decisions.md) | Decision log: what was decided, when and why. |
| [`docs/credits.md`](docs/credits.md) | Credits and licenses. |
| [`tools/README.md`](tools/README.md) | The analysis scripts for bringing up a game: index the generated code, find the PM4 builders, summarize functions, read trace dumps. |

The game-side document for POTCAWE (its D3D map, engine behavior and measurements) is
[`docs/native-x-renderer-potcawe.md`](https://github.com/ElGuslml/PiratesAtWorldsEndRecomp/blob/main/docs/native-x-renderer-potcawe.md)
in PiratesAtWorldsEndRecomp. Paths in these documents that are not in this repository (`src/`,
`launcher/`, `docs/native-x-renderer-potcawe.md`, `tools/`, `local_docs/`) are that repository's, or its
author's local notes.

## Credits

The approach of replacing the D3D layer of a recompiled Xbox 360 game comes from
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp). Its application on top of ReXGlue
with DiligentCore comes from [Hell's Gate](https://github.com/florinp93/hells-gate-recomp) (Dante's
Inferno), whose design documents shaped this one. No code was taken from either project. Full credits:
[`docs/credits.md`](docs/credits.md).

## License

MIT for NativeXRenderer's own code (see [`LICENSE`](LICENSE)). DiligentCore, NVIDIA DLSS, SMAA and the
parts of the ReXGlue SDK it compiles keep their own licenses: see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
