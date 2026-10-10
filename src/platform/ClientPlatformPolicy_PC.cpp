#include "platform/ClientPlatformPolicy.h"

#include "net/minecraft/src/GameResources.h"
#include "net/minecraft/src/GameSettings.h"
#include "pc/CrashHandler.h"
#include "platform/PlatformConfig.h"

namespace ClientPlatformPolicy
{
int initialWidth()
{
#ifdef VITA_PLATFORM
    return 960;
#else
    return 854;
#endif
}

int initialHeight()
{
#ifdef VITA_PLATFORM
    return 544;
#else
    return 480;
#endif
}

std::string minecraftDirectory()
{
    return GameResources::getExeDir() + "/.minecraft";
}

bool saveConverterUsesSavesSubdirectory()
{
    return true;
}

void applyGameSettingsDefaults(GameSettings* settings)
{
#if PLATFORM_PC_LEGACY
    if (settings == nullptr)
        return;

    settings->particleSetting = 2;
    settings->ofVoidParticles = false;
    settings->ofWaterParticles = false;
    settings->ofRainSplash = false;
    settings->ofPortalParticles = false;
    settings->ofDrippingWaterLava = false;
    settings->ofWeather = false;
    settings->ofSky = false;
    settings->ofSunMoon = false;
    settings->ofClouds = 3;
#else
    (void)settings;
#endif
}

void preloadStartupTextures(RenderEngine*)
{
}

void releaseWorldEntryAssets(RenderEngine*)
{
}

int panoramaSampleGrid()
{
#ifdef VITA_PLATFORM
    // 2x2 like PS2: 24 draws instead of 384; every vitaGL draw call is costly.
    return 2;
#else
    return 8;
#endif
}


void reportCrash(const std::string& description)
{
    CrashHandler::Crash(description);
}
}
