// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/cache_envelope.h"

namespace VideoCommon::ShaderWarm {

/// Precache file records: compute records of the envelope (CacheEnvelope::PayloadKind::WarmCompute)
/// with no recorded reads (the emulator guesses them, see GuessMissing).

/// Precache files use the Vulkan pipeline cache version: the emulator loads one only when it
/// matches, so the two must move together (vk_pipeline_cache.cpp checks it).
constexpr u32 WarmCacheVersion = 35;

/// What the builder finds in the game files, recorded in the precache's count file. A file built by
/// an older builder is rebuilt at boot (RefreshWarmFiles). 2: Mario Kart World's .esetb effects.
/// 3: .ptcl particle effects (Fire Emblem).
constexpr u32 WarmBuilderVersion = 3;

using ComputeKey = std::array<u8, 24>;

/// A compute program lowered and laid out like GenericEnvironment::LowerNativeCode.
struct LoweredCode {
    std::vector<u8> code;
    /// Constant buffer the program's bound textures read, ~0U when it names none.
    u32 texture_buffer{~0U};
};

/// Captures and lowers one extracted compute program as the runtime does for a dispatch and lays
/// it out with LayOutLoweredNativeCode (a zero shader header). Matrix multiply programs, lowered
/// per title at runtime, are not converted. Returns the error text when the program cannot be
/// lowered.
std::optional<std::string> LowerProgram(std::span<const u8> program, LoweredCode& out);

/// The pipeline cache key hash of laid out code (NativeShaderHash).
u64 StageHash(std::span<const u8> code);

struct ComputeControl {
    std::array<u32, 3> workgroup{};
    u32 shared_memory{};
    u32 local_memory{};
};

/// Workgroup (control bytes 88, 92, 96), shared memory (byte 100 rounded up to 256) and local
/// memory (byte 104). Empty when the control is too short.
std::optional<ComputeControl> ReadComputeControl(std::span<const u8> control);

/// One compute record payload in the envelope's record codec (EncodePipelineRecord): environment
/// count 1, the environment (length-prefixed; code, compute trailer, no recorded reads), then the
/// 24-byte key (length-prefixed). `key` gets the same 24 bytes: the explicit little endian
/// encoding of ComputePipelineCacheKey (u64 stage hash, u32 shared memory, 3 x u32 workgroup).
std::vector<u8> ComputeEntry(const LoweredCode& lowered, const ComputeControl& control,
                             ComputeKey& key);

/// Whether EDEN_NX2_WARM_ADDON=1 asks for add-on content to be read too (read once).
bool WarmAddonsEnabled();

/// The identity of the precache file: kind WarmCompute, shader format WarmCacheVersion, the
/// host's build, title and game version, and a content stamp that mixes the host's with the
/// add-on choice. The device fields stay zero (the precache is built without a Vulkan device, the
/// done marker carries the driver) and the settings hash is zero (no setting changes how a program
/// is lowered). The builder, RefreshWarmFiles and the pipeline cache's reader must all use it.
CacheEnvelope::CacheIdentity WarmFileIdentity(const CacheEnvelope::HostIdentity& host,
                                              bool with_addons);

} // namespace VideoCommon::ShaderWarm
