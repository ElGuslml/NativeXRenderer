// NativeXRenderer - the tool modes (D129): a pipeline list made from stored pipelines (the developers'), and the
// preparation of the list's pipelines on a player's machine (the launcher's "Compiling shaders").
//
// The preparation does what the draw path does at a start with stored pipelines (D42, DrawPath::OpenStorage and
// PrepareStored), without the game and without a window: the list's records are rebuilt from the player's copy of the
// game and added to the player's storage, then every stored translation and pipeline is made, on all the cores, and
// DiligentCore's pipeline cache is saved. The game started next finds the driver's binaries in that cache and its
// own, and creates its pipelines in milliseconds.

#include <native_x_renderer/native_x_renderer.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "DataBlob.h"
#include "ThreadPool.h"
#include "PipelineStateCache.h"
#include "device.h"
#include "pipeline_list.h"
#include "pipelines.h"
#include "shaders.h"
#include "storage.h"
#include "targets.h"

REXCVAR_DEFINE_STRING(native_x_renderer_prepare_pipelines, "", "NativeXRenderer",
                      "Tool mode (D129): compile the pipeline list beside the executable and the stored pipelines for "
                      "this GPU and driver, write the progress to this file, and quit without starting the game (the "
                      "launcher's \"Compiling shaders\")")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_x_renderer_make_pipeline_list, "", "NativeXRenderer",
                      "Tool mode (D129): write a pipeline list to this file from the stored pipelines named by "
                      "native_x_renderer_pipeline_list_inputs, and quit without starting the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_x_renderer_pipeline_list_inputs, "", "NativeXRenderer",
                      "The stored pipelines (<TITLEID>.pipelines files) a pipeline list is made from, separated by '|' "
                      "(D129)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, native_x_renderer);
REXCVAR_DECLARE(bool, native_x_renderer_pipeline_storage);
REXCVAR_DECLARE(bool, native_x_renderer_scale_in_constants);
REXCVAR_DECLARE(std::string, native_x_renderer_antialiasing);

namespace native_x_renderer {

using namespace Diligent;
namespace rg = rex::graphics;

namespace {

// The progress file the launcher reads: one line, "<state> <done> <total>", replaced whole.
class ProgressFile {
 public:
  explicit ProgressFile(std::filesystem::path path) : path_(std::move(path)) {}
  void Write(const char* state, uint64_t done, uint64_t total, const std::string& note = {}) {
    if (path_.empty()) return;
    std::filesystem::path temporary = path_;
    temporary += ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file << state << ' ' << done << ' ' << total;
      if (!note.empty()) file << ' ' << note;
      file << '\n';
    }
    std::error_code error;
    std::filesystem::rename(temporary, path_, error);
  }

 private:
  std::filesystem::path path_;
};

// "<size> <last write time>" of a file, as the launcher compares it; empty if it is missing.
std::string FileIdentity(const std::filesystem::path& path) {
  std::error_code error;
  uint64_t size = std::filesystem::file_size(path, error);
  if (error) return {};
  auto time = std::filesystem::last_write_time(path, error);
  if (error) return {};
  return fmt::format("{} {}", size, int64_t(time.time_since_epoch().count()));
}

std::filesystem::path StorageBase(const Profile& profile, const std::filesystem::path& cache_root) {
  return cache_root / "native_x_renderer" / fmt::format("{:08X}", profile.title_id);
}

std::filesystem::path ListPath(const Profile& profile) {
  return rex::filesystem::GetExecutableFolder() / fmt::format("{:08X}.pipelines.list", profile.title_id);
}

std::filesystem::path With(std::filesystem::path base, const char* extension) {
  base += extension;
  return base;
}

int MakeList(const Profile& profile, const std::filesystem::path& game_data_root) {
  std::vector<std::filesystem::path> inputs;
  const std::string& joined = REXCVAR_GET(native_x_renderer_pipeline_list_inputs);
  for (size_t start = 0; start <= joined.size();) {
    size_t end = joined.find('|', start);
    if (end == std::string::npos) end = joined.size();
    if (end > start) inputs.push_back(rex::to_path(joined.substr(start, end - start)));
    start = end + 1;
  }
  const std::filesystem::path out = rex::to_path(REXCVAR_GET(native_x_renderer_make_pipeline_list));
  if (inputs.empty() || !profile.title_id || !profile.shader_file_count) {
    REXLOG_ERROR("[native_x_renderer] make_pipeline_list: no inputs, or the profile names no title or shader files");
    return 1;
  }
  GameShaders game;
  if (!game.Load(game_data_root, profile.shader_files, profile.shader_file_count)) {
    REXLOG_ERROR("[native_x_renderer] make_pipeline_list: none of the game's shader files could be read");
    return 1;
  }
  PipelineList list;
  MakeReport report;
  if (!MakePipelineList(game, profile.title_id, inputs, list, report)) return 1;
  if (!list.Write(out)) {
    REXLOG_ERROR("[native_x_renderer] make_pipeline_list: cannot write {}", out.string());
    return 1;
  }
  REXLOG_INFO("[native_x_renderer] make_pipeline_list: {} from {} storages: {} of {} shaders ({} pixel, {} vertex; {} "
              "not in the game's files, {} of them vertex shaders patched beyond their vertex layout), {} translations "
              "and {} pipelines ({} and {} left out with their shaders)",
              out.string(), report.storages, list.shaders.size(), report.shaders, report.pixel_found,
              report.vertex_found, report.not_found, report.other_fields, report.translations, report.pipelines,
              report.dropped_translations, report.dropped_pipelines);
  return 0;
}

int Prepare(const Profile& profile, const std::filesystem::path& game_data_root,
            const std::filesystem::path& cache_root) {
  ProgressFile progress(rex::to_path(REXCVAR_GET(native_x_renderer_prepare_pipelines)));
  auto fail = [&](const char* reason) {
    REXLOG_ERROR("[native_x_renderer] prepare_pipelines: {}", reason);
    progress.Write("failed", 0, 0, reason);
    return 1;
  };
  progress.Write("starting", 0, 0);
  if (!profile.title_id) return fail("the profile names no title");
  if (!REXCVAR_GET(native_x_renderer)) return fail("NativeXRenderer is off: the Xenos path draws");
  if (cache_root.empty() || !REXCVAR_GET(native_x_renderer_pipeline_storage)) return fail("no pipeline storage");
  // A translation with the render scale written into it is the window's (D128): nothing to prepare ahead.
  if (!REXCVAR_GET(native_x_renderer_scale_in_constants)) return fail("native_x_renderer_scale_in_constants is off");
  const auto start = std::chrono::steady_clock::now();

  // The list, resolved with the player's copy of the game.
  std::vector<PipelineStorage::Shader> list_shaders;
  std::vector<PipelineStorage::Translation> list_translations;
  std::vector<PipelineStorage::PipelineRecord> list_pipelines;
  const std::filesystem::path list_path = ListPath(profile);
  {
    PipelineList list;
    std::string error;
    if (!list.Read(list_path, error)) {
      REXLOG_INFO("[native_x_renderer] prepare_pipelines: no pipeline list ({}: {}); the stored pipelines only",
                  list_path.string(), error);
    } else if (list.title_id != profile.title_id) {
      REXLOG_WARN("[native_x_renderer] prepare_pipelines: {} is another title's ({:08X}); not used",
                  list_path.string(), list.title_id);
    } else {
      GameShaders game;
      if (!game.Load(game_data_root, profile.shader_files, profile.shader_file_count)) {
        REXLOG_WARN("[native_x_renderer] prepare_pipelines: none of the game's shader files could be read");
      } else {
        ResolveReport report;
        ResolvePipelineList(list, game, list_shaders, list_translations, list_pipelines, report);
        REXLOG_INFO("[native_x_renderer] prepare_pipelines: list {}: {} of {} shaders rebuilt from the game's files "
                    "({} not found, {} with another hash), {} translations, {} pipelines",
                    list_path.string(), report.shaders, list.shaders.size(), report.missing, report.mismatched,
                    report.translations, report.pipelines);
      }
    }
  }

  // Into the player's storage (D42), which keeps what it had.
  const std::filesystem::path base = StorageBase(profile, cache_root);
  const std::filesystem::path records = With(base, ".pipelines");
  {
    PipelineStorage storage;
    std::vector<PipelineStorage::Shader> s;
    std::vector<PipelineStorage::Translation> t;
    std::vector<PipelineStorage::PipelineRecord> p;
    if (!storage.Open(records, s, t, p)) return fail("cannot write the pipeline storage");
    for (const auto& shader : list_shaders) {
      storage.AddShader(shader.type, shader.hash, shader.ucode.data(), uint32_t(shader.ucode.size()));
    }
    for (const auto& translation : list_translations) {
      storage.AddTranslation(translation.type, translation.hash, translation.modification);
    }
    for (const auto& pipeline : list_pipelines) storage.AddPipeline(pipeline);
    storage.Flush();
  }
  std::vector<PipelineStorage::Shader> shaders;
  std::vector<PipelineStorage::Translation> translations;
  std::vector<PipelineStorage::PipelineRecord> pipelines;
  if (!PipelineStorage::Read(records, shaders, translations, pipelines)) return fail("cannot read the pipeline storage");

  // The device as the game creates it (its pipelines must be the game's), without a window, and every core.
  const unsigned cores = std::max(4u, std::thread::hardware_concurrency());
  const unsigned translators = std::clamp(cores / 6, 2u, 4u);
  Device device;
  const bool fsr = REXCVAR_GET(native_x_renderer_antialiasing).rfind("fsr", 0) == 0;
  if (!device.Create(nullptr, fsr, cores - translators)) return fail("no Vulkan device");
  IRenderDevice* render_device = device.device();
  ShaderCache shader_cache;
  if (!shader_cache.Initialize(render_device, RenderScale{}, true, translators)) return fail("no shader translator");
  PipelineCache pipeline_cache;
  pipeline_cache.Initialize(render_device, PipelineBuffers{}, device.pipeline_library());
  const std::filesystem::path vkcache = With(base, ".vkcache");
  RefCntAutoPtr<IPipelineStateCache> cache;
  {
    // Started empty (D132): the driver's cache only grows, and kept the pipelines of every earlier version (render passes
    // before D130, layouts before D131, the linked and the whole ones); made again here it holds what this version uses,
    // from the driver's own cache in about a second.
    std::error_code error;
    const uint64_t old_size = std::filesystem::file_size(vkcache, error);
    if (!error) REXLOG_INFO("[native_x_renderer] prepare_pipelines: the pipeline cache of {} KB made again", old_size >> 10);
    PipelineStateCacheCreateInfo cache_info;
    cache_info.Desc.Name = "NativeXRenderer pipeline cache";
    cache_info.Desc.Mode = PSO_CACHE_MODE_LOAD_STORE;
    render_device->CreatePipelineStateCache(cache_info, &cache);
  }
  if (cache) pipeline_cache.SetCache(cache);

  // As DrawPath::OpenStorage: the shaders analyzed, every translation queued, the pipelines once their shaders are.
  for (const auto& shader : shaders) {
    shader_cache.Get(rg::xenos::ShaderType(shader.type), shader.ucode.data(), uint32_t(shader.ucode.size()));
  }
  struct Translation {
    rg::SpirvShader* shader;
    uint64_t modification;
    bool done;
  };
  std::vector<Translation> pending_translations;
  std::set<std::pair<const rg::SpirvShader*, uint64_t>> queued;
  auto queue_translation = [&](rg::SpirvShader* shader, uint64_t modification) {
    if (queued.emplace(shader, modification).second) pending_translations.push_back({shader, modification, false});
  };
  for (const auto& t : translations) {
    if (rg::SpirvShader* shader = shader_cache.Find(rg::xenos::ShaderType(t.type), t.hash)) {
      queue_translation(shader, t.modification);
    }
  }
  struct Stored {
    rg::SpirvShader* vertex_shader;
    uint64_t vertex_modification;
    rg::SpirvShader* pixel_shader;
    uint64_t pixel_modification;
    PipelineState state;
    Pipeline* pipeline;
    bool done;
  };
  std::vector<Stored> stored;
  const bool depth_unorm24 = DepthUnorm24Supported(render_device);
  for (const auto& record : pipelines) {
    Stored s{shader_cache.Find(rg::xenos::ShaderType::kVertex, record.vertex_hash), record.vertex_modification,
             record.pixel_hash ? shader_cache.Find(rg::xenos::ShaderType::kPixel, record.pixel_hash) : nullptr,
             record.pixel_modification, record.state, nullptr, false};
    if (!s.vertex_shader || (record.pixel_hash && !s.pixel_shader)) continue;
    if (!depth_unorm24 && s.state.dsv_format == TEX_FORMAT_D24_UNORM_S8_UINT) {
      s.state.dsv_format = TEX_FORMAT_D32_FLOAT_S8X24_UINT;  // D103
    }
    queue_translation(s.vertex_shader, s.vertex_modification);
    if (s.pixel_shader) queue_translation(s.pixel_shader, s.pixel_modification);
    stored.push_back(s);
  }
  const uint64_t total = pending_translations.size() + stored.size();
  uint64_t done = 0, failed = 0;
  progress.Write("compiling", 0, total);
  auto last_write = std::chrono::steady_clock::now();
  while (done < total) {
    for (Translation& t : pending_translations) {
      if (t.done) continue;
      bool pending = false;
      shader_cache.GetHostShader(*t.shader, t.modification, pending);
      if (!pending) t.done = true, ++done;
    }
    for (Stored& s : stored) {
      if (s.done) continue;
      if (!s.pipeline) {
        bool vertex_pending = false, pixel_pending = false;
        IShader* vertex = shader_cache.GetHostShader(*s.vertex_shader, s.vertex_modification, vertex_pending);
        IShader* pixel = s.pixel_shader
                             ? shader_cache.GetHostShader(*s.pixel_shader, s.pixel_modification, pixel_pending)
                             : nullptr;
        if (vertex_pending || pixel_pending) continue;
        if (!vertex || (s.pixel_shader && !pixel)) {
          s.done = true, ++done, ++failed;  // a translation failed (logged by the shader cache)
          continue;
        }
        s.state.vertex_shader = vertex;
        s.state.pixel_shader = pixel;
        s.pipeline = pipeline_cache.Get(s.state, *s.vertex_shader, s.pixel_shader);
        if (!s.pipeline) {
          s.done = true, ++done, ++failed;
          continue;
        }
      }
      PIPELINE_STATE_STATUS status = s.pipeline->pso ? s.pipeline->pso->GetStatus() : PIPELINE_STATE_STATUS_FAILED;
      if (status == PIPELINE_STATE_STATUS_COMPILING) continue;
      if (status != PIPELINE_STATE_STATUS_READY) ++failed;
      s.done = true, ++done;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last_write > std::chrono::milliseconds(100)) {
      progress.Write("compiling", done, total);
      last_write = now;
    }
    if (done < total) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const double compile_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  // D132: the whole pipelines compiled behind the linked ones, so the cache saved has them.
  if (IThreadPool* pool = render_device->GetShaderCompilationThreadPool()) pool->WaitForAllTasks();
  const double whole_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  const std::array<uint32_t, 5> library = device.PipelineLibraryStats();
  // Nothing was drawn, but the device was set up through the context: finished before it goes.
  device.context()->Flush();
  render_device->IdleGPU();

  // DiligentCore's pipeline cache, as DrawPath::SavePipelineCache writes it.
  progress.Write("saving", done, total);
  uint64_t cache_bytes = 0;
  if (cache) {
    RefCntAutoPtr<IDataBlob> blob;
    cache->GetData(&blob);
    if (blob) {
      std::filesystem::path temporary = With(vkcache, ".tmp");
      bool saved = false;
      {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(blob->GetConstDataPtr()), std::streamsize(blob->GetSize()));
        saved = bool(file);
      }
      std::error_code error;
      if (saved) std::filesystem::rename(temporary, vkcache, error);
      if (saved && !error) cache_bytes = blob->GetSize();
    }
  }

  // The stamp: for which executable and libraries, list and driver this was done. The launcher compares it.
  {
    // The game's executable and the SDK's libraries beside it (rex*.dll: the shader translator is the SDK's).
    const std::filesystem::path exe = rex::filesystem::GetExecutablePath();
    std::ofstream stamp(With(base, ".prepared"), std::ios::binary | std::ios::trunc);
    stamp << "native_x_renderer prepared 1\n";
    std::vector<std::filesystem::path> files{exe};
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(exe.parent_path(), error)) {
      std::string lower = rex::path_to_utf8(entry.path().filename());
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (lower.rfind("rex", 0) == 0 && lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".dll") == 0) {
        files.push_back(entry.path());
      }
    }
    for (const auto& file : files) {
      std::string identity = FileIdentity(file);
      if (!identity.empty()) stamp << "file " << identity << ' ' << rex::path_to_utf8(file.filename()) << '\n';
    }
    std::string list_identity = FileIdentity(list_path);
    if (!list_identity.empty()) stamp << "file " << list_identity << ' ' << rex::path_to_utf8(list_path.filename()) << '\n';
    stamp << "driver " << device.DriverIdentity() << '\n';
  }
  progress.Write("done", done, total);
  REXLOG_INFO("[native_x_renderer] prepare_pipelines: {} translations and {} pipelines made in {:.0f} ms ({} failed) "
              "with {} threads; pipeline cache {} KB; {} pipelines with the resource signature, {} with their own "
              "layout (D131)",
              pending_translations.size(), stored.size(), compile_ms, failed, cores, cache_bytes >> 10,
              pipeline_cache.signature_count(), pipeline_cache.own_layout_count());
  if (device.pipeline_library()) {
    REXLOG_INFO("[native_x_renderer] prepare_pipelines: linked from parts (D132): {} parts made, {} reused, {} pipelines "
                "linked, {} made whole for want of parts; every whole pipeline compiled after {:.0f} ms",
                library[0], library[1], library[2], library[4], whole_ms);
  }
  return 0;
}

}  // namespace

bool ToolModeRequested() {
  return !REXCVAR_GET(native_x_renderer_prepare_pipelines).empty() ||
         !REXCVAR_GET(native_x_renderer_make_pipeline_list).empty();
}

int RunToolMode(const Profile& profile, const std::filesystem::path& game_data_root,
                const std::filesystem::path& cache_root) {
  if (!REXCVAR_GET(native_x_renderer_make_pipeline_list).empty()) return MakeList(profile, game_data_root);
  return Prepare(profile, game_data_root, cache_root);
}

}  // namespace native_x_renderer
