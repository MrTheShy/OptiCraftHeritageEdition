#include "net/minecraft/src/RenderList.h"

#include "net/minecraft/src/WorldRenderer.h"
#include "platform/RenderAPI.h"

RenderList::RenderList()
{
    originX = 0;
    originY = 0;
    originZ = 0;
    viewerX = 0.0;
    viewerY = 0.0;
    viewerZ = 0.0;
    displayListIds.reserve(0x10000);
#if PLATFORM_TERRAIN_VBO_BATCH
    batchDraws.reserve(0x1000);
#endif
    initialized = false;
}

void RenderList::setup(int i, int j, int k, double d, double d1, double d2)
{
    initialized = true;
    displayListIds.clear();
#if PLATFORM_TERRAIN_VBO_BATCH
    batchDraws.clear();
#endif
    originX = i;
    originY = j;
    originZ = k;
    viewerX = d;
    viewerY = d1;
    viewerZ = d2;
}

bool RenderList::matchesPos(int i, int j, int k)
{
    return initialized && i == originX && j == originY && k == originZ;
}

void RenderList::addTerrainRenderer(WorldRenderer *renderer, int_t pass)
{
    if (renderer == nullptr)
        return;

    const int_t list = renderer->getGLCallListForPass(pass);
    if (list < 0)
        return;

#if PLATFORM_TERRAIN_VBO_BATCH
    const VitaTerrainMesh &mesh = renderer->terrainBatchMesh(pass);
    if (mesh.buffer != 0)
    {
        // Section origin relative to the eye, from the same viewer position the
        // list translation uses; the draw loop applies it per section.
        batchPass = static_cast<int>(pass);
        batchDraws.push_back({mesh.buffer, mesh.vertexCount, mesh.solidCount, mesh.shader,
            static_cast<float>(static_cast<double>(renderer->posX) - viewerX),
            static_cast<float>(static_cast<double>(renderer->posY) - viewerY),
            static_cast<float>(static_cast<double>(renderer->posZ) - viewerZ), &mesh});
    }
    if (!renderer->terrainBatchNeedsList(pass))
        return;
#endif
    displayListIds.push_back(list);
    if (displayListIds.size() >= displayListIds.capacity())
        render();
}

void RenderList::render()
{
    if (!initialized)
        return;
#if PLATFORM_TERRAIN_VBO_BATCH
    // Batch first: it sets client state once and leaves it as a list draw would.
    // The lists below enable and disable their own arrays, so they must not run
    // inside it.
    if (!batchDraws.empty())
    {
        vitaTerrainDrawBatch(batchDraws.data(), batchDraws.size(), batchPass);
    }
#endif
    if (displayListIds.empty())
        return;

    const float translateX = static_cast<float>(static_cast<double>(originX) - viewerX);
    const float translateY = static_cast<float>(static_cast<double>(originY) - viewerY);
    const float translateZ = static_cast<float>(static_cast<double>(originZ) - viewerZ);
    renderPushMatrix();
    renderTranslate(translateX, translateY, translateZ);
    renderCallDisplayLists(static_cast<int>(displayListIds.size()), displayListIds.data());
    renderPopMatrix();
}

void RenderList::reset()
{
    initialized = false;
    displayListIds.clear();
}
