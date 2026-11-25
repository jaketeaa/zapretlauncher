#include "config_manager.h"
#include "utils.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <system_error>
#include <cwchar>

namespace fs = std::filesystem;
using nlohmann::json;

ConfigManager::ConfigManager(fs::path baseDir) : base_(std::move(baseDir)) {}

fs::path ConfigManager::ZapretDir() const {
    if (!settings.zapretPath.empty()) {
        fs::path path(util::FromUtf8(settings.zapretPath));
        if (path.is_absolute()) return path;
    }
    return base_ / L"zapret";
}

bool ConfigManager::Load(std::string& error) {
    std::error_code ec;
    if (!fs::exists(ConfigPath(), ec)) {
        const auto stable = base_ / L"config.json";
        std::string text;
        if (fs::exists(stable, ec)) {
            if (!ReadTextFile(stable, text, error)) return false;
            try {
                settings = json::parse(text).at("settings").get<Settings>();
            } catch (const std::exception&) {
                settings = Settings{};
                error = "Не удалось прочитать стабильные настройки. Бета использует значения по умолчанию.";
                return false;
            }
        } else if (ec) {
            error = "Не удалось проверить наличие стабильного конфига.";
            return false;
        }
        return Save(error);
    }
    std::string text;
    if (!ReadTextFile(ConfigPath(), text, error)) return false;
    try {
        json j = json::parse(text);
        if (j.contains("settings")) settings = j["settings"].get<Settings>();
        return true;
    } catch (const std::exception& e) {
        fs::path bak = ConfigPath();
        bak += L".broken";
        fs::copy_file(ConfigPath(), bak, fs::copy_options::overwrite_existing, ec);
        settings = Settings{};
        error = std::string("config-beta.json повреждён (") + e.what() +
                "). Загружены настройки по умолчанию, копия сохранена в config-beta.json.broken";
        return false;
    }
}

bool ConfigManager::Save(std::string& error) {
    try {
        json j;
        j["settings"] = settings;
        std::string text = j.dump(2, ' ', false, json::error_handler_t::replace);
        auto temporary = ConfigPath();
        temporary += L".tmp";
        if (!WriteTextFile(temporary, text, error)) return false;
        if (MoveFileExW(temporary.c_str(), ConfigPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        error = "Не удалось заменить config-beta.json (код " + std::to_string(GetLastError()) + ").";
        return false;
    } catch (const std::exception& e) {
        error = std::string("Ошибка сохранения config-beta.json: ") + e.what();
        return false;
    }
}

std::vector<std::string> ConfigManager::ListFiles(const fs::path& dir, const wchar_t* ext) const {
    std::vector<std::string> r;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return r;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        std::wstring e = it->path().extension().wstring();
        if (_wcsicmp(e.c_str(), ext) == 0)
            r.push_back(util::ToUtf8(it->path().filename().wstring()));
    }
    std::sort(r.begin(), r.end());
    return r;
}

std::vector<std::string> ConfigManager::ListStrategies() const {
    std::vector<std::string> r;
    for (const auto& name : ListFiles(ZapretDir(), L".bat")) {
        if (util::ToLowerAscii(name).rfind("general", 0) != 0) continue;
        r.push_back(name);
    }
    std::sort(r.begin(), r.end(), [](const std::string& a, const std::string& b) {
        return util::NaturalKey(a) < util::NaturalKey(b);
    });
    return r;
}

std::vector<std::string> ConfigManager::ListLists()    const { return ListFiles(ListsDir(), L".txt"); }

bool ConfigManager::ReadTextFile(const fs::path& p, std::string& out, std::string& error) const {
    std::ifstream f(p, std::ios::binary);
    if (!f) { error = "Не удалось открыть файл: " + util::ToUtf8(p.wstring()); return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    return true;
}

bool ConfigManager::WriteTextFile(const fs::path& p, const std::string& text, std::string& error) const {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { error = "Не удалось записать файл: " + util::ToUtf8(p.wstring()); return false; }
    f.write(text.data(), (std::streamsize)text.size());
    if (!f) { error = "Ошибка записи файла: " + util::ToUtf8(p.wstring()); return false; }
    return true;
}
