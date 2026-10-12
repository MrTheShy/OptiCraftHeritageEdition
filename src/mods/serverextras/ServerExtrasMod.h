#pragma once

#include <string>
#include <vector>

#include "mods/IMod.h"
#include "mods/serverextras/ServerBook.h"
#include "mods/serverextras/ServerItemNames.h"
#include "mods/serverextras/ServerSkins.h"

// Client side of four optional plugin channels a server can use to give 1.2.5
// clients what the protocol lacks:
//   OC|Skin  player and NPC skins served by the server itself
//   OC|Book  books shown in a page screen (1.2.5 has no written books)
//   OC|Menu  item names and lore in tooltips
//   OC|Perf  a frame-rate target per map (only where FrameRateOverride is read)
// The parsing lives in ServerSkins, ServerBook and ServerItemNames, which know
// nothing about the engine; this class applies what they decide. On join it
// says "hello 1" on each channel; servers that do not answer see nothing else.
class ServerExtrasMod : public IMod
{
public:
    std::string getId() const override { return "serverextras"; }
    std::string getName() const override { return "Server Extras"; }
    std::string getVersion() const override { return "v1.0"; }
    std::string getDescription() const override { return "Skins, books and item names from servers"; }
    std::string getAuthor() const override { return "MrTheShy"; }
    bool isEnabled() const override { return enabled; }
    void setEnabled(bool state) override { enabled = state; }

    void onInit(Minecraft *minecraft) override { mc = minecraft; }
    std::vector<std::string> getPluginChannels() const override;
    void onPluginMessage(const std::string &channel, const std::vector<byte_t> &data) override;
    void onServerJoin(NetClientHandler *net) override;
    void onServerLeave() override;
    void onNamedEntitySpawn(EntityOtherPlayerMP *player, int_t entityId) override;
    void onWindowOpened(int_t windowId) override;
    void onItemTooltip(std::vector<std::string> &lines, int_t windowId, int_t slot, ItemStack *stack) override;

private:
    void applySkin(const std::string &line);
    void openBook(const std::string &line);
    void setFrameRate(const std::string &line);
    void reset();

    Minecraft *mc = nullptr;
    bool enabled = true;
    std::string serverHost;
    ServerSkins skins;
    ServerBook book;
    ServerItemNames itemNames;
};
