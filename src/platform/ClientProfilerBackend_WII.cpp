#include <malloc.h>
#include "platform/ClientProfilerBackend.h"

#include "platform/Log.h"
#include "platform/world/LightingFrameBudget.h"
#include "platform/Profiler.h"
#include "java/System.h"
#include "net/minecraft/src/RenderGlobal.h"
#include "net/minecraft/src/World.h"
#ifdef WII_PLATFORM
#include "wii/gx_wii.h"
#endif

#ifdef VITA_PLATFORM
// Counted by the vitaGL render backend (PcOpenGlRenderBackend.cpp).
extern "C" unsigned g_vitaPerfDraws, g_vitaPerfTexUploads, g_vitaPerfBufferBytes;
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
constexpr int kRenderPhaseSlots = 14;
constexpr int kTickPhaseSlots = 96;
constexpr int kTickPhaseNameChars = 16;
constexpr int kSpikeSlots = 6;
// A frame over this is logged on its own with everything it did, so one run
// says what each stutter was instead of only that there was one.
constexpr long long kSpikeNs = 20000000LL;

constexpr int kTopPhases = 4;
constexpr int kSlowGroups = static_cast<int>(PlatformSlowItemGroup::Count);
constexpr int kSlowNameChars = 32;

struct TopPhase
{
	char name[kTickPhaseNameChars];
	long long ns;
};

struct SlowItem
{
	char name[kSlowNameChars];
	long long ns;
};

// What one frame did; filled while it runs, copied out if it was a spike.
struct FrameSnapshot
{
	long long frameNs = 0, tickNs = 0, renderNs = 0, lightingNs = 0;
	int ticks = 0, chunkUpdates = 0;
	unsigned draws = 0, texUploads = 0, bufferBytes = 0;
	long long phaseUs[14] = {};
	char topTickPhase[kTickPhaseNameChars] = {};
	long long topTickPhaseNs = 0;
	// The kTopPhases largest tick phases of the frame, largest first. A phase
	// nested in another (entTick inside entities, inSfx inside input) is listed
	// next to its parent, which is how the spike line names the real culprit.
	TopPhase topPhases[kTopPhases] = {};
	// Slowest single item per PlatformSlowItemGroup this frame (entity class,
	// random-tick block class).
	SlowItem slow[kSlowGroups] = {};
};
FrameSnapshot g_frame;

struct WiiTickPhase
{
	char name[kTickPhaseNameChars];
	long long sumNs;
	long long maxNs;
	int count;
};

struct WiiWindow
{
	long long startedMs = 0;
	long long frameNs = 0;
	long long tickNs = 0;
	long long renderNs = 0;
	// Time inside lwjgl::Display::update(). On Wii that is the wait for the
	// EFB->XFB copy plus VIDEO_WaitVSync(), i.e. how much of the frame the CPU
	// spends parked on the GP and the retrace. It is the number that separates
	// a GP-bound build from a CPU-bound one, so it cannot stay unrecorded.
	long long swapNs = 0;
	long long maxSwapNs = 0;
	// swapNs split: parked on GX_WaitDrawDone() versus parked on the retrace.
	// A gpWait that grows past the tick+lighting it overlaps is a GP-bound
	// frame, and no CPU-side budget will bring that one back under the cap.
	long long gpWaitNs = 0, maxGpWaitNs = 0;
	long long vsyncNs = 0, maxVsyncNs = 0;
	// World::updatingLighting() sits between the ticks and the swap and was
	// not in any of the three columns above.
	long long lightingNs = 0, maxLightingNs = 0;
	// Light drain: jobs per frame, and the extra budget the adaptive controller
	// had granted (LightingFrameBudget).
	long long lightJobs = 0, maxLightJobs = 0, lightBudgetUs = 0, lightUsedUs = 0;
	unsigned lightLateFrames = 0, lightExtraUs = 0;
	long long maxFrameNs = 0;
	long long maxTickNs = 0;
	long long maxRenderNs = 0;
	// Presents that landed a retrace late. What has to fit between two
	// presents is the render of one frame plus the ticks and lighting of the
	// next, so the interval is measured in displayUpdate() and the breakdown
	// kept for the worst one is exactly those pieces. At WII_TARGET_FPS 30 an
	// average frame of 36 ms against a 33 ms period only says that some
	// intervals took 50: this says how many, and what the worst one was doing.
	int slippedPresents = 0;
	long long worstSlipIntervalNs = 0;
	long long worstSlipRenderNs = 0, worstSlipTickNs = 0, worstSlipLightingNs = 0;
	long long worstSlipGpWaitNs = 0, worstSlipVsyncNs = 0;
	int worstSlipTicks = 0;
	// Pieces of the interval in progress: the previous frame's render, then
	// this frame's ticks and lighting, all known by the time the swap returns.
	long long lastPresentEndNs = 0;
	long long prevRenderNs = 0;
	long long curTickNs = 0, curLightingNs = 0;
	int curTicks = 0;
	// Render phases (PlatformRenderPhase order) and named tick phases, both
	// per window. The tick table is keyed by name the way the PS2 one is: the
	// same literal is written at more than one call site.
	long long renderPhaseNs[kRenderPhaseSlots] = {};
	long long maxRenderPhaseNs[kRenderPhaseSlots] = {};
	WiiTickPhase tickPhases[kTickPhaseSlots] = {};
	int tickPhaseCount = 0;
	long long chunkLoadNs = 0, maxChunkLoadNs = 0;
	int chunkLoadCount = 0;
	long long populateNs = 0, maxPopulateNs = 0;
	int populateCount = 0;
	long long populatePhaseNs[8] = {};
	long long maxPopulatePhaseNs[8] = {};
	int populatePhaseCount[8] = {};
	long long generateNs = 0, maxGenerateNs = 0;
	int generateCount = 0;
	long long meshNs = 0, maxMeshNs = 0;
	int meshCount = 0;
	long long unloadSaveNs = 0, maxUnloadSaveNs = 0;
	int unloadSaveCount = 0;
	long long tickUpdatesNs = 0, maxTickUpdatesNs = 0;
	long long tickQueueSum = 0, tickQueueMax = 0;
	long long mobSpawnNs = 0, maxMobSpawnNs = 0;
	long long saveInfoNs = 0, maxSaveInfoNs = 0;
	long long mapStorageNs = 0, maxMapStorageNs = 0;
	int frames = 0;
	int ticks = 0;
	int maxTicksPerFrame = 0;
	int chunkUpdates = 0;
	int slowTicks = 0;
	long long draws = 0, texUploads = 0, bufferBytes = 0;
	unsigned maxDraws = 0;
	FrameSnapshot spikes[kSpikeSlots] = {};
	int spikeCount = 0, spikesSeen = 0;
	// Slowest single item per group over the whole window.
	SlowItem slowWorst[kSlowGroups] = {};
} g_wii;

constexpr long long kSlowTickNs = 100000000LL;
// Time the previous report itself took to format and write, so the profiler's
// own cost shows up in the numbers instead of hiding in a frame spike.
long long g_lastReportNs = 0;
// One retrace on NTSC/PAL60. Close enough on PAL50 for the slip threshold,
// which only has to tell a 50 ms frame from a 33 ms one.
constexpr long long kRetraceNs = 16666667LL;

void add(long long ns, long long& total, long long& maximum)
{
	total += ns;
	if (ns > maximum) maximum = ns;
}

void addSample(long long ns, long long& total, long long& maximum, int& count)
{
	add(ns, total, maximum);
	++count;
}

void resetWindow(long long nowMs)
{
	g_wii = WiiWindow{};
	g_wii.startedMs = nowMs;
}
}

extern "C" void wii_perf_add_render_phase_us(int phase, unsigned int us)
{
	if (phase >= 0 && phase < kRenderPhaseSlots)
	{
		add((long long)us * 1000LL, g_wii.renderPhaseNs[phase], g_wii.maxRenderPhaseNs[phase]);
		g_frame.phaseUs[phase] += us;
	}
}
extern "C" void wii_perf_note_tick_phase(const char* name, long long ns)
{
	if (name == nullptr)
		return;
	if (ns < 0)
		ns = 0;
	if (ns > g_frame.topTickPhaseNs)
	{
		g_frame.topTickPhaseNs = ns;
		strncpy(g_frame.topTickPhase, name, kTickPhaseNameChars - 1);
		g_frame.topTickPhase[kTickPhaseNameChars - 1] = '\0';
	}
	// A phase runs several times per frame when ticks > 1 or when it is
	// called per item; keep the single largest call under each name.
	{
		int existing = -1;
		for (int i = 0; i < kTopPhases; i++)
			if (g_frame.topPhases[i].ns > 0 && strncmp(g_frame.topPhases[i].name, name, kTickPhaseNameChars - 1) == 0)
				existing = i;
		if (existing >= 0)
		{
			if (ns > g_frame.topPhases[existing].ns)
				g_frame.topPhases[existing].ns = ns;
		}
		else if (ns > g_frame.topPhases[kTopPhases - 1].ns)
		{
			strncpy(g_frame.topPhases[kTopPhases - 1].name, name, kTickPhaseNameChars - 1);
			g_frame.topPhases[kTopPhases - 1].name[kTickPhaseNameChars - 1] = '\0';
			g_frame.topPhases[kTopPhases - 1].ns = ns;
			existing = kTopPhases - 1;
		}
		// restore descending order (bubble the touched slot up)
		for (int i = existing; i > 0 && existing >= 0 && g_frame.topPhases[i].ns > g_frame.topPhases[i - 1].ns; i--)
			std::swap(g_frame.topPhases[i], g_frame.topPhases[i - 1]);
	}
	for (int i = 0; i < g_wii.tickPhaseCount; i++)
	{
		WiiTickPhase& slot = g_wii.tickPhases[i];
		if (strncmp(slot.name, name, kTickPhaseNameChars - 1) != 0)
			continue;
		addSample(ns, slot.sumNs, slot.maxNs, slot.count);
		return;
	}
	if (g_wii.tickPhaseCount >= kTickPhaseSlots)
		return;
	WiiTickPhase& slot = g_wii.tickPhases[g_wii.tickPhaseCount++];
	strncpy(slot.name, name, kTickPhaseNameChars - 1);
	slot.name[kTickPhaseNameChars - 1] = '\0';
	slot.sumNs = ns;
	slot.maxNs = ns;
	slot.count = 1;
}
extern "C" void wii_perf_note_slow_item(int group, const char* name, long long ns)
{
	if (name == nullptr || group < 0 || group >= kSlowGroups)
		return;
	if (ns > g_frame.slow[group].ns)
	{
		strncpy(g_frame.slow[group].name, name, kSlowNameChars - 1);
		g_frame.slow[group].name[kSlowNameChars - 1] = '\0';
		g_frame.slow[group].ns = ns;
	}
	if (ns > g_wii.slowWorst[group].ns)
	{
		strncpy(g_wii.slowWorst[group].name, name, kSlowNameChars - 1);
		g_wii.slowWorst[group].name[kSlowNameChars - 1] = '\0';
		g_wii.slowWorst[group].ns = ns;
	}
}
extern "C" void wii_perf_add_chunk_load_ns(long long ns) { addSample(ns, g_wii.chunkLoadNs, g_wii.maxChunkLoadNs, g_wii.chunkLoadCount); }
extern "C" void wii_perf_add_populate_ns(long long ns) { addSample(ns, g_wii.populateNs, g_wii.maxPopulateNs, g_wii.populateCount); }
extern "C" void wii_perf_add_populate_phase_ns(int phase, long long ns)
{
	if (phase >= 0 && phase < 8)
		addSample(ns, g_wii.populatePhaseNs[phase], g_wii.maxPopulatePhaseNs[phase], g_wii.populatePhaseCount[phase]);
}
extern "C" void wii_perf_add_generate_ns(long long ns) { addSample(ns, g_wii.generateNs, g_wii.maxGenerateNs, g_wii.generateCount); }
extern "C" void wii_perf_add_mesh_ns(long long ns) { addSample(ns, g_wii.meshNs, g_wii.maxMeshNs, g_wii.meshCount); }
extern "C" void wii_perf_add_unload_save_ns(long long ns) { addSample(ns, g_wii.unloadSaveNs, g_wii.maxUnloadSaveNs, g_wii.unloadSaveCount); }
extern "C" void wii_perf_add_tickupdates_ns(long long ns) { add(ns, g_wii.tickUpdatesNs, g_wii.maxTickUpdatesNs); }
extern "C" void wii_perf_add_tickupdates_queue(long long size) { g_wii.tickQueueSum += size; g_wii.tickQueueMax = std::max(g_wii.tickQueueMax, size); }
extern "C" void wii_perf_add_mobspawn_ns(long long ns) { add(ns, g_wii.mobSpawnNs, g_wii.maxMobSpawnNs); }
extern "C" void wii_perf_add_saveworldinfo_ns(long long ns) { add(ns, g_wii.saveInfoNs, g_wii.maxSaveInfoNs); }
extern "C" void wii_perf_add_mapstorage_ns(long long ns) { add(ns, g_wii.mapStorageNs, g_wii.maxMapStorageNs); }

namespace ClientProfilerBackend
{
void frameBegin()
{
	if (g_wii.startedMs == 0)
		resetWindow(System::currentTimeMillis());
}

void ticks(long long ns, int ticksThisFrame)
{
	g_wii.tickNs += ns;
	g_wii.maxTickNs = std::max(g_wii.maxTickNs, ns);
	g_wii.ticks += ticksThisFrame;
	g_wii.maxTicksPerFrame = std::max(g_wii.maxTicksPerFrame, ticksThisFrame);
	if (ns > kSlowTickNs) ++g_wii.slowTicks;
	g_wii.curTickNs = ns;
	g_wii.curTicks = ticksThisFrame;
	g_frame.tickNs = ns;
	g_frame.ticks = ticksThisFrame;
}

void lighting(long long ns)
{
	add(ns, g_wii.lightingNs, g_wii.maxLightingNs);
	g_wii.curLightingNs = ns;
	g_frame.lightingNs = ns;
	const LightingFrameBudget::Stats& ls = LightingFrameBudget::stats();
	add(ls.lastJobs, g_wii.lightJobs, g_wii.maxLightJobs);
	g_wii.lightBudgetUs += ls.lastBudgetUs;
	g_wii.lightUsedUs += ls.lastUsedUs;
	g_wii.lightExtraUs = ls.extraUs;
	g_wii.lightLateFrames = ls.frameLate;
}

void displayUpdate(long long ns)
{
	add(ns, g_wii.swapNs, g_wii.maxSwapNs);
	long long gpWaitNs = 0, vsyncNs = 0;
#ifdef WII_PLATFORM
	wiigl_last_present_ns(&gpWaitNs, &vsyncNs);
	const int retracesPerFrame = wiigl_retraces_per_frame();
#else
	// Vita reuses this backend: the whole swap is in swapNs, no split.
	const int retracesPerFrame = 1;
#endif
	add(gpWaitNs, g_wii.gpWaitNs, g_wii.maxGpWaitNs);
	add(vsyncNs, g_wii.vsyncNs, g_wii.maxVsyncNs);

	// Period of the cap plus half a retrace of slack: an interval past that
	// lost at least one retrace.
	const long long now = System::nanoTime();
	const long long periodNs = kRetraceNs * retracesPerFrame;
	if (g_wii.lastPresentEndNs != 0)
	{
		const long long intervalNs = now - g_wii.lastPresentEndNs;
		if (intervalNs > periodNs + kRetraceNs / 2)
		{
			++g_wii.slippedPresents;
			if (intervalNs > g_wii.worstSlipIntervalNs)
			{
				g_wii.worstSlipIntervalNs = intervalNs;
				g_wii.worstSlipRenderNs = g_wii.prevRenderNs;
				g_wii.worstSlipTickNs = g_wii.curTickNs;
				g_wii.worstSlipLightingNs = g_wii.curLightingNs;
				g_wii.worstSlipGpWaitNs = gpWaitNs;
				g_wii.worstSlipVsyncNs = vsyncNs;
				g_wii.worstSlipTicks = g_wii.curTicks;
			}
		}
	}
	g_wii.lastPresentEndNs = now;
}

void render(long long) {}

void frameEnd(long long frameNs, long long, long long renderNs,
              int, int chunkUpdates, World* world, RenderGlobal* renderGlobal)
{
	g_wii.frameNs += frameNs;
	g_wii.renderNs += renderNs;
	g_wii.maxFrameNs = std::max(g_wii.maxFrameNs, frameNs);
	g_wii.maxRenderNs = std::max(g_wii.maxRenderNs, renderNs);
	++g_wii.frames;
	// WorldRenderer::chunksUpdated counts up and is zeroed once a second
	// (the F3 line); summing it every frame overstated builds ~fps/2 times.
	static int lastChunkUpdates = 0;
	const int builtThisFrame = chunkUpdates >= lastChunkUpdates ? chunkUpdates - lastChunkUpdates : chunkUpdates;
	lastChunkUpdates = chunkUpdates;
	g_wii.chunkUpdates += builtThisFrame;

	g_wii.prevRenderNs = renderNs;
	g_wii.curTickNs = 0;
	g_wii.curLightingNs = 0;
	g_wii.curTicks = 0;

	g_frame.frameNs = frameNs;
	g_frame.renderNs = renderNs;
	g_frame.chunkUpdates = builtThisFrame;
#ifdef VITA_PLATFORM
	g_frame.draws = g_vitaPerfDraws;
	g_frame.texUploads = g_vitaPerfTexUploads;
	g_frame.bufferBytes = g_vitaPerfBufferBytes;
	g_vitaPerfDraws = g_vitaPerfTexUploads = g_vitaPerfBufferBytes = 0;
#endif
	g_wii.draws += g_frame.draws;
	g_wii.texUploads += g_frame.texUploads;
	g_wii.bufferBytes += g_frame.bufferBytes;
	g_wii.maxDraws = std::max(g_wii.maxDraws, g_frame.draws);
	if (frameNs > kSpikeNs)
	{
		++g_wii.spikesSeen;
		// Keep the worst kSpikeSlots of the window.
		int slot = g_wii.spikeCount < kSpikeSlots ? g_wii.spikeCount++ : -1;
		if (slot < 0)
		{
			slot = 0;
			for (int i = 1; i < kSpikeSlots; i++)
				if (g_wii.spikes[i].frameNs < g_wii.spikes[slot].frameNs)
					slot = i;
			if (g_wii.spikes[slot].frameNs >= frameNs)
				slot = -1;
		}
		if (slot >= 0)
			g_wii.spikes[slot] = g_frame;
	}
	g_frame = FrameSnapshot{};

	const long long now = System::currentTimeMillis();
	if (now - g_wii.startedMs < 10000)
		return;

	const long long reportStartNs = System::nanoTime();
	const long long avgFrame = g_wii.frames ? g_wii.frameNs / g_wii.frames : 0;
	const long long avgTick = g_wii.frames ? g_wii.tickNs / g_wii.frames : 0;
	const long long avgRender = g_wii.frames ? g_wii.renderNs / g_wii.frames : 0;
	const long long avgSwap = g_wii.frames ? g_wii.swapNs / g_wii.frames : 0;
	MC_LOG_INFO("wii.perf", "frames=%d fps=%d frame=%ld/%ldms tick=%ld/%ldms render=%ld/%ldms swap=%ld/%ldms ticks=%d maxTicks=%d built=%d pending=%d slowTicks=%d lastReport=%ldus\n",
	            g_wii.frames, g_wii.frames / 10,
	            (long)(avgFrame / 1000000LL), (long)(g_wii.maxFrameNs / 1000000LL),
	            (long)(avgTick / 1000000LL), (long)(g_wii.maxTickNs / 1000000LL),
	            (long)(avgRender / 1000000LL), (long)(g_wii.maxRenderNs / 1000000LL),
	            (long)(avgSwap / 1000000LL), (long)(g_wii.maxSwapNs / 1000000LL),
	            g_wii.ticks, g_wii.maxTicksPerFrame, g_wii.chunkUpdates,
	            renderGlobal != nullptr ? (int)renderGlobal->pendingRendererUpdateCount() : 0,
	            g_wii.slowTicks, (long)(g_lastReportNs / 1000LL));
	MC_LOG_INFO("wii.perf", "lightdrain jobs=%ld/%ld/frame used=%ldus of budget=%ldus extra=%uus lateFrames=%u\n",
	            (long)(g_wii.frames ? g_wii.lightJobs / g_wii.frames : 0), (long)g_wii.maxLightJobs,
	            (long)(g_wii.frames ? g_wii.lightUsedUs / g_wii.frames : 0),
	            (long)(g_wii.frames ? g_wii.lightBudgetUs / g_wii.frames : 0),
	            g_wii.lightExtraUs, g_wii.lightLateFrames);
	MC_LOG_INFO("wii.perf", "slipped=%d/%d gpWait=%ld/%ldms vsync=%ld/%ldms light=%ld/%ldms worst: interval=%ldms render=%ldms tick=%ldms light=%ldms gpWait=%ldms vsync=%ldms ticks=%d\n",
	            g_wii.slippedPresents, g_wii.frames,
	            (long)(g_wii.frames ? g_wii.gpWaitNs / g_wii.frames / 1000000LL : 0), (long)(g_wii.maxGpWaitNs / 1000000LL),
	            (long)(g_wii.frames ? g_wii.vsyncNs / g_wii.frames / 1000000LL : 0), (long)(g_wii.maxVsyncNs / 1000000LL),
	            (long)(g_wii.frames ? g_wii.lightingNs / g_wii.frames / 1000000LL : 0), (long)(g_wii.maxLightingNs / 1000000LL),
	            (long)(g_wii.worstSlipIntervalNs / 1000000LL),
	            (long)(g_wii.worstSlipRenderNs / 1000000LL),
	            (long)(g_wii.worstSlipTickNs / 1000000LL),
	            (long)(g_wii.worstSlipLightingNs / 1000000LL),
	            (long)(g_wii.worstSlipGpWaitNs / 1000000LL),
	            (long)(g_wii.worstSlipVsyncNs / 1000000LL),
	            g_wii.worstSlipTicks);
	{
		// Per-frame average and window maximum, in the PlatformRenderPhase
		// order. Tenths of a millisecond: most phases sit under 1 ms.
		static const char* const kRenderPhaseNames[kRenderPhaseSlots] = {
			"sky", "frustum", "build", "opaque", "ents", "transl", "hand", "hud",
			"entDraw", "tileDraw", "hudItems", "hudText", "hudHints", "?" };
		char line[512];
		int len = 0;
		for (int i = 0; i < kRenderPhaseSlots && len < (int)sizeof(line) - 40; i++)
		{
			if (g_wii.renderPhaseNs[i] == 0)
				continue;
			const long avg10 = (long)(g_wii.frames ? g_wii.renderPhaseNs[i] / g_wii.frames / 100000LL : 0);
			const long max10 = (long)(g_wii.maxRenderPhaseNs[i] / 100000LL);
			len += snprintf(line + len, sizeof(line) - len, " %s=%ld.%ld/%ld.%ld",
			                kRenderPhaseNames[i], avg10 / 10, avg10 % 10, max10 / 10, max10 % 10);
		}
		MC_LOG_INFO("wii.perf", "renderPhase(ms)%s\n", line);

		// Cost per frame (window total / frames), then the window maximum, in
		// 0.01 ms; phases that never reached 0.2 ms at their worst are left out.
		// One log call carries at most 768 characters, so a long list is
		// continued on further tickPhase(ms) lines.
		len = 0;
		for (int i = 0; i < g_wii.tickPhaseCount; i++)
		{
			const WiiTickPhase& slot = g_wii.tickPhases[i];
			if (slot.name[0] == '#' || slot.maxNs < 200000LL)
				continue;
			const long perFrame = (long)(g_wii.frames ? slot.sumNs / g_wii.frames / 10000LL : 0);
			const long worst = (long)(slot.maxNs / 10000LL);
			len += snprintf(line + len, sizeof(line) - len, " %s=%ld.%02ld/%ld.%02ld",
			                slot.name, perFrame / 100, perFrame % 100, worst / 100, worst % 100);
			if (len > 440)
			{
				MC_LOG_INFO("wii.perf", "tickPhase(ms)%s\n", line);
				len = 0;
				line[0] = '\0';
			}
		}
		if (len > 0 || g_wii.tickPhaseCount == 0)
			MC_LOG_INFO("wii.perf", "tickPhase(ms)%s\n", line);
		// Event counters recorded as "#name" tick phases (value = count, not
		// time): total per window and the largest single report.
		len = 0;
		line[0] = '\0';
		for (int i = 0; i < g_wii.tickPhaseCount; i++)
		{
			const WiiTickPhase& slot = g_wii.tickPhases[i];
			if (slot.name[0] != '#')
				continue;
			len += snprintf(line + len, sizeof(line) - len, " %s=%lld/%lld", slot.name + 1, slot.sumNs, slot.maxNs);
		}
		if (len > 0)
			MC_LOG_INFO("wii.perf", "counts(window total/max)%s\n", line);
		// Single slowest entity update / random block tick of the window: reads
		// "class:ms". Empty (-) when nothing took 1 ms.
		MC_LOG_INFO("wii.perf", "slowest(window) entity=%s:%ldms block=%s:%ldms\n",
		            g_wii.slowWorst[0].ns >= 1000000LL ? g_wii.slowWorst[0].name : "-", (long)(g_wii.slowWorst[0].ns / 1000000LL),
		            g_wii.slowWorst[1].ns >= 1000000LL ? g_wii.slowWorst[1].name : "-", (long)(g_wii.slowWorst[1].ns / 1000000LL));
	}
	MC_LOG_INFO("wii.perf", "chunkLoad=%d:%ld/%ldms generate=%d:%ld/%ldms populate=%d:%ld/%ldms mesh=%d:%ld/%ldms unloadSave=%d:%ld/%ldms tickUpdates=%ld/%ldms queue=%ld/%ld mobSpawn=%ld/%ldms saveInfo=%ld/%ldms mapStorage=%ld/%ldms\n",
	            g_wii.chunkLoadCount, (long)(g_wii.chunkLoadCount ? g_wii.chunkLoadNs / g_wii.chunkLoadCount / 1000000LL : 0), (long)(g_wii.maxChunkLoadNs / 1000000LL),
	            g_wii.generateCount, (long)(g_wii.generateCount ? g_wii.generateNs / g_wii.generateCount / 1000000LL : 0), (long)(g_wii.maxGenerateNs / 1000000LL),
	            g_wii.populateCount, (long)(g_wii.populateCount ? g_wii.populateNs / g_wii.populateCount / 1000000LL : 0), (long)(g_wii.maxPopulateNs / 1000000LL),
	            g_wii.meshCount, (long)(g_wii.meshCount ? g_wii.meshNs / g_wii.meshCount / 1000000LL : 0), (long)(g_wii.maxMeshNs / 1000000LL),
	            g_wii.unloadSaveCount, (long)(g_wii.unloadSaveCount ? g_wii.unloadSaveNs / g_wii.unloadSaveCount / 1000000LL : 0), (long)(g_wii.maxUnloadSaveNs / 1000000LL),
	            (long)(g_wii.ticks ? g_wii.tickUpdatesNs / g_wii.ticks / 1000000LL : 0), (long)(g_wii.maxTickUpdatesNs / 1000000LL),
	            (long)(g_wii.ticks ? g_wii.tickQueueSum / g_wii.ticks : 0), (long)g_wii.tickQueueMax,
	            (long)(g_wii.ticks ? g_wii.mobSpawnNs / g_wii.ticks / 1000000LL : 0), (long)(g_wii.maxMobSpawnNs / 1000000LL),
	            (long)(g_wii.ticks ? g_wii.saveInfoNs / g_wii.ticks / 1000000LL : 0), (long)(g_wii.maxSaveInfoNs / 1000000LL),
	            (long)(g_wii.ticks ? g_wii.mapStorageNs / g_wii.ticks / 1000000LL : 0), (long)(g_wii.maxMapStorageNs / 1000000LL));
	MC_LOG_INFO("wii.perf", "populatePhase total=%ld/%ld lakes=%ld/%ld dungeons=%ld/%ld fillers=%ld/%ld ores=%ld/%ld decoration=%ld/%ld springs=%ld/%ld snow=%ld/%ldms\n",
	            (long)(g_wii.populatePhaseCount[0] ? g_wii.populatePhaseNs[0] / g_wii.populatePhaseCount[0] / 1000000LL : 0), (long)(g_wii.maxPopulatePhaseNs[0] / 1000000LL),
	            (long)(g_wii.populatePhaseCount[1] ? g_wii.populatePhaseNs[1] / g_wii.populatePhaseCount[1] / 1000000LL : 0), (long)(g_wii.maxPopulatePhaseNs[1] / 1000000LL),
	            (long)(g_wii.populatePhaseCount[2] ? g_wii.populatePhaseNs[2] / g_wii.populatePhaseCount[2] / 1000000LL : 0), (long)(g_wii.maxPopulatePhaseNs[2] / 1000000LL),
	            (long)(g_wii.populatePhaseCount[3] ? g_wii.populatePhaseNs[3] / g_wii.populatePhaseCount[3] / 1000000LL : 0), (long)(g_wii.maxPopulatePhaseNs[3] / 1000000LL),
	            (long)(g_wii.populatePhaseCount[4] ? g_wii.populatePhaseNs[4] / g_wii.populatePhaseCount[4] / 1000000LL : 0), (long)(g_wii.maxPopulatePhaseNs[4] / 1000000LL),
	            (long)(g_wii.populatePhaseCount[5] ? g_wii.populatePhaseNs[5] / g_wii.populatePhaseCount[5] / 1000000LL : 0), (long)(g_wii.maxPopulatePhaseNs[5] / 1000000LL),
	            (long)(g_wii.populatePhaseCount[6] ? g_wii.populatePhaseNs[6] / g_wii.populatePhaseCount[6] / 1000000LL : 0), (long)(g_wii.maxPopulatePhaseNs[6] / 1000000LL),
	            (long)(g_wii.populatePhaseCount[7] ? g_wii.populatePhaseNs[7] / g_wii.populatePhaseCount[7] / 1000000LL : 0), (long)(g_wii.maxPopulatePhaseNs[7] / 1000000LL));
	if (world != nullptr)
	{
		const std::string chunks = world->getChunkProviderStats();
		MC_LOG_INFO("wii.perf", "world ents=%d lightQ=%d pending=%d heap=%dMB | %s\n",
		            (int)world->getLoadedEntityList().size(),
		            (int)world->getPendingLightingUpdateCount(),
		            renderGlobal != nullptr ? (int)renderGlobal->pendingRendererUpdateCount() : 0,
		            (int)(mallinfo().uordblks >> 20), chunks.c_str());
	}
	{
		const int frames = g_wii.frames ? g_wii.frames : 1;
		MC_LOG_INFO("wii.perf", "work draws=%ld/%u texUploads=%ld vboKB=%ld per frame (avg/max); spikes>%ldms=%d\n",
		            (long)(g_wii.draws / frames), g_wii.maxDraws,
		            (long)(g_wii.texUploads / frames), (long)(g_wii.bufferBytes / frames / 1024),
		            (long)(kSpikeNs / 1000000LL), g_wii.spikesSeen);
		static const char* const kPhaseNames[kRenderPhaseSlots] = {
			"sky", "frustum", "build", "opaque", "ents", "transl", "hand", "hud",
			"entDraw", "tileDraw", "hudItems", "hudText", "hudHints", "?" };
		for (int i = 0; i < g_wii.spikeCount; i++)
		{
			const FrameSnapshot& f = g_wii.spikes[i];
			char phases[256];
			int len = 0;
			phases[0] = '\0';
			for (int p = 0; p < kRenderPhaseSlots && len < (int)sizeof(phases) - 24; p++)
				if (f.phaseUs[p] >= 1000)
					len += snprintf(phases + len, sizeof(phases) - len, " %s=%ld", kPhaseNames[p], (long)(f.phaseUs[p] / 1000));
			// Largest tick phases of that frame (nested ones next to their
			// parent) and the slowest single entity / random-tick block.
			char top[160];
			int topLen = 0;
			top[0] = '\0';
			for (int p = 0; p < kTopPhases && f.topPhases[p].ns >= 1000000LL && topLen < (int)sizeof(top) - 32; p++)
				topLen += snprintf(top + topLen, sizeof(top) - topLen, " %s=%ld",
				                   f.topPhases[p].name, (long)(f.topPhases[p].ns / 1000000LL));
			char slow[128];
			int slowLen = 0;
			slow[0] = '\0';
			if (f.slow[0].ns >= 1000000LL)
				slowLen += snprintf(slow + slowLen, sizeof(slow) - slowLen, " slowEntity=%s:%ld", f.slow[0].name, (long)(f.slow[0].ns / 1000000LL));
			if (f.slow[1].ns >= 1000000LL)
				slowLen += snprintf(slow + slowLen, sizeof(slow) - slowLen, " slowBlock=%s:%ld", f.slow[1].name, (long)(f.slow[1].ns / 1000000LL));
			MC_LOG_INFO("wii.perf", "spike frame=%ldms tick=%ldms(x%d top %s=%ldms) light=%ldms render=%ldms draws=%u tex=%u vboKB=%u chunks=%d |%s\n",
			            (long)(f.frameNs / 1000000LL), (long)(f.tickNs / 1000000LL), f.ticks,
			            f.topTickPhase[0] ? f.topTickPhase : "-", (long)(f.topTickPhaseNs / 1000000LL),
			            (long)(f.lightingNs / 1000000LL), (long)(f.renderNs / 1000000LL),
			            f.draws, f.texUploads, f.bufferBytes / 1024, f.chunkUpdates, phases);
			MC_LOG_INFO("wii.perf", "spike.detail frame=%ldms phases(ms):%s |%s\n",
			            (long)(f.frameNs / 1000000LL), top, slow);
		}
	}
	g_lastReportNs = System::nanoTime() - reportStartNs;
	resetWindow(now);
	g_wii.prevRenderNs = renderNs;
}
}
