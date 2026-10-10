#ifdef VITA_PLATFORM
#include "platform/Trophies.h"

#include "platform/Log.h"

#include <psp2/common_dialog.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

// SceNpTrophy has stubs in VitaSDK but no header.
namespace
{
struct SceNpTrophySetupDialogParam
{
	int sdkVersion;
	SceCommonDialogParam commonParam;
	int context;
	int options;
	std::uint8_t reserved[128];
};

struct SceNpTrophyUnlockState
{
	std::uint32_t bits[4];
};
}

extern "C" {
int sceNpTrophyInit(void *unused);
int sceNpTrophyCreateContext(int *context, char *commId, char *commSign, std::uint64_t options);
int sceNpTrophySetupDialogInit(SceNpTrophySetupDialogParam *param);
SceCommonDialogStatus sceNpTrophySetupDialogGetStatus();
int sceNpTrophySetupDialogTerm();
int sceNpTrophyCreateHandle(int *handle);
int sceNpTrophyDestroyHandle(int handle);
int sceNpTrophyUnlockTrophy(int context, int handle, int id, int *platinumId);
int sceNpTrophyGetTrophyUnlockState(int context, int handle, SceNpTrophyUnlockState *state, std::uint32_t *count);
}

namespace
{
// Our own set, sce_sys/trophy/OPTC00001_00/TROPHY.TRP (vita/trophies).
// NoTrpDrm makes the signature check pass, so any blob does.
char g_commId[12] = "OPTC00001";
char g_signature[160] = {static_cast<char>(0xb9), static_cast<char>(0xdd), static_cast<char>(0xe1), 0x3b, 0x01, 0x00};

int g_context = -1;
bool g_available = false;
SceNpTrophyUnlockState g_unlocked = {};
std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<int> g_pending;

bool alreadyUnlocked(int trophyId)
{
	return (g_unlocked.bits[trophyId >> 5] & (1u << (trophyId & 31))) != 0;
}

// Unlocking blocks for a moment and shows the system pop-up, so it stays off
// the game thread.
void unlockWorker()
{
	for (;;)
	{
		int trophyId;
		{
			std::unique_lock<std::mutex> lock(g_mutex);
			g_wake.wait(lock, [] { return !g_pending.empty(); });
			trophyId = g_pending.front();
			g_pending.pop_front();
		}
		int handle = -1;
		if (sceNpTrophyCreateHandle(&handle) < 0)
			continue;
		int platinumId = -1;
		const int result = sceNpTrophyUnlockTrophy(g_context, handle, trophyId, &platinumId);
		sceNpTrophyDestroyHandle(handle);
		if (result < 0)
			MC_LOG_WARN("trophy", "unlock %d failed: 0x%08x\n", trophyId, (unsigned)result);
	}
}
}

namespace PlatformTrophies
{
void init()
{
	sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY);
	sceNpTrophyInit(nullptr);
	int result = sceNpTrophyCreateContext(&g_context, g_commId, g_signature, 0);
	if (result < 0)
	{
		// Expected without NoTrpDrm: play on with trophies off.
		MC_LOG_INFO("trophy", "disabled (create context 0x%08x)\n", (unsigned)result);
		return;
	}

	SceNpTrophySetupDialogParam setup;
	std::memset(&setup, 0, sizeof(setup));
	_sceCommonDialogSetMagicNumber(&setup.commonParam);
	setup.sdkVersion = PSP2_SDK_VERSION;
	setup.context = g_context;
	result = sceNpTrophySetupDialogInit(&setup);
	if (result < 0)
	{
		MC_LOG_WARN("trophy", "setup dialog failed: 0x%08x\n", (unsigned)result);
		return;
	}
	// The dialog only advances while frames are presented.
	while (sceNpTrophySetupDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING)
		vglSwapBuffers(GL_TRUE);
	sceNpTrophySetupDialogTerm();

	int handle = -1;
	std::uint32_t count = 0;
	if (sceNpTrophyCreateHandle(&handle) >= 0)
	{
		sceNpTrophyGetTrophyUnlockState(g_context, handle, &g_unlocked, &count);
		sceNpTrophyDestroyHandle(handle);
	}

	std::thread(unlockWorker).detach();
	g_available = true;
	MC_LOG_INFO("trophy", "ready\n");
}

void unlockAchievement(int index)
{
	// Trophy 0 is the platinum; achievement N is trophy N + 1.
	const int trophyId = index + 1;
	if (!g_available || index < 0 || trophyId >= 128)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (alreadyUnlocked(trophyId))
		return;
	g_unlocked.bits[trophyId >> 5] |= 1u << (trophyId & 31);
	g_pending.push_back(trophyId);
	g_wake.notify_one();
}
}
#endif
