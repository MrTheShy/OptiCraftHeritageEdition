#pragma once

// A frame-rate target set at runtime (a server can ask for one per map, through
// a mod). 0 means the build's default. Only platforms that pace their own
// frames read it; `supported` says whether this one does.
namespace FrameRateOverride
{
#if defined(PS2_PLATFORM) || defined(VITA_PLATFORM)
constexpr bool supported = true;
#else
constexpr bool supported = false;
#endif

inline int &storage()
{
    static int fps = 0;
    return fps;
}

// 0 restores the default; other values are clamped to 15..60.
inline void set(int fps)
{
    storage() = fps <= 0 ? 0 : (fps < 15 ? 15 : (fps > 60 ? 60 : fps));
}

inline int get()
{
    return storage();
}
}
