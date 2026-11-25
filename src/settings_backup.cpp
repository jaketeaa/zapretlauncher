#include "settings_backup.h"
#include "app_version.h"
#include "beta_state.h"
#include "installation.h"
#include "utils.h"

#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <system_error>

namespace settings_backup {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;
using Microsoft::WRL::ComPtr;
constexpr std::size_t kMaxFile = 512 * 1024;
constexpr std::size_t kMaxBackup = 8 * 1024 * 1024;
constexpr std::size_t kMaxLists = 128;

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle(const Handle&) = delete;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};

bool ValidLeaf(const std::string& name) {
    if (name.empty() || name.size() > 180 || name.find_first_of("<>:\"/\\|?*") != std::string::npos ||
        name.front() == '.' || name.back() == ' ' || name.back() == '.' ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
    const auto wide = util::FromUtf8(name);
    if (wide.empty() || util::ToUtf8(wide) != name) return false;
    auto stem = wide.substr(0, wide.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    if (_wcsicmp(stem.c_str(), L"CON") == 0 || _wcsicmp(stem.c_str(), L"PRN") == 0 ||
        _wcsicmp(stem.c_str(), L"AUX") == 0 || _wcsicmp(stem.c_str(), L"NUL") == 0) return false;
    return !(stem.size() == 4 && (_wcsnicmp(stem.c_str(), L"COM", 3) == 0 || _wcsnicmp(stem.c_str(), L"LPT", 3) == 0));
}

bool Strategy(const std::string& value) {
    if (!ValidLeaf(value)) return false;
    const auto lower = util::ToLowerAscii(value);
    return lower.rfind("general", 0) == 0 && fs::path(util::FromUtf8(lower)).extension() == L".bat";
}

bool UserList(const std::string& value) {
    if (!ValidLeaf(value)) return false;
    const auto lower = util::ToLowerAscii(value);
    return fs::path(util::FromUtf8(lower)).extension() == L".txt" &&
           lower != "ipset-all.txt" && lower != "ipset-exclude.txt" &&
           lower != "list-exclude.txt" && lower != "list-general.txt" && lower != "list-google.txt";
}

void ValidateSettings(const Settings& s) {
    const auto validStrategies = [](const auto& items, std::size_t limit) {
        if (items.size() > limit) return false;
        std::set<std::string> names;
        for (const auto& name : items)
            if (!Strategy(name) || !names.insert(util::ToLowerAscii(name)).second) return false;
        return true;
    };
    if (!s.zapretPath.empty() || (!s.selectedBat.empty() && !Strategy(s.selectedBat)) ||
        !validStrategies(s.favoriteStrategies, 512) || !validStrategies(s.recentStrategies, 5) ||
        (s.monitorSeconds != 5 && s.monitorSeconds != 10 && s.monitorSeconds != 30 && s.monitorSeconds != 60) ||
        (!s.updateChannel.empty() && s.updateChannel != "stable" && s.updateChannel != "beta") ||
        (s.gameFilter != "disabled" && s.gameFilter != "all" && s.gameFilter != "tcp" && s.gameFilter != "udp") ||
        (s.ipsetFilter != "none" && s.ipsetFilter != "loaded" && s.ipsetFilter != "any"))
        throw Failure("Недопустимое значение в настройках резервной копии.");
}

void ValidateList(const ListFile& list) {
    if (!UserList(list.name) || list.text.size() > kMaxFile ||
        std::any_of(list.text.begin(), list.text.end(), [](unsigned char c) { return c < 32 && c != '\n' && c != '\r' && c != '\t'; }))
        throw Failure("Некорректное имя или содержимое пользовательского списка.");
}

struct CompareNames {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};

// Hold parent directories open without delete sharing while files are inspected
// or replaced. A directory junction or mount point is never followed.
std::vector<Handle> LockParents(const fs::path& path, bool create = false) {
    const auto root = path.root_path();
    if (!path.is_absolute() || GetDriveTypeW(root.c_str()) != DRIVE_FIXED || path != path.lexically_normal())
        throw Failure("Для записи требуется локальная папка с обычным путём.");
    std::vector<Handle> locks;
    auto dir = root;
    for (const auto& part : path.relative_path()) {
        dir /= part;
        if (create && !CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            throw Failure("Не удалось подготовить папку резервной копии или списков.");
        Handle h(CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!h || !GetFileInformationByHandle(h.value, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            throw Failure("Не удалось открыть обычную локальную папку. Ссылки и точки подключения не поддерживаются.");
        locks.push_back(std::move(h));
    }
    return locks;
}

std::optional<std::string> ReadBytes(const fs::path& path, std::size_t limit) {
    Handle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        throw Failure("Не удалось открыть файл резервной копии или списка.");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(h.value, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !GetFileSizeEx(h.value, &size) || size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > limit)
        throw Failure("Файл слишком большой или является ссылкой.");
    std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD bytes = 0;
    if (!ReadFile(h.value, data.data(), static_cast<DWORD>(data.size()), &bytes, nullptr) || bytes != data.size())
        throw Failure("Не удалось полностью прочитать файл.");
    return data;
}

std::wstring Unique() {
    GUID id{};
    wchar_t value[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, value, static_cast<int>(std::size(value))))
        throw Failure("Не удалось подготовить уникальный путь.");
    return value;
}

struct Temporary {
    fs::path path;
    explicit Temporary(fs::path p) : path(std::move(p)) {}
    Temporary(Temporary&& other) noexcept : path(std::exchange(other.path, {})) {}
    Temporary(const Temporary&) = delete;
    ~Temporary() { if (!path.empty()) DeleteFileW(path.c_str()); }
};

Temporary Stage(const fs::path& target, const std::string& bytes) {
    const auto name = target.parent_path() / (L".zapret-" + Unique() + L".tmp");
    Handle h(CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h) throw Failure("Не удалось создать временный файл.");
    Temporary file(name);
    DWORD written = 0;
    const bool ok = WriteFile(h.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size() && FlushFileBuffers(h.value);
    CloseHandle(std::exchange(h.value, INVALID_HANDLE_VALUE));
    if (!ok) throw Failure("Не удалось записать временный файл.");
    return file;
}

bool Replace(Temporary& stage, const fs::path& target, bool overwrite) {
    if (!MoveFileExW(stage.path.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
        return false;
    stage.path.clear();
    return true;
}

std::string Encode(const Backup& backup) {
    json lists = json::array();
    for (const auto& list : backup.lists) lists.push_back({{"name", list.name}, {"text", list.text}});
    auto data = json{{"format", "ZapretLauncherBackup"}, {"schema", 1}, {"launcher", backup.version},
        {"createdAt", backup.createdAt}, {"settings", backup.settings}, {"lists", std::move(lists)}}.dump(2);
    if (data.size() > kMaxBackup) throw Failure("Резервная копия превышает 8 МБ.");
    return data;
}

Backup Capture(const ConfigManager& config) {
    Backup result{config.settings, {}, beta::LocalTime(beta::WallClock::now(), "%Y-%m-%d %H:%M:%S"), LAUNCHER_DISPLAY_VERSION};
    result.settings.zapretPath.clear();
    ValidateSettings(result.settings);
    const auto dir = config.ListsDir();
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        if (ec) throw Failure("Не удалось проверить папку списков.");
        return result;
    }
    const auto locks = LockParents(dir);
    std::size_t total = 0;
    for (const auto& name : config.ListLists()) {
        if (!UserList(name)) continue;
        const auto content = ReadBytes(dir / util::FromUtf8(name), kMaxFile);
        if (!content) throw Failure("Список изменился во время резервного копирования.");
        ListFile list{name, *content};
        ValidateList(list);
        total += list.text.size();
        if (result.lists.size() == kMaxLists || total > kMaxBackup) throw Failure("Слишком много данных в списках.");
        result.lists.push_back(std::move(list));
    }
    return result;
}
}

std::filesystem::path PickPath(HWND owner, bool save, std::string& error) {
    const auto init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init)) { error = "Не удалось открыть системный диалог файлов."; return {}; }
    struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
    ComPtr<IFileDialog> dialog;
    auto hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                               CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr)) {
        const COMDLG_FILTERSPEC filter{L"Резервная копия ZAPRET LAUNCHER", L"*.zapretbackup"};
        FILEOPENDIALOGOPTIONS flags{};
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                           (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
        dialog->SetFileTypes(1, &filter);
        dialog->SetDefaultExtension(L"zapretbackup");
        if (save) {
            const auto name = L"ZapretLauncher-backup-" +
                util::FromUtf8(beta::LocalTime(beta::WallClock::now(), "%Y-%m-%d")) + L".zapretbackup";
            dialog->SetFileName(name.c_str());
        }
        hr = dialog->Show(owner);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
        ComPtr<IShellItem> item;
        if (SUCCEEDED(hr)) hr = dialog->GetResult(&item);
        PWSTR value = nullptr;
        if (SUCCEEDED(hr)) hr = item->GetDisplayName(SIGDN_FILESYSPATH, &value);
        if (SUCCEEDED(hr) && value) {
            const std::filesystem::path path(value);
            CoTaskMemFree(value);
            if (_wcsicmp(path.extension().c_str(), L".zapretbackup") == 0) return path;
            error = "Выберите файл с расширением .zapretbackup.";
            return {};
        }
    }
    error = "Не удалось выбрать файл резервной копии.";
    return {};
}

Result Read(const std::filesystem::path& path) {
    try {
        const auto text = ReadBytes(path, kMaxBackup);
        if (!text) throw Failure("Файл резервной копии отсутствует.");
        const auto doc = json::parse(*text, [](int depth, json::parse_event_t, json&) {
            if (depth > 32) throw Failure("В резервной копии слишком много уровней вложенности.");
            return true;
        });
        if (!doc.is_object() || doc.at("format") != "ZapretLauncherBackup" ||
            !doc.at("schema").is_number_integer() || doc.at("schema") != 1)
            throw Failure("Неизвестный формат или версия схемы резервной копии.");
        Backup result;
        const auto& settings = doc.at("settings");
        if (!settings.is_object() ||
            (settings.contains("monitorSeconds") && !settings["monitorSeconds"].is_number_integer()) ||
            (settings.contains("monitorSeconds") && settings["monitorSeconds"] != 5 &&
             settings["monitorSeconds"] != 10 && settings["monitorSeconds"] != 30 && settings["monitorSeconds"] != 60))
            throw Failure("Неверные типы или значения настроек.");
        result.settings = settings.get<Settings>();
        ValidateSettings(result.settings);
        result.createdAt = doc.at("createdAt").get<std::string>();
        result.version = doc.at("launcher").get<std::string>();
        if (result.createdAt.empty() || result.createdAt.size() > 64 || result.version.empty() || result.version.size() > 64 ||
            result.createdAt.find_first_of("\r\n\0", 0, 3) != std::string::npos ||
            result.version.find_first_of("\r\n\0", 0, 3) != std::string::npos)
            throw Failure("Некорректные метаданные копии.");
        const auto& lists = doc.at("lists");
        if (!lists.is_array() || lists.size() > kMaxLists) throw Failure("Недопустимое количество списков.");
        std::set<std::wstring, CompareNames> unique;
        for (const auto& value : lists) {
            ListFile list{value.at("name").get<std::string>(), value.at("text").get<std::string>()};
            ValidateList(list);
            if (!unique.insert(util::FromUtf8(list.name)).second) throw Failure("В копии повторяются имена списков.");
            result.lists.push_back(std::move(list));
        }
        return {{}, std::move(result)};
    } catch (const Failure& e) {
        return {e.what(), {}};
    } catch (const std::exception&) {
        return {"Повреждённая резервная копия или неверные типы данных.", {}};
    }
}

Result Export(const ConfigManager& config, const std::filesystem::path& output) {
    try {
        const auto locks = LockParents(output.parent_path());
        auto stage = Stage(output, Encode(Capture(config)));
        if (!Replace(stage, output, true)) throw Failure("Не удалось сохранить резервную копию.");
        return {};
    } catch (const Failure& e) {
        return {e.what(), {}};
    } catch (const std::exception&) {
        return {"Не удалось записать резервную копию.", {}};
    }
}

Result Restore(ConfigManager config, const Backup& backup) {
    try {
        const auto dir = config.ListsDir();
        const auto configFile = config.ConfigPath();
        const auto configLocks = LockParents(configFile.parent_path());
        if (!backup.lists.empty() && !installation::IsManaged(config.ZapretDir(), configFile))
            throw Failure("Для восстановления списков сначала подготовьте рабочую папку в настройках.");
        ValidateSettings(backup.settings);
        std::set<std::wstring, CompareNames> names;
        if (backup.lists.size() > kMaxLists) throw Failure("Недопустимое количество списков.");
        for (const auto& list : backup.lists) {
            ValidateList(list);
            if (!names.insert(util::FromUtf8(list.name)).second) throw Failure("В копии повторяются имена списков.");
        }
        const auto recovery = configFile.parent_path() / L"beta-backups";
        const auto recoveryLocks = LockParents(recovery, true);
        const auto before = Capture(config);
        const auto copy = recovery / (L"before-import-" + Unique() + L".zapretbackup");
        auto recoveryStage = Stage(copy, Encode(before));
        if (!Replace(recoveryStage, copy, false)) throw Failure("Не удалось сохранить копию перед импортом.");

        const auto listLocks = backup.lists.empty() ? std::vector<Handle>{} : LockParents(dir, true);
        struct Change {
            fs::path target;
            Temporary staged;
            std::optional<Temporary> original;
        };
        std::vector<Change> changes;
        changes.reserve(backup.lists.size() + 1);
        auto add = [&](const fs::path& target, const std::string& text, std::size_t limit) {
            std::optional<Temporary> rollback;
            if (const auto original = ReadBytes(target, limit)) rollback.emplace(Stage(target, *original));
            changes.push_back({target, Stage(target, text), std::move(rollback)});
        };
        for (const auto& list : backup.lists)
            add(dir / util::FromUtf8(list.name), list.text, kMaxFile);
        auto restored = backup;
        restored.settings.zapretPath = config.settings.zapretPath;
        restored.settings.filtersPending = true;
        add(configFile, json{{"settings", restored.settings}}.dump(2), kMaxBackup);

        std::size_t applied = 0;
        for (; applied < changes.size(); ++applied) {
            auto& change = changes[applied];
            if (!Replace(change.staged, change.target, change.original.has_value())) break;
        }
        if (applied != changes.size()) {
            bool recovered = true;
            while (applied > 0) {
                auto& change = changes[--applied];
                const bool ok = change.original ? Replace(*change.original, change.target, true)
                                                : DeleteFileW(change.target.c_str()) != FALSE;
                recovered = recovered && ok;
            }
            if (!recovered) return {"Импорт прерван, часть файлов заблокирована. Копия сохранена: " +
                                      util::ToUtf8(copy.wstring()), {}};
            return {"Импорт отменён. Исходные файлы восстановлены.", {}};
        }
        return {{}, std::move(restored)};
    } catch (const Failure& e) {
        return {e.what(), {}};
    } catch (const std::exception&) {
        return {"Импорт не выполнен. Проверьте доступ к папке настроек и списков.", {}};
    }
}
}
