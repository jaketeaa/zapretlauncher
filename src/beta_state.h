#pragma once

#include "diagnostics.h"
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace beta {
using Clock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;

std::string LocalTime(WallClock::time_point time, const char* pattern = "%H:%M:%S");

class Uptime {
public:
    void Observe(bool running, bool known, std::uint64_t filetime);
    bool Verified() const { return running_ && started_ != 0; }
    std::string Text(Clock::time_point now = Clock::now()) const;
    Clock::time_point NextChange(Clock::time_point now) const;
private:
    bool running_ = false, known_ = true;
    std::uint64_t started_ = 0;
    Clock::time_point anchor_{};
    Clock::duration Elapsed(Clock::time_point now) const;
    Clock::duration elapsedAtAnchor_{};
};

struct Sample {
    WallClock::time_point date;
    Clock::time_point time;
    std::optional<long long> ms;
};
class History {
public:
    void Add(const std::vector<diagnostics::Result>& results);
    void Clear();
    const std::deque<Sample>& operator[](std::size_t i) const { return samples_[i]; }
private:
    std::deque<Sample> samples_[std::size(diagnostics::kServices)];
};

enum class Kind { Success, Info, Warning, Error };
struct Notification {
    unsigned id;
    Kind kind;
    std::string title, message;
    WallClock::time_point date;
    Clock::time_point time;
    bool unread = true, toast = true;
};
// Submitted by the UI thread after polling completed background operations.
class Notifications {
public:
    void Add(Kind kind, const std::string& title, const std::string& message);
    std::size_t Unread() const;
    void MarkAll();
    void Clear() { events.clear(); }
    std::deque<Notification> events;
private:
    unsigned next_ = 1;
};
}
