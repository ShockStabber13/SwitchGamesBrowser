#pragma once
#include <switch.h>
#include <chrono>
#include <string>
#include <vector>

namespace sgb {
std::vector<std::string> probeCpuClockReadOnly();

// Controlled CPU-only clock experiment: 10-second mode or app-session mode.
// Both restore the original rate on normal exit. No RAM/GPU/voltage writes.
class CpuClockBoostTrial {
public:
    ~CpuClockBoostTrial();
    CpuClockBoostTrial(const CpuClockBoostTrial&) = delete;
    CpuClockBoostTrial& operator=(const CpuClockBoostTrial&) = delete;
    CpuClockBoostTrial() = default;

    std::vector<std::string> begin(u32 requestedHz = 1224000000u, bool holdUntilChanged = false);
    std::vector<std::string> tick();
    std::vector<std::string> stop(const char* reason);
    bool active() const { return active_; }
    bool held() const { return active_ && holdUntilChanged_; }
    u32 targetHz() const { return active_ ? targetHz_ : 0; }
    unsigned secondsRemaining() const;

private:
    void close();
    ClkrstSession session_{};
    bool initialized_ = false;
    bool sessionOpen_ = false;
    bool active_ = false;
    u32 originalHz_ = 0;
    u32 targetHz_ = 0;
    bool holdUntilChanged_ = false;
    std::chrono::steady_clock::time_point deadline_{};
};
} // namespace sgb
