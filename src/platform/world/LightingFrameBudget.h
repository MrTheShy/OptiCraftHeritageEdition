#pragma once

#include <cstdint>

#include "platform/PlatformTuning.h"

// Extra time World::updatingLighting() may spend on a long queue, learned from
// frame pacing (the CPU waits inside the vsync, so busy time can't be read).
// After kProbeFrames on-time frames the budget grows by kStepUs; a late frame
// cuts it to 3/4 and holds growth for kHoldFrames. It changes when light jobs
// run, never what they compute. Main thread only.
namespace LightingFrameBudget
{
    struct Stats
    {
        uint32_t extraUs = 0;      // learned extra budget (adaptive mode)
        uint32_t lastJobs = 0;     // jobs run by the most recent drain
        uint32_t lastUsedUs = 0;   // wall time of that drain
        uint32_t lastBudgetUs = 0; // budget it was given
        uint32_t lastQueue = 0;    // queue size left behind
        uint32_t frameLate = 0;    // frames counted late since the last report
        uint32_t frameCount = 0;
        uint32_t onTimeRun = 0;
        uint32_t holdFrames = 0;
    };

    inline Stats &stats() { static Stats s; return s; }

    // Called once per rendered frame with the time since the previous one.
    inline void noteFrame(uint64_t frameUs, uint64_t periodUs)
    {
        constexpr uint32_t kStepUs = 250;
        constexpr uint32_t kProbeFrames = 20;
        constexpr uint32_t kHoldFrames = 90;
        Stats &s = stats();
        ++s.frameCount;
        if (periodUs == 0 || frameUs > 250000) // loading screen / pause: not a pacing signal
            return;
        if (s.holdFrames > 0)
            --s.holdFrames;
        if (frameUs * 4 <= periodUs * 5)
        {
            if (++s.onTimeRun >= kProbeFrames && s.holdFrames == 0 &&
                s.extraUs < (uint32_t)PLATFORM_LIGHTING_EXTRA_BUDGET_US)
            {
                s.extraUs += kStepUs;
                if (s.extraUs > (uint32_t)PLATFORM_LIGHTING_EXTRA_BUDGET_US)
                    s.extraUs = (uint32_t)PLATFORM_LIGHTING_EXTRA_BUDGET_US;
                s.onTimeRun = kProbeFrames - 4; // keep probing every 4 frames
            }
        }
        else
        {
            ++s.frameLate;
            s.onTimeRun = 0;
            s.holdFrames = kHoldFrames;
            s.extraUs = s.extraUs * 3 / 4;
        }
    }

    // mode: 0 = Smooth (no extra), 1 = Fast (learned), 2 = Fastest (fixed).
    inline uint32_t extraUs(int mode)
    {
        if (mode <= 0) return 0;
        if (mode >= 2) return (uint32_t)PLATFORM_LIGHTING_FASTEST_BUDGET_US;
        return stats().extraUs;
    }

    inline void recordDrain(uint32_t jobs, uint32_t usedUs, uint32_t budgetUs, uint32_t queueLeft)
    {
        Stats &s = stats();
        s.lastJobs = jobs;
        s.lastUsedUs = usedUs;
        s.lastBudgetUs = budgetUs;
        s.lastQueue = queueLeft;
    }
}
