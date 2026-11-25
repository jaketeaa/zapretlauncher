#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class LauncherLog {
public:
    void AppendLog(const std::string& line);
    void ClearLog();
    std::vector<std::string> GetLogSnapshot();
    size_t LogCounter() const { return counter_.load(); }

private:
    std::mutex mutex_;
    std::deque<std::string> entries_;
    std::atomic<size_t> counter_{0};
};
