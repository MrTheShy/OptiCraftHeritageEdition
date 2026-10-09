#include "platform/PlatformConfig.h"

#if PLATFORM_PC_LEGACY

#include "net/minecraft/src/RenderGlobal.h"

#include "java/Arithmetic.h"
#include "net/minecraft/src/AxisAlignedBB.h"
#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/ICamera.h"
#include "net/minecraft/src/MathHelper.h"
#include "net/minecraft/src/World.h"
#include "net/minecraft/src/WorldRenderer.h"
#include "pc/render/PcLegacySectionVisibility.h"
#include "pc/tuning/PcLegacyTuning.h"

#include <cstdint>
#include <vector>

namespace
{
    constexpr int kFaceCount = 6;
    constexpr int kOppositeFace[kFaceCount] = {1, 0, 3, 2, 5, 4};
    constexpr int kOffsetX[kFaceCount] = {0, 0, 0, 0, -1, 1};
    constexpr int kOffsetY[kFaceCount] = {-1, 1, 0, 0, 0, 0};
    constexpr int kOffsetZ[kFaceCount] = {0, 0, -1, 1, 0, 0};

    int positiveModulo(int value, int modulus)
    {
        int result = value % modulus;
        if (result < 0)
            result += modulus;
        return result;
    }
}

int_t RenderGlobal::pcLegacyRendererIndexAtSection(int_t sectionX, int_t sectionY, int_t sectionZ) const
{
    if (worldRenderers == nullptr || renderChunksWide <= 0 || renderChunksTall <= 0 || renderChunksDeep <= 0)
        return -1;

    const int_t ySlot = sectionY - verticalStartSection;
    if (ySlot < 0 || ySlot >= renderChunksTall)
        return -1;

    const int_t xSlot = positiveModulo(sectionX, renderChunksWide);
    const int_t zSlot = positiveModulo(sectionZ, renderChunksDeep);
    const int_t index = (zSlot * renderChunksTall + ySlot) * renderChunksWide + xSlot;
    WorldRenderer *renderer = worldRenderers[index];
    if (renderer == nullptr ||
        JavaArithmetic::intShr(renderer->posX, 4) != sectionX ||
        JavaArithmetic::intShr(renderer->posY, 4) != sectionY ||
        JavaArithmetic::intShr(renderer->posZ, 4) != sectionZ)
    {
        return -1;
    }
    return index;
}

#if defined(VITA_PLATFORM) && PC_LEGACY_CPU_SECTION_OCCLUSION
// Same walk and same result as the generic version below, built for the A9:
//  - only what can be on screen is walked, as later vanilla does: never step
//    back toward the camera's section, never enter a section outside the
//    frustum;
//  - each exit of a section is tried once, not once per face it was entered
//    by (enqueue is idempotent, so the closure is unchanged);
//  - neighbours come from slot arithmetic on a table filled once per frame,
//    with no integer division (the A9 has no divider) and no renderer reads
//    for sections that are never entered.
namespace
{
    struct VisibilitySlot
    {
        int_t x, y, z; // section coordinates of the resident renderer
        std::uint8_t slotX, slotY, slotZ;
        bool present;
    };
    std::vector<VisibilitySlot> g_visibilitySlots;
    // Copies of each renderer's frustum box, contiguous: the walk tests them
    // without loading the renderer's box pointer and the scattered heap box.
    // Only setPosition changes a box, and it bumps layoutVersion.
    std::vector<AxisAlignedBB> g_visibilityBounds;
    std::vector<std::uint8_t> g_visibilityProcessed;
    std::vector<std::uint8_t> g_visibilityExited;
    std::vector<std::uint8_t> g_visibilityQueued;
    std::vector<int_t> g_visibilityMarked; // renderers set visible last frame
    unsigned g_visibilityLayoutVersion = ~0u;
    WorldRenderer **g_visibilityLayoutBase = nullptr;
    // For pcLegacyVisibleSortedOrder: grid index of each chunkIndex (fixed
    // while the grid is), position of each grid index in sortedWorldRenderers
    // (checked on use, rebuilt after a re-sort), and a bit per sorted position.
    std::vector<int_t> g_indexByChunk;
    std::vector<int_t> g_sortedRankByIndex;
    std::vector<std::uint32_t> g_visibleSortedBits;
}

void RenderGlobal::updatePcLegacySectionVisibility(EntityLiving *viewer)
{
    ICamera *frustum = pcLegacyVisibilityFrustum;
    pcLegacyVisibilityFrustum = nullptr;
    if (worldRenderers == nullptr)
        return;

    const int_t wide = renderChunksWide;
    const int_t tall = renderChunksTall;
    const int_t deep = renderChunksDeep;
    const int_t total = wide * tall * deep;
    if (total <= 0)
        return;

    // The slot table only changes when renderers move, so it is rebuilt then,
    // not every frame; and only the renderers marked visible last frame are
    // cleared, instead of a pass over the whole grid (~1400 renderer reads).
    const bool layoutChanged = g_visibilityLayoutVersion != WorldRenderer::layoutVersion ||
        g_visibilityLayoutBase != worldRenderers || g_visibilitySlots.size() != static_cast<std::size_t>(total);
    if (layoutChanged)
    {
        g_visibilityLayoutVersion = WorldRenderer::layoutVersion;
        g_visibilityLayoutBase = worldRenderers;
        g_visibilitySlots.resize(static_cast<std::size_t>(total));
        g_visibilityBounds.resize(static_cast<std::size_t>(total));
        g_indexByChunk.assign(static_cast<std::size_t>(total), -1);
        int_t index = 0;
        for (int_t zSlot = 0; zSlot < deep; ++zSlot)
            for (int_t ySlot = 0; ySlot < tall; ++ySlot)
                for (int_t xSlot = 0; xSlot < wide; ++xSlot, ++index)
                {
                    VisibilitySlot &slot = g_visibilitySlots[static_cast<std::size_t>(index)];
                    slot.slotX = static_cast<std::uint8_t>(xSlot);
                    slot.slotY = static_cast<std::uint8_t>(ySlot);
                    slot.slotZ = static_cast<std::uint8_t>(zSlot);
                    WorldRenderer *renderer = worldRenderers[index];
                    slot.present = renderer != nullptr;
                    if (renderer == nullptr)
                        continue;
                    renderer->pcLegacyCpuVisible = false;
                    if (renderer->rendererBoundingBox != nullptr)
                        g_visibilityBounds[static_cast<std::size_t>(index)] = *renderer->rendererBoundingBox;
                    if (renderer->chunkIndex >= 0 && renderer->chunkIndex < total)
                        g_indexByChunk[static_cast<std::size_t>(renderer->chunkIndex)] = index;
                    slot.x = JavaArithmetic::intShr(renderer->posX, 4);
                    slot.y = JavaArithmetic::intShr(renderer->posY, 4);
                    slot.z = JavaArithmetic::intShr(renderer->posZ, 4);
                }
    }
    else
    {
        for (int_t index : g_visibilityMarked)
            worldRenderers[index]->pcLegacyCpuVisible = false;
    }
    g_visibilityMarked.clear();
    auto markVisible = [&](int_t index)
    {
        WorldRenderer *renderer = worldRenderers[index];
        if (!renderer->pcLegacyCpuVisible)
        {
            renderer->pcLegacyCpuVisible = true;
            g_visibilityMarked.push_back(index);
        }
    };

    if (viewer == nullptr)
    {
        for (int_t i = 0; i < total; ++i)
        {
            if (worldRenderers[i] == nullptr)
                continue;
            markVisible(i);
            if (frustum != nullptr)
                worldRenderers[i]->updateInFrustrum(frustum);
        }
        return;
    }

    const int_t cameraSectionX = JavaArithmetic::intShr(MathHelper::floor_double(viewer->posX), 4);
    const int_t cameraSectionY = JavaArithmetic::intShr(MathHelper::floor_double(viewer->posY), 4);
    const int_t cameraSectionZ = JavaArithmetic::intShr(MathHelper::floor_double(viewer->posZ), 4);
    const int_t cameraIndex = pcLegacyRendererIndexAtSection(cameraSectionX, cameraSectionY, cameraSectionZ);
    if (cameraIndex < 0)
    {
        for (int_t i = 0; i < total; ++i)
        {
            if (worldRenderers[i] == nullptr)
                continue;
            markVisible(i);
            if (frustum != nullptr)
                worldRenderers[i]->updateInFrustrum(frustum);
        }
        return;
    }

    const std::size_t count = static_cast<std::size_t>(total);
    pcLegacyVisibilityEntryMasks.assign(count, 0);
    g_visibilityProcessed.assign(count, 0);
    g_visibilityExited.assign(count, 0);
    g_visibilityQueued.assign(count, 0);
    pcLegacyVisibilityQueue.clear();

    constexpr std::uint8_t kRejected = 0x80;
    const int_t layer = wide * tall;
    std::vector<int_t> &queue = pcLegacyVisibilityQueue;

    auto enqueueNeighbour = [&](int_t from, int exitFace)
    {
        const VisibilitySlot &slot = g_visibilitySlots[static_cast<std::size_t>(from)];
        if ((slot.x - cameraSectionX) * kOffsetX[exitFace] < 0 ||
            (slot.y - cameraSectionY) * kOffsetY[exitFace] < 0 ||
            (slot.z - cameraSectionZ) * kOffsetZ[exitFace] < 0)
            return;

        int_t neighbour;
        switch (exitFace)
        {
            case 0: if (slot.slotY == 0) return; neighbour = from - wide; break;
            case 1: if (slot.slotY + 1 >= tall) return; neighbour = from + wide; break;
            case 2: neighbour = slot.slotZ == 0 ? from + (deep - 1) * layer : from - layer; break;
            case 3: neighbour = slot.slotZ + 1 >= deep ? from - (deep - 1) * layer : from + layer; break;
            case 4: neighbour = slot.slotX == 0 ? from + (wide - 1) : from - 1; break;
            default: neighbour = slot.slotX + 1 >= wide ? from - (wide - 1) : from + 1; break;
        }
        const VisibilitySlot &next = g_visibilitySlots[static_cast<std::size_t>(neighbour)];
        if (!next.present || next.x != slot.x + kOffsetX[exitFace] ||
            next.y != slot.y + kOffsetY[exitFace] || next.z != slot.z + kOffsetZ[exitFace])
            return;

        const std::uint8_t entryBit = static_cast<std::uint8_t>(1u << kOppositeFace[exitFace]);
        std::uint8_t &entryMask = pcLegacyVisibilityEntryMasks[static_cast<std::size_t>(neighbour)];
        if ((entryMask & (entryBit | kRejected)) != 0)
            return;
        WorldRenderer *renderer = worldRenderers[neighbour];
        if (entryMask == 0 && frustum != nullptr)
        {
            // First visit this frame. Empty sections skip the per-frame clip
            // pass, so their flag can be stale: test here, where it matters.
            renderer->updateInFrustrum(frustum, &g_visibilityBounds[static_cast<std::size_t>(neighbour)]);
            if (!renderer->isInFrustum)
            {
                entryMask = kRejected;
                return;
            }
        }
        entryMask |= entryBit;
        markVisible(neighbour);
        if (!g_visibilityQueued[static_cast<std::size_t>(neighbour)])
        {
            g_visibilityQueued[static_cast<std::size_t>(neighbour)] = 1;
            queue.push_back(neighbour);
        }
    };

    markVisible(cameraIndex);
    if (frustum != nullptr)
        worldRenderers[cameraIndex]->updateInFrustrum(frustum, &g_visibilityBounds[static_cast<std::size_t>(cameraIndex)]);
    for (int exitFace = 0; exitFace < kFaceCount; ++exitFace)
        enqueueNeighbour(cameraIndex, exitFace);

    for (std::size_t read = 0; read < queue.size(); ++read)
    {
        const int_t index = queue[read];
        const std::size_t cell = static_cast<std::size_t>(index);
        g_visibilityQueued[cell] = 0;
        const std::uint8_t newEntries = static_cast<std::uint8_t>(
            pcLegacyVisibilityEntryMasks[cell] & 0x3f & ~g_visibilityProcessed[cell]);
        g_visibilityProcessed[cell] |= newEntries;

        WorldRenderer *renderer = worldRenderers[index];
        // A section not built yet has no connectivity. If its column is
        // resident it is a hole for now: the walk passes through it (all
        // faces open), or every built section behind it went black until it
        // was built (joining a server: ~2300 sections queued, the world dark
        // for tens of seconds). A column that is not resident (past the
        // server's or the generator's range) stops the walk: treated as
        // open, that outer ring let the walk visit every section in the
        // frustum each frame.
        const bool built = renderer->hasPublishedTerrain() || index == cameraIndex;
        if (!built)
        {
            const VisibilitySlot &slot = g_visibilitySlots[cell];
            if (renderer->worldObj == nullptr || renderer->worldObj->getChunkIfExists(slot.x, slot.z) == nullptr)
                continue;
        }
        std::uint8_t exits = 0;
        for (int entryFace = 0; entryFace < kFaceCount; ++entryFace)
        {
            if ((newEntries & (1u << entryFace)) != 0)
                exits |= built ? renderer->pcLegacyVisibleFacesFrom(entryFace) : PcLegacySectionVisibility::kAllFaces;
        }
        exits = static_cast<std::uint8_t>(exits & ~g_visibilityExited[cell]);
        g_visibilityExited[cell] |= exits;
        for (int exitFace = 0; exitFace < kFaceCount; ++exitFace)
        {
            if ((exits & (1u << exitFace)) != 0)
                enqueueNeighbour(index, exitFace);
        }
    }

    // Every ~10 s: renderers neither built nor queued (needsUpdate false) are
    // "orphans": the walk stops at them, so everything past one is not drawn.
    // Mark them dirty so the scheduler builds them.
    static unsigned frames = 0;
    if (++frames >= 600)
    {
        frames = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            WorldRenderer *renderer = worldRenderers[i];
            if (renderer != nullptr && !renderer->hasPublishedTerrain() && !renderer->needsUpdate)
            {
                renderer->markDirty();
                enqueueRendererUpdate(renderer);
            }
        }
    }
}

// The sorted positions of the sections the walk marked visible, ascending: the
// only ones a terrain pass can draw, in the order the full scan of
// sortedWorldRenderers would meet them. False when the marks do not describe
// the current grid (it moved or was rebuilt since the walk): scan everything.
bool RenderGlobal::pcLegacyVisibleSortedOrder(std::vector<int_t> &order)
{
    const int_t total = renderChunksWide * renderChunksTall * renderChunksDeep;
    if (worldRenderers == nullptr || sortedWorldRenderers == nullptr || total <= 0 ||
        g_visibilityLayoutBase != worldRenderers || g_visibilityLayoutVersion != WorldRenderer::layoutVersion ||
        g_visibilitySlots.size() != static_cast<std::size_t>(total) ||
        g_indexByChunk.size() != static_cast<std::size_t>(total))
        return false;

    const std::size_t words = static_cast<std::size_t>((total + 31) >> 5);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (attempt > 0 || g_sortedRankByIndex.size() != static_cast<std::size_t>(total))
        {
            // Only after a re-sort (every 4 blocks walked) or a new grid.
            g_sortedRankByIndex.assign(static_cast<std::size_t>(total), -1);
            for (int_t position = 0; position < total; ++position)
            {
                WorldRenderer *renderer = sortedWorldRenderers[position];
                if (renderer == nullptr || renderer->chunkIndex < 0 || renderer->chunkIndex >= total)
                    return false;
                const int_t index = g_indexByChunk[static_cast<std::size_t>(renderer->chunkIndex)];
                if (index < 0 || worldRenderers[index] != renderer)
                    return false;
                g_sortedRankByIndex[static_cast<std::size_t>(index)] = position;
            }
        }

        g_visibleSortedBits.assign(words, 0u);
        bool current = true;
        for (int_t index : g_visibilityMarked)
        {
            const int_t position = g_sortedRankByIndex[static_cast<std::size_t>(index)];
            if (position < 0 || sortedWorldRenderers[position] != worldRenderers[index])
            {
                current = false; // re-sorted since the ranks were taken
                break;
            }
            g_visibleSortedBits[static_cast<std::size_t>(position >> 5)] |= 1u << (position & 31);
        }
        if (!current)
            continue;

        order.clear();
        for (std::size_t word = 0; word < words; ++word)
        {
            for (std::uint32_t bits = g_visibleSortedBits[word]; bits != 0; bits &= bits - 1)
                order.push_back(static_cast<int_t>(word << 5) + __builtin_ctz(bits));
        }
        return true;
    }
    return false;
}
#else
void RenderGlobal::updatePcLegacySectionVisibility(EntityLiving *viewer)
{
#if !PC_LEGACY_CPU_SECTION_OCCLUSION
    (void)viewer;
    return;
#else
    if (worldRenderers == nullptr)
        return;

    const int_t total = renderChunksWide * renderChunksTall * renderChunksDeep;
    if (total <= 0)
        return;

    for (int_t i = 0; i < total; ++i)
    {
        if (worldRenderers[i] != nullptr)
            worldRenderers[i]->pcLegacyCpuVisible = false;
    }

    if (viewer == nullptr)
    {
        for (int_t i = 0; i < total; ++i)
        {
            if (worldRenderers[i] != nullptr)
                worldRenderers[i]->pcLegacyCpuVisible = true;
        }
        return;
    }

    const int_t cameraSectionX = JavaArithmetic::intShr(MathHelper::floor_double(viewer->posX), 4);
    const int_t cameraSectionY = JavaArithmetic::intShr(MathHelper::floor_double(viewer->posY), 4);
    const int_t cameraSectionZ = JavaArithmetic::intShr(MathHelper::floor_double(viewer->posZ), 4);
    const int_t cameraIndex = pcLegacyRendererIndexAtSection(cameraSectionX, cameraSectionY, cameraSectionZ);
    if (cameraIndex < 0)
    {
        for (int_t i = 0; i < total; ++i)
        {
            if (worldRenderers[i] != nullptr)
                worldRenderers[i]->pcLegacyCpuVisible = true;
        }
        return;
    }

    pcLegacyVisibilityEntryMasks.assign(static_cast<std::size_t>(total), 0);
    pcLegacyVisibilityQueue.clear();
    const std::size_t queueCapacity = static_cast<std::size_t>(total) * kFaceCount;
    if (pcLegacyVisibilityQueue.capacity() < queueCapacity)
        pcLegacyVisibilityQueue.reserve(queueCapacity);

    WorldRenderer *cameraRenderer = worldRenderers[cameraIndex];
    cameraRenderer->pcLegacyCpuVisible = true;

#ifdef VITA_PLATFORM
    // Walk only what can be on screen, as later vanilla does: never step back
    // toward the camera's section, and never enter a section outside the
    // frustum. On a large grid the unrestricted walk cost more than drawing.
    ICamera *frustum = pcLegacyVisibilityFrustum;
    pcLegacyVisibilityFrustum = nullptr;
    constexpr std::uint8_t kRejected = 0x80;
#endif

    auto enqueueNeighbour = [&](WorldRenderer *renderer, int exitFace)
    {
        const int_t rendererX = JavaArithmetic::intShr(renderer->posX, 4);
        const int_t rendererY = JavaArithmetic::intShr(renderer->posY, 4);
        const int_t rendererZ = JavaArithmetic::intShr(renderer->posZ, 4);
#ifdef VITA_PLATFORM
        const int_t awayX = rendererX - cameraSectionX;
        const int_t awayY = rendererY - cameraSectionY;
        const int_t awayZ = rendererZ - cameraSectionZ;
        if (awayX * kOffsetX[exitFace] < 0 || awayY * kOffsetY[exitFace] < 0 || awayZ * kOffsetZ[exitFace] < 0)
            return;
#endif
        const int_t sectionX = rendererX + kOffsetX[exitFace];
        const int_t sectionY = rendererY + kOffsetY[exitFace];
        const int_t sectionZ = rendererZ + kOffsetZ[exitFace];
        const int_t neighbourIndex = pcLegacyRendererIndexAtSection(sectionX, sectionY, sectionZ);
        if (neighbourIndex < 0)
            return;

        const int entryFace = kOppositeFace[exitFace];
        const std::uint8_t entryBit = static_cast<std::uint8_t>(1u << entryFace);
        std::uint8_t &entryMask = pcLegacyVisibilityEntryMasks[static_cast<std::size_t>(neighbourIndex)];
        if ((entryMask & entryBit) != 0)
            return;
#ifdef VITA_PLATFORM
        if ((entryMask & kRejected) != 0)
            return;
        if (entryMask == 0 && frustum != nullptr)
        {
            // First visit this frame. Empty sections skip the per-frame clip
            // pass, so their flag can be stale: test here, where it matters.
            WorldRenderer *candidate = worldRenderers[neighbourIndex];
            candidate->updateInFrustrum(frustum);
            if (!candidate->isInFrustum)
            {
                entryMask = kRejected;
                return;
            }
        }
#endif

        entryMask |= entryBit;
        WorldRenderer *neighbour = worldRenderers[neighbourIndex];
        neighbour->pcLegacyCpuVisible = true;
        pcLegacyVisibilityQueue.push_back(neighbourIndex * kFaceCount + entryFace);
    };

    for (int exitFace = 0; exitFace < kFaceCount; ++exitFace)
        enqueueNeighbour(cameraRenderer, exitFace);

    std::size_t queueRead = 0;
    while (queueRead < pcLegacyVisibilityQueue.size())
    {
        const int_t encoded = pcLegacyVisibilityQueue[queueRead++];
        const int_t rendererIndex = encoded / kFaceCount;
        const int_t entryFace = encoded % kFaceCount;
        if (rendererIndex < 0 || rendererIndex >= total)
            continue;

        WorldRenderer *renderer = worldRenderers[rendererIndex];
        if (renderer == nullptr)
            continue;

        const std::uint8_t exits = renderer->pcLegacyVisibleFacesFrom(entryFace);
        for (int exitFace = 0; exitFace < kFaceCount; ++exitFace)
        {
            if ((exits & (1u << exitFace)) == 0)
                continue;
            enqueueNeighbour(renderer, exitFace);
        }
    }
#endif
}
#endif

#endif
