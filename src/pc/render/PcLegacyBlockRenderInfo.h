#pragma once

#include "platform/PlatformConfig.h"

#if PLATFORM_PC_LEGACY

#include <array>
#include <cstdint>
#include "java/Type.h"

struct PcLegacyBlockRenderInfo
{
    std::int8_t renderType = -1;
    std::uint8_t renderPass = 0;
    bool opaqueCube = false;
    bool normalCube = false;
    bool simpleOpaqueCube = false;
    bool staticTextureByMetadata = false;
    bool defaultWhiteColorMultiplier = false;
    // Bit per metadata whose six textures the compact unit-cube emitter takes
    // unchanged: atlas tiles 0..255, and no side tile that RenderBlocks swaps
    // or doubles for Better Grass / fancy grass (3, 68, 77).
    std::uint16_t compactTextureMetadataMask = 0;
    std::array<std::array<std::int16_t, 6>, 16> textureByMetadata{};
};

const PcLegacyBlockRenderInfo &pcLegacyGetBlockRenderInfo(int_t blockId);

#endif
