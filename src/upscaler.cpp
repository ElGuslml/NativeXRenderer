// XenosNative - temporal antialiasing and upscaling through NVIDIA DLSS (see upscaler.h).

#include "upscaler.h"

#include <rex/logging.h>

#include <string>
#include <string_view>
#include <vector>

#if XENOS_NATIVE_DLSS
#include "SuperResolution.h"
#include "SuperResolutionFactory.h"
#endif

namespace xenos_native {

using namespace Diligent;

Upscaler::Upscaler() = default;
Upscaler::~Upscaler() = default;

bool Upscaler::Initialize(IRenderDevice* device, uint32_t output_width, uint32_t output_height) {
#if XENOS_NATIVE_DLSS
  SuperResolutionFactoryCreateInfo create_info;
  create_info.pDevice = device;
  CreateSuperResolutionFactory(create_info, &factory_);
  if (!factory_) {
    REXLOG_WARN("[xenos_native] DLSS: DiligentCore's super resolution factory could not be created");
    return false;
  }
  Uint32 count = 0;
  factory_->EnumerateVariants(count, nullptr);
  std::vector<SuperResolutionInfo> variants(count);
  factory_->EnumerateVariants(count, variants.data());
  const SuperResolutionInfo* dlss = nullptr;
  for (const SuperResolutionInfo& variant : variants) {
    if (variant.Type == SUPER_RESOLUTION_TYPE_TEMPORAL && std::string_view(variant.Name).find("DLSS") != std::string_view::npos) {
      dlss = &variant;
    }
  }
  if (!dlss) {
    REXLOG_INFO("[xenos_native] DLSS: not available on this GPU or driver ({} other upscalers)", count);
    return false;
  }
  // The input sizes NGX recommends for each quality mode, for this output.
  static constexpr struct {
    SUPER_RESOLUTION_OPTIMIZATION_TYPE type;
    const char* name;
  } kModes[] = {{SUPER_RESOLUTION_OPTIMIZATION_TYPE_MAX_QUALITY, "ultra quality"},
                {SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_QUALITY, "quality"},
                {SUPER_RESOLUTION_OPTIMIZATION_TYPE_BALANCED, "balanced"},
                {SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_PERFORMANCE, "performance"},
                {SUPER_RESOLUTION_OPTIMIZATION_TYPE_MAX_PERFORMANCE, "ultra performance"}};
  std::string modes;
  for (const auto& mode : kModes) {
    SuperResolutionSourceSettingsAttribs attribs;
    attribs.VariantId = dlss->VariantId;
    attribs.OutputWidth = output_width;
    attribs.OutputHeight = output_height;
    attribs.OptimizationType = mode.type;
    SuperResolutionSourceSettings settings;
    factory_->GetSourceSettings(attribs, settings);
    if (!modes.empty()) modes += ", ";
    modes += fmt::format("{} {}x{}", mode.name, settings.OptimalInputWidth, settings.OptimalInputHeight);
  }
  REXLOG_INFO("[xenos_native] DLSS available ({}{}); for a {}x{} output: {}", dlss->Name,
              (dlss->TemporalCapFlags & SUPER_RESOLUTION_TEMPORAL_CAP_FLAG_NATIVE) ? ", native resolution (DLAA)" : "",
              output_width, output_height, modes);
  available_ = true;
  return true;
#else
  (void)device;
  (void)output_width;
  (void)output_height;
  REXLOG_INFO("[xenos_native] DLSS: not in this build");
  return false;
#endif
}

}  // namespace xenos_native
