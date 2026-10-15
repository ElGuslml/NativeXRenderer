# SMAA

Subpixel Morphological Antialiasing, the reference implementation by Jorge Jimenez, Jose I. Echevarria,
Belen Masia, Fernando Navarro and Diego Gutierrez: https://github.com/iryoku/smaa, commit `71c806a`
(fetched on 2026-10-14). MIT license, see `LICENSE.txt`.

Files, unchanged:
- `SMAA.hlsl`: the shaders, written for HLSL and GLSL. NativeXRenderer compiles them as GLSL
  (`SMAA_GLSL_4`) at startup (`src/aa.cpp`); CMake embeds the file as a string.
- `AreaTex.h`, `SearchTex.h`: the precomputed lookup textures, as byte arrays.
