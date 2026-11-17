// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fmt/format.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// sddl.h needs windows.h first
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "yuzu_cmd/control_transport.h"

namespace Control {

#ifdef _WIN32

namespace {

/// A descriptor that lets only the user of this process open the pipe. The default descriptor of
/// a pipe also grants read access to Everyone and to the anonymous account.
bool CurrentUserOnlyDescriptor(PSECURITY_DESCRIPTOR& descriptor, std::string& error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = fmt::format("cannot read the process token: Windows error {}", GetLastError());
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    const bool have_user =
        size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    const DWORD token_error = GetLastError();
    CloseHandle(token);
    if (!have_user) {
        error = fmt::format("cannot read the user of the process: Windows error {}", token_error);
        return false;
    }
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                                &sid_text)) {
        error = fmt::format("cannot format the user SID: Windows error {}", GetLastError());
        return false;
    }
    // Protected DACL with one entry: generic all for this user.
    const std::wstring sddl = std::wstring{L"D:P(A;;GA;;;"} + sid_text + L")";
    LocalFree(sid_text);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        error = fmt::format("cannot build the pipe access list: Windows error {}", GetLastError());
        return false;
    }
    return true;
}

class PipeTransport final : public Transport {
public:
    ~PipeTransport() override {
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
        }
        if (stop_event != nullptr) {
            CloseHandle(stop_event);
        }
        if (io_event != nullptr) {
            CloseHandle(io_event);
        }
    }

    std::string Open(const std::string& name) override {
        address = "\\\\.\\pipe\\" + name;
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop_event == nullptr || io_event == nullptr) {
            return fmt::format("cannot create an event: Windows error {}", GetLastError());
        }
        std::string error;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!CurrentUserOnlyDescriptor(descriptor, error)) {
            return error;
        }
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        const std::wstring wide{address.begin(), address.end()}; // the name is ASCII
        // One instance, so a second client cannot connect while the first one is here, and the
        // first-instance flag makes the call fail when another process owns the name.
        pipe = CreateNamedPipeW(wide.c_str(),
                                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                    FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                    PIPE_REJECT_REMOTE_CLIENTS,
                                1, 64 * 1024, 64 * 1024, 0, &attributes);
        const DWORD create_error = GetLastError();
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE) {
            return fmt::format("cannot create {}: Windows error {}{}", address, create_error,
                               create_error == ERROR_ACCESS_DENIED || create_error == ERROR_PIPE_BUSY
                                   ? " (the name is probably in use)"
                                   : "");
        }
        return {};
    }

    std::string Address() const override {
        return address;
    }

    bool WaitForClient() override {
        for (;;) {
            if (Stopped()) {
                return false;
            }
            OVERLAPPED overlapped{};
            overlapped.hEvent = io_event;
            ResetEvent(io_event);
            if (ConnectNamedPipe(pipe, &overlapped)) {
                return true;
            }
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                return true; // the client was faster than this call
            }
            if (error == ERROR_NO_DATA) {
                // The client came and went before this call: reset the pipe and wait again.
                DisconnectNamedPipe(pipe);
                continue;
            }
            if (error != ERROR_IO_PENDING) {
                return false;
            }
            DWORD ignored = 0;
            return Await(overlapped, ignored);
        }
    }

    std::size_t Read(char* buffer, std::size_t size) override {
        if (Stopped()) {
            return 0;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = io_event;
        ResetEvent(io_event);
        DWORD count = 0;
        const DWORD length = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
        if (ReadFile(pipe, buffer, length, nullptr, &overlapped)) {
            if (!GetOverlappedResult(pipe, &overlapped, &count, TRUE)) {
                return 0;
            }
        } else if (GetLastError() != ERROR_IO_PENDING || !Await(overlapped, count)) {
            return 0;
        }
        return count;
    }

    bool Write(std::string_view data) override {
        while (!data.empty()) {
            if (Stopped()) {
                return false;
            }
            OVERLAPPED overlapped{};
            overlapped.hEvent = io_event;
            ResetEvent(io_event);
            DWORD written = 0;
            const DWORD length = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 20));
            if (WriteFile(pipe, data.data(), length, nullptr, &overlapped)) {
                if (!GetOverlappedResult(pipe, &overlapped, &written, TRUE)) {
                    return false;
                }
            } else if (GetLastError() != ERROR_IO_PENDING || !Await(overlapped, written)) {
                return false;
            }
            data.remove_prefix(written);
        }
        return true;
    }

    void DropClient() override {
        DisconnectNamedPipe(pipe);
    }

    void Stop() override {
        SetEvent(stop_event);
    }

private:
    bool Stopped() const {
        return WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
    }

    /// Waits for an overlapped call to finish or for Stop. On Stop the call is cancelled and
    /// waited for, because the OVERLAPPED must outlive it. False on Stop or failure.
    bool Await(OVERLAPPED& overlapped, DWORD& count) {
        const HANDLE handles[2] = {overlapped.hEvent, stop_event};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
            return GetOverlappedResult(pipe, &overlapped, &count, FALSE) != FALSE;
        }
        CancelIoEx(pipe, &overlapped);
        GetOverlappedResult(pipe, &overlapped, &count, TRUE);
        return false;
    }

    std::string address;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE stop_event = nullptr;
    HANDLE io_event = nullptr;
};

} // namespace

std::unique_ptr<Transport> MakeTransport() {
    return std::make_unique<PipeTransport>();
}

#else // POSIX

namespace {

void SetCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

void SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void CloseFd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

class UnixTransport final : public Transport {
public:
    ~UnixTransport() override {
        CloseFd(client_fd);
        CloseFd(listen_fd);
        if (bound) {
            // Only the socket this process made; a name that was replaced is not ours to remove.
            struct stat now{};
            if (lstat(path.c_str(), &now) == 0 && S_ISSOCK(now.st_mode) &&
                now.st_dev == socket_device && now.st_ino == socket_inode) {
                unlink(path.c_str());
            }
        }
        CloseFd(lock_fd); // last: until here nobody else can bind this name
        CloseFd(wake_read);
        CloseFd(wake_write);
    }

    std::string Open(const std::string& name) override {
        int wake[2];
        if (pipe(wake) != 0) {
            return fmt::format("cannot create a wake-up pipe: {}", std::strerror(errno));
        }
        wake_read = wake[0];
        wake_write = wake[1];
        for (const int fd : wake) {
            SetCloseOnExec(fd);
            SetNonBlocking(fd);
        }

        // The runtime directory when there is one, else a private directory under /tmp. Either
        // way it must be ours alone: the socket file itself is only a name in that directory.
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        const std::string directory = runtime != nullptr && *runtime != '\0'
                                          ? std::string{runtime}
                                          : fmt::format("/tmp/eglantine-{}", getuid());
        mkdir(directory.c_str(), 0700);
        struct stat info{};
        if (stat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
            info.st_uid != getuid() || (info.st_mode & 077) != 0) {
            return fmt::format("{} must be a directory owned by you that others cannot access",
                               directory);
        }
        path = directory + "/" + name + ".sock";
        sockaddr_un address{};
        if (path.size() >= sizeof(address.sun_path)) {
            return fmt::format("the socket path {} is too long", path);
        }

        // Only the holder of this lock may probe, remove or bind the socket, so two launches that
        // use one name cannot interleave. The lock file stays; the lock goes with the process.
        const std::string lock_path = directory + "/" + name + ".lock";
        lock_fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (lock_fd < 0) {
            return fmt::format("cannot open the lock file {}: {}", lock_path,
                               std::strerror(errno));
        }
        struct stat lock_info{};
        if (fstat(lock_fd, &lock_info) != 0 || !S_ISREG(lock_info.st_mode) ||
            lock_info.st_uid != getuid()) {
            return fmt::format("{} is not a regular file owned by you", lock_path);
        }
        if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            return errno == EWOULDBLOCK
                       ? fmt::format("the control name {} is in use by another process", name)
                       : fmt::format("cannot lock {}: {}", lock_path, std::strerror(errno));
        }

        if (lstat(path.c_str(), &info) == 0) {
            if (!S_ISSOCK(info.st_mode)) {
                return fmt::format("{} exists and is not a socket", path);
            }
            // The lock is held, so a socket here should be a leftover of a run that did not clean
            // up. It is probed without blocking all the same: a connection, or a full backlog,
            // means a live listener; only a refusal means nobody listens.
            int probe = socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe < 0) {
                return fmt::format("cannot create a socket: {}", std::strerror(errno));
            }
            SetNonBlocking(probe);
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
            const int connected =
                connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            const int probe_error = errno;
            CloseFd(probe);
            if (connected == 0 || probe_error == EINPROGRESS || probe_error == EAGAIN ||
                probe_error == EWOULDBLOCK) {
                return fmt::format("{} is in use by another process", path);
            }
            if (probe_error != ECONNREFUSED && probe_error != ENOENT) {
                return fmt::format("cannot tell whether {} is in use: {}", path,
                                   std::strerror(probe_error));
            }
            unlink(path.c_str());
        }

        listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            return fmt::format("cannot create a socket: {}", std::strerror(errno));
        }
        SetCloseOnExec(listen_fd);
        address = {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            return fmt::format("cannot bind {}: {}", path, std::strerror(errno));
        }
        bound = true;
        if (lstat(path.c_str(), &info) == 0) {
            socket_device = info.st_dev;
            socket_inode = info.st_ino;
        }
        chmod(path.c_str(), 0600);
        if (listen(listen_fd, 1) != 0) {
            return fmt::format("cannot listen on {}: {}", path, std::strerror(errno));
        }
        return {};
    }

    std::string Address() const override {
        return path;
    }

    bool WaitForClient() override {
        for (;;) {
            if (stopped.load()) {
                return false;
            }
            pollfd fds[2] = {{listen_fd, POLLIN, 0}, {wake_read, POLLIN, 0}};
            if (poll(fds, 2, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (fds[1].revents != 0) {
                return false;
            }
            if ((fds[0].revents & POLLIN) == 0) {
                continue;
            }
            const int fd = accept(listen_fd, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) {
                    continue;
                }
                return false;
            }
            SetCloseOnExec(fd);
            SetNonBlocking(fd);
#ifdef SO_NOSIGPIPE
            const int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            client_fd = fd;
            return true;
        }
    }

    std::size_t Read(char* buffer, std::size_t size) override {
        for (;;) {
            if (stopped.load()) {
                return 0;
            }
            pollfd fds[2] = {{client_fd, POLLIN, 0}, {wake_read, POLLIN, 0}};
            if (poll(fds, 2, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return 0;
            }
            if (fds[1].revents != 0) {
                return 0;
            }
            const ssize_t count = recv(client_fd, buffer, size, 0);
            if (count > 0) {
                return static_cast<std::size_t>(count);
            }
            if (count == 0 || (errno != EINTR && errno != EAGAIN)) {
                return 0;
            }
        }
    }

    bool Write(std::string_view data) override {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        while (!data.empty()) {
            if (stopped.load()) {
                return false;
            }
            const ssize_t count = send(client_fd, data.data(), data.size(), flags);
            if (count > 0) {
                data.remove_prefix(static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd fds[2] = {{client_fd, POLLOUT, 0}, {wake_read, POLLIN, 0}};
                if (poll(fds, 2, -1) < 0 && errno != EINTR) {
                    return false;
                }
                if (fds[1].revents != 0) {
                    return false;
                }
                continue;
            }
            return false;
        }
        return true;
    }

    void DropClient() override {
        CloseFd(client_fd);
    }

    void Stop() override {
        stopped.store(true);
        const char byte = 0;
        // Non-blocking: a full pipe already holds a wake-up.
        [[maybe_unused]] const ssize_t written = write(wake_write, &byte, 1);
    }

private:
    std::string path;
    bool bound = false;
    dev_t socket_device = 0; // what bind created, to remove only that
    ino_t socket_inode = 0;
    int lock_fd = -1;        // holds the per-name lock for the life of the endpoint
    int listen_fd = -1;
    int client_fd = -1;
    int wake_read = -1;
    int wake_write = -1;
    std::atomic<bool> stopped{false};
};

} // namespace

std::unique_ptr<Transport> MakeTransport() {
    return std::make_unique<UnixTransport>();
}

#endif

} // namespace Control
