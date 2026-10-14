// XenosNative - see shaders.h.

#include "shaders.h"

#include "spirv_patch.h"

#include <rex/logging.h>

#include <exception>
#include <string>
#include <vector>

#include <xxhash.h>

namespace xenos_native {

using namespace Diligent;
namespace rg = rex::graphics;

namespace {

// The SDK's analysis throws on some invalid microcode and dereferences null on other (a texture fetch
// whose filter has no name): the access violation becomes a failure here, like the exceptions (which
// the filter lets through to the caller's catch).
bool AnalyzeProtected(rg::Shader& shader, rex::string::StringBuffer& disassembly) {
#ifdef _WIN32
  __try {
    shader.AnalyzeUcode(disassembly);
    return true;
  } __except (_exception_code() == 0xC0000005u ? 1 /* EXCEPTION_EXECUTE_HANDLER */
                                                : 0 /* EXCEPTION_CONTINUE_SEARCH */) {
    return false;
  }
#else
  shader.AnalyzeUcode(disassembly);
  return true;
#endif
}

}  // namespace

ShaderCache::ShaderCache() = default;
ShaderCache::~ShaderCache() = default;

bool ShaderCache::Initialize(IRenderDevice* device) {
  device_ = device;
  // What the device created by device.cpp has. DiligentCore enables neither clip and cull distances nor
  // sample rate shading, and XenosNative sets texture swizzles in the shader (system constants) rather
  // than in image views. Memory export (vertex and pixel stores) is not used by POTCAWE; off for now.
  features_ = rg::SpirvShaderTranslator::Features(false);
  const auto& info = device->GetDeviceInfo();
  if (info.APIVersion >= Version{1, 2}) {
    features_.spirv_version = spv::Spv_1_5;
  } else if (info.APIVersion >= Version{1, 1}) {
    features_.spirv_version = spv::Spv_1_3;
  }
  features_.max_storage_buffer_range = UINT32_MAX;  // one binding for the whole 512 MB
  features_.full_draw_index_uint32 = true;
  features_.image_view_format_swizzle = false;
  translator_ = std::make_unique<rg::SpirvShaderTranslator>(features_, true, true, false);
  return true;
}

rg::SpirvShader* ShaderCache::Get(rg::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords,
                                  bool* new_failure) {
  if (new_failure) *new_failure = false;
  uint64_t hash = XXH3_64bits(ucode, size_t(dwords) * sizeof(uint32_t));
  auto key = std::make_pair(hash, uint32_t(type));
  auto it = shaders_.find(key);
  if (it != shaders_.end()) return it->second.get();
  auto shader = std::make_unique<rg::SpirvShader>(type, hash, ucode, dwords);
  std::string error;
  try {
    if (!AnalyzeProtected(*shader, disassembly_buffer_)) error = "access violation";
  } catch (const std::exception& e) {
    // The SDK's disassembly throws on what it cannot name (an unknown vertex format, for one).
    error = e.what();
  }
  if (!error.empty()) {
    REXLOG_ERROR("[xenos_native] {} shader {:016X} ({} dwords): analysis failed: {}",
                 type == rg::xenos::ShaderType::kVertex ? "vertex" : "pixel", hash, dwords, error);
    disassembly_buffer_.Reset();
    shader.reset();
    if (new_failure) *new_failure = true;
  }
  return shaders_.emplace(key, std::move(shader)).first->second.get();
}

IShader* ShaderCache::GetHostShader(rg::SpirvShader& shader, uint64_t modification) {
  rg::Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  auto it = host_shaders_.find(translation);
  if (it != host_shaders_.end()) return it->second;
  RefCntAutoPtr<IShader>& host = host_shaders_[translation];

  bool vertex = shader.type() == rg::xenos::ShaderType::kVertex;
  bool translated = translation->is_translated();
  if (!translated) {
    try {
      translated = translator_->TranslateAnalyzedShader(*translation);
    } catch (const std::exception& e) {
      REXLOG_ERROR("[xenos_native] translation threw: {}", e.what());
    }
  }
  if (!translated) {
    REXLOG_ERROR("[xenos_native] {} shader {:016X} (modification {:016X}): translation failed",
                 vertex ? "vertex" : "pixel", shader.ucode_data_hash(), modification);
    return nullptr;
  }
  if (!translation->is_valid()) return nullptr;
  // The translator writes no OpSource, which DiligentCore requires (D18).
  const std::vector<uint8_t>& binary = translation->translated_binary();
  std::vector<uint32_t> spirv =
      spirv::PrepareForDiligent(reinterpret_cast<const uint32_t*>(binary.data()), binary.size() / 4);

  std::string name = std::string(vertex ? "VS " : "PS ") + std::to_string(shader.ucode_data_hash()) + " mod " +
                     std::to_string(modification);
  ShaderCreateInfo create_info;
  create_info.Desc.Name = name.c_str();
  create_info.Desc.ShaderType = vertex ? SHADER_TYPE_VERTEX : SHADER_TYPE_PIXEL;
  create_info.Desc.UseCombinedTextureSamplers = false;
  create_info.EntryPoint = "main";
  create_info.ByteCode = spirv.data();
  create_info.ByteCodeSize = spirv.size() * 4;
  device_->CreateShader(create_info, &host);
  if (!host) {
    REXLOG_ERROR("[xenos_native] {}: DiligentCore did not accept the SPIR-V ({} bytes)", name, spirv.size() * 4);
  }
  return host;
}

}  // namespace xenos_native
