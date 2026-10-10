#pragma once

// System trophies mirroring the achievements. Only the Vita backend does
// anything: it needs the NoTrpDrm plugin, and without it init() fails quietly
// and every unlock is ignored.
namespace PlatformTrophies
{
#ifdef VITA_PLATFORM
// Once the GL context exists: installs the trophy set on first run.
void init();
// AchievementList index. Returns at once; the unlock runs on a worker.
void unlockAchievement(int index);
#else
inline void init() {}
inline void unlockAchievement(int) {}
#endif
}
