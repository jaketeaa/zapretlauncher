#include "service_control.h"
#include "utils.h"
#include <tlhelp32.h>
#include <cwchar>
#include <vector>
#include <cwctype>
#include <algorithm>

namespace svc {

namespace {
struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE x = nullptr) : h(x) {}
    ~ScHandle() { if (h) CloseServiceHandle(h); }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
};

bool QueryState(SC_HANDLE service, DWORD& state, DWORD* pid = nullptr) {
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed))
        return false;
    state = ssp.dwCurrentState;
    if (pid) *pid = ssp.dwProcessId;
    return true;
}
}

Info Query(const wchar_t* name) {
    Info info;
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) { info.error = GetLastError(); return info; }
    ScHandle s(OpenServiceW(scm.h, name, SERVICE_QUERY_STATUS));
    if (!s.h) { info.error = GetLastError(); return info; }
    info.installed = true;
    if (!QueryState(s.h, info.state, &info.pid)) info.error = GetLastError();
    return info;
}

std::string StateText(const Info& i) {
    if (i.error == ERROR_SERVICE_MARKED_FOR_DELETE) return "Ожидает удаления";
    if (i.error && i.error != ERROR_SERVICE_DOES_NOT_EXIST)
        return "Ошибка чтения SCM (" + std::to_string(i.error) + ")";
    if (!i.installed) return "Не установлена";
    switch (i.state) {
        case SERVICE_RUNNING:       return "Работает";
        case SERVICE_STOPPED:       return "Остановлена";
        case SERVICE_START_PENDING: return "Запускается";
        case SERVICE_STOP_PENDING:  return "Останавливается";
        case SERVICE_PAUSED:        return "Приостановлена";
        default:                    return "Неизвестно";
    }
}

bool Stop(const wchar_t* name, DWORD timeoutMs) {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return false;
    ScHandle s(OpenServiceW(scm.h, name, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!s.h) return false;
    DWORD st = 0;
    if (QueryState(s.h, st) && st == SERVICE_STOPPED) return true;
    SERVICE_STATUS ss{};
    ControlService(s.h, SERVICE_CONTROL_STOP, &ss);
    ULONGLONG end = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < end) {
        if (QueryState(s.h, st) && st == SERVICE_STOPPED) return true;
        Sleep(200);
    }
    return false;
}

bool Start(const wchar_t* name, DWORD timeoutMs) {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return false;
    ScHandle s(OpenServiceW(scm.h, name, SERVICE_START | SERVICE_QUERY_STATUS));
    if (!s.h) return false;
    DWORD st = 0;
    if (QueryState(s.h, st) && st == SERVICE_RUNNING) return true;
    StartServiceW(s.h, 0, nullptr);
    ULONGLONG end = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < end) {
        if (QueryState(s.h, st) && st == SERVICE_RUNNING) return true;
        Sleep(200);
    }
    return false;
}

bool ProcessRunning(const wchar_t* exeName, DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if ((!pid || pe.th32ProcessID == pid) && _wcsicmp(pe.szExeFile, exeName) == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

namespace {
namespace fs = std::filesystem;

std::wstring ZapretImagePath() {
    constexpr wchar_t key[] = L"SYSTEM\\CurrentControlSet\\Services\\zapret";
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ImagePath", flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        !bytes || bytes > 65536) return {};
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ImagePath", flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        return {};
    return value.data();
}

fs::path ServiceExecutable(SC_HANDLE service, bool zapret = false) {
    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    std::wstring command;
    if (needed && needed <= 8192) {
        std::vector<BYTE> buffer(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service, config, needed, &needed) && config->lpBinaryPathName)
            command = config->lpBinaryPathName;
    }
    // SCM cannot return the complete record for some long winws command lines.
    if (command.empty() && zapret) command = ZapretImagePath();
    const auto first = command.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    command.erase(0, first);
    std::wstring executable;
    if (command.front() == L'"') {
        const auto end = command.find(L'"', 1);
        if (end == std::wstring::npos) return {};
        executable = command.substr(1, end - 1);
    } else {
        // Driver paths may be supplied unquoted and may contain spaces.
        auto lower = command;
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
        for (const auto* suffix : {L".exe", L".sys"}) {
            const auto end = lower.find(suffix);
            if (end != std::wstring::npos && (end + 4 == lower.size() || iswspace(lower[end + 4]))) {
                executable = command.substr(0, end + 4);
                break;
            }
        }
    }
    if (executable.rfind(L"\\??\\", 0) == 0 || executable.rfind(L"\\\\?\\", 0) == 0) executable.erase(0, 4);
    if (_wcsnicmp(executable.c_str(), L"\\SystemRoot\\", 12) == 0) executable.replace(0, 12, L"%SystemRoot%\\");
    else if (_wcsnicmp(executable.c_str(), L"System32\\", 9) == 0) executable.insert(0, L"%SystemRoot%\\");
    if (executable.find(L'%') != std::wstring::npos) {
        const DWORD size = ExpandEnvironmentStringsW(executable.c_str(), nullptr, 0);
        std::wstring expanded(size, L'\0');
        if (!size || ExpandEnvironmentStringsW(executable.c_str(), expanded.data(), size) != size) return {};
        expanded.resize(size - 1);
        executable = std::move(expanded);
    }
    return fs::path(executable);
}

bool SameExecutable(const fs::path& candidate, const fs::path& expected) {
    if (!candidate.is_absolute()) return false;
    std::error_code leftError, rightError;
    const auto left = fs::weakly_canonical(candidate, leftError);
    const auto right = fs::weakly_canonical(expected, rightError);
    return !leftError && !rightError &&
           CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool UnderDirectory(const fs::path& candidate, const fs::path& directory) {
    if (!candidate.is_absolute()) return false;
    std::error_code leftError, rightError;
    const auto left = fs::weakly_canonical(candidate, leftError);
    const auto right = fs::weakly_canonical(directory, rightError);
    if (leftError || rightError) return false;
    auto part = left.begin();
    for (const auto& parent : right) {
        if (part == left.end() || _wcsicmp((part++)->c_str(), parent.c_str()) != 0) return false;
    }
    return part != left.end();
}

bool ComponentExecutable(const fs::path& executable, bool driver) {
    const auto file = executable.filename().wstring();
    if (driver) return _wcsicmp(file.c_str(), L"WinDivert64.sys") == 0 || _wcsicmp(file.c_str(), L"WinDivert32.sys") == 0;
    return _wcsicmp(file.c_str(), L"winws.exe") == 0;
}

bool StopVerified(SC_HANDLE service) {
    DWORD state = 0;
    if (!QueryState(service, state)) return false;
    if (state == SERVICE_STOPPED) return true;
    if (state != SERVICE_STOP_PENDING) {
        SERVICE_STATUS status{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &status) &&
            GetLastError() != ERROR_SERVICE_NOT_ACTIVE) return false;
    }
    const auto until = GetTickCount64() + 20000;
    do {
        if (!QueryState(service, state)) return false;
        if (state == SERVICE_STOPPED) return true;
        Sleep(150);
    } while (GetTickCount64() < until);
    SetLastError(ERROR_TIMEOUT);
    return false;
}

bool StopRemaining(const fs::path& directory, std::string& error) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { error = "Не удалось получить список процессов Windows."; return false; }
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } snapshotGuard{snapshot};
    PROCESSENTRY32W entry{sizeof(PROCESSENTRY32W)};
    if (!Process32FirstW(snapshot, &entry)) { error = "Не удалось прочитать список процессов Windows."; return false; }
    do {
        if (_wcsicmp(entry.szExeFile, L"winws.exe") != 0) continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!process) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue;
            error = "Не удалось проверить путь процесса winws.exe (PID " + std::to_string(entry.th32ProcessID) + ").";
            return false;
        }
        Close processGuard{process};
        std::wstring name(32768, L'\0');
        DWORD length = static_cast<DWORD>(name.size());
        if (!QueryFullProcessImageNameW(process, 0, name.data(), &length)) {
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) continue;
            error = "Не удалось определить путь процесса winws.exe.";
            return false;
        }
        name.resize(length);
        if (!UnderDirectory(name, directory)) continue;
        // The original live handle pins the process identity while upgrading access.
        const HANDLE terminate = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!terminate) {
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) continue;
            error = "Нет доступа для остановки winws.exe этой установки.";
            return false;
        }
        Close terminateGuard{terminate};
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) continue;
        if ((!TerminateProcess(terminate, 0) && WaitForSingleObject(process, 0) != WAIT_OBJECT_0) ||
            WaitForSingleObject(process, 10000) != WAIT_OBJECT_0) {
            error = "Не удалось остановить winws.exe этой установки.";
            return false;
        }
    } while (Process32NextW(snapshot, &entry));
    if (GetLastError() == ERROR_NO_MORE_FILES) return true;
    error = "Не удалось завершить проверку процессов Windows.";
    return false;
}
}

ZapretInfo QueryZapret(const fs::path& directory) {
    ZapretInfo result;
    result.service = Query(L"zapret");
    if (!result.service.installed) return result;
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return result;
    ScHandle service(OpenServiceW(scm.h, L"zapret", SERVICE_QUERY_CONFIG));
    if (!service.h) return result;
    result.executable = ServiceExecutable(service.h, true);
    result.sameDirectory = SameExecutable(result.executable, directory / L"bin" / L"winws.exe");
    result.supported = result.executable.is_absolute() && ComponentExecutable(result.executable, false) &&
                       _wcsicmp(result.executable.parent_path().filename().c_str(), L"bin") == 0 &&
                       !ReadStrategyName().empty();
    if (result.service.pid && result.executable.is_absolute()) {
        bool pathReadable = false;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, result.service.pid);
        if (process) {
            std::wstring path(32768, L'\0');
            DWORD length = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
                pathReadable = true;
                path.resize(length);
                result.processRunning = SameExecutable(path, result.executable);
            }
            CloseHandle(process);
        }
        // A restricted process may hide its image path. The SCM PID still
        // distinguishes it from other winws.exe processes in the snapshot.
        if (!pathReadable && result.supported)
            result.processRunning = ProcessRunning(L"winws.exe", result.service.pid);
    }
    return result;
}

bool IsInDirectory(const fs::path& directory) {
    return QueryZapret(directory).sameDirectory;
}

bool RemoveZapretService(const fs::path& expected, std::string& error) {
    auto fail = [&](const char* operation, DWORD code) {
        error = std::string(operation) + " (код Windows " + std::to_string(code) + ").";
        return false;
    };
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return fail("Не удалось открыть SCM", GetLastError());
    {
        ScHandle service(OpenServiceW(scm.h, L"zapret", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE));
        if (!service.h) {
            const auto code = GetLastError();
            if (code == ERROR_SERVICE_DOES_NOT_EXIST) return true;
            if (code != ERROR_SERVICE_MARKED_FOR_DELETE) return fail("Не удалось открыть службу", code);
        } else {
            auto matches = [&] {
                const auto actual = ServiceExecutable(service.h, true);
                return ComponentExecutable(actual, false) &&
                       _wcsicmp(actual.parent_path().filename().c_str(), L"bin") == 0 &&
                       SameExecutable(actual, expected) && !ReadStrategyName().empty();
            };
            if (!matches()) { error = "Путь или принадлежность службы изменились. Удаление отменено."; return false; }
            if (!StopVerified(service.h)) return fail("Не удалось остановить службу", GetLastError());
            if (!matches()) { error = "Конфигурация службы изменилась во время остановки."; return false; }
            if (!DeleteService(service.h) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
                return fail("Не удалось удалить службу", GetLastError());
        }
    }

    const auto deadline = GetTickCount64() + 5000;
    do {
        ScHandle service(OpenServiceW(scm.h, L"zapret", SERVICE_QUERY_STATUS));
        if (!service.h) {
            const auto code = GetLastError();
            if (code == ERROR_SERVICE_DOES_NOT_EXIST) return true;
            if (code != ERROR_SERVICE_MARKED_FOR_DELETE) return fail("Не удалось проверить удаление службы", code);
        }
        Sleep(100);
    } while (GetTickCount64() < deadline);
    error = "Служба ожидает удаления. Windows завершит его после закрытия открытых дескрипторов.";
    return false;
}

bool RemoveInstallation(const fs::path& directory, std::string& error) {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
    if (!scm.h) { error = "Не удалось открыть диспетчер служб Windows."; return false; }

    struct Owned { std::wstring name; bool driver; };
    std::vector<Owned> owned;
    std::vector<BYTE> page(64 * 1024);
    DWORD resume = 0;
    for (;;) {
        DWORD needed = 0, count = 0;
        const BOOL complete = EnumServicesStatusExW(scm.h, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER,
            SERVICE_STATE_ALL, page.data(), static_cast<DWORD>(page.size()), &needed, &count, &resume, nullptr);
        if (!complete && GetLastError() != ERROR_MORE_DATA) {
            error = "Не удалось перечислить службы Windows.";
            return false;
        }
        auto* entries = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(page.data());
        for (DWORD i = 0; i < count; ++i) {
            const auto* name = entries[i].lpServiceName;
            const bool known = _wcsicmp(name, L"zapret") == 0 || _wcsnicmp(name, L"WinDivert", 9) == 0;
            ScHandle service(OpenServiceW(scm.h, name, SERVICE_QUERY_CONFIG));
            if (!service.h && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) continue;
            const auto executable = service.h ? ServiceExecutable(service.h, _wcsicmp(name, L"zapret") == 0) : fs::path{};
            if (known && !executable.is_absolute()) {
                error = "Не удалось подтвердить принадлежность службы " + util::ToUtf8(name) + ".";
                return false;
            }
            if (!UnderDirectory(executable, directory)) continue;
            const bool driver = (entries[i].ServiceStatusProcess.dwServiceType & SERVICE_DRIVER) != 0;
            if (ComponentExecutable(executable, driver)) owned.push_back({name, driver});
            else {
                error = "В рабочей папке обнаружена служба другого компонента: " + util::ToUtf8(name) + ".";
                return false;
            }
        }
        if (complete) break;
    }
    std::stable_sort(owned.begin(), owned.end(), [](const auto& a, const auto& b) { return a.driver < b.driver; });
    for (const auto& item : owned) {
        if (item.driver && !StopRemaining(directory, error)) return false;
        ScHandle service(OpenServiceW(scm.h, item.name.c_str(), SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE));
        if (!service.h && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) continue;
        auto stillOwned = [&] {
            const auto executable = ServiceExecutable(service.h, _wcsicmp(item.name.c_str(), L"zapret") == 0);
            return ComponentExecutable(executable, item.driver) && UnderDirectory(executable, directory);
        };
        if (!service.h || !stillOwned()) {
            error = "Не удалось подтвердить путь службы " + util::ToUtf8(item.name) + ".";
            return false;
        }
        if (!StopVerified(service.h) || !stillOwned() ||
            (!DeleteService(service.h) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)) {
            error = "Не удалось остановить и удалить службу " + util::ToUtf8(item.name) + ".";
            return false;
        }
    }
    if (!StopRemaining(directory, error)) return false;
    for (const auto& item : owned) {
        const auto deadline = GetTickCount64() + 3500;
        for (;;) {
            ScHandle service(OpenServiceW(scm.h, item.name.c_str(), SERVICE_QUERY_STATUS));
            if (!service.h && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) break;
            if (GetTickCount64() >= deadline) {
                error = "Удаление регистрации службы " + util::ToUtf8(item.name) + " пока не подтверждено.";
                return false;
            }
            Sleep(100);
        }
    }
    return true;
}

std::string ReadStrategyName() {
    wchar_t buf[512]{};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\zapret",
                              L"zapret-discord-youtube", RRF_RT_REG_SZ, nullptr, buf, &size);
    if (rc != ERROR_SUCCESS) return {};
    return util::ToUtf8(std::wstring(buf));
}

}
