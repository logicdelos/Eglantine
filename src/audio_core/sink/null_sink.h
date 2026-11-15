// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {
class NullSinkStreamImpl final : public SinkStream {
public:
    explicit NullSinkStreamImpl(Core::System& system_, StreamType type_)
        : SinkStream{system_, type_} {}
    ~NullSinkStreamImpl() override {}
    void AppendBuffer(SinkBuffer&, std::span<s16>) override {}
    std::vector<s16> ReleaseBuffer(u64) override {
        return {};
    }
};

/**
 * A no-op sink for when no audio out is wanted.
 */
class NullSink final : public Sink {
public:
    explicit NullSink(std::string_view) {}
    ~NullSink() override = default;

    SinkStream* AcquireSinkStream(Core::System& system, u32, const std::string&,
                                  StreamType type) override {
        // One stream per acquire like the real sinks: sessions must not share the played sample
        // count, ring size or system volume.
        SinkStreamPtr& stream = null_sinks.emplace_back(
            std::make_unique<NullSinkStreamImpl>(system, type));
        stream->SetDeviceVolume(device_volume);
        return stream.get();
    }

    void CloseStream(SinkStream* stream) override {
        std::erase_if(null_sinks, [stream](const SinkStreamPtr& s) { return s.get() == stream; });
    }
    void CloseStreams() override {
        null_sinks.clear();
    }
    f32 GetDeviceVolume() const override {
        return device_volume;
    }
    void SetDeviceVolume(f32 volume) override {
        device_volume = volume;
        for (auto& stream : null_sinks) {
            stream->SetDeviceVolume(volume);
        }
    }
    void SetSystemVolume(f32 volume) override {}

private:
    std::vector<SinkStreamPtr> null_sinks{};
};

} // namespace AudioCore::Sink
