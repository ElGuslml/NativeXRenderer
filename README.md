# XenosNative

A native renderer for Xbox 360 games statically recompiled with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

ReXGlue normally emulates the Xbox 360 GPU (Xenos): it executes the PM4 command stream that the game's
Direct3D library writes, and emulates the 10 MB of EDRAM, its resolves and its tiling on the host GPU.
XenosNative works one level higher. Every Xbox 360 game links Microsoft's XDK Direct3D library into its
executable. XenosNative hooks that library's entry points (draws, clears, resolves, swap…) and draws each
call directly on the host GPU through [DiligentCore](https://github.com/DiligentGraphics/DiligentCore)
(Vulkan). The game's code stays unchanged, and the SDK's Xenos emulation remains available as the fallback.

> [!NOTE]
> **Status: Phase 2 in progress, 2026-10-13.** XenosNative draws the game's frames on DiligentCore
> (Vulkan), still with placeholder textures. The game's D3D layer is mapped, and the observer rebuilds the
> GPU state of 100 % of the draws from the D3D device. The first game is Pirates of
> the Caribbean: At World's End (`POTCAWERecompVulkan`, this repository). The next planned games are Ghost
> Recon Advanced Warfighter (GRAWRecomp) and Kameo (KameoRecomp). The work plan is
> [`docs/plan.md`](docs/plan.md).

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
 PM4 packets into the ring          XenosNative reads the D3D device's state
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
Only the drawing moves to XenosNative.

## Bring-up tools

Two library features, off by default and free when off, take a new game from "mapped" to "verified" before
any rendering. See [`docs/architecture.md`](docs/architecture.md#bring-up-tools-trace-and-observer).

| Feature | Turn on | Does |
|---|---|---|
| Trace | `--xenos_native_trace=true`; F9 dumps a frame | Counts the game's D3D calls per frame; dumps frames call by call with arguments and draw state. |
| Observer | `--xenos_native_verify=true`; F10 verifies a frame | Rebuilds every draw's GPU state from the D3D device and compares it with the SDK command processor's register file (SDK patch `sdk_patches/0001`). |

## Layers

| Layer | Where it lives | Changes per… |
|---|---|---|
| **Core**: device, pipelines, shaders (SDK translator), buffers, textures, render targets, resolves, present | this folder (`xenos_native/`) | never; shared by all games |
| **XDK profile**: addresses of the D3D entry points (`XENOS_NATIVE_HOOK`, [`include/xenos_native/hooks.h`](include/xenos_native/hooks.h)), layout of the D3D device structure (`xenos_native::DeviceLayout`) | the game's repository (a hook file and a profile table) | executable, and XDK version |
| **Game integration**: engine-specific behavior (tiling, surface aliasing tricks, queries, video) | the game's repository | game / engine |

This folder must not contain game-specific code or addresses. That rule is what makes it possible to
move it into its own repository with `git subtree split --prefix=xenos_native` once a second game uses it.

## Documents

| Document | Contents |
|---|---|
| [`docs/plan.md`](docs/plan.md) | Work plan: phases, tasks with checkboxes, exit criteria, the baseline to beat, test protocol, risks. |
| [`docs/architecture.md`](docs/architecture.md) | Design: data flow, components, threading, resolution model, fallback, the hard parts. |
| [`docs/xdk-d3d-reference.md`](docs/xdk-d3d-reference.md) | What is known about the XDK Direct3D library across games, and how to find its entry points in a new executable. |
| [`docs/integrating-a-game.md`](docs/integrating-a-game.md) | Checklist for adding XenosNative to another ReXGlue game. |
| [`docs/decisions.md`](docs/decisions.md) | Decision log: what was decided, when and why. |
| [`docs/credits.md`](docs/credits.md) | Credits and licenses. |
| [`tools/README.md`](tools/README.md) | The analysis scripts for bringing up a game: index the generated code, find the PM4 builders, summarize functions, read trace dumps. |

The game-side document for POTCAWE (its D3D map, engine behavior and measurements) is
[`../docs/xenos-native-potcawe.md`](../docs/xenos-native-potcawe.md).

## Credits

The approach of replacing the D3D layer of a recompiled Xbox 360 game comes from
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp). Its application on top of ReXGlue
with DiligentCore comes from [Hell's Gate](https://github.com/florinp93/hells-gate-recomp) (Dante's
Inferno), whose design documents shaped this one. No code was taken from either project. Full credits
and licenses: [`docs/credits.md`](docs/credits.md).
