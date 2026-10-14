// XenosNative - see storage.h.

#include "storage.h"

#include <rex/logging.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include <xxhash.h>

namespace xenos_native {

namespace {

constexpr uint32_t kMagic = 0x53504E58;  // "XNPS"
// Bump when what a record means changes: PipelineState's layout or its DiligentCore enums, the
// translator's modification bits, the host formats, the records themselves (2: translation records).
constexpr uint32_t kVersion = 2;
constexpr uint8_t kShaderTag = 'S', kTranslationTag = 'T', kPipelineTag = 'P';

struct Header {
  uint32_t magic, version, state_size, reserved;
};

static_assert(sizeof(PipelineStorage::PipelineRecord) == 32 + sizeof(PipelineState),
              "PipelineRecord must have no padding (it is stored and hashed as bytes)");

uint64_t ShaderKey(uint8_t type, uint64_t hash) { return hash ^ (0x9E3779B97F4A7C15ull * (uint64_t(type) + 1)); }

uint64_t TranslationKey(uint8_t type, uint64_t hash, uint64_t modification) {
  const uint64_t fields[3] = {type, hash, modification};
  return XXH3_64bits(fields, sizeof(fields));
}

template <typename T>
void Put(std::vector<uint8_t>& out, const T& value) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool Get(const std::vector<uint8_t>& in, size_t& pos, T& value) {
  if (in.size() - pos < sizeof(T)) return false;
  std::memcpy(&value, in.data() + pos, sizeof(T));
  pos += sizeof(T);
  return true;
}

}  // namespace

bool PipelineStorage::Open(const std::filesystem::path& path, std::vector<Shader>& shaders,
                           std::vector<Translation>& translations, std::vector<PipelineRecord>& pipelines) {
  path_ = path;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);

  std::vector<uint8_t> data;
  {
    std::ifstream file(path, std::ios::binary);
    if (file) data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  Header header{};
  bool header_ok = data.size() >= sizeof(Header);
  if (header_ok) {
    std::memcpy(&header, data.data(), sizeof(Header));
    header_ok = header.magic == kMagic && header.version == kVersion && header.state_size == sizeof(PipelineState);
  }
  size_t valid_end = sizeof(Header);
  if (header_ok) {
    size_t pos = sizeof(Header);
    while (pos < data.size()) {
      uint8_t tag = data[pos++];
      if (tag == kShaderTag) {
        Shader shader;
        uint32_t dwords = 0;
        if (!Get(data, pos, shader.type) || !Get(data, pos, dwords) || !Get(data, pos, shader.hash)) break;
        if (data.size() - pos < size_t(dwords) * 4) break;
        shader.ucode.resize(dwords);
        std::memcpy(shader.ucode.data(), data.data() + pos, size_t(dwords) * 4);
        pos += size_t(dwords) * 4;
        shader_keys_.insert(ShaderKey(shader.type, shader.hash));
        shaders.push_back(std::move(shader));
      } else if (tag == kTranslationTag) {
        Translation translation;
        if (!Get(data, pos, translation.type) || !Get(data, pos, translation.hash) ||
            !Get(data, pos, translation.modification)) {
          break;
        }
        translation_keys_.insert(TranslationKey(translation.type, translation.hash, translation.modification));
        translations.push_back(translation);
      } else if (tag == kPipelineTag) {
        PipelineRecord record;
        if (!Get(data, pos, record)) break;
        record.state.vertex_shader = nullptr;
        record.state.pixel_shader = nullptr;
        pipeline_keys_.insert(XXH3_64bits(&record, sizeof(record)));
        pipelines.push_back(record);
      } else {
        break;
      }
      valid_end = pos;
    }
  }
  // A file of another version, or one whose last record was cut short: written again with what is valid.
  if (!header_ok || valid_end != data.size()) {
    if (!data.empty()) {
      REXLOG_INFO("[xenos_native] pipeline storage {}: {}", path.string(),
                  header_ok ? "a record cut short at the end, dropped" : "another version, started again");
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    header = {kMagic, kVersion, uint32_t(sizeof(PipelineState)), 0};
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (header_ok && valid_end > sizeof(Header)) {
      file.write(reinterpret_cast<const char*>(data.data() + sizeof(Header)), valid_end - sizeof(Header));
    }
    if (!file) {
      REXLOG_WARN("[xenos_native] pipeline storage: cannot write {}", path.string());
      return false;
    }
  }
  open_ = true;
  return true;
}

void PipelineStorage::AddShader(uint8_t type, uint64_t hash, const uint32_t* ucode, uint32_t dwords) {
  if (!open_ || !shader_keys_.insert(ShaderKey(type, hash)).second) return;
  pending_.push_back(kShaderTag);
  Put(pending_, type);
  Put(pending_, dwords);
  Put(pending_, hash);
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(ucode);
  pending_.insert(pending_.end(), bytes, bytes + size_t(dwords) * 4);
}

void PipelineStorage::AddTranslation(uint8_t type, uint64_t hash, uint64_t modification) {
  if (!open_ || !translation_keys_.insert(TranslationKey(type, hash, modification)).second) return;
  pending_.push_back(kTranslationTag);
  Put(pending_, type);
  Put(pending_, hash);
  Put(pending_, modification);
}

void PipelineStorage::AddPipeline(const PipelineRecord& record) {
  if (!open_) return;
  PipelineRecord stored = record;  // the host shaders are this run's: stored as null
  stored.state.vertex_shader = nullptr;
  stored.state.pixel_shader = nullptr;
  if (!pipeline_keys_.insert(XXH3_64bits(&stored, sizeof(stored))).second) return;
  pending_.push_back(kPipelineTag);
  Put(pending_, stored);
}

void PipelineStorage::Flush() {
  if (pending_.empty()) return;
  std::ofstream file(path_, std::ios::binary | std::ios::app);
  file.write(reinterpret_cast<const char*>(pending_.data()), pending_.size());
  if (!file) {
    REXLOG_WARN("[xenos_native] pipeline storage: cannot append to {}; not stored any more", path_.string());
    open_ = false;
  }
  pending_.clear();
}

}  // namespace xenos_native
