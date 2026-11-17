// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "common/scm_rev.h"
#include "yuzu_cmd/run_report.h"
#include "yuzu_cmd/version_info.h"

namespace VersionInfo {

namespace {

using Json = nlohmann::json;

// Set by yuzu_cmd/CMakeLists.txt from the build options of the same name, or by the targets
// yuzu-cmd links (video_core, core).
#ifdef EGLANTINE_BUILD_UPDATE_CHECKER
constexpr bool kUpdateChecker = true;
#else
constexpr bool kUpdateChecker = false;
#endif
#ifdef EGLANTINE_BUILD_DISCORD_PRESENCE
constexpr bool kDiscordPresence = true;
#else
constexpr bool kDiscordPresence = false;
#endif
#ifdef YUZU_ENABLE_MCP
constexpr bool kMcpServer = true;
#else
constexpr bool kMcpServer = false;
#endif
#ifdef HAS_OPENGL
constexpr bool kOpenGl = true;
#else
constexpr bool kOpenGl = false;
#endif
#ifdef ENABLE_WEB_SERVICE
constexpr bool kWebService = true;
#else
constexpr bool kWebService = false;
#endif

/// A constant of the build as a string, or null when the build recorded nothing for it.
Json TextOrNull(const char* text) {
    return text[0] != '\0' ? Json(text) : Json(nullptr);
}

} // Anonymous namespace

std::string BuildInfoJson() {
    Json options = Json::object();
    options["discord_presence"] = kDiscordPresence;
    options["mcp_server"] = kMcpServer;
    options["opengl"] = kOpenGl;
    options["update_checker"] = kUpdateChecker;
    options["web_service"] = kWebService;

    Json document = Json::object();
    document["name"] = std::string(Common::g_build_name);
    document["fork_version"] = TextOrNull(Common::g_display_version);
    document["flavour"] = TextOrNull(Common::g_build_flavour);
    document["revision"] = std::string(Common::g_scm_rev);
    document["branch"] = std::string(Common::g_scm_branch);
    document["description"] = std::string(Common::g_scm_desc);
    document["upstream_base"] = TextOrNull(Common::g_upstream_base);
    document["build_id"] = std::string(Common::g_build_id);
    document["build_date"] = std::string(Common::g_build_date);
    document["compiler"] = std::string(Common::g_compiler_id);
    document["report_schema_version"] = RunReport::kSchemaVersion;
    document["options"] = std::move(options);
    return document.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
}

} // namespace VersionInfo
