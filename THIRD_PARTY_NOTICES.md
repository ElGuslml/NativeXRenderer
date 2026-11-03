# Third-party notices

NativeXRenderer's own code is under the MIT License (see [`LICENSE`](LICENSE)). A game built with it also
contains the following components, which keep their own licenses.

| Component | License | License file |
|---|---|---|
| [DiligentCore](https://github.com/DiligentGraphics/DiligentCore), Diligent Graphics LLC: the Vulkan backend, the graphics accessories and the SuperResolution module. NativeXRenderer's changes are in [`diligent_patches/`](diligent_patches/). | Apache 2.0 | `thirdparty/diligent-core/License.txt` |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), part of DiligentCore's Vulkan backend | Apache 2.0 | `thirdparty/diligent-core/ThirdParty/SPIRV-Cross/LICENSE` |
| [volk](https://github.com/zeux/volk), part of DiligentCore's Vulkan backend | MIT | `thirdparty/diligent-core/ThirdParty/volk/LICENSE.md` |
| [NVIDIA DLSS and NGX](https://github.com/DiligentGraphics/DLSS-Headers): the NGX library linked into the game, and `nvngx_dlss.dll` next to it. Fetched by DiligentCore when configuring. | NVIDIA RTX SDKs License | `LICENSE.txt` of DLSS-Headers (in the build's `_deps/dlss-headers-src/`) |
| [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) and [CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS), Advanced Micro Devices: the scaling and sharpening shaders (D115) | MIT | [`thirdparty/fsr1/LICENSE.txt`](thirdparty/fsr1/LICENSE.txt), [`thirdparty/cas/LICENSE.txt`](thirdparty/cas/LICENSE.txt) |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 1.1.4, Advanced Micro Devices: the FidelityFX API's headers and `amd_fidelityfx_vk.dll` next to the game (FSR 3.1, D116), fetched when configuring | MIT | [`thirdparty/ffx_api/LICENSE.txt`](thirdparty/ffx_api/LICENSE.txt) |
| [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling) 1.0.3, NVIDIA: the scaling and sharpening shaders (D115) | MIT | [`thirdparty/nis/LICENSE.txt`](thirdparty/nis/LICENSE.txt) |
| [King Kong Recompiled](https://github.com/TekRantGaming/king-kong-recompiled), TekRantGaming and contributors: the camera vote and its perspective test (`src/camera_vote.cpp`, D118), adapted | MIT | [`thirdparty/king-kong-recompiled/LICENSE`](thirdparty/king-kong-recompiled/LICENSE) |
| [SMAA](https://www.iryoku.com/smaa/), Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez: the antialiasing shaders and their lookup textures | MIT-style | [`thirdparty/smaa/LICENSE.txt`](thirdparty/smaa/LICENSE.txt) |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), Tom Clay, derived from [Xenia](https://xenia.jp): its shader translator, texture loaders and primitive processor are compiled into the library from the game's SDK checkout. NativeXRenderer's changes to it are commits of the SDK's [`recomp`](https://github.com/ElGuslml/rexglue-sdk/tree/recomp) branch (D89). | BSD 3-Clause | the SDK's `LICENSE` |

## NVIDIA DLSS

DLAA and the DLSS modes use NVIDIA DLSS. NVIDIA and DLSS are trademarks or registered trademarks of NVIDIA
Corporation in the U.S. and other countries. DLSS works only on NVIDIA RTX GPUs; elsewhere, NativeXRenderer
uses SMAA instead.
