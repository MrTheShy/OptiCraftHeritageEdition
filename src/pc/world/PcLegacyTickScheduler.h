#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "java/HashSet.h"
#include "java/Type.h"
#include "net/minecraft/src/NextTickListEntry.h"

class PcLegacyTickScheduler
{
public:
	PcLegacyTickScheduler() = default;
	~PcLegacyTickScheduler();

	PcLegacyTickScheduler(const PcLegacyTickScheduler &) = delete;
	PcLegacyTickScheduler &operator=(const PcLegacyTickScheduler &) = delete;

	bool schedule(int_t x, int_t y, int_t z, int_t blockId, long_t scheduledTime);
	NextTickListEntry *popNext(long_t currentTime, bool force);
	std::vector<NextTickListEntry *> getPendingForChunk(int_t chunkX, int_t chunkZ, bool remove);

	void clear();
	std::size_t size() const;
	bool empty() const;

private:
	using ChunkKey = std::uint64_t;
	using ChunkEntries = std::vector<NextTickListEntry *>;

	// std::hash<uint64_t> is the low 32 bits on 32-bit targets (Vita): the key
	// would hash by chunk Z alone. Map iteration order is never observed.
	struct ChunkKeyHash
	{
		std::size_t operator()(ChunkKey key) const
		{
			return static_cast<std::size_t>(static_cast<std::uint32_t>(key >> 32) * 0x9E3779B1u ^ static_cast<std::uint32_t>(key));
		}
	};

	static ChunkKey makeChunkKey(int_t chunkX, int_t chunkZ);
	static ChunkKey makeChunkKeyForEntry(const NextTickListEntry *entry);

	void attachToChunk(NextTickListEntry *entry);
	void detachFromChunk(NextTickListEntry *entry);
	void heapPlace(std::size_t index, NextTickListEntry *entry);
	void heapSiftUp(std::size_t index);
	void heapSiftDown(std::size_t index);
	void heapErase(NextTickListEntry *entry);

	// Binary min-heap on NextTickListEntry::compareTo (time, then insertion
	// id). The key is a total order, so pops come out exactly as from Java's
	// TreeSet, without a tree node allocation per tick.
	std::vector<NextTickListEntry *> orderedEntries;
	JavaHashSet<NextTickListEntry *, NextTickListEntryHash, NextTickListEntryEqual> uniqueEntries;
	std::unordered_map<ChunkKey, ChunkEntries, ChunkKeyHash> entriesByChunk;
};
