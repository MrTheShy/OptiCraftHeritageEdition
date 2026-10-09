#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "ChunkCoordIntPair.h"
#include "IChunkLoader.h"
#include "IThreadedFileIO.h"

class AnvilChunkLoaderPending;
class NBTTagCompound;

// net.minecraft.src.AnvilChunkLoader
class AnvilChunkLoader : public IChunkLoader, public IThreadedFileIO
{
public:
    explicit AnvilChunkLoader(const std::string &worldDir, bool readOnly = false);
    ~AnvilChunkLoader() override;

    Chunk *loadChunk(World *world, int_t x, int_t z,
                     ChunkLoadStatus *status = nullptr) override;
    // Whether loadChunk() would find data for this chunk: queued for writing
    // or already in its region file. Lets the streaming provider hand only
    // genuinely new terrain to the generation worker, which has no loader of
    // its own and would otherwise generate over a saved chunk.
    bool isChunkSaved(int_t x, int_t z);
    // Thread-safe (streaming worker): reads the saved bytes -- the newest
    // queued/in-flight write, else the region file -- into a local buffer and
    // parses the NBT root without building a Chunk. False with *status Missing
    // when nothing is readable, ReadError when stored data is unusable.
    bool readChunkRoot(int_t x, int_t z, std::unique_ptr<NBTTagCompound> &root,
                       ChunkLoadStatus *status);
    // Game-thread half of an async load: builds the chunk from readChunkRoot().
    Chunk *loadChunkFromCompound(World *world, int_t expectedX, int_t expectedZ,
                                 NBTTagCompound *root, ChunkLoadStatus *status);
    void saveChunk(World *world, Chunk *chunk) override;
    void saveExtraChunkData(World *world, Chunk *chunk) override;
    void addRandomArmor() override;
    void chunkTick() override;
    void saveExtraData() override;
    bool writeNextIO() override;

private:
    Chunk *readChunkFromLevel(World *world, NBTTagCompound *level);
    void writeChunkToLevel(Chunk *chunk, World *world, NBTTagCompound *level);
    void queueChunkToSave(const ChunkCoordIntPair &position, std::vector<byte_t> serialized);
    void writePendingChunk(AnvilChunkLoaderPending *pending);
    bool copyPendingChunkData(const ChunkCoordIntPair &position, std::vector<byte_t> &out);

    std::string worldDir;
    bool storageDisabled;
    bool readOnly;
    std::vector<byte_t> readScratch;
    std::vector<byte_t> writeScratch;

    // pendingCoordinates holds every coordinate that is in pendingSaves OR is
    // the entry being written (inFlight). A coordinate leaves it only after
    // its region write has returned (and no newer entry is queued), so
    // "not in the set" implies "already in the region file". All three are
    // guarded by pendingMutex.
    std::vector<AnvilChunkLoaderPending *> pendingSaves;
    AnvilChunkLoaderPending *inFlight = nullptr;
    std::unordered_set<ChunkCoordIntPair, ChunkCoordIntPairValueHash, ChunkCoordIntPairValueEqual> pendingCoordinates;
    std::mutex pendingMutex;
};
