// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string_view>

#include <boost/algorithm/string/replace.hpp>
#include <fmt/format.h>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/settings_secret.h"
#include "frontend_common/bug_bundle.h"
#include "frontend_common/bug_zip.h"

namespace FrontendCommon::BugBundle {
namespace {

constexpr u64 MiB = 1ULL << 20;
/// Room kept for bundle.json and README.md, which are written last.
constexpr u64 ManifestReserve = 256 * 1024;
/// A log over this keeps its first LogHead and last LogTail bytes.
constexpr u64 LogTrim = 8 * MiB;
constexpr u64 LogHead = 1 * MiB;
constexpr u64 LogTail = 6 * MiB;
/// The preview shows this many characters of a long text and says how long it is.
constexpr size_t PreviewExcerpt = 2000;

constexpr std::string_view Readme = R"(# Eglantine bug report

Written by Help > Report a Bug after you reviewed it. It stays on your computer: Eglantine never
sends it. Share it yourself, with the people you choose, and only after you read it.

- bundle.json: start here. Build, the game that was running, your description, the host and what
  each part holds.
- settings.txt: every setting except credentials, with the game's own overrides.
- screenshot.png: the screen when you asked for the report (only when you included it).
- logs/: the log of this session (only when you included it), with your user name replaced.

It holds no game files, saves, keys or firmware. A screenshot can show anything that was on screen.
)";

std::string Stamp(std::time_t time) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", local.tm_year + 1900, local.tm_mon + 1,
                       local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

std::string UtcStamp(std::time_t time) {
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", utc.tm_year + 1900, utc.tm_mon + 1,
                       utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
}

std::string JsonString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string Excerpt(const std::string& text) {
    if (text.size() <= PreviewExcerpt) {
        return text;
    }
    return fmt::format("{}\n[... {} more characters, all of them in the report ...]",
                       text.substr(0, PreviewExcerpt), text.size() - PreviewExcerpt);
}

std::string ManifestJson(const Report& report, std::time_t now, const std::string& parts,
                         const std::string& dropped, u64 size_limit) {
    std::string out = "{\n";
    out += "  \"format\": \"eglantine-bug-bundle\",\n  \"version\": 1,\n";
    out += fmt::format("  \"created\": {},\n  \"trigger\": {},\n",
                       JsonString(UtcStamp(now)), JsonString(report.trigger));
    out += fmt::format("  \"build\": {{\"name\": {}, \"scm_rev\": {}, \"branch\": {}}},\n",
                       JsonString(Common::BuildDisplayName()), JsonString(Common::g_scm_rev),
                       JsonString(Common::g_scm_branch));
    out += fmt::format("  \"game\": {{\"title\": {}, \"program_id\": {}, \"version\": {}}},\n",
                       JsonString(report.game_title), JsonString(report.game_program_id),
                       JsonString(report.game_version));
    out += "  \"system\": {";
    for (size_t i = 0; i < report.system.size(); ++i) {
        out += fmt::format("{}{}: {}", i == 0 ? "" : ", ", JsonString(report.system[i].first),
                           JsonString(report.system[i].second));
    }
    out += "},\n";
    out += fmt::format("  \"description\": {},\n", JsonString(report.description));
    out += fmt::format("  \"size_limit\": {},\n  \"parts\": [{}],\n  \"dropped\": [{}]\n}}\n",
                       size_limit, parts, dropped);
    return out;
}

} // Anonymous namespace

std::filesystem::path DefaultDirectory() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "bug-reports";
}

std::string Redact(std::string text) {
    for (const char* const variable : {"LOGNAME", "USERNAME", "USER"}) {
        if (const char* const name = std::getenv(variable);
            name != nullptr && std::string_view{name}.size() > 2) {
            boost::replace_all(text, name, "user");
        }
    }
    return text;
}

std::string SettingsReport(bool changed_only) {
    std::string out;
    for (const auto& [category, settings] : Settings::values.linkage.by_category) {
        if (category == Settings::Category::WebService ||
            category == Settings::Category::Multiplayer) {
            continue;
        }
        for (const Settings::BasicSetting* const setting : settings) {
            const std::string& label = setting->GetLabel();
            if (Settings::IsSecretSetting(label) || label.find("username") != std::string::npos) {
                continue;
            }
            if (changed_only && setting->ToString() == setting->DefaultToString()) {
                continue;
            }
            out += fmt::format("[{}] {} = {}", Settings::TranslateCategory(category), label,
                               setting->ToString());
            if (setting->Switchable() && !setting->UsingGlobal()) {
                out += fmt::format(" (global: {})", setting->ToStringGlobal());
            }
            out += '\n';
        }
    }
    return Redact(std::move(out));
}

std::string ReadLog(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return {};
    }
    std::string log{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (log.size() > LogTrim) {
        // Cut at line ends, so no word (a redacted user name included) is split.
        size_t head_end = log.rfind('\n', LogHead);
        head_end = head_end == std::string::npos ? LogHead : head_end + 1;
        size_t tail_begin = log.find('\n', log.size() - LogTail);
        tail_begin = tail_begin == std::string::npos ? log.size() - LogTail : tail_begin + 1;
        log = fmt::format("{}[... {:.1f} MiB cut from the middle by the bug report ...]\n{}",
                          log.substr(0, head_end),
                          static_cast<double>(tail_begin - head_end) / MiB,
                          log.substr(tail_begin));
    }
    return Redact(std::move(log));
}

std::string Preview(const Report& report) {
    std::string out =
        "Nothing is sent. This is a summary of what the report file will hold: long texts are "
        "shortened and the screenshot is not shown. Open the ZIP and inspect it before you share "
        "it.\n\n";
    out += fmt::format("== Build ==\n{} ({})\n\n", Common::BuildDisplayName(), Common::g_scm_rev);
    out += "== Game ==\n";
    if (report.game_title.empty() && report.game_program_id.empty()) {
        out += "(no game running)\n";
    } else {
        out += fmt::format("{}\n{}  {}\n", report.game_title, report.game_program_id,
                           report.game_version);
    }
    out += "\n== Your description ==\n";
    out += report.description.empty() ? std::string{"(empty)\n"} : report.description + "\n";
    out += "\n== This computer ==\n";
    for (const auto& [key, value] : report.system) {
        out += fmt::format("{}: {}\n", key, value);
    }
    out += fmt::format("\n== Settings (settings.txt, {} bytes, excerpt) ==\n{}\n",
                       report.settings.size(), Excerpt(report.settings));
    out += report.screenshot_png.empty()
               ? std::string{"\n== Screenshot ==\n(not included)\n"}
               : fmt::format("\n== Screenshot ==\nscreenshot.png, {} bytes (not shown here). It "
                             "can show anything that was on screen: open it and check it.\n",
                             report.screenshot_png.size());
    out += report.log.empty()
               ? std::string{"\n== Log ==\n(not included)\n"}
               : fmt::format("\n== Log (logs/{}, {} bytes, user name replaced) ==\n{}\n",
                             report.log_name, report.log.size(), Excerpt(report.log));
    return out;
}

Result WriteBundle(const Report& report, const std::filesystem::path& directory, u64 size_limit) {
    std::filesystem::create_directories(directory);
    const std::time_t now = std::time(nullptr);
    Result result{.path = directory / fmt::format("eglantine-bug-{}.zip", Stamp(now))};
    auto partial = result.path;
    partial += ".partial";
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::remove(partial, error);
    };

    ZipWriter zip{partial};
    std::string parts;
    std::string dropped;
    const u64 budget = size_limit > ManifestReserve ? size_limit - ManifestReserve : 0;

    const auto add = [&](std::string name, std::span<const u8> data, bool compress) {
        ZipWriter::Entry entry = ZipWriter::Prepare(std::move(name), data, compress);
        if (zip.Size() + entry.ZipBytes() > budget) {
            return false;
        }
        zip.Add(entry);
        parts += fmt::format("{}{{\"path\": {}, \"bytes\": {}}}", parts.empty() ? "" : ", ",
                             JsonString(entry.name), data.size());
        return true;
    };
    const auto drop = [&](const std::string& what, u64 bytes) {
        result.dropped.push_back(
            fmt::format("{}: {:.1f} MiB", what, static_cast<double>(bytes) / MiB));
        dropped += fmt::format("{}{{\"what\": {}, \"bytes\": {}, \"reason\": \"size limit\"}}",
                               dropped.empty() ? "" : ", ", JsonString(what), bytes);
    };

    if (!add("settings.txt", Bytes(report.settings), true)) {
        drop("settings.txt", report.settings.size());
    }
    if (!report.screenshot_png.empty() && !add("screenshot.png", report.screenshot_png, false)) {
        drop("screenshot.png", report.screenshot_png.size());
    }
    if (!report.log.empty()) {
        const std::string name = "logs/" + report.log_name;
        if (!add(name, Bytes(report.log), true)) {
            drop(name, report.log.size());
        }
    }
    zip.Add(ZipWriter::Prepare("README.md", Bytes(Readme), true));
    parts += fmt::format("{}{{\"path\": \"README.md\", \"bytes\": {}}}", parts.empty() ? "" : ", ",
                         Readme.size());
    zip.Add(ZipWriter::Prepare("bundle.json",
                               Bytes(ManifestJson(report, now, parts, dropped, size_limit)), true));
    zip.Finish();
    std::filesystem::rename(partial, result.path);
    result.bytes = std::filesystem::file_size(result.path);
    return result;
}

} // namespace FrontendCommon::BugBundle
