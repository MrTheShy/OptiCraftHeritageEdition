#include "mods/serverextras/GuiServerBook.h"

#include "FontRenderer.h"
#include "Minecraft.h"
#include "SoundManager.h"
#include "platform/Input.h"
#include "pc/lwjgl/Keyboard.h"

namespace
{
constexpr int_t kPageWidth = 196;
constexpr int_t kPageHeight = 220;
constexpr int_t kTextMargin = 14;
constexpr int_t kInk = 0x2a1d0e;
constexpr int_t kFaded = 0x7a6248;
constexpr int_t kTextWidth = kPageWidth - 2 * kTextMargin;
constexpr int_t kTextBottom = kPageHeight - 14; // above the page counter

// Server text can be longer than a page: cut it where the page ends.
std::string fitPage(FontRenderer *font, std::string text, int_t height)
{
    while (!text.empty() && font->splitStringWidth(text, kTextWidth) > height)
    {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
            text.pop_back(); // never leave half a UTF-8 character
    }
    return text;
}
}

GuiServerBook::GuiServerBook(const std::string &bookTitle, const std::string &bookBy,
                             const std::vector<std::string> &bookPages, int_t firstPage,
                             std::function<void()> lastPageCallback)
    : onLastPage(std::move(lastPageCallback)), title(bookTitle), by(bookBy), pages(bookPages), startPage(firstPage)
{
    if (pages.empty())
        pages.emplace_back();
}

void GuiServerBook::initGui()
{
    // Turn to the start page once the screen is shown, so a one-page book
    // reports "read" only when it is actually on screen.
    if (startPage > 0)
    {
        turn(startPage - 1);
        startPage = 0;
    }
    if (fontRenderer == nullptr)
        return;
    const int_t header = (title.empty() ? 0 : 12) + (by.empty() ? 0 : 12) + 4;
    for (std::size_t i = 0; i < pages.size(); ++i)
        pages[i] = fitPage(fontRenderer, pages[i], kTextBottom - kTextMargin - (i == 0 ? header : 0));
}

void GuiServerBook::turn(int_t delta)
{
    const int_t last = static_cast<int_t>(pages.size()) - 1;
    int_t next = page + delta;
    next = next < 0 ? 0 : (next > last ? last : next);
    if (next != page && mc != nullptr && mc->sndManager != nullptr)
        mc->sndManager->playSoundFX("random.click", 1.0f, 1.0f);
    page = next;
    if (page == last && !lastPageSeen)
    {
        lastPageSeen = true;
        if (onLastPage)
            onLastPage();
    }
}

void GuiServerBook::close()
{
    if (mc != nullptr)
        mc->displayGuiScreen(nullptr);
}

void GuiServerBook::updateScreen()
{
    // Pads without a keyboard mapping (PS2, Wii) report through the text
    // input snapshot; on PC the pad arrives as keys (keyTyped).
    const PlatformTextInputSnapshot pad = platformTextInputSnapshot(platformMenuPad());
    if (!pad.connected)
        return;
    if (pad.pressed & (PLATFORM_TEXT_LEFT | PLATFORM_TEXT_PREV_PAGE))
        turn(-1);
    if (pad.pressed & (PLATFORM_TEXT_RIGHT | PLATFORM_TEXT_NEXT_PAGE))
        turn(1);
    if (pad.pressed & PLATFORM_TEXT_CLOSE)
        close();
}

void GuiServerBook::keyTyped(char_t c, int_t key)
{
    if (key == lwjgl::Keyboard::KEY_LEFT || key == lwjgl::Keyboard::KEY_PRIOR)
        turn(-1);
    else if (key == lwjgl::Keyboard::KEY_RIGHT || key == lwjgl::Keyboard::KEY_NEXT)
        turn(1);
    else
        GuiScreen::keyTyped(c, key); // Esc closes
}

void GuiServerBook::mouseClicked(int_t x, int_t y, int_t button)
{
    (void)y;
    if (button == 0)
        turn(x < width / 2 ? -1 : 1);
}

void GuiServerBook::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    const int_t x0 = (width - kPageWidth) / 2;
    const int_t y0 = (height - kPageHeight) / 2;
    const int_t textWidth = kTextWidth;
    // Cover edge, then the paper.
    drawRect(x0 - 3, y0 - 3, x0 + kPageWidth + 3, y0 + kPageHeight + 3, 0xFF5a3a1e);
    drawRect(x0, y0, x0 + kPageWidth, y0 + kPageHeight, 0xFFf3e7c6);

    int_t textY = y0 + kTextMargin;
    if (page == 0)
    {
        const std::string heading = fontRenderer->trimStringToWidth(title, textWidth);
        const std::string byline = fontRenderer->trimStringToWidth(by, textWidth);
        if (!heading.empty())
        {
            fontRenderer->drawString(heading, x0 + (kPageWidth - fontRenderer->getStringWidth(heading)) / 2, textY, kInk);
            textY += 12;
        }
        if (!byline.empty())
        {
            fontRenderer->drawString(byline, x0 + (kPageWidth - fontRenderer->getStringWidth(byline)) / 2, textY, kFaded);
            textY += 12;
        }
        textY += 4;
    }
    fontRenderer->drawSplitString(pages[static_cast<std::size_t>(page)], x0 + kTextMargin, textY, textWidth, kInk);

    const std::string counter = std::to_string(page + 1) + " / " + std::to_string(pages.size());
    fontRenderer->drawString(counter, x0 + (kPageWidth - fontRenderer->getStringWidth(counter)) / 2,
                             y0 + kPageHeight - 12, kFaded);
    if (page > 0)
        fontRenderer->drawString("<", x0 + 6, y0 + kPageHeight - 12, kInk);
    if (page + 1 < static_cast<int_t>(pages.size()))
        fontRenderer->drawString(">", x0 + kPageWidth - 12, y0 + kPageHeight - 12, kInk);

    GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}
