#include "platform/world/LightingSectionQueue.h"

#include <algorithm>
#include <cstring>

namespace
{
    // Sections kept allocated once the queue has drained; a larger pool left by
    // a backlog is returned to the heap.
    constexpr std::size_t KEEP_POOL = 256;
}

LightingSectionQueue::LightingSectionQueue() : buckets(BUCKETS, -1)
{
}

std::size_t LightingSectionQueue::bucketOf(int_t type, int_t chunkX, int_t sectionY, int_t chunkZ)
{
    // 32-bit multiplies only: cheap on the A9 and the EE alike.
    const std::uint32_t h = (std::uint32_t)chunkX * 0x9E3779B1u ^
                            (std::uint32_t)chunkZ * 0x85EBCA77u ^
                            (std::uint32_t)(sectionY << 1 | type) * 0xC2B2AE3Du;
    return (std::size_t)(h >> 16) & (BUCKETS - 1);
}

int_t LightingSectionQueue::find(int_t type, int_t chunkX, int_t sectionY, int_t chunkZ) const
{
    for (int_t i = buckets[bucketOf(type, chunkX, sectionY, chunkZ)]; i >= 0; i = pool[(std::size_t)i]->hashNext)
    {
        const Section &s = *pool[(std::size_t)i];
        if (s.chunkX == chunkX && s.chunkZ == chunkZ && s.sectionY == sectionY && s.type == type)
            return i;
    }
    return -1;
}

int_t LightingSectionQueue::allocate(int_t type, int_t chunkX, int_t sectionY, int_t chunkZ)
{
    int_t index;
    if (!freeList.empty())
    {
        index = freeList.back();
        freeList.pop_back();
    }
    else
    {
        index = (int_t)pool.size();
        pool.emplace_back(new Section());
    }
    Section &s = *pool[(std::size_t)index];
    s.chunkX = chunkX;
    s.chunkZ = chunkZ;
    s.sectionY = sectionY;
    s.type = type;
    s.pending = 0;
    std::memset(s.bits, 0, sizeof(s.bits));
    const std::size_t bucket = bucketOf(type, chunkX, sectionY, chunkZ);
    s.hashNext = buckets[bucket];
    buckets[bucket] = index;
    s.prev = -1;
    s.next = -1;
    pushFront(index);
    return index;
}

void LightingSectionQueue::unlink(int_t index)
{
    Section &s = *pool[(std::size_t)index];
    if (s.prev >= 0)
        pool[(std::size_t)s.prev]->next = s.next;
    else
        head = s.next;
    if (s.next >= 0)
        pool[(std::size_t)s.next]->prev = s.prev;
    s.prev = -1;
    s.next = -1;
}

void LightingSectionQueue::pushFront(int_t index)
{
    Section &s = *pool[(std::size_t)index];
    s.prev = -1;
    s.next = head;
    if (head >= 0)
        pool[(std::size_t)head]->prev = index;
    head = index;
}

bool LightingSectionQueue::insert(int_t type, int_t x, int_t y, int_t z)
{
    const int_t chunkX = x >> 4;
    const int_t chunkZ = z >> 4;
    const int_t sectionY = y >> 4;
    int_t index = lastIndex;
    if (index < 0)
        index = find(type, chunkX, sectionY, chunkZ);
    else
    {
        const Section &last = *pool[(std::size_t)index];
        if (last.chunkX != chunkX || last.chunkZ != chunkZ || last.sectionY != sectionY || last.type != type)
            index = find(type, chunkX, sectionY, chunkZ);
    }
    if (index < 0)
        index = allocate(type, chunkX, sectionY, chunkZ);
    lastIndex = index;

    Section &s = *pool[(std::size_t)index];
    const int_t cell = (y & 15) << 8 | (z & 15) << 4 | (x & 15);
    std::uint32_t &word = s.bits[cell >> 5];
    const std::uint32_t bit = 1u << (cell & 31);
    if ((word & bit) != 0)
    {
        ++duplicates;
        return false;
    }
    word |= bit;
    ++s.pending;
    ++cells;
    if (head != index)
    {
        unlink(index);
        pushFront(index);
    }
    return true;
}

int_t LightingSectionQueue::take(int_t index, int_t from)
{
    Section &s = *pool[(std::size_t)index];
    if (s.pending == 0)
        return -1;
    int_t word = (from >> 5) & (WORDS - 1);
    std::uint32_t bits = s.bits[word] & (~0u << (from & 31));
    // pending > 0, so a bit turns up within one lap (the first word is
    // revisited whole at the end for the bits below `from`).
    for (int_t scanned = 0; scanned <= WORDS; ++scanned)
    {
        if (bits != 0)
        {
            const int_t bit = __builtin_ctz(bits);
            s.bits[word] &= ~(1u << bit);
            --s.pending;
            --cells;
            return word << 5 | bit;
        }
        word = (word + 1) & (WORDS - 1);
        bits = s.bits[word];
    }
    return -1;
}

void LightingSectionQueue::releaseIfEmpty(int_t index)
{
    Section &s = *pool[(std::size_t)index];
    if (s.pending != 0)
        return;
    unlink(index);
    int_t *link = &buckets[bucketOf(s.type, s.chunkX, s.sectionY, s.chunkZ)];
    while (*link != index)
        link = &pool[(std::size_t)*link]->hashNext;
    *link = s.hashNext;
    freeList.push_back(index);
    if (lastIndex == index)
        lastIndex = -1;
    if (cells == 0 && pool.size() > KEEP_POOL)
        clear();
}

void LightingSectionQueue::clear()
{
    pool.clear();
    pool.shrink_to_fit();
    freeList.clear();
    freeList.shrink_to_fit();
    std::fill(buckets.begin(), buckets.end(), -1);
    head = -1;
    lastIndex = -1;
    cells = 0;
}
