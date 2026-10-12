#pragma once

#include <functional>
#include <string>
#include <vector>

#include "GuiScreen.h"

// A book sent by the server on the optional "OC|Book" channel (written books
// arrive in 1.3). Pages turn with left/right, L/R or a click on either half of
// the screen; the game keeps running behind it.
class GuiServerBook : public GuiScreen
{
public:
    // onLastPage runs once, when the reader reaches the last page.
    GuiServerBook(const std::string &title, const std::string &by, const std::vector<std::string> &pages,
                  int_t startPage, std::function<void()> onLastPage);

    void initGui() override;
    void updateScreen() override;
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
    void keyTyped(char_t c, int_t key) override;
    void mouseClicked(int_t x, int_t y, int_t button) override;
    bool usesSpecializedMenuNavigation() const override { return true; }
    bool doesGuiPauseGame() override { return false; }

private:
    void turn(int_t delta);
    void close();

    std::function<void()> onLastPage;
    std::string title;
    std::string by;
    std::vector<std::string> pages;
    int_t page = 0;
    int_t startPage;
    bool lastPageSeen = false;
};
