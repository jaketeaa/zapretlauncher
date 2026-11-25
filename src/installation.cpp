#include "installation.h"
#include "service_control.h"
#include "utils.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <vector>

namespace installation {
namespace fs = std::filesystem;
namespace {
constexpr wchar_t kMarker[] = L".jaketea-managed";

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() { if (*this) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
};

bool SamePart(const Path& a, const Path& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool Contains(const Path& parent, const Path& path) {
    auto part = path.begin();
    for (const auto& expected : parent) {
        if (part == path.end() || !SamePart(*part++, expected)) return false;
    }
    return true;
}

std::wstring Extended(const Path& path) { return L"\\\\?\\" + path.native(); }

Path Normal(Path path) {
    path = path.lexically_normal();
    if (path != path.root_path() && path.filename().empty()) path = path.parent_path();
    return path;
}

bool Protected(const Path& candidate, const Path& configPath) {
    auto canonical = [](const Path& path) {
        std::error_code ec;
        auto result = fs::weakly_canonical(path, ec);
        return Normal(ec ? path : result);
    };
    if (Contains(candidate, canonical(util::ExePath())) || Contains(candidate, canonical(configPath))) return true;
    const KNOWNFOLDERID* folders[] = {&FOLDERID_Documents, &FOLDERID_Desktop, &FOLDERID_Downloads,
        &FOLDERID_Profile, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData,
        &FOLDERID_Windows, &FOLDERID_System, &FOLDERID_SystemX86};
    for (auto* id : folders) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &raw))) {
            const auto known = canonical(Path(raw));
            CoTaskMemFree(raw);
            if (Contains(candidate, known) ||
                ((*id == FOLDERID_Windows || *id == FOLDERID_System || *id == FOLDERID_SystemX86) &&
                 Contains(known, candidate))) return true;
        }
    }
    return false;
}

bool SafePath(const Path& path, const Path& configPath, std::string& error, bool requireLeaf = true) {
    const auto root = path.root_name().native();
    if (!path.is_absolute() || root.size() != 2 || root[1] != L':' || path == path.root_path() ||
        (requireLeaf && !SamePart(path.filename(), L"zapret")) ||
        GetDriveTypeW(path.root_path().c_str()) != DRIVE_FIXED || Protected(path, configPath)) {
        error = "Выберите отдельную папку zapret на локальном диске. Системные папки и файлы лаунчера защищены.";
        return false;
    }
    // Extended paths do not trim Win32 alias forms such as names ending in dots/spaces.
    for (const auto& component : path.relative_path()) {
        const auto& name = component.native();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ' ||
            name.find(L':') != std::wstring::npos) {
            error = "Недопустимое имя в пути рабочей папки.";
            return false;
        }
    }
    return true;
}

Handle Open(const Path& path, DWORD access, DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE) {
    return Handle(CreateFileW(Extended(path).c_str(), access, share, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}

bool EntryInfo(const Handle& file, BY_HANDLE_FILE_INFORMATION& info) {
    return file && GetFileInformationByHandle(file.value, &info) &&
           !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool SameFinalPath(const Handle& root, const Path& path) {
    if (!root) return false;
    std::wstring finalPath(32768, L'\0');
    const DWORD size = GetFinalPathNameByHandleW(root.value, finalPath.data(), static_cast<DWORD>(finalPath.size()),
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size || size >= finalPath.size()) return false;
    finalPath.resize(size);
    return CompareStringOrdinal(finalPath.c_str(), -1, Extended(path).c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool LockParents(const Path& path, std::vector<Handle>& locks, std::string& error) {
    Path current = path.root_path();
    auto relative = path.parent_path().relative_path();
    for (const auto& component : relative) {
        current /= component;
        auto handle = Open(current, FILE_READ_ATTRIBUTES);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!EntryInfo(handle, info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = "Не удалось проверить родительский каталог. Символические ссылки и junction запрещены: " +
                    util::ToUtf8(current.wstring());
            return false;
        }
        locks.push_back(std::move(handle));
    }
    return true;
}

std::string Token(const BY_HANDLE_FILE_INFORMATION& info) {
    return "JAKETEA-ZAPRET-1 " + std::to_string(info.dwVolumeSerialNumber) + " " +
           std::to_string(info.nFileIndexHigh) + " " + std::to_string(info.nFileIndexLow) + " " +
           std::to_string(info.ftCreationTime.dwHighDateTime) + " " + std::to_string(info.ftCreationTime.dwLowDateTime);
}

bool CheckMarker(const Path& directory, const BY_HANDLE_FILE_INFORMATION& root) {
    auto marker = Open(directory / kMarker, GENERIC_READ, FILE_SHARE_READ);
    BY_HANDLE_FILE_INFORMATION info{};
    char bytes[192]{};
    DWORD read = 0;
    return EntryInfo(marker, info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           info.nNumberOfLinks == 1 && !info.nFileSizeHigh && info.nFileSizeLow < sizeof(bytes) &&
           ReadFile(marker.value, bytes, sizeof(bytes), &read, nullptr) && std::string(bytes, read) == Token(root);
}

std::string At(const Path& path, const char* message) {
    return std::string(message) + " " + util::ToUtf8(path.wstring());
}

bool MarkDeleted(HANDLE file) {
    FILE_DISPOSITION_INFO info{TRUE};
    return SetFileInformationByHandle(file, FileDispositionInfo, &info, sizeof(info)) != FALSE;
}

// Each directory is opened without FILE_SHARE_DELETE before descending. Every
// leaf is checked with OPEN_REPARSE_POINT and is deleted through that same handle.
bool Visit(const Path& path, bool erase, bool isRoot, std::string& error, unsigned depth = 0) {
    if (depth > 128) {
        error = At(path, "Достигнут предел вложенности каталогов.");
        return false;
    }
    std::vector<Path> children;
    {
        WIN32_FIND_DATAW found{};
        HANDLE scan = FindFirstFileW(Extended(path / L"*").c_str(), &found);
        if (scan == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
            error = At(path, "Не удалось прочитать содержимое:");
            return false;
        }
        struct FindGuard { HANDLE handle; ~FindGuard() { FindClose(handle); } } guard{scan};
        do {
            Path name(found.cFileName);
            if (name != L"." && name != L".." && !(isRoot && SamePart(name, kMarker)))
                children.push_back(std::move(name));
        } while (FindNextFileW(scan, &found));
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            error = At(path, "Не удалось дочитать содержимое:");
            return false;
        }
    }
    for (const auto& name : children) {
        const Path file = path / name;
        auto entry = Open(file, FILE_READ_ATTRIBUTES | (erase ? DELETE : 0));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!EntryInfo(entry, info)) {
            error = At(file, "Файл занят, недоступен или является точкой повторной обработки:");
            return false;
        }
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !Visit(file, erase, false, error, depth + 1))
            return false;
        if (erase && !MarkDeleted(entry.value)) {
            const auto code = GetLastError();
            error = At(file, "Не удалось удалить:") + " (код Windows " + std::to_string(code) + ").";
            return false;
        }
    }
    return true;
}
}

Path ResolveSelection(const Path& selected) {
    auto result = Normal(selected);
    if (!result.empty() && !SamePart(result.filename(), L"zapret")) result /= L"zapret";
    return result;
}

bool Prepare(const Path& directory, const Path& configPath, std::string& error) {
    const auto path = Normal(directory);
    // Existing configurations may predate the naming rule. They remain usable,
    // but only a newly managed folder named zapret can be removed from the UI.
    if (!SafePath(path, configPath, error, false)) return false;
    std::vector<Handle> parents;
    if (!LockParents(path, parents, error)) return false;
    std::error_code ec;
    fs::create_directories(path, ec);
    auto root = Open(path, FILE_READ_ATTRIBUTES | FILE_ADD_FILE);
    BY_HANDLE_FILE_INFORMATION info{};
    if (ec || !SameFinalPath(root, path) || !EntryInfo(root, info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = At(path, "Не удалось создать рабочую папку или получить доступ на запись:");
        return false;
    }
    if (CheckMarker(path, info)) return true;

    const bool empty = fs::is_empty(path, ec);
    if (ec) {
        error = At(path, "Не удалось проверить рабочую папку:");
        return false;
    }
    const bool managed = empty && SamePart(path.filename(), L"zapret");
    const Path file = path / (managed ? kMarker :
        (L".jaketea-write-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    Handle probe(CreateFileW(Extended(file).c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                             (managed ? 0 : FILE_FLAG_DELETE_ON_CLOSE), nullptr));
    if (!probe) {
        error = At(path, "Не удалось записать в рабочую папку:");
        return false;
    }
    if (!managed) return true;
    const auto token = Token(info);
    DWORD written = 0;
    if (WriteFile(probe.value, token.data(), static_cast<DWORD>(token.size()), &written, nullptr) &&
        written == token.size() && FlushFileBuffers(probe.value)) return true;
    MarkDeleted(probe.value);
    error = At(path, "Не удалось записать маркер установки:");
    return false;
}

bool IsManaged(const Path& directory, const Path& configPath) {
    const auto path = Normal(directory);
    std::string error;
    std::vector<Handle> parents;
    if (!SafePath(path, configPath, error) || !LockParents(path, parents, error)) return false;
    auto root = Open(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    BY_HANDLE_FILE_INFORMATION info{};
    return SameFinalPath(root, path) && EntryInfo(root, info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && CheckMarker(path, info);
}

std::string Remove(const Path& directory, const Path& configPath, const std::function<void()>& closeTerminal) {
    const auto path = Normal(directory);
    std::string error;
    std::vector<Handle> parents;
    if (!SafePath(path, configPath, error) || !LockParents(path, parents, error)) return error;
    std::string identity;
    {
        auto root = Open(path, FILE_READ_ATTRIBUTES);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!SameFinalPath(root, path) || !EntryInfo(root, info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !CheckMarker(path, info))
            return "Принадлежность папки лаунчеру не подтверждена. Удаление отменено.";
        identity = Token(info);
        if (!Visit(path, false, true, error)) return error;
        closeTerminal();
        if (!svc::RemoveInstallation(path, error)) return "Удаление не завершено. " + error;

    }
    {
        auto root = Open(path, FILE_READ_ATTRIBUTES | DELETE);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!SameFinalPath(root, path) || !EntryInfo(root, info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Token(info) != identity || !CheckMarker(path, info))
            return "Службы проверены, но папку не удалось открыть для удаления.";
        if (!Visit(path, true, true, error)) return "Удаление не завершено. " + error;
        {
            auto marker = Open(path / kMarker, DELETE | FILE_READ_ATTRIBUTES);
            BY_HANDLE_FILE_INFORMATION markerInfo{};
            if (!EntryInfo(marker, markerInfo) || !MarkDeleted(marker.value))
                return "Файлы удалены, но не удалось удалить маркер установки.";
        }
        if (!MarkDeleted(root.value)) {
            // Restore the marker if another program left new files inside the directory.
            Handle marker(CreateFileW(Extended(path / kMarker).c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (marker) {
                const auto token = Token(info);
                DWORD written = 0;
                WriteFile(marker.value, token.data(), static_cast<DWORD>(token.size()), &written, nullptr);
            }
            return "Часть файлов удалена, но рабочая папка осталась. Закройте программы, использующие её, и повторите.";
        }
    }
    if (GetFileAttributesW(Extended(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return {};
    }
    return "Удаление папки не подтверждено. Проверьте, не удерживает ли её другая программа.";
}
}
