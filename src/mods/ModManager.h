#pragma once

#include "java/Type.h"
#include "IMod.h"
#include "OchPackReader.h"
#include <vector>
#include <string>
#include <memory>

class Minecraft;
class GuiContainer;
class GuiIngame;
class NetClientHandler;
class EntityOtherPlayerMP;
class ItemStack;

class ModManager
{
public:
    static ModManager &getInstance();

    void init(Minecraft *mc);
    void shutdown();

    void registerMod(std::unique_ptr<IMod> mod);
    const std::vector<std::unique_ptr<IMod>> &getMods() const { return mods; }
    std::vector<std::unique_ptr<IMod>> &getMods() { return mods; }

    IMod *getMod(const std::string &id);
    bool isModEnabled(const std::string &id);
    void setModEnabled(const std::string &id, bool enabled);

    // .ochpack Pack Management
    void scanAndLoadPacks();
    std::string getInstalledModVersion(const std::string &id) const;
    bool installModPack(const std::string &sourcePath, std::string &outError);
    bool deleteMod(const std::string &id);
    std::string getGameModsDir() const;

    // Persistence: saves/loads enabled states to/from mods.txt
    void load();
    void save();

    // Dispatched events to all enabled mods
    void onTick();
    void onRenderGameOverlay(GuiIngame *gui, int_t screenWidth, int_t screenHeight, float_t partialTick);
    void onDrawContainer(GuiContainer *container, int_t mouseX, int_t mouseY);
    bool onContainerMouseClicked(GuiContainer *container, int_t x, int_t y, int_t button);
    bool onContainerKeyTyped(char_t c, int_t key);
    void onServerJoin(NetClientHandler *net);
    void onServerLeave(NetClientHandler *net);
    void onPluginMessage(const std::string &channel, const std::vector<byte_t> &data);
    void onNamedEntitySpawn(EntityOtherPlayerMP *player, int_t entityId);
    void onWindowOpened(int_t windowId);
    void onItemTooltip(std::vector<std::string> &lines, int_t windowId, int_t slot, ItemStack *stack);

    // Packet250 to the current server; does nothing when not connected.
    void sendPluginMessage(const std::string &channel, const std::string &data);

private:
    ModManager();
    ~ModManager();
    ModManager(const ModManager &) = delete;
    ModManager &operator=(const ModManager &) = delete;

    std::string getConfigPath() const;
    // Bukkit REGISTER / UNREGISTER: channel names separated by NUL.
    void announceChannels(const char *verb, const std::vector<std::string> &channels);

    Minecraft *mc;
    std::vector<std::unique_ptr<IMod>> mods;
    bool initialized;
    NetClientHandler *connection = nullptr; // between onServerJoin and onServerLeave
};
