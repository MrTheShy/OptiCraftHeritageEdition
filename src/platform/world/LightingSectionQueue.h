#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "java/Type.h"

// Cells waiting for a light recompute: one bit per cell per section and light
// type (PLATFORM_LIGHTING_SECTION_QUEUE). It replaces World::lightingToUpdate,
// whose hard cap dropped requests and left dark patches after decoration; here
// asking again costs one bit test and nothing is dropped (512 bytes per section
// with pending cells). Most recently touched sections drain first, so a block
// the player just placed is lit before the backlog.
class LightingSectionQueue
{
public:
    static constexpr int_t WORDS = 4096 / 32;

    struct Section
    {
        int_t chunkX = 0;
        int_t chunkZ = 0;
        int_t sectionY = 0;
        int_t type = 0;           // 0 = sky, 1 = block
        std::uint32_t pending = 0;
        int_t prev = -1;          // recency list
        int_t next = -1;
        int_t hashNext = -1;
        std::uint32_t bits[WORDS]; // cell y << 8 | z << 4 | x within the section
    };

    LightingSectionQueue();

    // y must lie inside the world. Returns false when the cell was already pending.
    bool insert(int_t type, int_t x, int_t y, int_t z);

    // Most recently touched section, or -1 when nothing is pending.
    int_t front() const { return head; }
    Section &at(int_t index) { return *pool[(std::size_t)index]; }

    // Clears and returns the first pending cell at or after `from`, wrapping
    // around to the start of the section; -1 when the section has none left.
    int_t take(int_t index, int_t from);
    // Frees a section whose cells have all been taken. A section stays allocated
    // while it is being drained, so cells queued into it meanwhile land in it.
    void releaseIfEmpty(int_t index);

    std::size_t size() const { return cells; }
    bool empty() const { return cells == 0; }
    void clear();

    // Requests that found the cell already pending, since the last call.
    std::uint32_t takeDuplicateCount()
    {
        const std::uint32_t n = duplicates;
        duplicates = 0;
        return n;
    }

private:
    static constexpr std::size_t BUCKETS = 1024;

    static std::size_t bucketOf(int_t type, int_t chunkX, int_t sectionY, int_t chunkZ);
    int_t find(int_t type, int_t chunkX, int_t sectionY, int_t chunkZ) const;
    int_t allocate(int_t type, int_t chunkX, int_t sectionY, int_t chunkZ);
    void unlink(int_t index);
    void pushFront(int_t index);

    std::vector<std::unique_ptr<Section>> pool;
    std::vector<int_t> freeList;
    std::vector<int_t> buckets;
    int_t head = -1;
    int_t lastIndex = -1; // consecutive requests nearly always hit the same section
    std::size_t cells = 0;
    std::uint32_t duplicates = 0;
};
