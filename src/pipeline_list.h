// NativeXRenderer - the pipeline list shipped beside a game, and the game's own shaders it is resolved with (internal,
// D129).
//
// The storage (D42) learns a player's shaders and pipelines as the game draws them, and prepares them at the next
// start: a first run, and a first visit anywhere, still compiles at its draws. A pipeline list is D42's records of
// earlier play, shipped with the game, without any of the game's code: a shader is named by the XXH3 of its
// microcode, as D42 keys it, and its microcode comes from the player's copy of the game, the files where the game
// keeps its compiled shaders (XDK shader containers: GH Metallica's DATA/FXFILES/MaterialLibrary.bin.xen).
//
// A pixel shader is loaded as the file has it. A vertex shader is loaded with its vertex fetches patched by the D3D
// library for the vertex declaration it draws with (the format, offset and stride of each vfetch), and in some games
// with its exports patched for the pixel shader it draws with (D136): the list carries the hash of the program as the
// file has it, and the new values of those instructions' fields, in the Xenos ISA's terms (the SDK's ucode.h).
// Resolving a record rebuilds the microcode and checks its hash.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "storage.h"

namespace native_x_renderer {

struct Profile;

// The compiled shaders in a game's files: XDK shader containers, found by their magic (0x102A, the compiler's version
// byte, then 0 pixel or 1 vertex: 0x102A1100 / 0x102A1101 in the Guitar Hero games, 0x102A0E00 / 01 in GRAW's
// NewYetiShadersX360.sdf), each with its microcode section (dword 1: the header's size, the section's start; dword 2:
// its size in bytes). A Neversoft CHNK file (GH World Tour's MaterialLibrary.bin.xen) is decompressed first; a game's
// own reader gives the buffers of archives NativeXRenderer cannot read (Profile::read_shader_archives, D136). The
// program a draw loads is the end of that section (GH Metallica: the section's start, or 64 bytes of literal
// constants after it), so a program is looked up by its length and hash among the tails of every section of that
// length or longer. The sections are kept once each (a level's archives repeat the shaders the levels share).
class GameShaders {
 public:
  // The profile's shader files (missing ones are logged and skipped) and its archive reader. False if no container was
  // found.
  bool Load(const std::filesystem::path& root, const Profile& profile);
  // The containers in a buffer: their sections kept, those not kept before. Returns how many containers it has.
  size_t Scan(const uint8_t* data, size_t size);
  size_t container_count() const { return containers_.size(); }

  // The program of `type` (0 vertex, 1 pixel) whose `dwords` big-endian dwords hash to `hash`; null if none.
  const uint32_t* Find(uint8_t type, uint32_t dwords, uint64_t hash);
  // Every program of `type` and length `dwords` (the tails of the sections at least that long).
  std::vector<const uint32_t*> Candidates(uint8_t type, uint32_t dwords) const;

 private:
  struct Container {
    uint8_t type;
    std::vector<uint32_t> section;  // big-endian dwords
  };
  std::vector<Container> containers_;
  std::unordered_set<uint64_t> kept_;  // the sections' hashes, with their type
  // By type and length: the tails' hashes (built on first use).
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, const uint32_t*>> index_;
};

// An instruction's fields as the D3D library sets them (D129, D136): a vertex fetch's for the vertex declaration a
// shader draws with (kFetchFields, pipeline_list.cpp), or an ALU instruction's that exports, for the pixel shader it
// draws with (kAluFields). `fields` has a bit per entry of that table, `values` the new value of each set field, in
// their order.
struct InstructionPatch {
  enum class Kind : uint8_t { kVertexFetch, kAluExport };
  Kind kind = Kind::kVertexFetch;
  uint16_t slot = 0;    // the instruction's slot: its dwords are 3 * slot .. 3 * slot + 2
  uint32_t fields = 0;  // the fields changed
  std::vector<uint32_t> values;
};

struct ListShader {
  uint8_t type;
  uint32_t dwords;
  uint64_t hash;         // of the program as the game loads it (D42's key)
  uint64_t source_hash;  // of the program as the game's files have it (the hash for a pixel shader)
  std::vector<InstructionPatch> patches;
};

struct PipelineList {
  uint32_t title_id = 0;
  std::vector<ListShader> shaders;
  std::vector<PipelineStorage::Translation> translations;
  std::vector<PipelineStorage::PipelineRecord> pipelines;

  bool Read(const std::filesystem::path& path, std::string& error);
  bool Write(const std::filesystem::path& path) const;
};

// The slots of a program's vertex fetch instructions and of its ALU instructions, from its control flow (big-endian
// dwords). Either may be null.
void ExecSlots(const uint32_t* ucode, uint32_t dwords, std::vector<uint32_t>* vertex_fetch, std::vector<uint32_t>* alu);

// What the list maker found (logged by the tool).
struct MakeReport {
  uint32_t storages = 0, shaders = 0, pixel_found = 0, vertex_found = 0;
  uint32_t vertex_linked = 0;  // of them, those whose exports the D3D library patched for their pixel shader (D136)
  uint32_t not_found = 0;      // shaders not in the game's files
  uint32_t other_fields = 0;   // vertex shaders whose patch changes something besides the vertex layout and exports
  uint32_t translations = 0, pipelines = 0, dropped_translations = 0, dropped_pipelines = 0;
};

// The list of the records in `storages` (D42 files, read without being changed) whose shaders the game's files have.
bool MakePipelineList(GameShaders& game, uint32_t title_id, const std::vector<std::filesystem::path>& storages,
                      PipelineList& list, MakeReport& report);

// The list's records with their microcode rebuilt from the game's files, as D42 stores them. Shaders that cannot be
// rebuilt (not in these files, or a hash that does not match) are left out with their translations and pipelines.
struct ResolveReport {
  uint32_t shaders = 0, missing = 0, mismatched = 0, translations = 0, pipelines = 0;
};
void ResolvePipelineList(const PipelineList& list, GameShaders& game, std::vector<PipelineStorage::Shader>& shaders,
                         std::vector<PipelineStorage::Translation>& translations,
                         std::vector<PipelineStorage::PipelineRecord>& pipelines, ResolveReport& report);

}  // namespace native_x_renderer
