// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/lz4_compression.h"
#include "core/file_sys/fssystem/fssystem_compression_configuration.h"
#include "video_core/nx2_spikes.h"

namespace FileSys {

namespace {

Result DecompressLz4(void* dst, size_t dst_size, const void* src, size_t src_size) {
    // The reads of the compressed bytes are Disk spans at the file read below; this is the decoding.
    const VideoCommon::Nx2Spikes::SpanScope decompress{VideoCommon::Nx2Spikes::Cause::Decompression,
                                                       0, dst_size};
    auto result = Common::Compression::DecompressDataLZ4(dst, dst_size, src, src_size);
    R_UNLESS(static_cast<size_t>(result) == dst_size, ResultUnexpectedInCompressedStorageC);
    R_SUCCEED();
}

constexpr DecompressorFunction GetNcaDecompressorFunction(CompressionType type) {
    switch (type) {
    case CompressionType::Lz4:
        return DecompressLz4;
    default:
        return nullptr;
    }
}

} // namespace

const NcaCompressionConfiguration& GetNcaCompressionConfiguration() {
    static const NcaCompressionConfiguration configuration = {
        .get_decompressor = GetNcaDecompressorFunction,
    };

    return configuration;
}

} // namespace FileSys
