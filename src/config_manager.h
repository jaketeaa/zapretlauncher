#pragma once
#include <filesystem>
#include <utility>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct Settings {
    std::string selectedBat;
    bool terminalKillOnLeave = true;
    std::string zapretPath;
    std::vector<std::string> favoriteStrategies, recentStrategies;
    bool monitorOn = false;
    int monitorSeconds = 10;
    std::string updateChannel;
    bool checkLauncherOnStart = false;
    bool closeToTray = true;
    bool launchHidden = true;
    bool backupAutostart = false;
    bool filtersPending = false;
    std::string gameFilter = "disabled", ipsetFilter = "none";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, selectedBat, terminalKillOnLeave, zapretPath, favoriteStrategies, recentStrategies, monitorOn, monitorSeconds, updateChannel, checkLauncherOnStart, closeToTray, launchHidden, backupAutostart, filtersPending, gameFilter, ipsetFilter)

class ConfigManager {
public:
    explicit ConfigManager(std::filesystem::path baseDir);
    Settings settings;

    std::filesystem::path ZapretDir() const;
    std::filesystem::path ListsDir() const { return ZapretDir() / L"lists"; }
    std::filesystem::path ConfigPath() const { return base_ / L"config-beta.json"; }

    bool Load(std::string& error);
    bool Save(std::string& error);

    std::vector<std::string> ListLists() const;
    std::vector<std::string> ListStrategies() const;
    bool ReadTextFile(const std::filesystem::path& p, std::string& out, std::string& error) const;
    bool WriteTextFile(const std::filesystem::path& p, const std::string& text, std::string& error) const;

private:
    std::vector<std::string> ListFiles(const std::filesystem::path& dir, const wchar_t* ext) const;
    std::filesystem::path base_;
};
