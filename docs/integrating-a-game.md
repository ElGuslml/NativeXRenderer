# Integrating NativeXRenderer into a ReXGlue game

A checklist for adding NativeXRenderer to another recompiled Xbox 360 game. POTCAWE is the first game and went
through steps 1–3 on 2026-10-13, in one day; GRAW is the second (steps 0–3 on 2026-10-15, in an afternoon).
Refine each step with what they teach, and keep the time each step took, so the next estimate is better.

## Before you start

- [ ] The game boots and plays on the Xenos path. NativeXRenderer is the fallback's replacement, not a way to
      make a broken game work, and the Xenos path is the reference every comparison needs.
- [ ] The game's `thirdparty/rexglue-sdk` submodule is the `recomp` branch of ElGuslml/rexglue-sdk (D89): the
      shared fixes and NativeXRenderer's changes to the SDK, one commit each (its `RECOMP.md`). Change 33
      counts the game's swaps, for showing frames with them (D86, an option); change 31 lets the renderer run
      on a thread of its own (D71).
- [ ] Note the XDK version (`node tools/xdkver.mjs assets/default.xex`). If a game with the same version is
      already mapped, its profile is the starting point.
- [ ] Have a way to drive the game without playing by hand, to reach the same spot every time (POTCAWE:
      `tools/game.ps1` and `tools/level2.ps1`, which can also press F9/F10). Every comparison depends on it.

## 1. Map the D3D layer

Follow [`xdk-d3d-reference.md`](xdk-d3d-reference.md#finding-the-entry-points-in-a-new-executable). What
worked for POTCAWE, in order:

- [ ] Index the generated C++ into functions and instructions with addresses. Use the `loc_` labels: blocks
      are not always emitted in address order. The tools are in [`../tools/`](../tools/README.md)
      (`ppc_index.py`, `pm4_consts.py`, `fn_summary.py`, `rs_setters.py`).
- [ ] Scan every function for PM4 packet headers. This finds the draws (even when they sit apart from the
      rest of the library), the resolve, the clear, the tiling functions, the queries and the segment end.
- [ ] Read one draw function: its flush calls give the device layout (dirty masks, constants, fetch
      constants, every register shadow group) in one place.
- [ ] Summarize the library's functions by the device fields they write: SetRenderTarget, the shader
      setters, SetIndices, SetStreamSource, SetTexture, the constant setters, the render-state setters (by
      register). Start from the nearest known XDK layout: fields move by a few blocks between versions.
- [ ] Find the engine's wrappers and draw submitters (few call sites), and whether the engine writes
      constants inline (a scan for `ld`/`or`/`std` on the dirty masks).
- [ ] Write each entry point into the game's NativeXRenderer document with its evidence: the packet it writes,
      the fields it touches, its callers.
- [ ] Find the resource objects' layout too (`ObjectLayout`): where a texture keeps its fetch constant and a
      surface its info and size. SetTexture and SetRenderTarget read them. They moved between XDK 2417 and
      5426.
- [ ] Check whether the engine calls BeginVertices / EndVertices itself (GRAW does; POTCAWE only through
      DrawVerticesUP): then hook EndVertices too, and BeginIndexedVertices if it exists. If EndVertices is not a
      function (Kameo: inlined by LTCG, `[dev+0] = [dev+<inline end>]` in the engine), set the profile's
      `inline_draw_end` instead (D77).

Remember: the command buffer write pointer holds the last dword written (the library writes with `stwu`).
Its offset, and the order of the dirty masks, change between XDK versions: read them in the draw's flush.

## 2. Write the game side

- [ ] The hook file (`src/native_x_renderer_<game>.cpp`), compiled into the executable, one line per entry
      point:

      ```cpp
      #include <native_x_renderer/hooks.h>
      NATIVE_X_RENDERER_HOOK(824AC3C0, DrawIndexedVertices)
      NATIVE_X_RENDERER_HOOK(822B5D58, Present)   // Present ends the frame
      ```

      The calls a game can report are in `include/native_x_renderer/calls.inc`. Hook the segment start
      (`KickSegment`): the observer needs it to follow a call's packets into a new segment.
- [ ] The profile: a `native_x_renderer::Profile` with the `DeviceLayout` offsets (POTCAWE:
      `potcawe::NativeXRendererProfile()`).
- [ ] `native_x_renderer::Initialize(profile, runtime()->graphics_system())` in the app's `OnPostSetup()`, before
      the guest starts.
- [ ] Defaults in the game's startup code: `native_x_renderer` off until parity. POTCAWE turned it on after
      its long sessions (D56).

## 3. Verify before rendering

- [ ] Trace (`--native_x_renderer_trace=true`): the per-frame summary in the log; F9 dumps a frame call by call
      (`python native_x_renderer/tools/frame_trace.py <log>` lists the dumps, `<log> <frame> passes` prints the passes). Check:
  - the call counts per frame, and that the arguments make sense;
  - that everything runs on one render thread (the summary counts other threads apart);
  - the tiling brackets and the resolves.

  Each dumped call ends with " from 0x… < 0x…", its return address in the caller and the caller's own
  (from the stack): the way to find an engine call the profile has to name, such as the resolve of the
  finished scene for the antialiasing (`scene_resolve_caller`, D41; and `scene_resolve_outer_caller` when
  every resolve goes through one engine function, as in GRAW, D65). The caller's own address is the frame's
  LR slot, which `__savegprlr` fills: with the codegen's `non_volatile_as_local` the SDK elides that helper,
  and the shared SDK series' codegen patch keeps its store (without it the slot holds garbage and the outer
  caller never matches).
- [ ] Observer (`--native_x_renderer_verify=true`, F10, or `--native_x_renderer_verify_at=<frame>`): one frame is
      verified after a warm-up frame, and the report is logged about 10 frames later. Iterate until 100 % of
      the draws match in several scenes:
  - a mismatch in a register or constant is a rule to find (see the rules in
    [`xdk-d3d-reference.md`](xdk-d3d-reference.md#rebuilding-a-draws-gpu-state));
  - unmatched draw packets attributed to Clear/ClearF/Resolve are the library's own;
  - points from buffers outside the ring are SetShaderGPRAllocation's;
  - a draw packet "written outside any hooked call" means a missing hook, or an engine that writes packets
    itself.
- [ ] Answer, with evidence:
  - [ ] Does the engine bypass D3D (direct PM4 writes, precompiled command buffers)? (The observer.)
  - [ ] Does it use predicated tiling, and what differs per tile? (The observer's tile differences.)
  - [ ] Does the CPU read GPU results? (Texture locks in the trace.)
  - [ ] Does any shader use memexport? (The observer.)
  - [ ] How are the videos drawn, and does the CPU write textures without Unlock? (Trace a video frame.)

## 4. Turn it on

- [ ] `--native_x_renderer=true`: first frames. Compare screenshots with the Xenos path at the same spots.
      What GRAW's first frames needed (D60–D62): a crash within seconds is worth a run under `cdb` with
      `sxd av` (the SDK uses first-chance access violations); a wrong color is worth a RenderDoc capture of
      NativeXRenderer's Vulkan device: the pixel history names the draw, and the shader debugger tells a
      wrong input from a shader the GPU runs differently from its SPIR-V.
- [ ] Engine-specific behavior goes into game-side callbacks, never into `native_x_renderer/`. If a callback the
      game needs does not exist yet, add it to the core in a general form.
- [ ] Parity checklist: every pass of a frame, menus, HUD, videos, every level reachable.
- [ ] Performance A/B against the Xenos path at the same spots, at 30 FPS for GPU load. Give both paths the
      same resolution on the command line: the game's options file may hold a render scale for the Xenos path
      (Kameo's first comparison had the Xenos path at 3× without noticing). Kameo's series (three outputs, both
      renderers, 30 and 60 FPS, a scripted route) is in its `docs/native-x-renderer-kameo.md`.
- [ ] Launcher row for the renderer. Make it the default only after a full play test (POTCAWE: D56). If
      NativeXRenderer cannot start, the Xenos path draws.
- [ ] Other aspect ratios (optional, D43). Find the camera builder: the code that turns the field of view
      into the projection and the culling frustum (`tan` callers are a good start). Hook it to widen the
      game's 16:9 views by `native_x_renderer::AspectFactor()`, and set `Profile::widens_cameras`. Then find where
      each view starts (the call stacks of its draws; the trace's " from" is one level) and call
      `native_x_renderer::SetGameAspectBox(true)` for 2D views, and `false` before anything that covers the
      whole screen. Look for 3D effects clipped to screen rectangles computed for 16:9. POTCAWE's
      `src/aspect.cpp` is the example.
      - A game that already has ultrawide hooks for the Xenos path (a wider camera, the HUD narrowed for an
        image the presenter stretches) can reuse them: they take `AspectFactor()` when
        `native_x_renderer::Renders()`, and the renderer stretches the image instead of the presenter. If they
        handle wider outputs only, keep `native_x_renderer_fill_screen` off for taller ones. GRAW is the
        example (D66, its `src/widescreen.cpp`).
      - Kameo narrows its 2D layer at the builders of the screen's orthographic projections, and only while the
        render target is the screen: the same projections draw the menu's pages into a texture. A rectangle
        across the whole width (a fade, the cinematics' bars) keeps its width (its `src/widescreen.cpp`).
- [ ] DLAA (optional, D47). Call `native_x_renderer::BeginView` at each view's start, with the 3D views'
      view-projection matrix (the constants the view gives its vertex shaders), and
      `native_x_renderer::EndViewGeometry` where the view's post-processing starts. Check the depth convention
      in a trace (a ClearF to 0 means reversed depth): the viewport carries it, nothing else to do. POTCAWE's
      `src/views.cpp` is the example.
      - The DLSS modes come with it too (D49). They need the view's clear to happen after `BeginView`,
        since the view's reduced targets are cleared then. Look in the log for the warning about pixel
        positions read in a reduced view: such draws need the scale reverted per draw.
      - The motion of objects that move on their own comes with it (D48), with nothing more to hook. The log
        names the registers the view shares ("object motion: the registers the main view shares"); they
        should be the camera's. It assumes skinning and animation in the vertex shader's constants: vertex
        data the CPU rewrites each frame keeps the camera's motion.

## What to write down

The game's repository gets a document like
[`docs/native-x-renderer-potcawe.md`](../../docs/native-x-renderer-potcawe.md):
- the D3D map with evidence;
- the device offsets;
- the frame's structure from the trace;
- the engine's behavior;
- the baseline measurements and the results.

Anything learned that is true for every game goes back into this folder's documents.

## Time taken

| Game | Step 1 | Steps 2–3 | Notes |
|---|---|---|---|
| POTCAWE (XDK 5426) | a few hours | the rest of the day | The draws were found only by the PM4 scan. The observer needed four rounds: the ring reuse, the ring wrap, the write pointer semantics, then the rules. |
| GRAW (XDK 2417) | about 1.5 hours | about 2 hours | The PM4 scan and the draw's flush gave the library's range and the device layout in minutes; a background agent mapped the rest with evidence while the library learned the new masks and objects. The observer went 78.5 % → 99.2 % (the fetch rule) → 100 % (the inline begin tracked, then the segment rule), five runs of the training mission. Step 0 (syncing the shared SDK series) found that patch 0020 made GRAW wait on its occlusion queries. |
| Kameo (XDK 2099) | about 2 hours (a background agent, from the PM4 scan and GRAW's map) | about 2 hours | GRAW's layout up to `+12964`. The observer started at 0 %: the command buffer is above 0xE0000000, which the library read 4 KB off (D76); then 100 % of the hooked draws, and with the inline draws the engine commits itself (EndVertices inlined by LTCG, D77) 100 % of all, in the title video, the menu, a video and the first level. |
