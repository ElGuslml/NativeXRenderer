// NativeXRenderer - see device.h.

#include "device.h"

#include "machine.h"
#include "queries.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "EngineFactoryVk.h"
#include "CommandQueue.h"
#include "Fence.h"
#include "NativeWindow.h"
// DiligentCore's Vulkan interfaces take Vulkan's types from the includer (its functions are loaded by volk).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
#include "RenderDeviceVk.h"
#include "SwapChainVk.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(native_x_renderer_present_mode, "", "NativeXRenderer",
                      "How frames reach the screen (D106, D107): empty = MAILBOX (no tearing, no wait for the "
                      "display), else IMMEDIATE (tearing); \"vsync\" = FIFO, each frame shown once, never torn; "
                      "\"mailbox\" = MAILBOX, else FIFO; \"immediate\" = IMMEDIATE (tearing, or a variable refresh "
                      "rate display following the game), else MAILBOX. FIFO where the driver offers none of them");

REXCVAR_DEFINE_STRING(native_x_renderer_window_present_mode, "vsync", "NativeXRenderer",
                      "With V-Sync (native_x_renderer_present_mode \"vsync\") in a window that does not cover its "
                      "display (D182): \"vsync\" = FIFO as everywhere else; \"mailbox\" or \"immediate\" = that "
                      "mode there, and the display queue drain (D172) not acting. Such a window is on a display plane "
                      "or, with something animating the desktop behind it, composed by Windows, where FIFO held "
                      "frames back. Borderless and full screen keep FIFO");

REXCVAR_DEFINE_BOOL(native_x_renderer_dynamic_rendering, true, "NativeXRenderer",
                    "Draw with Vulkan's dynamic rendering (VK_KHR_dynamic_rendering, core in 1.3) where the device has "
                    "it, without render pass objects (D130): a pipeline names its targets' formats only, the first "
                    "step to pipelines linked from parts. False: DiligentCore's render passes, as before. Read at "
                    "startup");

REXCVAR_DEFINE_STRING(native_x_renderer_pipeline_library, "auto", "NativeXRenderer",
                      "Game pipelines linked from parts (Vulkan's graphics pipeline libraries, D132): a draw whose "
                      "pipeline is new gets one linked from parts in microseconds, and the whole pipeline, compiled "
                      "behind it, replaces it. auto = on an NVIDIA GPU (an AMD one lost its device with it in King Kong "
                      "Recompiled), on, off. Needs dynamic rendering (D130) and the resource signature (D131). Read at "
                      "startup")
    .allowed({"auto", "on", "off"});

REXCVAR_DECLARE(bool, native_x_renderer_gpu_passes);

namespace native_x_renderer {

using namespace Diligent;

namespace {

// DiligentCore's messages, in the game's log.
void DILIGENT_CALL_TYPE OnDiligentMessage(DEBUG_MESSAGE_SEVERITY severity, const Char* message, const Char* function,
                                          const Char* file, int line) {
  (void)file, (void)line;
  const char* text = message ? message : "";
  const char* where = function ? function : "";
  switch (severity) {
    case DEBUG_MESSAGE_SEVERITY_INFO:
      REXLOG_INFO("[native_x_renderer] DiligentCore: {}", text);
      break;
    case DEBUG_MESSAGE_SEVERITY_WARNING:
      REXLOG_WARN("[native_x_renderer] DiligentCore: {} ({})", text, where);
      break;
    default:
      REXLOG_ERROR("[native_x_renderer] DiligentCore: {} ({})", text, where);
      break;
  }
}

}  // namespace

namespace {

// Whether the GPU Vulkan picks has graphics pipeline libraries with fast linking (D132), and an NVIDIA one when
// `nvidia_only`; asked through the Vulkan loader before DiligentCore creates its device (an extension it does not have
// would fail the device). `reason` says why not.
bool PipelineLibrarySupported(bool nvidia_only, uint32_t chosen_vendor, uint32_t chosen_device, std::string& reason) {
#ifdef _WIN32
  HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
  if (!loader) {
    reason = "no Vulkan loader";
    return false;
  }
  auto get = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(loader, name)); };
  auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get("vkCreateInstance"));
  auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get("vkDestroyInstance"));
  auto enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get("vkEnumeratePhysicalDevices"));
  auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get("vkGetPhysicalDeviceProperties"));
  auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get("vkGetPhysicalDeviceProperties2"));
  auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get("vkGetPhysicalDeviceFeatures2"));
  auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get("vkEnumerateDeviceExtensionProperties"));
  if (!create_instance || !destroy_instance || !enumerate_devices || !properties || !properties2 || !features2 ||
      !extensions) {
    reason = "an old Vulkan loader";
    return false;
  }
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &application;
  VkInstance instance = VK_NULL_HANDLE;
  if (create_instance(&info, nullptr, &instance) != VK_SUCCESS) {
    reason = "no Vulkan 1.3 instance";
    return false;
  }
  bool supported = false;
  reason = nvidia_only ? "no NVIDIA GPU" : "no GPU";
  uint32_t count = 0;
  enumerate_devices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  if (count) enumerate_devices(instance, &count, devices.data());
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties p{};
    properties(device, &p);
    if (chosen_vendor) {
      if (p.vendorID != chosen_vendor || p.deviceID != chosen_device) continue;  // the player's GPU (D141)
    } else if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && devices.size() > 1) {
      continue;
    }
    if (nvidia_only && p.vendorID != 0x10DE) continue;
    if (p.apiVersion < VK_API_VERSION_1_3) {
      reason = "Vulkan 1.3 missing";
      continue;
    }
    uint32_t extension_count = 0;
    extensions(device, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> list(extension_count);
    if (extension_count) extensions(device, nullptr, &extension_count, list.data());
    bool library = false, graphics_library = false;
    for (const VkExtensionProperties& e : list) {
      library |= std::strcmp(e.extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) == 0;
      graphics_library |= std::strcmp(e.extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) == 0;
    }
    if (!library || !graphics_library) {
      reason = "the driver has no graphics pipeline libraries";
      continue;
    }
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 all_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f};
    features2(device, &all_features);
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT lp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 all_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &lp};
    properties2(device, &all_properties);
    if (!f.graphicsPipelineLibrary || !lp.graphicsPipelineLibraryFastLinking) {
      reason = "the driver's graphics pipeline libraries have no fast linking";
      continue;
    }
    supported = true;
    break;
  }
  destroy_instance(instance, nullptr);
  return supported;
#else
  (void)nvidia_only;
  reason = "not on this system";
  return false;
#endif
}

// D181: the machine, for a player's log to answer what it ran on: the processor and memory (machine.h), the GPU's
// driver and memory heaps, and the device limits and formats that a decision assumes (a single 512 MB storage buffer,
// the dynamic uniform buffers and sampled images a draw binds, sparse residency for the SDK's own shared memory, 16-bit
// SNORM render targets).
void LogMachine(VkPhysicalDevice physical) {
  REXLOG_INFO("[native_x_renderer] machine (D181): {}", machine::Describe());
#ifdef _WIN32
  HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
  if (!loader || !physical) return;
  auto get_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(GetProcAddress(loader, "vkGetPhysicalDeviceProperties"));
  auto get_properties2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(GetProcAddress(loader, "vkGetPhysicalDeviceProperties2"));
  auto get_memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      GetProcAddress(loader, "vkGetPhysicalDeviceMemoryProperties"));
  auto get_features =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(GetProcAddress(loader, "vkGetPhysicalDeviceFeatures"));
  auto get_format = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      GetProcAddress(loader, "vkGetPhysicalDeviceFormatProperties"));
  if (!get_properties || !get_memory || !get_features || !get_format) return;
  VkPhysicalDeviceProperties properties{};
  get_properties(physical, &properties);
  // The driver's own version string (Vulkan 1.2's driver properties): "566.36" where driverVersion packs it its way.
  std::string driver = fmt::format("driver version {:#x}", properties.driverVersion);
  if (get_properties2 && properties.apiVersion >= VK_API_VERSION_1_2) {
    VkPhysicalDeviceDriverProperties driver_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver_properties};
    get_properties2(physical, &properties2);
    driver = fmt::format("driver {} {} ({:#x})", driver_properties.driverName, driver_properties.driverInfo,
                         properties.driverVersion);
  }
  VkPhysicalDeviceMemoryProperties memory{};
  get_memory(physical, &memory);
  std::string heaps;
  for (uint32_t h = 0; h < memory.memoryHeapCount; ++h) {
    bool host_visible = false;
    for (uint32_t t = 0; t < memory.memoryTypeCount; ++t) {
      if (memory.memoryTypes[t].heapIndex == h &&
          (memory.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        host_visible = true;
      }
    }
    const bool local = memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    heaps += fmt::format("{}{} MB {}{}", h ? ", " : "", memory.memoryHeaps[h].size >> 20,
                         local ? "device-local" : "system", local && host_visible ? " (host-visible)" : "");
  }
  VkPhysicalDeviceFeatures features{};
  get_features(physical, &features);
  auto target = [&](VkFormat format) {
    VkFormatProperties format_properties{};
    get_format(physical, format, &format_properties);
    const VkFormatFeatureFlags f = format_properties.optimalTilingFeatures;
    return !(f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)       ? "no"
           : (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) ? "yes, blended"
                                                                : "yes, not blended";
  };
  const VkPhysicalDeviceLimits& limits = properties.limits;
  REXLOG_INFO("[native_x_renderer] GPU (D181): {} ({:04X}:{:04X}), Vulkan {}.{}.{}, {}; memory heaps: {}; a storage "
              "buffer up to {} MB, {} sampled images a stage, {} dynamic uniform buffers a set; sparse residency of "
              "buffers {}; render targets of R16G16_SNORM {}, of R16G16B16A16_SNORM {}",
              properties.deviceName, properties.vendorID, properties.deviceID, VK_API_VERSION_MAJOR(properties.apiVersion),
              VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion), driver, heaps,
              limits.maxStorageBufferRange >> 20, limits.maxPerStageDescriptorSampledImages,
              limits.maxDescriptorSetUniformBuffersDynamic,
              features.sparseBinding && features.sparseResidencyBuffer ? "yes" : "no",
              target(VK_FORMAT_R16G16_SNORM), target(VK_FORMAT_R16G16B16A16_SNORM));
#else
  (void)physical;
#endif
}

}  // namespace

std::array<uint32_t, 5> Device::PipelineLibraryStats() const {
  std::array<uint32_t, 5> counts{};
  RefCntAutoPtr<IRenderDeviceVk> vk(device_, IID_RenderDeviceVk);
  if (vk) vk->GetPipelineLibraryStats(counts.data());
  return counts;
}

bool Device::Create(void* native_window, bool half_precision, uint32_t compile_threads) {
  window_ = native_window;
  IEngineFactoryVk* factory = GetEngineFactoryVk();
  if (!factory) {
    REXLOG_ERROR("[native_x_renderer] no DiligentCore Vulkan factory");
    return false;
  }
  factory->SetMessageCallback(OnDiligentMessage);

  EngineVkCreateInfo create_info;
  create_info.EnableValidation = false;
  // D141: the player's GPU (the SDK's gpu_adapter, "VVVV:DDDD", its PCI IDs; read by name, so an SDK without it
  // leaves DiligentCore's choice: discrete, then integrated).
  uint32_t chosen_vendor = 0, chosen_device = 0;
  {
    const std::string chosen = rex::cvar::Query<std::string>("gpu_adapter");
    unsigned vendor = 0, device = 0;
    char rest = 0;
    if (!chosen.empty() && std::sscanf(chosen.c_str(), "%x:%x%c", &vendor, &device, &rest) == 2 && vendor) {
      Uint32 count = 0;
      factory->EnumerateAdapters(Version{}, count, nullptr);
      std::vector<GraphicsAdapterInfo> adapters(count);
      if (count) factory->EnumerateAdapters(Version{}, count, adapters.data());
      for (Uint32 i = 0; i < count; ++i) {
        if (adapters[i].VendorId == vendor && adapters[i].DeviceId == device) {
          create_info.AdapterId = i;
          chosen_vendor = vendor;
          chosen_device = device;
          REXLOG_INFO("[native_x_renderer] gpu_adapter {:04X}:{:04X}: {}", vendor, device, adapters[i].Description);
          break;
        }
      }
      if (!chosen_vendor) {
        REXLOG_WARN("[native_x_renderer] gpu_adapter {:04X}:{:04X} not found among the Vulkan devices: the default one",
                    vendor, device);
      }
    }
  }
  // What guest draws need: independent blending per render target, depth clamp (PA_CL_CLIP_CNTL
  // clip_disable), wireframe (polygon modes), BC textures. Optional ones are used when present.
  create_info.Features.IndependentBlend = DEVICE_FEATURE_STATE_ENABLED;
  create_info.Features.DepthClamp = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.DepthBiasClamp = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.WireframeFill = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.TextureCompressionBC = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.OcclusionQueries = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.QueryPoolSizes[QUERY_TYPE_OCCLUSION] = OcclusionQueries::kPoolSize;  // the guest's (D62)
  create_info.Features.DurationQueries = DEVICE_FEATURE_STATE_OPTIONAL;  // GPU time per frame, for the log
  // GPU time per pass, for measuring (renderer.cpp's PassTimer): up to 384 timestamps a frame, 4 frames read late.
  if (REXCVAR_GET(native_x_renderer_gpu_passes)) {
    create_info.Features.TimestampQueries = DEVICE_FEATURE_STATE_OPTIONAL;
    create_info.QueryPoolSizes[QUERY_TYPE_TIMESTAMP] = 4096;
  }
  // Pipelines compile in DiligentCore's thread pool (D28). A few threads: new pipelines come in bursts
  // (a level, a new effect), and the game's own threads need the CPU.
  create_info.Features.AsyncShaderCompilation = DEVICE_FEATURE_STATE_OPTIONAL;
  if (half_precision) create_info.Features.ShaderFloat16 = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.NumAsyncShaderCompilationThreads = compile_threads ? compile_threads : 4;
  // D130: no render pass or framebuffer objects; DiligentCore begins and ends rendering itself (vkCmdBeginRendering).
  if (REXCVAR_GET(native_x_renderer_dynamic_rendering)) {
    create_info.FeaturesVk.DynamicRendering = DEVICE_FEATURE_STATE_OPTIONAL;
  }
  // D132: the two extensions and the feature, asked for only where the GPU has them (DiligentCore fails a device whose
  // extension is missing).
  static const char* const kLibraryExtensions[] = {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
                                                   VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME};
  VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT library_features{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
  library_features.graphicsPipelineLibrary = VK_TRUE;
  const std::string& library_mode = REXCVAR_GET(native_x_renderer_pipeline_library);
  std::string library_reason = "native_x_renderer_pipeline_library off";
  bool library = false;
  if (library_mode != "off") {
    if (!REXCVAR_GET(native_x_renderer_dynamic_rendering)) {
      library_reason = "dynamic rendering off (D130)";
    } else {
      library = PipelineLibrarySupported(library_mode == "auto", chosen_vendor, chosen_device, library_reason);
    }
  }
  if (library) {
    create_info.DeviceExtensionCount = uint32_t(std::size(kLibraryExtensions));
    create_info.ppDeviceExtensionNames = kLibraryExtensions;
    create_info.pDeviceExtensionFeatures = &library_features;
  }
  // Every draw maps its constant buffers from the dynamic heap (system, bool/loop and fetch constants:
  // 1.4 KB, plus the float constants its shaders use), with several frames in flight.
  create_info.DynamicHeapSize = 256 << 20;
  create_info.DynamicHeapPageSize = 1 << 20;
  IDeviceContext* contexts[1] = {};
  factory->CreateDeviceAndContextsVk(create_info, &device_, contexts);
  context_.Attach(contexts[0]);
  queue_ = std::make_unique<RecordQueue>(context_);
  if (!device_ || !context_) {
    REXLOG_ERROR("[native_x_renderer] could not create the Vulkan device");
    return false;
  }

  FenceDesc fence_desc;
  fence_desc.Name = "NativeXRenderer frames";
  fence_desc.Type = FENCE_TYPE_CPU_WAIT_ONLY;
  device_->CreateFence(fence_desc, &frame_fence_);
  if (!frame_fence_) {
    REXLOG_ERROR("[native_x_renderer] could not create the frame fence");
    return false;
  }

  {
    RefCntAutoPtr<IRenderDeviceVk> vk(device_, IID_RenderDeviceVk);
    DeviceFeaturesVk features_vk;
    if (vk) vk->GetDeviceFeaturesVk(features_vk);
    dynamic_rendering_ = features_vk.DynamicRendering == DEVICE_FEATURE_STATE_ENABLED;
    pipeline_library_ = library && dynamic_rendering_;
    if (library && !dynamic_rendering_) library_reason = "the device has no dynamic rendering (D130)";
    REXLOG_INFO("[native_x_renderer] game pipelines linked from parts (graphics pipeline libraries, D132): {}",
                pipeline_library_ ? std::string("on") : "off: " + library_reason);
    REXLOG_INFO("[native_x_renderer] drawing with {} (D130)",
                dynamic_rendering_ ? "dynamic rendering"
                : REXCVAR_GET(native_x_renderer_dynamic_rendering) ? "render passes: the device has no dynamic rendering"
                                                                   : "render passes (native_x_renderer_dynamic_rendering off)");
#ifdef _WIN32
    // D134: the draws' viewports are kept within it (draw.cpp).
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    auto get_properties = loader ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                                       GetProcAddress(loader, "vkGetPhysicalDeviceProperties"))
                                 : nullptr;
    if (vk && get_properties) {
      VkPhysicalDeviceProperties properties{};
      get_properties(vk->GetVkPhysicalDevice(), &properties);
      for (int i = 0; i < 2; ++i) {
        if (properties.limits.maxViewportDimensions[i] >= 4096) {  // Vulkan's required minimum
          max_viewport_[i] = properties.limits.maxViewportDimensions[i];
        }
      }
    }
#endif
    REXLOG_INFO("[native_x_renderer] viewports up to {}x{} host pixels (D134)", max_viewport_[0], max_viewport_[1]);
    if (vk) LogMachine(vk->GetVkPhysicalDevice());
  }
  if (!native_window) {
    REXLOG_INFO("[native_x_renderer] Vulkan device on {}, without a window", device_->GetAdapterInfo().Description);
    return true;
  }

  SwapChainDesc swap_chain_desc;
  // The guest's front buffer is 8-bit RGBA, already gamma-corrected by the game: no sRGB conversion
  // on the way out.
  swap_chain_desc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM;
  swap_chain_desc.DepthBufferFormat = TEX_FORMAT_UNKNOWN;
  swap_chain_desc.BufferCount = 3;
#ifdef _WIN32
  Win32NativeWindow window{static_cast<HWND>(native_window)};
#else
  NativeWindow window{};
#endif
  factory->CreateSwapChainVk(device_, context_, swap_chain_desc, window, &swap_chain_);
  if (!swap_chain_) {
    REXLOG_ERROR("[native_x_renderer] could not create the swap chain on the game window");
    return false;
  }

  const auto& adapter = device_->GetAdapterInfo();
  const auto& desc = swap_chain_->GetDesc();
  REXLOG_INFO("[native_x_renderer] Vulkan device on {}; swap chain {}x{}", adapter.Description, desc.Width,
              desc.Height);
  return true;
}

bool SampleRateShadingEnabled(IRenderDevice* device) {
#ifdef _WIN32
  RefCntAutoPtr<IRenderDeviceVk> vk(device, IID_RenderDeviceVk);
  HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
  auto get_features = loader ? reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
                                   GetProcAddress(loader, "vkGetPhysicalDeviceFeatures"))
                             : nullptr;
  if (!vk || !get_features) return false;
  VkPhysicalDeviceFeatures features{};
  get_features(vk->GetVkPhysicalDevice(), &features);
  return features.sampleRateShading == VK_TRUE;
#else
  (void)device;
  return false;
#endif
}

std::string Device::DriverIdentity() const {
#ifdef _WIN32
  RefCntAutoPtr<IRenderDeviceVk> vk(device_, IID_RenderDeviceVk);
  HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
  auto get_properties = loader ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                                     GetProcAddress(loader, "vkGetPhysicalDeviceProperties"))
                               : nullptr;
  if (!vk || !get_properties) return {};
  VkPhysicalDeviceProperties properties{};
  get_properties(vk->GetVkPhysicalDevice(), &properties);
  char text[96];
  int length = std::snprintf(text, sizeof(text), "%04x %04x %08x ", properties.vendorID, properties.deviceID,
                             properties.driverVersion);
  std::string identity(text, size_t(std::max(length, 0)));
  for (uint8_t b : properties.pipelineCacheUUID) {
    std::snprintf(text, sizeof(text), "%02x", b);
    identity += text;
  }
  return identity;
#else
  return {};
#endif
}

bool Device::SizeUnchanged() const {
#ifdef _WIN32
  RECT rect{};
  if (!GetClientRect(static_cast<HWND>(window_), &rect)) return false;
  const uint32_t width = uint32_t(rect.right - rect.left), height = uint32_t(rect.bottom - rect.top);
  const auto& desc = swap_chain_->GetDesc();
  return width && height && desc.Width == width && desc.Height == height;
#else
  return true;
#endif
}

bool Device::UpdateSize() {
  if (queue_) queue_->Flush();
#ifdef _WIN32
  RECT rect{};
  if (!GetClientRect(static_cast<HWND>(window_), &rect)) return false;
  uint32_t width = uint32_t(rect.right - rect.left), height = uint32_t(rect.bottom - rect.top);
#else
  uint32_t width = swap_chain_->GetDesc().Width, height = swap_chain_->GetDesc().Height;
#endif
  if (!width || !height) return false;
  const auto& desc = swap_chain_->GetDesc();
  if (desc.Width != width || desc.Height != height) swap_chain_->Resize(width, height);
  return true;
}

Device::FenceState Device::GetFenceState() {
  if (queue_) queue_->Flush();
  FenceState state{frame_.load(), frame_fence_->GetCompletedValue(), 0, 0};
  if (ICommandQueue* queue = context_->LockCommandQueue()) {
    state.queue_next = queue->GetNextFenceValue();
    state.queue_completed = queue->GetCompletedFenceValue();
    context_->UnlockCommandQueue();
  }
  return state;
}

bool Device::WindowCoversDisplay() {
#ifdef _WIN32
  HWND hwnd = static_cast<HWND>(window_);
  RECT client{};
  if (!hwnd || !GetClientRect(hwnd, &client)) return true;
  POINT top_left{client.left, client.top}, bottom_right{client.right, client.bottom};
  if (!ClientToScreen(hwnd, &top_left) || !ClientToScreen(hwnd, &bottom_right)) return true;
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return true;
  return top_left.x <= info.rcMonitor.left && top_left.y <= info.rcMonitor.top &&
         bottom_right.x >= info.rcMonitor.right && bottom_right.y >= info.rcMonitor.bottom;
#else
  return true;
#endif
}

void Device::ApplyPresentMode() {
  std::string mode = force_vsync_ ? std::string("vsync") : REXCVAR_GET(native_x_renderer_present_mode);
  // D182: V-Sync in a window that does not cover its display. Such a window is on a display plane, or composed by
  // Windows when something animates the desktop behind it (PresentMon: "Composed: Flip"); the compositor already shows
  // one whole picture a refresh, and FIFO in front of it had a second stable phase, one image held a refresh longer
  // (frames presented - completed 2, a wait for an image, frames missed: GH Warriors of Rock's local_docs/74, 2.1). On
  // a plane MAILBOX showed every frame too. native_x_renderer_window_present_mode chooses the mode there;
  // the window is looked at again every kCoverCheck presents, so a resize or borderless on and off is followed.
  // D172 acts on FIFO only (fifo()), so not there.
  bool in_window = false;
  if (mode == "vsync" && !force_vsync_) {
    if (--cover_check_ <= 0) {
      covers_display_ = WindowCoversDisplay();
      cover_check_ = kCoverCheck;
    }
    const std::string& window_mode = REXCVAR_GET(native_x_renderer_window_present_mode);
    if (!covers_display_ && (window_mode == "mailbox" || window_mode == "immediate")) {
      mode = window_mode;
      in_window = true;
    }
  }
  if (present_mode_applied_ && mode == present_mode_) return;
  present_mode_applied_ = true;
  present_mode_ = mode;
  if (in_window) {
    REXLOG_INFO("[native_x_renderer] V-Sync in a window that does not cover its display (D182): presented with {} "
                "(native_x_renderer_window_present_mode); the display queue drain (D172) does not act there",
                mode);
  }
  RefCntAutoPtr<ISwapChainVk> swap_chain_vk(swap_chain_, IID_SwapChainVk);
  if (!swap_chain_vk) return;
  // D107: V-Sync is FIFO, which every driver offers. NVIDIA's MAILBOX queued three frames whenever the game ran at the
  // display's rate (16-19 ms from present to display at 160 Hz, FIFO 5.8 ms).
  static constexpr VkPresentModeKHR kVsync[] = {VK_PRESENT_MODE_FIFO_KHR};
  static constexpr VkPresentModeKHR kMailbox[] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
  static constexpr VkPresentModeKHR kImmediate[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                                    VK_PRESENT_MODE_FIFO_KHR};
  if (mode == "vsync") {
    swap_chain_vk->SetPresentModes(kVsync, Uint32(std::size(kVsync)));
  } else if (mode == "mailbox") {
    swap_chain_vk->SetPresentModes(kMailbox, Uint32(std::size(kMailbox)));
  } else if (mode == "immediate") {
    swap_chain_vk->SetPresentModes(kImmediate, Uint32(std::size(kImmediate)));
  } else {
    if (!mode.empty()) {
      REXLOG_WARN("[native_x_renderer] native_x_renderer_present_mode \"{}\" is not one: the default", mode);
    }
    swap_chain_vk->SetPresentModes(nullptr, 0);
  }
  REXLOG_INFO("[native_x_renderer] present mode: {}", mode == "vsync"       ? "vsync (FIFO)"
                                                      : mode == "mailbox"   ? "mailbox (MAILBOX, else FIFO)"
                                                      : mode == "immediate" ? "immediate (IMMEDIATE, else MAILBOX)"
                                                                            : "default (MAILBOX, else IMMEDIATE)");
}

double Device::Present(bool wait, bool show) {
  queue_->Flush();
  ApplyPresentMode();
  context_->EnqueueSignal(frame_fence_, ++frame_);
  if (show) {
    swap_chain_->Present(0);
  } else {
    // D172: what the swap chain's Present does besides presenting. No image was acquired for this frame (its back buffer
    // was never asked for), so the swap chain's state is as after the last Present.
    context_->Flush();
    context_->FinishFrame();
    device_->ReleaseStaleResources();
  }
  return wait ? WaitForFramesInFlight() : 0.0;
}

double Device::WaitForExpectedFrames() {
  const uint64_t expected = expected_.load(std::memory_order_relaxed);
  if (expected <= kMaxFramesInFlight) return 0.0;
  auto start = std::chrono::steady_clock::now();
  frame_fence_->Wait(expected - kMaxFramesInFlight);
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

double Device::FinishHiddenFrame() {
  queue_->Flush();
  context_->EnqueueSignal(frame_fence_, ++frame_);
  context_->Flush();
  context_->FinishFrame();
  device_->ReleaseStaleResources();
  return WaitForFramesInFlight();
}

double Device::WaitForFramesInFlight() {
  if (frame_ <= kMaxFramesInFlight) return 0.0;
  auto start = std::chrono::steady_clock::now();
  frame_fence_->Wait(frame_ - kMaxFramesInFlight);
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace native_x_renderer
