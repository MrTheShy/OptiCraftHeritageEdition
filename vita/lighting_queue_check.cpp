// Light queue check for PLATFORM_LIGHTING_SECTION_QUEUE: LightingSectionQueue
// and relaxLightCell (the code World::updatingLighting runs) drained over a
// synthetic world must settle on exactly the light an independent fixed-point
// solver computes, after block edits and after vegetation column regens queued
// the way Chunk does it. The old engine (MetadataChunkBlock's neighbour test,
// newest job first, no cap) runs on the same edits for comparison.
// From src/: clang++ -std=c++17 -O2 -I. ../vita/lighting_queue_check.cpp \
//   platform/world/LightingSectionQueue.cpp -o /tmp/lightq && /tmp/lightq
#include "platform/world/LightingSectionQueue.h"
#include "platform/world/LightingCellRelax.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
constexpr int SX = 48, SZ = 48, SY = 256; // 3x3 chunks
enum { AIR, STONE, LEAVES, WATER, GLASS, TORCH, GLOWSTONE, ICE, LAVA, WOOD, BLOCK_KINDS };
const int kOpacity[BLOCK_KINDS] = {0, 255, 1, 3, 0, 0, 255, 3, 255, 255};
const int kEmission[BLOCK_KINDS] = {0, 0, 0, 0, 0, 14, 15, 0, 15, 0};

struct World
{
    std::vector<unsigned char> block, light[2];
    std::vector<int> height;
    World() : block(SX * SY * SZ), height(SX * SZ)
    {
        light[0].resize(block.size());
        light[1].resize(block.size());
    }
    static int at(int x, int y, int z) { return (y * SZ + z) * SX + x; }
    static bool inside(int x, int z) { return x >= 0 && z >= 0 && x < SX && z < SZ; }
    // The outer ring of columns stands in for chunks that are not loaded: never
    // recomputed, never queued (World skips cells without loaded neighbours).
    static bool interior(int x, int z) { return x >= 1 && z >= 1 && x < SX - 1 && z < SZ - 1; }
    int id(int x, int y, int z) const
    {
        return inside(x, z) && y >= 0 && y < SY ? block[at(x, y, z)] : AIR;
    }
    void column(int x, int z)
    {
        int h = SY;
        while (h > 0 && kOpacity[id(x, h - 1, z)] == 0)
            --h;
        height[z * SX + x] = h;
    }
    int saved(int type, int x, int y, int z) const
    {
        y = y < 0 ? 0 : (y >= SY ? SY - 1 : y);
        if (!inside(x, z))
            return type == 0 ? 15 : 0;
        return light[type][at(x, y, z)];
    }
    int emitted(int type, int x, int y, int z) const
    {
        if (type == 0)
            return y >= height[z * SX + x] ? 15 : 0;
        return kEmission[id(x, y, z)];
    }
    // The value MetadataChunkBlock::updateLight computes, written independently.
    int target(int type, int x, int y, int z) const
    {
        int op = kOpacity[id(x, y, z)];
        if (op == 0)
            op = 1;
        int v = emitted(type, x, y, z);
        if (op < 15 || v > 0)
        {
            const int d[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
            for (const auto &o : d)
            {
                const int n = saved(type, x + o[0], y + o[1], z + o[2]) - op;
                if (n > v)
                    v = n;
            }
        }
        return v;
    }
};

// Fixed point from scratch: every interior cell starts dark and is raised
// until nothing changes. Values only grow, so this is the least fixed point,
// which is the only one.
void solve(World &w, std::vector<unsigned char> out[2])
{
    World s = w;
    for (int type = 0; type < 2; ++type)
    {
        for (int z = 0; z < SZ; ++z)
            for (int x = 0; x < SX; ++x)
                for (int y = 0; y < SY; ++y)
                    if (World::interior(x, z))
                        s.light[type][World::at(x, y, z)] = 0;
        for (bool changed = true; changed;)
        {
            changed = false;
            for (int y = 0; y < SY; ++y)
                for (int z = 1; z < SZ - 1; ++z)
                    for (int x = 1; x < SX - 1; ++x)
                    {
                        const int v = s.target(type, x, y, z);
                        unsigned char &cell = s.light[type][World::at(x, y, z)];
                        if (v != cell)
                        {
                            cell = (unsigned char)v;
                            changed = true;
                        }
                    }
        }
        out[type] = s.light[type];
    }
}

struct QueueView
{
    World &w;
    LightingSectionQueue &q;
    int type;
    long long sets = 0;
    int_t light(int_t x, int_t y, int_t z) { return w.saved(type, x, y, z); }
    int_t opacity(int_t x, int_t y, int_t z) { return kOpacity[w.id(x, y, z)]; }
    int_t emitted(int_t x, int_t y, int_t z) { return w.emitted(type, x, y, z); }
    void set(int_t x, int_t y, int_t z, int_t v)
    {
        w.light[type][World::at(x, y, z)] = (unsigned char)v;
        ++sets;
    }
    void schedule(int_t x, int_t y, int_t z)
    {
        if (World::interior(x, z))
            q.insert(type, x, y, z);
    }
};

// The drain loop of World::updatingLighting, without the clock.
long long drain(World &w, LightingSectionQueue &q)
{
    long long evaluated = 0;
    while (!q.empty())
    {
        const int_t index = q.front();
        LightingSectionQueue::Section &s = q.at(index);
        QueueView view{w, q, s.type};
        const int_t baseX = s.chunkX * 16, baseY = s.sectionY * 16, baseZ = s.chunkZ * 16;
        for (int_t cell = q.take(index, 0); cell >= 0; cell = q.take(index, cell))
        {
            ++evaluated;
            relaxLightCell(view, baseX + (cell & 15), baseY + (cell >> 8), baseZ + (cell >> 4 & 15));
        }
        q.releaseIfEmpty(index);
    }
    return evaluated;
}

// The previous engine: MetadataChunkBlock::updateLight for single cells,
// newest job first, neighbours queued when they differ from (new - 1) after
// the sky / emission adjustment, with the same exact dedup and no cap.
long long drainOld(World &w, std::vector<int> &jobs, std::vector<unsigned char> &queued)
{
    long long evaluated = 0;
    auto push = [&](int type, int x, int y, int z) {
        const int key = World::at(x, y, z) * 2 + type;
        if (!queued[key])
        {
            queued[key] = 1;
            jobs.push_back(key);
        }
    };
    while (!jobs.empty())
    {
        const int key = jobs.back();
        jobs.pop_back();
        queued[key] = 0;
        ++evaluated;
        const int type = key & 1, cell = key >> 1;
        const int x = cell % SX, z = cell / SX % SZ, y = cell / (SX * SZ);
        const int v = w.target(type, x, y, z);
        if (v == w.light[type][cell])
            continue;
        w.light[type][cell] = (unsigned char)v;
        const int spread = v - 1 < 0 ? 0 : v - 1;
        const int d[6][3] = {{-1, 0, 0}, {0, -1, 0}, {0, 0, -1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (const auto &o : d)
        {
            const int nx = x + o[0], ny = y + o[1], nz = z + o[2];
            if (ny < 0 || ny >= SY || !World::interior(nx, nz))
                continue;
            int s = spread;
            if (type == 0 && ny >= w.height[nz * SX + nx])
                s = 15;
            if (type == 1 && kEmission[w.id(nx, ny, nz)] > s)
                s = kEmission[w.id(nx, ny, nz)];
            if (w.light[type][World::at(nx, ny, nz)] != s)
                push(type, nx, ny, nz);
        }
    }
    return evaluated;
}

unsigned rng = 12345;
int rnd(int n) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 8) % (unsigned)n); }

void terrain(World &w)
{
    for (int z = 0; z < SZ; ++z)
        for (int x = 0; x < SX; ++x)
        {
            const int top = 60 + (x * 7 + z * 3) % 9 + rnd(3);
            for (int y = 0; y < top; ++y)
                w.block[World::at(x, y, z)] = STONE;
            if ((x / 8 + z / 8) % 3 == 0) // ponds
                for (int y = top - 4; y < top; ++y)
                    w.block[World::at(x, y, z)] = y == top - 1 && x % 5 == 0 ? ICE : WATER;
        }
    for (int i = 0; i < 40; ++i) // caves
    {
        const int cx = 2 + rnd(SX - 4), cy = 20 + rnd(35), cz = 2 + rnd(SZ - 4);
        for (int dz = -3; dz <= 3; ++dz)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -3; dx <= 3; ++dx)
                    if (World::interior(cx + dx, cz + dz))
                        w.block[World::at(cx + dx, cy + dy, cz + dz)] = AIR;
        w.block[World::at(cx, cy - 2, cz)] = (unsigned char)(i % 3 == 0 ? TORCH : (i % 3 == 1 ? LAVA : GLOWSTONE));
    }
    for (int z = 0; z < SZ; ++z)
        for (int x = 0; x < SX; ++x)
            w.column(x, z);
}

// Chunk::generateSkylightMap(columnMask) for one column, column-major form,
// plus what Chunk queues for it under PLATFORM_LIGHTING_SECTION_QUEUE.
template <class Schedule>
void regenColumn(World &w, int x, int z, Schedule schedule)
{
    w.column(x, z);
    int light = 15, y = SY - 1;
    for (; y > 0 && light > 0; --y)
    {
        light -= kOpacity[w.id(x, y, z)];
        if (light <= 0)
            continue;
        unsigned char &cell = w.light[0][World::at(x, y, z)];
        if (cell != light)
        {
            cell = (unsigned char)light;
            schedule(x, y, z);
            schedule(x - 1, y, z); schedule(x + 1, y, z);
            schedule(x, y - 1, z); schedule(x, y + 1, z);
            schedule(x, y, z - 1); schedule(x, y, z + 1);
        }
    }
    if (light <= 0) // stopped by an opaque block or a deep canopy: anything below still at 15 lost its sky
        for (int below = y + 1; below >= 0 && w.light[0][World::at(x, below, z)] == 15; --below)
            schedule(x, below, z);
}

struct Result { long long newEvaluated = 0, oldEvaluated = 0, oldWrong = 0, cells = 0; };

void run(const char *name, World &w, const std::vector<int> &edits, bool vegetation, Result &total)
{
    World old = w;
    LightingSectionQueue q;
    std::vector<int> jobs;
    std::vector<unsigned char> queued(w.block.size() * 2);
    auto both = [&](int type, int x, int y, int z) {
        if (y < 0 || y >= SY || !World::interior(x, z))
            return;
        q.insert(type, x, y, z);
        const int key = World::at(x, y, z) * 2 + type;
        if (!queued[key])
        {
            queued[key] = 1;
            jobs.push_back(key);
        }
    };
    for (std::size_t i = 0; i + 4 <= edits.size(); i += 4)
    {
        const int x = edits[i], y = edits[i + 1], z = edits[i + 2], id = edits[i + 3];
        const int oldHeight = w.height[z * SX + x];
        w.block[World::at(x, y, z)] = old.block[World::at(x, y, z)] = (unsigned char)id;
        if (vegetation)
        {
            // Vegetation fast path: the column is regenerated vertically, the
            // block's own cell is queued for both types.
            auto sky = [&](int sx, int sy, int sz) { both(0, sx, sy, sz); };
            regenColumn(w, x, z, sky);
            old.height[z * SX + x] = w.height[z * SX + x];
            for (int yy = 0; yy < SY; ++yy)
                old.light[0][World::at(x, yy, z)] = w.light[0][World::at(x, yy, z)];
        }
        else
        {
            w.column(x, z);
            old.column(x, z);
            // relightBlock's column range: every cell whose sky view changed.
            const int newHeight = w.height[z * SX + x];
            for (int yy = std::min(oldHeight, newHeight); yy < std::max(oldHeight, newHeight); ++yy)
                both(0, x, yy, z);
        }
        both(0, x, y, z);
        both(1, x, y, z);
    }

    Result r;
    r.cells = (long long)q.size();
    r.newEvaluated = drain(w, q);
    r.oldEvaluated = drainOld(old, jobs, queued);

    std::vector<unsigned char> ref[2];
    solve(w, ref);
    for (int type = 0; type < 2; ++type)
        for (std::size_t c = 0; c < ref[type].size(); ++c)
        {
            if (w.light[type][c] != ref[type][c])
            {
                std::printf("%s: MISMATCH type %d cell %zu queue=%d fixed point=%d\n",
                            name, type, c, w.light[type][c], ref[type][c]);
                std::exit(1);
            }
            r.oldWrong += old.light[type][c] != ref[type][c];
        }
    std::printf("%-22s queued %6lld  evaluated new %7lld  old %7lld (x%.2f)  old engine wrong cells %lld\n",
                name, r.cells, r.newEvaluated, r.oldEvaluated,
                r.newEvaluated ? (double)r.oldEvaluated / (double)r.newEvaluated : 0.0, r.oldWrong);
    total.newEvaluated += r.newEvaluated;
    total.oldEvaluated += r.oldEvaluated;
    total.oldWrong += r.oldWrong;
}

void queueUnitChecks()
{
    LightingSectionQueue q;
    assert(q.insert(0, -1, 5, -17));
    assert(!q.insert(0, -1, 5, -17));
    assert(q.insert(1, -1, 5, -17));
    assert(q.size() == 2);
    const int_t front = q.front();
    assert(q.at(front).type == 1 && q.at(front).chunkX == -1 && q.at(front).chunkZ == -2);
    const int_t cell = q.take(front, 0);
    assert(cell == (5 << 8 | 15 << 4 | 15));
    assert(q.take(front, cell) == -1);
    q.releaseIfEmpty(front);
    assert(q.size() == 1 && q.front() >= 0 && q.at(q.front()).type == 0);
    // Wrap-around: a cell queued behind the cursor is still found.
    const int_t s = q.front();
    q.insert(0, -16, 0, -32); // cell 0 of the same section
    assert(q.take(s, 4095) == 0);
    for (int i = 0; i < 5000; ++i)
        q.insert(i & 1, i * 16, i % 256, -i * 16);
    assert(q.size() == 5001);
    q.clear();
    assert(q.empty() && q.front() == -1);
}
} // namespace

int main()
{
    queueUnitChecks();
    Result total;
    for (int seed = 1; seed <= 6; ++seed)
    {
        rng = (unsigned)seed * 7919u;
        World w;
        terrain(w);
        std::vector<unsigned char> ref[2];
        solve(w, ref);
        w.light[0] = ref[0];
        w.light[1] = ref[1];

        // Trees: trunk and canopy through the vegetation fast path.
        std::vector<int> trees;
        for (int t = 0; t < 6; ++t)
        {
            const int tx = 4 + rnd(SX - 8), tz = 4 + rnd(SZ - 8);
            const int base = w.height[tz * SX + tx];
            for (int y = base; y < base + 5; ++y)
                trees.insert(trees.end(), {tx, y, tz, WOOD});
            for (int y = base + 3; y < base + 7; ++y)
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dx = -2; dx <= 2; ++dx)
                        if ((dx || dz || y >= base + 5) && w.id(tx + dx, y, tz + dz) == AIR)
                            trees.insert(trees.end(), {tx + dx, y, tz + dz, LEAVES});
        }
        char name[32];
        std::snprintf(name, sizeof(name), "seed %d trees", seed);
        run(name, w, trees, true, total);

        // Player-style edits: dig, fill, place lights and glass anywhere.
        std::vector<int> edits;
        for (int e = 0; e < 300; ++e)
        {
            const int x = 2 + rnd(SX - 4), z = 2 + rnd(SZ - 4), y = 15 + rnd(70);
            const int kinds[] = {AIR, AIR, STONE, TORCH, GLASS, WATER, LEAVES, GLOWSTONE, ICE};
            edits.insert(edits.end(), {x, y, z, kinds[rnd(9)]});
        }
        std::snprintf(name, sizeof(name), "seed %d edits", seed);
        run(name, w, edits, false, total);
    }
    std::printf("identical to the fixed point everywhere; cells evaluated new %lld vs old %lld (old/new %.2f); "
                "old engine left %lld wrong cells\n",
                total.newEvaluated, total.oldEvaluated,
                (double)total.oldEvaluated / (double)total.newEvaluated, total.oldWrong);
}
