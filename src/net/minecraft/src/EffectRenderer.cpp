#include "EffectRenderer.h"
#include "ActiveRenderInfo.h"

#include <algorithm>
#include <sstream>

#include "Entity.h"
#include "EntityFX.h"
#include "EntityDiggingFX.h"
#include "Block.h"
#include "MathHelper.h"
#include "RenderEngine.h"
#include "Tessellator.h"
#include "World.h"
#include "platform/PlatformTuning.h"
#include "platform/PlatformPerfOptions.h"
#include "platform/Profiler.h"
#if PLATFORM_PS2
#include "ps2/diagnostics/Ps2OptimizationValidation.h"
#endif
#include "platform/RenderAPI.h"

EffectRenderer::EffectRenderer(World *world, RenderEngine *renderengine)
    : worldObj(nullptr), renderer(renderengine), rand()
{
    if (world != nullptr)
        worldObj = world;

    for (int i = 0; i < 4; i++)
        fxLayers[i].clear();
}

EffectRenderer::~EffectRenderer() = default;

void EffectRenderer::addEffect(EntityFX *entityfx)
{
	if (entityfx == nullptr)
		return;
    int_t i = entityfx->getFXLayer();
    if (!updatingEffects && (int_t)fxLayers[i].size() >= PLATFORM_MAX_PARTICLES_PER_LAYER)
    {
#ifdef PS2_OPTIMIZATION_VALIDATION
        Ps2OptimizationValidation::particleLayerCapEviction();
#endif
        fxLayers[i].pop_front();
    }
	fxLayers[i].emplace_back(entityfx);
}

void EffectRenderer::updateEffects()
{
    if (platformPerf().skipWorldParticles)
    {
    // Particles are not drawn with this option, but every one still ran its
    // onUpdate with a block collision sweep until it died. A broken block adds
    // 64 of them (addBlockDestroyEffects): measured 2026-09-16 as the
    // 11-21 ms `effects` tick phase in the frames right after an edit, on top
    // of the mesh rebuild the edit already costs. Drop them here rather than
    // in addEffect: spawnParticleEffect hands the pointer back to callers that
    // still set colour and velocity on it during the tick that spawned it.
    for (int i = 0; i < 4; i++)
        fxLayers[i].clear();
    return;
    }
    // One stable compaction pass per layer: the dead are dropped and the rest
    // keep their order, without shifting the layer once per dead particle
    // (boat splashes and block breaks kill dozens a tick). Particles spawned
    // by an onUpdate land at the end and are updated in the same pass, as before.
    updatingEffects = true;
    int_t live = 0;
    for (int i = 0; i < 4; i++)
    {
        std::deque<std::unique_ptr<EntityFX>> &layer = fxLayers[i];
        std::size_t kept = 0;
        for (std::size_t j = 0; j < layer.size(); j++)
        {
            EntityFX *entityfx = layer[j].get();
            entityfx->onUpdate();
            if (entityfx->isDead)
                continue;
            if (kept != j)
                layer[kept] = std::move(layer[j]);
            kept++;
        }
        layer.erase(layer.begin() + (std::ptrdiff_t)kept, layer.end());
        while ((int_t)layer.size() > PLATFORM_MAX_PARTICLES_PER_LAYER)
            layer.pop_front();
        live += (int_t)layer.size();
    }
    updatingEffects = false;
#if PLATFORM_PROFILE_RENDER_PHASES
    platformProfileTickPhase("#fx", live);
#else
    (void)live;
#endif
}

void EffectRenderer::renderParticles(Entity *entity, float_t f)
{
    const float_t f1 = ActiveRenderInfo::rotationX;
    const float_t f2 = ActiveRenderInfo::rotationZ;
    const float_t f3 = ActiveRenderInfo::rotationYZ;
    const float_t f4 = ActiveRenderInfo::rotationXY;
    const float_t f5 = ActiveRenderInfo::rotationXZ;

    EntityFX::interpPosX = entity->lastTickPosX + (entity->posX - entity->lastTickPosX) * (double)f;
    EntityFX::interpPosY = entity->lastTickPosY + (entity->posY - entity->lastTickPosY) * (double)f;
    EntityFX::interpPosZ = entity->lastTickPosZ + (entity->posZ - entity->lastTickPosZ) * (double)f;

    for (int i = 0; i < 3; i++)
    {
        if (fxLayers[i].empty())
            continue;

        int_t j = 0;
        if (i == 0) j = renderer->getTexture("/particles.png");
        if (i == 1) j = renderer->getTexture("/terrain.png");
        if (i == 2) j = renderer->getTexture("/gui/items.png");

        renderBindTexture(j);

        Tessellator *tessellator = &Tessellator::instance;
        renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        tessellator->startDrawingQuads();

        for (int k = 0; k < (int)fxLayers[i].size(); k++)
        {
			EntityFX *entityfx = fxLayers[i][k].get();
            tessellator->setBrightness(entityfx->getBrightnessForRender(f));
            entityfx->renderParticle(tessellator, f, f1, f5, f2, f3, f4);
        }

        tessellator->draw();
    }
}

void EffectRenderer::renderLitParticles(Entity *entity, float_t f)
{
    const float_t f1 = MathHelper::cos(entity->rotationYaw * (3.1415927f / 180.0f));
    const float_t f2 = MathHelper::sin(entity->rotationYaw * (3.1415927f / 180.0f));
    const float_t f3 = -f2 * MathHelper::sin(entity->rotationPitch * (3.1415927f / 180.0f));
    const float_t f4 = f1 * MathHelper::sin(entity->rotationPitch * (3.1415927f / 180.0f));
    const float_t f5 = MathHelper::cos(entity->rotationPitch * (3.1415927f / 180.0f));
    const int_t layer = 3;
    if (fxLayers[layer].empty())
        return;

    Tessellator *tessellator = &Tessellator::instance;
    for (int i = 0; i < (int)fxLayers[layer].size(); i++)
    {
		EntityFX *entityfx = fxLayers[layer][i].get();
        tessellator->setBrightness(entityfx->getBrightnessForRender(f));
        entityfx->renderParticle(tessellator, f, f1, f5, f2, f3, f4);
    }
}

void EffectRenderer::clearEffects(World *world)
{
    worldObj = world;
    for (int i = 0; i < 4; i++)
        fxLayers[i].clear();
}

void EffectRenderer::addBlockDestroyEffects(int_t i, int_t j, int_t k, int_t l, int_t i1)
{
    if (l <= 0 || l >= Block::BLOCK_REGISTRY_SIZE)
        return;

    Block *block = Block::blocksList[l];
    if (block == nullptr)
        return;

    // updateEffects() would delete all 64 of them on its next call anyway.
    if (platformPerf().skipWorldParticles)
        return;

    int_t j1 = PLATFORM_BLOCK_DESTROY_PARTICLE_GRID;

    for (int k1 = 0; k1 < j1; k1++)
    {
        for (int l1 = 0; l1 < j1; l1++)
        {
            for (int i2 = 0; i2 < j1; i2++)
            {
                const float localX = (static_cast<float>(k1) + 0.5f) / static_cast<float>(j1);
                const float localY = (static_cast<float>(l1) + 0.5f) / static_cast<float>(j1);
                const float localZ = (static_cast<float>(i2) + 0.5f) / static_cast<float>(j1);
                const double d = static_cast<double>(i) + static_cast<double>(localX);
                const double d1 = static_cast<double>(j) + static_cast<double>(localY);
                const double d2 = static_cast<double>(k) + static_cast<double>(localZ);
                int_t j2 = rand.nextInt(6);
                addEffect(
                    (new EntityDiggingFX(worldObj, d, d1, d2,
                        static_cast<double>(localX - 0.5f),
                        static_cast<double>(localY - 0.5f),
                        static_cast<double>(localZ - 0.5f),
                        block, j2, i1))->applyColourMultiplier(i, j, k)
                );
            }
        }
    }
}

void EffectRenderer::addBlockHitEffects(int_t i, int_t j, int_t k, int_t l)
{
    if (worldObj == nullptr)
        return;

    int_t i1 = worldObj->getBlockId(i, j, k);
    if (i1 <= 0 || i1 >= Block::BLOCK_REGISTRY_SIZE)
        return;

    Block *block = Block::blocksList[i1];
    if (block == nullptr)
        return;

    float_t f = 0.1f;

    double d  = (double)i + rand.nextDouble() * (block->maxX - block->minX - (double)(f * 2.0f)) + (double)f + block->minX;
    double d1 = (double)j + rand.nextDouble() * (block->maxY - block->minY - (double)(f * 2.0f)) + (double)f + block->minY;
    double d2 = (double)k + rand.nextDouble() * (block->maxZ - block->minZ - (double)(f * 2.0f)) + (double)f + block->minZ;

    if (l == 0) d1 = ((double)j + block->minY) - (double)f;
    if (l == 1) d1 =  (double)j + block->maxY  + (double)f;
    if (l == 2) d2 = ((double)k + block->minZ) - (double)f;
    if (l == 3) d2 =  (double)k + block->maxZ  + (double)f;
    if (l == 4) d  = ((double)i + block->minX) - (double)f;
    if (l == 5) d  =  (double)i + block->maxX  + (double)f;

    addEffect(
        (new EntityDiggingFX(worldObj, d, d1, d2, 0.0, 0.0, 0.0,
            block, l, worldObj->getBlockMetadata(i, j, k)))
            ->applyColourMultiplier(i, j, k)->multiplyVelocity(0.2f)->multiplyParticleScaleBy(0.6f)
    );
}

std::string EffectRenderer::getStatistics()
{
    std::ostringstream oss;
    oss << (fxLayers[0].size() + fxLayers[1].size() + fxLayers[2].size());
    return oss.str();
}
