#include "console_session.h"
#include "utils.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kDisplayMax = 262144;
constexpr size_t kDisplayKeep = 196608;
constexpr size_t kFeedMax = 1048576;
constexpr size_t kFeedKeep = 524288;

bool ValidUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t len;
        if (c < 0x80) { ++i; continue; }
        else if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

// Сколько последних байт образуют начало (неполную) UTF-8 последовательность
size_t IncompleteUtf8Tail(const std::string& s) {
    size_t n = s.size();
    for (size_t k = 1; k <= 3 && k <= n; ++k) {
        unsigned char c = (unsigned char)s[n - k];
        if ((c & 0xC0) == 0x80) continue;           // продолжение - смотрим дальше назад
        size_t need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        return need > k ? k : 0;
    }
    return 0;
}

std::string OemToUtf8(const std::string& s) {
    if (s.empty()) return {};
    UINT cp = 866;
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return s;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), w.data(), n);
    return util::ToUtf8(w);
}

std::string Utf8ToOem(const std::string& s) {
    bool ascii = true;
    for (unsigned char c : s) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return s;
    std::wstring w = util::FromUtf8(s);
    UINT cp = 866;
    int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, "?", nullptr);
    if (n <= 0) return s;
    std::string r((size_t)n, '\0');
    WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), r.data(), n, "?", nullptr);
    return r;
}

// Декодирует накопленные байты. В pending остаётся только неполный хвост UTF-8.
std::string DecodeBytes(std::string& pending, bool flush) {
    size_t keep = flush ? 0 : IncompleteUtf8Tail(pending);
    std::string chunk = pending.substr(0, pending.size() - keep);
    pending.erase(0, pending.size() - keep);
    if (chunk.empty()) return {};
    if (ValidUtf8(chunk)) return chunk;

    // Разные команды service.bat могут менять кодовую страницу между строками.
    std::string decoded;
    size_t start = 0;
    while (start < chunk.size()) {
        const auto end = chunk.find('\n', start);
        const auto line = chunk.substr(start, end == std::string::npos ? end : end + 1 - start);
        decoded += ValidUtf8(line) ? line : OemToUtf8(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return decoded;
}

void PopUtf8Char(std::string& s) {
    if (s.empty()) return;
    size_t start = s.size() - 1;
    while (start > 0 && ((unsigned char)s[start] & 0xC0) == 0x80) --start;
    s.erase(start);
}

} // namespace

ConsoleSession::ConsoleSession() {}

ConsoleSession::~ConsoleSession() { Stop(); }

std::wstring ConsoleSession::MakeServiceBatCommand() {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const auto cmd = (fs::path(system) / L"cmd.exe").wstring();
    // Аргумент "admin" обязателен: без него service.bat пытается перезапустить себя через
    // PowerShell с запросом UAC и открывает отдельное окно, а лаунчер уже работает от администратора.
    return L"\"" + cmd + L"\" /d /v:off /c .\\service.bat admin";
}

bool ConsoleSession::Start(const std::wstring& commandLine, const fs::path& workDir, std::string& error) {
    Stop();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        display_.clear();
        feed_.clear();
        feedBase_ = 0;
        escState_ = 0;
        pendingCR_ = false;
    }
    ++counter_;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE outRd = nullptr, outWr = nullptr, inRd = nullptr, inWr = nullptr;
    if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
        error = "Не удалось создать канал вывода (ошибка " + std::to_string(GetLastError()) + ").";
        return false;
    }
    if (!CreatePipe(&inRd, &inWr, &sa, 0)) {
        CloseHandle(outRd); CloseHandle(outWr);
        error = "Не удалось создать канал ввода (ошибка " + std::to_string(GetLastError()) + ").";
        return false;
    }
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOEXW si{};
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = inRd;
    si.StartupInfo.hStdOutput = outWr;
    si.StartupInfo.hStdError = outWr;

    // Наследуем только нужные хэндлы, чтобы чужие процессы не удерживали наш пайп
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize ? attrSize : 1);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool attrOk = attrSize && InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
    HANDLE inheritList[2] = {inRd, outWr};
    if (attrOk) {
        if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inheritList, sizeof(inheritList), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            attrOk = false;
        }
    }
    si.StartupInfo.cb = attrOk ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);

    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (attrOk ? EXTENDED_STARTUPINFO_PRESENT : 0);
    std::wstring cmd = commandLine;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(),
                             reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
    DWORD lastErr = GetLastError();
    if (attrOk) DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(outWr);
    CloseHandle(inRd);
    if (!ok) {
        CloseHandle(outRd);
        CloseHandle(inWr);
        error = "Не удалось запустить cmd.exe (ошибка Windows " + std::to_string(lastErr) + ").";
        return false;
    }

    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
        if (!AssignProcessToJobObject(job_, pi.hProcess)) {
            CloseHandle(job_);
            job_ = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    process_ = pi.hProcess;
    readPipe_ = outRd;
    writePipe_ = inWr;
    lastActivity_ = GetTickCount64();
    reader_ = std::thread(&ConsoleSession::ReaderLoop, this);
    return true;
}

void ConsoleSession::CloseHandles() {
    if (readPipe_)  { CloseHandle(readPipe_);  readPipe_ = nullptr; }
    {
        std::lock_guard<std::mutex> lk(writeMutex_);
        if (writePipe_) { CloseHandle(writePipe_); writePipe_ = nullptr; }
    }
    if (process_)   { CloseHandle(process_);   process_ = nullptr; }
    if (job_)       { CloseHandle(job_);       job_ = nullptr; }
}

void ConsoleSession::Stop() {
    if (!process_ && !reader_.joinable() && !job_) return;
    if (job_) TerminateJobObject(job_, 1);
    else if (process_) TerminateProcess(process_, 1);
    if (process_) WaitForSingleObject(process_, 3000);
    if (readPipe_) CancelIoEx(readPipe_, nullptr);
    if (reader_.joinable()) reader_.join();
    CloseHandles();
}

bool ConsoleSession::IsRunning() const {
    if (!process_) return false;
    return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool ConsoleSession::WaitExit(DWORD timeoutMs) {
    if (!process_) return true;
    return WaitForSingleObject(process_, timeoutMs) == WAIT_OBJECT_0;
}

DWORD ConsoleSession::ExitCode() const {
    DWORD code = 0;
    if (process_) GetExitCodeProcess(process_, &code);
    return code;
}

void ConsoleSession::Send(const std::string& utf8) {
    std::lock_guard<std::mutex> lk(writeMutex_);
    if (!writePipe_) return;
    std::string data = Utf8ToOem(utf8);
    size_t off = 0;
    while (off < data.size()) {
        DWORD w = 0;
        if (!WriteFile(writePipe_, data.data() + off, (DWORD)(data.size() - off), &w, nullptr) || w == 0) break;
        off += w;
    }
}

void ConsoleSession::SendLine(const std::string& utf8) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        display_ += utf8;
        display_ += '\n';
    }
    ++counter_;
    Send(utf8 + "\r\n");
}

std::string ConsoleSession::DisplayText() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return display_;
}

void ConsoleSession::ClearDisplay() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        display_.clear();
    }
    ++counter_;
}

void ConsoleSession::ReaderLoop() {
    char buf[4096];
    std::string pending;
    DWORD n = 0;
    while (ReadFile(readPipe_, buf, sizeof(buf), &n, nullptr) && n > 0) {
        lastActivity_ = GetTickCount64();
        pending.append(buf, n);
        std::string text = DecodeBytes(pending, false);
        if (!text.empty()) Ingest(text);
    }
    if (!pending.empty()) {
        std::string text = DecodeBytes(pending, true);
        if (!text.empty()) Ingest(text);
    }
}

void ConsoleSession::Ingest(const std::string& u) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto both = [&](char c) { display_.push_back(c); feed_.push_back(c); };

    for (unsigned char c : u) {
        // Управляющие последовательности ESC [ ... - вырезаем
        if (escState_ == 1) { escState_ = (c == '[') ? 2 : 0; continue; }
        if (escState_ == 2) { if (c >= 0x40 && c <= 0x7E) escState_ = 0; continue; }
        if (c == 0x1B) { escState_ = 1; continue; }

        if (pendingCR_) {
            pendingCR_ = false;
            if (c == '\n') { both('\n'); continue; }
            // одиночный CR: возврат каретки - стираем текущую строку
            size_t nl = display_.find_last_of('\n');
            display_.erase(nl == std::string::npos ? 0 : nl + 1);
            feed_.push_back('\n');
        }

        switch (c) {
        case '\r': pendingCR_ = true; break;
        case '\f': display_.clear(); feed_.push_back('\n'); break;   // cls
        case '\b': if (!display_.empty() && display_.back() != '\n') PopUtf8Char(display_); break;
        case '\n': both('\n'); break;
        case '\t': both('\t'); break;
        default:
            if (c < 0x20) break;
            both((char)c);
        }
    }

    if (display_.size() > kDisplayMax) {
        size_t cut = display_.size() - kDisplayKeep;
        size_t nl = display_.find('\n', cut);
        display_.erase(0, nl == std::string::npos ? cut : nl + 1);
    }
    if (feed_.size() > kFeedMax) {
        size_t drop = feed_.size() - kFeedKeep;
        feed_.erase(0, drop);
        feedBase_ += drop;
    }
    ++counter_;
}

bool ConsoleSession::Find(const std::string& needle, size_t from, size_t& at) const {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t start = from > feedBase_ ? from - feedBase_ : 0;
    if (start > feed_.size()) return false;
    auto it = std::search(feed_.begin() + (std::ptrdiff_t)start, feed_.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    if (it == feed_.end()) return false;
    at = feedBase_ + (size_t)(it - feed_.begin());
    return true;
}

bool ConsoleSession::WaitFor(const std::string& needle, size_t& pos, DWORD timeoutMs, const std::atomic<bool>* cancel) {
    ULONGLONG end = GetTickCount64() + timeoutMs;
    size_t at = 0;
    for (;;) {
        if (Find(needle, pos, at)) { pos = at + needle.size(); return true; }
        if (cancel && cancel->load()) return false;
        if (!IsRunning()) {
            Sleep(150);   // дочитываем остаток вывода
            if (Find(needle, pos, at)) { pos = at + needle.size(); return true; }
            return false;
        }
        if (GetTickCount64() >= end) return false;
        Sleep(30);
    }
}

std::string ConsoleSession::FeedRange(size_t from, size_t to) const {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t end = feedBase_ + feed_.size();
    if (to > end) to = end;
    if (from < feedBase_) from = feedBase_;
    if (from >= to) return {};
    return feed_.substr(from - feedBase_, to - from);
}

size_t ConsoleSession::FeedEnd() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return feedBase_ + feed_.size();
}
