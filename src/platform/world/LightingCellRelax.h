#pragma once

#include "java/Type.h"
#include "net/minecraft/src/WorldHeight.h"

// One cell of the deferred light queue (PLATFORM_LIGHTING_SECTION_QUEUE):
// MetadataChunkBlock::updateLight for a single cell, then the neighbours to
// queue. Shared with the host-side check vita/lighting_queue_check.cpp.
//
// Every step loses at least 1, so the queue settles on vanilla's flood-fill
// values in any order, as long as every cell that may be wrong gets queued.
// The neighbour test is World.updateLightByType's: queue a neighbour if this
// cell got brighter than it plus its opacity, or got darker and may have been
// what lit it. (MetadataChunkBlock missed the second case for water and ice,
// which then stayed too bright.)
//
// View, world coordinates: light(), opacity(), emitted() (sky: 15 under open
// sky; block: Block::lightValue), set(), schedule(). Returns true on change.
template <class View>
inline bool relaxLightCell(View &view, int_t x, int_t y, int_t z)
{
    const int_t current = view.light(x, y, z);
    int_t opacity = view.opacity(x, y, z);
    if (opacity == 0)
        opacity = 1;
    int_t light = view.emitted(x, y, z);
    if (opacity < 15 || light > 0)
    {
        int_t best = view.light(x - 1, y, z);
        int_t n = view.light(x + 1, y, z);
        if (n > best) best = n;
        n = view.light(x, y - 1, z);
        if (n > best) best = n;
        n = view.light(x, y + 1, z);
        if (n > best) best = n;
        n = view.light(x, y, z - 1);
        if (n > best) best = n;
        n = view.light(x, y, z + 1);
        if (n > best) best = n;
        best -= opacity;
        if (best > light)
            light = best;
    }
    if (light == current)
        return false;

    view.set(x, y, z, light);

    // Same order as MetadataChunkBlock's propagation.
    static const int_t offsets[6][3] = {
        {-1, 0, 0}, {0, -1, 0}, {0, 0, -1},
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}
    };
    const bool darker = light < current;
    for (const auto &offset : offsets)
    {
        const int_t nx = x + offset[0];
        const int_t ny = y + offset[1];
        const int_t nz = z + offset[2];
        if (ny < 0 || ny >= WorldHeight::HEIGHT)
            continue;
        const int_t neighbour = view.light(nx, ny, nz);
        int_t neighbourOpacity = view.opacity(nx, ny, nz);
        if (neighbourOpacity == 0)
            neighbourOpacity = 1;
        if (light - neighbourOpacity > neighbour ||
            (darker && neighbour > 0 && current - neighbourOpacity == neighbour))
            view.schedule(nx, ny, nz);
    }
    return true;
}
