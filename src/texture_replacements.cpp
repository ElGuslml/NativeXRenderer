// NativeXRenderer - see texture_replacements.h.

#include "texture_replacements.h"

#include <native_x_renderer/native_x_renderer.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iterator>

// The image decoders of stb_image (the SDK's thirdparty), private to this file: PNG, JPEG, BMP and TGA.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include "stb_image.h"

REXCVAR_DEFINE_STRING(native_x_renderer_texture_replacements, "", "NativeXRenderer",
                      "A folder of images that replace the game's textures (D176): each named by the hash "
                      "native_x_renderer_texture_log gives (16 hexadecimal digits, then anything), .png, .jpg, .jpeg, "
                      ".bmp or .tga, subfolders too; relative to the game's executable. Empty: none");
REXCVAR_DEFINE_BOOL(native_x_renderer_texture_log, false, "NativeXRenderer",
                    "Log each texture the first time it loads: the hash of its base level that names a replacement "
                    "image (D176), its format, size and levels");
REXCVAR_DEFINE_BOOL(native_x_renderer_texture_dump, false, "NativeXRenderer",
                    "Write each 2D DXT1, DXT3, DXT5 or 8_8_8_8 texture's base level the first time it loads, as a DDS "
                    "file named by its hash in texture_dump next to the executable (D176)");

namespace native_x_renderer {

using namespace Diligent;

namespace {

// The hash that names an image: its first 16 characters hexadecimal, and no 17th hexadecimal one.
bool HashOf(const std::filesystem::path& file, uint64_t& hash) {
  std::string stem = file.stem().string();
  if (stem.size() < 16 || (stem.size() > 16 && std::isxdigit(uint8_t(stem[16])))) return false;
  for (size_t i = 0; i < 16; ++i) {
    if (!std::isxdigit(uint8_t(stem[i]))) return false;
  }
  hash = std::strtoull(stem.substr(0, 16).c_str(), nullptr, 16);
  return true;
}

bool IsImage(const std::filesystem::path& file) {
  std::string ext = file.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

// The next level, each texel the average of the 2x2 (or 2x1, 1x2) texels it covers.
std::vector<uint8_t> HalfLevel(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, uint32_t& nw, uint32_t& nh) {
  nw = std::max(1u, w / 2);
  nh = std::max(1u, h / 2);
  std::vector<uint8_t> dst(size_t(nw) * nh * 4);
  for (uint32_t y = 0; y < nh; ++y) {
    const uint32_t y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
    for (uint32_t x = 0; x < nw; ++x) {
      const uint32_t x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1);
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t sum = src[(size_t(y0) * w + x0) * 4 + c] + src[(size_t(y0) * w + x1) * 4 + c] +
                             src[(size_t(y1) * w + x0) * 4 + c] + src[(size_t(y1) * w + x1) * 4 + c];
        dst[(size_t(y) * nw + x) * 4 + c] = uint8_t((sum + 2) / 4);
      }
    }
  }
  return dst;
}

// An image file decoded with its mips, its colors times `brightness` toward black (kAutoBrightness: as much as its
// average luminance is above kAutoLuminance, to at most kAutoLeast); false if it cannot be read.
bool DecodeImage(const std::filesystem::path& path, float brightness, TextureReplacements::Decoded& decoded) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = bytes.empty() ? nullptr
                                  : stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
  if (!pixels || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
    REXLOG_WARN("[native_x_renderer] texture replacements: {} could not be read{}{}", path.string(),
                pixels ? "" : ": ", pixels ? "" : stbi_failure_reason());
    if (pixels) stbi_image_free(pixels);
    return false;
  }
  decoded.file = path.filename().string();
  decoded.width = uint32_t(w);
  decoded.height = uint32_t(h);
  decoded.levels.clear();
  decoded.levels.emplace_back(pixels, pixels + size_t(w) * h * 4);
  stbi_image_free(pixels);
  std::vector<uint8_t>& base = decoded.levels.front();
  if (brightness < 0.0f) {
    double sum = 0.0;
    for (size_t i = 0; i < base.size(); i += 4) sum += 0.2126 * base[i] + 0.7152 * base[i + 1] + 0.0722 * base[i + 2];
    const double luminance = sum / (double(w) * h * 255.0);
    brightness = luminance > kAutoLuminance ? std::max(kAutoLeast, float(kAutoLuminance / luminance)) : 1.0f;
  }
  decoded.brightness = std::clamp(brightness, 0.0f, 1.0f);
  if (decoded.brightness < 1.0f) {
    const uint32_t factor = uint32_t(std::lround(decoded.brightness * 256.0f));
    for (size_t i = 0; i < base.size(); i += 4) {
      for (size_t c = 0; c < 3; ++c) base[i + c] = uint8_t((base[i + c] * factor + 128) >> 8);
    }
  }
  uint32_t lw = decoded.width, lh = decoded.height;
  while (lw > 1 || lh > 1) {
    uint32_t nw, nh;
    decoded.levels.push_back(HalfLevel(decoded.levels.back(), lw, lh, nw, nh));
    lw = nw;
    lh = nh;
  }
  return true;
}

// D177: the game's choices, from any thread, and the images decoded for them on one worker thread.
struct Wanted {
  std::filesystem::path image;
  float brightness = 1.0f;
  bool operator==(const Wanted& o) const { return image == o.image && brightness == o.brightness; }
};

class Choices {
 public:
  ~Choices() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  void Replace(uint64_t hash, const std::filesystem::path& image, float brightness) {
    std::lock_guard lock(mutex_);
    requested_[hash] = {image, brightness};
    if (!image.empty()) QueueLocked({image, brightness});
    ++version_;
  }

  void Preload(const std::filesystem::path& image, float brightness) {
    if (image.empty()) return;
    std::lock_guard lock(mutex_);
    QueueLocked({image, brightness});
  }

  uint64_t version() const { return version_.load(std::memory_order_acquire); }

  // The requests, with each image decoded so far (null while it is not).
  struct Request {
    uint64_t hash;
    Wanted wanted;
    std::shared_ptr<const TextureReplacements::Decoded> decoded;
  };
  std::vector<Request> Requests(uint64_t& version) {
    std::lock_guard lock(mutex_);
    version = version_;
    std::vector<Request> out;
    for (const auto& [hash, wanted] : requested_) {
      std::shared_ptr<const TextureReplacements::Decoded> found;
      for (const auto& [key, decoded] : cache_) {
        if (key == wanted) found = decoded;
      }
      out.push_back({hash, wanted, found});
    }
    return out;
  }

 private:
  void QueueLocked(const Wanted& wanted) {
    for (const auto& [key, decoded] : cache_) {
      if (key == wanted) return;
    }
    if (std::find(queue_.begin(), queue_.end(), wanted) != queue_.end()) return;
    queue_.push_back(wanted);
    if (!worker_.joinable()) worker_ = std::thread([this] { Work(); });
    wake_.notify_one();
  }

  void Work() {
    std::unique_lock lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      Wanted wanted = queue_.front();
      queue_.pop_front();
      lock.unlock();
      auto decoded = std::make_shared<TextureReplacements::Decoded>();
      const auto start = std::chrono::steady_clock::now();
      const bool read = DecodeImage(wanted.image, wanted.brightness, *decoded);
      const auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      lock.lock();
      if (!read) continue;
      REXLOG_INFO("[native_x_renderer] texture image {} decoded in {} ms ({}x{}, brightness {:.2f})",
                  decoded->file, ms, decoded->width, decoded->height, decoded->brightness);
      cache_.emplace_back(wanted, std::move(decoded));
      // Kept: the images asked for, and the two newest others (the preloads).
      size_t others = 0;
      for (size_t i = cache_.size(); i-- > 0;) {
        const bool asked = std::any_of(requested_.begin(), requested_.end(),
                                       [&](const auto& r) { return r.second == cache_[i].first; });
        if (!asked && ++others > 2) cache_.erase(cache_.begin() + ptrdiff_t(i));
      }
      ++version_;
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  bool stop_ = false;
  std::deque<Wanted> queue_;
  std::vector<std::pair<Wanted, std::shared_ptr<const TextureReplacements::Decoded>>> cache_;
  std::unordered_map<uint64_t, Wanted> requested_;
  std::atomic<uint64_t> version_{0};
};

Choices& TheChoices() {
  static Choices choices;
  return choices;
}

}  // namespace

void ReplaceTexture(uint64_t hash, const std::filesystem::path& image, float brightness) {
  TheChoices().Replace(hash, image, brightness);
}

void PreloadTextureImage(const std::filesystem::path& image, float brightness) {
  TheChoices().Preload(image, brightness);
}

TextureReplacements::~TextureReplacements() {
  if (worker_.joinable()) worker_.join();
}

bool TextureReplacements::Initialize(IRenderDevice* device) {
  device_ = device;
  const std::string setting = REXCVAR_GET(native_x_renderer_texture_replacements);
  if (setting.empty()) return false;
  std::filesystem::path folder = std::filesystem::u8path(setting);
  if (folder.is_relative()) folder = rex::filesystem::GetExecutableFolder() / folder;
  std::error_code error;
  if (!std::filesystem::is_directory(folder, error)) {
    REXLOG_INFO("[native_x_renderer] texture replacements: no folder {}", folder.string());
    return false;
  }
  std::vector<std::pair<uint64_t, std::filesystem::path>> files;
  for (auto it = std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    uint64_t hash;
    if (it->is_regular_file(error) && IsImage(it->path()) && HashOf(it->path(), hash)) {
      files.emplace_back(hash, it->path());
    }
  }
  // One image a hash: the first by its path, so the same folder always gives the same one.
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  std::vector<std::pair<uint64_t, std::filesystem::path>> unique;
  for (const auto& file : files) {
    auto same = std::find_if(unique.begin(), unique.end(), [&](const auto& u) { return u.first == file.first; });
    if (same != unique.end()) {
      REXLOG_WARN("[native_x_renderer] texture replacements: {} and {} name the same texture; the first is used",
                  same->second.string(), file.second.string());
      continue;
    }
    unique.push_back(file);
  }
  if (unique.empty()) {
    REXLOG_INFO("[native_x_renderer] texture replacements: no image named by a texture's hash in {}", folder.string());
    return false;
  }
  REXLOG_INFO("[native_x_renderer] texture replacements: {} images in {}, decoding", unique.size(), folder.string());
  worker_ = std::thread([this, unique = std::move(unique)]() mutable { DecodeFolder(std::move(unique)); });
  return true;
}

void TextureReplacements::DecodeFolder(std::vector<std::pair<uint64_t, std::filesystem::path>> files) {
  const auto start = std::chrono::steady_clock::now();
  for (const auto& [hash, path] : files) {
    Decoded decoded;
    if (!DecodeImage(path, 1.0f, decoded)) continue;
    std::lock_guard lock(mutex_);
    decoded_.emplace_back(hash, std::move(decoded));
    pending_ = true;
  }
  REXLOG_INFO("[native_x_renderer] texture replacements: decoded in {} ms",
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

bool TextureReplacements::Make(uint64_t hash, const Decoded& decoded, Replacement& replacement) {
  TextureDesc desc;
  const std::string name = fmt::format("NativeXRenderer replacement {:016X}", hash);
  desc.Name = name.c_str();
  desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;  // what a 1D or 2D binding samples, as the guest textures
  desc.Width = decoded.width;
  desc.Height = decoded.height;
  desc.ArraySize = 1;
  desc.MipLevels = uint32_t(decoded.levels.size());
  desc.Format = TEX_FORMAT_RGBA8_UNORM;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  std::vector<TextureSubResData> data;
  uint32_t w = decoded.width;
  for (const std::vector<uint8_t>& level : decoded.levels) {
    data.push_back(TextureSubResData{level.data(), uint64_t(w) * 4});
    w = std::max(1u, w / 2);
  }
  TextureData initial{data.data(), uint32_t(data.size())};
  replacement.file = decoded.file;
  device_->CreateTexture(desc, &initial, &replacement.texture);
  if (!replacement.texture) {
    REXLOG_WARN("[native_x_renderer] texture replacements: {} ({}x{}) could not be made a texture", decoded.file,
                decoded.width, decoded.height);
    return false;
  }
  replacement.view = replacement.texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  return true;
}

void TextureReplacements::BeginFrame() {
  ++frame_;
  // Textures replaced 8 frames ago: no queued draw names them any more.
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [&](const auto& r) { return frame_ - r.first > 8; }),
                 retired_.end());
  if (pending_.load(std::memory_order_acquire)) {
    std::vector<std::pair<uint64_t, Decoded>> ready;
    {
      std::lock_guard lock(mutex_);
      ready.swap(decoded_);
      pending_ = false;
    }
    for (auto& [hash, decoded] : ready) {
      Replacement replacement;
      if (!Make(hash, decoded, replacement)) continue;
      REXLOG_INFO("[native_x_renderer] texture {:016X}: {} ready ({}x{}, {} levels)", hash, decoded.file, decoded.width,
                  decoded.height, decoded.levels.size());
      replacements_[hash] = std::move(replacement);
    }
  }
  // D177: the game's choices, when they or the decoded images changed.
  Choices& choices = TheChoices();
  if (choices.version() == chosen_version_) return;
  uint64_t version;
  std::vector<Choices::Request> requests = choices.Requests(version);
  chosen_version_ = version;
  for (const Choices::Request& request : requests) {
    auto it = chosen_.find(request.hash);
    if (request.wanted.image.empty()) {
      if (it != chosen_.end()) {
        retired_.emplace_back(frame_, std::move(it->second.texture));
        chosen_.erase(it);
        REXLOG_INFO("[native_x_renderer] texture {:016X}: its own again", request.hash);
      }
      continue;
    }
    if (!request.decoded) continue;  // still decoding: what it showed stays
    if (it != chosen_.end() && it->second.image == request.wanted.image &&
        it->second.brightness == request.wanted.brightness) {
      continue;
    }
    Replacement replacement;
    if (!Make(request.hash, *request.decoded, replacement)) continue;
    replacement.image = request.wanted.image;
    replacement.brightness = request.wanted.brightness;
    REXLOG_INFO("[native_x_renderer] texture {:016X}: now {} ({}x{}, brightness {:.2f})", request.hash,
                request.decoded->file, request.decoded->width, request.decoded->height, request.decoded->brightness);
    if (it != chosen_.end()) {
      retired_.emplace_back(frame_, std::move(it->second.texture));
      it->second = std::move(replacement);
    } else {
      chosen_.emplace(request.hash, std::move(replacement));
    }
  }
}

ITextureView* TextureReplacements::Find(uint64_t hash) {
  auto chosen = chosen_.find(hash);
  if (chosen != chosen_.end()) return chosen->second.view;
  if (replacements_.empty()) return nullptr;
  auto it = replacements_.find(hash);
  if (it == replacements_.end()) return nullptr;
  if (!it->second.used) {
    it->second.used = true;
    REXLOG_INFO("[native_x_renderer] texture {:016X}: drawn with {}", hash, it->second.file);
  }
  return it->second.view;
}

}  // namespace native_x_renderer
