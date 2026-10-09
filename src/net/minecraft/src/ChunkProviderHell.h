#pragma once
#include "NoiseBuffer.h"

#include <vector>

#include "IChunkProvider.h"
#include "java/Type.h"
#include "java/Random.h"
#include "java/String.h"
#include "NoiseGeneratorOctaves.h"

class BiomeGenBase;
class World;
class Chunk;
class MapGenBase;
class MapGenNetherBridge;
class IProgressUpdate;

// net.minecraft.src.ChunkProviderHell
class ChunkProviderHell : public IChunkProvider
{
public:
	ChunkProviderHell(World *world, long_t l);
	~ChunkProviderHell() override;

	void generateHellTerrain(int_t i, int_t j, byte_t *abyte0);  // func_4059_a
	void replaceHellBlocks(int_t i, int_t j, byte_t *abyte0);    // func_4058_b

	Chunk *prepareChunk(int_t i, int_t j) override;
	Chunk *provideChunk(int_t i, int_t j) override;
	// provideChunk split for the generation worker: terrain, surface and caves
	// (this generator only) into `data`, then on the game thread the fortress
	// starts, the Chunk and its biomes. Java's populate() keeps drawing from
	// hellRNG where the last provideChunk left it, so that state travels too.
	bool generateAsyncChunkData(int_t i, int_t j, std::vector<byte_t> &data);
	Chunk *finishAsyncChunkData(int_t i, int_t j, std::vector<byte_t> &data);

	bool    chunkExists(int_t i, int_t j) override;
	void    populate(IChunkProvider *ichunkprovider, int_t i, int_t j) override;
	// populate() in pieces (one glowstone cluster each, up to 1500 tries) so
	// deferred decoration stays inside its frame budget. The task draws from a
	// copy of hellRNG taken at the first step, i.e. what populate() would see.
	bool    populateStep(IChunkProvider *ichunkprovider, int_t i, int_t j) override;
	void    abandonPopulateTask() override { populateTask.active = false; }
	bool    saveChunks(bool flag, IProgressUpdate *iprogressupdate) override;
	bool    unload100OldestChunks() override;
	bool    canSave() override;
	jstring makeString() override;
	std::vector<SpawnListEntry> *getPossibleCreatures(const EnumCreatureType &type, int_t x, int_t y, int_t z) override;
	ChunkPosition *findClosestStructure(World *world, const jstring &name, int_t x, int_t y, int_t z) override;

private:
	TerrainNoiseBuffer &generateNoiseField(TerrainNoiseBuffer &ad, int_t i, int_t j, int_t k,
	                                        int_t l, int_t i1, int_t j1); // func_4057_a

	// C++ constructs members in declaration order. Keep the Java RNG/noise order:
	// every NoiseGeneratorOctaves constructor consumes hellRNG immediately.
	Random hellRNG;
	struct PopulateTask
	{
		bool active = false;
		int_t chunkX = 0;
		int_t chunkZ = 0;
		int_t stage = 0;
		int_t index = 0;
		int_t count = 0;
		Random random{0};
	} populateTask;
	NoiseGeneratorOctaves field_4169_i;
	NoiseGeneratorOctaves field_4168_j;
	NoiseGeneratorOctaves field_4167_k;
	NoiseGeneratorOctaves field_4166_l;
	NoiseGeneratorOctaves field_4165_m;

public:
	NoiseGeneratorOctaves field_4177_a;
	NoiseGeneratorOctaves field_4176_b;

private:
	World *worldObj;

	TerrainNoiseBuffer field_4163_o;
	TerrainNoiseBuffer field_4162_p;
	TerrainNoiseBuffer field_4161_q;
	TerrainNoiseBuffer field_4160_r;

	MapGenBase *caveGenerator;  // field_4159_s
	MapGenNetherBridge *netherBridgeGenerator;

	std::vector<BiomeGenBase *> biomesForGeneration;
	TerrainNoiseBuffer field_4175_c;
	TerrainNoiseBuffer field_4174_d;
	TerrainNoiseBuffer field_4173_e;
	TerrainNoiseBuffer field_4172_f;
	TerrainNoiseBuffer field_4171_g;
};
