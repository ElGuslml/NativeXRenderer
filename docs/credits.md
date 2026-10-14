# Credits and licenses

## Projects this design builds on

- **[Hell's Gate](https://github.com/florinp93/hells-gate-recomp)** by florinp93: a PC port of Dante's
  Inferno on the ReXGlue SDK. Its native renderer replaces the game's Direct3D layer with an
  implementation on DiligentCore (Vulkan), while keeping the SDK's Xenos path as a fallback. XenosNative
  follows the same architecture. Hell's Gate's design documents (`docs/NATIVE_RENDERER_DESIGN.md` and
  `docs/NATIVE_D3D_MAP.md`, read on 2026-10-13 at commit `f43bb48`) were the source of these ideas:
  - hooking the XDK D3D entry points and running the original body;
  - rebuilding draw state from the device's register shadow at draw time;
  - keeping the command processor as a sync engine;
  - render targets keyed by the guest surface, and resolves as host textures registered at the
    destination address;
  - the milestone method: a map of the D3D layer, then an observer at 100 %, then rendering;
  - the DiligentCore integration problems and their fixes;
  - the layout facts about Dante's Inferno's D3D library quoted in
    [`xdk-d3d-reference.md`](xdk-d3d-reference.md).

  The repository has no license, so **no code was taken from it**. Thank you.
- **[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp)** by hedge-dev: the first
  recompiled Xbox 360 game to replace the D3D layer with a native renderer. Hell's Gate's design names it
  as its model.
- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** by Tom Clay and contributors: the recompiler
  and runtime this project runs on, including the Xenos shader translator and the texture loaders
  XenosNative reuses. Its license covers portions derived from Xenia.
- **[Xenia](https://github.com/xenia-project/xenia)**: the Xbox 360 emulator whose GPU code (shader
  translation, texture formats, the knowledge of Xenos) is the base of the SDK's.
- **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)** by hedge-dev: the static recompilation
  approach that ReXGlue's code generator is based on.

## Libraries

| Library | License | Used for |
|---|---|---|
| [DiligentCore](https://github.com/DiligentGraphics/DiligentCore) | Apache 2.0 | Host graphics API layer (Vulkan). To be added in Phase 2, together with its notice in `THIRD_PARTY_NOTICES.md`. |
| ReXGlue SDK and its third-party libraries | see `thirdparty/rexglue-sdk/LICENSE` and the project's `THIRD_PARTY_NOTICES.md` | Runtime, shader translator, texture code, Vulkan headers and loader, glslang, SPIR-V tools |

## XenosNative

Written by Gustavo Garcia with Claude (Anthropic). License: MIT, like the projects that use it, unless
decided otherwise before it is published.
