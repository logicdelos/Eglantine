// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/util/icon_search.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/algorithm/string/replace.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "common/fs/fs_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/string_util.h"
#include "qt_common/config/uisettings.h"
#include "common/net/ca_store.h"

namespace {

constexpr int IconTargetSize = 256;
constexpr int MinCandidateScore = 300;
constexpr std::size_t MaxIconDownloads = 3;
constexpr std::size_t MaxSgdbChoices = 8;

std::vector<u8> EncodeSquareIcon(const QImage& source) {
    if (source.isNull()) {
        return {};
    }
    QImage img = source.convertToFormat(QImage::Format_RGBA8888);
    if (img.width() != IconTargetSize || img.height() != IconTargetSize) {
        img = img.scaled(IconTargetSize, IconTargetSize, Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
        if (img.width() > IconTargetSize || img.height() > IconTargetSize) {
            const int x = (img.width() - IconTargetSize) / 2;
            const int y = img.height() > img.width() ? (img.height() - IconTargetSize) / 3
                                                     : (img.height() - IconTargetSize) / 2;
            img = img.copy(x, y, IconTargetSize, IconTargetSize);
        }
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return std::vector<u8>(encoded.begin(), encoded.end());
}

std::string NormalizeGameSlug(std::string title) {
    title = Common::ToLower(title);
    boost::replace_all(title, "é", "e");
    boost::replace_all(title, "è", "e");
    boost::replace_all(title, "ê", "e");
    boost::replace_all(title, "ō", "o");
    std::replace(title.begin(), title.end(), ' ', '-');
    std::replace(title.begin(), title.end(), '_', '-');
    std::erase_if(title, [](unsigned char c) { return !std::isalnum(c) && c != '-'; });
    title.erase(title.begin(),
                std::find_if(title.begin(), title.end(), [](char ch) { return ch != '-'; }));
    title.erase(std::find_if(title.rbegin(), title.rend(), [](char ch) { return ch != '-'; }).base(),
                title.end());
    title.erase(std::unique(title.begin(), title.end(),
                            [](char a, char b) { return a == '-' && b == '-'; }),
                title.end());
    return title;
}

std::string ResolveGameNameAlias(const std::string& raw_name, u64 title_id) {
    static const std::unordered_map<u64, std::string> by_title_id{
        {0x0400C3F00006E000ULL, "Mario Kart World"},
        {0x0100000000010000ULL, "Super Mario Odyssey"},
        {0x0100152000022000ULL, "Mario Kart 8 Deluxe"},
        {0x01007EF00011E000ULL, "The Legend of Zelda: Breath of the Wild"},
        {0x0100F2C0115B6000ULL, "The Legend of Zelda: Tears of the Kingdom"},
        {0x01006A800016E000ULL, "Super Smash Bros. Ultimate"},
        {0x01006F8002326000ULL, "Animal Crossing: New Horizons"},
    };
    static const std::unordered_map<std::string, std::string> by_alias{
        {"mkw", "Mario Kart World"},
        {"mariokartworld", "Mario Kart World"},
        {"mario-kart-world", "Mario Kart World"},
        {"dkb", "Donkey Kong Bananza"},
        {"donkeykongbananza", "Donkey Kong Bananza"},
        {"kar", "Kirby Air Riders"},
        {"kirbyairriders", "Kirby Air Riders"},
        {"mp4", "Metroid Prime 4: Beyond"},
        {"metroidprime4", "Metroid Prime 4: Beyond"},
        {"plza", "Pokemon Legends: Z-A"},
        {"pokemonlegendsza", "Pokemon Legends: Z-A"},
        {"mk8", "Mario Kart 8 Deluxe"},
        {"mk8d", "Mario Kart 8 Deluxe"},
        {"smo", "Super Mario Odyssey"},
        {"botw", "The Legend of Zelda: Breath of the Wild"},
        {"totk", "The Legend of Zelda: Tears of the Kingdom"},
        {"ssbu", "Super Smash Bros. Ultimate"},
        {"acnh", "Animal Crossing: New Horizons"},
        {"smbw", "Super Mario Bros. Wonder"},
        {"splatoon3", "Splatoon 3"},
        {"xc3", "Xenoblade Chronicles 3"},
        {"lm3", "Luigi's Mansion 3"},
        {"pikmin4", "Pikmin 4"},
    };
    const std::string slug = NormalizeGameSlug(raw_name);
    if (const auto it = by_alias.find(slug); it != by_alias.end()) {
        return it->second;
    }
    if (slug.empty()) {
        if (const auto it = by_title_id.find(title_id); it != by_title_id.end()) {
            return it->second;
        }
    }
    return raw_name;
}

std::string UrlEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

constexpr std::chrono::seconds SearchDeadline{10};
constexpr std::size_t MaxHttpBodySize = 4 * 1024 * 1024;
constexpr std::chrono::seconds PrimarySourceDeadline{5};
constexpr std::string_view IconSiteHost = "https://switchiconshowdown.com";
constexpr std::string_view SgdbApiBase = "https://www.steamgriddb.com/api/v2";

bool IsAllowedIconHost(std::string_view host) {
    return host == "switchiconshowdown.com" || host == "steamgriddb.com" ||
           host.ends_with(".steamgriddb.com");
}

enum class HttpStatus { Ok, NetworkError, Timeout, Cancelled, Unauthorized };

struct HttpResult {
    HttpStatus status{HttpStatus::NetworkError};
    std::string body;
    std::string detail;
};

// cpp-httplib cannot be interrupted from another thread while it connects or waits for the first
// bytes of a reply, so these waits are kept short: a cancelled lookup ends within them instead of
// at the deadline of the whole search. A download in progress stops on cancel right away.
constexpr std::chrono::milliseconds ConnectTimeout{5000};
constexpr std::chrono::milliseconds ReadTimeout{8000};
constexpr int MaxRedirects = 3;

struct SplitUrl {
    std::string origin; // "https://host"
    std::string host;
    std::string path;
};

std::optional<SplitUrl> SplitHttpsUrl(const std::string& url) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) {
        return std::nullopt;
    }
    const auto path_start = url.find('/', scheme.size());
    SplitUrl out;
    out.origin = path_start == std::string::npos ? url : url.substr(0, path_start);
    out.host = out.origin.substr(scheme.size());
    out.path = path_start == std::string::npos ? std::string{"/"} : url.substr(path_start);
    if (out.host.empty() || out.host.find_first_of(":@") != std::string::npos) {
        return std::nullopt;
    }
    return out;
}

struct HttpOptions {
    bool (*allowed_host)(std::string_view host) = IsAllowedIconHost;
    std::size_t max_body = MaxHttpBodySize;
    /// Upper bound of one request; zero means only the shared deadline applies.
    std::chrono::milliseconds max_request{0};
};

// Every request shares one deadline and cancel flag, so a whole search can never run past it.
// Redirects are followed by hand: every hop must be https to an allowed host, and the bearer token
// is only sent to the host it was given for.
HttpResult HttpGet(const std::string& full_url, std::chrono::steady_clock::time_point deadline,
                   const std::atomic_bool& cancel, const std::string& bearer_token = {},
                   const HttpOptions& options = {}) {
    using namespace std::chrono;
    HttpResult result;
    std::string url = full_url;
    const auto first = SplitHttpsUrl(url);
    const std::string token_host = first ? first->host : std::string{};
    for (int hop = 0; hop <= MaxRedirects; ++hop) {
        if (cancel) {
            result.status = HttpStatus::Cancelled;
            return result;
        }
        const auto split = SplitHttpsUrl(url);
        if (!split) {
            result.status = HttpStatus::NetworkError;
            result.detail = "unsupported URL";
            return result;
        }
        if (!options.allowed_host(split->host)) {
            result.status = HttpStatus::NetworkError;
            result.detail = "unexpected host";
            return result;
        }
        auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
        if (options.max_request.count() > 0) {
            remaining = std::min(remaining, options.max_request);
        }
        if (remaining.count() <= 0) {
            result.status = HttpStatus::Timeout;
            return result;
        }
        try {
            httplib::Client client(split->origin);
            client.set_connection_timeout(std::min(remaining, ConnectTimeout));
            client.set_read_timeout(std::min(remaining, ReadTimeout));
            client.set_write_timeout(std::min(remaining, ConnectTimeout));
            client.set_max_timeout(remaining);
            client.set_follow_location(false);
            Common::Net::ConfigureCaStore(client);
            httplib::Headers headers{{"User-Agent", "Eglantine"}};
            if (!bearer_token.empty() && split->host == token_host) {
                headers.emplace("Authorization", "Bearer " + bearer_token);
            }
            // Stops a download in progress as soon as cancel is set or the reply grows too big.
            auto res = client.Get(split->path, headers,
                                  [&cancel, &options](uint64_t current, uint64_t) {
                                      return !cancel.load() && current <= options.max_body;
                                  });
            if (!res) {
                result.status = cancel ? HttpStatus::Cancelled
                                : res.error() == httplib::Error::Canceled ||
                                        steady_clock::now() >= deadline
                                    ? HttpStatus::Timeout
                                    : HttpStatus::NetworkError;
                result.detail = httplib::to_string(res.error());
                return result;
            }
            if (res->status == 301 || res->status == 302 || res->status == 303 ||
                res->status == 307 || res->status == 308) {
                std::string location = res->get_header_value("Location");
                if (location.starts_with("/") && !location.starts_with("//")) {
                    location = split->origin + location;
                }
                if (location.empty() || hop == MaxRedirects) {
                    result.status = HttpStatus::NetworkError;
                    result.detail = fmt::format("HTTP {} redirect refused", res->status);
                    return result;
                }
                url = std::move(location);
                continue;
            }
            if (res->status != 200) {
                result.status = res->status == 401 || res->status == 403
                                    ? HttpStatus::Unauthorized
                                    : HttpStatus::NetworkError;
                result.detail = fmt::format("HTTP {}", res->status);
                return result;
            }
            if (res->body.empty() || res->body.size() > options.max_body) {
                result.status = HttpStatus::NetworkError;
                result.detail = "unexpected response size";
                return result;
            }
            result.status = HttpStatus::Ok;
            result.body = std::move(res->body);
            return result;
        } catch (const std::exception& e) {
            result.status = HttpStatus::NetworkError;
            result.detail = e.what();
            return result;
        } catch (...) {
            result.status = HttpStatus::NetworkError;
            result.detail = "unknown error";
            return result;
        }
    }
    result.status = HttpStatus::NetworkError;
    result.detail = "too many redirects";
    return result;
}

// Decodes untrusted image bytes only when they are a common picture format of a sane size.
QImage DecodeBoundedImage(const std::string& bytes) {
    constexpr std::size_t MaxBytes = 8 * 1024 * 1024;
    constexpr int MaxSide = 8192;
    constexpr qint64 MaxPixels = 16LL * 1024 * 1024;
    if (bytes.empty() || bytes.size() > MaxBytes) {
        return {};
    }
    QByteArray data = QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size()));
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QByteArray format = reader.format();
    if (format != "png" && format != "jpeg" && format != "bmp" && format != "webp") {
        return {};
    }
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > MaxSide || size.height() > MaxSide ||
        static_cast<qint64>(size.width()) * size.height() > MaxPixels) {
        return {};
    }
    QImage image;
    if (!reader.read(&image)) {
        return {};
    }
    return image;
}

struct IconCandidate {
    int id{};
    std::string title;
};

std::string DecodeHtmlEntities(std::string text) {
    static constexpr std::pair<std::string_view, std::string_view> entities[]{
        {"&amp;", "&"},  {"&lt;", "<"},     {"&gt;", ">"},     {"&quot;", "\""},
        {"&#039;", "'"}, {"&#39;", "'"},    {"&apos;", "'"},   {"&eacute;", "\xC3\xA9"},
    };
    for (const auto& [from, to] : entities) {
        boost::replace_all(text, from, to);
    }
    return text;
}

// Rows look like: <a href='./detail.php?id=N'><img ...></a></td><td><center>Title</center>
std::vector<IconCandidate> ParseSearchResults(const std::string& html) {
    constexpr std::string_view id_marker = "detail.php?id=";
    constexpr std::string_view title_marker = "<td><center>";
    std::vector<IconCandidate> out;
    std::size_t pos = 0;
    while (out.size() < 16) {
        const auto id_pos = html.find(id_marker, pos);
        if (id_pos == std::string::npos) {
            break;
        }
        pos = id_pos + id_marker.size();
        const auto id_end = html.find_first_not_of("0123456789", pos);
        const auto digits = html.substr(pos, id_end == std::string::npos ? id_end : id_end - pos);
        if (digits.empty() || digits.size() > 9) {
            continue;
        }
        const auto title_pos = html.find(title_marker, pos);
        if (title_pos == std::string::npos) {
            break;
        }
        const auto title_begin = title_pos + title_marker.size();
        const auto title_end = html.find("</center>", title_begin);
        if (title_end == std::string::npos) {
            break;
        }
        auto title = DecodeHtmlEntities(html.substr(title_begin, title_end - title_begin));
        if (title.find('<') == std::string::npos && !title.empty()) {
            out.push_back({std::stoi(digits), std::move(title)});
        }
        pos = title_end;
    }
    return out;
}

std::string UrlEncodePathSegment(std::string value) {
    std::ranges::replace(value, '/', ' ');
    std::ranges::replace(value, '\\', ' ');
    auto encoded = UrlEncode(value);
    boost::replace_all(encoded, "+", "%20");
    return encoded;
}

// Icon image URLs from a SteamGridDB response, best size first.
std::vector<std::string> ParseSgdbIconUrls(const std::string& body) {
    std::vector<std::string> urls;
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("data") ||
        !json["data"].is_array()) {
        return urls;
    }
    for (const auto& item : json["data"]) {
        if (urls.size() >= MaxSgdbChoices) {
            break;
        }
        if (item.is_object() && item.contains("url") && item["url"].is_string()) {
            urls.push_back(item["url"].get<std::string>());
        }
    }
    return urls;
}

int ScoreCandidateTitle(const std::string& candidate, const std::string& query) {
    const std::string c_slug = NormalizeGameSlug(candidate);
    const std::string q_slug = NormalizeGameSlug(query);
    if (c_slug.empty() || q_slug.empty()) {
        return -1000;
    }
    if (c_slug == q_slug) {
        return 1000;
    }
    int score = 0;
    if (c_slug.starts_with(q_slug + "-") || q_slug.starts_with(c_slug + "-")) {
        score += 750 - static_cast<int>(std::abs(static_cast<long>(c_slug.size()) -
                                                 static_cast<long>(q_slug.size())));
    } else if (c_slug.find(q_slug) != std::string::npos) {
        score += 600;
    }
    const auto split_words = [](const std::string& s) {
        std::set<std::string> words;
        std::size_t start = 0;
        while (start < s.size()) {
            const auto dash = s.find('-', start);
            const auto word = s.substr(start, dash - start);
            if (!word.empty()) {
                words.insert(word);
            }
            if (dash == std::string::npos) {
                break;
            }
            start = dash + 1;
        }
        return words;
    };
    const auto q_words = split_words(q_slug);
    const auto c_words = split_words(c_slug);
    int overlap = 0;
    for (const auto& w : q_words) {
        if (c_words.contains(w)) {
            ++overlap;
        }
    }
    score += overlap * 120 - static_cast<int>(c_words.size()) * 10;
    const std::string lower_cand = Common::ToLower(candidate);
    if (lower_cand.find("(series)") != std::string::npos ||
        lower_cand.find("(character)") != std::string::npos ||
        lower_cand.find("soundtrack") != std::string::npos) {
        score -= 400;
    }
    return score;
}


IconSearchResult RunIconSearch(const std::string& raw_query, u64 tid, const std::string& api_key,
                               const std::atomic_bool& cancel) {
    using namespace std::chrono;
    IconSearchResult out;
    try {
        const auto start = steady_clock::now();
        const auto deadline = start + SearchDeadline;
        const std::string search_name = ResolveGameNameAlias(raw_query, tid);
        if (search_name != raw_query) {
            out.resolved_title = search_name;
        }
        if (search_name.empty()) {
            out.status = IconSearchStatus::NotFound;
            return out;
        }

        const auto to_status = [](const HttpResult& res) {
            switch (res.status) {
            case HttpStatus::Timeout:
                return IconSearchStatus::Timeout;
            case HttpStatus::Cancelled:
                return IconSearchStatus::Cancelled;
            case HttpStatus::Unauthorized:
                return IconSearchStatus::BadApiKey;
            default:
                return IconSearchStatus::NetworkError;
            }
        };
        const auto decode_icon = [&out](const std::string& body) {
            out.icon_bytes = EncodeSquareIcon(DecodeBoundedImage(body));
            return !out.icon_bytes.empty();
        };

        // Primary: switchiconshowdown.com, limited so the fallback keeps some of the time.
        IconSearchStatus primary = IconSearchStatus::NotFound;
        std::string primary_error;
        const auto primary_deadline = std::min(deadline, start + PrimarySourceDeadline);
        do {
            std::vector<std::string> queries{search_name};
            if (const auto colon = search_name.find(':');
                colon != std::string::npos && colon > 0) {
                queries.push_back(search_name.substr(0, colon));
            }
            std::vector<IconCandidate> candidates;
            for (const auto& query : queries) {
                const auto res =
                    HttpGet(std::string{IconSiteHost} + "/search.php?query=" + UrlEncode(query),
                            primary_deadline, cancel);
                if (res.status != HttpStatus::Ok) {
                    primary = to_status(res);
                    primary_error = res.detail;
                    break;
                }
                candidates = ParseSearchResults(res.body);
                if (!candidates.empty()) {
                    break;
                }
            }
            if (primary != IconSearchStatus::NotFound) {
                break;
            }

            // The site can list one game several times; keep the closest title first.
            std::vector<std::pair<int, IconCandidate>> scored;
            for (auto& cand : candidates) {
                const int score = ScoreCandidateTitle(cand.title, search_name);
                if (score >= MinCandidateScore) {
                    scored.emplace_back(score, std::move(cand));
                }
            }
            std::ranges::stable_sort(
                scored, [](const auto& a, const auto& b) { return a.first > b.first; });
            for (std::size_t i = 0; i < scored.size() && i < MaxIconDownloads; ++i) {
                const auto res = HttpGet(fmt::format("{}/images/100/{}.png", IconSiteHost,
                                                     scored[i].second.id),
                                         primary_deadline, cancel);
                if (res.status != HttpStatus::Ok) {
                    primary = to_status(res);
                    primary_error = res.detail;
                    break;
                }
                if (decode_icon(res.body)) {
                    out.status = IconSearchStatus::Found;
                    return out;
                }
            }
        } while (false);

        if (primary == IconSearchStatus::Cancelled) {
            out.status = primary;
            return out;
        }

        // Fallback: SteamGridDB, which needs the user's API key.
        if (api_key.empty()) {
            out.status = primary == IconSearchStatus::NotFound ? IconSearchStatus::NeedsApiKey
                                                               : primary;
            out.error = primary_error;
            return out;
        }
        const auto sgdb_get = [&](const std::string& path) {
            return HttpGet(std::string{SgdbApiBase} + path, deadline, cancel, api_key);
        };
        const auto search = sgdb_get("/search/autocomplete/" + UrlEncodePathSegment(search_name));
        if (search.status != HttpStatus::Ok) {
            out.status = to_status(search);
            out.error = search.detail;
            return out;
        }
        const auto parsed = nlohmann::json::parse(search.body, nullptr, false);
        int game_id = 0;
        if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("data") &&
            parsed["data"].is_array() && !parsed["data"].empty() &&
            parsed["data"][0].is_object() && parsed["data"][0].contains("id") &&
            parsed["data"][0]["id"].is_number_integer()) {
            game_id = parsed["data"][0]["id"].get<int>();
        }
        if (game_id <= 0) {
            out.status = IconSearchStatus::NotFound;
            return out;
        }
        const auto icons =
            sgdb_get(fmt::format("/icons/game/{}?dimensions=256,512&mimes=image/png", game_id));
        if (icons.status != HttpStatus::Ok) {
            out.status = to_status(icons);
            out.error = icons.detail;
            return out;
        }
        // Collect every icon that arrives in time; the user picks one.
        for (const auto& url : ParseSgdbIconUrls(icons.body)) {
            const auto img = HttpGet(url, deadline, cancel);
            if (img.status != HttpStatus::Ok) {
                if (img.status == HttpStatus::Cancelled || img.status == HttpStatus::Timeout) {
                    if (out.choices.empty()) {
                        out.status = to_status(img);
                        out.error = img.detail;
                        return out;
                    }
                    break;
                }
                continue;
            }
            if (decode_icon(img.body)) {
                out.choices.push_back(std::move(out.icon_bytes));
                out.icon_bytes.clear();
            }
        }
        if (!out.choices.empty()) {
            out.status = IconSearchStatus::Choices;
            return out;
        }
        out.status = IconSearchStatus::NotFound;
    } catch (const std::exception& e) {
        out.status = IconSearchStatus::InternalError;
        out.error = e.what();
    } catch (...) {
        out.status = IconSearchStatus::InternalError;
        out.error = "unknown error";
    }
    return out;
}

bool IsAllowedDetectHost(std::string_view host) {
    static constexpr std::string_view hosts[]{
        "raw.githubusercontent.com", "en.wikipedia.org", "upload.wikimedia.org",
        "www.bing.com",              "th.bing.com",      "tse1.mm.bing.com",
        "tse2.mm.bing.com",          "tse3.mm.bing.com", "tse4.mm.bing.com",
    };
    return std::ranges::find(hosts, host) != std::end(hosts);
}

ArtworkLookupResult LookUpSteamGridDbArtworkUntil(const std::string& title,
                                                  const std::string& api_key,
                                                  const std::atomic_bool& cancel,
                                                  bool prefer_newest,
                                                  std::chrono::steady_clock::time_point deadline) {
    ArtworkLookupResult out;
    // A trailing "(2026)" is not part of the name SteamGridDB knows; it says which release.
    std::string search_title = title;
    int wanted_year = 0;
    if (const auto open = search_title.rfind('(');
        open != std::string::npos && open > 0 && search_title.ends_with(')')) {
        const std::string inner = search_title.substr(open + 1, search_title.size() - open - 2);
        if (inner.size() == 4 &&
            std::ranges::all_of(inner, [](unsigned char c) { return std::isdigit(c) != 0; })) {
            wanted_year = std::stoi(inner);
            search_title.erase(open);
            while (!search_title.empty() &&
                   std::isspace(static_cast<unsigned char>(search_title.back()))) {
                search_title.pop_back();
            }
        }
    }
    if (title.empty() || search_title.empty() || api_key.empty()) {
        out.status = ArtworkLookupStatus::NotFound;
        return out;
    }
    const auto failed = [&out](const HttpResult& res) {
        out.status = res.status == HttpStatus::Unauthorized ? ArtworkLookupStatus::BadApiKey
                     : res.status == HttpStatus::Cancelled  ? ArtworkLookupStatus::Cancelled
                                                            : ArtworkLookupStatus::NetworkError;
        out.detail = res.detail;
        return out;
    };
    const auto sgdb_get = [&](const std::string& path) {
        return HttpGet(std::string{SgdbApiBase} + path, deadline, cancel, api_key);
    };
    try {
        const auto search = sgdb_get("/search/autocomplete/" + UrlEncodePathSegment(search_title));
        if (search.status != HttpStatus::Ok) {
            return failed(search);
        }
        // The first autocomplete entry is SteamGridDB's best match. Entries with the same name
        // are other releases of it (an original and its remake); the year picks between them.
        struct Entry {
            int id{};
            std::string name;
            int year{};
            s64 release{};
        };
        std::vector<Entry> entries;
        const auto parsed = nlohmann::json::parse(search.body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("data") &&
            parsed["data"].is_array()) {
            for (const auto& item : parsed["data"]) {
                if (!item.is_object() || !item.contains("id") || !item["id"].is_number_integer()) {
                    continue;
                }
                Entry entry{.id = item["id"].get<int>()};
                if (item.contains("name") && item["name"].is_string()) {
                    entry.name = item["name"].get<std::string>();
                }
                if (item.contains("release_date") && item["release_date"].is_number_integer()) {
                    entry.release = item["release_date"].get<s64>();
                    entry.year =
                        QDateTime::fromSecsSinceEpoch(entry.release).toUTC().date().year();
                }
                entries.push_back(std::move(entry));
            }
        }
        const auto simple_name = [](const std::string& name) {
            std::string out_name;
            for (const unsigned char c : name) {
                if (std::isalnum(c)) {
                    out_name += static_cast<char>(std::tolower(c));
                }
            }
            return out_name;
        };
        int game_id = 0;
        if (!entries.empty()) {
            const Entry* chosen = &entries.front();
            const std::string best_name = simple_name(chosen->name);
            const std::string searched_name = simple_name(search_title);
            // The year only picks between releases of the game: an entry of that year with
            // another name is a different game.
            const Entry* same_name_in_year = nullptr;
            const Entry* searched_name_in_year = nullptr;
            if (wanted_year != 0) {
                for (const auto& entry : entries) {
                    const std::string name = simple_name(entry.name);
                    // Names with no ASCII letters or digits (Japanese-only) all match each other.
                    if (entry.year != wanted_year || name.empty()) {
                        continue;
                    }
                    if (!same_name_in_year && name == best_name) {
                        same_name_in_year = &entry;
                    }
                    if (!searched_name_in_year && name == searched_name) {
                        searched_name_in_year = &entry;
                    }
                }
            }
            if (same_name_in_year || searched_name_in_year) {
                chosen = same_name_in_year ? same_name_in_year : searched_name_in_year;
            } else if (prefer_newest && !best_name.empty()) {
                for (const auto& entry : entries) {
                    if (simple_name(entry.name) == best_name && entry.release > chosen->release) {
                        chosen = &entry;
                    }
                }
            }
            game_id = chosen->id;
        }
        if (game_id <= 0) {
            out.status = ArtworkLookupStatus::NotFound;
            return out;
        }
        // Square grids first, then icons.
        for (const auto& list_path :
             {fmt::format("/grids/game/{}?dimensions=512x512,1024x1024", game_id),
              fmt::format("/icons/game/{}", game_id)}) {
            const auto list = sgdb_get(list_path);
            if (list.status != HttpStatus::Ok) {
                return failed(list);
            }
            for (const auto& url : ParseSgdbIconUrls(list.body)) {
                const auto image = HttpGet(url, deadline, cancel);
                if (image.status == HttpStatus::Cancelled || image.status == HttpStatus::Timeout) {
                    return failed(image);
                }
                if (image.status != HttpStatus::Ok) {
                    continue;
                }
                QImage decoded = DecodeBoundedImage(image.body);
                if (!decoded.isNull()) {
                    out.status = ArtworkLookupStatus::Found;
                    out.image = std::move(decoded);
                    return out;
                }
            }
        }
        out.status = ArtworkLookupStatus::NotFound;
    } catch (const std::exception& e) {
        out.status = ArtworkLookupStatus::NetworkError;
        out.detail = e.what();
    }
    return out;
}

} // namespace

ArtworkLookupResult LookUpSteamGridDbArtwork(const std::string& title, const std::string& api_key,
                                             const std::atomic_bool& cancel, bool prefer_newest) {
    return LookUpSteamGridDbArtworkUntil(title, api_key, cancel, prefer_newest,
                                         std::chrono::steady_clock::now() +
                                             std::chrono::seconds{30});
}

std::vector<u8> ReadSquareIconFile(const QString& path) {
    constexpr qint64 MaxBytes = 8 * 1024 * 1024;
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > MaxBytes) {
        return {};
    }
    QFile image_file(path);
    if (!image_file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = image_file.read(MaxBytes + 1);
    return EncodeSquareIcon(
        DecodeBoundedImage(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()))));
}

IconDetectResult DetectGameIcon(const std::string& name, u64 title_id,
                                const std::filesystem::path& game_path, bool allow_online,
                                const std::string& sgdb_api_key, const std::atomic_bool& cancel) {
    using namespace std::chrono;
    IconDetectResult out;
    const auto deadline = steady_clock::now() + seconds{20};
    int requests_left = 24;
    const auto stopped = [&] { return cancel.load() || steady_clock::now() >= deadline; };
    const auto stop_result = [&] {
        out.icon_bytes.clear();
        out.status = cancel.load() ? IconDetectStatus::Cancelled : IconDetectStatus::TimedOut;
        return out;
    };
    const auto found = [&] {
        out.status = IconDetectStatus::Found;
        return out;
    };
    try {
        const std::string search_name = ResolveGameNameAlias(name, title_id);
        if (search_name != name) {
            out.resolved_title = search_name;
        }
        out.search_name = search_name;
        const std::string slug = NormalizeGameSlug(search_name);

        // 1. Image files next to the game file.
        if (game_path.has_parent_path()) {
            const auto dir = game_path.parent_path();
            const std::string stem = Common::FS::PathToUTF8String(game_path.stem());
            for (const std::string& base : {slug, stem, std::string{"icon"}}) {
                if (base.empty()) {
                    continue;
                }
                for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
                    if (stopped()) {
                        return stop_result();
                    }
                    const auto candidate = dir / Common::FS::ToU8String(base + ext);
                    out.icon_bytes = ReadSquareIconFile(
                        QString::fromStdString(Common::FS::PathToUTF8String(candidate)));
                    if (!out.icon_bytes.empty()) {
                        return found();
                    }
                }
            }
        }
        // Nothing leaves the computer unless the user allowed it for this lookup.
        if (!allow_online) {
            out.status = IconDetectStatus::NoLocalMatch;
            return out;
        }
        if (search_name.empty()) {
            out.status = IconDetectStatus::NotFound;
            return out;
        }

        // 2. SteamGridDB, only with the user's own API key.
        if (!sgdb_api_key.empty()) {
            const auto art =
                LookUpSteamGridDbArtworkUntil(search_name, sgdb_api_key, cancel, false, deadline);
            if (art.status == ArtworkLookupStatus::Found) {
                out.icon_bytes = EncodeSquareIcon(art.image);
                if (!out.icon_bytes.empty()) {
                    return found();
                }
            }
            if (stopped()) {
                return stop_result();
            }
        }

        const HttpOptions image_options{IsAllowedDetectHost, 8 * 1024 * 1024, milliseconds{5000}};
        const HttpOptions page_options{IsAllowedDetectHost, 256 * 1024, milliseconds{5000}};
        const auto get = [&](const std::string& url,
                             const HttpOptions& options) -> std::optional<std::string> {
            if (stopped() || requests_left <= 0 || url.size() > 4096) {
                return std::nullopt;
            }
            --requests_left;
            auto res = HttpGet(url, deadline, cancel, {}, options);
            if (res.status != HttpStatus::Ok) {
                return std::nullopt;
            }
            return std::move(res.body);
        };
        const auto try_image = [&](const std::string& url) {
            const auto body = get(url, image_options);
            if (!body) {
                return false;
            }
            out.icon_bytes = EncodeSquareIcon(DecodeBoundedImage(*body));
            return !out.icon_bytes.empty();
        };

        // 3. The Eden boxart repository, by name slug.
        if (!slug.empty() &&
            try_image(fmt::format("https://raw.githubusercontent.com/eden-emulator/boxart/"
                                  "refs/heads/master/img/{}.png",
                                  slug))) {
            return found();
        }

        // 4. Wikipedia page images.
        for (const char* suffix : {" video game", " Nintendo", ""}) {
            if (stopped()) {
                break;
            }
            const std::string api_url = fmt::format(
                "https://en.wikipedia.org/w/api.php?action=query&format=json&generator=search"
                "&gsrsearch={}&gsrlimit=8&prop=pageimages&piprop=thumbnail|original"
                "&pithumbsize=512&pilicense=any",
                UrlEncode(search_name + suffix));
            const auto json_body = get(api_url, page_options);
            if (!json_body) {
                continue;
            }
            const auto parsed = nlohmann::json::parse(
                *json_body,
                [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                    return depth < 32;
                },
                false);
            if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("query") ||
                !parsed["query"].is_object() || !parsed["query"].contains("pages") ||
                !parsed["query"]["pages"].is_object()) {
                continue;
            }
            struct WikiCandidate {
                int score{};
                int index{};
                std::string img_url;
            };
            std::vector<WikiCandidate> candidates;
            int pages_left = 8;
            for (const auto& [_, page] : parsed["query"]["pages"].items()) {
                if (pages_left-- <= 0) {
                    break;
                }
                if (!page.is_object() || !page.contains("title") || !page["title"].is_string()) {
                    continue;
                }
                std::string img_url;
                for (const char* image : {"thumbnail", "original"}) {
                    if (page.contains(image) && page[image].is_object() &&
                        page[image].contains("source") && page[image]["source"].is_string()) {
                        img_url = page[image]["source"].get<std::string>();
                        break;
                    }
                }
                const auto page_title = page["title"].get<std::string>();
                if (img_url.empty() || img_url.size() > 4096 || page_title.size() > 1024) {
                    continue;
                }
                const int page_index = page.contains("index") && page["index"].is_number_integer()
                                           ? page["index"].get<int>()
                                           : 99;
                candidates.push_back(
                    {ScoreCandidateTitle(page_title, search_name), page_index, std::move(img_url)});
            }
            std::ranges::sort(candidates, [](const auto& a, const auto& b) {
                return a.score != b.score ? a.score > b.score : a.index < b.index;
            });
            int images_left = 3;
            for (const auto& cand : candidates) {
                if (cand.score < MinCandidateScore || images_left-- <= 0) {
                    break;
                }
                if (try_image(cand.img_url)) {
                    return found();
                }
            }
        }

        // 5. Bing square thumbnails.
        if (!stopped()) {
            const std::string bing_url = fmt::format(
                "https://www.bing.com/images/search?q={}&qft=+filterui:aspect-square",
                UrlEncode(search_name + " nintendo switch game icon cover"));
            if (const auto html = get(bing_url, page_options)) {
                static const QRegularExpression thumb_re(QStringLiteral(
                    R"(https://(?:tse[1-4]\.mm|th)\.bing\.com/th/id/OIP\.[A-Za-z0-9_\-]+)"));
                auto matches = thumb_re.globalMatch(QString::fromStdString(*html));
                int images_left = 4;
                while (!stopped() && images_left-- > 0 && matches.hasNext()) {
                    if (try_image(matches.next().captured().toStdString() + "?w=256&h=256&c=7")) {
                        return found();
                    }
                }
            }
        }
        if (stopped()) {
            return stop_result();
        }
        out.icon_bytes.clear();
        out.status = IconDetectStatus::NotFound;
    } catch (const std::exception&) {
        out.icon_bytes.clear();
        out.status = IconDetectStatus::Failed;
    }
    return out;
}

IconSearch::IconSearch(QWidget* parent) : QObject(parent), parent_widget{parent} {
    connect(&watcher, &QFutureWatcher<IconSearchResult>::finished, this, &IconSearch::OnFinished);
}

IconSearch::~IconSearch() {
    // Never block the UI thread: the detached worker stops on its own once it sees the flag.
    cancel_flag->store(true);
}

void IconSearch::Start(const QString& name, u64 title_id) {
    if (watcher.isRunning()) {
        return;
    }
    const std::string query = name.trimmed().toStdString();
    if (query.empty() && title_id == 0) {
        QMessageBox::information(parent_widget, tr("Find Icon"),
                                 tr("Please enter a game name first so the icon can be found."));
        return;
    }
    last_name = name;
    last_title_id = title_id;
    cancel_flag->store(false);
    emit RunningChanged(true);
    // The worker captures no pointer to the dialog, so it may safely outlive it.
    watcher.setFuture(QtConcurrent::run(
        [query, title_id, api_key = UISettings::values.steamgriddb_api_key.GetValue(),
         cancel = cancel_flag] { return RunIconSearch(query, title_id, api_key, *cancel); }));
}

bool IconSearch::IsRunning() const {
    return watcher.isRunning();
}

int IconSearch::ChooseIcon(const std::vector<std::vector<u8>>& choices) {
    if (choices.size() == 1) {
        return 0;
    }
    QDialog dialog(parent_widget);
    dialog.setWindowTitle(tr("Choose an Icon"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(
        new QLabel(tr("SteamGridDB has several icons for this game. Pick one:"), &dialog));
    auto* list = new QListWidget(&dialog);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(128, 128));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setSpacing(8);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    for (std::size_t i = 0; i < choices.size(); ++i) {
        QPixmap map;
        if (map.loadFromData(choices[i].data(), static_cast<uint>(choices[i].size()))) {
            auto* item = new QListWidgetItem(QIcon(map), QString::number(i + 1));
            item->setData(Qt::UserRole, static_cast<int>(i));
            list->addItem(item);
        }
    }
    if (list->count() == 0) {
        return -1;
    }
    list->setCurrentRow(0);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    layout->addWidget(list);
    layout->addWidget(buttons);
    dialog.resize(520, 420);
    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr) {
        return -1;
    }
    return list->currentItem()->data(Qt::UserRole).toInt();
}

bool IconSearch::AskForApiKey(bool missing) {
    QString text = missing ? tr("No matching icon was found on switchiconshowdown.com. A "
                                "SteamGridDB API key enables a fallback search.")
                           : tr("SteamGridDB rejected the saved API key.");
    if (UISettings::values.steamgriddb_api_key.GetValue().empty()) {
        text += QStringLiteral("<br><br>") +
                tr("Create a free key on the <a href=\"%1\">SteamGridDB API preferences</a> page.")
                    .arg(QStringLiteral("https://www.steamgriddb.com/profile/preferences/api"));
    }
    QDialog dialog(parent_widget);
    dialog.setWindowTitle(tr("SteamGridDB API Key"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(text, &dialog);
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    label->setWordWrap(true);
    auto* key_edit = new QLineEdit(
        QString::fromStdString(UISettings::values.steamgriddb_api_key.GetValue()), &dialog);
    key_edit->setEchoMode(QLineEdit::Password);
    key_edit->setPlaceholderText(tr("API key"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(label);
    layout->addWidget(key_edit);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(420);
    if (dialog.exec() != QDialog::Accepted || key_edit->text().trimmed().isEmpty()) {
        return false;
    }
    UISettings::values.steamgriddb_api_key.SetValue(key_edit->text().trimmed().toStdString());
    return true;
}

void IconSearch::OnFinished() {
    emit RunningChanged(false);

    IconSearchResult result;
    try {
        result = watcher.result();
    } catch (const std::exception& e) {
        result.status = IconSearchStatus::InternalError;
        result.error = e.what();
    } catch (...) {
        result.status = IconSearchStatus::InternalError;
    }

    const auto select = [&](std::vector<u8> png) {
        emit IconSelected(QByteArray(reinterpret_cast<const char*>(png.data()),
                                     static_cast<qsizetype>(png.size())),
                          QString::fromStdString(result.resolved_title));
    };

    QString message;
    switch (result.status) {
    case IconSearchStatus::Found:
        select(std::move(result.icon_bytes));
        return;
    case IconSearchStatus::Choices: {
        const int index = ChooseIcon(result.choices);
        if (index >= 0) {
            select(std::move(result.choices[static_cast<std::size_t>(index)]));
        }
        return;
    }
    case IconSearchStatus::NotFound:
        message = tr("No matching icon was found on switchiconshowdown.com or SteamGridDB. Try a "
                     "different game name or choose an icon file.");
        break;
    case IconSearchStatus::NeedsApiKey:
    case IconSearchStatus::BadApiKey:
        if (AskForApiKey(result.status == IconSearchStatus::NeedsApiKey)) {
            Start(last_name, last_title_id);
        }
        return;
    case IconSearchStatus::Timeout:
        message = tr("Searching for an icon took longer than %1 seconds and was stopped. Check "
                     "your connection and try again.")
                      .arg(SearchDeadline.count());
        break;
    case IconSearchStatus::NetworkError:
        message = tr("Could not reach the icon sources (%1). Check your connection and try again.")
                      .arg(QString::fromStdString(result.error));
        break;
    case IconSearchStatus::Cancelled:
        return;
    case IconSearchStatus::InternalError:
        message = tr("Searching for an icon failed unexpectedly: %1")
                      .arg(QString::fromStdString(result.error));
        break;
    }
    LOG_WARNING(Frontend, "Icon search failed: {}", message.toStdString());
    QMessageBox::warning(parent_widget, tr("Find Icon"), message);
}
