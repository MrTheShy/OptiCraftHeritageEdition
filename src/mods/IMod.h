#pragma once

#include "java/Type.h"
#include <string>
#include <vector>

class Minecraft;
class GuiContainer;
class GuiIngame;
class NetClientHandler;
class EntityOtherPlayerMP;
class ItemStack;

// Base interface for all OptiCraft mods
class IMod
{
public:
    virtual ~IMod() = default;

    // Unique identifier for configuration persistence (e.g. "toomanyitems")
    virtual std::string getId() const = 0;

    // Mod display info in the Mods menu
    virtual std::string getName() const = 0;
    virtual std::string getVersion() const = 0;
    virtual std::string getDescription() const = 0;
    virtual std::string getAuthor() const = 0;

    // Toggle state
    virtual bool isEnabled() const = 0;
    virtual void setEnabled(bool enabled) = 0;

    // Package path & removability
    virtual std::string getPackPath() const { return packPath; }
    virtual void setPackPath(const std::string &path) { packPath = path; }
    virtual bool isRemovable() const { return !packPath.empty(); }

    // Mod lifecycle
    virtual void onInit(Minecraft *mc) {}
    virtual void onTick() {}

    // In-game HUD overlay hook (rendered while playing)
    virtual void onRenderGameOverlay(GuiIngame *gui, int_t screenWidth, int_t screenHeight, float_t partialTick) {}

    // GUI Container hooks (inventory, chests, crafting tables, etc.)
    virtual void onDrawContainer(GuiContainer *container, int_t mouseX, int_t mouseY) {}
    virtual bool onContainerMouseClicked(GuiContainer *container, int_t x, int_t y, int_t button) { return false; }
    virtual bool onContainerKeyTyped(char_t c, int_t key) { return false; }

    // Multiplayer hooks. All of them run on the main thread.
    //
    // Plugin channels (Packet250) a mod listens on. ModManager registers them
    // with the server (Bukkit REGISTER) on join and when the mod is switched on
    // mid-session, and routes each message to every mod that listed its channel.
    // Send with ModManager::sendPluginMessage.
    virtual std::vector<std::string> getPluginChannels() const { return {}; }
    virtual void onPluginMessage(const std::string &channel, const std::vector<byte_t> &data) {}
    // After the server's login packet, or when the mod is switched on while
    // connected. It can run again without a leave (a proxy logging in again).
    // Don't keep the handler: it is deleted when the connection ends.
    virtual void onServerJoin(NetClientHandler *net) {}
    // The connection is being torn down, or the mod was switched off while
    // connected. Don't send anything from here.
    virtual void onServerLeave() {}
    // A player-shaped entity (Packet20), before it joins the world: skinUrl
    // set here is the one the renderer downloads.
    virtual void onNamedEntitySpawn(EntityOtherPlayerMP *player, int_t entityId) {}
    // The server opened a window (Packet100).
    virtual void onWindowOpened(int_t windowId) {}
    // Item tooltip lines (first = name). windowId/slot are the server's: the
    // hotbar is window 0, slots 36-44.
    virtual void onItemTooltip(std::vector<std::string> &lines, int_t windowId, int_t slot, ItemStack *stack) {}

protected:
    std::string packPath;
};
