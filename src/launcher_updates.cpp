#include "launcher_updates.h"
#include "app_version.h"
#include "http_client.h"
#include "utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <regex>
#include <vector>

namespace launcher_updates {
namespace {
using nlohmann::json;
struct Version {
    std::array<std::string, 3> number;
    std::vector<std::string> pre;
};
bool Digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
std::optional<Version> Parse(const std::string& tag) {
    if (tag.size() > 128) return {};
    static const std::regex pattern(R"(^[vV]?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*))?(?:\+([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*))?$)");
    std::smatch match;
    if (!std::regex_match(tag, match, pattern)) return {};
    Version version{{match[1], match[2], match[3]}, {}};
    std::string pre = match[4];
    std::size_t start = 0;
    while (start < pre.size()) {
        const auto end = pre.find('.', start);
        auto id = pre.substr(start, end == std::string::npos ? end : end - start);
        if (Digits(id) && id.size() > 1 && id[0] == '0') return {};
        version.pre.push_back(std::move(id));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return version;
}
int Numeric(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a.compare(b);
}
int Compare(const Version& a, const Version& b) {
    for (std::size_t i = 0; i < a.number.size(); ++i)
        if (const int order = Numeric(a.number[i], b.number[i])) return order;
    if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;
    for (std::size_t i = 0; i < std::min(a.pre.size(), b.pre.size()); ++i) {
        const auto& x = a.pre[i];
        const auto& y = b.pre[i];
        const bool dx = Digits(x), dy = Digits(y);
        if (dx != dy) return dx ? -1 : 1;
        if (const int order = dx ? Numeric(x, y) : x.compare(y)) return order;
    }
    return static_cast<int>(a.pre.size()) - static_cast<int>(b.pre.size());
}
}

std::wstring Repository() { return std::wstring(L"https://github.com/") + kRepositoryPath; }

int CompareVersions(const std::string& lhs, const std::string& rhs) {
    const auto a = Parse(lhs), b = Parse(rhs);
    if (!a || !b) throw std::invalid_argument("Invalid release version");
    return Compare(*a, *b);
}

Result FromReleases(const std::string& body, bool includeBeta) {
    Result result;
    const auto releases = json::parse(body, nullptr, false);
    if (!releases.is_array()) {
        result.error = "GitHub вернул некорректный список релизов.";
        return result;
    }
    std::optional<Version> best;
    bool malformed = false;
    for (const auto& release : releases) {
        try {
            if (!release.is_object()) { malformed = true; continue; }
            if (!release.contains("draft") || !release["draft"].is_boolean()) { malformed = true; continue; }
            if (release["draft"].get<bool>() || !release.contains("published_at") || !release["published_at"].is_string())
                continue;
            const auto tag = release.at("tag_name").get<std::string>();
            auto version = Parse(tag);
            if (!version) { malformed = true; continue; }
            const bool preview = release.at("prerelease").get<bool>() || !version->pre.empty();
            if (!includeBeta && preview) continue;
            if (preview && version->pre.empty()) version->pre.push_back("beta");
            if (best && Compare(*version, *best) <= 0) continue;

            Result candidate;
            candidate.tag = tag;
            const auto notes = release.find("body");
            if (notes != release.end() && notes->is_string()) {
                candidate.notes = notes->get<std::string>();
                if (candidate.notes.size() > 12000) {
                    std::size_t n = 12000;
                    while ((static_cast<unsigned char>(candidate.notes[n]) & 0xC0) == 0x80) --n;
                    candidate.notes.resize(n);
                    candidate.notes += "\n…";
                }
            }
            const auto prefix = Repository() + L"/releases/";
            candidate.page = prefix + L"tag/" + util::FromUtf8(tag);
            if (const auto assets = release.find("assets"); assets != release.end() && assets->is_array()) {
                static const std::regex namePattern(R"(^ZapretLauncher(?:[-_][A-Za-z0-9.+_-]+)?\.exe$)", std::regex::icase);
                for (const auto& asset : *assets) {
                    if (!asset.is_object()) continue;
                    const auto name = asset.find("name"), url = asset.find("browser_download_url"), state = asset.find("state");
                    if (name == asset.end() || !name->is_string() || url == asset.end() || !url->is_string() ||
                        state == asset.end() || *state != "uploaded") continue;
                    const auto filename = name->get<std::string>();
                    if (filename.size() > 180 || !std::regex_match(filename, namePattern)) continue;
                    const auto expected = prefix + L"download/" + util::FromUtf8(tag) + L"/" + util::FromUtf8(filename);
                    const auto actual = util::FromUtf8(url->get<std::string>());
                    if (_wcsicmp(expected.c_str(), actual.c_str()) == 0) { candidate.asset = actual; break; }
                }
            }
            candidate.newer = Compare(*version, *Parse(LAUNCHER_VERSION "-beta")) > 0;
            result = std::move(candidate);
            best = std::move(version);
        } catch (const json::exception&) {
            malformed = true;
        }
    }
    if (!best && malformed) result.error = "Не удалось распознать опубликованные версии GitHub.";
    return result;
}

Result Check(bool includeBeta) {
    std::string body, error;
    const auto url = std::wstring(L"https://api.github.com/repos/") + kRepositoryPath + L"/releases?per_page=100";
    if (!http::GetString(url, true, body, error)) {
        Result result;
        result.error = "Проверка релизов GitHub не удалась. " + error;
        return result;
    }
    return FromReleases(body, includeBeta);
}
}
