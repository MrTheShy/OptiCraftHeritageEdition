#include "platform/Log.h"
#include "ChunkProvider.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <sstream>

#include "World.h"
#include "WorldInfo.h"
#include "WorldProvider.h"
#include "Chunk.h"
#include "EmptyChunk.h"
#include "IChunkLoader.h"
#include "IProgressUpdate.h"
#include "ThreadedFileIOBase.h"
#include "ChunkCoordIntPair.h"
#include "ChunkProviderLoadOrGenerate.h"
#include "ChunkProviderGenerate.h"
#include "ChunkProviderHell.h"
#include "BiomeDecorator.h"
#include "McRegionChunkLoader.h"
#include "AnvilChunkLoader.h"
#include "RegionFile.h"
#include "Config.h"
#include "java/Arithmetic.h"
#include "java/String.h"
#include "java/System.h"
#include "platform/PlatformTuning.h"
#include "platform/WorldLoadTrace.h"
#include "platform/chunks/ChunkGenerationScheduler.h"
#include "platform/chunks/ChunkMemoryPolicy.h"
#include "platform/world/StreamingFrameBudget.h"
#include "ISaveHandler.h"

#include "platform/Profiler.h"

namespace
{
	int_t clampInt(int_t value, int_t minValue, int_t maxValue)
	{
		if (value < minValue) return minValue;
		if (value > maxValue) return maxValue;
		return value;
	}

	// Only the unbounded (desktop) path uses this; PLATFORM_BOUNDED_WORLD takes
	// both radii straight from the tuning table instead of deriving one.
	const int_t CACHE_RADIUS_MARGIN = 4;


#if PLATFORM_DEFERRED_POPULATE
	const int_t POPULATION_FOOTPRINT_AXIS = 2;
	const int_t POPULATION_SECTION_COUNT = 8;
#endif
}

ChunkProvider::ChunkProvider(World *world, IChunkLoader *ichunkloader, IChunkProvider *ichunkprovider)
#if PLATFORM_ASYNC_CHUNK_GENERATION
	: asyncGenerationScheduler(nullptr)
	, droppedChunksSet()
#else
	: droppedChunksSet()
#endif
	, blankChunk(nullptr)
	, chunkProvider(ichunkprovider)
	, chunkLoader(ichunkloader)
	, chunkMap()
	, chunkList()
	, lastChunk(nullptr)
	, lastChunkX(0)
	, lastChunkZ(0)
	, worldObj(world)
	, curChunkX(0)
	, curChunkZ(0)
	, chunkLoadRadius(15)
	, chunkUnloadRadius(15)
	, chunkTopologyVersion(1)
{
	blankChunk = new EmptyChunk(world, std::vector<byte_t>(32768, 0), 0, 0);
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
	generationMoveX = 0;
	generationMoveZ = 0;
	generationCenterInitialized = false;
#endif
#if PLATFORM_BOUNDED_WORLD
	{
		// A resident region (the End) sits on top of the sliding window.
		std::size_t reserve = PLATFORM_CHUNK_MAP_RESERVE;
		const int_t residentRadius = world != nullptr && world->worldProvider != nullptr
			? world->worldProvider->getResidentChunkRadius() : -1;
		if (residentRadius >= 0)
			reserve += static_cast<std::size_t>(residentRadius * 2 + 1) * (residentRadius * 2 + 1);
		chunkMap.reserve(reserve);
		chunkList.reserve(reserve);
	}
	genChunksThisTick = 0;
	setChunkLoadRadius(PLATFORM_CHUNK_CACHE_RADIUS);
#else
	chunkMap.reserve(256);
	chunkList.reserve(256);
#endif
#if PLATFORM_ASYNC_CHUNK_GENERATION
	asyncGenerationScheduler = nullptr;
	asyncSavedChunkProbe = nullptr;
	McRegionChunkLoader *asyncRegionLoader = dynamic_cast<McRegionChunkLoader *>(ichunkloader);
	AnvilChunkLoader *asyncAnvilLoader = dynamic_cast<AnvilChunkLoader *>(ichunkloader);
	// The worker reads region files only through McRegionChunkLoader. With the
	// Anvil loader it runs generation alone (a nullptr region loader), and
	// requestChunkDetailed() checks the save first so nothing on disk is ever
	// handed to it. Release worlds are Anvil, so without this branch the
	// worker never existed and every chunk generated on the game thread.
	const bool netherGenerator = dynamic_cast<ChunkProviderHell *>(chunkProvider) != nullptr;
	if ((dynamic_cast<ChunkProviderGenerate *>(chunkProvider) != nullptr || netherGenerator) && worldObj != nullptr &&
	    (asyncRegionLoader != nullptr || asyncAnvilLoader != nullptr))
	{
		asyncSavedChunkProbe = asyncRegionLoader == nullptr ? asyncAnvilLoader : nullptr;
#if PLATFORM_PC_LEGACY || PLATFORM_WII
		// The worker only builds terrain/cave buffers. Structure discovery,
		// decoration, Chunk construction, lighting and publication stay on the
		// game thread: the per-biome BiomeDecorator and the chunk-local
		// decoration scope on World are shared state, and a worker running
		// provideChunk() to completion raced the game thread's own decoration
		// ("Already decorating!!" on Wii).
		// The Nether's generator is cheap to duplicate (noise, caves, its own
		// hellRNG); its fortress starts and Chunk stay on the game thread.
		auto newWorkerGenerator = [this, netherGenerator]() -> IChunkProvider *
		{
			if (netherGenerator)
				return new ChunkProviderHell(worldObj, worldObj->getRandomSeed());
			return new ChunkProviderGenerate(worldObj, worldObj->getRandomSeed(), false,
				PLATFORM_ASYNC_ISOLATED_BIOME_SOURCE != 0);
		};
		asyncGenerationScheduler = new ChunkGenerationScheduler(newWorkerGenerator(), asyncRegionLoader, worldObj);
		for (int worker = 1; worker < PLATFORM_ASYNC_GENERATION_WORKERS; ++worker)
			asyncGenerationScheduler->addWorker(newWorkerGenerator());
#else
		asyncGenerationScheduler = new ChunkGenerationScheduler(
			new ChunkProviderGenerate(worldObj, worldObj->getRandomSeed()),
			asyncRegionLoader, worldObj);
#endif
#if PLATFORM_ASYNC_SAVED_CHUNK_LOAD
		// Saved chunks go to the worker too: it reads and parses them, the
		// game thread only builds the Chunk. Otherwise revisiting terrain
		// stalls a frame on memory-card I/O and inflate for every chunk.
		// With no probe, requestChunkDetailed() no longer touches the region
		// files from the game thread either.
		if (asyncAnvilLoader != nullptr && asyncRegionLoader == nullptr)
		{
			asyncGenerationScheduler->setAnvilLoader(asyncAnvilLoader);
			asyncSavedChunkProbe = nullptr;
		}
#endif
		if (!asyncGenerationScheduler->start())
		{
			delete asyncGenerationScheduler;
			asyncGenerationScheduler = nullptr;
			asyncSavedChunkProbe = nullptr;
		}
	}
#endif
}

ChunkProvider::~ChunkProvider()
{
#if PLATFORM_ASYNC_CHUNK_GENERATION
	delete asyncGenerationScheduler;
	asyncGenerationScheduler = nullptr;
#endif
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
	if (ChunkProviderGenerate *generator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider))
		generator->cancelGenerationTask();
	generationQueue.clear();
	generationQueued.clear();
#endif
	std::unordered_set<Chunk *> uniqueChunks;
	uniqueChunks.reserve(chunkMap.size());
	for (auto &entry : chunkMap)
	{
		Chunk *chunk = entry.second;
		if (chunk != nullptr && chunk != blankChunk)
			uniqueChunks.insert(chunk);
	}

	for (Chunk *chunk : uniqueChunks)
		delete chunk;

	chunkMap.clear();
	chunkList.clear();
	droppedChunksSet.clear();

	delete blankChunk;
	delete chunkLoader;
	delete chunkProvider;

	blankChunk = nullptr;
	chunkLoader = nullptr;
	chunkProvider = nullptr;
	worldObj = nullptr;
}

std::uint64_t ChunkProvider::chunkKey(int_t i, int_t j)
{
	return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32)
	     | static_cast<std::uint32_t>(j);
}

bool ChunkProvider::findResidentChunk(int_t i, int_t j, Chunk *&chunk)
{
	const std::uint64_t key = chunkKey(i, j);
	ResidentSlot &slot = residentSlots[(i & 7) | ((j & 7) << 3)];
	if (slot.version == chunkTopologyVersion && slot.key == key)
	{
		chunk = slot.chunk;
#if PLATFORM_VERIFY_ENTITY_SHORTCUTS
		auto check = chunkMap.find(key);
		if (check == chunkMap.end() || check->second != chunk)
			MC_LOG_ERROR("chunk", "shortcut mismatch: residentSlots %d,%d\n", i, j);
#endif
		return true;
	}
	auto it = chunkMap.find(key);
	if (it == chunkMap.end())
		return false;
	slot.key = key;
	slot.chunk = it->second;
	slot.version = chunkTopologyVersion;
	chunk = it->second;
	return true;
}

void ChunkProvider::markChunkTopologyChanged()
{
	++chunkTopologyVersion;
	if (chunkTopologyVersion == 0)
		++chunkTopologyVersion;
}

void ChunkProvider::setCurrentChunkOver(int_t i, int_t j)
{
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
	if (generationCenterInitialized)
	{
		const int_t deltaX = JavaArithmetic::intSub(i, curChunkX);
		const int_t deltaZ = JavaArithmetic::intSub(j, curChunkZ);
		if (deltaX != 0 || deltaZ != 0)
		{
			generationMoveX = deltaX > 0 ? 1 : (deltaX < 0 ? -1 : 0);
			generationMoveZ = deltaZ > 0 ? 1 : (deltaZ < 0 ? -1 : 0);
		}
	}
	else
	{
		generationMoveX = 0;
		generationMoveZ = 0;
		generationCenterInitialized = true;
	}
#endif
	curChunkX = i;
	curChunkZ = j;

	// Propagate the player-centred unload origin to the inner provider. Without
	// this the inner ChunkProviderLoadOrGenerate keeps curChunkX/Z at its initial
	// (spawn) value, so its distance-based unload100OldestChunks() never evicts the
	// chunks it generates around the moving player -- they accumulate and leak RAM
	// as you walk (the 32 MB "OOM after ~30 s of moving"). Both cache tiers must be
	// centred on the player. dynamic_cast because setCurrentChunkOver is not on the
	// IChunkProvider interface.
	if (ChunkProviderLoadOrGenerate *inner = dynamic_cast<ChunkProviderLoadOrGenerate *>(chunkProvider))
		inner->setCurrentChunkOver(i, j);
	else if (ChunkProvider *inner2 = dynamic_cast<ChunkProvider *>(chunkProvider))
		inner2->setCurrentChunkOver(i, j);
}

#if PLATFORM_BOUNDED_WORLD || PLATFORM_ASYNC_CHUNK_GENERATION
void ChunkProvider::notifyChunkPublished(Chunk *chunk)
{
	if (worldObj == nullptr || chunk == nullptr || chunk == blankChunk)
		return;

	// A console renderer at the edge of the sliding world cache is allowed to
	// build against EmptyChunk for source chunks outside the current load radius.
	// If that chunk later becomes resident, its completed empty column is stale.
	// Chunk publication is the authoritative transition, so invalidate that
	// column here. Use the interior range because RenderGlobal expands dirty
	// ranges by one block; this reaches the chunk bounds without rebuilding all
	// eight horizontal neighbours for every streamed chunk. Active neighbouring
	// builds detect source availability changes themselves in WorldRenderer.
	const int_t minX = JavaArithmetic::intMul(chunk->xPosition, 16);
	const int_t minZ = JavaArithmetic::intMul(chunk->zPosition, 16);
	platformProfileTickPhase("#dirtyChunkPub", 1);
	worldObj->markBlocksDirty(minX + 1, 1, minZ + 1,
	                          minX + 14, 126, minZ + 14);
#if PLATFORM_PS2
	worldObj->notifyChunkPublishedForRender(chunk->xPosition, chunk->zPosition);
#endif
}
#endif

void ChunkProvider::setChunkLoadRadius(int_t radius)
{
	// Use a slightly larger RAM cache than the strict visible radius.  This avoids
	// unloading/reloading the same chunks when the player moves a few blocks,
	// without going back to the old fixed 1024-slot cache.
#if PLATFORM_BOUNDED_WORLD
	(void)radius;
	ISaveHandler *saveHandler = worldObj != nullptr ? worldObj->getSaveHandler() : nullptr;
	const ChunkMemoryPolicy::RetentionPolicy policy =
		ChunkMemoryPolicy::retentionPolicy(saveHandler != nullptr && saveHandler->isReadOnly());
	chunkLoadRadius = policy.loadRadius;
	chunkUnloadRadius = policy.unloadRadius;
#elif PLATFORM_PC_LEGACY && defined(VITA_PLATFORM)
	// Follows the render-distance option. Never below the profile's cache
	// radius, so random ticks and spawning (radius 4) stay inside loaded
	// chunks and short distances behave exactly as before.
	chunkLoadRadius = std::max<int_t>(radius, PLATFORM_CHUNK_CACHE_RADIUS);
	chunkUnloadRadius = std::max<int_t>(chunkLoadRadius + 1, PLATFORM_CHUNK_UNLOAD_RADIUS);
#elif PLATFORM_PC_LEGACY
	(void)radius;
	chunkLoadRadius = PLATFORM_CHUNK_CACHE_RADIUS;
	chunkUnloadRadius = PLATFORM_CHUNK_UNLOAD_RADIUS;
#else
	chunkLoadRadius = clampInt(radius, 2, 15);
	chunkUnloadRadius = clampInt(chunkLoadRadius + CACHE_RADIUS_MARGIN, chunkLoadRadius, 15);
#endif
}

void ChunkProvider::setChunkLoadRadiusFromRenderDistance(int_t renderDistance)
{
#if PLATFORM_PC_LEGACY && defined(VITA_PLATFORM) && !PLATFORM_BOUNDED_WORLD
	(void)renderDistance;
	setChunkLoadRadius(Config::getRenderDistanceFine() / 16 + 1);
#elif PLATFORM_BOUNDED_WORLD || PLATFORM_PC_LEGACY
	(void)renderDistance;
	setChunkLoadRadius(0);
#else
	renderDistance &= 3;
	int_t blocks = 64 << (3 - renderDistance);
	if (blocks > 400)
		blocks = 400;

	const int_t renderChunksWide = blocks / 16 + 1;
	setChunkLoadRadius(renderChunksWide / 2 + 2);
#endif
}

bool ChunkProvider::canChunkExist(int_t i, int_t j) const
{
	if (worldObj != nullptr && worldObj->isLimitedWorld())
	{
		const int_t minChunk = worldObj->getWorldInfo() != nullptr ? worldObj->getWorldInfo()->getLimitedWorldMinChunk() : -8;
		const int_t maxChunk = worldObj->getWorldInfo() != nullptr ? worldObj->getWorldInfo()->getLimitedWorldMaxChunk() : 7;
		if (i < minChunk || i > maxChunk || j < minChunk || j > maxChunk)
			return false;
	}
	const int_t minX = JavaArithmetic::intSub(curChunkX, chunkLoadRadius);
	const int_t minZ = JavaArithmetic::intSub(curChunkZ, chunkLoadRadius);
	const int_t maxX = JavaArithmetic::intAdd(curChunkX, chunkLoadRadius);
	const int_t maxZ = JavaArithmetic::intAdd(curChunkZ, chunkLoadRadius);
	if (i >= minX && j >= minZ && i <= maxX && j <= maxZ)
		return true;
	if (worldObj != nullptr && worldObj->isChunkResident(i, j))
		return true;
#if PLATFORM_ENTITY_CHUNK_RETENTION
	return worldObj != nullptr && worldObj->isChunkRetainedByEntity(i, j);
#else
	return false;
#endif
}

long_t ChunkProvider::currentWorldTime() const
{
	return worldObj != nullptr ? worldObj->getWorldTime() : 0LL;
}

bool ChunkProvider::isOutsideUnloadRadius(int_t i, int_t j) const
{
	if (worldObj != nullptr && worldObj->isChunkResident(i, j))
		return false;
#if PLATFORM_ENTITY_CHUNK_RETENTION
	if (worldObj != nullptr && worldObj->isChunkRetainedByEntity(i, j))
		return false;
#endif
	const long_t dx = static_cast<long_t>(i) - static_cast<long_t>(curChunkX);
	const long_t dz = static_cast<long_t>(j) - static_cast<long_t>(curChunkZ);
	const long_t radius = static_cast<long_t>(chunkUnloadRadius);
	return dx < -radius || dz < -radius || dx > radius || dz > radius;
}

bool ChunkProvider::chunkExists(int_t i, int_t j)
{
	Chunk *chunk;
	return findResidentChunk(i, j, chunk);
}

Chunk *ChunkProvider::getChunkIfSaved(int_t i, int_t j)
{
	Chunk *chunk;
	if (findResidentChunk(i, j, chunk))
		return chunk;
	AnvilChunkLoader *anvil = dynamic_cast<AnvilChunkLoader *>(chunkLoader);
	if (anvil != nullptr && !anvil->isChunkSaved(i, j))
		return nullptr;
	return prepareChunk(i, j);
}

Chunk *ChunkProvider::getChunkIfExists(int_t i, int_t j)
{
#if !PLATFORM_BOUNDED_WORLD
	// lastChunk is always a resident map entry (provideChunk trusts it too).
	if (lastChunk != nullptr && i == lastChunkX && j == lastChunkZ)
		return lastChunk;
#endif
	Chunk *chunk;
	if (!findResidentChunk(i, j, chunk))
		return nullptr;

#if PLATFORM_BOUNDED_WORLD
	if (worldObj != nullptr && !worldObj->findingSpawnPoint && !canChunkExist(i, j))
		return blankChunk;
#endif
	if (lastChunk != nullptr && i == lastChunkX && j == lastChunkZ)
		return lastChunk;

	if (chunk != nullptr)
		chunk->lastAccessTick = currentWorldTime();
	if (chunk != nullptr && chunk != blankChunk)
	{
		lastChunk = chunk;
		lastChunkX = i;
		lastChunkZ = j;
	}
	return chunk;
}

#if PLATFORM_ASYNC_CHUNK_GENERATION
namespace
{
// Request outcomes since the last decoration log line (index = ChunkRequestStatus).
int_t g_requestStatusCounts[8];
}

ChunkProvider::ChunkRequestStatus ChunkProvider::requestChunkDetailed(int_t i, int_t j)
{
	const ChunkRequestStatus status = requestChunkDetailedImpl(i, j);
	++g_requestStatusCounts[static_cast<int>(status) & 7];
	return status;
}

void ChunkProvider::describeStreaming(char *out, int size)
{
	int written = std::snprintf(out, size, "requests ok %d queued %d loaded %d full %d range %d disk %d off %d; gen ",
		(int)g_requestStatusCounts[0], (int)g_requestStatusCounts[1], (int)g_requestStatusCounts[2],
		(int)g_requestStatusCounts[3], (int)g_requestStatusCounts[4], (int)g_requestStatusCounts[5],
		(int)g_requestStatusCounts[6]);
	for (int_t &count : g_requestStatusCounts)
		count = 0;
	if (written < 0 || written >= size)
		return;
	if (asyncGenerationScheduler != nullptr)
		asyncGenerationScheduler->describe(out + written, size - written);
	else
		std::snprintf(out + written, size - written, "none");
}

ChunkProvider::ChunkRequestStatus ChunkProvider::requestChunkDetailedImpl(int_t i, int_t j)
{
	if (asyncGenerationScheduler == nullptr || !asyncGenerationScheduler->active())
		return ChunkRequestStatus::Inactive;
	if (worldObj != nullptr && !worldObj->findingSpawnPoint && !canChunkExist(i, j))
		return ChunkRequestStatus::OutOfRange;
	if (chunkMap.count(chunkKey(i, j)) != 0)
		return ChunkRequestStatus::AlreadyLoaded;
	if (asyncSavedChunkProbe != nullptr && asyncSavedChunkProbe->isChunkSaved(i, j))
		return ChunkRequestStatus::SavedOnDisk;
	// Every block read in a missing column asks again (8.5M "full" and 400k
	// "queued" answers a minute on the Vita): answer those without the
	// scheduler's lock. asyncRequestStale holds exactly the claimed columns;
	// the queue can only free a slot when a result is drained or a request
	// rejected, which is where asyncQueueFull is cleared.
	if (asyncRequestStale.count(chunkKey(i, j)) != 0)
		return ChunkRequestStatus::AlreadyQueued;
	if (asyncQueueFull)
		return ChunkRequestStatus::QueueFull;

	switch (asyncGenerationScheduler->requestDetailed(i, j, PLATFORM_ASYNC_GENERATION_QUEUE_LIMIT))
	{
	case ChunkGenerationScheduler::RequestStatus::Accepted:
		asyncRequestStale[chunkKey(i, j)] = false;
		return ChunkRequestStatus::Accepted;
	case ChunkGenerationScheduler::RequestStatus::AlreadyQueued:
		return ChunkRequestStatus::AlreadyQueued;
	case ChunkGenerationScheduler::RequestStatus::QueueFull:
		asyncQueueFull = true;
		return ChunkRequestStatus::QueueFull;
	case ChunkGenerationScheduler::RequestStatus::Inactive:
	default:
		return ChunkRequestStatus::Inactive;
	}
}

bool ChunkProvider::requestChunk(int_t i, int_t j)
{
	return requestChunkDetailed(i, j) == ChunkRequestStatus::Accepted;
}

void ChunkProvider::serviceAsyncChunkStreaming()
{
	if (asyncGenerationScheduler == nullptr || !asyncGenerationScheduler->active())
		return;

	asyncGenerationScheduler->setFocus(curChunkX, curChunkZ);
	if (PLATFORM_ASYNC_GENERATION_PUBLISH_PER_FRAME > 0)
		drainAsyncGeneratedChunks(PLATFORM_ASYNC_GENERATION_PUBLISH_PER_FRAME);
	if (PLATFORM_ASYNC_GENERATION_REQUESTS_PER_FRAME > 0)
		drainAsyncGenerationRequests(PLATFORM_ASYNC_GENERATION_REQUESTS_PER_FRAME);
}

void ChunkProvider::markAsyncRequestStale(std::uint64_t key)
{
	if (asyncRequestStale.empty())
		return;
	auto it = asyncRequestStale.find(key);
	if (it != asyncRequestStale.end())
		it->second = true;
}

bool ChunkProvider::acceptAsyncGenerationCoordinate(void* context, int_t i, int_t j)
{
	ChunkProvider* self = static_cast<ChunkProvider*>(context);
	if (self == nullptr)
		return false;
	const std::uint64_t key = chunkKey(i, j);
	if (self->chunkMap.count(key) != 0
		|| !(self->worldObj == nullptr || self->worldObj->findingSpawnPoint || self->canChunkExist(i, j)))
	{
		// dispatch() releases the scheduler's claim on a rejected coordinate.
		self->asyncRequestStale.erase(key);
		return false;
	}
	// The worker has not seen this coordinate yet and will read the current
	// saved state, so anything that happened since the request is irrelevant.
	self->asyncRequestStale[key] = false;
	return true;
}

bool ChunkProvider::drainAsyncGenerationRequests(int_t budget)
{
	// A rejected coordinate leaves the queue (dispatch releases it).
	asyncQueueFull = false;
	return asyncGenerationScheduler != nullptr &&
		asyncGenerationScheduler->dispatch(budget, &ChunkProvider::acceptAsyncGenerationCoordinate, this);
}

bool ChunkProvider::drainAsyncGeneratedChunks(int_t budget)
{
	if (asyncGenerationScheduler == nullptr || budget <= 0)
		return false;

	bool published = false;
	for (int_t n = 0; n < budget; ++n)
	{
		ChunkGenerationScheduler::Result result;
		if (!asyncGenerationScheduler->popResult(result))
			break;

		const std::uint64_t key = chunkKey(result.x, result.z);
		const auto staleIt = asyncRequestStale.find(key);
		const bool stale = staleIt != asyncRequestStale.end() && staleIt->second;
		const bool wanted = !stale && chunkMap.count(key) == 0
			&& (worldObj == nullptr || worldObj->findingSpawnPoint || canChunkExist(result.x, result.z));

		Chunk *chunk = nullptr;
		if (wanted)
		{
			switch (result.kind)
			{
			case ChunkGenerationScheduler::ResultKind::LoadedData:
			{
				McRegionChunkLoader* regionLoader = dynamic_cast<McRegionChunkLoader*>(chunkLoader);
				if (regionLoader != nullptr)
				{
					ChunkLoadStatus loadStatus = ChunkLoadStatus::ReadError;
					chunk = regionLoader->loadChunkFromData(worldObj, result.x, result.z, result.data, &loadStatus);
					if (chunk != nullptr)
						chunk->lastSaveTime = currentWorldTime();
					else if (loadStatus == ChunkLoadStatus::ReadError)
						chunk = blankChunk;
				}
				break;
			}
			case ChunkGenerationScheduler::ResultKind::LoadedRoot:
			{
				AnvilChunkLoader* anvilLoader = dynamic_cast<AnvilChunkLoader*>(chunkLoader);
				ChunkLoadStatus loadStatus = ChunkLoadStatus::ReadError;
#if PLATFORM_PROFILE_STREAMING || MC_LOG_LEVEL > 2
				const long_t buildStartNs = System::nanoTime();
#endif
				if (anvilLoader != nullptr)
					chunk = anvilLoader->loadChunkFromCompound(worldObj, result.x, result.z, result.nbt.get(), &loadStatus);
#if PLATFORM_PROFILE_STREAMING
				// "chunkLoad" stays the game-thread cost of bringing a saved
				// chunk in; the worker's read/inflate/parse is logged below.
				platformProfileChunkLoad(System::nanoTime() - buildStartNs);
#endif
#if MC_LOG_LEVEL > 2
				MC_LOG_DEBUG("chunk", "async load %d,%d worker=%.2fms build=%.2fms\n", result.x, result.z,
					result.workerNs / 1.0e6, (System::nanoTime() - buildStartNs) / 1.0e6);
#endif
				if (chunk != nullptr)
				{
					chunk->lastSaveTime = currentWorldTime();
				}
				else if (loadStatus == ChunkLoadStatus::ReadError)
				{
					MC_LOG_ERROR("chunk", "ChunkProvider: refusing to regenerate unreadable chunk %d,%d\n", result.x, result.z);
					chunk = blankChunk;
				}
				else
				{
					// Parsed but structurally empty (no Level/Sections): the
					// synchronous path regenerates it, so do the same.
					prepareChunk(result.x, result.z);
				}
				break;
			}
			case ChunkGenerationScheduler::ResultKind::LoadedChunk:
				chunk = result.chunk;
				result.chunk = nullptr;
				McRegionChunkLoader::attachChunkEntities(worldObj, chunk, result.nbt.get());
				chunk->lastSaveTime = currentWorldTime();
				break;
			case ChunkGenerationScheduler::ResultKind::GeneratedData:
			{
				if (ChunkProviderGenerate *generator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider))
					chunk = generator->finishAsyncChunkData(result.x, result.z, result.data);
				else if (ChunkProviderHell *nether = dynamic_cast<ChunkProviderHell *>(chunkProvider))
					chunk = nether->finishAsyncChunkData(result.x, result.z, result.data);
				break;
			}
			case ChunkGenerationScheduler::ResultKind::Generated:
				chunk = result.chunk;
				result.chunk = nullptr;
				break;
			case ChunkGenerationScheduler::ResultKind::Dropped:
				break;
			case ChunkGenerationScheduler::ResultKind::ReadError:
				MC_LOG_ERROR("chunk", "ChunkProvider: refusing to regenerate unreadable chunk %d,%d\n", result.x, result.z);
				chunk = blankChunk;
				break;
			}
		}

		if (chunk != nullptr)
		{
			published = true;
			chunkMap[key] = chunk;
			markChunkTopologyChanged();
			chunkList.push_back(chunk);
			chunk->lastAccessTick = currentWorldTime();
			if (chunk != blankChunk)
			{
				chunk->onChunkLoadData();
				chunk->onChunkLoad();
				notifyChunkPublished(chunk);
#if PLATFORM_DEFERRED_POPULATE
				const int_t westX = JavaArithmetic::intSub(result.x, 1);
				const int_t northZ = JavaArithmetic::intSub(result.z, 1);
				enqueuePopulate(result.x, result.z);
				enqueuePopulate(westX, result.z);
				enqueuePopulate(result.x, northZ);
				enqueuePopulate(westX, northZ);
#endif
			}
		}

		delete result.chunk;
		asyncRequestStale.erase(key);
		asyncGenerationScheduler->complete(result.x, result.z);
		asyncQueueFull = false;
	}
	return published;
}
#endif

void ChunkProvider::publishPreparedChunk(int_t i, int_t j, Chunk *chunk)
{
	if (chunk == nullptr)
		return;

	const std::uint64_t key = chunkKey(i, j);
#if PLATFORM_ASYNC_CHUNK_GENERATION
	markAsyncRequestStale(key);
#endif
	chunkMap[key] = chunk;
	markChunkTopologyChanged();
	chunkList.push_back(chunk);
	chunk->lastAccessTick = currentWorldTime();
	chunk->onChunkLoadData();
	chunk->onChunkLoad();

#if PLATFORM_BOUNDED_WORLD
	if (chunk != blankChunk)
		notifyChunkPublished(chunk);
#endif

	if (chunk == blankChunk)
		return;

	const int_t eastX = JavaArithmetic::intAdd(i, 1);
	const int_t westX = JavaArithmetic::intSub(i, 1);
	const int_t southZ = JavaArithmetic::intAdd(j, 1);
	const int_t northZ = JavaArithmetic::intSub(j, 1);
#if PLATFORM_DEFERRED_POPULATE
	const bool deferPopulate =
#if PLATFORM_PC_LEGACY
		worldObj == nullptr || !worldObj->findingSpawnPoint;
#else
		true;
#endif
	if (deferPopulate)
	{
		enqueuePopulate(i, j);
		enqueuePopulate(westX, j);
		enqueuePopulate(i, northZ);
		enqueuePopulate(westX, northZ);
		return;
	}
#endif

	if (!chunk->isTerrainPopulated
		&& chunkExists(eastX, southZ)
		&& chunkExists(i, southZ)
		&& chunkExists(eastX, j))
	{
		populate(this, i, j);
	}
	if (chunkExists(westX, j) && !provideChunk(westX, j)->isTerrainPopulated
		&& chunkExists(westX, southZ)
		&& chunkExists(i, southZ)
		&& chunkExists(westX, j))
	{
		populate(this, westX, j);
	}
	if (chunkExists(i, northZ) && !provideChunk(i, northZ)->isTerrainPopulated
		&& chunkExists(eastX, northZ)
		&& chunkExists(i, northZ)
		&& chunkExists(eastX, j))
	{
		populate(this, i, northZ);
	}
	if (chunkExists(westX, northZ) && !provideChunk(westX, northZ)->isTerrainPopulated
		&& chunkExists(westX, northZ)
		&& chunkExists(i, northZ)
		&& chunkExists(westX, j))
	{
		populate(this, westX, northZ);
	}
}

Chunk *ChunkProvider::prepareChunk(int_t i, int_t j)
{
	return prepareChunkInternal(i, j, false);
}

Chunk *ChunkProvider::prepareChunkInternal(int_t i, int_t j, bool deferGeneration)
{
#if !PLATFORM_INCREMENTAL_CHUNK_GENERATION
	(void)deferGeneration;
#endif
#if PLATFORM_BOUNDED_WORLD
	if (worldObj != nullptr && !worldObj->findingSpawnPoint && !canChunkExist(i, j))
		return blankChunk;
#endif
	const std::uint64_t key = chunkKey(i, j);
	droppedChunksSet.erase(key);

	auto it = chunkMap.find(key);
	Chunk *chunk = (it != chunkMap.end()) ? it->second : nullptr;
	if (chunk != nullptr)
	{
		chunk->lastAccessTick = currentWorldTime();
		return chunk;
	}

	WORLD_LOAD_STAGE("prepareChunk");
	bool readFailed = false;
#if PLATFORM_PROFILE_STREAMING
	const long_t chunkLoadStartNs = System::nanoTime();
#endif
	WorldLoadTrace::step("loadChunkFromFile");
	chunk = loadChunkFromFile(i, j, readFailed);
#if PLATFORM_PROFILE_STREAMING
	platformProfileChunkLoad(System::nanoTime() - chunkLoadStartNs);
#endif
	if (chunk == nullptr && readFailed)
	{
		MC_LOG_ERROR("chunk", "ChunkProvider: refusing to regenerate unreadable chunk %d,%d\n", i, j);
		chunk = blankChunk;
	}
	else if (chunk == nullptr)
	{
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
		if (deferGeneration)
		{
			enqueueGeneration(i, j);
			return blankChunk;
		}
#endif
		if (chunkProvider == nullptr)
		{
			chunk = blankChunk;
		}
		else
		{
#if PLATFORM_PROFILE_STREAMING
			const long_t generateStartNs = System::nanoTime();
#endif
			WorldLoadTrace::step("generate");
			chunk = chunkProvider->provideChunk(i, j);
#if PLATFORM_PROFILE_STREAMING
			platformProfileGenerate(System::nanoTime() - generateStartNs);
#endif
		}
	}
	if (chunk == nullptr)
		chunk = blankChunk;

	publishPreparedChunk(i, j, chunk);
	return chunk;
}

#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
void ChunkProvider::enqueueGeneration(int_t i, int_t j)
{
	const std::uint64_t key = chunkKey(i, j);
	if (generationQueued.insert(key).second)
		generationQueue.emplace_back(i, j);
}

void ChunkProvider::cancelQueuedGeneration(int_t i, int_t j)
{
	const std::uint64_t key = chunkKey(i, j);
	generationQueued.erase(key);
	generationQueue.erase(
		std::remove_if(generationQueue.begin(), generationQueue.end(),
			[i, j](const std::pair<int_t, int_t> &coord)
			{
				return coord.first == i && coord.second == j;
			}),
		generationQueue.end());

	ChunkProviderGenerate *generator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider);
	if (generator != nullptr && generator->hasGenerationTask()
		&& generator->generationTaskX() == i && generator->generationTaskZ() == j)
	{
		generator->cancelGenerationTask();
	}
}

int_t ChunkProvider::countLoadedGenerationNeighbours(int_t i, int_t j) const
{
	int_t loaded = 0;
	for (int_t dx = -1; dx <= 1; ++dx)
	{
		for (int_t dz = -1; dz <= 1; ++dz)
		{
			if (dx == 0 && dz == 0)
				continue;

			const int_t neighbourX = JavaArithmetic::intAdd(i, dx);
			const int_t neighbourZ = JavaArithmetic::intAdd(j, dz);
			auto it = chunkMap.find(chunkKey(neighbourX, neighbourZ));
			if (it != chunkMap.end() && it->second != nullptr && it->second != blankChunk)
				++loaded;
		}
	}
	return loaded;
}

std::deque<std::pair<int_t, int_t>>::iterator ChunkProvider::selectNextGenerationCoordinate()
{
	auto best = generationQueue.begin();
	if (best == generationQueue.end())
		return best;

	auto chebyshevDistance = [this](const std::pair<int_t, int_t> &coord)
	{
		long_t dx = static_cast<long_t>(coord.first) - static_cast<long_t>(curChunkX);
		long_t dz = static_cast<long_t>(coord.second) - static_cast<long_t>(curChunkZ);
		if (dx < 0) dx = -dx;
		if (dz < 0) dz = -dz;
		return dx > dz ? dx : dz;
	};

	auto movementAlignment = [this](const std::pair<int_t, int_t> &coord)
	{
		const long_t dx = static_cast<long_t>(coord.first) - static_cast<long_t>(curChunkX);
		const long_t dz = static_cast<long_t>(coord.second) - static_cast<long_t>(curChunkZ);
		return dx * static_cast<long_t>(generationMoveX)
		     + dz * static_cast<long_t>(generationMoveZ);
	};

	long_t bestDistance = chebyshevDistance(*best);
	for (auto it = generationQueue.begin() + 1; it != generationQueue.end(); ++it)
	{
		const long_t distance = chebyshevDistance(*it);
		if (distance < bestDistance)
		{
			best = it;
			bestDistance = distance;
		}
	}

	int_t bestNeighbours = countLoadedGenerationNeighbours(best->first, best->second);
	long_t bestAlignment = movementAlignment(*best);
	for (auto it = generationQueue.begin(); it != generationQueue.end(); ++it)
	{
		if (it == best || chebyshevDistance(*it) != bestDistance)
			continue;

		const int_t neighbours = countLoadedGenerationNeighbours(it->first, it->second);
		if (neighbours < bestNeighbours)
			continue;

		const long_t alignment = movementAlignment(*it);
		if (neighbours > bestNeighbours || alignment > bestAlignment)
		{
			best = it;
			bestNeighbours = neighbours;
			bestAlignment = alignment;
		}
	}

	return best;
}

bool ChunkProvider::drainPendingGeneration(int_t stepBudget, bool &publishedChunk)
{
	return drainPendingGeneration(stepBudget,
		static_cast<long_t>(PLATFORM_GENERATION_BUDGET_US) * 1000LL, publishedChunk);
}

void ChunkProvider::serviceFrameGeneration()
{
	if (PLATFORM_GENERATION_STEPS_PER_FRAME <= 0)
		return;
	bool publishedChunk = false;
	drainPendingGeneration(PLATFORM_GENERATION_STEPS_PER_FRAME,
		static_cast<long_t>(PLATFORM_GENERATION_FRAME_BUDGET_US) * 1000LL, publishedChunk);
}

bool ChunkProvider::drainPendingGeneration(int_t stepBudget, long_t budgetNs, bool &publishedChunk)
{
	// publishedChunk reports the subset of the work below that actually adds a
	// chunk to chunkMap, which is a different question from the return value.
	// See the decoration gate in unload100OldestChunks().
	publishedChunk = false;
	if (stepBudget <= 0)
		return false;

	ChunkProviderGenerate *generator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider);
	if (generator == nullptr)
		return false;

	bool didWork = false;
	int_t steps = 0;
	// Both slices (tick and frame) come through here, so the shared frame
	// allowance is applied once, in one place.
	PlatformStreamingFrameBudgetScope frameBudgetScope;
	budgetNs = PlatformStreamingFrameBudget::clampUs(budgetNs / 1000LL) * 1000LL;
	const long_t budgetStartNs = budgetNs > 0 ? System::nanoTime() : 0;

	while (steps < stepBudget)
	{
		if (!generator->hasGenerationTask())
		{
			bool started = false;
			while (!generationQueue.empty())
			{
				auto coordIt = selectNextGenerationCoordinate();
				if (coordIt == generationQueue.end())
					break;
				const std::pair<int_t, int_t> coord = *coordIt;
				generationQueue.erase(coordIt);
				const std::uint64_t key = chunkKey(coord.first, coord.second);
				if (generationQueued.count(key) == 0)
					continue;

				const bool wanted = chunkMap.count(key) == 0
					&& (worldObj == nullptr || worldObj->findingSpawnPoint
						|| canChunkExist(coord.first, coord.second));
				if (!wanted)
				{
					generationQueued.erase(key);
					continue;
				}

				started = generator->beginGenerationTask(coord.first, coord.second);
				if (started)
					break;
				generationQueue.emplace_back(coord);
				return didWork;
			}
			if (!started)
				break;
		}

		const int_t taskX = generator->generationTaskX();
		const int_t taskZ = generator->generationTaskZ();
		const std::uint64_t key = chunkKey(taskX, taskZ);
		const bool wanted = chunkMap.count(key) == 0
			&& (worldObj == nullptr || worldObj->findingSpawnPoint || canChunkExist(taskX, taskZ));
		if (!wanted)
		{
			generator->cancelGenerationTask();
			generationQueued.erase(key);
			continue;
		}

		if (!generator->advanceGenerationTask())
			break;
		didWork = true;
		++steps;

		Chunk *completed = generator->takeGeneratedChunk();
		if (completed != nullptr)
		{
			generationQueued.erase(key);
			const bool stillWanted = chunkMap.count(key) == 0
				&& (worldObj == nullptr || worldObj->findingSpawnPoint || canChunkExist(taskX, taskZ));
			if (stillWanted)
			{
				publishPreparedChunk(taskX, taskZ, completed);
				publishedChunk = true;
			}
			else
			{
				delete completed;
			}
		}

		if (budgetNs > 0 && System::nanoTime() - budgetStartNs >= budgetNs)
			break;
	}
	return didWork;
}

bool ChunkProvider::isChunkGenerationPending(int_t i, int_t j) const
{
	return generationQueued.count(chunkKey(i, j)) != 0;
}
#endif

Chunk *ChunkProvider::provideChunk(int_t i, int_t j)
{
	if (worldObj != nullptr && worldObj->isLimitedWorld())
	{
		const int_t minChunk = worldObj->getWorldInfo() != nullptr ? worldObj->getWorldInfo()->getLimitedWorldMinChunk() : -8;
		const int_t maxChunk = worldObj->getWorldInfo() != nullptr ? worldObj->getWorldInfo()->getLimitedWorldMaxChunk() : 7;
		if (i < minChunk || i > maxChunk || j < minChunk || j > maxChunk)
			return blankChunk;
	}
#if PLATFORM_BOUNDED_WORLD
	if (worldObj != nullptr && !worldObj->findingSpawnPoint && !canChunkExist(i, j))
		return blankChunk;
#endif
	// Fast path: same chunk as the previous lookup. Skips the chunkMap hash
	// lookup entirely and, same as before, does not bother updating the access
	// timestamp here -- a chunk only reached through this path is the one the
	// player is standing in, always inside the unload radius regardless of how
	// stale lastAccessTick is. Last-access tracking lives on the Chunk itself
	// now (see Chunk::lastAccessTick), not a side map keyed by chunk coordinate.
	if (lastChunk != nullptr && i == lastChunkX && j == lastChunkZ)
		return lastChunk;

	const std::uint64_t key = chunkKey(i, j);
	Chunk *resident;
	if (!findResidentChunk(i, j, resident))
	{
#if PLATFORM_ASYNC_CHUNK_GENERATION && PLATFORM_PC_LEGACY
		if (worldObj == nullptr || !worldObj->findingSpawnPoint)
		{
			long_t dcx = static_cast<long_t>(i) - static_cast<long_t>(curChunkX);
			long_t dcz = static_cast<long_t>(j) - static_cast<long_t>(curChunkZ);
			if (dcx < 0) dcx = -dcx;
			if (dcz < 0) dcz = -dcz;
			const long_t cheb = dcx > dcz ? dcx : dcz;
			const bool critical = cheb <= PLATFORM_GENERATE_SYNC_RADIUS;
			if (!critical && asyncGenerationScheduler != nullptr && asyncGenerationScheduler->active())
			{
				const ChunkRequestStatus requestStatus = requestChunkDetailed(i, j);
				if (requestStatus == ChunkRequestStatus::Accepted ||
					requestStatus == ChunkRequestStatus::AlreadyQueued ||
					requestStatus == ChunkRequestStatus::QueueFull)
					return blankChunk;
			}
		}
#endif
#if PLATFORM_BOUNDED_WORLD && PLATFORM_GENERATE_CHUNKS_PER_TICK > 0
		if (worldObj == nullptr || !worldObj->findingSpawnPoint)
		{
			long_t dcx = static_cast<long_t>(i) - static_cast<long_t>(curChunkX);
			long_t dcz = static_cast<long_t>(j) - static_cast<long_t>(curChunkZ);
			if (dcx < 0) dcx = -dcx;
			if (dcz < 0) dcz = -dcz;
			const long_t cheb = dcx > dcz ? dcx : dcz;
			bool critical = cheb <= PLATFORM_GENERATE_SYNC_RADIUS;
#if PLATFORM_ENTITY_CHUNK_RETENTION
			critical = critical || (worldObj != nullptr && worldObj->isChunkRequiredByRetainedEntity(i, j));
#endif
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
			ChunkProviderGenerate *incrementalGenerator = dynamic_cast<ChunkProviderGenerate *>(chunkProvider);
			if (incrementalGenerator != nullptr)
			{
				if (critical)
				{
					cancelQueuedGeneration(i, j);
				}
				else
				{
					if (generationQueued.count(key) != 0)
						return blankChunk;
					return prepareChunkInternal(i, j, true);
				}
			}
#endif
#if PLATFORM_ASYNC_CHUNK_GENERATION
			// Non-critical terrain can be queued on a low-priority generation service.
			// Minecraft keeps rendering the current frame until the completed chunk is
			// published on a later tick.
			if (!critical && asyncGenerationScheduler != nullptr && asyncGenerationScheduler->active())
			{
				const ChunkRequestStatus requestStatus = requestChunkDetailed(i, j);
				if (requestStatus == ChunkRequestStatus::Accepted ||
					requestStatus == ChunkRequestStatus::AlreadyQueued)
					return blankChunk;
			}
#endif
			if (!critical && genChunksThisTick >= PLATFORM_GENERATE_CHUNKS_PER_TICK)
				return blankChunk;
			genChunksThisTick++;
		}
#endif
		return prepareChunk(i, j);
	}

	if (resident != nullptr)
		resident->lastAccessTick = currentWorldTime();
	if (resident != nullptr && resident != blankChunk)
	{
		lastChunk  = resident;
		lastChunkX = i;
		lastChunkZ = j;
	}
	return resident;
}

Chunk *ChunkProvider::loadChunkFromFile(int_t i, int_t j, bool &readFailed)
{
	readFailed = false;
	if (chunkLoader == nullptr) return nullptr;
	try
	{
		ChunkLoadStatus status = ChunkLoadStatus::Missing;
		Chunk *chunk = chunkLoader->loadChunk(worldObj, i, j, &status);
		readFailed = status == ChunkLoadStatus::ReadError;
		if (chunk != nullptr)
			chunk->lastSaveTime = worldObj->getWorldTime();
		return chunk;
	}
	catch (...)
	{
		readFailed = true;
		MC_LOG_ERROR("chunk", "ChunkProvider::loadChunkFromFile - exception loading %d,%d\n", i, j);
	}
	return nullptr;
}

void ChunkProvider::saveExtraChunkData(Chunk *chunk)
{
	if (chunkLoader == nullptr || chunk == nullptr || chunk == blankChunk) return;
	try
	{
		chunkLoader->saveExtraChunkData(worldObj, chunk);
	}
	catch (...)
	{
		MC_LOG_ERROR("chunk", "ChunkProvider::saveExtraChunkData - exception\n");
	}
}

void ChunkProvider::saveChunkToFile(Chunk *chunk)
{
	if (chunkLoader == nullptr || chunk == nullptr || chunk == blankChunk) return;
	try
	{
		chunk->lastSaveTime = worldObj->getWorldTime();
		chunkLoader->saveChunk(worldObj, chunk);
	}
	catch (...)
	{
		MC_LOG_ERROR("chunk", "ChunkProvider::saveChunkToFile - exception\n");
	}
}

void ChunkProvider::unloadChunk(std::uint64_t key, Chunk *chunk)
{
	if (chunk == nullptr || chunk == blankChunk)
		return;
#if PLATFORM_ASYNC_CHUNK_GENERATION
	// A queued request for this coordinate was started before this chunk's
	// (possibly edited) state is saved below.
	markAsyncRequestStale(key);
#else
	(void)key;
#endif

	// Drop the single-entry cache if it points at the chunk being freed.
	if (lastChunk == chunk)
		lastChunk = nullptr;

#if PLATFORM_SAVE_RUNTIME_CHUNK_EDITS_ON_UNLOAD
	// PS2 serializes only gameplay-edited chunks here. Generated/lighting-only
	// dirtiness stays memory-only so walking does not create continuous writes.
	if (!chunk->neverSave && chunk->isRuntimeSaveRequired())
	{
#if PLATFORM_PROFILE_STREAMING
		const long_t unloadSaveStartNs = System::nanoTime();
#endif
		saveChunkToFile(chunk);
		chunk->isModified = false;
		chunk->clearRuntimeSaveRequired();
		saveExtraChunkData(chunk);
#if PLATFORM_PROFILE_STREAMING
		platformProfileUnloadSave(System::nanoTime() - unloadSaveStartNs);
#endif
	}
#elif !PLATFORM_CONSOLE_LOW
	// Unloading must not force a disk write for clean chunks. The v10 cache
	// optimization saved every chunk on eviction, which caused heavy IO spikes
	// while walking. Save only chunks that Java would consider dirty/stale.
	if (!chunk->neverSave && chunk->needsSaving(false))
	{
#if PLATFORM_PROFILE_STREAMING
		const long_t unloadSaveStartNs = System::nanoTime();
#endif
		saveChunkToFile(chunk);
		chunk->isModified = false;
		chunk->clearRuntimeSaveRequired();
		saveExtraChunkData(chunk);
#if PLATFORM_PROFILE_STREAMING
		platformProfileUnloadSave(System::nanoTime() - unloadSaveStartNs);
#endif
	}
#endif

	chunk->onChunkUnload();
	// Last, right before the free: saving and onChunkUnload can look this
	// chunk up again (it is still in chunkMap) and re-arm either cache with it.
	if (lastChunk == chunk)
		lastChunk = nullptr;
	for (ResidentSlot &slot : residentSlots)
		if (slot.chunk == chunk)
			slot.version = 0;
	delete chunk;
}

bool ChunkProvider::isInPopulateFootprint(int_t i, int_t j) const
{
#if PLATFORM_DEFERRED_POPULATE
	if (!populateFrontInProgress || populateQueue.empty())
		return false;
	const int_t dx = i - populateQueue.front().first;
	const int_t dz = j - populateQueue.front().second;
	return dx >= 0 && dx <= 1 && dz >= 0 && dz <= 1;
#else
	(void)i; (void)j;
	return false;
#endif
}

bool ChunkProvider::isChunkPopulationPending(int_t i, int_t j) const
{
#if PLATFORM_DEFERRED_POPULATE
	return populateQueued.find(chunkKey(i, j)) != populateQueued.end();
#else
	(void)i;
	(void)j;
	return false;
#endif
}

void ChunkProvider::populate(IChunkProvider *ichunkprovider, int_t i, int_t j)
{
#if PLATFORM_DEFERRED_POPULATE
	while (!populateDeferredStep(i, j))
	{
	}
#else
	Chunk *chunk = provideChunk(i, j);
	if (chunk != nullptr && chunk != blankChunk && !chunk->isTerrainPopulated)
	{
		chunk->isTerrainPopulated = true;
		if (chunkProvider != nullptr)
		{
			chunkProvider->populate(ichunkprovider, i, j);
			chunk->setChunkModified();
		}
	}
#endif
}

#if PLATFORM_DEFERRED_POPULATE
namespace
{
// Light flush, skylight regeneration and dirty marking around each batch.
long_t g_populateOverheadNs = 0;
}

bool ChunkProvider::populateDeferredBatch(int_t i, int_t j, int_t maxSteps, long_t deadlineNs, int_t &stepsRun)
{
	stepsRun = 0;
	Chunk *chunk = provideChunk(i, j);
	if (chunk == nullptr || chunk == blankChunk || chunk->isTerrainPopulated)
		return true;
	if (chunkProvider == nullptr)
	{
		chunk->isTerrainPopulated = true;
		stepsRun = 1;
		return true;
	}

	Chunk *populationChunks[POPULATION_FOOTPRINT_AXIS * POPULATION_FOOTPRINT_AXIS] = {};
	std::uint32_t before[POPULATION_FOOTPRINT_AXIS * POPULATION_FOOTPRINT_AXIS]
	                    [POPULATION_SECTION_COUNT] = {};
	for (int_t dz = 0; dz < POPULATION_FOOTPRINT_AXIS; ++dz)
	{
		for (int_t dx = 0; dx < POPULATION_FOOTPRINT_AXIS; ++dx)
		{
			const int_t footprintIndex = dz * POPULATION_FOOTPRINT_AXIS + dx;
			Chunk *footprintChunk = getLoadedChunk(JavaArithmetic::intAdd(i, dx), JavaArithmetic::intAdd(j, dz));
			populationChunks[footprintIndex] = footprintChunk;
			for (int_t sectionY = 0; sectionY < POPULATION_SECTION_COUNT; ++sectionY)
			{
				before[footprintIndex][sectionY] = footprintChunk != nullptr
					? footprintChunk->getBlockSectionRevision(sectionY)
					: 0u;
			}
		}
	}

	const long_t setupStartNs = System::nanoTime();
	if (worldObj != nullptr)
		worldObj->beginPopulationFastPath(i, j);
	g_populateOverheadNs += System::nanoTime() - setupStartNs;

	bool complete = false;
	try
	{
		while (stepsRun < maxSteps && !complete)
		{
			complete = chunkProvider->populateStep(this, i, j);
			++stepsRun;
			if (deadlineNs > 0 && System::nanoTime() >= deadlineNs)
				break;
		}
	}
	catch (...)
	{
		if (worldObj != nullptr)
			worldObj->endPopulationFastPath();
		throw;
	}

	const long_t flushStartNs = System::nanoTime();
	if (worldObj != nullptr)
		worldObj->endPopulationFastPath();

	struct FlushTimer
	{
		long_t start;
		~FlushTimer() { g_populateOverheadNs += System::nanoTime() - start; }
	} flushTimer{flushStartNs};
	if (worldObj != nullptr)
	{
		for (int_t footprintIndex = 0;
			 footprintIndex < POPULATION_FOOTPRINT_AXIS * POPULATION_FOOTPRINT_AXIS;
			 ++footprintIndex)
		{
			Chunk *footprintChunk = populationChunks[footprintIndex];
			if (footprintChunk == nullptr || footprintChunk == blankChunk)
				continue;

			for (int_t sectionY = 0; sectionY < POPULATION_SECTION_COUNT; ++sectionY)
			{
				if (before[footprintIndex][sectionY] ==
					footprintChunk->getBlockSectionRevision(sectionY))
					continue;

				const int_t minX = JavaArithmetic::intMul(footprintChunk->xPosition, 16);
				const int_t minY = sectionY << 4;
				const int_t minZ = JavaArithmetic::intMul(footprintChunk->zPosition, 16);
				worldObj->markBlocksDirty(minX, minY, minZ,
				                          JavaArithmetic::intAdd(minX, 15), minY + 15,
				                          JavaArithmetic::intAdd(minZ, 15));
			}
		}
	}

	if (complete)
	{
		chunk->isTerrainPopulated = true;
		chunk->setChunkModified();
	}
	return complete;
}

bool ChunkProvider::populateDeferredStep(int_t i, int_t j)
{
	int_t stepsRun = 0;
	return populateDeferredBatch(i, j, 1, 0, stepsRun);
}

Chunk *ChunkProvider::getLoadedChunk(int_t i, int_t j)
{
	auto it = chunkMap.find(chunkKey(i, j));
	return it != chunkMap.end() ? it->second : nullptr;
}

bool ChunkProvider::canPopulateChunk(int_t i, int_t j)
{
	// Read straight from the map -- never call provideChunk() here, or the gate
	// would force-generate the very neighbours we are only meant to test for.
	Chunk *c = getLoadedChunk(i, j);
	if (c == nullptr || c == blankChunk || c->isTerrainPopulated)
		return false;
	// Decoration writes up to +8 blocks into the +x/+z neighbours, so they (and
	// the diagonal) must already exist. Same invariant the inline path enforced.
	const int_t eastX = JavaArithmetic::intAdd(i, 1);
	const int_t southZ = JavaArithmetic::intAdd(j, 1);
	return chunkExists(eastX, j)
		&& chunkExists(i, southZ)
		&& chunkExists(eastX, southZ);
}

void ChunkProvider::enqueuePopulate(int_t i, int_t j)
{
#if PLATFORM_POPULATE_CHUNKS_PER_TICK <= 0
	// Deferred decoration is disabled on this platform; do not let the queue grow
	// (drainPendingPopulate never runs, so entries would accumulate forever).
	(void)i; (void)j;
	return;
#else
	Chunk *c = getLoadedChunk(i, j);
	if (c == nullptr || c == blankChunk || c->isTerrainPopulated)
		return;
	const std::uint64_t key = chunkKey(i, j);
	if (populateQueued.insert(key).second)
		populateQueue.emplace_back(i, j);
		++g_chunkPopulationPendingRevision;
	}
#endif
}

namespace
{
// Decoration throughput, logged every 10 s while there is work.
struct PopulateStats
{
	long_t since = 0;
	long_t spentNs = 0;
	int_t completed = 0;

	void add(long_t ns, int_t done, std::size_t queued, ChunkProvider *provider)
	{
		spentNs += ns;
		completed += done;
		const long_t now = System::nanoTime();
		if (since == 0)
			since = now;
		if (now - since < 10000000000LL)
			return;
		const double perChunk = completed > 0 ? 1.0e6 * completed : 1.0e6;
		auto phase = [perChunk](PlatformPopulatePhase p)
		{ return ChunkProviderGenerate::takePopulatePhaseNs(static_cast<int>(p)) / perChunk; };
		const double structures = phase(PlatformPopulatePhase::Structures);
		const double lakes = phase(PlatformPopulatePhase::Lakes);
		const double dungeons = phase(PlatformPopulatePhase::Dungeons);
		const double decorate = phase(PlatformPopulatePhase::Decoration);
		const double animals = phase(PlatformPopulatePhase::Total);
		const double snow = phase(PlatformPopulatePhase::Snow);
		const double flush = g_populateOverheadNs / perChunk;
		g_populateOverheadNs = 0;
		char slowest[128];
		BiomeDecorator::takeSlowestStages(slowest, sizeof(slowest), perChunk);
#if PLATFORM_ASYNC_CHUNK_GENERATION
		char streaming[192];
		provider->describeStreaming(streaming, sizeof(streaming));
		MC_LOG_INFO("chunk", "streaming: %d chunks resident, %s\n", (int)provider->residentChunkCount(), streaming);
#endif
		MC_LOG_INFO("chunk", "decoration: %d chunks in %.1f s, %.1f ms of game thread each, %d queued "
			"(structures %.1f lakes %.1f dungeons %.1f decorator %.1f animals %.1f snow %.1f light %.1f; slowest:%s)\n",
			(int)completed, (now - since) / 1.0e9, completed > 0 ? spentNs / 1.0e6 / completed : 0.0, (int)queued,
			structures, lakes, dungeons, decorate, animals, snow, flush, slowest);
		since = now;
		spentNs = 0;
		completed = 0;
	}
} g_populateStats;
}

bool ChunkProvider::preloadWithWorkers(int_t centerX, int_t centerZ, int_t radius, IProgressUpdate *progress)
{
#if PLATFORM_ASYNC_CHUNK_GENERATION && PLATFORM_DEFERRED_POPULATE
	if (asyncGenerationScheduler == nullptr || !asyncGenerationScheduler->active() || radius < 0)
		return false;
	asyncGenerationScheduler->setFocus(centerX, centerZ);

	// Nearest first, so the player's surroundings are the first ready.
	std::vector<std::pair<int_t, int_t>> columns;
	for (int_t dz = -radius; dz <= radius; ++dz)
		for (int_t dx = -radius; dx <= radius; ++dx)
			columns.emplace_back(centerX + dx, centerZ + dz);
	std::stable_sort(columns.begin(), columns.end(),
		[centerX, centerZ](const std::pair<int_t, int_t> &a, const std::pair<int_t, int_t> &b)
		{
			const long_t da = std::max(std::abs((long_t)a.first - centerX), std::abs((long_t)a.second - centerZ));
			const long_t db = std::max(std::abs((long_t)b.first - centerX), std::abs((long_t)b.second - centerZ));
			return da < db;
		});

	const std::size_t total = columns.size();
	std::size_t next = 0;
	const long_t startNs = System::nanoTime();
	long_t decorateDuringNs = 0;
	for (;;)
	{
		while (next < total)
		{
			const std::pair<int_t, int_t> &column = columns[next];
			const ChunkRequestStatus status = requestChunkDetailed(column.first, column.second);
			if (status == ChunkRequestStatus::QueueFull)
				break;
			if (status == ChunkRequestStatus::OutOfRange || status == ChunkRequestStatus::SavedOnDisk ||
				status == ChunkRequestStatus::Inactive)
				prepareChunk(column.first, column.second);
			++next;
		}
		drainAsyncGenerationRequests(PLATFORM_ASYNC_GENERATION_QUEUE_LIMIT);
		const bool published = drainAsyncGeneratedChunks(PLATFORM_ASYNC_GENERATION_QUEUE_LIMIT);

		std::size_t present = 0;
		for (const std::pair<int_t, int_t> &column : columns)
			present += chunkMap.count(chunkKey(column.first, column.second));
		if (progress != nullptr)
			progress->setLoadingProgress(static_cast<int_t>(present * 100 / (total == 0 ? 1 : total)));
		if (present == total && next >= total)
			break;
		// A dropped request leaves its column missing: ask again (present and
		// claimed columns answer at once).
		if (next >= total)
			next = 0;
		// While the workers generate, decorate what already can be.
		if (!published && !populateQueue.empty())
		{
			const long_t decorateStartNs = System::nanoTime();
			drainPendingPopulate(PLATFORM_POPULATE_STEPS_PER_TICK);
			decorateDuringNs += System::nanoTime() - decorateStartNs;
		}
		else if (!published)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	const long_t columnsDoneNs = System::nanoTime();
	// Then everything that can be decorated (columns at the edge wait for
	// neighbours outside the square, as in play).
	for (std::size_t guard = 0; guard < 100000 && !populateQueue.empty(); ++guard)
	{
		const std::size_t before = populateQueue.size();
		drainPendingPopulate(PLATFORM_POPULATE_STEPS_PER_TICK);
		if (populateQueue.size() == before && !populateFrontInProgress)
			break;
	}
	MC_LOG_INFO("chunk", "preload: %d columns in %.1f s (decorating meanwhile %.1f s), then decoration %.1f s\n",
		(int)total, (columnsDoneNs - startNs) / 1.0e9, decorateDuringNs / 1.0e9,
		(System::nanoTime() - columnsDoneNs) / 1.0e9);
	return true;
#else
	(void)centerX; (void)centerZ; (void)radius; (void)progress;
	return false;
#endif
}

void ChunkProvider::serviceFramePopulate()
{
#if PLATFORM_DEFERRED_POPULATE && defined(PLATFORM_POPULATE_FRAME_BUDGET_US)
	drainPendingPopulate(PLATFORM_POPULATE_STEPS_PER_TICK, true);
#endif
}

void ChunkProvider::drainPendingPopulate(int_t budget, bool frameSlice)
{
	if (budget <= 0)
		return;
	const long_t statStartNs = System::nanoTime();
	int_t statCompleted = 0;
	struct StatScope
	{
		const long_t &start;
		int_t &done;
		std::deque<std::pair<int_t, int_t>> &queue;
		ChunkProvider *provider;
		~StatScope() { g_populateStats.add(System::nanoTime() - start, done, queue.size(), provider); }
	} statScope{statStartNs, statCompleted, populateQueue, this};
	if (populateQueue.empty())
		return;
	// Bound the scan to the current queue length so dropping not-yet-ready entries
	// cannot spin. A dropped chunk is re-enqueued when its last missing neighbour
	// is generated (that prepareChunk enqueues this chunk as one of its four).
	int_t scan = (int_t)populateQueue.size();
	int_t steps = 0;
	long_t populateBudgetUs = (long_t)PLATFORM_POPULATE_BUDGET_US;
#ifdef PLATFORM_POPULATE_NEAR_RADIUS
	// Nearest column first: arrival order put far columns ahead of the ones
	// around the player, which stayed bare for seconds. One linear scan when a
	// new column starts; sorting the whole queue (hundreds) every tick cost
	// about as much as the decoration it ordered.
	auto distance = [this](const std::pair<int_t, int_t> &coord)
	{
		const long_t dx = std::abs(static_cast<long_t>(coord.first) - static_cast<long_t>(curChunkX));
		const long_t dz = std::abs(static_cast<long_t>(coord.second) - static_cast<long_t>(curChunkZ));
		return dx > dz ? dx : dz;
	};
	int_t pullsLeft = 8;
	auto pullNearestToFront = [&]()
	{
		if (populateFrontInProgress || populateQueue.size() < 2 || pullsLeft-- <= 0)
			return;
		auto best = populateQueue.begin();
		long_t bestDistance = distance(*best);
		for (auto it = best + 1; it != populateQueue.end(); ++it)
		{
			const long_t d = distance(*it);
			if (d < bestDistance)
			{
				best = it;
				bestDistance = d;
			}
		}
		std::iter_swap(populateQueue.begin(), best);
	};
	pullNearestToFront();
	if (!populateQueue.empty() && distance(populateQueue.front()) <= PLATFORM_POPULATE_NEAR_RADIUS
		&& canPopulateChunk(populateQueue.front().first, populateQueue.front().second))
	{
		populateBudgetUs = PLATFORM_POPULATE_NEAR_BUDGET_US;
		budget = std::max<int_t>(budget, PLATFORM_POPULATE_STEPS_PER_TICK);
	}
#ifdef PLATFORM_POPULATE_FRAME_BUDGET_US
	if (frameSlice)
	{
		// Only while something next to the player waits; a half-done far
		// column at the front still runs, since it blocks the near ones.
		bool nearWaiting = false;
		for (const std::pair<int_t, int_t> &coord : populateQueue)
			if (distance(coord) <= PLATFORM_POPULATE_NEAR_RADIUS && canPopulateChunk(coord.first, coord.second))
			{
				nearWaiting = true;
				break;
			}
		if (!nearWaiting)
			return;
		populateBudgetUs = PLATFORM_POPULATE_FRAME_BUDGET_US;
	}
#endif
#endif
#if PLATFORM_POPULATE_BUDGET_US > 0
	PlatformStreamingFrameBudgetScope frameBudgetScope;
	const long_t budgetStartNs = System::nanoTime();
	const long_t budgetNs =
		PlatformStreamingFrameBudget::clampUs(populateBudgetUs) * 1000LL;
	const long_t deadlineNs = budgetStartNs + budgetNs;
#else
	const long_t deadlineNs = 0;
#endif
	while (steps < budget && scan-- > 0 && !populateQueue.empty())
	{
#ifdef PLATFORM_POPULATE_NEAR_RADIUS
		pullNearestToFront();
#endif
		const std::pair<int_t, int_t> coord = populateQueue.front();
		populateQueue.pop_front();
		const std::uint64_t key = chunkKey(coord.first, coord.second);

		// Keep the key registered while the step runs. Population writes dirty
		// render sections before populateDeferredStep() returns, and the Legacy
		// renderer uses this set to coalesce those mutations instead of restarting
		// an active staging mesh. It also prevents recursive enqueue attempts from
		// creating a duplicate queue entry for the chunk currently being processed.
		if (!canPopulateChunk(coord.first, coord.second))
		{
			populateQueued.erase(key);
			++g_chunkPopulationPendingRevision;
			if (populateFrontInProgress)
			{
				// The half-decorated column lost its footprint (unloaded). The
				// generator would refuse every other column until it finished,
				// which stalled decoration for good. Drop the task and keep the
				// column from being decorated a second time on top.
				populateFrontInProgress = false;
				if (chunkProvider != nullptr)
					chunkProvider->abandonPopulateTask();
				Chunk *partial = getLoadedChunk(coord.first, coord.second);
				if (partial != nullptr && partial != blankChunk)
					partial->isTerrainPopulated = true;
			}
			continue;
		}

		int_t batchSteps = 0;
		const bool complete = populateDeferredBatch(
			coord.first, coord.second, budget - steps, deadlineNs, batchSteps);
		steps += batchSteps;
		populateFrontInProgress = !complete;
		if (!complete)
		{
			populateQueue.emplace_front(coord);
			scan++;
		}
		else
		{
			populateQueued.erase(key);
		}
#if PLATFORM_POPULATE_BUDGET_US > 0
		if (System::nanoTime() - budgetStartNs >= budgetNs)
			break;
#endif
		if (!complete)
			continue;
	}
}
#else
void ChunkProvider::serviceFramePopulate() {}
#endif

bool ChunkProvider::saveChunks(bool flag, IProgressUpdate *iprogressupdate)
{
	// Chunk persistence is region-file backed. Runtime autosaves call this with
	// flag=false from inside World::tick(), so keep that path deliberately small:
	// compressing and writing a large dirty batch synchronously produces a visible
	// libfat stall. Full/menu saves still drain every dirty chunk and flush the
	// RegionFile cache through saveExtraData() below.
	int_t saved = 0;
	int_t totalToSave = 0;
	if (iprogressupdate != nullptr)
	{
		for (Chunk *chunk : chunkList)
		{
			if (chunk != nullptr && chunk != blankChunk && chunk->needsSaving(flag))
				totalToSave++;
		}
	}

	int_t progress = 0;
	const long_t saveStartNs = System::nanoTime();
	long_t serializeNs = 0;
	long_t writerWaitNs = 0;
	for (Chunk *chunk : chunkList)
	{
		if (chunk == nullptr || chunk == blankChunk) continue;
		if (flag && !chunk->neverSave)
			saveExtraChunkData(chunk);
		if (!chunk->needsSaving(flag))
			continue;
		const long_t serializeStartNs = System::nanoTime();
		saveChunkToFile(chunk);
		serializeNs += System::nanoTime() - serializeStartNs;
		chunk->isModified = false;
		chunk->clearRuntimeSaveRequired();
		++saved;

		// Java can let the threaded writer queue grow because the desktop JVM has
		// a large GC heap.  In this C++ port every queued chunk owns a complete NBT
		// tree until the writer consumes it.  A new-world full save can otherwise
		// retain hundreds of chunk NBT trees at once and exhaust/fragment the heap
		// while the loading screen says "Saving chunks".  Drain in bounded batches
		// without changing which chunks are saved or the on-disk format.
#ifdef PLATFORM_FULL_SAVE_DRAIN_BATCH
		if (flag && (saved % PLATFORM_FULL_SAVE_DRAIN_BATCH) == 0)
#else
		if (flag && (saved % std::max<int_t>(1, PLATFORM_INCREMENTAL_CHUNK_SAVE_LIMIT)) == 0)
#endif
		{
			const long_t waitStartNs = System::nanoTime();
			ThreadedFileIOBase::threadedIOInstance.waitForFinish();
			writerWaitNs += System::nanoTime() - waitStartNs;
		}

		// Release 1.2.5 uses 24 here. Platform tuning may lower the batch on
		// storage-constrained consoles without changing desktop/parity behavior.
		if (saved == PLATFORM_INCREMENTAL_CHUNK_SAVE_LIMIT && !flag)
			return false;
		if (iprogressupdate != nullptr && totalToSave > 0 && ++progress % 10 == 0)
			iprogressupdate->setLoadingProgress((progress * 100) / totalToSave);
	}
	if (flag)
	{
		if (chunkLoader == nullptr) return true;
		const long_t flushStartNs = System::nanoTime();
		chunkLoader->saveExtraData();
		if (saved > 0)
		{
			long long io[4];
			RegionFile::takeWriteStats(io);
			MC_LOG_INFO("chunk", "full save: %d chunks in %.1f s (serialize %.1f s, waiting for the writer %.1f s, final flush %.1f s; "
				"writer: deflate %.1f s, data %.1f s, header %.1f s for %lld writes)\n",
				(int)saved, (System::nanoTime() - saveStartNs) / 1.0e9, serializeNs / 1.0e9, writerWaitNs / 1.0e9,
				(System::nanoTime() - flushStartNs) / 1.0e9, io[0] / 1.0e9, io[1] / 1.0e9, io[2] / 1.0e9, io[3]);
		}
	}
	return true;
}

bool ChunkProvider::unload100OldestChunks()
{
#if PLATFORM_DEFERRED_POPULATE
	bool publishedThisTick = false;
#endif
#if PLATFORM_BOUNDED_WORLD
	// Per-tick hook (World::tick calls this once). Refill the synchronous-generation
	// budget for the new tick. Deferred decoration has its own feature gate below
	// so unbounded low-end desktop profiles can reuse it without adopting the
	// console chunk-cache policy.
	genChunksThisTick = 0;
#if PLATFORM_INCREMENTAL_CHUNK_GENERATION
#if PLATFORM_PROFILE_STREAMING
	const long_t generationStartNs = System::nanoTime();
#endif
	bool incrementalPublishedChunk = false;
	const bool incrementalGenerationWorked =
		drainPendingGeneration(PLATFORM_GENERATION_STEPS_PER_TICK, incrementalPublishedChunk);
#if PLATFORM_PROFILE_STREAMING
	if (incrementalGenerationWorked)
		platformProfileGenerate(System::nanoTime() - generationStartNs);
#endif
#if PLATFORM_DEFERRED_POPULATE
	// Advancing a stage does NOT reduce the decoration budget, publishing does
	// -- the same rule the async block below documents at length, applied to the
	// incremental generator.
	//
	// drainPendingGeneration() reports work for any advanced stage, and a chunk
	// takes five to eight of them (ChunkProviderGenerate::advanceGenerationTask).
	// While the player explores the generation queue is never empty, so gating on
	// the old flag closed this branch on essentially every tick: decoration did
	// not slow down, it stopped, and chunks stayed terrain-complete and bare for
	// as long as the walk lasted.
	//
	// Publishing is the tick that actually adds the column to chunkMap and builds
	// its skylight, so that one decorates on the reduced budget instead. Every
	// other generation tick now pays at most one extra PLATFORM_POPULATE_BUDGET_US.
	publishedThisTick = incrementalPublishedChunk;
#endif
#endif
#endif
#if PLATFORM_ASYNC_CHUNK_GENERATION
	const bool asyncPublishedChunk = drainAsyncGeneratedChunks(PLATFORM_ASYNC_GENERATION_PUBLISH_PER_TICK);
	// Dispatching does NOT reduce the decoration budget, publishing does.
	//
	// ChunkGenerationScheduler::dispatch() moves a coordinate from the pending
	// queue to the worker's queue under a mutex and returns; the generation
	// itself runs on ChunkGenerationScheduler::runWorker(). Letting dispatch close
	// this gate costs the whole decoration budget of every tick that
	// queued a request -- while the player explores, nearly all of them. The
	// decorator is one feature per step (see BiomeDecorator's stage machine), so
	// starving it does not slow decoration down, it stops it: chunks stayed
	// terrain-complete and undecorated for as long as the player kept walking.
	// Trees are what you notice, because they sit late in that order, after the
	// ores, clay and sand that are underground or unremarkable.
	//
	// Publishing is a real generation frame -- drainAsyncGeneratedChunks() is
	// what adds the ~80 KB column to chunkMap and regenerates its skylight -- so
	// that one decorates on PLATFORM_POPULATE_STEPS_AFTER_PUBLISH instead of the
	// full budget. It is a smaller share, not a stand-down: once the worker is
	// fed at its queue limit a result is ready on nearly every tick, and a gate
	// that closes on every publish is a gate that never opens.
	drainAsyncGenerationRequests(PLATFORM_ASYNC_GENERATION_REQUESTS_PER_TICK);
#if PLATFORM_DEFERRED_POPULATE
	publishedThisTick = publishedThisTick || asyncPublishedChunk;
#endif
#endif

#if PLATFORM_DEFERRED_POPULATE
	{
		const int_t populateSteps = publishedThisTick
			? PLATFORM_POPULATE_STEPS_AFTER_PUBLISH
			: PLATFORM_POPULATE_STEPS_PER_TICK;
#if PLATFORM_PROFILE_STREAMING
		const long_t populateStartNs = System::nanoTime();
#endif
		// A budget of 0 returns immediately, which is what the platforms that
		// keep PLATFORM_POPULATE_STEPS_AFTER_PUBLISH at 0 rely on.
		drainPendingPopulate(populateSteps);
#if PLATFORM_PROFILE_STREAMING
		platformProfilePopulate(System::nanoTime() - populateStartNs);
#endif
	}
#endif

#if PLATFORM_PROFILE_STREAMING
	const long_t chunkEvictStartNs = System::nanoTime();
#endif
	int_t unloaded = 0;
	ISaveHandler *saveHandler = worldObj != nullptr ? worldObj->getSaveHandler() : nullptr;
	const ChunkMemoryPolicy::RetentionPolicy retentionPolicy =
		ChunkMemoryPolicy::retentionPolicy(saveHandler != nullptr && saveHandler->isReadOnly());

	// Old explicit drop queue, kept for compatibility with the decompiled layout.
	while (!droppedChunksSet.empty() && unloaded < retentionPolicy.maxUnloadsPerTick)
	{
		std::uint64_t key = *droppedChunksSet.begin();
		droppedChunksSet.erase(droppedChunksSet.begin());

		auto it = chunkMap.find(key);
		if (it == chunkMap.end())
			continue;

		Chunk *chunk = it->second;
		unloadChunk(key, chunk);
		chunkMap.erase(it);
		markChunkTopologyChanged();
		chunkList.erase(std::remove(chunkList.begin(), chunkList.end(), chunk), chunkList.end());
		unloaded++;
	}

	// Real distance-based unload: the previous port never inserted into
	// droppedChunksSet, so chunks stayed in memory forever. This keeps the visible
	// radius plus margin, and removes only chunks that are already outside it.
	//
	// EMERGENCY UNLOAD: if the resident chunk map has grown well past what the
	// unload radius should ever hold, ignore MAX_UNLOADS_PER_TICK and the time
	// gate and drain out-of-radius chunks at an accelerated bounded rate. Without this, a burst
	// that gets chunks into the map faster than 8/tick can drain them (e.g. a
	// stall or a hitch during initial load bunching several ticks' worth of
	// requests together) lets the map grow without bound until the heap is
	// exhausted -- this mirrors the equivalent fix in
	// ChunkProviderLoadOrGenerate::unload100OldestChunks; both cache tiers need
	// it since ChunkProvider is the one actually in the World::getChunkProvider()
	// path.
	const size_t maxResidentChunks = (size_t)((chunkUnloadRadius * 2 + 1) * (chunkUnloadRadius * 2 + 1));
	size_t chunksOutsideRadius = 0;
	for (const auto &entry : chunkMap)
	{
		Chunk *chunk = entry.second;
		if (chunk != nullptr && chunk != blankChunk
			&& isOutsideUnloadRadius(chunk->xPosition, chunk->zPosition)
			&& !isInPopulateFootprint(chunk->xPosition, chunk->zPosition))
		{
			chunksOutsideRadius++;
		}
	}
	const bool emergency = chunkMap.size() > maxResidentChunks && chunksOutsideRadius > 0;
	const int_t unloadLimit = ChunkMemoryPolicy::unloadLimit(retentionPolicy, emergency);

	const long_t now = currentWorldTime();
	for (auto it = chunkMap.begin(); it != chunkMap.end() && unloaded < unloadLimit; )
	{
		Chunk *chunk = it->second;
		if (chunk == nullptr || chunk == blankChunk)
		{
			it = chunkMap.erase(it);
			markChunkTopologyChanged();
			chunkList.erase(std::remove(chunkList.begin(), chunkList.end(), chunk), chunkList.end());
			continue;
		}

		const long_t lastAccess = chunk->lastAccessTick;

		if (isOutsideUnloadRadius(chunk->xPosition, chunk->zPosition)
			&& !isInPopulateFootprint(chunk->xPosition, chunk->zPosition)
			&& (emergency || JavaArithmetic::longSub(now, lastAccess) >= retentionPolicy.minUnusedTicksBeforeUnload))
		{
			unloadChunk(it->first, chunk);
			it = chunkMap.erase(it);
			markChunkTopologyChanged();
			chunkList.erase(std::remove(chunkList.begin(), chunkList.end(), chunk), chunkList.end());
			unloaded++;
			if (chunksOutsideRadius > 0)
				chunksOutsideRadius--;
			if (emergency && chunksOutsideRadius == 0)
				break;
		}
		else
		{
			++it;
		}
	}

#if PLATFORM_PROFILE_STREAMING
	platformProfileChunkEvict(System::nanoTime() - chunkEvictStartNs);
#endif
	if (chunkLoader != nullptr)
		chunkLoader->chunkTick();

	const bool childUnloaded = chunkProvider != nullptr && chunkProvider->unload100OldestChunks();
	return unloaded > 0 || childUnloaded;
}

bool ChunkProvider::canSave()
{
	return true;
}

jstring ChunkProvider::makeString()
{
	jstring result = "ServerChunkCache: " + String::fromInt((int_t)chunkMap.size())
		+ " Radius: " + String::fromInt(chunkLoadRadius)
		+ " UnloadRadius: " + String::fromInt(chunkUnloadRadius)
		+ " Drop: " + String::fromInt((int_t)droppedChunksSet.size());
#if PLATFORM_ASYNC_CHUNK_GENERATION
	if (asyncGenerationScheduler != nullptr)
	{
		int_t pending = 0, completed = 0;
		asyncGenerationScheduler->queueSizes(pending, completed);
		result += " GenQ: " + String::fromInt(pending)
		       + " GenDone: " + String::fromInt(completed);
	}
#endif
	return result;
}

std::vector<SpawnListEntry> *ChunkProvider::getPossibleCreatures(const EnumCreatureType &type, int_t x, int_t y, int_t z)
{
	return chunkProvider != nullptr ? chunkProvider->getPossibleCreatures(type, x, y, z) : nullptr;
}

ChunkPosition *ChunkProvider::findClosestStructure(World *world, const jstring &name, int_t x, int_t y, int_t z)
{
	return chunkProvider != nullptr ? chunkProvider->findClosestStructure(world, name, x, y, z) : nullptr;
}

void ChunkProvider::removeEntityFromLoadedChunks(Entity *entity)
{
	for (const auto &entry : chunkMap)
	{
		Chunk *chunk = entry.second;
		if (chunk != nullptr && chunk != blankChunk)
			chunk->removeEntityFromAllSections(entity);
	}
}
