#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class ConsoleSession;

struct ServiceStatus {
    bool serviceBatPresent = false;
    bool winwsPresent = false;
    bool driverPresent = false;

    bool installed = false;
    bool usesDirectory = false;
    bool supported = false;
    DWORD queryError = 0;
    std::filesystem::path executable;
    DWORD state = 0;
    std::string stateText = "Не установлена";
    std::string strategy;          // имя стратегии из реестра (без .bat)
    bool winwsRunning = false;

    std::string gameMode = "disabled";   // disabled / all / tcp / udp
    std::string gameTcp = "1024-65535";
    std::string gameUdp = "1024-65535";

    std::string ipsetMode = "missing";   // none / loaded / any / missing
    bool ipsetBackup = false;
};

// Операции над службой zapret и фильтрами Flowseal. Всё выполняется в фоновом потоке.
// Установка службы идёт через service.bat, удаление регистрации — через SCM.
class ServiceManager {
public:
    enum class State { Idle, Working, Success, Failed };
    using LogFn = std::function<void(const std::string&)>;

    ServiceManager(std::filesystem::path zapretDir, LogFn log);
    ~ServiceManager();
    ServiceManager(const ServiceManager&) = delete;
    ServiceManager& operator=(const ServiceManager&) = delete;

    void SetDirectory(std::filesystem::path directory);
    static ServiceStatus ReadStatus(const std::filesystem::path& zapretDir);

    void InstallStrategy(const std::string& batName);
    void RemoveService(std::filesystem::path expected);
    void SetRunning(bool running);
    void UpdateIpset();
    void UpdateHosts();
    void SetGameFilter(const std::string& mode);   // disabled / all / tcp / udp
    void SetIpsetMode(const std::string& mode);    // none / loaded / any

    bool        Busy() const { return state_.load() == State::Working; }
    State       GetState() const { return state_.load(); }
    std::string GetStatus() const;
    unsigned    Generation() const { return generation_.load(); }

private:
    void Launch(const std::string& title, std::function<void()> job);
    void SetStatus(const std::string& s);
    void Done(const std::string& s);
    void Fail(const std::string& s);
    void Log(const std::string& s);
    void LogOutput(const std::string& text);

    bool InstallCore(const std::string& bat, std::string& msg);
    bool UpdateIpsetCore(std::string& msg);
    bool SetIpsetCore(const std::string& mode, std::string& msg);
    bool UpdateHostsCore(std::string& msg);
    bool RestartServiceIfRunning(std::string& note);

    bool OpenMenu(ConsoleSession& cs, size_t& pos, std::string& menuText, std::string& err);
    size_t FinishToMenuAndExit(ConsoleSession& cs, size_t& pos);

    std::filesystem::path zapret_;
    LogFn log_;
    std::thread worker_;
    std::atomic<State> state_{State::Idle};
    std::atomic<unsigned> generation_{0};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mtx_;
    std::string status_;
};
