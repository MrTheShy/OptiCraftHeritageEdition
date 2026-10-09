#include "platform/PlatformConfig.h"

#if PLATFORM_PC_LEGACY

#include "net/minecraft/src/WorldRenderer.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "java/Arithmetic.h"
#include "net/minecraft/src/Block.h"
#include "net/minecraft/src/Chunk.h"
#include "net/minecraft/src/ConnectedTextures.h"
#include "net/minecraft/src/Config.h"
#include "net/minecraft/src/ExtendedBlockStorage.h"
#include "net/minecraft/src/RenderBlocks.h"
#include "net/minecraft/src/Tessellator.h"
#include "net/minecraft/src/TileEntity.h"
#include "net/minecraft/src/TileEntityRenderer.h"
#include "pc/render/PcLegacyMeshScheduler.h"
#include "pc/tuning/PcLegacyTuning.h"
#include "net/minecraft/src/OpenGlHelper.h"
#include "net/minecraft/src/World.h"
#include "pc/render/PcLegacyTerrainStaging.h"
#include "pc/render/PcLegacyBlockRenderInfo.h"
#include "pc/render/PcLegacyStaticTileEntityMesh.h"
#include "pc/render/PcLegacyStaticTileEntityPolicy.h"
#include "platform/PlatformCompat.h"
#include "platform/PlatformTuning.h"
#include "pc/tuning/PcLegacyTuning.h"
#include "platform/RenderAPI.h"
#include "platform/Profiler.h"
#if PLATFORM_MESH_WORKER
#include "client/Minecraft.h"
#include "net/minecraft/src/CustomColorizer.h"
#include "pc/render/PcLegacyCubeMaterialInfo.h"
#include "pc/render/PcLegacyMeshWorker.h"
#endif
#if PLATFORM_MESH_WORKER_VERIFY
#include <cstring>
#include "platform/Log.h"
#endif

namespace
{
    unsigned int pcLegacySourceAvailability(World *world, int_t x0, int_t z0, int_t x1, int_t z1)
    {
        unsigned int availability = 0u;
        unsigned int bit = 1u;
        const int_t ccx0 = JavaArithmetic::intShr(x0 - 1, 4);
        const int_t ccx1 = JavaArithmetic::intShr(x1 + 1, 4);
        const int_t ccz0 = JavaArithmetic::intShr(z0 - 1, 4);
        const int_t ccz1 = JavaArithmetic::intShr(z1 + 1, 4);
        for (int_t ccx = ccx0; ccx <= ccx1; ++ccx)
        {
            for (int_t ccz = ccz0; ccz <= ccz1; ++ccz, bit <<= 1)
            {
                if (world->chunkExists(ccx, ccz))
                    availability |= bit;
            }
        }
        return availability;
    }

    bool pcLegacyStepTimeExpired(std::uint64_t stepStartUs)
    {
        if (PLATFORM_CHUNK_BUILD_STEP_US <= 0)
            return false;
        // A step never outlasts this frame's adaptive mesh budget either
        // (a frame with a world tick gets little; see PcLegacyMeshScheduler).
        std::uint64_t limitUs = static_cast<std::uint64_t>(PLATFORM_CHUNK_BUILD_STEP_US);
        if (g_pcLegacyMeshBudgetUs > 0)
            limitUs = std::min<std::uint64_t>(limitUs,
                static_cast<std::uint64_t>(std::max(g_pcLegacyMeshBudgetUs, PC_LEGACY_MESH_BUDGET_MIN_US)));
        const std::uint64_t nowUs = PlatformCompat::getMonotonicMicros();
        return nowUs > stepStartUs && nowUs - stepStartUs >= limitUs;
    }

    bool pcLegacyCentralSectionIsEmpty(World *world, int_t x, int_t y, int_t z)
    {
        if (world == nullptr || y < 0)
            return false;

        const int_t chunkX = JavaArithmetic::intShr(x, 4);
        const int_t chunkZ = JavaArithmetic::intShr(z, 4);
        if (!world->chunkExists(chunkX, chunkZ))
            return false;

        Chunk *chunk = world->getChunkFromChunkCoords(chunkX, chunkZ);
        if (chunk == nullptr || chunk->isEmptyChunk())
            return false;

        const ExtendedBlockStorage *storage = chunk->getBlockStorage(JavaArithmetic::intShr(y, 4));
        return storage == nullptr || storage->getIsEmpty();
    }

#if PLATFORM_MESH_WORKER_VERIFY
    // The worker's part of the section as the game thread meshes it, taken at
    // the same moment as the job's snapshot.
    RenderCapturedMesh g_meshVerifyReference;
    bool g_meshVerifyLeaked = false;

    void pcLegacyMeshVerifyBuildReference(PcLegacySectionCache &cache, int_t x0, int_t y0, int_t z0, bool ao)
    {
        g_meshVerifyReference.clear();
        Tessellator &tessellator = Tessellator::instance;
        RenderBlocks renderblocks(&cache);
        renderblocks.setPcLegacyCompactTerrainMesh(&g_meshVerifyReference, x0, y0, z0);
        tessellator.startDrawingQuads();
        tessellator.setTranslationD(-static_cast<double>(x0), -static_cast<double>(y0), -static_cast<double>(z0));
        const int_t workCount = cache.getRenderWorkCount(0);
        for (int_t i = 0; i < workCount; ++i)
        {
            const int_t cursor = cache.getRenderWorkEntry(0, i);
            const int_t lx = cursor & 15;
            const int_t lz = (cursor >> 4) & 15;
            const int_t ly = (cursor >> 8) & 15;
            const int_t id = cache.getBlockIdLocal(lx, ly, lz);
            if (id <= 0 || id >= Block::BLOCK_REGISTRY_SIZE || Block::blocksList[id] == nullptr)
                continue;
            const PcLegacyBlockRenderInfo &info = pcLegacyGetBlockRenderInfo(id);
            const std::uint8_t faceMask = cache.getExposedFaceMaskLocal(lx, ly, lz);
            if (info.renderPass != 0 || faceMask == 0 ||
                !pcLegacyMeshWorkerOwnsBlock(info, id, cache.getBlockMetadata(x0 + lx, y0 + ly, z0 + lz), ao))
                continue;
            renderblocks.renderSimpleOpaqueCubeLegacy(Block::blocksList[id], x0 + lx, y0 + ly, z0 + lz, faceMask);
        }
        // Anything in the tessellator means a block the worker owns does not
        // take the compact path on the game thread.
        g_meshVerifyLeaked = tessellator.hasPendingVertices();
        tessellator.cancelDrawing();
        tessellator.setTranslationD(0.0, 0.0, 0.0);
    }

    // True when the worker mesh matches; otherwise logs and puts the game
    // thread's mesh in its place.
    bool pcLegacyMeshVerifyCompare(RenderCapturedMesh &workerMesh, int_t x0, int_t y0, int_t z0)
    {
        const RenderCapturedMesh &ref = g_meshVerifyReference;
        const bool same = !g_meshVerifyLeaked && workerMesh.vertexCount == ref.vertexCount &&
            workerMesh.raw.size() == ref.raw.size() && workerMesh.stride == ref.stride &&
            workerMesh.positionShort == ref.positionShort && workerMesh.hasColor == ref.hasColor &&
            workerMesh.hasBrightness == ref.hasBrightness && workerMesh.hasTexture == ref.hasTexture &&
            (ref.raw.empty() || std::memcmp(workerMesh.raw.data(), ref.raw.data(),
                ref.raw.size() * sizeof(std::int32_t)) == 0);
        if (same)
            return true;
        std::size_t firstDiff = 0;
        while (firstDiff < workerMesh.raw.size() && firstDiff < ref.raw.size() &&
               workerMesh.raw[firstDiff] == ref.raw[firstDiff])
            ++firstDiff;
        MC_LOG_ERROR("mesh", "worker mesh differs at section %d,%d,%d: verts %d/%d ints %d/%d first diff int %d leaked=%d\n",
            (int)x0, (int)y0, (int)z0, workerMesh.vertexCount, ref.vertexCount,
            (int)workerMesh.raw.size(), (int)ref.raw.size(), (int)firstDiff, g_meshVerifyLeaked ? 1 : 0);
        workerMesh = ref;
        return false;
    }
#endif
}

void WorldRenderer::updateRenderer()
{
    pcLegacyStepDidWork = false;
    if (!needsUpdate)
    {
        if (pcLegacyBuildActive)
            pcLegacyResetBuildState();
        return;
    }

    pcLegacyBuildRendererStep(PLATFORM_CHUNK_BUILD_BLOCKS_PER_STEP);
}

void WorldRenderer::pcLegacyResetBuildState()
{
#if PLATFORM_MESH_WORKER
    pcLegacyMeshWorkerCancel(pcLegacyMeshJob);
#endif
    pcLegacyMeshJob = 0;
    pcLegacyMeshReplayIndex = -1;
    pcLegacyTerrainStagingRelease(this);
    pcLegacyBuildActive = false;
    pcLegacyBuildSourceAvailability = 0u;
    pcLegacyBuildSourceAvailabilityValid = false;
    pcLegacyBuildPass = 0;
    pcLegacyBuildCursor = 0;
    pcLegacyBuildHasPass1 = false;
    pcLegacyBuildChunkLit = false;
    pcLegacyStepDidWork = false;
    pcLegacyBuildDirtyDuringBuild = false;
}

bool WorldRenderer::pcLegacyBeginBuildState()
{
    pcLegacyResetBuildState();
    if (worldObj == nullptr)
        return false;

    const int_t x1 = posX + sizeWidth;
    const int_t y1 = posY + sizeHeight;
    const int_t z1 = posZ + sizeDepth;
    {
        PlatformTickPhaseScope acquirePhase("meshAcquire");
        if (!pcLegacyTerrainStagingAcquire(this, worldObj, posX, posY, posZ, x1, y1, z1))
            return false;
    }

    pcLegacyBuildActive = true;
    isVisibleFromPosition = false;
    // Also the modes this thread's compact faces use for the whole build.
    const std::uint8_t *naturalModes = RenderBlocks::pcLegacyPrepareNaturalTextures();
    (void)naturalModes;

#if PLATFORM_MESH_WORKER
    const bool skipWorker = pcLegacyMeshWorkerSkip;
    pcLegacyMeshWorkerSkip = false;
    PcLegacyTerrainStaging *staging = pcLegacyTerrainStagingGet(this);
    // The worker only produces compact-emitter faces; with connected/natural
    // textures or block palettes none are, so the section stays here.
    if (!skipWorker && PLATFORM_COMPACT_TERRAIN_VERTICES && staging != nullptr &&
        staging->sectionCache.has_value() && staging->sourceCache.has_value() &&
        !ConnectedTextures::isActive() && (!Config::isNaturalTextures() || naturalModes != nullptr) &&
        !CustomColorizer::anyBlockPalette())
    {
        PlatformTickPhaseScope phase("meshSnap");
        pcLegacyMeshJobAo = Minecraft::isAmbientOcclusionEnabled();
        pcLegacyMeshJob = pcLegacyMeshWorkerSubmit(*staging->sectionCache, *staging->sourceCache,
            posX, posY, posZ, pcLegacyMeshJobAo, naturalModes);
#if PLATFORM_PROFILE_RENDER_PHASES
        platformProfileTickPhase(pcLegacyMeshJob != 0 ? "#meshJobs" : "#meshNoJob", 1);
#endif
#if PLATFORM_MESH_WORKER_VERIFY
        if (pcLegacyMeshJob != 0)
            pcLegacyMeshVerifyBuildReference(*staging->sectionCache, posX, posY, posZ, pcLegacyMeshJobAo);
#endif
    }
#endif
    return true;
}

bool WorldRenderer::isTerrainBuildInProgress() const
{
    return pcLegacyBuildActive;
}

bool WorldRenderer::lastTerrainBuildStepDidWork() const
{
    return pcLegacyStepDidWork;
}

std::uint8_t WorldRenderer::pcLegacyVisibleFacesFrom(int_t face) const
{
    if (!isInitialized || face < 0 || face >= 6)
        return 0x3f;
    return pcLegacyPublishedVisibility[face];
}

void WorldRenderer::pcLegacyPublishBuild(PcLegacyTerrainStaging &staging)
{
    std::vector<TileEntity *> rebuiltStaticTileEntityRenderers;

#if PC_LEGACY_STATIC_TILE_ENTITY_MESH
    // ModelRenderer itself owns per-part display lists on desktop. Compile those
    // before opening the chunk display list; OpenGL does not allow glNewList to
    // be nested inside another glNewList.
    if (worldObj != nullptr)
    {
        for (const PcLegacyStaticTileEntityCandidate &candidate : staging.staticTileEntityCandidates)
        {
            TileEntity *te = candidate.tileEntity;
            if (te == nullptr || te->isInvalid())
                continue;
            TileEntity *current = worldObj->getBlockTileEntity(candidate.x, candidate.y, candidate.z);
            if (!pcLegacyStaticTileEntityCandidateIsCurrent(candidate, current))
                continue;
            (void)TileEntityRenderer::instance.prepareStaticPart(te);
        }
    }
#endif

    for (int_t pass = 0; pass < 2; ++pass)
    {
#if PLATFORM_TERRAIN_VBO_BATCH
        // Terrain goes to one VBO drawn by the batched RenderList loop. The
        // display list is kept only for what the batch cannot hold; a mesh the
        // format rejects (coordinates out of range, unexpected layout) falls
        // back to the old list draw for this section.
        VitaTerrainPublish batchResult = VitaTerrainPublish::Unsupported;
        if (Config::isFastTerrain())
        {
            batchResult = vitaTerrainPublish(
                vitaTerrain[pass], staging.compactPassMeshes[pass], staging.passMeshes[pass], pass, Config::isTerrainShader());
        }
        else
        {
            vitaTerrainRelease(vitaTerrain[pass]);
        }
        const bool batched = batchResult != VitaTerrainPublish::Unsupported;
        bool listNeeded = true;
        if (batched)
        {
            listNeeded = pass == 0 && !staging.staticTileEntityCandidates.empty();
            for (const TessellatorTextureMesh &group : staging.extraTextureMeshes[pass])
                listNeeded |= !group.mesh.empty();
        }
        if (!listNeeded)
        {
            // Drop whatever an earlier build recorded (and its VBOs).
            renderDeleteDisplayLists(glRenderList + pass, 1);
            vitaTerrainList[pass] = false;
            _skipRenderPass[pass] = batchResult == VitaTerrainPublish::Empty;
            continue;
        }
#else
        constexpr bool batched = false;
#endif
        renderBeginDisplayList(glRenderList + pass);

        // Terrain keeps the tiny vanilla/OptiFine seam scale. Static TileEntity
        // geometry is emitted later under a separate matrix so it never inherits
        // this scale (otherwise sign text/chest lids can misalign).
        renderPushMatrix();
        renderTranslate(static_cast<float>(posXClip), static_cast<float>(posYClip), static_cast<float>(posZClip));
        const float terrainScale = pcLegacyTerrainListScale();
        renderTranslate(-static_cast<float>(sizeDepth) / 2.0f,
            -static_cast<float>(sizeHeight) / 2.0f,
            -static_cast<float>(sizeDepth) / 2.0f);
        renderScale(terrainScale, terrainScale, terrainScale);
        renderTranslate(static_cast<float>(sizeDepth) / 2.0f,
            static_cast<float>(sizeHeight) / 2.0f,
            static_cast<float>(sizeDepth) / 2.0f);

        const bool compactDrawn = !batched && renderDrawCaptured(staging.compactPassMeshes[pass]);
        const bool baseDrawn = !batched && renderDrawCaptured(staging.passMeshes[pass]);
        bool extraDrawn = false;
        for (const TessellatorTextureMesh &group : staging.extraTextureMeshes[pass])
        {
            if (group.mesh.empty())
                continue;
            renderBindTexture(group.textureId);
            extraDrawn |= renderDrawCaptured(group.mesh);
        }
        if (extraDrawn)
            renderBindTexture(ConnectedTextures::getTerrainTextureId());
        renderPopMatrix();

        bool staticTileDrawn = false;
#if PC_LEGACY_STATIC_TILE_ENTITY_MESH
        if (pass == 0 && worldObj != nullptr && !staging.staticTileEntityCandidates.empty())
        {
            renderPushMatrix();
            renderTranslate(static_cast<float>(posXClip), static_cast<float>(posYClip), static_cast<float>(posZClip));
            const float staticScale = pcLegacyStaticTileEntityListScale();
            renderScale(staticScale, staticScale, staticScale);

            for (const PcLegacyStaticTileEntityCandidate &candidate : staging.staticTileEntityCandidates)
            {
                TileEntity *te = candidate.tileEntity;
                if (te == nullptr || te->isInvalid())
                    continue;

                // Meshing is incremental. Revalidate ownership immediately before
                // dereferencing/rendering a candidate so a chest/sign removed while
                // this section was being built can never leave a stale pointer here.
                TileEntity *current = worldObj->getBlockTileEntity(candidate.x, candidate.y, candidate.z);
                if (!pcLegacyStaticTileEntityCandidateIsCurrent(candidate, current))
                    continue;

                const int_t brightness = worldObj->getLightBrightnessForSkyBlocks(
                    candidate.x, candidate.y, candidate.z, 0);
                OpenGlHelper::setLightmapTextureCoords(OpenGlHelper::lightmapTexUnit,
                    static_cast<float>(brightness % 65536), static_cast<float>(brightness / 65536));
                renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);

                const bool drew = TileEntityRenderer::instance.renderStaticPart(te,
                    static_cast<double>(candidate.x - posX),
                    static_cast<double>(candidate.y - posY),
                    static_cast<double>(candidate.z - posZ));
                if (!drew)
                    continue;

                staticTileDrawn = true;
                if (std::find(rebuiltStaticTileEntityRenderers.begin(), rebuiltStaticTileEntityRenderers.end(), te) ==
                    rebuiltStaticTileEntityRenderers.end())
                {
                    rebuiltStaticTileEntityRenderers.push_back(te);
                }
            }

            // A static chest/sign binds its own texture inside the display list.
            // Restore the terrain atlas so the next batched section starts from
            // the same texture state as before this optimization existed.
            renderBindTexture(ConnectedTextures::getTerrainTextureId());
            renderPopMatrix();
        }
#endif

        renderEndDisplayList();
#if PLATFORM_TERRAIN_VBO_BATCH
        vitaTerrainList[pass] = true;
        _skipRenderPass[pass] = !(compactDrawn || baseDrawn || extraDrawn || staticTileDrawn ||
            vitaTerrain[pass].buffer != 0);
#else
        _skipRenderPass[pass] = !(compactDrawn || baseDrawn || extraDrawn || staticTileDrawn);
#endif
    }

    const std::vector<TileEntity *> &rebuiltTileEntityRenderers = staging.tileEntityRenderers;
    for (TileEntity *te : rebuiltTileEntityRenderers)
    {
        if (std::find(tileEntityRenderers.begin(), tileEntityRenderers.end(), te) == tileEntityRenderers.end())
            pushUniqueTileEntityRef(tileEntities, te);
    }
    for (TileEntity *te : tileEntityRenderers)
    {
        if (std::find(rebuiltTileEntityRenderers.begin(), rebuiltTileEntityRenderers.end(), te) == rebuiltTileEntityRenderers.end())
            eraseAllTileEntityRefs(tileEntities, te);
    }

#if PC_LEGACY_STATIC_TILE_ENTITY_MESH
    for (TileEntity *te : pcLegacyStaticTileEntityRenderers)
        pcLegacyStaticTileEntityUnpublish(te, this);
    pcLegacyStaticTileEntityRenderers = rebuiltStaticTileEntityRenderers;
    for (TileEntity *te : pcLegacyStaticTileEntityRenderers)
        pcLegacyStaticTileEntityPublish(te, this);
#else
    pcLegacyStaticTileEntityRenderers.clear();
#endif

    tileEntityRenderers = rebuiltTileEntityRenderers;
    // No snapshot: the section had no block, which the visibility build
    // reports as every face seeing every other.
    for (int_t face = 0; face < 6; ++face)
    {
        pcLegacyPublishedVisibility[face] = staging.sectionCache.has_value()
            ? staging.sectionCache->getVisibleFacesFrom(face)
            : PcLegacySectionVisibility::kAllFaces;
    }
    isChunkLit = pcLegacyBuildChunkLit;
    isInitialized = true;
    const bool dirtyDuringBuild = pcLegacyBuildDirtyDuringBuild;
    needsUpdate = dirtyDuringBuild;
    pcLegacyEditOrder = 0;
    pcLegacyBuiltFarLeaves = pcLegacyBuildFarLeaves;
    ++chunksUpdated;

    pcLegacyResetBuildState();
    pcLegacyStepDidWork = true;
}

bool WorldRenderer::pcLegacyBuildRendererStep(int_t blockBudget)
{
    pcLegacyStepDidWork = false;
    const std::uint64_t stepStartUs = PlatformCompat::getMonotonicMicros();

    if (worldObj == nullptr)
    {
        pcLegacyResetBuildState();
        needsUpdate = false;
        return true;
    }

    const int_t x0 = posX;
    const int_t y0 = posY;
    const int_t z0 = posZ;
    const int_t x1 = posX + sizeWidth;
    const int_t y1 = posY + sizeHeight;
    const int_t z1 = posZ + sizeDepth;

    unsigned int sourceAvailability = pcLegacySourceAvailability(worldObj, x0, z0, x1, z1);
    if (pcLegacyBuildActive && pcLegacyBuildSourceAvailabilityValid &&
        pcLegacyBuildSourceAvailability != sourceAvailability)
    {
        pcLegacyResetBuildState();
        return false;
    }

    const bool centralSectionEmpty = !pcLegacyBuildActive &&
        pcLegacyCentralSectionIsEmpty(worldObj, x0, y0, z0);

    const int_t ccx0 = JavaArithmetic::intShr(x0 - 1, 4);
    const int_t ccx1 = JavaArithmetic::intShr(x1 + 1, 4);
    const int_t ccz0 = JavaArithmetic::intShr(z0 - 1, 4);
    const int_t ccz1 = JavaArithmetic::intShr(z1 + 1, 4);
    int_t requestedDependencies = 0;
    unsigned int sourceBit = 1u;
    for (int_t ccx = ccx0; ccx <= ccx1 && !centralSectionEmpty; ++ccx)
    {
        for (int_t ccz = ccz0; ccz <= ccz1; ++ccz, sourceBit <<= 1)
        {
            if ((sourceAvailability & sourceBit) != 0u)
                continue;

            // Queued requests cost nothing and do not count as work, so the
            // scheduler moves on to other renderers until the chunk arrives.
            ++requestedDependencies;
            if (worldObj->requestChunkForMesh(ccx, ccz))
            {
                pcLegacyStepDidWork = true;
                return false;
            }
        }
    }
    if (requestedDependencies > 0)
    {
#if PLATFORM_PROFILE_RENDER_PHASES
        platformProfileTickPhase("#meshDep", 1);
#endif
        return false;
    }

    if (centralSectionEmpty)
    {
        // Nothing to mesh (sky above the terrain is most sections of a new
        // column): the snapshot of 18^3 cells and its 94 KB of tables would
        // only confirm that. Same published state as a full build of it.
        pcLegacyResetBuildState();
        pcLegacyMeshWorkerSkip = false;
        if (!pcLegacyTerrainStagingAcquire(this, worldObj, x0, y0, z0, x1, y1, z1, false))
            return false;
        pcLegacyBuildActive = true;
        isVisibleFromPosition = false;
        PcLegacyTerrainStaging *emptyStaging = pcLegacyTerrainStagingGet(this);
#if PLATFORM_PROFILE_RENDER_PHASES
        platformProfileTickPhase("#meshEmpty", 1);
#endif
        pcLegacyPublishBuild(*emptyStaging);
        return true;
    }

    if (!pcLegacyBuildActive)
    {
        if (!pcLegacyBeginBuildState())
            return false;
        pcLegacyBuildSourceAvailability = sourceAvailability;
        pcLegacyBuildSourceAvailabilityValid = true;
        pcLegacyStepDidWork = true;
        if (pcLegacyStepTimeExpired(stepStartUs))
            return false;
    }

    PcLegacyTerrainStaging *staging = pcLegacyTerrainStagingGet(this);
    if (staging == nullptr || !staging->sectionCache.has_value() ||
        !staging->sourceCache.has_value() || !staging->sourceCache->refreshResidentChunks())
    {
        pcLegacyResetBuildState();
        return false;
    }

    PcLegacySectionCache &sectionCache = *staging->sectionCache;
    pcLegacyBuildHasPass1 = sectionCache.getRenderWorkCount(1) > 0;
    if (!sectionCache.hasRenderWork())
    {
        pcLegacyPublishBuild(*staging);
        return true;
    }

    const int_t totalBlocks = sizeWidth * sizeHeight * sizeDepth;
    if (blockBudget <= 0)
        blockBudget = totalBlocks;

    int_t processed = 0;
    while (pcLegacyBuildPass < 2)
    {
        if (pcLegacyBuildPass == 1 && !pcLegacyBuildHasPass1)
        {
            ++pcLegacyBuildPass;
            pcLegacyBuildCursor = 0;
            continue;
        }

        const int_t workCount = sectionCache.getRenderWorkCount(pcLegacyBuildPass);
        if (workCount <= 0)
        {
            ++pcLegacyBuildPass;
            pcLegacyBuildCursor = 0;
            continue;
        }

        sectionCache.beginStep();
        RenderBlocks renderblocks(&sectionCache);
        if (PLATFORM_COMPACT_TERRAIN_VERTICES)
            renderblocks.setPcLegacyCompactTerrainMesh(&staging->compactPassMeshes[pcLegacyBuildPass], posX, posY, posZ);
        Tessellator *tessellator = &Tessellator::instance;
        Chunk::isLit = false;
        tessellator->startDrawingQuads();
        tessellator->setTranslationD(-static_cast<double>(posX),
            -static_cast<double>(posY), -static_cast<double>(posZ));

        bool stepDrew = false;
        PlatformTickPhaseScope walkPhase(pcLegacyBuildPass == 0 ? "meshWalk0" : "meshWalk1");
#if PLATFORM_PROFILE_RENDER_PHASES
        // Walk time by block kind: render types 0-25, other (26), untaken
        // cubes white (27) and tinted (28). One block in 8 is timed and the
        // step reports once (x8): a profiler call per block cost more than
        // a cube.
        static const char *const kKindNames[] = {"wRT0", "wRT1", "wRT2", "wRT3", "wRT4", "wRT5", "wRT6",
            "wRT7", "wRT8", "wRT9", "wRT10", "wRT11", "wRT12", "wRT13", "wRT14", "wRT15", "wRT16",
            "wRT17", "wRT18", "wRT19", "wRT20", "wRT21", "wRT22", "wRT23", "wRT24", "wRT25", "wRTx",
            "wCubeWhite", "wCubeTint"};
        long long kindUs[29] = {};
#endif
#if PLATFORM_MESH_WORKER
        // A block left to the worker still sets the tessellator state its last
        // face would have set on this thread; replay it before anything that
        // could read it, so the game thread's mesh stays the same.
        auto replayWorkerBlockState = [&]()
        {
            if (pcLegacyMeshReplayIndex < 0)
                return;
            const int_t rx = pcLegacyMeshReplayIndex & 15;
            const int_t rz = (pcLegacyMeshReplayIndex >> 4) & 15;
            const int_t ry = (pcLegacyMeshReplayIndex >> 8) & 15;
            const std::uint8_t mask = sectionCache.getExposedFaceMaskLocal(rx, ry, rz);
            int_t side = 5;
            while (side > 0 && (mask & (1u << side)) == 0)
                --side;
            const int_t id = sectionCache.getBlockIdLocal(rx, ry, rz);
            pcLegacySetVanillaFaceState(*tessellator, side,
                sectionCache.getFacePackedBrightness(x0 + rx, y0 + ry, z0 + rz, side, Block::lightValue[id]));
            renderblocks.enableAO = false;
            pcLegacyMeshReplayIndex = -1;
        };
#endif
        while (pcLegacyBuildCursor < workCount && processed < blockBudget)
        {
            if (processed > 0 &&
                (processed % PLATFORM_CHUNK_BUILD_TIME_CHECK_BLOCKS) == 0 &&
                pcLegacyStepTimeExpired(stepStartUs))
            {
                break;
            }

            const int_t cursor = static_cast<int_t>(sectionCache.getRenderWorkEntry(
                pcLegacyBuildPass, pcLegacyBuildCursor++));
            const int_t lx = cursor & 15;
            const int_t lz = (cursor >> 4) & 15;
            const int_t ly = (cursor >> 8) & 15;
            const int_t x = x0 + lx;
            const int_t y = y0 + ly;
            const int_t z = z0 + lz;
            ++processed;

            const int_t id = sectionCache.getBlockIdLocal(lx, ly, lz);
            if (id <= 0 || id >= Block::BLOCK_REGISTRY_SIZE)
                continue;

            Block *block = Block::blocksList[id];
            if (block == nullptr)
                continue;

            if (pcLegacyBuildPass == 0 && Block::isBlockContainer[id])
            {
                TileEntity *te = sectionCache.getBlockTileEntity(x, y, z);
                if (te != nullptr && TileEntityRenderer::instance.hasSpecialRenderer(te))
                {
                    if (std::find(staging->tileEntityRenderers.begin(), staging->tileEntityRenderers.end(), te) ==
                        staging->tileEntityRenderers.end())
                    {
                        staging->tileEntityRenderers.push_back(te);
                    }
#if PC_LEGACY_STATIC_TILE_ENTITY_MESH
                    if (TileEntityRenderer::instance.canRenderStaticPart(te))
                    {
                        const bool alreadyQueued = std::any_of(staging->staticTileEntityCandidates.begin(),
                            staging->staticTileEntityCandidates.end(),
                            [te](const PcLegacyStaticTileEntityCandidate &candidate) {
                                return candidate.tileEntity == te;
                            });
                        if (!alreadyQueued)
                        {
                            PcLegacyStaticTileEntityCandidate candidate{te, x, y, z};
                            if (pcLegacyStaticTileEntityCandidateInsideSection(candidate, posX, posY, posZ))
                                staging->staticTileEntityCandidates.push_back(candidate);
                        }
                    }
#endif
                }
            }

            const PcLegacyBlockRenderInfo &blockInfo = pcLegacyGetBlockRenderInfo(id);
            if (blockInfo.renderPass != pcLegacyBuildPass)
                continue;

            if (blockInfo.simpleOpaqueCube)
            {
                const std::uint8_t faceMask = sectionCache.getExposedFaceMaskLocal(lx, ly, lz);
                if (faceMask == 0)
                    continue;
#if PLATFORM_MESH_WORKER
                if (pcLegacyMeshJob != 0 && pcLegacyBuildPass == 0 &&
                    pcLegacyMeshWorkerOwnsBlock(blockInfo, id, pcLegacyMeshWorkerMetadata(cursor), pcLegacyMeshJobAo))
                {
                    pcLegacyMeshReplayIndex = cursor;
                    continue;
                }
                replayWorkerBlockState();
#endif
#if PLATFORM_PROFILE_RENDER_PHASES
                const bool timed = (processed & 7) == 0;
                const std::uint64_t cubeStartUs = timed ? PlatformCompat::getMonotonicMicros() : 0;
#endif
                stepDrew |= renderblocks.renderSimpleOpaqueCubeLegacy(block, x, y, z, faceMask);
#if PLATFORM_PROFILE_RENDER_PHASES
                // Cubes the worker did not take, by colour: white (stone without
                // the worker) or tinted (grass, leaves).
                if (timed)
                    kindUs[blockInfo.defaultWhiteColorMultiplier ? 27 : 28] +=
                        (long long)(PlatformCompat::getMonotonicMicros() - cubeStartUs);
#endif
            }
            else
            {
#if PLATFORM_MESH_WORKER
                replayWorkerBlockState();
#endif
#if PLATFORM_PROFILE_RENDER_PHASES
                const bool timed = (processed & 7) == 0;
                const std::uint64_t otherStartUs = timed ? PlatformCompat::getMonotonicMicros() : 0;
#endif
                stepDrew |= renderblocks.renderBlockByRenderType(block, x, y, z);
#if PLATFORM_PROFILE_RENDER_PHASES
                if (timed)
                {
                    const int_t renderType = block->getRenderType();
                    kindUs[renderType >= 0 && renderType < 26 ? renderType : 26] +=
                        (long long)(PlatformCompat::getMonotonicMicros() - otherStartUs);
                }
#endif
            }
        }
#if PLATFORM_MESH_WORKER
        replayWorkerBlockState();
#endif
#if PLATFORM_PROFILE_RENDER_PHASES
        for (int kind = 0; kind < 29; ++kind)
            if (kindUs[kind] > 0)
                platformProfileTickPhase(kKindNames[kind], kindUs[kind] * 8000LL);
#endif

        PlatformTickPhaseScope capturePhase("meshCapture");
        const bool textureGroupsCaptureOk = tessellator->captureTextureGroups(
            staging->extraTextureMeshes[pcLegacyBuildPass], true);
        const bool hadBaseVertices = tessellator->hasPendingVertices();
        const bool baseCaptureOk = tessellator->capture(staging->passMeshes[pcLegacyBuildPass], true);
        tessellator->setTranslationD(0.0, 0.0, 0.0);
        if (!textureGroupsCaptureOk || (hadBaseVertices && !baseCaptureOk))
        {
            pcLegacyResetBuildState();
            return false;
        }

        pcLegacyBuildChunkLit = pcLegacyBuildChunkLit || Chunk::isLit;
        pcLegacyStepDidWork = pcLegacyStepDidWork || processed > 0 || stepDrew;

        if (pcLegacyBuildCursor < workCount)
            return false;

        ++pcLegacyBuildPass;
        pcLegacyBuildCursor = 0;

        if (processed >= blockBudget || pcLegacyStepTimeExpired(stepStartUs))
            return false;
    }

#if PLATFORM_MESH_WORKER
    if (pcLegacyMeshJob != 0)
    {
        PlatformTickPhaseScope phase("meshMerge");
        std::uint32_t workUs = 0;
        // The worker owns every compact face of pass 0: anything this thread
        // put there means the two disagreed about a block.
        const PcLegacyMeshJobState state = staging->compactPassMeshes[0].empty()
            ? pcLegacyMeshWorkerCollect(pcLegacyMeshJob, staging->compactPassMeshes[0], workUs)
            : PcLegacyMeshJobState::Lost;
        if (state == PcLegacyMeshJobState::Pending)
        {
#if PLATFORM_PROFILE_RENDER_PHASES
            platformProfileTickPhase("#meshWait", 1);
#endif
            return false;
        }
        if (state == PcLegacyMeshJobState::Lost)
        {
#if PLATFORM_PROFILE_RENDER_PHASES
            platformProfileTickPhase("#meshFallback", 1);
#endif
            pcLegacyMeshWorkerSkip = true;
            pcLegacyResetBuildState();
            return false;
        }
        pcLegacyMeshJob = 0;
#if PLATFORM_PROFILE_RENDER_PHASES
        platformProfileTickPhase("#meshWorkUs", static_cast<long long>(workUs));
#endif
#if PLATFORM_MESH_WORKER_VERIFY
        const bool verified = pcLegacyMeshVerifyCompare(staging->compactPassMeshes[0], posX, posY, posZ);
#if PLATFORM_PROFILE_RENDER_PHASES
        platformProfileTickPhase(verified ? "#meshVerifyOk" : "#meshVerifyBad", 1);
#endif
        (void)verified;
#endif
    }
#endif

    PlatformTickPhaseScope phase("meshPublish");
    pcLegacyPublishBuild(*staging);
    return true;
}

#endif
