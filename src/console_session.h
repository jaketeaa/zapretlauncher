#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

// Скрытый консольный процесс с перенаправлением stdin/stdout через анонимные пайпы.
// Вывод декодируется (UTF-8 или OEM-кодовая страница, например CP866) в UTF-8.
// Все дочерние процессы помещены в Job Object: при Stop() / закрытии программы они гарантированно завершаются.
class ConsoleSession {
public:
    ConsoleSession();
    ~ConsoleSession();
    ConsoleSession(const ConsoleSession&) = delete;
    ConsoleSession& operator=(const ConsoleSession&) = delete;

    // Командная строка для запуска "cmd.exe /c service.bat admin"
    static std::wstring MakeServiceBatCommand();

    bool Start(const std::wstring& commandLine, const std::filesystem::path& workDir, std::string& error);
    void Stop();
    bool IsRunning() const;
    bool WaitExit(DWORD timeoutMs);
    DWORD ExitCode() const;

    void Send(const std::string& utf8);       // как есть (перевод строки добавляет вызывающий)
    void SendLine(const std::string& utf8);   // текст + CRLF, с локальным эхо в окне терминала

    // ---- Для окна терминала ----
    std::string DisplayText() const;
    size_t      DisplayCounter() const { return counter_.load(); }
    void        ClearDisplay();

    // ---- Для автоматизации (ожидание подсказок service.bat) ----
    // Абсолютные смещения в потоке вывода. Поиск без учёта регистра.
    bool        Find(const std::string& needle, size_t from, size_t& at) const;
    bool        WaitFor(const std::string& needle, size_t& pos, DWORD timeoutMs, const std::atomic<bool>* cancel);
    std::string FeedRange(size_t from, size_t to) const;
    size_t      FeedEnd() const;
    ULONGLONG   LastActivity() const { return lastActivity_.load(); }

private:
    void ReaderLoop();
    void Ingest(const std::string& utf8);
    void CloseHandles();

    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE readPipe_ = nullptr;
    HANDLE writePipe_ = nullptr;
    std::thread reader_;
    std::mutex writeMutex_;

    mutable std::mutex mtx_;
    std::string display_;
    std::string feed_;
    size_t feedBase_ = 0;
    int escState_ = 0;
    bool pendingCR_ = false;
    std::atomic<size_t> counter_{0};
    std::atomic<ULONGLONG> lastActivity_{0};
};
