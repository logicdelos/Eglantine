// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace Control {

/// A local, one-client byte stream for the control channel: a named pipe on Windows, a Unix domain
/// socket elsewhere. Only the owner of the process can connect. Everything except Stop is called
/// from the channel thread alone.
class Transport {
public:
    virtual ~Transport() = default;

    /// Creates the endpoint. `name` is already checked to be 1..64 of [A-Za-z0-9._-]. Returns an
    /// error text, empty on success.
    [[nodiscard]] virtual std::string Open(const std::string& name) = 0;

    /// Where clients connect, for the log.
    [[nodiscard]] virtual std::string Address() const = 0;

    /// Blocks until a client is connected. False once Stop was called or the endpoint failed.
    [[nodiscard]] virtual bool WaitForClient() = 0;

    /// Blocks until at least one byte arrives. Returns the number of bytes, or 0 when the client
    /// has left, the stream failed or Stop was called.
    [[nodiscard]] virtual std::size_t Read(char* buffer, std::size_t size) = 0;

    /// Writes all of `data`. False when the client has left, the stream failed or Stop was called.
    [[nodiscard]] virtual bool Write(std::string_view data) = 0;

    /// Drops the current client. The next WaitForClient accepts another one.
    virtual void DropClient() = 0;

    /// Wakes every blocked call (they fail) and makes later ones fail at once. Any thread.
    virtual void Stop() = 0;
};

/// The transport of this platform.
[[nodiscard]] std::unique_ptr<Transport> MakeTransport();

} // namespace Control
