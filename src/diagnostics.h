#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace diagnostics {
enum class Level { Good, Warning, Error };
struct Service {
    const char* name;
    const wchar_t* url;
};
inline constexpr Service kServices[] = {
    {"Discord", L"https://discord.com/"},
    {"YouTube", L"https://www.youtube.com/"},
    {"Cloudflare", L"https://www.cloudflare.com/cdn-cgi/trace"},
    {"GitHub", L"https://github.com/"},
};
struct Result {
    std::string name;
    Level level;
    std::string detail;
    std::optional<long long> latencyMs{};
    std::chrono::system_clock::time_point measuredAt{};
    std::chrono::steady_clock::time_point measuredTick{};
};
struct Report {
    std::vector<Result> network;
    std::vector<Result> system;
};
Report Run(bool includeSystem);
}
