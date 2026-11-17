// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <SDL3/SDL_keyboard.h>

#include "common/key_script.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_common.h"
#include "common/settings_secret.h"
#include "common/run_outcome.h"
#include "core/core.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/main.h"
#include "video_core/gpu.h"
#include "video_core/shader_notify.h"
#include "yuzu_cmd/cli_settings.h"
#include "yuzu_cmd/control_commands.h"
#include "yuzu_cmd/control_file.h"
#include "yuzu_cmd/frame_capture.h"

namespace Control {

using namespace std::chrono_literals;

namespace {

constexpr int MaxKeyCode = 511; // the keyboard driver's table size
constexpr int MaxHoldMs = 60'000;

/// The key of a press or release: an SDL scancode number, or an SDL scancode name ("Return").
bool ParseKey(const Json& args, int& code, std::string& error) {
    const auto key = args.find("key");
    if (key == args.end()) {
        error = "\"key\" is required: an SDL scancode number or name";
        return false;
    }
    if (key->is_number_integer()) {
        const long long value = key->get<long long>();
        if (value < 1 || value > MaxKeyCode) {
            error = fmt::format("\"key\" must be between 1 and {}", MaxKeyCode);
            return false;
        }
        code = static_cast<int>(value);
        return true;
    }
    if (key->is_string()) {
        const std::string name = key->get<std::string>();
        const int scancode = static_cast<int>(SDL_GetScancodeFromName(name.c_str()));
        if (scancode <= 0 || scancode > MaxKeyCode) {
            error = fmt::format("\"{}\" is not an SDL scancode name", name);
            return false;
        }
        code = scancode;
        return true;
    }
    error = "\"key\" must be a number or a string";
    return false;
}

/// An optional integer argument in [low, high]. False with `error` set when it is wrong.
bool ParseInteger(const Json& args, const char* name, long long low, long long high,
                  std::optional<long long>& out, std::string& error) {
    const auto it = args.find(name);
    if (it == args.end() || it->is_null()) {
        return true;
    }
    if (!it->is_number_integer() || it->get<long long>() < low || it->get<long long>() > high) {
        error = fmt::format("\"{}\" must be an integer from {} to {}", name, low, high);
        return false;
    }
    out = it->get<long long>();
    return true;
}

} // namespace

Commands::Commands(Core::System& system_, InputCommon::InputSubsystem& input_,
                   std::function<void()> request_stop_)
    : system{system_}, input{input_}, request_stop{std::move(request_stop_)},
      started{std::chrono::steady_clock::now()},
      channel{MakeTransport(), [this](const Request& request) { return HandleDirect(request); }} {}

std::string Commands::Start(const std::string& name) {
    return channel.Start(name);
}

std::optional<Response> Commands::HandleDirect(const Request& request) {
    if (request.cmd == "status") {
        return Status();
    }
    if (request.cmd == "press" || request.cmd == "release" || request.cmd == "screenshot" ||
        request.cmd == "get" || request.cmd == "set" || request.cmd == "stop") {
        return std::nullopt; // needs the main thread
    }
    return Response::Error("unknown_command", fmt::format("unknown command \"{}\"", request.cmd));
}

Response Commands::HandleMain(const Request& request) {
    if (request.cmd == "press") {
        return Key(request, true);
    }
    if (request.cmd == "release") {
        return Key(request, false);
    }
    if (request.cmd == "screenshot") {
        return Screenshot(request);
    }
    if (request.cmd == "get" || request.cmd == "set") {
        return Setting(request, request.cmd == "set");
    }
    return Stop();
}

// Channel thread. Atomics and the process-wide run state only: nothing here may touch the
// emulation, the GPU or the input subsystem.
Response Commands::Status() const {
    const u64 frames = Common::RunOutcome::FramesPresented();
    const bool stopping = stop_requested.load() || Common::RunOutcome::StopRequested();
    Json result = Json::object();
    result["protocol"] = 1;
    result["state"] = stopping ? "stopping" : (frames == 0 ? "loading" : "running");
    result["frames"] = frames;
    result["seconds"] =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const bool active = Common::RunOutcome::Active();
    result["unattended"] = active;
    result["run_seconds"] = active ? Json(Common::RunOutcome::ElapsedSeconds()) : Json(nullptr);
    const int building = shaders_building.load();
    result["shaders_building"] = building >= 0 ? Json(building) : Json(nullptr);
    if (const Common::KeyScript::Runner* script = Common::KeyScript::Current()) {
        const Common::KeyScript::Progress progress = script->GetProgress();
        Json steps = Json::object();
        steps["total"] = progress.total;
        steps["fired"] = progress.fired;
        steps["finished"] = progress.finished;
        result["script"] = std::move(steps);
    } else {
        result["script"] = nullptr;
    }
    return Response::Ok(std::move(result));
}

Response Commands::Key(const Request& request, bool press) {
    int code = 0;
    std::string error;
    if (!ParseKey(request.args, code, error)) {
        return Response::Error("bad_args", error);
    }
    std::optional<long long> hold_ms;
    if (press && !ParseInteger(request.args, "hold_ms", 1, MaxHoldMs, hold_ms, error)) {
        return Response::Error("bad_args", error);
    }
    auto* keyboard = input.GetKeyboard();
    if (keyboard == nullptr) {
        return Response::Error("unavailable", "the keyboard input is not available");
    }
    // At most one record per key: a command about a key replaces what an earlier one left, so an
    // old deadline can never release a later press.
    keys.erase(code);
    if (press) {
        keyboard->PressKey(code);
        KeyHold hold;
        hold.session = request.session;
        if (hold_ms) {
            hold.due = std::chrono::steady_clock::now() + std::chrono::milliseconds{*hold_ms};
        }
        keys[code] = hold;
    } else {
        keyboard->ReleaseKey(code);
    }
    LOG_INFO(Frontend, "Control: {} key {}", press ? "press" : "release", code);
    Json result = Json::object();
    result["key"] = code;
    return Response::Ok(std::move(result));
}

// Main thread: starts the capture and returns at once. The reply is completed on the channel
// thread (Response::finish): it waits for the frame, encodes the PNG and writes the file.
Response Commands::Screenshot(const Request& request) {
    const auto path_arg = request.args.find("path");
    if (path_arg == request.args.end() || !path_arg->is_string() ||
        path_arg->get<std::string>().empty()) {
        return Response::Error("bad_args", "\"path\" is required: the PNG file to write");
    }
    const std::string path_text = path_arg->get<std::string>();
    std::string error;
    std::optional<long long> width;
    std::optional<long long> timeout_ms;
    if (!ParseInteger(request.args, "width", 160, 3840, width, error) ||
        !ParseInteger(request.args, "timeout_ms", 100, 60'000, timeout_ms, error)) {
        return Response::Error("bad_args", error);
    }
    const auto overwrite_arg = request.args.find("overwrite");
    const bool overwrite =
        overwrite_arg != request.args.end() && overwrite_arg->is_boolean() && overwrite_arg->get<bool>();
    const std::filesystem::path path{std::u8string{path_text.begin(), path_text.end()}};
    std::error_code ec;
    if (!overwrite && std::filesystem::exists(path, ec)) {
        return Response::Error("exists", "the file exists; pass \"overwrite\": true to replace it");
    }

    auto begun = FrameCapture::Begin(system, static_cast<u32>(width.value_or(1280)), error);
    if (!begun) {
        return Response::Error("unavailable", error);
    }
    auto pending = std::make_shared<FrameCapture::Pending>(std::move(*begun));
    const auto timeout = std::chrono::milliseconds{timeout_ms.value_or(10'000)};

    Response response = Response::Ok();
    response.finish = [this, pending, path, path_text, timeout, overwrite](Response& out) {
        std::string wait_error;
        std::optional<FrameCapture::Frame> frame;
        // In slices, so that shutting down does not wait for the whole timeout.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!frame && std::chrono::steady_clock::now() < deadline && !channel.Stopping()) {
            frame = pending->Wait(100ms, wait_error);
        }
        if (!frame) {
            // The fields are set one by one: `out` owns this very function object.
            out.error_code = channel.Stopping() ? "shutting_down" : "timeout";
            out.error_message =
                channel.Stopping() ? "the emulator is shutting down" : wait_error;
            return;
        }
        const u32 width_px = frame->width;
        const u32 height_px = frame->height;
        const std::vector<u8> png =
            FrameCapture::EncodePng(width_px, height_px, std::move(frame->bgra));
        // The PNG goes to a new temporary file and is moved into place; the destination itself
        // is never opened, and without `overwrite` it is never replaced.
        const PublishOutcome published = PublishFile(
            path, std::string_view{reinterpret_cast<const char*>(png.data()), png.size()},
            overwrite);
        if (published.result != PublishResult::Ok) {
            out.error_code = published.result == PublishResult::Exists
                                 ? "exists"
                                 : (published.result == PublishResult::NotRegular ? "bad_args"
                                                                                  : "io_error");
            out.error_message = fmt::format("{}: {}", path_text, published.message);
            return;
        }
        Json result = Json::object();
        result["path"] = path_text;
        result["width"] = width_px;
        result["height"] = height_px;
        result["bytes"] = png.size();
        out.result = std::move(result);
        LOG_INFO(Frontend, "Control: screenshot {}x{} written to {}", width_px, height_px,
                 path_text);
    };
    return response;
}

// Main thread, so that the setting is not read or written while Poll is doing something else
// with the emulator. `get` describes a setting like one line of --list-settings; `set` also
// changes it, and only when the registry marks the setting as changeable while the guest runs.
Response Commands::Setting(const Request& request, bool change) {
    const auto name_arg = request.args.find("name");
    if (name_arg == request.args.end() || !name_arg->is_string() ||
        name_arg->get<std::string>().empty()) {
        return Response::Error("bad_args", "\"name\" is required: the name of a setting "
                                           "(eden-cli --list-settings shows them)");
    }
    const std::string name = name_arg->get<std::string>();
    const auto found = Settings::values.linkage.by_key.find(name);
    if (found == Settings::values.linkage.by_key.end()) {
        return Response::Error("bad_args", fmt::format("{}: unknown setting", name));
    }
    Settings::BasicSetting& setting = *found->second;

    if (change) {
        if (Settings::IsSecretSetting(name)) {
            return Response::Error("not_modifiable",
                                   fmt::format("{}: a credential is not read or changed through "
                                               "the control channel",
                                               name));
        }
        if (!setting.RuntimeModifiable()) {
            return Response::Error(
                "not_modifiable",
                fmt::format("{}: this setting cannot be changed while the emulator runs; start "
                            "eden-cli with --set {}=<value> instead",
                            name, name));
        }
        const auto value_arg = request.args.find("value");
        if (value_arg == request.args.end()) {
            return Response::Error("bad_args", "\"value\" is required");
        }
        std::string text;
        if (value_arg->is_string()) {
            text = value_arg->get<std::string>();
        } else if (value_arg->is_boolean()) {
            text = value_arg->get<bool>() ? "true" : "false";
        } else if (value_arg->is_number()) {
            text = value_arg->dump();
        } else {
            return Response::Error("bad_args", "\"value\" must be a boolean, a number or a string");
        }
        std::string reason;
        const std::optional<std::string> canonical = CliSettings::CanonicalText(setting, text, reason);
        if (!canonical) {
            return Response::Error("bad_args", fmt::format("{}: invalid value: {}", name, reason));
        }
        setting.LoadString(*canonical);
        if (system.IsPoweredOn()) {
            system.ApplySettings();
        }
        // The value itself is not logged.
        LOG_INFO(Frontend, "Control: set {}", name);
    }

    Json result = CliSettings::Describe(name, setting);
    result["runtime_modifiable"] = setting.RuntimeModifiable();
    return Response::Ok(std::move(result));
}

Response Commands::Stop() {
    LOG_INFO(Frontend, "Control: stop");
    stop_requested.store(true);
    request_stop();
    Json result = Json::object();
    result["stopping"] = true;
    Response response = Response::Ok(std::move(result));
    // The reply is written before the main loop ends and the channel is shut down.
    response.flush_reply = true;
    return response;
}

// Clients left without releasing keys: release the ones they hold without a time limit. `first`
// to `last` are session numbers, both included; a key of a later session stays. A key with a
// hold_ms deadline is a tap that finishes by itself, and stays until its deadline.
void Commands::ReleaseSessions(std::uint64_t first, std::uint64_t last) {
    auto* keyboard = input.GetKeyboard();
    for (auto it = keys.begin(); it != keys.end();) {
        if (it->second.session >= first && it->second.session <= last && !it->second.due) {
            if (keyboard != nullptr) {
                keyboard->ReleaseKey(it->first);
            }
            LOG_INFO(Frontend, "Control: released key {} (the client left)", it->first);
            it = keys.erase(it);
        } else {
            ++it;
        }
    }
}

// Main thread, once per main loop iteration.
void Commands::Poll() {
    if (system.IsPoweredOn()) {
        shaders_building.store(static_cast<int>(system.GPU().ShaderNotify().NumBuildingNoReport()));
    }
    channel.Poll([this](const Request& request) { return HandleMain(request); },
                 [this](std::uint64_t first, std::uint64_t last) { ReleaseSessions(first, last); });

    const auto now = std::chrono::steady_clock::now();
    auto* keyboard = input.GetKeyboard();
    for (auto it = keys.begin(); it != keys.end();) {
        if (it->second.due && *it->second.due <= now) {
            if (keyboard != nullptr) {
                keyboard->ReleaseKey(it->first);
            }
            it = keys.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace Control
