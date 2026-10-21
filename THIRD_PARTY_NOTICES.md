# Third-party notices

NativeXRenderer's own code is under the MIT License (see [`LICENSE`](LICENSE)). A game built with it also
contains the following components, which keep their own licenses.

| Component | License | License file |
|---|---|---|
| [DiligentCore](https://github.com/DiligentGraphics/DiligentCore), Diligent Graphics LLC: the Vulkan backend, the graphics accessories and the SuperResolution module. NativeXRenderer's changes are in [`diligent_patches/`](diligent_patches/). | Apache 2.0 | `thirdparty/diligent-core/License.txt` |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), part of DiligentCore's Vulkan backend | Apache 2.0 | `thirdparty/diligent-core/ThirdParty/SPIRV-Cross/LICENSE` |
| [volk](https://github.com/zeux/volk), part of DiligentCore's Vulkan backend | MIT | `thirdparty/diligent-core/ThirdParty/volk/LICENSE.md` |
| [NVIDIA DLSS and NGX](https://github.com/DiligentGraphics/DLSS-Headers): the NGX library linked into the game, and `nvngx_dlss.dll` next to it. Fetched by DiligentCore when configuring. | NVIDIA RTX SDKs License | `LICENSE.txt` of DLSS-Headers (in the build's `_deps/dlss-headers-src/`) |
| [SMAA](https://www.iryoku.com/smaa/), Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez: the antialiasing shaders and their lookup textures | MIT-style | [`thirdparty/smaa/LICENSE.txt`](thirdparty/smaa/LICENSE.txt) |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), Tom Clay, derived from [Xenia](https://xenia.jp): its shader translator, texture loaders and primitive processor are compiled into the library from the game's SDK checkout. NativeXRenderer's changes to it are commits of the SDK's [`recomp`](https://github.com/ElGuslml/rexglue-sdk/tree/recomp) branch (D89). | BSD 3-Clause | the SDK's `LICENSE` |

## NVIDIA DLSS

DLAA and the DLSS modes use NVIDIA DLSS. NVIDIA and DLSS are trademarks or registered trademarks of NVIDIA
Corporation in the U.S. and other countries. DLSS works only on NVIDIA RTX GPUs; elsewhere, NativeXRenderer
uses SMAA instead.
