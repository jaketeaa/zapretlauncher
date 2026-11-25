#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Скачивание и обновление сборки Flowseal.
class GithubUpdater {
public:
    enum class State { Idle, Working, Success, Failed };
    enum class Task  { Install, Check };

    GithubUpdater(std::filesystem::path zapretDir, std::filesystem::path configPath);
    ~GithubUpdater();
    GithubUpdater(const GithubUpdater&) = delete;
    GithubUpdater& operator=(const GithubUpdater&) = delete;

    void SetDirectory(std::filesystem::path directory);
    std::string LatestTag() const;
    std::vector<std::string> MissingFiles() const;   // service.bat, bin/winws.exe, bin/WinDivert64.sys
    void Start(Task task);                           // запуск в отдельном потоке

    State GetState() const { return state_.load(); }
    bool Busy() const { return state_.load() == State::Working; }
    bool Installing() const { return Busy() && task_ == Task::Install; }
    std::string GetStatus() const;
    float GetProgress() const { return progress_.load(); }       // 0..1, либо < 0 = неопределённый
    int   UpdateStatus() const { return updateStatus_.load(); }  // -1 неизвестно, 0 актуально, 1 есть обновление
    unsigned Generation() const { return generation_.load(); }
    std::string LocalTag() const;                    // установленная версия (тег релиза) или пустая строка

private:
    struct Release {
        std::string tag;
        std::wstring downloadUrl;
        std::string assetName;
    };

    void Run(Task task);
    void DoCheck();
    void DoInstall();
    bool FetchRelease(Release& rel, std::string& err);
    void SetStatus(const std::string& s);
    void Fail(const std::string& s);
    void Done(const std::string& s);

    std::filesystem::path zapret_;
    std::filesystem::path configPath_;
    std::thread worker_;
    Task task_ = Task::Check;
    std::atomic<State> state_{State::Idle};
    std::atomic<float> progress_{-1.0f};
    std::atomic<int> updateStatus_{-1};
    std::atomic<unsigned> generation_{0};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mtx_;
    std::string status_;
    std::string latestTag_;
};
