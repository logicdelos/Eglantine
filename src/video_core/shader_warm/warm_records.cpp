// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_warm/warm_records.h"

namespace VideoCommon::ShaderWarm {
namespace {

/// The shader header a compute environment holds in front of its code (all zero).
constexpr size_t ComputeHeaderWords = 10;
constexpr size_t EnvHeaderSize = 6 * 8 + 7 * 4;
constexpr u32 ComputeStage = 5;

template <typename T>
void Put(std::vector<u8>& out, T value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

void PutBytes(std::vector<u8>& out, std::span<const u8> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

template <typename T>
T Get(std::span<const u8> data, size_t offset) {
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

/// The GenericEnvironment::Serialize header: six u64 counts and seven u32 fields.
void PutEnvHeader(std::vector<u8>& out, u64 code_size, u64 cbuf_count, u32 local_memory,
                  u32 texture_bound, u32 start_address, u32 stage) {
    Put<u64>(out, code_size);
    Put<u64>(out, 0); // texture types
    Put<u64>(out, 0); // texture pixel formats
    Put<u64>(out, cbuf_count);
    Put<u64>(out, 0); // storage pointer types
    Put<u64>(out, 0); // cbuf replacements
    Put<u32>(out, local_memory);
    Put<u32>(out, texture_bound);
    Put<u32>(out, start_address);
    Put<u32>(out, 0);                                  // read lowest
    Put<u32>(out, static_cast<u32>(code_size - 8));    // read highest
    Put<u32>(out, 1);                                  // viewport transform state
    Put<u32>(out, stage);
}

} // Anonymous namespace

std::optional<std::string> LowerProgram(std::span<const u8> program, LoweredCode& out) {
    using Shader::Ampere::Instruction;
    if (program.empty() || program.size() % sizeof(Instruction) ||
        program.size() / sizeof(Instruction) > 65536) {
        return "Native input must contain 1..65536 complete 16-byte instructions";
    }
    std::vector<Instruction> native(program.size() / sizeof(Instruction));
    std::memcpy(native.data(), program.data(), native.size() * sizeof(Instruction));
    Shader::Ampere::ProgramCaptureState capture;
    bool complete = false;
    for (u32 index = 0; index < native.size(); ++index) {
        const auto result = capture.Observe(native[index], index);
        if (result == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            return "Native allocation has an out-of-range branch target";
        }
        if (result == Shader::Ampere::ProgramCaptureState::Result::Complete) {
            native.resize(index + 1);
            complete = true;
            break;
        }
    }
    if (!complete) {
        return "Native allocation lacks a complete runtime capture";
    }
    // As GenericEnvironment::LowerNativeCode for a compute dispatch, without the tensor forms.
    auto lowered = Shader::Ampere::LowerToMaxwell(native, {});
    if (!lowered) {
        return std::string(lowered.error);
    }
    constexpr std::array<u64, ComputeHeaderWords> header{};
    const std::vector<u64> words = LayOutLoweredNativeCode(header, native, lowered);
    out.code.resize(words.size() * sizeof(u64));
    std::memcpy(out.code.data(), words.data(), out.code.size());
    out.texture_buffer = lowered.required_texture_buffer;
    return std::nullopt;
}

u64 StageHash(std::span<const u8> code) {
    std::vector<u64> words(code.size() / sizeof(u64));
    std::memcpy(words.data(), code.data(), words.size() * sizeof(u64));
    return NativeShaderHash(words);
}

std::optional<ComputeControl> ReadComputeControl(std::span<const u8> control) {
    if (control.size() < 108) {
        return std::nullopt;
    }
    ComputeControl result;
    for (size_t i = 0; i < 3; ++i) {
        result.workgroup[i] = Get<u32>(control, 88 + i * 4);
    }
    result.shared_memory =
        static_cast<u32>((u64{Get<u32>(control, 100)} + 255) / 256 * 256);
    result.local_memory = Get<u32>(control, 104);
    return result;
}

std::vector<u8> ComputeEntry(const LoweredCode& lowered, const ComputeControl& control,
                             ComputeKey& key) {
    std::vector<u8> env;
    env.reserve(EnvHeaderSize + lowered.code.size() + 16);
    // Like InitializeNativeCode: the program's own bank when it names one.
    const u32 texture_bound = lowered.texture_buffer != ~0U ? lowered.texture_buffer : 0;
    PutEnvHeader(env, lowered.code.size(), 0, control.local_memory, texture_bound, 80,
                 ComputeStage);
    PutBytes(env, lowered.code);
    for (const u32 size : control.workgroup) {
        Put<u32>(env, size);
    }
    Put<u32>(env, control.shared_memory);
    // The key as ComputePipelineCacheKey lays it out, written field by field.
    std::vector<u8> key_bytes;
    Put<u64>(key_bytes, StageHash(lowered.code));
    Put<u32>(key_bytes, control.shared_memory);
    for (const u32 size : control.workgroup) {
        Put<u32>(key_bytes, size);
    }
    std::memcpy(key.data(), key_bytes.data(), key.size());

    std::vector<u8> out;
    out.reserve(3 * sizeof(u32) + env.size() + key_bytes.size());
    Put<u32>(out, 1); // environments
    Put<u32>(out, static_cast<u32>(env.size()));
    PutBytes(out, env);
    Put<u32>(out, static_cast<u32>(key_bytes.size()));
    PutBytes(out, key_bytes);
    return out;
}

bool WarmAddonsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_WARM_ADDON");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

CacheEnvelope::CacheIdentity WarmFileIdentity(const CacheEnvelope::HostIdentity& host,
                                              bool with_addons) {
    CacheEnvelope::CacheIdentity identity;
    identity.kind = CacheEnvelope::PayloadKind::WarmCompute;
    identity.shader_format_version = WarmCacheVersion;
    identity.host = host;
    // What the game files hold is what is cached, so the content stamp and the add-on choice
    // identify it; no emulator setting changes how a program is lowered.
    identity.host.settings_hash = 0;
    const std::array<u64, 2> content{host.content_stamp, with_addons ? u64{1} : u64{0}};
    identity.host.content_stamp = CacheEnvelope::Checksum(std::span<const u8>(
        reinterpret_cast<const u8*>(content.data()), sizeof(content)));
    return identity;
}

std::optional<std::string> WarmHostIdentityError(const CacheEnvelope::HostIdentity& host,
                                                 u64 title_id) {
    if (host.build_id == 0 || host.content_stamp == 0) {
        return "No host identity was assembled for this title";
    }
    if (host.title_id != title_id) {
        return "The host identity is of another title";
    }
    return std::nullopt;
}

} // namespace VideoCommon::ShaderWarm
