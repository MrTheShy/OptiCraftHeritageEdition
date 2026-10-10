#pragma once

#include "LegacyOptionsScreen.h"

#include <vector>

class GuiButton;
class LegacyOptionCheckbox;

// Runtime performance trades (platform/PlatformPerfOptions.h) in the legacy UI.
// Checked rows named "Limit ..." / "Throttle ..." are the cuts; checked
// "Particles", "Rain & Snow", "Camera Effects", "Entity Shadows" are the vanilla
// visuals. Unchecking every "Limit" row and checking every visual row gives
// vanilla 1.2.5 simulation and rendering.
class LegacyPerformanceOptions : public LegacyOptionsScreen
{
public:
    LegacyPerformanceOptions(GuiScreen *parent, GameSettings *settings,
        LegacyOptionsBackgroundMode backgroundMode = LegacyOptionsBackgroundMode::Panorama);
    void initGui() override;
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;

protected:
    void actionPerformed(GuiButton *button) override;

private:
    std::vector<LegacyOptionCheckbox *> checkboxes;
};
