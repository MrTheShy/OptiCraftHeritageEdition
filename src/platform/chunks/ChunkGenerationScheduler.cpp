#include "platform/chunks/ChunkGenerationScheduler.h"
#include "platform/StatCounter.h"

#include "platform/PlatformCompat.h"
#include "platform/PlatformTuning.h"
#include "platform/Thread.h"
#include "net/minecraft/src/Chunk.h"
#include "net/minecraft/src/IChunkProvider.h"
#include "net/minecraft/src/ChunkProviderGenerate.h"
#include "net/minecraft/src/ChunkProviderHell.h"
#include "net/minecraft/src/IntCache.h"
#include "net/minecraft/src/AnvilChunkLoader.h"
#include "net/minecraft/src/McRegionChunkLoader.h"
#include "platform/Log.h"
#include "net/minecraft/src/NBTTagCompound.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

struct ChunkGenerationScheduler::Impl
{
    struct Worker
    {
        ChunkGenerationScheduler* owner = nullptr;
        IChunkProvider* generator = nullptr;
        PlatformThread thread;
        // steady_clock ns when the current job started, 0 while waiting.
        StatCounter busySince{0};
    };

    // Fixed pool, one generator per worker (noise, rand, biome source and
    // IntCache are per instance / thread_local, so they are never shared).
    Worker workers[PLATFORM_ASYNC_GENERATION_WORKERS];
    int workerCount = 0;
    IChunkProvider* generator = nullptr;
    McRegionChunkLoader* regionLoader = nullptr;
    AnvilChunkLoader* anvilLoader = nullptr;
    World* world = nullptr;
    std::atomic<int_t> focusX{0};
    std::atomic<int_t> focusZ{0};
    std::deque<std::pair<int_t, int_t>> pending;
    std::deque<std::pair<int_t, int_t>> worker;
    std::unordered_set<std::uint64_t> queued;
    std::deque<Result> results;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic_bool stop{false};
    // Throughput log, every 10 s while generating.
    StatCounter statNs{0};
    std::atomic<int> statCount{0};
    StatCounter statSince{0};
    // Region and Anvil readers are not safe for two concurrent readers.
    std::mutex diskMutex;
};

std::uint64_t ChunkGenerationScheduler::key(int_t x, int_t z)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32)
         | static_cast<std::uint32_t>(z);
}

ChunkGenerationScheduler::ChunkGenerationScheduler(IChunkProvider* ownedGenerator,
                                                   McRegionChunkLoader* regionLoader,
                                                   World* world)
    : impl_(new Impl())
{
    impl_->generator = ownedGenerator;
    addWorker(ownedGenerator);
    impl_->regionLoader = regionLoader;
    impl_->world = world;
}

void ChunkGenerationScheduler::addWorker(IChunkProvider* ownedGenerator)
{
    if (ownedGenerator == nullptr || impl_->workerCount >= PLATFORM_ASYNC_GENERATION_WORKERS)
    {
        if (ownedGenerator != impl_->generator)
            delete ownedGenerator;
        return;
    }
    Impl::Worker& worker = impl_->workers[impl_->workerCount++];
    worker.owner = this;
    worker.generator = ownedGenerator;
}

void ChunkGenerationScheduler::setAnvilLoader(AnvilChunkLoader* loader)
{
    impl_->anvilLoader = loader;
}

ChunkGenerationScheduler::~ChunkGenerationScheduler()
{
    stop();
    for (int index = 0; index < impl_->workerCount; ++index)
    {
        delete impl_->workers[index].generator;
        impl_->workers[index].generator = nullptr;
    }
    impl_->generator = nullptr;
    delete impl_;
}

bool ChunkGenerationScheduler::start()
{
#if PLATFORM_ASYNC_CHUNK_GENERATION
    if (impl_->generator == nullptr)
        return false;
    impl_->stop.store(false);
#if PLATFORM_ASYNC_SAVED_CHUNK_LOAD
    // NBT parse recursion and zlib run here too, on top of generation.
    const std::size_t workerStack = 64 * 1024;
#else
    const std::size_t workerStack = 48 * 1024;
#endif
    int started = 0;
    for (int index = 0; index < impl_->workerCount; ++index)
    {
        if (impl_->workers[index].thread.start(&ChunkGenerationScheduler::threadEntry,
                                               &impl_->workers[index], workerStack,
                                               PLATFORM_ASYNC_GENERATION_THREAD_PRIORITY,
                                               PLATFORM_ASYNC_GENERATION_AFFINITY_MASK))
            ++started;
        else if (index == 0)
            return false;
    }
    return started > 0;
#else
    return false;
#endif
}

void ChunkGenerationScheduler::stop()
{
#if PLATFORM_ASYNC_CHUNK_GENERATION
    // Under the mutex so the flag cannot be set between the worker evaluating
    // its wait predicate and actually sleeping, which would lose this wakeup
    // and leave join() blocked forever.
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->stop.store(true);
    }
    impl_->wake.notify_all();
    for (int index = 0; index < impl_->workerCount; ++index)
    {
        PlatformThread& thread = impl_->workers[index].thread;
        if (thread.joinable() && !thread.isCurrent())
            thread.join();
    }

    std::lock_guard<std::mutex> guard(impl_->mutex);
    for (Result& result : impl_->results)
        delete result.chunk;
    impl_->results.clear();
    impl_->pending.clear();
    impl_->worker.clear();
    impl_->queued.clear();
#endif
}

bool ChunkGenerationScheduler::active() const
{
#if PLATFORM_ASYNC_CHUNK_GENERATION
    return impl_->generator != nullptr && impl_->workers[0].thread.joinable() && !impl_->stop.load();
#else
    return false;
#endif
}

ChunkGenerationScheduler::RequestStatus ChunkGenerationScheduler::requestDetailed(int_t x, int_t z, int_t queueLimit)
{
#if PLATFORM_ASYNC_CHUNK_GENERATION
    if (!active())
        return RequestStatus::Inactive;

    const std::uint64_t k = key(x, z);
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->queued.count(k) != 0)
        return RequestStatus::AlreadyQueued;
    if ((int_t)(impl_->pending.size() + impl_->worker.size() + impl_->results.size()) >= queueLimit)
        return RequestStatus::QueueFull;
    impl_->queued.insert(k);
    impl_->pending.emplace_back(x, z);
    return RequestStatus::Accepted;
#else
    (void)x; (void)z; (void)queueLimit;
    return RequestStatus::Inactive;
#endif
}

bool ChunkGenerationScheduler::request(int_t x, int_t z, int_t queueLimit)
{
    return requestDetailed(x, z, queueLimit) == RequestStatus::Accepted;
}

bool ChunkGenerationScheduler::dispatch(int_t budget, CoordinatePredicate predicate, void* context)
{
#if PLATFORM_ASYNC_CHUNK_GENERATION
    if (budget <= 0 || !active())
        return false;

    bool dispatched = false;
    for (int_t n = 0; n < budget; ++n)
    {
        std::pair<int_t, int_t> coord;
        {
            std::lock_guard<std::mutex> guard(impl_->mutex);
            if (impl_->pending.empty())
                break;
            coord = impl_->pending.front();
            impl_->pending.pop_front();
        }

        if (predicate != nullptr && !predicate(context, coord.first, coord.second))
        {
            complete(coord.first, coord.second);
            continue;
        }

        {
            std::lock_guard<std::mutex> guard(impl_->mutex);
            impl_->worker.push_back(coord);
        }
        impl_->wake.notify_one();
        dispatched = true;
    }
    return dispatched;
#else
    (void)budget; (void)predicate; (void)context;
    return false;
#endif
}

void ChunkGenerationScheduler::setFocus(int_t chunkX, int_t chunkZ)
{
    impl_->focusX.store(chunkX, std::memory_order_relaxed);
    impl_->focusZ.store(chunkZ, std::memory_order_relaxed);
}

bool ChunkGenerationScheduler::popResult(Result& out)
{
#if PLATFORM_ASYNC_CHUNK_GENERATION
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->results.empty())
        return false;
    out = std::move(impl_->results.front());
    impl_->results.pop_front();
    return true;
#else
    (void)out;
    return false;
#endif
}

void ChunkGenerationScheduler::complete(int_t x, int_t z)
{
#if PLATFORM_ASYNC_CHUNK_GENERATION
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->queued.erase(key(x, z));
#else
    (void)x; (void)z;
#endif
}

void ChunkGenerationScheduler::describe(char* out, int size) const
{
    const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int written;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        written = std::snprintf(out, size, "pending %d dispatched %d results %d claimed %d busy",
                                (int)impl_->pending.size(), (int)impl_->worker.size(),
                                (int)impl_->results.size(), (int)impl_->queued.size());
    }
    for (int index = 0; index < impl_->workerCount && written > 0 && written < size; ++index)
    {
        const long long since = impl_->workers[index].busySince.load();
        written += std::snprintf(out + written, size - written, " %lldms",
                                 since == 0 ? -1LL : (now - since) / 1000000LL);
    }
}

void ChunkGenerationScheduler::queueSizes(int_t& pending, int_t& completed) const
{
#if PLATFORM_ASYNC_CHUNK_GENERATION
    std::lock_guard<std::mutex> guard(impl_->mutex);
    pending = static_cast<int_t>(impl_->pending.size() + impl_->worker.size());
    completed = static_cast<int_t>(impl_->results.size());
#else
    pending = 0;
    completed = 0;
#endif
}

void ChunkGenerationScheduler::pushDropped(int_t x, int_t z)
{
    Result result;
    result.x = x;
    result.z = z;
    result.kind = ResultKind::Dropped;
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->results.push_back(std::move(result));
}

void ChunkGenerationScheduler::noteGenerated(long long ns)
{
    impl_->statNs += ns;
    const int count = ++impl_->statCount;
    const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long since = impl_->statSince.load();
    if (since == 0)
    {
        impl_->statSince.compare_exchange_strong(since, now);
        return;
    }
    if (now - since < 10000000000LL || !impl_->statSince.compare_exchange_strong(since, now))
        return;
    const long long total = impl_->statNs.exchange(0);
    impl_->statCount -= count;
    long long phase[ChunkProviderGenerate::GenPhaseCount];
    ChunkProviderGenerate::takeGenPhaseStats(phase);
    const double perChunk = 1.0e6 * count;
    MC_LOG_INFO("chunk", "worldgen: %d chunks in %.1f s on %d workers, %.1f ms each "
                "(biomeLayers %.1f noise %.1f fill %.1f blockBiomes %.1f surface %.1f caves %.1f ravines %.1f)\n",
                count, (now - since) / 1.0e9, impl_->workerCount, total / perChunk,
                phase[0] / perChunk, phase[1] / perChunk, phase[2] / perChunk,
                phase[6] / perChunk, phase[3] / perChunk, phase[4] / perChunk, phase[5] / perChunk);
}

void* ChunkGenerationScheduler::threadEntry(void* argument)
{
    Impl::Worker* worker = static_cast<Impl::Worker*>(argument);
    worker->owner->runWorker(static_cast<int>(worker - worker->owner->impl_->workers));
    return nullptr;
}

void ChunkGenerationScheduler::runWorker(int workerIndex)
{
    Impl::Worker& self = impl_->workers[workerIndex];
    IChunkProvider* generator = self.generator;
#if PLATFORM_ASYNC_CHUNK_GENERATION
#if PLATFORM_WII
    // Every GenLayer step below allocates from IntCache, which the main thread
    // also uses for sky colour, mob spawning and getBiomeGenAt(). Claim the
    // worker's own slot before the first chunk.
    IntCache::bindGenerationThread();
#endif

    while (true)
    {
        std::pair<int_t, int_t> coord;
        {
            // Sleep rather than poll. The worker only ever gets the CPU the main
            // thread leaves behind, so a 1 ms poll spent a large share of that
            // gap deciding it had nothing to do; dispatch() now wakes it.
            std::unique_lock<std::mutex> lock(impl_->mutex);
            self.busySince = 0;
            impl_->wake.wait(lock, [this]()
            {
                return impl_->stop.load(std::memory_order_relaxed) || !impl_->worker.empty();
            });

            if (impl_->stop.load(std::memory_order_relaxed))
                break;

#if PLATFORM_ASYNC_NEAREST_FIRST
            // Nearest queued column to the focus first (Chebyshev, the chunk
            // ring metric). Requests arrive in renderer/prefetch order, which
            // after a turn is not the order the player will see them in.
            const int_t focusX = impl_->focusX.load(std::memory_order_relaxed);
            const int_t focusZ = impl_->focusZ.load(std::memory_order_relaxed);
            auto best = impl_->worker.begin();
            long_t bestDistance = 0;
            for (auto it = impl_->worker.begin(); it != impl_->worker.end(); ++it)
            {
                long_t dx = static_cast<long_t>(it->first) - static_cast<long_t>(focusX);
                long_t dz = static_cast<long_t>(it->second) - static_cast<long_t>(focusZ);
                if (dx < 0) dx = -dx;
                if (dz < 0) dz = -dz;
                const long_t distance = dx > dz ? dx : dz;
                if (it == impl_->worker.begin() || distance < bestDistance)
                {
                    best = it;
                    bestDistance = distance;
                }
            }
            coord = *best;
            impl_->worker.erase(best);
#else
            coord = impl_->worker.front();
            impl_->worker.pop_front();
#endif
        }

        self.busySince = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::unique_lock<std::mutex> diskLock(impl_->diskMutex);
#if PLATFORM_ASYNC_SAVED_CHUNK_LOAD
        // Saved Anvil chunk: never generate over it. isChunkSaved() and the
        // read below cannot straddle a writer (see AnvilChunkLoader), so
        // "saved" here means the bytes are reachable; a miss is an anomaly.
        if (impl_->anvilLoader != nullptr && impl_->anvilLoader->isChunkSaved(coord.first, coord.second))
        {
            const auto startTime = std::chrono::steady_clock::now();
            Result result;
            result.x = coord.first;
            result.z = coord.second;
            ChunkLoadStatus loadStatus = ChunkLoadStatus::Missing;
            bool loaded = false;
            for (int attempt = 0; attempt < 3 && !loaded && loadStatus != ChunkLoadStatus::ReadError; ++attempt)
            {
                if (attempt > 0)
                    std::this_thread::yield();
                loaded = impl_->anvilLoader->readChunkRoot(coord.first, coord.second, result.nbt, &loadStatus);
            }
            if (loaded)
            {
                result.kind = ResultKind::LoadedRoot;
            }
            else if (loadStatus == ChunkLoadStatus::ReadError)
            {
                result.kind = ResultKind::ReadError;
            }
            diskLock.unlock();
            if (!loaded && loadStatus != ChunkLoadStatus::ReadError)
            {
                // Saved but unreadable-as-missing: do not generate over it and
                // do not publish a hole. Drop the request; the streaming code
                // asks again, and a real fault then surfaces as a ReadError.
                MC_LOG_WARN("chunk", "Saved Anvil chunk %d,%d vanished while loading; retrying later\n",
                            coord.first, coord.second);
                pushDropped(coord.first, coord.second);
                continue;
            }
            result.workerNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            std::lock_guard<std::mutex> guard(impl_->mutex);
            impl_->results.push_back(std::move(result));
            continue;
        }
#endif

        if (impl_->regionLoader != nullptr)
        {
            std::vector<byte_t> data;
            ChunkLoadStatus loadStatus = ChunkLoadStatus::Missing;
            if (impl_->regionLoader->readChunkData(coord.first, coord.second, data, &loadStatus))
            {
#if PLATFORM_ASYNC_CHUNK_DECODE
                if (impl_->world != nullptr)
                {
                    // Blocks, light and heightmap only; the parsed root travels
                    // with the chunk so publish can add its entities. A decode
                    // failure falls through to the game-thread path, which
                    // reports it and preserves the region data as before.
                    Result result;
                    result.x = coord.first;
                    result.z = coord.second;
                    ChunkLoadStatus decodeStatus = ChunkLoadStatus::ReadError;
                    result.chunk = impl_->regionLoader->decodeChunkBlocksFromData(
                        impl_->world, coord.first, coord.second, data, result.nbt, &decodeStatus);
                    if (result.chunk != nullptr)
                    {
                        result.kind = ResultKind::LoadedChunk;
                        std::lock_guard<std::mutex> guard(impl_->mutex);
                        impl_->results.push_back(std::move(result));
                        continue;
                    }
                }
#endif
                Result result;
                result.x = coord.first;
                result.z = coord.second;
                result.kind = ResultKind::LoadedData;
                result.data = std::move(data);
                std::lock_guard<std::mutex> guard(impl_->mutex);
                impl_->results.push_back(std::move(result));
                continue;
            }
            if (loadStatus == ChunkLoadStatus::ReadError)
            {
                Result result;
                result.x = coord.first;
                result.z = coord.second;
                result.kind = ResultKind::ReadError;
                std::lock_guard<std::mutex> guard(impl_->mutex);
                impl_->results.push_back(std::move(result));
                continue;
            }
        }

        diskLock.unlock();

#if PLATFORM_PC_LEGACY || PLATFORM_WII
        if (ChunkProviderGenerate* terrain = dynamic_cast<ChunkProviderGenerate*>(generator))
        {
            std::vector<byte_t> generatedData;
            const auto startTime = std::chrono::steady_clock::now();
            if (!terrain->generateAsyncChunkData(coord.first, coord.second, generatedData))
            {
                pushDropped(coord.first, coord.second);
                continue;
            }
            noteGenerated(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - startTime).count());

            Result result;
            result.x = coord.first;
            result.z = coord.second;
            result.kind = ResultKind::GeneratedData;
            result.data = std::move(generatedData);
            std::lock_guard<std::mutex> guard(impl_->mutex);
            impl_->results.push_back(std::move(result));
            continue;
        }
        if (ChunkProviderHell* nether = dynamic_cast<ChunkProviderHell*>(generator))
        {
            Result result;
            result.x = coord.first;
            result.z = coord.second;
            result.kind = ResultKind::GeneratedData;
            nether->generateAsyncChunkData(coord.first, coord.second, result.data);
            std::lock_guard<std::mutex> guard(impl_->mutex);
            impl_->results.push_back(std::move(result));
            continue;
        }
#endif

        Chunk* generated = generator->provideChunk(coord.first, coord.second);
        if (generated == nullptr)
        {
            pushDropped(coord.first, coord.second);
            continue;
        }

        Result result;
        result.x = coord.first;
        result.z = coord.second;
        result.kind = ResultKind::Generated;
        result.chunk = generated;
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->results.push_back(std::move(result));
    }

#if PLATFORM_WII
    IntCache::unbindGenerationThread();
#endif
#endif
}
