// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <system_error>

#include <fmt/format.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "yuzu_cmd/control_file.h"

namespace Control {

namespace {

namespace fs = std::filesystem;

/// A temporary name beside the destination that no other call of this process uses.
fs::path TemporaryPath(const fs::path& destination, unsigned attempt) {
    static std::atomic<std::uint64_t> counter{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path leaf{"."};
    leaf += destination.filename();
    leaf += fmt::format(".{:x}.{:x}.{}.tmp", static_cast<std::uint64_t>(ticks), ++counter, attempt);
    return destination.parent_path() / leaf;
}

PublishOutcome Failure(PublishResult result, std::string message) {
    PublishOutcome outcome;
    outcome.result = result;
    outcome.message = std::move(message);
    return outcome;
}

#ifdef _WIN32

std::string WindowsError(const char* what, DWORD error) {
    return fmt::format("{}: Windows error {}", what, error);
}

#endif

} // namespace

PublishOutcome PublishFile(const fs::path& destination, std::string_view data, bool overwrite) {
    if (destination.filename().empty()) {
        return Failure(PublishResult::NotRegular, "the path names no file");
    }
#ifdef _WIN32
    // The device namespace (\\.\pipe\..., \\.\COM1) is not a place for files.
    if (const std::wstring& native = destination.native();
        native.starts_with(L"\\\\.\\") || native.starts_with(L"//./")) {
        return Failure(PublishResult::NotRegular, "device paths are not accepted");
    }
#endif

    std::error_code ec;
    const fs::file_status existing = fs::symlink_status(destination, ec);
    if (!ec && fs::exists(existing)) {
        if (!overwrite) {
            return Failure(PublishResult::Exists, "the file exists");
        }
        if (!fs::is_regular_file(existing)) {
            return Failure(PublishResult::NotRegular, "the destination is not an ordinary file");
        }
    }

    // The temporary file is created in a place that is only ever ours: exclusive creation fails
    // on any existing name, so the handle is of a new regular file and cannot block.
    fs::path temporary;
    bool created = false;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 8 && !created; ++attempt) {
        temporary = TemporaryPath(destination, attempt);
        handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            created = true;
        } else if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return Failure(PublishResult::IoError,
                           WindowsError("cannot create the file", GetLastError()));
        }
    }
    if (!created) {
        return Failure(PublishResult::IoError, "cannot find a free temporary file name");
    }
    bool written = GetFileType(handle) == FILE_TYPE_DISK;
    std::size_t offset = 0;
    while (written && offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1u << 24));
        DWORD done = 0;
        written = WriteFile(handle, data.data() + offset, chunk, &done, nullptr) != 0 && done != 0;
        offset += done;
    }
    const DWORD write_error = GetLastError();
    const bool closed = CloseHandle(handle) != 0;
    if (!written || !closed) {
        DeleteFileW(temporary.c_str());
        return Failure(PublishResult::IoError, WindowsError("cannot write the file", write_error));
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        if (!overwrite && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) {
            return Failure(PublishResult::Exists, "the file exists");
        }
        return Failure(PublishResult::IoError, WindowsError("cannot put the file in place", error));
    }
#else
    int fd = -1;
    for (unsigned attempt = 0; attempt < 8 && !created; ++attempt) {
        temporary = TemporaryPath(destination, attempt);
        fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0) {
            created = true;
        } else if (errno != EEXIST) {
            return Failure(PublishResult::IoError,
                           fmt::format("cannot create the file: {}", std::strerror(errno)));
        }
    }
    if (!created) {
        return Failure(PublishResult::IoError, "cannot find a free temporary file name");
    }
    bool written = true;
    std::size_t offset = 0;
    while (written && offset < data.size()) {
        const ssize_t count = write(fd, data.data() + offset, data.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            written = false;
        }
    }
    const int write_error = errno;
    const bool closed = close(fd) == 0;
    if (!written || !closed) {
        unlink(temporary.c_str());
        return Failure(PublishResult::IoError,
                       fmt::format("cannot write the file: {}", std::strerror(write_error)));
    }
    if (overwrite) {
        if (rename(temporary.c_str(), destination.c_str()) != 0) {
            const int error = errno;
            unlink(temporary.c_str());
            return Failure(PublishResult::IoError,
                           fmt::format("cannot put the file in place: {}", std::strerror(error)));
        }
    } else {
        // link fails with EEXIST when the destination exists, whenever it appeared.
        if (link(temporary.c_str(), destination.c_str()) != 0) {
            const int error = errno;
            unlink(temporary.c_str());
            if (error == EEXIST) {
                return Failure(PublishResult::Exists, "the file exists");
            }
            return Failure(PublishResult::IoError,
                           fmt::format("cannot put the file in place: {}", std::strerror(error)));
        }
        unlink(temporary.c_str());
    }
#endif
    return {};
}

} // namespace Control
