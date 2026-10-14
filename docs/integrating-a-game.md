# Integrating XenosNative into a ReXGlue game

A checklist for adding XenosNative to another recompiled Xbox 360 game. It is a first draft: POTCAWE is the
first game, and GRAW will be the second. Refine each step with what they teach, and keep the time each
step took, so the next estimate is better.

## Before you start

- [ ] The game boots and plays on the Xenos path. XenosNative is the fallback's replacement, not a way to
      make a broken game work, and the Xenos path is the reference every comparison needs.
- [ ] The game's project uses the shared SDK patch series, plus XenosNative's own SDK patches.
- [ ] Note the XDK version (`node tools/xdkver.mjs assets/default.xex`). If a game with the same version is
      already mapped, its profile is the starting point.
- [ ] Have a way to drive the game without playing by hand, to reach the same spot every time (POTCAWE:
      `tools/game.ps1` and `tools/level2.ps1`). Every comparison depends on it.

## 1. Map the D3D layer

Follow [`xdk-d3d-reference.md`](xdk-d3d-reference.md#finding-the-entry-points-in-a-new-executable).

- [ ] Find the library's address range.
- [ ] Find the entry points of the [table](xdk-d3d-reference.md#entry-points). Write each one into the game's
      XenosNative document with its evidence: the packet it writes, its callers, its arguments.
- [ ] Find the device structure offsets the core needs (register shadow, constants, fetch constants, current
      shaders and surfaces, dirty masks) and the object layouts (shader microcode, texture and vertex
      fetch constants).

## 2. Write the game side

- [ ] The hook file (`src/xenos_native_<game>.cpp`): one macro line per entry point.
- [ ] The profile (device and object offsets), passed to the core at startup.
- [ ] Defaults in the game's startup code: `xenos_native` off until parity.

## 3. Verify before rendering

- [ ] Trace one frame of several scenes (`xenos_native_trace`): call counts per frame, the order of calls,
      tiling brackets.
- [ ] Run the observer (`xenos_native_verify`) until 100 % of the draws match the command processor's state
      in those scenes.
- [ ] Answer, with evidence:
  - [ ] Does the engine bypass D3D (direct PM4 writes, precompiled command buffers)?
  - [ ] Does it use predicated tiling, and what differs per tile?
  - [ ] Does the CPU read GPU results?
  - [ ] Does any shader use memexport?
  - [ ] How are the videos drawn?

## 4. Turn it on

- [ ] `--xenos_native=true`: first frames. Compare screenshots with the Xenos path at the same spots.
- [ ] Engine-specific behavior goes into game-side callbacks, never into `xenos_native/`. If a callback the
      game needs does not exist yet, add it to the core in a general form.
- [ ] Parity checklist: every pass of a frame, menus, HUD, videos, every level reachable.
- [ ] Performance A/B against the Xenos path at the same spots, at 30 FPS for GPU load.
- [ ] Launcher row for the renderer. Make it the default only after a full play test.

## What to write down

The game's repository gets a document like
[`docs/xenos-native-potcawe.md`](../../docs/xenos-native-potcawe.md):
- the D3D map with evidence;
- the device offsets;
- the engine's behavior;
- the baseline measurements and the results.

Anything learned that is true for every game goes back into this folder's documents.
