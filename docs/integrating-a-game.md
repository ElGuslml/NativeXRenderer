# Integrating XenosNative into a ReXGlue game

A checklist for adding XenosNative to another recompiled Xbox 360 game. POTCAWE is the first game and went
through steps 1–3 on 2026-10-13, in one day; GRAW will be the second. Refine each step with what they teach,
and keep the time each step took, so the next estimate is better.

## Before you start

- [ ] The game boots and plays on the Xenos path. XenosNative is the fallback's replacement, not a way to
      make a broken game work, and the Xenos path is the reference every comparison needs.
- [ ] The game's project uses the shared SDK patch series, plus XenosNative's own SDK patches
      (`xenos_native/sdk_patches/`, applied after the shared series by `cmake/apply_sdk_patches.cmake`).
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
- [ ] Write each entry point into the game's XenosNative document with its evidence: the packet it writes,
      the fields it touches, its callers.

Remember: the command buffer write pointer holds the last dword written (the library writes with `stwu`).

## 2. Write the game side

- [ ] The hook file (`src/xenos_native_<game>.cpp`), compiled into the executable, one line per entry
      point:

      ```cpp
      #include <xenos_native/hooks.h>
      XENOS_NATIVE_HOOK(824AC3C0, DrawIndexedVertices)
      XENOS_NATIVE_HOOK(822B5D58, Present)   // Present ends the frame
      ```

      The calls a game can report are in `include/xenos_native/calls.inc`. Hook the segment start
      (`KickSegment`): the observer needs it to follow a call's packets into a new segment.
- [ ] The profile: a `xenos_native::Profile` with the `DeviceLayout` offsets (POTCAWE:
      `potcawe::XenosNativeProfile()`).
- [ ] `xenos_native::Initialize(profile, runtime()->graphics_system())` in the app's `OnPostSetup()`, before
      the guest starts.
- [ ] Defaults in the game's startup code: `xenos_native` off until parity.

## 3. Verify before rendering

- [ ] Trace (`--xenos_native_trace=true`): the per-frame summary in the log; F9 dumps a frame call by call
      (`python xenos_native/tools/xn_trace.py <log>` lists the dumps, `<log> <frame> passes` prints the passes). Check:
  - the call counts per frame, and that the arguments make sense;
  - that everything runs on one render thread (the summary counts other threads apart);
  - the tiling brackets and the resolves.

  Each dumped call ends with " from 0x…", its return address in the caller: the way to find an engine
  call the profile has to name, such as the resolve of the finished scene for the antialiasing
  (`scene_resolve_caller`, D41).
- [ ] Observer (`--xenos_native_verify=true`, F10, or `--xenos_native_verify_at=<frame>`): one frame is
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

- [ ] `--xenos_native=true`: first frames. Compare screenshots with the Xenos path at the same spots.
- [ ] Engine-specific behavior goes into game-side callbacks, never into `xenos_native/`. If a callback the
      game needs does not exist yet, add it to the core in a general form.
- [ ] Parity checklist: every pass of a frame, menus, HUD, videos, every level reachable.
- [ ] Performance A/B against the Xenos path at the same spots, at 30 FPS for GPU load.
- [ ] Launcher row for the renderer. Make it the default only after a full play test.
- [ ] Other aspect ratios (optional, D43). Find the camera builder: the code that turns the field of view
      into the projection and the culling frustum (`tan` callers are a good start). Hook it to widen the
      game's 16:9 views by `xenos_native::AspectFactor()`, and set `Profile::widens_cameras`. Then find where
      each view starts (the call stacks of its draws; the trace's " from" is one level) and call
      `xenos_native::SetGameAspectBox(true)` for 2D views, and `false` before anything that covers the
      whole screen. Look for 3D effects clipped to screen rectangles computed for 16:9. POTCAWE's
      `src/aspect.cpp` is the example.

## What to write down

The game's repository gets a document like
[`docs/xenos-native-potcawe.md`](../../docs/xenos-native-potcawe.md):
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
