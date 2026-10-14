#pragma once

// -----------------------------------------------------------------------------
// PS Vita overrides, applied on top of the desktop / PC_LEGACY tables.
// -----------------------------------------------------------------------------
// Must be included after the other tuning headers.
#ifdef VITA_PLATFORM
// Autosave every 10 s instead of 2 s: it serializes and deflates chunks on the
// main thread. Each pass still flushes up to two dirty chunks, so edits reach
// the memory card within seconds even if the app is closed from the PS button.
#  undef  PLATFORM_AUTOSAVE_PERIOD_TICKS
#  define PLATFORM_AUTOSAVE_PERIOD_TICKS                200
// Spread chunk eviction (serialize + free) over several ticks, like PS2.
#  undef  PLATFORM_MAX_CHUNK_UNLOADS_PER_TICK
#  define PLATFORM_MAX_CHUNK_UNLOADS_PER_TICK           1
#  undef  PLATFORM_EMERGENCY_CHUNK_UNLOADS_PER_TICK
#  define PLATFORM_EMERGENCY_CHUNK_UNLOADS_PER_TICK     4
// Keep the chunks just preloaded instead of discarding and regenerating them.
#  undef  PLATFORM_UNLOAD_AFTER_PRELOAD
#  define PLATFORM_UNLOAD_AFTER_PRELOAD                 0
// No timer-hack thread, no paid check, no stats sync over the network.
#  undef  PLATFORM_CLIENT_TIMER_HACK_THREAD
#  define PLATFORM_CLIENT_TIMER_HACK_THREAD             0
#  undef  PLATFORM_CLIENT_PAID_CHECK
#  define PLATFORM_CLIENT_PAID_CHECK                    0
// Render distance is chosen in the options at runtime (RenderGlobal::
// loadRenderers, ChunkProvider::setChunkLoadRadius); this is only the upper
// bound, which also sizes the display-list range. Up to 16 chunks: at 16 about
// 1100 columns stay resident (heap use is in the perf log, "heap=").
#  undef  PLATFORM_VISIBLE_CHUNK_RADIUS
#  define PLATFORM_VISIBLE_CHUNK_RADIUS                 16
// Upper bound of the Vertical Distance option (GameSettings::verticalSections,
// 3..8 layers; 5 by default): sizes the display-list range for a full column.
#  undef  PLATFORM_VERTICAL_CHUNK_COUNT
#  define PLATFORM_VERTICAL_CHUNK_COUNT                 8

// Revisited terrain: region read, inflate and NBT parse of saved Anvil chunks
// move to the async worker (cores 1-2); the 6-8 ms they cost per chunk on the
// game thread showed up as 90-500 ms tick spikes. Chunks inside
// PLATFORM_GENERATE_SYNC_RADIUS still load synchronously.
#  undef  PLATFORM_ASYNC_SAVED_CHUNK_LOAD
#  define PLATFORM_ASYNC_SAVED_CHUNK_LOAD               1

// The per-frame mesh/lighting budgets stay at the PC_LEGACY values: cutting
// them (and adding a shared streaming cap) starved lighting once vanilla
// terrain arrived, and the queue grew without bound (brightness popping).

// Lighting backlog (brightness popping): on top of the 96-job / 1.5 ms drain,
// the queue gets up to 4 ms more per frame while frames keep their vsync
// period ("Light Updates: Fast", the default), or a fixed 9 ms for "Fastest".
// Only when the work is done changes, never what it computes. The cell set
// (16384 slots, 128 KB) only serves the vector queue, off below.
#  undef  PLATFORM_LIGHTING_EXTRA_BUDGET_US
#  define PLATFORM_LIGHTING_EXTRA_BUDGET_US              4000
#  undef  PLATFORM_LIGHTING_FASTEST_BUDGET_US
#  define PLATFORM_LIGHTING_FASTEST_BUDGET_US            9000
#  undef  PLATFORM_LIGHTING_CELL_SET_BITS
#  define PLATFORM_LIGHTING_CELL_SET_BITS                14

// Vanilla world generation, 1:1 with Java seeds: PC_LEGACY swaps in a
// heightmap generator, float noise and trimmed caves/ores/springs/dungeons.
// Generation runs on the async worker, so this costs chunk arrival time,
// not frame time. The values are the non-legacy ones in PlatformGameTuning.h.
#  undef  PLATFORM_USE_HEIGHTMAP_TERRAIN
#  define PLATFORM_USE_HEIGHTMAP_TERRAIN                0
#  undef  PLATFORM_FAST_SURFACE_PASS
#  define PLATFORM_FAST_SURFACE_PASS                    0
#  undef  PLATFORM_PRECOMPUTE_INITIAL_HEIGHTMAP
#  define PLATFORM_PRECOMPUTE_INITIAL_HEIGHTMAP         0
#  undef  PLATFORM_FLOAT_TERRAIN_NOISE
#  define PLATFORM_FLOAT_TERRAIN_NOISE                  0
#  undef  PLATFORM_FLOAT_BIOME_NOISE
#  define PLATFORM_FLOAT_BIOME_NOISE                    0
#  undef  PLATFORM_FLOAT_ORE_VEINS
#  define PLATFORM_FLOAT_ORE_VEINS                      0
#  undef  PLATFORM_FLOAT_CAVE_GENERATION
#  define PLATFORM_FLOAT_CAVE_GENERATION                0
#  undef  PLATFORM_CAVE_SOURCE_RADIUS
#  define PLATFORM_CAVE_SOURCE_RADIUS                   8
#  undef  PLATFORM_CAVE_RARITY
#  define PLATFORM_CAVE_RARITY                          15
#  undef  PLATFORM_TERRAIN_DENSITY_OCTAVES
#  define PLATFORM_TERRAIN_DENSITY_OCTAVES              16
#  undef  PLATFORM_TERRAIN_SELECT_OCTAVES
#  define PLATFORM_TERRAIN_SELECT_OCTAVES               8

// Fresh-install defaults that show the port as it runs now: Normal distance
// (the renderer keeps the budget), see GameDefaults.cpp for the 60 fps cap
// and GameSettings.cpp for the terrain shader.
#  undef  PLATFORM_DEFAULT_RENDER_DISTANCE
#  define PLATFORM_DEFAULT_RENDER_DISTANCE              1
#  undef  PLATFORM_POPULATE_WATER_SPRINGS
#  define PLATFORM_POPULATE_WATER_SPRINGS               50
#  undef  PLATFORM_POPULATE_LAVA_SPRINGS
#  define PLATFORM_POPULATE_LAVA_SPRINGS                20
#  undef  PLATFORM_POPULATE_DUNGEONS
#  define PLATFORM_POPULATE_DUNGEONS                    8
#  undef  PLATFORM_POPULATE_TREE_BONUS
#  define PLATFORM_POPULATE_TREE_BONUS                  0
#  undef  PLATFORM_POPULATE_CLAY_VEINS
#  define PLATFORM_POPULATE_CLAY_VEINS                  10
#  undef  PLATFORM_POPULATE_DIRT_VEINS
#  define PLATFORM_POPULATE_DIRT_VEINS                  20
#  undef  PLATFORM_POPULATE_GRAVEL_VEINS
#  define PLATFORM_POPULATE_GRAVEL_VEINS                10
#  undef  PLATFORM_POPULATE_SNOW_PASS
#  define PLATFORM_POPULATE_SNOW_PASS                   1

// Streaming order and throughput (vanilla generation is ~all of a core per
// chunk on the A9). Two generation workers share cores 1-2 with the mesh
// worker; each owns its generator, so the world is the same seed for seed.
// The worker takes the queued column nearest the player, and the prefetch
// ring fills nearest-first every frame (PS2 score: distance, then heading).
#  undef  PLATFORM_ASYNC_GENERATION_WORKERS
#  define PLATFORM_ASYNC_GENERATION_WORKERS             2
#  undef  PLATFORM_ASYNC_NEAREST_FIRST
#  define PLATFORM_ASYNC_NEAREST_FIRST                  1
#  undef  PLATFORM_ASYNC_GENERATION_QUEUE_LIMIT
#  define PLATFORM_ASYNC_GENERATION_QUEUE_LIMIT         8
#  undef  PLATFORM_ASYNC_GENERATION_REQUESTS_PER_FRAME
#  define PLATFORM_ASYNC_GENERATION_REQUESTS_PER_FRAME  8
#  undef  PLATFORM_ASYNC_GENERATION_PUBLISH_PER_TICK
#  define PLATFORM_ASYNC_GENERATION_PUBLISH_PER_TICK    2
#  undef  PLATFORM_PREFETCH_CHUNK_INTERVAL_FRAMES
#  define PLATFORM_PREFETCH_CHUNK_INTERVAL_FRAMES       1
#  undef  PLATFORM_PREFETCH_CHUNKS_PER_STEP
#  define PLATFORM_PREFETCH_CHUNKS_PER_STEP             2
// Decoration of a column within this Chebyshev distance of the player gets
// PLATFORM_POPULATE_NEAR_BUDGET_US instead of the normal budget: bare snow
// fields next to the player are what you notice. The queue is nearest-first.
#  define PLATFORM_POPULATE_NEAR_RADIUS                 2
#  define PLATFORM_POPULATE_NEAR_BUDGET_US              6000
// On top of that, a slice every rendered frame while such a column waits
// (about 2x the decoration rate next to the player, at most 2 ms a frame).
#  define PLATFORM_POPULATE_FRAME_BUDGET_US             2000

// Decoration (lakes, ores, trees, springs) runs in deferred steps on the game
// thread. PC_LEGACY's 4 steps / 1.2 ms per tick left terrain bare for 10-20 s
// with the vanilla feature counts above. The time budget bounds the frame; the
// step cap only stops a cheap backlog from spinning.
#  undef  PLATFORM_POPULATE_STEPS_PER_TICK
#  define PLATFORM_POPULATE_STEPS_PER_TICK              32
#  undef  PLATFORM_POPULATE_BUDGET_US
#  define PLATFORM_POPULATE_BUDGET_US                   2500
// Ticks that publish a chunk get the full step count too: while exploring
// nearly every tick publishes, and with 0 steps decoration stalled until
// generation caught up. The time budget bounds the tick.
#  undef  PLATFORM_POPULATE_STEPS_AFTER_PUBLISH
#  define PLATFORM_POPULATE_STEPS_AFTER_PUBLISH         32

// Frame-spike control (see World::updateBlocksAndPlayCaveSounds / World::
// getPathToEntity). Both only bound how much work one tick may do; skipped
// work rolls over to the next tick, so the long-run result is unchanged.
//  - Random block ticks: rotate through the chunk slice but stop after 5 ms.
//  - Pathfinding: 450 expanded nodes per tick across all searches, half of
//    the 3 searches x 300 nodes PC_LEGACY allows (~50 us per node).
#  undef  PLATFORM_RANDOM_TICK_BUDGET_US
#  define PLATFORM_RANDOM_TICK_BUDGET_US                5000
#  undef  PLATFORM_PATHFIND_NODE_BUDGET_PER_TICK
#  define PLATFORM_PATHFIND_NODE_BUDGET_PER_TICK        450
// Multiplayer: the server owns remote mobs' positions, so their client-side
// physics (collisions, water/lava scans, push checks; 0.2-0.4 ms per mob) runs
// on one tick in 8. Interpolation and limb animation stay per tick (see
// EntityLiving). Same value as PS2.
#  undef  PLATFORM_MULTIPLAYER_REMOTE_LIVING_PHYSICS_TICK_DIVISOR
#  define PLATFORM_MULTIPLAYER_REMOTE_LIVING_PHYSICS_TICK_DIVISOR 8

// Relight checks walk every air block of 8 columns per chunk per tick across
// the 9x9 random-tick area. Cheap in the Overworld (empty sky sections are
// skipped), 130-210 ms a tick in the Nether's air-filled caverns (measured, 3
// fps). Bounded per tick; every chunk still finishes its checks.
#  undef  PLATFORM_RELIGHT_CHECK_BUDGET_US
#  define PLATFORM_RELIGHT_CHECK_BUDGET_US               2000

// Multiplayer: trust the light the server sends (see PlatformGameTuning.h).
#  undef  PLATFORM_MP_CLIENT_RELIGHT_CHECKS
#  define PLATFORM_MP_CLIENT_RELIGHT_CHECKS              0
// Player light probe on the bounded light drain: one probe hitting an unlit
// lava sea was a 0.4-1.4 s synchronous flood fill (Nether, wRandom/wProbe).
#  undef  PLATFORM_DEFERRED_LIGHT_UPDATES
#  define PLATFORM_DEFERRED_LIGHT_UPDATES                1
// Multiplayer chunk packets: 6-16 ms ticks when several arrive together.
// After 3 ms of dispatch the next Packet50/51 waits a tick; one packet always
// runs, so a 20/s stream still keeps up.
#  undef  PLATFORM_NET_CHUNK_DISPATCH_BUDGET_US
#  define PLATFORM_NET_CHUNK_DISPATCH_BUDGET_US          3000
// Light queue: one bit per pending cell (512 bytes per section with work)
// instead of the MetadataChunkBlock vector, which hit its 65536 ceiling during
// decoration and dropped cells (stale dark patches). The bitset cannot drop and
// dedups exactly; the neighbour test is vanilla's (2-5x fewer cells queued,
// vita/lighting_queue_check.cpp).
#  undef  PLATFORM_LIGHTING_SECTION_QUEUE
#  define PLATFORM_LIGHTING_SECTION_QUEUE                1
// Ceiling of the vector queue (unused while the section queue is on). The
// desktop 1M entries is ~40 MB, a fifth of the heap.
#  undef  PLATFORM_LIGHTING_QUEUE_HARD_CAP
#  define PLATFORM_LIGHTING_QUEUE_HARD_CAP               65536

// Full saves (dimension change, quit): 24 queued chunks per writer drain
// instead of 2 (the autosave batch, which stays 2), so serialization and the
// writer do not run in lockstep. Region deflate is level 1 on Vita too
// (PLATFORM_FAST_REGION_COMPRESSION, PlatformConfig.h).
#  define PLATFORM_FULL_SAVE_DRAIN_BATCH                24
// Camera effects (hurt tilt, view bobbing, portal wobble) cost a few matrix
// calls; PC_LEGACY skips them. Vanilla here; Performance Settings can turn
// them off.
#  undef  PLATFORM_SKIP_CAMERA_FX
#  define PLATFORM_SKIP_CAMERA_FX                       0
// Decoration: every gravel/sand block placed schedules a tick, and the
// +-8 box chunk check was up to 4 chunk-map lookups each (Gravel 10-13 ms/chunk).
#  undef  PLATFORM_CACHE_TICK_CHUNK_EXISTENCE
#  define PLATFORM_CACHE_TICK_CHUNK_EXISTENCE           1
#endif
