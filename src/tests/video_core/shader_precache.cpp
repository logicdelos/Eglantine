// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "shader_recompiler/exception.h"
#include "core/core.h"
#include "video_core/cache_envelope.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"

namespace {

namespace ShaderWarm = VideoCommon::ShaderWarm;
namespace CE = VideoCommon::CacheEnvelope;

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-shader-precache-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void WriteBytes(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

CE::HostIdentity TestHost() {
    CE::HostIdentity host;
    host.build_id = 0xb01d;
    host.title_id = 0x0100aabbccddeeffULL;
    host.game_version = 3;
    host.content_stamp = 0xc0ffee;
    return host;
}

/// Sets the host identity the caches and the precache file are stamped with, and clears it again.
struct HostGuard {
    explicit HostGuard(const CE::HostIdentity& host) {
        CE::SetHostIdentity(host);
    }
    ~HostGuard() {
        CE::SetHostIdentity(CE::HostIdentity{});
    }
    HostGuard(const HostGuard&) = delete;
    HostGuard& operator=(const HostGuard&) = delete;
};

std::vector<u8> OneComputeEntry() {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    ShaderWarm::ComputeKey key{};
    return ShaderWarm::ComputeEntry(lowered, {{1, 1, 1}, 0, 0}, key);
}

/// A precache file as the builder leaves it: one compute record in an envelope for `identity`,
/// and the count file with `builder` as the builder version.
void WriteWarmFile(const std::filesystem::path& warm, const CE::CacheIdentity& identity,
                   u32 builder = ShaderWarm::WarmBuilderVersion) {
    auto writer = CE::EnvelopeWriter::Create(warm, identity, CE::MaxWarmBytes);
    REQUIRE(writer);
    CE::RecordMeta meta;
    meta.kind = CE::RecordKind::Compute;
    REQUIRE(writer->Append(meta, OneComputeEntry()) == CE::AppendResult::Ok);
    REQUIRE(writer->Commit());
    WriteBytes(ShaderWarm::WarmCountPath(warm),
               fmt::format("1 {} {}\n", std::filesystem::file_size(warm), builder));
}

} // Anonymous namespace

TEST_CASE("ShaderPrecache: the marker matches only the same file and driver", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    REQUIRE_FALSE(ShaderWarm::CurrentWarmStamp(warm, 0, 1, 2, 3).has_value());
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());

    WriteBytes(warm, "precache");
    const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53);
    REQUIRE(stamp.has_value());
    REQUIRE(stamp->size == 8);
    REQUIRE(ShaderWarm::WriteWarmMarker(warm, *stamp));
    REQUIRE(ShaderWarm::WarmMarkerPath(warm) == dir.path / "warm-compute.done");

    const auto marker = ShaderWarm::ReadWarmMarker(warm);
    REQUIRE(marker.has_value());
    REQUIRE(marker->entries == 12);
    // The entry count is informative: a stamp taken before reading the file still matches.
    REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 0, 0x1234, 0x2206, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1235, 0x2206, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2207, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 54)));

    WriteBytes(warm, "a rebuilt precache");
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));
}

TEST_CASE("ShaderPrecache: the marker carries the content stamp and an old marker is not one",
          "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "precache");
    auto host = TestHost();
    {
        const HostGuard guard{host};
        const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 12, 1, 2, 3);
        REQUIRE(stamp.has_value());
        REQUIRE(stamp->content_stamp == host.content_stamp);
        REQUIRE(ShaderWarm::WriteWarmMarker(warm, *stamp));
        const auto marker = ShaderWarm::ReadWarmMarker(warm);
        REQUIRE(marker.has_value());
        REQUIRE(marker->content_stamp == host.content_stamp);
        REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 1, 2, 3)));
        // Another executable or content set is another precache, though the file is the same.
        host.content_stamp += 1;
        CE::SetHostIdentity(host);
        REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 1, 2, 3)));
    }
    // A marker from before the stamp existed has six fields and is not read.
    WriteBytes(ShaderWarm::WarmMarkerPath(warm), "8 1234 12 1 2 3\n");
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());
}

TEST_CASE("ShaderPrecache: the entry count belongs to a file of one size", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "0123456789");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 0);

    WriteBytes(ShaderWarm::WarmCountPath(warm), "345 10\n");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 345);

    WriteBytes(warm, "01234567890");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 0);
}

TEST_CASE("ShaderPrecache: a compute entry reads back like a pipeline cache entry",
          "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    for (size_t i = 0; i < lowered.code.size(); ++i) {
        lowered.code[i] = static_cast<u8>(i * 7 + 1);
    }
    lowered.texture_buffer = 3;
    const ShaderWarm::ComputeControl control{{64, 2, 1}, 1024, 256};
    ShaderWarm::ComputeKey key_bytes{};
    const auto entry = ShaderWarm::ComputeEntry(lowered, control, key_bytes);

    const auto entry_view = std::span<const u8>(entry);
    VideoCommon::PipelineRecord record;
    REQUIRE(VideoCommon::DecodePipelineRecord(entry_view, false, record) == CE::RejectReason::None);
    REQUIRE(record.is_compute);
    REQUIRE(record.envs.size() == 1);
    VideoCommon::FileEnvironment& env = record.envs.front();
    Vulkan::ComputePipelineCacheKey key{};
    static_assert(sizeof(key) == sizeof(key_bytes));
    REQUIRE(record.key.size() == sizeof(key));
    std::memcpy(&key, record.key.data(), sizeof(key));
    // The key is the 24 bytes the entry reports: hash, shared memory, workgroup.
    REQUIRE(std::memcmp(record.key.data(), key_bytes.data(), sizeof(key)) == 0);

    REQUIRE(env.ShaderStage() == Shader::Stage::Compute);
    REQUIRE(env.WorkgroupSize() == control.workgroup);
    REQUIRE(env.SharedMemorySize() == control.shared_memory);
    REQUIRE(env.LocalMemorySize() == control.local_memory);
    REQUIRE(env.TextureBoundBuffer() == 3);
    REQUIRE(std::memcmp(&key, key_bytes.data(), sizeof(key)) == 0);
    REQUIRE(key.unique_hash == ShaderWarm::StageHash(lowered.code));
    REQUIRE(key.shared_memory_size == control.shared_memory);
    REQUIRE(key.workgroup_size == control.workgroup);

    // The entry has no recorded reads: without guesses they are errors, with them the guesses.
    REQUIRE_THROWS_AS(env.ReadCbufValue(0, 0x40), Shader::LogicError);
    static const VideoCommon::GuessMissing guess{};
    env.SetGuessMissing(&guess);
    REQUIRE(env.ReadCbufValue(0, 0x40) == guess.cbuf_bank0);
    REQUIRE(env.ReadCbufValue(3, 0x40) == guess.cbuf_other);
    REQUIRE(env.ReadTextureType(5) == guess.texture_type);
    REQUIRE(env.IsCbufStorageBufferPointer(0, 0x10) == guess.storage_pointer_bank0);
    REQUIRE(env.IsCbufStorageBufferPointer(1, 0x10) == guess.storage_pointer_other);
}

TEST_CASE("ShaderPrecache: a program naming no texture bank reads bank 0", "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    ShaderWarm::ComputeKey key{};
    const auto entry = ShaderWarm::ComputeEntry(lowered, {{1, 1, 1}, 0, 0}, key);
    u32 texture_bound{};
    // env_count, env_length, then six u64 counts and local memory before the texture bank.
    std::memcpy(&texture_bound, entry.data() + 4 + 4 + 6 * 8 + 4, sizeof(texture_bound));
    REQUIRE(texture_bound == 0);
}

TEST_CASE("ShaderPrecache: a file from an older builder, cache version, build, title or content is "
          "stale",
          "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    const auto host = TestHost();
    const HostGuard guard{host};
    const auto reason = [](CE::RejectReason rejection) {
        return fmt::format("cache file rejected ({})", CE::RejectReasonName(rejection));
    };
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == "none built yet");

    const auto current = ShaderWarm::WarmFileIdentity(host, ShaderWarm::WarmAddonsEnabled());
    WriteWarmFile(warm, current);
    const auto count = ShaderWarm::WarmCountPath(warm);
    REQUIRE_FALSE(ShaderWarm::WarmFilesStale(warm).has_value());

    // Another build, title or content set, or add-ons on the other side of the switch.
    auto other = host;
    other.build_id += 1;
    CE::SetHostIdentity(other);
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == reason(CE::RejectReason::BuildId));
    other = host;
    other.title_id += 1;
    CE::SetHostIdentity(other);
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == reason(CE::RejectReason::Title));
    other = host;
    other.game_version += 1;
    CE::SetHostIdentity(other);
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == reason(CE::RejectReason::GameVersion));
    other = host;
    other.content_stamp += 1;
    CE::SetHostIdentity(other);
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == reason(CE::RejectReason::Content));
    CE::SetHostIdentity(host);
    REQUIRE_FALSE(ShaderWarm::WarmFilesStale(warm).has_value());
    WriteWarmFile(warm, ShaderWarm::WarmFileIdentity(host, !ShaderWarm::WarmAddonsEnabled()));
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == reason(CE::RejectReason::Content));

    // A file of another cache version.
    auto old_version = current;
    old_version.shader_format_version -= 1;
    WriteWarmFile(warm, old_version);
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == "built for another cache version");

    // The count file: builder version and size.
    WriteWarmFile(warm, current, ShaderWarm::WarmBuilderVersion - 1);
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == "built by an older version");
    WriteBytes(count, fmt::format("1 {}\n", std::filesystem::file_size(warm)));
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == "built by an older version");
    WriteWarmFile(warm, current);
    REQUIRE_FALSE(ShaderWarm::WarmFilesStale(warm).has_value());
    WriteBytes(count, fmt::format("1 {} {}\n", std::filesystem::file_size(warm) + 1,
                                  ShaderWarm::WarmBuilderVersion));
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == "changed since it was built");

    // A file cut short by a crash is not an envelope.
    WriteWarmFile(warm, current);
    std::filesystem::resize_file(warm, std::filesystem::file_size(warm) - 1);
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == reason(CE::RejectReason::Truncated));
}

TEST_CASE("ShaderPrecache: a precache file in the old format is set aside once", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    const auto backup = std::filesystem::path(warm).concat(".legacy.bak");
    const HostGuard guard{TestHost()};
    std::string old_file = "yuzucach";
    const u32 version = ShaderWarm::WarmCacheVersion;
    old_file.append(reinterpret_cast<const char*>(&version), sizeof(version));
    old_file.append("entries");
    WriteBytes(warm, old_file);
    WriteBytes(ShaderWarm::WarmCountPath(warm),
               fmt::format("0 {} {}\n", old_file.size(), ShaderWarm::WarmBuilderVersion));
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == "built in the old file format");
    REQUIRE_FALSE(std::filesystem::exists(warm));
    REQUIRE(std::filesystem::file_size(backup) == old_file.size());
    // Nothing is left to reject, so nothing is backed up twice.
    REQUIRE(ShaderWarm::WarmFilesStale(warm) == "none built yet");
    REQUIRE(std::filesystem::exists(backup));
}

TEST_CASE("ShaderPrecache: every container the archive walker reads is discovered",
          "[video_core]") {
    using ShaderWarm::ClassifyGameFile;
    using ShaderWarm::GameFileKind;
    for (const char* path : {"Shader/Model.product.nin_ounce_nvn2.bfsha.zs",
                             "Shader/Model.bfsha", "Pack/Shaders.nin_ounce_nvn2.release.sarc.zs",
                             "Pack/Shaders.release.sarc", "Shader/Loose.bnsh",
                             "Shader/Loose.bnsh.zs", "Shader/Bundle.cgsb", "Shader/Bundle.cgsb.zs",
                             "Shader/Program.shn", "Shader/Program.shn.zs", "Shader/Archive.sharcb",
                             "Shader/Archive.sharcb.zs", "Shader/UPPER.BNSH", "Effect/Fx.bfsha.zs"}) {
        INFO(path);
        REQUIRE(ClassifyGameFile(path) == GameFileKind::ShaderArchive);
    }
    REQUIRE(ClassifyGameFile("Effect/Fx.nin_ounce_nvn2.vsetb.byml.zs") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/static.Nin_Ounce_NVN2.esetb.byml.zs") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/Particle/Magic.ptcl.zst") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/Particle/Magic.ptcl") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Pack/ZsDic.pack.zs") == GameFileKind::ZstdDictionaries);
    for (const char* path :
         {"Effect/Sub/Fx.vsetb.byml.zs", "Model/Fx.vsetb.byml.zs", "Effect/Fx.vsetb.byml",
          "Effect/Sub/Fx.esetb.byml.zs",
          "Model/Tex.bntx.zs", "Model/Mesh.bfres.zs", "Asset/assets_a000.dat",
          "Bin/Nvn2-upper_left.ndsr", "Shader/notes.shn.txt", "Shader/bnsh"}) {
        INFO(path);
        REQUIRE(ClassifyGameFile(path) == GameFileKind::Other);
    }
}

TEST_CASE("ShaderPrecache: a particle effect without shader programs is not an error",
          "[video_core]") {
    std::vector<u8> effect(64, 0);
    std::memcpy(effect.data(), "VFXB", 4);
    size_t records = 0;
    size_t errors = 0;
    REQUIRE_NOTHROW(ShaderWarm::WalkArchive(
        effect, "Magic.ptcl", nullptr, [&](const ShaderWarm::ShaderRecord&) { ++records; },
        [&](const std::string&) { ++errors; }, {}));
    REQUIRE(records == 0);
    REQUIRE(errors == 0);
}

TEST_CASE("ShaderPrecache: programs that cannot be lowered are refused", "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    REQUIRE(ShaderWarm::LowerProgram({}, lowered).has_value());
    const std::array<u8, 15> partial{};
    REQUIRE(ShaderWarm::LowerProgram(partial, lowered).has_value());
    // One instruction with no exit: not a complete program.
    const std::array<u8, 16> no_exit{};
    REQUIRE(ShaderWarm::LowerProgram(no_exit, lowered).has_value());
    REQUIRE(lowered.code.empty());
}

TEST_CASE("ShaderPrecache: the old cache file reader refuses a record without environments",
          "[video_core]") {
    TempDir dir;
    const auto file = dir.path / "pipelines.bin";
    const u32 version = 9;
    for (const u32 envs : {0U, static_cast<u32>(CE::MaxEnvsPerRecord) + 1, 0xffffffffU}) {
        INFO(envs);
        std::string bytes = "yuzucach";
        bytes.append(reinterpret_cast<const char*>(&version), sizeof(version));
        bytes.append(reinterpret_cast<const char*>(&envs), sizeof(envs));
        bytes.append(64, '\0');
        WriteBytes(file, bytes);
        size_t loaded = 0;
        VideoCommon::LoadPipelines(
            {}, file, version, [&](std::ifstream&, VideoCommon::FileEnvironment) { ++loaded; },
            [&](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) { ++loaded; });
        REQUIRE(loaded == 0);
        // The file is deleted as broken: it was neither read nor allocated for.
        REQUIRE_FALSE(std::filesystem::exists(file));
    }
}

TEST_CASE("ShaderPrecache: the builder stamps the identity it is given and refuses a bad one",
          "[video_core]") {
    TempDir dir;
    Core::System system;
    const auto missing_game = dir.path / "no-such-game.nsp";
    const auto build = [&](const CE::HostIdentity& host, u64 title_id, std::string_view folder) {
        ShaderWarm::BuildOptions options;
        options.output_dir = dir.path / folder;
        return ShaderWarm::BuildWarmFilesWith(system, missing_game, title_id, host, options, {}, {});
    };
    const auto host = TestHost();
    // Another title ran before and published its identity: the builder must not read it.
    auto previous = TestHost();
    previous.title_id += 1;
    previous.content_stamp += 1;
    const HostGuard guard{previous};

    // Never assembled: zero build or zero content stamp, whatever the process published.
    for (const auto& unassembled : {CE::HostIdentity{}, [&] {
                                        auto h = host;
                                        h.build_id = 0;
                                        return h;
                                    }(),
                                    [&] {
                                        auto h = host;
                                        h.content_stamp = 0;
                                        return h;
                                    }()}) {
        const auto result = build(unassembled, host.title_id, "unassembled");
        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error == "No host identity was assembled for this title");
    }
    // The identity of another title, though it is complete (what the global holds is that one).
    const auto wrong_title = build(previous, host.title_id, "unassembled");
    REQUIRE_FALSE(wrong_title.ok);
    REQUIRE(wrong_title.error == "The host identity is of another title");
    // Refused before anything is created.
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "unassembled"));

    // A complete identity of the title passes the check and goes on to open the game.
    const auto opened = build(host, host.title_id, "assembled");
    REQUIRE_FALSE(opened.ok);
    REQUIRE(opened.error == "Cannot open the game file");
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "assembled" / "warm-compute.bin"));

    // The explicit identity is what stamps the file: the global one is of another title and
    // would make the file stale for the title being built.
    const auto stamped = ShaderWarm::WarmFileIdentity(host, false);
    REQUIRE(stamped.host.title_id == host.title_id);
    REQUIRE(stamped.host.build_id == host.build_id);
    REQUIRE(stamped.host.game_version == host.game_version);
    REQUIRE_FALSE(ShaderWarm::WarmHostIdentityError(host, host.title_id).has_value());
    REQUIRE(ShaderWarm::WarmHostIdentityError(host, host.title_id + 1).has_value());
}
