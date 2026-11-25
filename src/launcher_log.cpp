#include "launcher_log.h"

void LauncherLog::AppendLog(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(line);
    if (entries_.size() > 3000) entries_.pop_front();
    ++counter_;
}

void LauncherLog::ClearLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    ++counter_;
}

std::vector<std::string> LauncherLog::GetLogSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {entries_.begin(), entries_.end()};
}
