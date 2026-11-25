#pragma once
#include <string>

namespace launcher_updates {
inline constexpr wchar_t kRepositoryPath[] = L"jaketeaa/zapretlauncher";
std::wstring Repository();

struct Result {
    std::string tag, notes, error;
    std::wstring page, asset;
    bool newer = false;
};

int CompareVersions(const std::string& lhs, const std::string& rhs);
Result Check(bool includeBeta);
Result FromReleases(const std::string& body, bool includeBeta);
}
