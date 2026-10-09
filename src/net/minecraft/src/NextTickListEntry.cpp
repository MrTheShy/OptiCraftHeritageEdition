#include "NextTickListEntry.h"
#include "java/Arithmetic.h"
#include <cstdint>

long_t NextTickListEntry::nextTickEntryID = 0LL;

#ifdef VITA_PLATFORM
// Decoration schedules (and the next ticks retire) thousands of these per
// chunk, all on the game thread: recycle them through a free list instead of
// a locked newlib malloc/free each.
// The list keeps its peak size (~40 B per pending tick) and is never returned
// to the heap.
static void *s_freeEntries = nullptr;

void *NextTickListEntry::operator new(std::size_t size)
{
	void *pointer = s_freeEntries;
	if (pointer == nullptr)
		return ::operator new(size);
	s_freeEntries = *static_cast<void **>(pointer);
	return pointer;
}

void NextTickListEntry::operator delete(void *pointer) noexcept
{
	if (pointer == nullptr)
		return;
	*static_cast<void **>(pointer) = s_freeEntries;
	s_freeEntries = pointer;
}
#endif

NextTickListEntry::NextTickListEntry(int_t i, int_t j, int_t k, int_t l)
{
	tickEntryID = nextTickEntryID;
	nextTickEntryID = JavaArithmetic::longAdd(nextTickEntryID, 1LL);
	xCoord = i;
	yCoord = j;
	zCoord = k;
	blockID = l;
}

bool NextTickListEntry::equals(const NextTickListEntry *nextticklistentry) const
{
	if (nextticklistentry != nullptr)
		return xCoord == nextticklistentry->xCoord && yCoord == nextticklistentry->yCoord && zCoord == nextticklistentry->zCoord && blockID == nextticklistentry->blockID;
	else
		return false;
}

int_t NextTickListEntry::hashCode() const
{
	std::uint32_t value = static_cast<std::uint32_t>(xCoord) * 1024u * 1024u;
	value += static_cast<std::uint32_t>(zCoord) * 1024u;
	value += static_cast<std::uint32_t>(yCoord);
	value = value * 256u + static_cast<std::uint32_t>(blockID);
	return JavaArithmetic::intFromBits(value);
}

NextTickListEntry *NextTickListEntry::setScheduledTime(long_t l)
{
	scheduledTime = l;
	return this;
}

int_t NextTickListEntry::compareTo(const NextTickListEntry *obj) const
{
	return comparer(obj);
}
