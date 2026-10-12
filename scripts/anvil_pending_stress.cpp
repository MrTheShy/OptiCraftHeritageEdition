// Host-side stress test for the AnvilChunkLoader pending-save / in-flight
// protocol that the async saved-chunk load relies on.
//
//   clang++ -std=c++17 -O1 -pthread [-fsanitize=thread] scripts/anvil_pending_stress.cpp -o /tmp/anvil_stress
//   /tmp/anvil_stress          # NEW protocol: must print "PASS", exit 0
//   /tmp/anvil_stress old      # pre-fix protocol: reports holes, exit 1
//
// The Loader below mirrors queueChunkToSave / copyPendingChunkData /
// isChunkSaved / readChunkRoot / writeNextIO (pendingSaves, pendingCoordinates,
// inFlight, one mutex) with an in-memory region whose write is slow and
// happens outside the lock, like RegionFile::write on a memory card.
//
// Invariant checked by the readers (the streaming worker's view): once a save
// of a chunk has returned, isSaved() is true and the read yields a version at
// least as new as that save -- never Missing (void chunk / regenerate over it).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

struct Pending { int coord; int version; };

struct Loader
{
    bool fixed;
    std::mutex m;
    std::vector<Pending *> pendingSaves;
    Pending *inFlight = nullptr;
    std::unordered_set<int> pendingCoordinates;
    std::mutex regionMutex;
    std::map<int, int> region;

    explicit Loader(bool f) : fixed(f) {}

    void queueSave(int coord, int version)
    {
        std::lock_guard<std::mutex> g(m);
        if (pendingCoordinates.count(coord))
            for (Pending *p : pendingSaves)
                if (p->coord == coord) { p->version = version; return; }
        pendingSaves.push_back(new Pending{coord, version});
        pendingCoordinates.insert(coord);
    }

    bool copyPending(int coord, int &version)
    {
        std::lock_guard<std::mutex> g(m);
        if (!pendingCoordinates.count(coord)) return false;
        for (Pending *p : pendingSaves)
            if (p->coord == coord) { version = p->version; return true; }
        if (inFlight && inFlight->coord == coord) { version = inFlight->version; return true; }
        return false;
    }

    bool isSaved(int coord)
    {
        { std::lock_guard<std::mutex> g(m); if (pendingCoordinates.count(coord)) return true; }
        std::lock_guard<std::mutex> r(regionMutex);
        return region.count(coord) != 0;
    }

    // true = loaded (version set); false = Missing
    bool readRoot(int coord, int &version)
    {
        if (copyPending(coord, version)) return true;
        std::lock_guard<std::mutex> r(regionMutex);
        auto it = region.find(coord);
        if (it == region.end()) return false;
        version = it->second;
        return true;
    }

    bool writeNext()
    {
        Pending *p;
        {
            std::lock_guard<std::mutex> g(m);
            if (pendingSaves.empty()) return false;
            p = pendingSaves.front();
            pendingSaves.erase(pendingSaves.begin());
            if (fixed) inFlight = p; else pendingCoordinates.erase(p->coord);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50 + (p->version * 37) % 200));
        { std::lock_guard<std::mutex> r(regionMutex); region[p->coord] = p->version; }
        if (fixed)
        {
            std::lock_guard<std::mutex> g(m);
            inFlight = nullptr;
            bool newer = std::any_of(pendingSaves.begin(), pendingSaves.end(),
                                     [p](Pending *o) { return o->coord == p->coord; });
            if (!newer) pendingCoordinates.erase(p->coord);
        }
        delete p;
        return true;
    }
};

int main(int argc, char **argv)
{
    const bool fixed = !(argc > 1 && std::strcmp(argv[1], "old") == 0);
    const int kCoords = 4;
    Loader loader(fixed);
    std::atomic<int> lastQueued[kCoords];
    for (auto &a : lastQueued) a = 0;
    std::atomic<bool> stop{false}, writerStop{false};
    std::atomic<long> holes{0}, notSaved{0}, stale{0}, reads{0};

    std::thread writer([&] {
        while (!writerStop) if (!loader.writeNext()) std::this_thread::yield();
    });
    std::vector<std::thread> readers;
    for (int t = 0; t < 2; ++t)
        readers.emplace_back([&, t] {
            unsigned seed = 12345u + t;
            while (!stop)
            {
                seed = seed * 1664525u + 1013904223u;
                int c = (seed >> 16) % kCoords;
                int floorV = lastQueued[c].load();      // a save of this version already returned
                if (floorV == 0) continue;
                if (!loader.isSaved(c)) { ++notSaved; continue; }  // worker would generate over it
                int v = 0;
                if (!loader.readRoot(c, v)) { ++holes; continue; } // worker would publish a void chunk
                if (v < floorV) ++stale;
                ++reads;
            }
        });
    // game thread: keeps re-saving (edit -> unload -> save), occasionally letting the queue drain
    for (int version = 1; version <= 20000; ++version)
    {
        int c = version % kCoords;
        loader.queueSave(c, version);
        lastQueued[c] = version;
        if (version % 7 == 0) std::this_thread::sleep_for(std::chrono::microseconds(120));
    }
    stop = true;
    for (auto &r : readers) r.join();
    writerStop = true;
    writer.join();

    std::printf("%s protocol: reads=%ld notSaved=%ld holes(Missing)=%ld stale=%ld\n",
                fixed ? "NEW" : "OLD", reads.load(), notSaved.load(), holes.load(), stale.load());
    const bool ok = notSaved == 0 && holes == 0 && stale == 0;
    std::printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
