// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <ranges>
#include <tuple>
#include <cstdlib>

#include "common/assert.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include <ranges>

namespace AudioCore {
constexpr u32 CurrentRevision = 15;
constexpr u32 NativeCurrentRevision = 16;
constexpr u32 MaxDocumentedRevision = 18;

constexpr bool IsNativeRevision(u32 revision) {
    return (revision & 0xffffff) == Common::MakeMagic('R', 'E', 'v', '\0');
}

constexpr u32 EncodeRevision(u32 revision, bool native) {
    return Common::MakeMagic('R', 'E', native ? 'v' : 'V',
                             static_cast<char>('0' + revision));
}

enum class SupportTags {
    CommandProcessingTimeEstimatorVersion4,
    CommandProcessingTimeEstimatorVersion3,
    CommandProcessingTimeEstimatorVersion2,
    MultiTapBiquadFilterProcessing,
    EffectInfoVer2,
    WaveBufferVer2,
    BiquadFilterFloatProcessing,
    VolumeMixParameterPrecisionQ23,
    MixInParameterDirtyOnlyUpdate,
    BiquadFilterEffectStateClearBugFix,
    VoicePlayedSampleCountResetAtLoopPoint,
    VoicePitchAndSrcSkipped,
    SplitterBugFix,
    FlushVoiceWaveBuffers,
    ElapsedFrameCount,
    AudioRendererVariadicCommandBufferSize,
    PerformanceMetricsDataFormatVersion2,
    AudioRendererProcessingTimeLimit80Percent,
    AudioRendererProcessingTimeLimit75Percent,
    AudioRendererProcessingTimeLimit70Percent,
    AdpcmLoopContextBugFix,
    Splitter,
    LongSizePreDelay,
    AudioUsbDeviceOutput,
    DeviceApiVersion2,
    DelayChannelMappingChange,
    ReverbChannelMappingChange,
    I3dl2ReverbChannelMappingChange,
    SplitterPrevVolumeReset,
    SplitterBiquadFilterParameter,
    SplitterDestinationV2b,
    VoiceInParameterV2,
    SplitterDepopBugFix,
    VoiceBiquadFilterCrossFadeCurve,
    FinalMixSubMixVolumeRamp,
    BufferMixerVolumeRamp,

    // Not a real tag, just here to get the count.
    Size
};

constexpr u32 GetRevisionNum(u32 user_revision) {
    if (user_revision < 0x100) {
        return user_revision;
    }
    if (!IsNativeRevision(user_revision) &&
        (user_revision & 0xffffff) != Common::MakeMagic('R', 'E', 'V', '\0')) {
        return ~u32{0};
    }
    const u32 encoded = user_revision >> 24;
    return encoded >= '0' ? encoded - '0' : ~u32{0};
};

constexpr bool CheckFeatureSupported(SupportTags tag, u32 user_revision) {
    constexpr std::array<std::pair<SupportTags, u32>, static_cast<u32>(SupportTags::Size)> features{
        {
            {SupportTags::AudioRendererProcessingTimeLimit70Percent, 1},
            {SupportTags::Splitter, 2},
            {SupportTags::AdpcmLoopContextBugFix, 2},
            {SupportTags::LongSizePreDelay, 3},
            {SupportTags::AudioUsbDeviceOutput, 4},
            {SupportTags::AudioRendererProcessingTimeLimit75Percent, 4},
            {SupportTags::VoicePlayedSampleCountResetAtLoopPoint, 5},
            {SupportTags::VoicePitchAndSrcSkipped, 5},
            {SupportTags::SplitterBugFix, 5},
            {SupportTags::FlushVoiceWaveBuffers, 5},
            {SupportTags::ElapsedFrameCount, 5},
            {SupportTags::AudioRendererProcessingTimeLimit80Percent, 5},
            {SupportTags::AudioRendererVariadicCommandBufferSize, 5},
            {SupportTags::PerformanceMetricsDataFormatVersion2, 5},
            {SupportTags::CommandProcessingTimeEstimatorVersion2, 5},
            {SupportTags::BiquadFilterEffectStateClearBugFix, 6},
            {SupportTags::BiquadFilterFloatProcessing, 7},
            {SupportTags::VolumeMixParameterPrecisionQ23, 7},
            {SupportTags::MixInParameterDirtyOnlyUpdate, 7},
            {SupportTags::WaveBufferVer2, 8},
            {SupportTags::CommandProcessingTimeEstimatorVersion3, 8},
            {SupportTags::EffectInfoVer2, 9},
            {SupportTags::CommandProcessingTimeEstimatorVersion4, 10},
            {SupportTags::MultiTapBiquadFilterProcessing, 10},
            {SupportTags::DeviceApiVersion2, 10},
            {SupportTags::DelayChannelMappingChange, 11},
            {SupportTags::ReverbChannelMappingChange, 11},
            {SupportTags::I3dl2ReverbChannelMappingChange, 11},
            {SupportTags::SplitterBiquadFilterParameter, 12},
            {SupportTags::SplitterPrevVolumeReset, 13},
            {SupportTags::SplitterDepopBugFix, 14},
            {SupportTags::SplitterDestinationV2b, 15},
            {SupportTags::VoiceInParameterV2, 15},
            {SupportTags::VoiceBiquadFilterCrossFadeCurve, 16},
            {SupportTags::FinalMixSubMixVolumeRamp, 17},
            {SupportTags::BufferMixerVolumeRamp, 18},
        }};

    const auto& feature =
        std::ranges::find_if(features, [tag](const auto& entry) { return entry.first == tag; });
    if (feature == features.cend()) {
        LOG_ERROR(Service_Audio, "Invalid SupportTag {}!", static_cast<u32>(tag));
        return false;
    }
    user_revision = GetRevisionNum(user_revision);
    return user_revision != ~u32{0} && (*feature).second <= user_revision;
}

/// Revisions up to MaxDocumentedRevision (REV18, firmware 23.0.0) are documented on switchbrew;
/// EDEN_NX2_AUDREN_REV18 makes the renderer accept them (read once), otherwise the legacy caps
/// apply.
inline bool AcceptDocumentedRevisions() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_AUDREN_REV18");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

inline bool CheckValidRevision(u32 user_revision) {
    const u32 legacy_cap = IsNativeRevision(user_revision) ? NativeCurrentRevision : CurrentRevision;
    return GetRevisionNum(user_revision) <=
           (AcceptDocumentedRevisions() ? MaxDocumentedRevision : legacy_cap);
}

} // namespace AudioCore
