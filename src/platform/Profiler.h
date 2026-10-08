#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "platform/PlatformConfig.h"

enum class PlatformRenderPhase
{
    Sky = 0,
    Frustum,
    Build,
    Opaque,
    Entities,
    Translucent,
    Hand,
    Hud,
    // Appended, not inserted: PS2's static_cast dispatch (Profiler_PS2.cpp ->
    // ps2_perf_add_render_phase) indexes this enum directly, so reordering the
    // values above would silently relabel their samples. Both are sub-phases of
    // Entities, not siblings of it; see Ps2RenderPhase.h. Wii and PC implement
    // platformProfileRenderPhaseEnd as a no-op and ignore them.
    EntityDraw,
    TileEntityDraw,
    // Sub-phases of Hud, on the same terms as the two above: nested inside it,
    // not siblings. Hud minus all three is the frame/crosshair/status setup.
    HudItems,
    HudText,
    HudHints
};

enum class PlatformPopulatePhase
{
    Total = 0,
    Lakes,
    Dungeons,
    Fillers,
    Ores,
    Decoration,
    Springs,
    Snow,
    // Appended, not inserted: PS2's static_cast dispatch (Profiler_PS2.cpp) and
    // Wii's (Profiler_WII.cpp, bounds-checked at 8) both index this enum
    // directly, so reordering existing values would silently relabel their
    // samples. Wii ignores this one (its bound check is `< 8`); it was never
    // wired to a distinct bucket there either, so nothing regresses.
    Structures
};

// Which kind of thing platformProfileSlowItem() names. The backend keeps the
// slowest of each kind per frame and per report window.
enum class PlatformSlowItemGroup
{
    Entity = 0,      // class of the entity whose update() was slowest
    RandomBlock = 1, // block class whose random updateTick() was slowest
    Count
};

std::uint32_t platformProfileRenderPhaseBegin();
void platformProfileRenderPhaseEnd(std::uint32_t start, PlatformRenderPhase phase);
void platformProfileTickPhase(const char* name, long long ns);
void platformProfileSlowItem(PlatformSlowItemGroup group, const char* name, long long ns);
void platformProfileChunkBuild(long long ns, int vertices);
void platformProfileChunkMeshPass(int pass, long long ns, int vertices);
void platformProfileSnowColumn(bool icePlaced, bool snowPlaced, int notifyCalls);
void platformProfilePopulatePhase(PlatformPopulatePhase phase, long long ns);

void platformProfileChunkLoad(long long ns);
void platformProfilePopulate(long long ns);
void platformProfileGenerate(long long ns);
void platformProfileMesh(long long ns);
void platformProfileUnloadSave(long long ns);
void platformProfileTickUpdates(long long ns);
void platformProfileTickQueue(long long size);
void platformProfileMobSpawn(long long ns);
void platformProfileSaveWorldInfo(long long ns);
void platformProfileMapStorage(long long ns);
void platformProfileChunkEvict(long long ns);

// Times the enclosing block as the named tick phase. Compiles to nothing where
// tick phases are not collected, so call sites need no #if.
#if PLATFORM_PROFILE_RENDER_PHASES
class PlatformTickPhaseScope
{
public:
    explicit PlatformTickPhaseScope(const char* phaseName)
        : name(phaseName), start(std::chrono::steady_clock::now()) {}
    ~PlatformTickPhaseScope()
    {
        platformProfileTickPhase(name, std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
    }
    PlatformTickPhaseScope(const PlatformTickPhaseScope&) = delete;
    PlatformTickPhaseScope& operator=(const PlatformTickPhaseScope&) = delete;
private:
    const char* name;
    std::chrono::steady_clock::time_point start;
};
#else
class PlatformTickPhaseScope
{
public:
    explicit PlatformTickPhaseScope(const char*) {}
};
#endif

#if PLATFORM_PROFILE_RENDER_PHASES
// Time and count per class over one pass (an entity tick, an entity draw),
// reported as "<prefix><Class>" tick phases and "#n<prefix><Class>" counters.
// Keyed by typeid(...).name(); classes beyond the first 12 are not reported.
class PlatformClassCost
{
public:
    void add(const char* mangled, long long ns)
    {
        int c = 0;
        while (c < count && slots[c].mangled != mangled)
            c++;
        if (c == count)
        {
            if (count == kSlots)
                return;
            slots[count++] = {mangled, 0, 0};
        }
        slots[c].ns += ns;
        slots[c].n++;
    }
    void flush(char prefix) const
    {
        for (int c = 0; c < count; c++)
        {
            // Itanium name ("12EntityZombie") -> "Zombie"
            const char* name = slots[c].mangled;
            while (*name >= '0' && *name <= '9')
                name++;
            if (std::strncmp(name, "TileEntity", 10) == 0 && name[10] != '\0')
                name += 10;
            else if (std::strncmp(name, "Entity", 6) == 0 && name[6] != '\0')
                name += 6;
            char phase[16];
            std::snprintf(phase, sizeof(phase), "%c%s", prefix, name);
            platformProfileTickPhase(phase, slots[c].ns);
            std::snprintf(phase, sizeof(phase), "#n%c%s", prefix, name);
            platformProfileTickPhase(phase, slots[c].n);
        }
    }
private:
    static constexpr int kSlots = 12;
    struct Slot { const char* mangled; long long ns; int n; };
    Slot slots[kSlots];
    int count = 0;
};
#endif
