#include "pc/world/PcLegacyTickScheduler.h"

#include <algorithm>
#include <cstdint>

#include "java/Arithmetic.h"

PcLegacyTickScheduler::~PcLegacyTickScheduler()
{
	clear();
}

bool PcLegacyTickScheduler::schedule(int_t x, int_t y, int_t z, int_t blockId, long_t scheduledTime)
{
	NextTickListEntry *entry = new NextTickListEntry(x, y, z, blockId);
	entry->setScheduledTime(scheduledTime);

	if (!uniqueEntries.add(entry))
	{
		delete entry;
		return false;
	}

	orderedEntries.push_back(entry);
	heapSiftUp(orderedEntries.size() - 1);
	attachToChunk(entry);
	return true;
}

NextTickListEntry *PcLegacyTickScheduler::popNext(long_t currentTime, bool force)
{
	if (orderedEntries.empty())
		return nullptr;

	NextTickListEntry *entry = orderedEntries.front();
	if (!force && entry->scheduledTime > currentTime)
		return nullptr;

	heapErase(entry);
	uniqueEntries.remove(entry);
	detachFromChunk(entry);
	return entry;
}

std::vector<NextTickListEntry *> PcLegacyTickScheduler::getPendingForChunk(int_t chunkX, int_t chunkZ, bool remove)
{
	std::vector<NextTickListEntry *> result;
	const ChunkKey key = makeChunkKey(chunkX, chunkZ);
	auto bucketIterator = entriesByChunk.find(key);
	if (bucketIterator == entriesByChunk.end())
		return result;

	ChunkEntries &entries = bucketIterator->second;
	result.assign(entries.begin(), entries.end());
	std::sort(result.begin(), result.end(), NextTickListEntryComparator{});
	if (!remove)
		return result;

	for (NextTickListEntry *entry : result)
	{
		heapErase(entry);
		uniqueEntries.remove(entry);
		entry->schedulerChunkIndex = -1;
	}
	entriesByChunk.erase(bucketIterator);
	return result;
}

void PcLegacyTickScheduler::clear()
{
	for (NextTickListEntry *entry : orderedEntries)
		delete entry;

	orderedEntries.clear();
	uniqueEntries.clear();
	entriesByChunk.clear();
}

std::size_t PcLegacyTickScheduler::size() const
{
	return orderedEntries.size();
}

bool PcLegacyTickScheduler::empty() const
{
	return orderedEntries.empty();
}

PcLegacyTickScheduler::ChunkKey PcLegacyTickScheduler::makeChunkKey(int_t chunkX, int_t chunkZ)
{
	const std::uint64_t x = static_cast<std::uint32_t>(chunkX);
	const std::uint64_t z = static_cast<std::uint32_t>(chunkZ);
	return (x << 32) | z;
}

PcLegacyTickScheduler::ChunkKey PcLegacyTickScheduler::makeChunkKeyForEntry(const NextTickListEntry *entry)
{
	return makeChunkKey(JavaArithmetic::intShr(entry->xCoord, 4), JavaArithmetic::intShr(entry->zCoord, 4));
}

void PcLegacyTickScheduler::attachToChunk(NextTickListEntry *entry)
{
	ChunkEntries &entries = entriesByChunk[makeChunkKeyForEntry(entry)];
	entry->schedulerChunkIndex = static_cast<int_t>(entries.size());
	entries.push_back(entry);
}

void PcLegacyTickScheduler::detachFromChunk(NextTickListEntry *entry)
{
	auto bucketIterator = entriesByChunk.find(makeChunkKeyForEntry(entry));
	if (bucketIterator == entriesByChunk.end())
		return;

	// The entry knows its slot: swap-remove instead of searching the chunk's
	// list (that search was O(k) per tick, O(k^2) for a chunk full of gravel).
	// The list is unordered; getPendingForChunk sorts what it returns.
	ChunkEntries &entries = bucketIterator->second;
	const std::size_t slot = static_cast<std::size_t>(entry->schedulerChunkIndex);
	NextTickListEntry *last = entries.back();
	entries[slot] = last;
	last->schedulerChunkIndex = static_cast<int_t>(slot);
	entries.pop_back();
	entry->schedulerChunkIndex = -1;

	if (entries.empty())
		entriesByChunk.erase(bucketIterator);
}

void PcLegacyTickScheduler::heapPlace(std::size_t index, NextTickListEntry *entry)
{
	orderedEntries[index] = entry;
	entry->schedulerHeapIndex = static_cast<int_t>(index);
}

void PcLegacyTickScheduler::heapSiftUp(std::size_t index)
{
	NextTickListEntry *entry = orderedEntries[index];
	while (index > 0)
	{
		const std::size_t parent = (index - 1) >> 1;
		if (entry->comparer(orderedEntries[parent]) >= 0)
			break;
		heapPlace(index, orderedEntries[parent]);
		index = parent;
	}
	heapPlace(index, entry);
}

void PcLegacyTickScheduler::heapSiftDown(std::size_t index)
{
	NextTickListEntry *entry = orderedEntries[index];
	const std::size_t count = orderedEntries.size();
	for (;;)
	{
		std::size_t child = index * 2 + 1;
		if (child >= count)
			break;
		if (child + 1 < count && orderedEntries[child + 1]->comparer(orderedEntries[child]) < 0)
			++child;
		if (orderedEntries[child]->comparer(entry) >= 0)
			break;
		heapPlace(index, orderedEntries[child]);
		index = child;
	}
	heapPlace(index, entry);
}

void PcLegacyTickScheduler::heapErase(NextTickListEntry *entry)
{
	const std::size_t index = static_cast<std::size_t>(entry->schedulerHeapIndex);
	NextTickListEntry *last = orderedEntries.back();
	orderedEntries.pop_back();
	entry->schedulerHeapIndex = -1;
	if (index >= orderedEntries.size())
		return;

	// The former last element takes the hole and moves whichever way it must.
	orderedEntries[index] = last;
	heapSiftDown(index);
	heapSiftUp(static_cast<std::size_t>(last->schedulerHeapIndex));
}
