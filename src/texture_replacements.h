// NativeXRenderer - a game's textures replaced by images (D176, D177).
//
// A guest texture is known by the hash of its base level's guest bytes (GuestTextures: XXH3 of the tiled, big-endian
// data, the same in every run while the game loads the same texture). Two ways to replace one:
// - D176, a folder: an image named by that hash (its 16 hexadecimal digits first, anything after them, .png, .jpg,
//   .jpeg, .bmp or .tga; subfolders searched too), from native_x_renderer_texture_replacements (relative to the game's
//   executable), decoded at the start.
// - D177, the game's choice at run time (native_x_renderer.h's ReplaceTexture, PreloadTextureImage): any image file for
//   a hash, with a brightness, changed whenever the game asks (a random note highway per song); it wins over the
//   folder's.
// Images are drawn at their own size, with mips made on the CPU. native_x_renderer_texture_log logs each texture's
// hash, format and size the first time it loads; native_x_renderer_texture_dump writes it as a DDS file.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"

namespace native_x_renderer {

class TextureReplacements {
 public:
  ~TextureReplacements();

  // Finds D176's images in the folder the cvar names and starts decoding them on a worker thread. False: no folder
  // or no image named by a hash (the game's own choices, D177, are still drawn).
  bool Initialize(Diligent::IRenderDevice* device);
  // On the renderer thread, once a frame: the images decoded since are made textures, and the game's choices applied.
  void BeginFrame();
  // The view of the image replacing a texture whose base level hashed to `hash`, or null.
  Diligent::ITextureView* Find(uint64_t hash);

  // An image decoded: RGBA8 levels, the base first.
  struct Decoded {
    std::string file;
    uint32_t width = 0;
    uint32_t height = 0;
    float brightness = 1.0f;  // as applied (an automatic one resolved)
    std::vector<std::vector<uint8_t>> levels;
  };

 private:
  struct Replacement {
    std::string file;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    Diligent::ITextureView* view = nullptr;
    bool used = false;
    std::filesystem::path image;  // D177: the chosen image and brightness it was made from
    float brightness = 1.0f;
  };

  void DecodeFolder(std::vector<std::pair<uint64_t, std::filesystem::path>> files);
  bool Make(uint64_t hash, const Decoded& decoded, Replacement& replacement);

  Diligent::IRenderDevice* device_ = nullptr;
  std::thread worker_;
  std::mutex mutex_;
  std::vector<std::pair<uint64_t, Decoded>> decoded_;  // D176: decoded, not made textures yet (guarded by mutex_)
  std::atomic<bool> pending_{false};
  std::unordered_map<uint64_t, Replacement> replacements_;  // D176's, the renderer thread's
  std::unordered_map<uint64_t, Replacement> chosen_;        // D177's, the renderer thread's
  uint64_t chosen_version_ = 0;                              // the requests' version applied
  // D177: textures replaced while queued draws or the record thread may still name their views, freed later.
  std::vector<std::pair<uint64_t, Diligent::RefCntAutoPtr<Diligent::ITexture>>> retired_;
  uint64_t frame_ = 0;
};

}  // namespace native_x_renderer
