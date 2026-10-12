// Identity check for the Vita section walk, the visible-order draw list and the
// mesh scheduler's population memo: the code before this change and the new
// one run side by side on the same random grids and must agree exactly.
// From src/, in bash:
//   git show origin/vita:src/pc/minecraft/RenderGlobalPcLegacyVisibility.cpp > /tmp/old_vis.cpp
//   git show origin/vita:src/pc/render/PcLegacyMeshScheduler.cpp > /tmp/old_sched.cpp
//   F="-std=c++17 -O1 -w -DMC_LINUX -DMC_LOG_LEVEL=1 -DPC_LEGACY_BUILD=1 -DVITA_PLATFORM -I. -Ipc -Inet/minecraft/src -I../vita/include -I../vita-deps/include -I../vita-deps/include/SDL2"
//   OLD="-DupdatePcLegacySectionVisibility=oldWalk -DpcLegacyRendererIndexAtSection=oldIndexAt -DpcLegacyVisibleSortedOrder=oldUnused -DpcLegacyRunMeshScheduler=oldRunMeshScheduler"
//   clang++ $F -c pc/minecraft/RenderGlobalPcLegacyVisibility.cpp -o /tmp/v_new.o &&
//   clang++ $F -c pc/render/PcLegacyMeshScheduler.cpp -o /tmp/s_new.o &&
//   clang++ $F $OLD -c /tmp/old_vis.cpp -o /tmp/v_old.o && clang++ $F $OLD -c /tmp/old_sched.cpp -o /tmp/s_old.o &&
//   clang++ $F $OLD -DOLD_SHIM -c ../vita/render_visibility_check.cpp -o /tmp/shim.o &&
//   clang++ $F ../vita/render_visibility_check.cpp /tmp/v_new.o /tmp/s_new.o /tmp/v_old.o /tmp/s_old.o /tmp/shim.o -o /tmp/vis && /tmp/vis
#ifdef OLD_SHIM
#define private public
#include "net/minecraft/src/RenderGlobal.h"
#undef private
#include "pc/render/PcLegacyMeshScheduler.h"
void callOldWalk(RenderGlobal *rg, EntityLiving *viewer) { rg->updatePcLegacySectionVisibility(viewer); }
void callOldScheduler(const std::vector<WorldRenderer *> &p, EntityLiving *v, bool f, int_t l) { pcLegacyRunMeshScheduler(p, v, f, l); }
#else
#define private public
#define protected public
#include "net/minecraft/src/AxisAlignedBB.h"
#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/ICamera.h"
#include "net/minecraft/src/MathHelper.h"
#include "net/minecraft/src/RenderGlobal.h"
#include "net/minecraft/src/World.h"
#include "net/minecraft/src/WorldRenderer.h"
#include "pc/render/PcLegacyMeshScheduler.h"
#undef private
#undef protected
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <set>
#include <vector>

void callOldWalk(RenderGlobal *rg, EntityLiving *viewer);
void callOldScheduler(const std::vector<WorldRenderer *> &p, EntityLiving *v, bool f, int_t l);

// --- stubs for what the files under test call ---------------------------------
unsigned WorldRenderer::layoutVersion = 0;
static std::set<std::pair<int, int>> g_populating;
static long g_populationLookups = 0;
static std::vector<WorldRenderer *> g_updateLog;
static int g_updateOrdinal = 0;
static std::set<WorldRenderer *> g_building;

int_t MathHelper::floor_double(double d) { return static_cast<int_t>(std::floor(d)); }
void WorldRenderer::updateInFrustrum(ICamera *icamera) { updateInFrustrum(icamera, rendererBoundingBox); }
void WorldRenderer::updateInFrustrum(ICamera *icamera, AxisAlignedBB *box)
{
    isInFrustum = icamera->isBoundingBoxInFrustum(box);
    isFullyInFrustum = false;
}
std::uint8_t WorldRenderer::pcLegacyVisibleFacesFrom(int_t face) const
{
    if (!isInitialized || face < 0 || face >= 6)
        return 0x3f;
    return pcLegacyPublishedVisibility[face];
}
float WorldRenderer::distanceToEntitySquared(Entity *entity)
{
    float dx = (float)entity->posX - (float)posXPlus, dy = (float)entity->posY - (float)posYPlus,
          dz = (float)entity->posZ - (float)posZPlus;
    return dx * dx + dy * dy + dz * dz;
}
bool WorldRenderer::isTerrainBuildInProgress() const { return g_building.count(const_cast<WorldRenderer *>(this)) != 0; }
void WorldRenderer::updateRenderer()
{
    g_updateLog.push_back(this);
    needsUpdate = (chunkIndex * 7 + ++g_updateOrdinal) % 3 == 0; // same in both grids
}
bool World::isChunkPopulationPendingForRendering(int_t x, int_t z) const
{
    ++g_populationLookups;
    return g_populating.count({x, z}) || g_populating.count({x - 1, z}) || g_populating.count({x, z - 1}) ||
           g_populating.count({x - 1, z - 1});
}

// Accepts a box by a hash of its exact corners, so a stale or wrong copy of
// the box changes the answer.
struct HashCamera : ICamera
{
    unsigned seed = 0;
    int percent = 60;
    bool isBoundingBoxInFrustum(AxisAlignedBB *b) override
    {
        unsigned h = seed;
        for (double v : {b->minX, b->minY, b->minZ, b->maxX, b->maxY, b->maxZ})
        {
            long long bits;
            std::memcpy(&bits, &v, 8);
            h = (h ^ static_cast<unsigned>(bits ^ (bits >> 32))) * 2654435761u;
        }
        return (h >> 8) % 100 < static_cast<unsigned>(percent);
    }
    void setPosition(double, double, double) override {}
};

template <class T> T *rawObject()
{
    void *p = std::calloc(1, sizeof(T));
    return static_cast<T *>(p);
}

struct Grid
{
    RenderGlobal *rg;
    std::vector<WorldRenderer *> renderers;
    EntityLiving *viewer;
};

static void setBox(WorldRenderer *r)
{
    r->rendererBoundingBox->minX = (float)r->posX; r->rendererBoundingBox->maxX = (float)(r->posX + 16);
    r->rendererBoundingBox->minY = (float)r->posY; r->rendererBoundingBox->maxY = (float)(r->posY + 16);
    r->rendererBoundingBox->minZ = (float)r->posZ; r->rendererBoundingBox->maxZ = (float)(r->posZ + 16);
    r->posXPlus = r->posX + 8; r->posYPlus = r->posY + 8; r->posZPlus = r->posZ + 8;
}

static int sectionFor(int slot, int camera, int wide)
{
    int s = camera - wide / 2;
    while (((s % wide) + wide) % wide != slot) ++s;
    return s;
}

static Grid makeGrid(unsigned seed, int wide, int tall, int deep, int camX, int camY, int camZ)
{
    std::mt19937 rng(seed);
    Grid g;
    g.rg = rawObject<RenderGlobal>();
    new (&g.rg->pcLegacyVisibilityEntryMasks) std::vector<std::uint8_t>();
    new (&g.rg->pcLegacyVisibilityQueue) std::vector<int_t>();
    new (&g.rg->pcLegacyVisibleOrder) std::vector<int_t>();
    g.rg->renderChunksWide = wide;
    g.rg->renderChunksTall = tall;
    g.rg->renderChunksDeep = deep;
    g.rg->verticalStartSection = 0;
    const int total = wide * tall * deep;
    g.rg->worldRenderers = new WorldRenderer *[total];
    g.rg->sortedWorldRenderers = new WorldRenderer *[total];
    std::vector<int> chunkIds(total);
    for (int i = 0; i < total; ++i) chunkIds[i] = i;
    std::shuffle(chunkIds.begin(), chunkIds.end(), rng);
    for (int z = 0, index = 0; z < deep; ++z)
        for (int y = 0; y < tall; ++y)
            for (int x = 0; x < wide; ++x, ++index)
            {
                WorldRenderer *r = rawObject<WorldRenderer>();
                r->rendererBoundingBox = new AxisAlignedBB();
                r->posX = sectionFor(x, camX, wide) * 16;
                r->posY = y * 16;
                r->posZ = sectionFor(z, camZ, deep) * 16;
                setBox(r);
                r->chunkIndex = chunkIds[index];
                r->isInitialized = rng() % 10 != 0;
                for (auto &faces : r->pcLegacyPublishedVisibility)
                    faces = static_cast<std::uint8_t>(rng() % 3 == 0 ? rng() & 0x3f : 0x3f);
                r->needsUpdate = rng() % 3 == 0;
                r->isInFrustum = rng() & 1;
                r->pcLegacyCpuVisible = true; // constructor default
                g.rg->worldRenderers[index] = r;
                g.rg->sortedWorldRenderers[index] = r;
                g.renderers.push_back(r);
            }
    std::shuffle(g.rg->sortedWorldRenderers, g.rg->sortedWorldRenderers + total, rng);
    g.viewer = rawObject<EntityLiving>();
    g.viewer->posX = camX * 16 + 7.5;
    g.viewer->posY = camY * 16 + 3.25;
    g.viewer->posZ = camZ * 16 + 11.0;
    return g;
}

int main()
{
    long walks = 0, orders = 0, fallbacks = 0, schedulerRuns = 0, oldLookups = 0, newLookups = 0;
    for (unsigned seed = 1; seed <= 60; ++seed)
    {
        std::mt19937 rng(seed * 7919u);
        const int wide = 5 + rng() % 13, tall = 2 + rng() % 7, deep = 5 + rng() % 13;
        const int camX = static_cast<int>(rng() % 200) - 100, camZ = static_cast<int>(rng() % 200) - 100;
        const int camY = static_cast<int>(rng() % (tall + 2)) - 1; // sometimes outside: fallback marks all
        Grid a = makeGrid(seed, wide, tall, deep, camX, camY, camZ);
        Grid b = makeGrid(seed, wide, tall, deep, camX, camY, camZ);
        const int total = wide * tall * deep;
        HashCamera camera;
        for (int frame = 0; frame < 40; ++frame)
        {
            camera.seed = rng();
            camera.percent = 20 + rng() % 81;
            const unsigned event = rng() % 10;
            if (event == 0) // re-sort, same permutation in both grids
            {
                std::vector<int> perm(total);
                for (int i = 0; i < total; ++i) perm[i] = i;
                std::shuffle(perm.begin(), perm.end(), rng);
                for (Grid *g : {&a, &b})
                {
                    std::vector<WorldRenderer *> old(g->rg->sortedWorldRenderers, g->rg->sortedWorldRenderers + total);
                    for (int i = 0; i < total; ++i) g->rg->sortedWorldRenderers[i] = old[perm[i]];
                }
            }
            else if (event == 1) // a few renderers move a whole grid width
            {
                for (int n = 0; n < 5; ++n)
                {
                    const int i = rng() % total, dir = rng() & 1 ? wide : -wide;
                    for (Grid *g : {&a, &b})
                    {
                        g->renderers[i]->posX += dir * 16;
                        setBox(g->renderers[i]);
                    }
                    ++WorldRenderer::layoutVersion;
                }
            }
            else if (event == 2) // published visibility changes
            {
                const int i = rng() % total, face = rng() % 6;
                const std::uint8_t mask = rng() & 0x3f;
                a.renderers[i]->pcLegacyPublishedVisibility[face] = mask;
                b.renderers[i]->pcLegacyPublishedVisibility[face] = mask;
            }

            a.rg->pcLegacyVisibilityFrustum = &camera;
            callOldWalk(a.rg, a.viewer);
            b.rg->pcLegacyVisibilityFrustum = &camera;
            b.rg->updatePcLegacySectionVisibility(b.viewer);
            ++walks;
            for (int i = 0; i < total; ++i)
            {
                assert(a.renderers[i]->pcLegacyCpuVisible == b.renderers[i]->pcLegacyCpuVisible);
                assert(a.renderers[i]->isInFrustum == b.renderers[i]->isInFrustum);
            }

            // The marked-only draw order must be the full scan's order.
            std::vector<int_t> expected;
            for (int p = 0; p < total; ++p)
                if (b.rg->sortedWorldRenderers[p]->pcLegacyCpuVisible)
                    expected.push_back(p);
            std::vector<int_t> order;
            if (b.rg->pcLegacyVisibleSortedOrder(order))
            {
                assert(order == expected);
                ++orders;
            }
            else
            {
                ++fallbacks;
            }
            // A move after the walk must make it refuse (marks are stale).
            if (rng() % 8 == 0)
            {
                ++WorldRenderer::layoutVersion;
                assert(!b.rg->pcLegacyVisibleSortedOrder(order));
            }

            // Scheduler: same renderers updated in the same order, fewer lookups.
            g_populating.clear();
            for (int n = 0; n < 40; ++n)
                g_populating.insert({camX + static_cast<int>(rng() % 20) - 10, camZ + static_cast<int>(rng() % 20) - 10});
            g_building.clear();
            std::vector<WorldRenderer *> pendingA, pendingB;
            for (int i = 0; i < total; ++i)
            {
                const bool pending = rng() % 2 == 0, building = rng() % 40 == 0;
                if (pending) { pendingA.push_back(a.renderers[i]); pendingB.push_back(b.renderers[i]); }
                if (building) { g_building.insert(a.renderers[i]); g_building.insert(b.renderers[i]); }
                a.renderers[i]->worldObj = b.renderers[i]->worldObj = reinterpret_cast<World *>(&g_populating);
            }
            const bool frustumOnly = rng() & 1;
            const int_t limit = 1 + rng() % 4;
            std::vector<int_t> idA, idB;
            g_updateLog.clear();
            g_updateOrdinal = 0;
            g_populationLookups = 0;
            callOldScheduler(pendingA, a.viewer, frustumOnly, limit);
            oldLookups += g_populationLookups;
            for (WorldRenderer *r : g_updateLog) idA.push_back(r->chunkIndex);
            g_updateLog.clear();
            g_updateOrdinal = 0;
            g_populationLookups = 0;
            pcLegacyRunMeshScheduler(pendingB, b.viewer, frustumOnly, limit);
            newLookups += g_populationLookups;
            for (WorldRenderer *r : g_updateLog) idB.push_back(r->chunkIndex);
            assert(idA == idB);
            for (int i = 0; i < total; ++i)
                assert(a.renderers[i]->needsUpdate == b.renderers[i]->needsUpdate);
            ++schedulerRuns;
        }
    }
    std::printf("walks %ld, orders %ld identical, %ld refused (stale), scheduler runs %ld, population lookups old %ld new %ld\n",
        walks, orders, fallbacks, schedulerRuns, oldLookups, newLookups);
    return 0;
}
#endif
