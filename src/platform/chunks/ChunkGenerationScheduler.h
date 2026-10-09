#pragma once

#include "java/Type.h"
#include "platform/PlatformConfig.h"
// Result owns the parsed compound through a unique_ptr, whose deleter is
// instantiated wherever Result is destroyed, so the full type is needed here.
#include "net/minecraft/src/NBTTagCompound.h"

#include <cstdint>
#include <memory>
#include <vector>

class Chunk;
class IChunkProvider;
class McRegionChunkLoader;
class AnvilChunkLoader;
class World;

class ChunkGenerationScheduler
{
public:
    enum class ResultKind
    {
        LoadedData,
        // Saved chunk already decoded on the worker (PLATFORM_ASYNC_CHUNK_DECODE):
        // `chunk` holds blocks/light/heightmap, `nbt` the parsed root whose
        // entities the game thread still has to construct.
        LoadedChunk,
        // Saved Anvil chunk read and parsed on the worker: `nbt` holds the
        // root, the game thread builds the Chunk from it.
        LoadedRoot,
        GeneratedData,
        Generated,
        ReadError,
        // Nothing to publish; the provider releases the request.
        Dropped
    };

    enum class RequestStatus
    {
        Accepted,
        AlreadyQueued,
        QueueFull,
        Inactive
    };

    struct Result
    {
        int_t x = 0;
        int_t z = 0;
        ResultKind kind = ResultKind::ReadError;
        Chunk* chunk = nullptr;
        std::vector<byte_t> data;
        std::unique_ptr<NBTTagCompound> nbt;
        // Worker time spent on this result (read + inflate + parse), for logs.
        long long workerNs = 0;
    };

    using CoordinatePredicate = bool (*)(void* context, int_t x, int_t z);

    // `world` is only read by the worker-side chunk decode; pass nullptr to
    // keep saved chunks on the LoadedData path.
    ChunkGenerationScheduler(IChunkProvider* ownedGenerator, McRegionChunkLoader* regionLoader,
                             World* world = nullptr);
    ~ChunkGenerationScheduler();

    // Saved Anvil chunks are then read on the worker (ResultKind::LoadedRoot)
    // instead of the game thread. Call before start(); the loader must outlive
    // stop().
    void setAnvilLoader(AnvilChunkLoader* loader);
    // Another worker thread with its own generator (owned). Call before start();
    // ignored past PLATFORM_ASYNC_GENERATION_WORKERS.
    void addWorker(IChunkProvider* ownedGenerator);
    bool start();
    void stop();
    bool active() const;

    RequestStatus requestDetailed(int_t x, int_t z, int_t queueLimit);
    bool request(int_t x, int_t z, int_t queueLimit);
    bool dispatch(int_t budget, CoordinatePredicate predicate, void* context);
    // Chunk coordinate the worker orders its queue around (PLATFORM_ASYNC_NEAREST_FIRST).
    void setFocus(int_t chunkX, int_t chunkZ);
    bool popResult(Result& out);
    void complete(int_t x, int_t z);
    void queueSizes(int_t& pending, int_t& completed) const;
    // One-line state for the log: queue sizes and how long each worker has
    // been on its current job (-1 = idle).
    void describe(char* out, int size) const;

private:
    static std::uint64_t key(int_t x, int_t z);
    static void* threadEntry(void* argument);
    void runWorker(int workerIndex);
    void noteGenerated(long long ns);
    void pushDropped(int_t x, int_t z);

    struct Impl;
    Impl* impl_;
};
