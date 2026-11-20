// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/replay/format.h"

namespace R = VideoCore::Replay;

namespace {

R::FileHeader SampleHeader() {
    R::FileHeader header;
    header.first_frame = 100;
    header.requested_frames = 3;
    header.finalized_frames = 3;
    header.preroll_frames = 2;
    header.chunk_count = 17;
    header.file_length = 4096;
    header.index_offset = 3000;
    header.session_id = 0x1122334455667788ULL;
    header.revision[0] = 'a';
    header.revision[1] = 'b';
    return header;
}

} // namespace

TEST_CASE("Replay format: byte order and bounds", "[video_core][replay]") {
    std::vector<u8> bytes;
    R::ByteWriter writer{bytes};
    writer.Put32(0x01020304U);
    writer.Put16(0x0506U);
    writer.Put64(0x0807060504030201ULL);
    REQUIRE(bytes.size() == 14);
    REQUIRE(bytes[0] == 4);
    REQUIRE(bytes[3] == 1);
    REQUIRE(bytes[4] == 6);

    R::ByteReader reader{bytes};
    REQUIRE(reader.Get32() == 0x01020304U);
    REQUIRE(reader.Get16() == 0x0506U);
    REQUIRE(reader.Get64() == 0x0807060504030201ULL);
    REQUIRE(reader.Ok());
    REQUIRE(reader.Remaining() == 0);
    // Reading past the end fails the reader and returns zero; it never reads out of bounds.
    REQUIRE(reader.Get8() == 0);
    REQUIRE_FALSE(reader.Ok());
}

TEST_CASE("Replay format: counts are bounded by the remaining bytes", "[video_core][replay]") {
    std::vector<u8> bytes;
    R::ByteWriter writer{bytes};
    writer.Put32(1000);
    writer.PutZero(8);
    R::ByteReader reader{bytes};
    REQUIRE(reader.GetCount(4096, 4) == 0);
    REQUIRE_FALSE(reader.Ok());

    R::ByteReader limited{bytes};
    REQUIRE(limited.GetCount(10, 0) == 0);
    REQUIRE_FALSE(limited.Ok());
}

TEST_CASE("Replay format: checksum vectors", "[video_core][replay]") {
    REQUIRE(R::Fnv1a64({}) == 0xcbf29ce484222325ULL);
    const std::array<u8, 1> a{'a'};
    REQUIRE(R::Fnv1a64(a) == 0xaf63dc4c8601ec8cULL);
    // The chain is order sensitive.
    const u64 ab = R::ChainStep(R::ChainStep(R::Fnv1aOffset, 1), 2);
    const u64 ba = R::ChainStep(R::ChainStep(R::Fnv1aOffset, 2), 1);
    REQUIRE(ab != ba);
}

TEST_CASE("Replay format: file header round trip", "[video_core][replay]") {
    const R::FileHeader header = SampleHeader();
    const std::vector<u8> bytes = R::EncodeFileHeader(header);
    REQUIRE(bytes.size() == R::FileHeaderBytes);
    for (size_t i = 0; i < R::Magic.size(); ++i) {
        REQUIRE(bytes[i] == R::Magic[i]);
    }

    R::FileHeader decoded;
    REQUIRE(R::DecodeFileHeader(bytes, decoded) == R::ReadError::None);
    REQUIRE(decoded.first_frame == header.first_frame);
    REQUIRE(decoded.requested_frames == header.requested_frames);
    REQUIRE(decoded.finalized_frames == header.finalized_frames);
    REQUIRE(decoded.preroll_frames == header.preroll_frames);
    REQUIRE(decoded.chunk_count == header.chunk_count);
    REQUIRE(decoded.file_length == header.file_length);
    REQUIRE(decoded.index_offset == header.index_offset);
    REQUIRE(decoded.session_id == header.session_id);
    REQUIRE(decoded.revision == header.revision);
    REQUIRE(decoded.title_version == R::UnavailableVersion);
    REQUIRE(R::EncodeFileHeader(decoded) == bytes);
}

TEST_CASE("Replay format: file header rejections", "[video_core][replay]") {
    const std::vector<u8> good = R::EncodeFileHeader(SampleHeader());
    R::FileHeader out;

    REQUIRE(R::DecodeFileHeader(std::span<const u8>{good}.first(255), out) ==
            R::ReadError::Truncated);

    std::vector<u8> bad = good;
    bad[0] ^= 0xFF;
    REQUIRE(R::DecodeFileHeader(bad, out) == R::ReadError::BadMagic);

    bad = good;
    bad[100] ^= 0x01;
    REQUIRE(R::DecodeFileHeader(bad, out) == R::ReadError::BadHeaderChecksum);

    // A different major version is refused before anything else is trusted.
    bad = good;
    bad[8] = static_cast<u8>(R::FormatMajor + 1);
    REQUIRE(R::DecodeFileHeader(bad, out) == R::ReadError::BadVersion);

    // An unknown required feature is refused even with a valid checksum.
    R::FileHeader unsupported = SampleHeader();
    unsupported.required_features |= 1ULL << 40;
    REQUIRE(R::DecodeFileHeader(R::EncodeFileHeader(unsupported), out) ==
            R::ReadError::UnsupportedFeature);

    // An unknown optional feature is accepted.
    R::FileHeader optional = SampleHeader();
    optional.optional_features |= 1ULL << 40;
    REQUIRE(R::DecodeFileHeader(R::EncodeFileHeader(optional), out) == R::ReadError::None);

    R::FileHeader too_many = SampleHeader();
    too_many.requested_frames = R::MaxSelectedFrames + 1;
    too_many.finalized_frames = 0;
    REQUIRE(R::DecodeFileHeader(R::EncodeFileHeader(too_many), out) ==
            R::ReadError::LimitExceeded);
}

TEST_CASE("Replay format: chunk header round trip and rejections", "[video_core][replay]") {
    const std::array<u8, 5> payload{1, 2, 3, 4, 5};
    R::ChunkHeader header;
    header.type = static_cast<u32>(R::ChunkType::Session);
    header.sequence = 7;
    header.frame_id = 0;
    const std::array<u8, R::ChunkHeaderBytes> bytes = R::EncodeChunkHeader(header, payload);
    REQUIRE(header.stored_length == payload.size());
    REQUIRE(header.uncompressed_length == payload.size());

    R::ChunkHeader decoded;
    REQUIRE(R::DecodeChunkHeader(bytes, decoded) == R::ReadError::None);
    REQUIRE(decoded.type == header.type);
    REQUIRE(decoded.sequence == 7);
    REQUIRE(decoded.header_checksum == header.header_checksum);
    REQUIRE(R::VerifyPayload(decoded, payload) == R::ReadError::None);

    std::array<u8, 5> corrupt = payload;
    corrupt[2] ^= 0x10;
    REQUIRE(R::VerifyPayload(decoded, corrupt) == R::ReadError::BadChunkChecksum);
    REQUIRE(R::VerifyPayload(decoded, std::span<const u8>{payload}.first(4)) ==
            R::ReadError::Truncated);

    std::array<u8, R::ChunkHeaderBytes> flipped = bytes;
    flipped[9] ^= 0x01;
    REQUIRE(R::DecodeChunkHeader(flipped, decoded) == R::ReadError::BadChunkChecksum);
    REQUIRE(R::DecodeChunkHeader(std::span<const u8>{bytes}.first(63), decoded) ==
            R::ReadError::Truncated);
}
