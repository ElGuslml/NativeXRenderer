// XenosNative - the shaders and pipelines a game used, kept on disk for the next start (internal, D42).
//
// The first time a draw needs a shader or a pipeline in a run, it is skipped while the shader is translated
// (D34) and the pipeline compiled (D28): objects appear a few frames late, mostly while a level loads. The
// storage removes that from the second run on:
//   - every new guest shader (its microcode), every new translation (a shader and a modification) and every
//     new pipeline (its two shaders, their modifications, and the fixed-function state) is appended to a
//     file under the runtime's cache directory, one per title;
//   - at the next start, before the first draw, the stored shaders are analyzed, the stored translations
//     queued to the workers, and each stored pipeline is created as soon as its shaders are ready, so the
//     draws find them done.
// Next to it, DiligentCore's pipeline cache (Vulkan's, driver binaries) is saved too, so compiling those
// pipelines again does not depend on the driver's own cache.
//
// The file is records appended to a header. A record cut short (the process killed while writing) ends the
// file; a header of another version starts the file again.

#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_set>
#include <vector>

#include "pipelines.h"

namespace xenos_native {

class PipelineStorage {
 public:
  struct Shader {
    uint8_t type;  // xenos::ShaderType
    uint64_t hash;
    std::vector<uint32_t> ucode;  // big-endian dwords, as in guest memory
  };
  struct Translation {
    uint8_t type;
    uint64_t hash, modification;
  };
  struct PipelineRecord {
    uint64_t vertex_hash, vertex_modification;
    uint64_t pixel_hash, pixel_modification;  // 0, 0: no pixel shader
    PipelineState state;                      // its shader pointers are null
  };

  // Reads `path` (a missing file, or one of another version, is started again), returning what it holds;
  // new records are appended to it from then on. False if the file cannot be written.
  bool Open(const std::filesystem::path& path, std::vector<Shader>& shaders, std::vector<Translation>& translations,
            std::vector<PipelineRecord>& pipelines);
  bool is_open() const { return open_; }

  // Called when a shader or a pipeline is new to this run; known ones are not stored twice.
  void AddShader(uint8_t type, uint64_t hash, const uint32_t* ucode, uint32_t dwords);
  void AddTranslation(uint8_t type, uint64_t hash, uint64_t modification);
  void AddPipeline(const PipelineRecord& record);

  // Appends what was added since the last call to the file (nothing to do most frames).
  void Flush();

  // Records stored in the file: read at Open, and added since.
  uint32_t shader_count() const { return uint32_t(shader_keys_.size()); }
  uint32_t pipeline_count() const { return uint32_t(pipeline_keys_.size()); }

 private:
  std::filesystem::path path_;
  bool open_ = false;
  std::vector<uint8_t> pending_;
  std::unordered_set<uint64_t> shader_keys_;
  std::unordered_set<uint64_t> translation_keys_;
  std::unordered_set<uint64_t> pipeline_keys_;
};

}  // namespace xenos_native
