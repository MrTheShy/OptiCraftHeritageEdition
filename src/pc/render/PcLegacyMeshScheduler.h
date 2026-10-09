#pragma once

#include "platform/PlatformConfig.h"

#if PLATFORM_PC_LEGACY

#include <vector>

#include "java/Type.h"

class EntityLiving;
class WorldRenderer;

#include <cstdint>

// This frame's mesh budget in microseconds (0: PLATFORM_CHUNK_BUILD_BUDGET_MS),
// and the time the scheduler spent this frame, for the next frame's estimate.
extern int g_pcLegacyMeshBudgetUs;
extern std::uint64_t g_pcLegacyMeshSpentUs;

void pcLegacyRunMeshScheduler(
    const std::vector<WorldRenderer *> &pending,
    EntityLiving *viewer,
    bool frustumOnly,
    int_t requestedUpdateLimit,
    bool paused = false);

#endif
