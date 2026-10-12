#include "mods/serverextras/ServerExtrasMod.h"

#include "mods/ModManager.h"
#include "mods/serverextras/GuiServerBook.h"
#include "mods/serverextras/LineProtocol.h"
#include "net/minecraft/src/EntityOtherPlayerMP.h"
#include "net/minecraft/src/FontRenderer.h"
#include "net/minecraft/src/ItemStack.h"
#include "net/minecraft/src/NetClientHandler.h"
#include "net/minecraft/src/World.h"
#include "client/Minecraft.h"
#include "java/System.h"
#include "platform/FrameRateOverride.h"

namespace
{
const char kSkin[] = "OC|Skin";
const char kBook[] = "OC|Book";
const char kMenu[] = "OC|Menu";
const char kPerf[] = "OC|Perf";

constexpr int_t kMaxTooltipWidth = 256; // pixels: server text never runs off screen
}

std::vector<std::string> ServerExtrasMod::getPluginChannels() const
{
    std::vector<std::string> channels = {kSkin, kBook, kMenu};
    if (FrameRateOverride::supported)
        channels.push_back(kPerf);
    return channels;
}

void ServerExtrasMod::reset()
{
    serverHost.clear();
    skins.clear();
    book.clear();
    itemNames.clear();
    FrameRateOverride::set(0);
}

void ServerExtrasMod::onServerJoin(NetClientHandler *net)
{
    reset();
    serverHost = net->getServerHostname();
    for (const std::string &channel : getPluginChannels())
        ModManager::getInstance().sendPluginMessage(channel, "hello 1");
}

void ServerExtrasMod::onServerLeave()
{
    reset(); // single player and the next server start clean
}

void ServerExtrasMod::onPluginMessage(const std::string &channel, const std::vector<byte_t> &data)
{
    const std::string line(data.begin(), data.end());
    if (channel == kSkin)
        applySkin(line);
    else if (channel == kBook)
        openBook(line);
    else if (channel == kMenu)
        itemNames.handleLine(line);
    else if (channel == kPerf)
        setFrameRate(line);
}

void ServerExtrasMod::setFrameRate(const std::string &line)
{
    // "fps <n>", 0 = the client's default; "reset". FrameRateOverride clamps.
    std::string verb;
    std::string rest;
    LineProtocol::splitVerb(line, verb, rest);
    long fps = 0;
    if (verb == "reset")
        FrameRateOverride::set(0);
    else if (verb == "fps" && LineProtocol::parseInt(rest, 0, 1000, fps))
        FrameRateOverride::set(static_cast<int>(fps));
}

void ServerExtrasMod::applySkin(const std::string &line)
{
    World *world = mc != nullptr ? mc->theWorld : nullptr;
    if (world != nullptr && skins.full())
        skins.prune([world](int id) { return world->getEntityByID(id) != nullptr; });
    const int entityId = skins.handleLine(line, System::nanoTime() / 1000000);
    if (entityId < 0 || world == nullptr)
        return;
    EntityOtherPlayerMP *player = dynamic_cast<EntityOtherPlayerMP *>(world->getEntityByID(entityId));
    const std::string url = skins.urlFor(serverHost, entityId);
    if (player == nullptr || url.empty())
        return;
    // In the world its skin is held by the renderer and must be swapped;
    // otherwise the renderer takes the new URL when it joins.
    if (world->isLoadedEntityPointer(player))
        world->changeEntitySkin(player, url);
    else
        player->skinUrl = url;
}

void ServerExtrasMod::onNamedEntitySpawn(EntityOtherPlayerMP *player, int_t entityId)
{
    const std::string url = skins.urlFor(serverHost, entityId);
    if (!url.empty())
        player->skinUrl = url;
}

void ServerExtrasMod::openBook(const std::string &line)
{
    ServerBook::Book opened;
    if (!book.handleLine(line, opened) || mc == nullptr)
        return;
    // Never over another screen: the pause menu, a container, the loading screen.
    if (mc->currentScreen != nullptr && dynamic_cast<GuiServerBook *>(mc->currentScreen) == nullptr)
        return;
    const std::string doneLine = "done " + opened.id;
    mc->displayGuiScreen(new GuiServerBook(opened.title, opened.by, opened.pages, opened.startPage,
        [doneLine]() { ModManager::getInstance().sendPluginMessage(kBook, doneLine); }));
}

void ServerExtrasMod::onWindowOpened(int_t windowId)
{
    (void)windowId;
    itemNames.clearContainerWindows(); // names left by the previous window
}

void ServerExtrasMod::onItemTooltip(std::vector<std::string> &lines, int_t windowId, int_t slot, ItemStack *stack)
{
    if (stack == nullptr)
        return;
    const ServerItemNames::Entry *entry = itemNames.find(windowId, slot, stack->itemID, stack->getItemDamage());
    if (entry == nullptr)
        return;
    FontRenderer *font = mc != nullptr ? mc->fontRenderer : nullptr;
    auto fit = [font](const std::string &text) { return font != nullptr ? font->trimStringToWidth(text, kMaxTooltipWidth) : text; };
    if (!entry->name.empty())
    {
        if (lines.empty())
            lines.push_back(fit(entry->name));
        else
            lines[0] = fit(entry->name);
    }
    for (const std::string &lore : entry->lore)
        lines.push_back(fit("\xC2\xA7" "7" + lore));
}
