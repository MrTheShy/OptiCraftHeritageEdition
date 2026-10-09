#pragma once

#include "platform/PlatformConfig.h"
#include "platform/PlatformTuning.h"

#if PLATFORM_PC_LEGACY && PLATFORM_MESH_WORKER

#include <cstdint>

#include "java/Type.h"
#include "net/minecraft/src/Block.h"
#include "pc/render/PcLegacyBlockRenderInfo.h"

class ChunkCache;
class PcLegacySectionCache;
struct RenderCapturedMesh;

// Opaque-pass faces of plain cubes (stone, dirt, ores...) meshed on cores 1-2
// while the game thread meshes the rest of the section; the worker's mesh
// replaces the compact pass-0 mesh. One job slot: the game thread fills the
// input (copies only, nothing pointing into the world) while it is idle and
// collects the output for the same ticket. A cancelled job runs to its end and
// is dropped, so cancelling never waits.

// Same test on both threads: every face of this block goes through the compact
// emitter, so the worker can produce them.
inline bool pcLegacyMeshWorkerOwnsBlock(const PcLegacyBlockRenderInfo &info, int_t blockId, int_t metadata,
    bool ambientOcclusion)
{
    return info.simpleOpaqueCube && info.staticTextureByMetadata && info.defaultWhiteColorMultiplier &&
        ((info.compactTextureMetadataMask >> (metadata & 15)) & 1u) != 0 &&
        (!ambientOcclusion || Block::lightValue[blockId] != 0);
}

enum class PcLegacyMeshJobState
{
    Pending,
    Done,
    // Cancelled, replaced or failed: mesh the section without the worker.
    Lost
};

// Returns the job ticket, or 0 when the worker is busy or the section cannot
// use it (the game thread then meshes everything, as before).
std::uint32_t pcLegacyMeshWorkerSubmit(PcLegacySectionCache &cache, ChunkCache &source,
    int_t x0, int_t y0, int_t z0, bool ambientOcclusion, const std::uint8_t *naturalModes);
// Metadata of a section block (y << 8 | z << 4 | x) as copied for the job in
// flight; the game thread uses it so both sides classify blocks the same way.
int_t pcLegacyMeshWorkerMetadata(int_t localIndex);
// On Done, swaps the worker's mesh into out and frees the slot.
PcLegacyMeshJobState pcLegacyMeshWorkerCollect(std::uint32_t ticket, RenderCapturedMesh &out,
    std::uint32_t &workUs);
void pcLegacyMeshWorkerCancel(std::uint32_t ticket);

#endif
