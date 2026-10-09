#include "BiomeCacheBlock.h"

#include "BiomeCache.h"
#include "WorldChunkManager.h"
#include "java/Arithmetic.h"

#include <typeinfo>

BiomeCacheBlock::BiomeCacheBlock(BiomeCache *cache, int_t chunkX, int_t chunkZ)
    : temperatureValues(256, 0.0f),
      rainfallValues(256, 0.0f),
      biomes(256, nullptr),
      xPosition(chunkX),
      zPosition(chunkZ),
      lastAccessTime(0),
      biomeCache(cache)
{
    WorldChunkManager *manager = biomeCache->getChunkManager();
    // Subclasses override the three getters; only the base one can share.
    if (typeid(*manager) == typeid(WorldChunkManager) &&
        manager->fillCacheBlock(JavaArithmetic::intShl(chunkX, 4), JavaArithmetic::intShl(chunkZ, 4),
                                temperatureValues, rainfallValues, biomes))
        return;
    manager->getTemperatures(temperatureValues, JavaArithmetic::intShl(chunkX, 4), JavaArithmetic::intShl(chunkZ, 4), 16, 16);
    manager->getRainfall(rainfallValues, JavaArithmetic::intShl(chunkX, 4), JavaArithmetic::intShl(chunkZ, 4), 16, 16);
    manager->getBiomeGenAt(biomes, JavaArithmetic::intShl(chunkX, 4), JavaArithmetic::intShl(chunkZ, 4), 16, 16, false);
}

BiomeGenBase *BiomeCacheBlock::getBiomeGenAt(int_t x, int_t z) const
{
    return biomes[static_cast<std::size_t>((x & 15) | ((z & 15) << 4))];
}
