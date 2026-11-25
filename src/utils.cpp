#include "utils.h"
#include <shlobj.h>
#include <stdexcept>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>

namespace util {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// ---------------- Кодировки ----------------
bool IsValidUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { ++i; continue; }
        size_t len;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}


std::string DecodeConsole(const std::string& bytes) {
    if (bytes.empty()) return {};
    if (IsValidUtf8(bytes)) return bytes;
    UINT cp = GetOEMCP();
    int n = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (n <= 0) return bytes;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), w.data(), n);
    return ToUtf8(w);
}

// ---------------- Пути ----------------
fs::path ExePath() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    buf.resize(n);
    return fs::path(buf);
}

fs::path DocumentsDataDir() {
    for (const auto* id : {&FOLDERID_Documents, &FOLDERID_RoamingAppData}) {
        PWSTR raw = nullptr;
        const HRESULT result = SHGetKnownFolderPath(*id, 0, nullptr, &raw);
        fs::path path = SUCCEEDED(result) ? fs::path(raw) : fs::path();
        CoTaskMemFree(raw);
        if (!path.empty()) return path / L"ZapretLauncher";
    }
    throw std::runtime_error("Не удалось определить папку «Документы» или AppData.");
}

// ---------------- Запуск процессов ----------------
int RunHidden(const std::wstring& cmdLine, DWORD timeoutMs, const fs::path* workDir) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring buf = cmdLine;
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, workDir ? workDir->c_str() : nullptr, &si, &pi);
    if (!ok) return -1;
    int result;
    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        result = -2;
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result = (int)code;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

int RunCapture(const std::wstring& cmdLine, DWORD timeoutMs, std::string& outUtf8, const fs::path* workDir) {
    outUtf8.clear();
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : nullptr;
    si.hStdOutput = wr;
    si.hStdError = wr;

    PROCESS_INFORMATION pi{};
    std::wstring buf = cmdLine;
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, workDir ? workDir->c_str() : nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }
    CloseHandle(pi.hThread);

    std::string raw;
    char chunk[4096];
    ULONGLONG start = GetTickCount64();
    int result = 0;
    auto drain = [&]() {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
            DWORD n = 0;
            DWORD want = avail < (DWORD)sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
            if (!ReadFile(rd, chunk, want, &n, nullptr) || n == 0) break;
            raw.append(chunk, n);
        }
    };
    for (;;) {
        drain();
        DWORD w = WaitForSingleObject(pi.hProcess, 30);
        if (w == WAIT_OBJECT_0) {
            drain();
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            result = (int)code;
            break;
        }
        if (GetTickCount64() - start > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            result = -2;
            break;
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    outUtf8 = DecodeConsole(raw);
    return result;
}

// ---------------- Автозапуск ----------------
static const wchar_t* kTaskName = L"ZapretLauncher";

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': r += L"&amp;"; break;
            case L'<': r += L"&lt;"; break;
            case L'>': r += L"&gt;"; break;
            case L'"': r += L"&quot;"; break;
            default: r += c;
        }
    }
    return r;
}

static std::wstring EnvVar(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return L"";
    std::wstring v(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(got);
    return v;
}

bool IsAutostartEnabled() {
    return RunHidden(L"schtasks.exe /Query /TN \"ZapretLauncher\"", 15000) == 0;
}

bool SetAutostart(bool enable, std::string& error) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kTaskName);
        RegCloseKey(k);
    }

    if (!enable) {
        int rc = RunHidden(L"schtasks.exe /Delete /TN \"ZapretLauncher\" /F", 15000);
        if (rc != 0 && IsAutostartEnabled()) {
            error = "Не удалось удалить задачу автозапуска из Планировщика заданий.";
            return false;
        }
        return true;
    }

    std::wstring domain = EnvVar(L"USERDOMAIN");
    if (domain.empty()) domain = EnvVar(L"COMPUTERNAME");
    std::wstring user = domain + L"\\" + EnvVar(L"USERNAME");
    std::wstring exe = ExePath().wstring();
    std::wstring dir = ExePath().parent_path().wstring();

    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + XmlEscape(user) + L"</UserId></LogonTrigger></Triggers>\r\n"
        L"  <Principals><Principal id=\"Author\"><UserId>" + XmlEscape(user) + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit></Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exe) + L"</Command>"
        L"<Arguments>--tray</Arguments><WorkingDirectory>" + XmlEscape(dir) + L"</WorkingDirectory></Exec></Actions>\r\n"
        L"</Task>\r\n";

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    fs::path xmlPath = fs::path(tmp) / L"zapret_launcher_task.xml";
    {
        std::ofstream f(xmlPath, std::ios::binary | std::ios::trunc);
        if (!f) { error = "Не удалось создать временный XML-файл задачи."; return false; }
        const unsigned char bom[2] = {0xFF, 0xFE};
        f.write(reinterpret_cast<const char*>(bom), 2);
        f.write(reinterpret_cast<const char*>(xml.data()), (std::streamsize)(xml.size() * sizeof(wchar_t)));
    }
    int rc = RunHidden(L"schtasks.exe /Create /TN \"ZapretLauncher\" /XML \"" + xmlPath.wstring() + L"\" /F", 20000);
    std::error_code ec;
    fs::remove(xmlPath, ec);
    if (rc != 0) {
        error = "Не удалось создать задачу автозапуска (schtasks, код " + std::to_string(rc) + ").";
        return false;
    }
    return true;
}

std::string TrimCopy(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string NaturalKey(const std::string& s) {
    std::string r;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            size_t j = i;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
            size_t len = j - i;
            if (len < 8) r.append(8 - len, '0');
            r.append(s, i, len);
            i = j;
        } else {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            r.push_back(c);
            ++i;
        }
    }
    return r;
}

} // namespace util
