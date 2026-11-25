#pragma once
#include "config_manager.h"
#include <windows.h>
#include <optional>

namespace settings_backup {
struct ListFile { std::string name, text; };
struct Backup {
    Settings settings;
    std::vector<ListFile> lists;
    std::string createdAt, version;
};
struct Result {
    std::string error;
    std::optional<Backup> backup;
};
std::filesystem::path PickPath(HWND owner, bool save, std::string& error);
Result Read(const std::filesystem::path& path);
Result Export(const ConfigManager& config, const std::filesystem::path& output);
Result Restore(ConfigManager config, const Backup& backup);
}
