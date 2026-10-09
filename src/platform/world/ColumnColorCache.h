#pragma once

#include <cstdint>

#include "java/Type.h"

// Biome tints of grass, tall grass, leaves and vines depend only on the world,
// the block column (x, z) and the Smooth Biomes option, but colorMultiplier()
// recomputed them for every block, each a 3x3 average of biome lookups (vines
// asked the biome generator itself, 9 times per block): the same value up to
// 8 times per column, once per section. A small direct-mapped cache per kind
// returns the value computed the first time. Game thread only (meshing,
// particles); the world is told apart by its WorldChunkManager.
class ColumnColorCache
{
public:
    template <class Compute>
    int_t get(const void *world, bool smooth, int_t x, int_t z, Compute compute)
    {
        Entry &entry = entries[(static_cast<unsigned>(x) & 63u) | ((static_cast<unsigned>(z) & 63u) << 6)];
        if (entry.used && entry.x == x && entry.z == z && entry.world == world && entry.smooth == smooth)
            return entry.color;
        const int_t color = compute();
        entry = Entry{x, z, world, color, smooth, true};
        return color;
    }

private:
    struct Entry
    {
        int_t x = 0;
        int_t z = 0;
        const void *world = nullptr;
        int_t color = 0;
        bool smooth = false;
        bool used = false;
    };
    Entry entries[4096];
};
