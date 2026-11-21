// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <chrono>

namespace oxrsys
{
// Keep a stable deadline grid. Oversleep must shorten the next wait rather
// than shift every subsequent frame; late applications need no extra wait.
class FramePacer
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Period = std::chrono::nanoseconds;
    TimePoint Deadline(TimePoint now, Period period)
    {
        if (period.count() <= 0) period = Period(1);
        if (!initialized_ || period != period_)
        {
            period_ = period;
            next_ = now + period_;
            initialized_ = true;
        }
        return next_;
    }
    void Advance(TimePoint now)
    {
        if (!initialized_) return;
        next_ += period_;
        if (next_ <= now)
            next_ += period_ * ((now - next_) / period_ + 1);
    }
    void Reset() { initialized_ = false; }
private:
    bool initialized_ = false;
    Period period_{};
    TimePoint next_{};
};
}
