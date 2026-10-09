#include "pc/render/PcLegacyMeshWorker.h"

#if PLATFORM_PC_LEGACY && PLATFORM_MESH_WORKER

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#ifdef VITA_PLATFORM
#include <psp2/kernel/threadmgr.h>
#endif

#include "net/minecraft/src/ChunkCache.h"
#include "net/minecraft/src/ExtendedBlockStorage.h"
#include "net/minecraft/src/WorldHeight.h"
#include "pc/render/PcLegacyCubeMaterialInfo.h"
#include "pc/render/PcLegacySectionCache.h"
#include "platform/RenderAPI.h"

namespace
{
    constexpr int kSize = PcLegacySectionCache::kSize;
    constexpr int kCells = PcLegacySectionCache::kCellCount;
    constexpr int kBlocks = PcLegacySectionCache::kSectionBlockCount;
    constexpr int kNibbleBytes = kBlocks / 2;

    enum class JobState { Idle, Queued, Running, Done };

    struct Job
    {
        // Input, written by the game thread only while the slot is Idle.
        int_t x0 = 0;
        int_t y0 = 0;
        int_t z0 = 0;
        bool ambientOcclusion = false;
        // Natural Textures modes per tile (copied at submit), or off.
        bool natural = false;
        std::uint8_t naturalModes[256] = {};
        int workCount = 0;
        std::array<std::uint16_t, kBlocks> work{};
        std::array<std::uint16_t, kCells> ids{};
        std::array<std::uint8_t, kCells> faceMask{};
        std::array<std::uint8_t, kNibbleBytes> metadata{};
        std::array<std::uint8_t, kNibbleBytes> skyLight{};
        std::array<std::uint8_t, kNibbleBytes> blockLight{};
        // sky << 4 | block per cell. The game thread fills the six face slabs
        // around the section; the worker decodes the central 16^3 into it.
        std::array<std::uint8_t, kCells> light{};
        // Central cells whose light is not their own (slabs, stairs, farmland:
        // Block::useNeighborBrightness), computed by the game thread.
        int overrideCount = 0;
        std::array<std::uint16_t, kBlocks> overrideCell{};
        std::array<std::uint8_t, kBlocks> overrideLight{};
        // Below y=0 / above y=255 the world clamps to the edge layer.
        bool mirrorBottom = false;
        bool mirrorTop = false;

        // Output, written by the worker only.
        RenderCapturedMesh mesh;
        bool failed = false;
        std::uint32_t workUs = 0;
    };

    Job g_job;
    std::mutex g_mutex;
    std::condition_variable g_wake;
    JobState g_state = JobState::Idle;
    std::uint32_t g_ticket = 0;
    std::uint32_t g_lastTicket = 0;
    bool g_cancelled = false;

    int cellOf(int x, int y, int z)
    {
        return ((y + 1) * kSize + (z + 1)) * kSize + (x + 1);
    }

    int nibbleAt(const std::array<std::uint8_t, kNibbleBytes> &data, int index)
    {
        const int packed = data[static_cast<std::size_t>(index >> 1)];
        return (index & 1) == 0 ? (packed & 0xf) : (packed >> 4);
    }

    std::uint8_t packLight(int_t packedBrightness)
    {
        return static_cast<std::uint8_t>(((packedBrightness >> 16) & 0xf0) | ((packedBrightness >> 4) & 0x0f));
    }

    bool runJob(Job &job)
    {
        job.mesh.clear();
        for (int index = 0; index < kBlocks; ++index)
        {
            job.light[static_cast<std::size_t>(cellOf(index & 15, index >> 8, (index >> 4) & 15))] =
                static_cast<std::uint8_t>((nibbleAt(job.skyLight, index) << 4) | nibbleAt(job.blockLight, index));
        }
        for (int i = 0; i < job.overrideCount; ++i)
            job.light[job.overrideCell[static_cast<std::size_t>(i)]] = job.overrideLight[static_cast<std::size_t>(i)];
        for (int z = 0; z < 16; ++z)
        {
            for (int x = 0; x < 16; ++x)
            {
                if (job.mirrorBottom)
                    job.light[static_cast<std::size_t>(cellOf(x, -1, z))] = job.light[static_cast<std::size_t>(cellOf(x, 0, z))];
                if (job.mirrorTop)
                    job.light[static_cast<std::size_t>(cellOf(x, 16, z))] = job.light[static_cast<std::size_t>(cellOf(x, 15, z))];
            }
        }

        static constexpr int kNeighbour[6] = {-kSize * kSize, kSize * kSize, -kSize, kSize, -1, 1};
        // Same walk and per-face values as the opaque pass of
        // WorldRenderer::pcLegacyBuildRendererStep -> renderSimpleOpaqueCubeLegacy.
        for (int i = 0; i < job.workCount; ++i)
        {
            const int index = job.work[static_cast<std::size_t>(i)];
            const int x = index & 15;
            const int z = (index >> 4) & 15;
            const int y = (index >> 8) & 15;
            const int cell = cellOf(x, y, z);
            const int_t id = job.ids[static_cast<std::size_t>(cell)];
            if (id <= 0 || id >= Block::BLOCK_REGISTRY_SIZE)
                continue;
            const PcLegacyBlockRenderInfo &info = pcLegacyGetBlockRenderInfo(id);
            if (info.renderPass != 0 || !info.simpleOpaqueCube)
                continue;
            const unsigned mask = job.faceMask[static_cast<std::size_t>(cell)];
            if (mask == 0)
                continue;
            const int_t metadata = nibbleAt(job.metadata, index);
            if (!pcLegacyMeshWorkerOwnsBlock(info, id, metadata, job.ambientOcclusion))
                continue;

            const int_t minimumBlockLight = Block::lightValue[id];
            for (int side = 0; side < 6; ++side)
            {
                if ((mask & (1u << side)) == 0)
                    continue;
                const int light = job.light[static_cast<std::size_t>(cell + kNeighbour[side])];
                int_t blockLight = light & 15;
                if (blockLight < minimumBlockLight)
                    blockLight = minimumBlockLight;
                const int_t packedBrightness = (static_cast<int_t>(light >> 4) << 20) | (blockLight << 4);
                const int_t texture = info.textureByMetadata[static_cast<std::size_t>(metadata)][static_cast<std::size_t>(side)];
                const int naturalVariant = job.natural && texture >= 0 && texture < 256
                    ? pcLegacyNaturalVariant(job.naturalModes[texture], job.x0 + x, job.y0 + y, job.z0 + z, side)
                    : -1;
                if (!pcLegacyEmitCompactUnitCubeFace(job.mesh, job.x0, job.y0, job.z0, side,
                        job.x0 + x, job.y0 + y, job.z0 + z, texture,
                        pcLegacyGetVanillaFacePackedColor(side), packedBrightness, naturalVariant))
                    return false;
            }
        }
        return true;
    }

    void workerMain()
    {
#ifdef VITA_PLATFORM
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
            SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
#endif
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_wake.wait(lock, [] { return g_state == JobState::Queued; });
                g_state = JobState::Running;
            }
            const auto start = std::chrono::steady_clock::now();
            const bool ok = runJob(g_job);
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();

            std::lock_guard<std::mutex> lock(g_mutex);
            g_job.failed = !ok;
            g_job.workUs = static_cast<std::uint32_t>(elapsed);
            g_state = g_cancelled ? JobState::Idle : JobState::Done;
            g_cancelled = false;
        }
    }

    bool ensureStarted()
    {
        static bool attempted = false;
        static bool running = false;
        if (attempted)
            return running;
        attempted = true;

        // Build the lazily initialised tables the worker reads before the
        // thread exists, so it never races their first use.
        (void)pcLegacyGetBlockRenderInfo(0);
        RenderCapturedMesh prime;
        (void)pcLegacyEmitCompactUnitCubeFace(prime, 0, 0, 0, 0, 0, 0, 0, 0, pcLegacyGetVanillaFacePackedColor(0), 0);

        try
        {
            std::thread(workerMain).detach();
            running = true;
        }
        catch (...)
        {
            running = false;
        }
        return running;
    }

    // One 16x16 face slab of light around the section, with the exact values
    // ChunkCache::getLightBrightnessForSkyBlocks would return.
    void fillShellFace(Job &job, ChunkCache &source, int side)
    {
        const int axis = side >> 1;
        const int fixed = (side & 1) != 0 ? kSize - 1 : 0;
        auto cacheCell = [&](int u, int v, int &cx, int &cy, int &cz)
        {
            if (axis == 0) { cx = u; cz = v; cy = fixed; }
            else if (axis == 1) { cx = u; cy = v; cz = fixed; }
            else { cz = u; cy = v; cx = fixed; }
        };

        int cx = 0;
        int cy = 0;
        int cz = 0;
        cacheCell(1, 1, cx, cy, cz);
        const int_t firstY = job.y0 - 1 + cy;
        if (firstY < WorldHeight::MIN_Y)
        {
            job.mirrorBottom = true;
            return;
        }
        if (firstY >= WorldHeight::HEIGHT)
        {
            job.mirrorTop = true;
            return;
        }
        const int_t firstX = job.x0 - 1 + cx;
        const int_t firstZ = job.z0 - 1 + cz;
        const bool resident = source.hasResidentChunkAtBlock(firstX, firstZ);
        const ExtendedBlockStorage *storage = resident ? source.getResidentBlockStorageAt(firstX, firstY, firstZ) : nullptr;

        for (int v = 1; v <= 16; ++v)
        {
            for (int u = 1; u <= 16; ++u)
            {
                cacheCell(u, v, cx, cy, cz);
                const int_t x = job.x0 - 1 + cx;
                const int_t y = job.y0 - 1 + cy;
                const int_t z = job.z0 - 1 + cz;
                std::uint8_t light;
                if (!resident)
                {
                    light = packLight(source.getLightBrightnessForSkyBlocks(x, y, z, 0));
                }
                else if (storage == nullptr)
                {
                    light = 0xf0; // missing section: default sky 15, block 0
                }
                else
                {
                    const int_t id = storage->getExtBlockID(x & 15, y & 15, z & 15);
                    light = Block::useNeighborBrightness[id]
                        ? packLight(source.getLightBrightnessForSkyBlocks(x, y, z, 0))
                        : static_cast<std::uint8_t>((storage->getExtSkylightValue(x & 15, y & 15, z & 15) << 4) |
                            storage->getExtBlocklightValue(x & 15, y & 15, z & 15));
                }
                job.light[static_cast<std::size_t>((cy * kSize + cz) * kSize + cx)] = light;
            }
        }
    }
}

std::uint32_t pcLegacyMeshWorkerSubmit(PcLegacySectionCache &cache, ChunkCache &source,
    int_t x0, int_t y0, int_t z0, bool ambientOcclusion, const std::uint8_t *naturalModes)
{
    if (cache.getRenderWorkCount(0) <= 0 || x0 < -29999000 || x0 > 29999000 || z0 < -29999000 || z0 > 29999000)
        return 0;
    const ExtendedBlockStorage *storage = source.getResidentBlockStorageAt(x0, y0, z0);
    if (storage == nullptr)
        return 0;
    const std::vector<byte_t> &metadata = storage->func_48697_j().data;
    const std::vector<byte_t> &skyLight = storage->getSkylightArray().data;
    const std::vector<byte_t> &blockLight = storage->getBlocklightArray().data;
    if (metadata.size() != kNibbleBytes || skyLight.size() != kNibbleBytes || blockLight.size() != kNibbleBytes)
        return 0;

    // No block the worker would take (with smooth lighting on that is nearly
    // every section: it only takes light-emitting cubes then): the job would
    // copy ~28 KB, read the light shell and hand back an empty mesh a frame
    // later. Mesh it all here, as when the worker is busy.
    {
        const int workCount = cache.getRenderWorkCount(0);
        const std::uint16_t *work = cache.renderWorkEntries(0);
        const std::uint16_t *ids = cache.blockIdCells();
        const std::uint8_t *faceMasks = cache.exposedFaceMaskCells();
        bool anyOwned = false;
        for (int i = 0; i < workCount && !anyOwned; ++i)
        {
            const int index = work[i];
            const int cell = cellOf(index & 15, index >> 8, (index >> 4) & 15);
            const int_t id = ids[cell];
            if (id <= 0 || id >= Block::BLOCK_REGISTRY_SIZE || faceMasks[cell] == 0)
                continue;
            const PcLegacyBlockRenderInfo &info = pcLegacyGetBlockRenderInfo(id);
            const int packed = metadata[static_cast<std::size_t>(index >> 1)];
            const int_t blockMetadata = (index & 1) == 0 ? (packed & 0xf) : ((packed >> 4) & 0xf);
            anyOwned = info.renderPass == 0 && pcLegacyMeshWorkerOwnsBlock(info, id, blockMetadata, ambientOcclusion);
        }
        if (!anyOwned)
            return 0;
    }

    if (!ensureStarted())
        return 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state != JobState::Idle)
            return 0;
    }

    Job &job = g_job;
    job.x0 = x0;
    job.y0 = y0;
    job.z0 = z0;
    job.ambientOcclusion = ambientOcclusion;
    job.natural = naturalModes != nullptr;
    if (naturalModes != nullptr)
        std::memcpy(job.naturalModes, naturalModes, sizeof(job.naturalModes));
    std::memcpy(job.metadata.data(), metadata.data(), kNibbleBytes);
    std::memcpy(job.skyLight.data(), skyLight.data(), kNibbleBytes);
    std::memcpy(job.blockLight.data(), blockLight.data(), kNibbleBytes);
    std::memcpy(job.ids.data(), cache.blockIdCells(), sizeof(job.ids));
    std::memcpy(job.faceMask.data(), cache.exposedFaceMaskCells(), sizeof(job.faceMask));
    job.workCount = cache.getRenderWorkCount(0);
    std::memcpy(job.work.data(), cache.renderWorkEntries(0), static_cast<std::size_t>(job.workCount) * sizeof(std::uint16_t));

    job.overrideCount = 0;
    for (int index = 0; index < kBlocks; ++index)
    {
        const int x = index & 15;
        const int z = (index >> 4) & 15;
        const int y = index >> 8;
        const int cell = cellOf(x, y, z);
        if (!Block::useNeighborBrightness[job.ids[static_cast<std::size_t>(cell)]])
            continue;
        job.overrideCell[static_cast<std::size_t>(job.overrideCount)] = static_cast<std::uint16_t>(cell);
        job.overrideLight[static_cast<std::size_t>(job.overrideCount)] =
            packLight(source.getLightBrightnessForSkyBlocks(x0 + x, y0 + y, z0 + z, 0));
        ++job.overrideCount;
    }
    job.mirrorBottom = false;
    job.mirrorTop = false;
    for (int side = 0; side < 6; ++side)
        fillShellFace(job, source, side);

    std::uint32_t ticket;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (++g_lastTicket == 0)
            ++g_lastTicket;
        ticket = g_lastTicket;
        g_ticket = ticket;
        g_cancelled = false;
        g_state = JobState::Queued;
    }
    g_wake.notify_one();
    return ticket;
}

int_t pcLegacyMeshWorkerMetadata(int_t localIndex)
{
    return nibbleAt(g_job.metadata, static_cast<int>(localIndex & (kBlocks - 1)));
}

PcLegacyMeshJobState pcLegacyMeshWorkerCollect(std::uint32_t ticket, RenderCapturedMesh &out, std::uint32_t &workUs)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ticket == 0 || ticket != g_ticket || g_state == JobState::Idle)
        return PcLegacyMeshJobState::Lost;
    if (g_state != JobState::Done)
        return PcLegacyMeshJobState::Pending;
    g_state = JobState::Idle;
    if (g_job.failed)
        return PcLegacyMeshJobState::Lost;
    std::swap(out, g_job.mesh);
    workUs = g_job.workUs;
    return PcLegacyMeshJobState::Done;
}

void pcLegacyMeshWorkerCancel(std::uint32_t ticket)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ticket == 0 || ticket != g_ticket)
        return;
    if (g_state == JobState::Queued || g_state == JobState::Done)
        g_state = JobState::Idle;
    else if (g_state == JobState::Running)
        g_cancelled = true;
}

#endif
