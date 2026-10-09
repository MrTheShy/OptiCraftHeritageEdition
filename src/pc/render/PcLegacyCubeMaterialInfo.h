#pragma once

#include "platform/PlatformConfig.h"

#if PLATFORM_PC_LEGACY

#include <cstdint>

#include "java/Type.h"

class Tessellator;
struct RenderCapturedMesh;

struct PcLegacyCubeUv
{
    float u0 = 0.0f;
    float u1 = 0.0f;
    float v0 = 0.0f;
    float v1 = 0.0f;
    int_t u0Bits = 0;
    int_t u1Bits = 0;
    int_t v0Bits = 0;
    int_t v1Bits = 0;
};

int_t pcLegacyGetVanillaFacePackedColor(int_t side);
void pcLegacySetVanillaFaceState(Tessellator &tessellator, int_t side, int_t packedBrightness);
// naturalVariant: -1, or rotation * 2 + flip of Natural Textures for this face
// (pcLegacyNaturalVariant); the UVs then come from pcLegacyNaturalCubeUv.
bool pcLegacyEmitCompactUnitCubeFace(RenderCapturedMesh &mesh, int_t originX, int_t originY, int_t originZ,
    int_t side, int_t x, int_t y, int_t z, int_t tile, int_t packedColor, int_t packedBrightness,
    int naturalVariant = -1);

// Natural Textures on compact cube faces. The UVs RenderBlocks gives a full
// cube face for each rotation and flip, captured from its own face functions
// (RenderBlocks::pcLegacyCaptureNaturalCubeUv) as offsets from the tile corner
// in atlas pixels, per face corner (x | y << 1 | z << 2).
struct PcLegacyNaturalCubeUv
{
    float offset[6][4][2][8][2] = {};
    bool ready = false;
};
PcLegacyNaturalCubeUv &pcLegacyNaturalCubeUv();
// Per terrain tile, for the threads that cannot read NaturalTextures: 0 none,
// else NaturalProperties::rotation (1, 2 or 4) | 8 when it can flip.
// Filled on the game thread (pcLegacyRefreshNaturalTileModes).
using PcLegacyNaturalTileModes = std::uint8_t[256];
void pcLegacyRefreshNaturalTileModes(PcLegacyNaturalTileModes &out);
// The face's variant as RenderBlocks::resolveFaceTexture picks it, or -1.
int pcLegacyNaturalVariant(std::uint8_t mode, int_t x, int_t y, int_t z, int_t side);
bool pcLegacyEmitUnitCubeFace(Tessellator &tessellator, int_t side, int_t x, int_t y, int_t z, int_t tile);

#endif
