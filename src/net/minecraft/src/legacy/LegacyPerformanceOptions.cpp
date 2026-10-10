#include "net/minecraft/src/UiStrings.h"
#include "LegacyPerformanceOptions.h"

#include "LegacyGuiButton.h"
#include "LegacyOptionCheckbox.h"
#include "LegacyOptionMetrics.h"
#include "net/minecraft/src/EnumOptions.h"
#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/GuiButton.h"
#include "net/minecraft/src/Minecraft.h"
#include "platform/PlatformPerfOptions.h"

namespace
{
constexpr int_t BUTTON_FIRST_OPTION = 700;
constexpr int_t BUTTON_DONE = 799;

// Same order as platformPerfOptionRows().
const char *const kLabels[PLATFORM_PERF_OPTION_COUNT] = {
    "Throttle Distant AI", "Limit Pathfinding", "Limit Mob Spawns", "Limit Mob Pushing",
    "Hide Distant Mobs", "World Particles", "Rain & Snow", "Camera Effects",
    "Entity Shadows", "Half-Rate Animation", "Random Tick Budget"
};

bool isChecked(int_t index)
{
    const PlatformPerfOptionRow &row = platformPerfOptionRows()[index];
    return platformPerf().*row.field != row.shownInverted;
}
}

LegacyPerformanceOptions::LegacyPerformanceOptions(GuiScreen *parent, GameSettings *settingsValue,
    LegacyOptionsBackgroundMode backgroundModeValue)
    : LegacyOptionsScreen(parent, settingsValue, backgroundModeValue)
{
}

void LegacyPerformanceOptions::initGui()
{
    checkboxes.clear();
    configureLegacyLayout(PLATFORM_PERF_OPTION_COUNT + 1, true, LegacyOptionsLayoutPreset::Compact);
    const int_t x = legacyLayout.contentX;
    const int_t w = legacyLayout.contentWidth;
    const int_t h = legacyLayout.rowHeight;
    int_t row = 0;
    for (int_t i = 0; i < PLATFORM_PERF_OPTION_COUNT; ++i)
    {
        LegacyOptionCheckbox *box = new LegacyOptionCheckbox(BUTTON_FIRST_OPTION + i, x,
            legacyLayout.rowY(row++), w, h, uiText(kLabels[i]), isChecked(i));
        checkboxes.push_back(box);
        controlList.push_back(box);
    }
    controlList.push_back(new LegacyGuiButton(BUTTON_DONE, x, legacyLayout.rowY(row), w, h, uiText("Done")));
}

void LegacyPerformanceOptions::actionPerformed(GuiButton *button)
{
    if (button == nullptr || !button->enabled)
        return;

    const int_t index = button->id - BUTTON_FIRST_OPTION;
    if (index >= 0 && index < PLATFORM_PERF_OPTION_COUNT)
    {
        settings->setOptionValue(EnumOptions::PERF_OPTIONS[index], 1);
        checkboxes[index]->setChecked(isChecked(index));
        return;
    }
    if (button->id == BUTTON_DONE)
        returnToParent();
}

void LegacyPerformanceOptions::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    drawLegacyBackground(partialTick);
    updateLegacyPointerHover(mouseX, mouseY);
    GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}
