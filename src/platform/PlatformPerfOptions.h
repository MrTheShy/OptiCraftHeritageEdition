#pragma once

#include "platform/PlatformTuning.h"

// -----------------------------------------------------------------------------
// Runtime performance trades
// -----------------------------------------------------------------------------
// Every field is a cut that the compile-time PLATFORM_* tables used to force on
// low-end builds. The default of each one is that table's value, so a platform
// behaves exactly as before until the player flips it; `false` is the vanilla
// 1.2.5 behaviour. Only PLATFORM_PC_LEGACY builds expose these in the menus and
// persist them (GameSettings::setOptionValue, loadOptions, saveOptions); the
// other profiles keep the value the tuning table gave them.
struct PlatformPerfOptions
{
    // Entity AI decisions run every Nth tick for mobs far from the player.
    bool aiThrottle = PLATFORM_ENTITY_AI_FAR_TICK_DIVISOR > 1 || PLATFORM_ENTITY_AI_MID_TICK_DIVISOR > 1;
    // At most PLATFORM_PATHFIND_BUDGET_PER_TICK searches and a node budget per tick.
    bool pathLimits = PLATFORM_PATHFIND_BUDGET_PER_TICK < 0x7fffffff;
    // Hostile/passive spawning every Nth tick and a cap on live mobs.
    bool mobLimits = PLATFORM_MAX_LIVE_MOBS < 0x7fffffff || PLATFORM_MOB_SPAWN_INTERVAL_TICKS > 1;
    // Mobs only collide with each other near the player.
    bool pushLimit = PLATFORM_LIMIT_ENTITY_PUSH_COLLISIONS != 0;
    // Mobs beyond PLATFORM_ENTITY_RENDER_RADIUS_BLOCKS are not drawn.
    bool entityCull = PLATFORM_LIMIT_ENTITY_RENDER_DISTANCE != 0;
    // Block fragments, smoke, flames, drips: not simulated and not drawn.
    bool skipWorldParticles = PLATFORM_SKIP_WORLD_PARTICLES != 0;
    bool skipRainSnow = PLATFORM_SKIP_RAIN_SNOW != 0;
    // Hurt tilt, view bobbing and the portal wobble.
    bool skipCameraFx = PLATFORM_SKIP_CAMERA_FX != 0;
    bool skipEntityShadows = PLATFORM_SKIP_ENTITY_SHADOWS != 0;
    // Water/lava/fire/portal atlas animation steps every Nth tick.
    bool halfRateTextures = PLATFORM_DYNAMIC_TEXTURE_INTERVAL_TICKS > 1;
    // Random block ticks stop for the tick once a wall-clock budget is spent
    // and resume from the same chunk next tick.
    bool randomTickBudget = PLATFORM_RANDOM_TICK_BUDGET_US > 0;
};

inline PlatformPerfOptions &platformPerfMutable()
{
    static PlatformPerfOptions options;
    return options;
}

inline const PlatformPerfOptions &platformPerf()
{
    return platformPerfMutable();
}

// One row per menu entry, in EnumOptions ordinal order starting at
// PLATFORM_PERF_OPTION_FIRST_ORDINAL. `shownInverted` rows are phrased as the
// feature ("World Particles: ON") while the field holds the cut ("skip").
struct PlatformPerfOptionRow
{
    const char *label;
    const char *key; // options.txt
    bool PlatformPerfOptions::*field;
    bool shownInverted;
};

constexpr int PLATFORM_PERF_OPTION_FIRST_ORDINAL = 76;
constexpr int PLATFORM_PERF_OPTION_COUNT = 11;

inline const PlatformPerfOptionRow *platformPerfOptionRows()
{
    static const PlatformPerfOptionRow rows[PLATFORM_PERF_OPTION_COUNT] = {
        {"AI Throttle",      "perfAiThrottle",      &PlatformPerfOptions::aiThrottle,         false},
        {"Path Limits",      "perfPathLimits",      &PlatformPerfOptions::pathLimits,         false},
        {"Mob Limits",       "perfMobLimits",       &PlatformPerfOptions::mobLimits,          false},
        {"Push Limit",       "perfPushLimit",       &PlatformPerfOptions::pushLimit,          false},
        {"Entity Cull",      "perfEntityCull",      &PlatformPerfOptions::entityCull,         false},
        {"World Particles",  "perfWorldParticles",  &PlatformPerfOptions::skipWorldParticles, true},
        {"Rain & Snow Fx",   "perfRainSnow",        &PlatformPerfOptions::skipRainSnow,       true},
        {"Camera Effects",   "perfCameraFx",        &PlatformPerfOptions::skipCameraFx,       true},
        {"Entity Shadows",   "perfEntityShadows",   &PlatformPerfOptions::skipEntityShadows,  true},
        {"Slow Tex Anim",    "perfHalfRateTextures",&PlatformPerfOptions::halfRateTextures,   false},
        {"Tick Budget",      "perfRandomTickBudget",&PlatformPerfOptions::randomTickBudget,   false},
    };
    return rows;
}
