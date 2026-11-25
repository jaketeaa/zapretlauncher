#include "beta_state.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace beta {
std::string LocalTime(WallClock::time_point time, const char* pattern) {
    const auto value = WallClock::to_time_t(time);
    std::tm local{};
    char text[64]{};
    if (localtime_s(&local, &value) != 0 || !std::strftime(text, sizeof(text), pattern, &local)) return "—";
    return text;
}

void Uptime::Observe(bool running, bool known, std::uint64_t filetime) {
    running_ = running;
    known_ = known;
    if (!running || !filetime) {
        started_ = 0;
        return;
    }
    if (started_ == filetime) return;
    FILETIME now{};
    const auto anchor = Clock::now();
    GetSystemTimePreciseAsFileTime(&now);
    const auto wallNow = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    started_ = filetime <= wallNow ? filetime : 0;
    if (started_) {
        anchor_ = anchor;
        using FileTimeTicks = std::chrono::duration<std::uint64_t, std::ratio<1, 10000000>>;
        elapsedAtAnchor_ = std::chrono::duration_cast<Clock::duration>(FileTimeTicks(wallNow - started_));
    }
}

Clock::duration Uptime::Elapsed(Clock::time_point now) const {
    return elapsedAtAnchor_ + std::max(Clock::duration::zero(), now - anchor_);
}

Clock::time_point Uptime::NextChange(Clock::time_point now) const {
    if (!known_ || !Verified()) return Clock::time_point::max();
    const auto elapsed = Elapsed(now);
    const auto boundary = std::chrono::floor<std::chrono::seconds>(elapsed) + std::chrono::seconds(1);
    return now + std::chrono::duration_cast<Clock::duration>(boundary - elapsed);
}

std::string Uptime::Text(Clock::time_point now) const {
    if (!known_ || (running_ && !started_)) return "—";
    const auto seconds = running_ ? std::chrono::floor<std::chrono::seconds>(Elapsed(now)).count() : 0;
    char text[32]{};
    std::snprintf(text, sizeof(text), "%02lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
    return text;
}

void History::Add(const std::vector<diagnostics::Result>& results) {
    const auto date = WallClock::now();
    const auto time = Clock::now();
    for (std::size_t i = 0; i < std::min(std::size(samples_), results.size()); ++i) {
        auto& series = samples_[i];
        if (series.size() == 120) series.pop_front();
        const auto& result = results[i];
        const bool measured = result.measuredTick != Clock::time_point{};
        series.push_back({measured ? result.measuredAt : date, measured ? result.measuredTick : time, result.latencyMs});
    }
}

void History::Clear() {
    for (auto& series : samples_) series.clear();
}

void Notifications::Add(Kind kind, const std::string& title, const std::string& message) {
    const auto now = Clock::now();
    for (const auto& event : events) {
        if (now - event.time > std::chrono::seconds(45)) break;
        if (event.kind == kind && event.title == title && event.message == message) return;
    }
    if (events.size() == 100) events.pop_back();
    events.push_front({next_++, kind, title, message, WallClock::now(), now});
}
std::size_t Notifications::Unread() const {
    return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.unread; }));
}
void Notifications::MarkAll() {
    for (auto& event : events) event.unread = false;
}
}
