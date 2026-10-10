#include "pc/render/PcLegacyMeshScheduler.h"

#if PLATFORM_PC_LEGACY

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "java/Arithmetic.h"
#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/IChunkProvider.h"
#include "net/minecraft/src/World.h"
#include "net/minecraft/src/WorldRenderer.h"
#include "pc/tuning/PcLegacyTuning.h"
#include "platform/PlatformCompat.h"
#include "platform/PlatformTuning.h"
#include "platform/Profiler.h"

int g_pcLegacyMeshBudgetUs = 0;
std::uint64_t g_pcLegacyMeshSpentUs = 0;

namespace
{
    struct PcLegacyMeshCandidate
    {
        WorldRenderer *renderer;
        float distance;
        std::uint8_t editOrder;
        bool active;
        bool inFrustum;
    };

    static std::vector<PcLegacyMeshCandidate> s_candidates;

    class PcLegacyFrameMeshBudget
    {
    public:
        // Paused (a single-player menu over a still world): a longer budget,
        // so a settings change that rebuilds every section is done in seconds
        // rather than minutes; the menu runs at a lower frame rate meanwhile.
        PcLegacyFrameMeshBudget(int_t requestedUpdateLimit, bool paused)
            : startUs(PlatformCompat::getMonotonicMicros()),
              spentStepUs(0),
              steps(0),
              maxSteps(paused ? PC_LEGACY_PAUSED_MAX_STEPS_PER_FRAME : std::max(1, std::min(
                  PC_LEGACY_SCHEDULER_MAX_STEPS_PER_FRAME,
                  std::max(1, requestedUpdateLimit) * PC_LEGACY_SCHEDULER_STEPS_PER_UPDATE_UNIT))),
              budgetUs(paused ? PC_LEGACY_PAUSED_CHUNK_BUILD_BUDGET_MS * 1000
                  : g_pcLegacyMeshBudgetUs > 0 ? g_pcLegacyMeshBudgetUs
                  : PLATFORM_CHUNK_BUILD_BUDGET_MS * 1000)
        {
        }

        bool exhausted() const
        {
            if (steps >= maxSteps)
                return true;

            if (budgetUs <= 0)
                return false;

            const std::uint64_t nowUs = PlatformCompat::getMonotonicMicros();
            std::uint64_t elapsedUs = spentStepUs;
            if (startUs != 0 && nowUs > startUs)
                elapsedUs = std::max(elapsedUs, nowUs - startUs);

            if (elapsedUs >= static_cast<std::uint64_t>(budgetUs))
                return true;

            return steps > 0 &&
                elapsedUs + static_cast<std::uint64_t>(PLATFORM_CHUNK_BUILD_STEP_US) > static_cast<std::uint64_t>(budgetUs);
        }

        void run(WorldRenderer *renderer)
        {
            const std::uint64_t stepStartUs = PlatformCompat::getMonotonicMicros();
            renderer->updateRenderer();
            const std::uint64_t stepEndUs = PlatformCompat::getMonotonicMicros();

            ++steps;
            if (stepEndUs > stepStartUs)
                spentStepUs += stepEndUs - stepStartUs;
        }

    private:
        std::uint64_t startUs;
        std::uint64_t spentStepUs;
        int_t steps;
        int_t maxSteps;
        int_t budgetUs;
    };
}

void pcLegacyRunMeshScheduler(
    const std::vector<WorldRenderer *> &pending,
    EntityLiving *viewer,
    bool frustumOnly,
    int_t requestedUpdateLimit,
    bool paused)
{
    const std::size_t fixedRendererCapacity = static_cast<std::size_t>(
        PLATFORM_VISIBLE_CHUNK_DIAMETER *
        PLATFORM_VISIBLE_CHUNK_DIAMETER *
        PLATFORM_VERTICAL_CHUNK_COUNT);
    if (s_candidates.capacity() < fixedRendererCapacity)
        s_candidates.reserve(fixedRendererCapacity);

    // The selection (a pass over every dirty section: distance, decoration
    // check, sort) was redone every frame for ~1300 sections while exploring,
    // 1.8 ms. It is reused for a few frames now, continuing down the sorted
    // list; a player edit, moved renderers or a used-up list rescan at once.
    // New dirty sections wait up to PC_LEGACY_MESH_RESCAN_FRAMES frames.
    static std::size_t s_cursor = 0;
    static std::size_t s_count = 0;
    static unsigned s_framesSinceScan = PC_LEGACY_MESH_RESCAN_FRAMES;
    static unsigned s_scanLayout = ~0u;
    static unsigned s_scanEditStamp = ~0u;
    static bool s_scanFrustumOnly = false;
    const bool reuse = s_framesSinceScan + 1 < PC_LEGACY_MESH_RESCAN_FRAMES &&
        s_scanLayout == WorldRenderer::layoutVersion && s_scanEditStamp == WorldRenderer::editStamp &&
        s_scanFrustumOnly == frustumOnly && s_cursor < s_count;
    // Selection only (filter + sort); the builds are timed by "build".
    const std::uint64_t selectStartUs = PlatformCompat::getMonotonicMicros();
    struct SpentScope
    {
        std::uint64_t startUs;
        ~SpentScope() { g_pcLegacyMeshSpentUs = PlatformCompat::getMonotonicMicros() - startUs; }
    } spentScope{selectStartUs};
    std::size_t candidateCount = s_count;
    std::size_t candidateIndex = s_cursor;
    if (reuse)
    {
        ++s_framesSinceScan;
    }
    else
    {
    s_framesSinceScan = 0;
    s_scanLayout = WorldRenderer::layoutVersion;
    s_scanEditStamp = WorldRenderer::editStamp;
    s_scanFrustumOnly = frustumOnly;
    s_count = 0;
    s_cursor = 0;
    s_candidates.clear();
    bool editPending = false;
    for (WorldRenderer *candidate : pending)
    {
        if (candidate == nullptr || !candidate->needsUpdate)
            continue;

        const bool active = candidate->isTerrainBuildInProgress();
        const bool inFrustum = candidate->isInFrustum;
        const std::uint8_t editOrder = candidate->pcLegacyEditOrder;
        editPending |= editOrder != 0;
        if (frustumOnly && !inFrustum && !active)
            continue;

        const float distance = viewer != nullptr
            ? candidate->distanceToEntitySquared(viewer)
            : 0.0f;

#if PC_LEGACY_DEFER_MESH_DURING_POPULATE
        // Keep an already-published distant mesh visible while incremental
        // population can still mutate this section. Nearby sections remain
        // immediate so block interaction and close terrain edits stay responsive.
        // Unpublished sections are always allowed one provisional build so a new
        // chunk never becomes a visible hole just because decoration is pending.
        if (!active && editOrder == 0 && candidate->hasPublishedTerrain() && candidate->worldObj != nullptr &&
            distance > PC_LEGACY_POPULATE_MESH_DEFER_DISTANCE_SQ)
        {
            const int_t chunkX = JavaArithmetic::intShr(candidate->posX, 4);
            const int_t chunkZ = JavaArithmetic::intShr(candidate->posZ, 4);
            // The answer only changes when the decoration queue does (a few
            // times a second), but every waiting section asked it every frame:
            // four hash lookups each, ~1000 sections during a decoration backlog.
            if (candidate->pcLegacyPopulateMemoRevision != g_chunkPopulationPendingRevision ||
                candidate->pcLegacyPopulateMemoX != chunkX || candidate->pcLegacyPopulateMemoZ != chunkZ)
            {
                candidate->pcLegacyPopulateMemoPending =
                    candidate->worldObj->isChunkPopulationPendingForRendering(chunkX, chunkZ);
                candidate->pcLegacyPopulateMemoRevision = g_chunkPopulationPendingRevision;
                candidate->pcLegacyPopulateMemoX = chunkX;
                candidate->pcLegacyPopulateMemoZ = chunkZ;
            }
            if (candidate->pcLegacyPopulateMemoPending)
                continue;
        }
#endif
        s_candidates.push_back({candidate, distance, editOrder, active, inFrustum});
    }

    if (s_candidates.empty())
        return;

    // A player edit goes first, ahead of a streaming build in progress: that
    // build is dropped (it restarts later) since the one staging slot would
    // otherwise keep the edit waiting for frames.
    if (editPending)
    {
        for (PcLegacyMeshCandidate &candidate : s_candidates)
        {
            if (candidate.active && candidate.editOrder == 0)
            {
                candidate.renderer->pcLegacyAbandonBuild();
                candidate.active = false;
            }
        }
    }

    auto priority = [](const PcLegacyMeshCandidate &a, const PcLegacyMeshCandidate &b)
    {
        const int aEdit = a.editOrder != 0 ? a.editOrder : 3;
        const int bEdit = b.editOrder != 0 ? b.editOrder : 3;
        if (aEdit != bEdit)
            return aEdit < bEdit;

        const bool aActive = a.active;
        const bool bActive = b.active;
        if (aActive != bActive)
            return aActive;

        if (a.inFrustum != b.inFrustum)
            return a.inFrustum;

        if (a.distance != b.distance)
            return a.distance < b.distance;

        return a.renderer->chunkIndex < b.renderer->chunkIndex;
    };

    candidateCount = std::min<std::size_t>(
        s_candidates.size(), static_cast<std::size_t>(PC_LEGACY_RENDERER_UPDATE_CANDIDATES_PER_FRAME));
    if (candidateCount > 0)
    {
        std::partial_sort(
            s_candidates.begin(),
            s_candidates.begin() + candidateCount,
            s_candidates.end(),
            priority);
    }
    s_count = candidateCount;
    candidateIndex = 0;
    }

#if PLATFORM_PROFILE_RENDER_PHASES
    platformProfileTickPhase("meshSched",
        static_cast<long long>(PlatformCompat::getMonotonicMicros() - selectStartUs) * 1000LL);
#else
    (void)selectStartUs;
#endif
    PcLegacyFrameMeshBudget budget(requestedUpdateLimit, paused);
    WorldRenderer *current = nullptr;

    while (!budget.exhausted())
    {
        if (current == nullptr)
        {
            while (candidateIndex < candidateCount)
            {
                WorldRenderer *nextCandidate = s_candidates[candidateIndex++].renderer;
                if (nextCandidate != nullptr && nextCandidate->needsUpdate)
                {
                    current = nextCandidate;
                    break;
                }
            }
            if (current == nullptr)
                break;
        }

        budget.run(current);

        if (!current->needsUpdate)
        {
            current = nullptr;
            continue;
        }

        if (current->isTerrainBuildInProgress())
            continue;

        // Dependency-gated or otherwise non-active work gets one attempt per
        // frame pass. This prevents a missing source chunk from consuming the
        // entire Legacy meshing budget while another visible section is ready.
        current = nullptr;
    }
    // A build left in progress resumes next frame from the same entry.
    s_cursor = current != nullptr ? candidateIndex - 1 : candidateIndex;
}

#endif
