#include "net/minecraft/src/UiStrings.h"
#include "LegacyVideoOptions.h"

#include <algorithm>

#include "LegacyGuiButton.h"
#include "LegacyOptionCheckbox.h"
#include "LegacyOptionSlider.h"
#include "LegacyOptionState.h"
#include "LegacyOptionMetrics.h"
#include "net/minecraft/src/EnumOptions.h"
#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/GuiButton.h"
#include "net/minecraft/src/Minecraft.h"
#include "platform/PlatformConfig.h"
#include "platform/PlatformUserSettings.h"
#include "platform/PlatformTuning.h"

namespace
{
enum LegacyVideoButtonId
{
    BUTTON_RENDER_DISTANCE = 300,
    BUTTON_GRAPHICS = 301,
    BUTTON_SMOOTH_LIGHTING = 302,
    BUTTON_VIEW_BOBBING = 303,
    BUTTON_CLOUDS = 304,
    BUTTON_FOG = 305,
    BUTTON_BRIGHTNESS = 306,
    BUTTON_DEFLICKER = 307,
    BUTTON_FAST_TERRAIN = 308,
    BUTTON_FRAME_CAP = 309,
    BUTTON_LIGHT_UPDATES = 310,
    BUTTON_VERTICAL_DISTANCE = 311,
    BUTTON_DONE = 399
};

}

LegacyVideoOptions::LegacyVideoOptions(GuiScreen *parent, GameSettings *settingsValue,
    LegacyOptionsBackgroundMode backgroundModeValue)
    : LegacyOptionsScreen(parent, settingsValue, backgroundModeValue), graphicsCheckbox(nullptr), smoothLightingCheckbox(nullptr),
      viewBobbingCheckbox(nullptr), cloudsCheckbox(nullptr), fogCheckbox(nullptr), deflickerCheckbox(nullptr)
{
}

void LegacyVideoOptions::initGui()
{
#if PLATFORM_WII
    int_t rowCount = 7;
#elif PLATFORM_PS2
    int_t rowCount = 6;
#elif PLATFORM_TERRAIN_VBO_BATCH
    int_t rowCount = 9;
#else
    int_t rowCount = 8;
#endif
    // Frame cap (display vsync) and Light Updates: cycling buttons below the
    // sliders, added only while the panel still has room for them.
    int_t extraRows = 0;
#ifdef VITA_PLATFORM
    rowCount += 1; // Vertical Distance slider
    extraRows = 2;
#elif PLATFORM_LIGHTING_EXTRA_BUDGET_US > 0
    extraRows = 1;
#endif
    if (extraRows > 0)
    {
        const int_t fit = legacyOptionsMaxRows(width, height, LegacyOptionsLayoutPreset::Compact) - rowCount;
        extraRows = std::max<int_t>(0, std::min<int_t>(extraRows, fit));
        rowCount += extraRows;
    }
    configureLegacyLayout(rowCount, true, LegacyOptionsLayoutPreset::Compact);
    const int_t x = legacyLayout.contentX;
    const int_t w = legacyLayout.contentWidth;
    const int_t h = legacyLayout.rowHeight;
    int_t row = 0;

    graphicsCheckbox = new LegacyOptionCheckbox(BUTTON_GRAPHICS, x, legacyLayout.rowY(row++), w, h,
        uiText("Fancy Graphics"), settings->fancyGraphics);
    smoothLightingCheckbox = new LegacyOptionCheckbox(BUTTON_SMOOTH_LIGHTING, x, legacyLayout.rowY(row++), w, h,
        uiText("Smooth Lighting"), legacySmoothLightingChecked(settings->ofAoLevel));
    viewBobbingCheckbox = new LegacyOptionCheckbox(BUTTON_VIEW_BOBBING, x, legacyLayout.rowY(row++), w, h,
        uiText("View Bobbing"), settings->viewBobbing);

    controlList.push_back(graphicsCheckbox);
    controlList.push_back(smoothLightingCheckbox);
    controlList.push_back(viewBobbingCheckbox);

#if !(PLATFORM_PS2 || PLATFORM_WII)
    cloudsCheckbox = new LegacyOptionCheckbox(BUTTON_CLOUDS, x, legacyLayout.rowY(row++), w, h,
        uiText("Render Clouds"), legacyCloudsChecked(settings->ofClouds));
    controlList.push_back(cloudsCheckbox);
#else
    cloudsCheckbox = nullptr;
#endif

#if !(PLATFORM_PS2 || PLATFORM_WII)
    fogCheckbox = new LegacyOptionCheckbox(BUTTON_FOG, x, legacyLayout.rowY(row++), w, h,
        uiText("Fog"), legacyFogChecked(settings->ofFogOff));
    controlList.push_back(fogCheckbox);
#else
    fogCheckbox = nullptr;
#endif

#if PLATFORM_WII
    // The vertical copy filter is what reads as "anti-aliasing" on the Wii:
    // it steadies a 480i picture at the cost of a vertical blur.
    deflickerCheckbox = new LegacyOptionCheckbox(BUTTON_DEFLICKER, x, legacyLayout.rowY(row++), w, h,
        uiText("Deflicker Filter"), settings->wiiDeflicker);
    controlList.push_back(deflickerCheckbox);
#else
    deflickerCheckbox = nullptr;
#endif

#if PLATFORM_TERRAIN_VBO_BATCH
    fastTerrainCheckbox = new LegacyOptionCheckbox(BUTTON_FAST_TERRAIN, x, legacyLayout.rowY(row++), w, h,
        uiText("Fast Terrain"), settings->ofFastTerrain);
    controlList.push_back(fastTerrainCheckbox);
#else
    fastTerrainCheckbox = nullptr;
#endif

    controlList.push_back(new LegacyOptionSlider(BUTTON_RENDER_DISTANCE, x, legacyLayout.rowY(row++), w, h,
        settings, EnumOptions::RENDER_DISTANCE_FINE));
#ifdef VITA_PLATFORM
    controlList.push_back(new LegacyOptionSlider(BUTTON_VERTICAL_DISTANCE, x, legacyLayout.rowY(row++), w, h,
        settings, EnumOptions::VERTICAL_DISTANCE));
#endif
    controlList.push_back(new LegacyOptionSlider(BUTTON_BRIGHTNESS, x, legacyLayout.rowY(row++), w, h,
        settings, EnumOptions::BRIGHTNESS));
#ifdef VITA_PLATFORM
    if (extraRows >= 1)
        controlList.push_back(new LegacyGuiButton(BUTTON_FRAME_CAP, x, legacyLayout.rowY(row++), w, h,
            settings->getKeyBinding(EnumOptions::FRAMERATE_LIMIT)));
#endif
#if PLATFORM_LIGHTING_EXTRA_BUDGET_US > 0
#ifdef VITA_PLATFORM
    if (extraRows >= 2)
#else
    if (extraRows >= 1)
#endif
        controlList.push_back(new LegacyGuiButton(BUTTON_LIGHT_UPDATES, x, legacyLayout.rowY(row++), w, h,
            settings->getKeyBinding(EnumOptions::LIGHTING_UPDATES)));
#endif
    controlList.push_back(new LegacyGuiButton(BUTTON_DONE, x, legacyLayout.rowY(row), w, h, uiText("Done")));
}

void LegacyVideoOptions::syncCheckboxes()
{
    graphicsCheckbox->setChecked(settings->fancyGraphics);
    smoothLightingCheckbox->setChecked(legacySmoothLightingChecked(settings->ofAoLevel));
    viewBobbingCheckbox->setChecked(settings->viewBobbing);
    if (cloudsCheckbox != nullptr)
        cloudsCheckbox->setChecked(legacyCloudsChecked(settings->ofClouds));
    if (fogCheckbox != nullptr)
        fogCheckbox->setChecked(legacyFogChecked(settings->ofFogOff));
    if (deflickerCheckbox != nullptr)
        deflickerCheckbox->setChecked(settings->wiiDeflicker);
    if (fastTerrainCheckbox != nullptr)
        fastTerrainCheckbox->setChecked(settings->ofFastTerrain);
}

void LegacyVideoOptions::actionPerformed(GuiButton *button)
{
    if (button == nullptr || !button->enabled)
        return;

    switch (button->id)
    {
    case BUTTON_GRAPHICS:
        settings->setOptionValue(EnumOptions::GRAPHICS, 1);
        syncCheckboxes();
        return;
    case BUTTON_SMOOTH_LIGHTING:
        settings->setOptionFloatValue(EnumOptions::AO_LEVEL,
            legacySmoothLightingToggleValue(settings->ofAoLevel));
        syncCheckboxes();
        return;
    case BUTTON_VIEW_BOBBING:
        settings->setOptionValue(EnumOptions::VIEW_BOBBING, 1);
        syncCheckboxes();
        return;
    case BUTTON_CLOUDS:
        settings->ofClouds = legacyCloudsToggledValue(settings->ofClouds);
        settings->saveOptions();
        syncCheckboxes();
        return;
    case BUTTON_FOG:
        settings->ofFogOff = legacyFogToggledOff(settings->ofFogOff);
        settings->saveOptions();
        syncCheckboxes();
        return;
    case BUTTON_DEFLICKER:
        settings->wiiDeflicker = !settings->wiiDeflicker;
        PlatformUserSettings::setDisplayDeflicker(settings->wiiDeflicker);
        settings->saveOptions();
        syncCheckboxes();
        return;
    case BUTTON_FAST_TERRAIN:
        settings->setOptionValue(EnumOptions::FAST_TERRAIN, 1);
        settings->saveOptions();
        syncCheckboxes();
        return;
    case BUTTON_FRAME_CAP:
        settings->setOptionValue(EnumOptions::FRAMERATE_LIMIT, 1);
        button->displayString = settings->getKeyBinding(EnumOptions::FRAMERATE_LIMIT);
        settings->saveOptions();
        return;
    case BUTTON_LIGHT_UPDATES:
        settings->setOptionValue(EnumOptions::LIGHTING_UPDATES, 1);
        button->displayString = settings->getKeyBinding(EnumOptions::LIGHTING_UPDATES);
        settings->saveOptions();
        return;
    case BUTTON_DONE:
        returnToParent();
        return;
    default:
        return;
    }
}

void LegacyVideoOptions::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    drawLegacyBackground(partialTick);
    updateLegacyPointerHover(mouseX, mouseY);
    GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}
