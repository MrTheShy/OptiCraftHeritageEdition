#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

// "OC|Menu": item names and lore from the server (1.2.5 items have no custom
// names). A name for a slot of a window wins over a name for the item type.
// Lines: "name|lore <window> <slot> <text>", "clear <window>",
// "item|itemlore <id> <damage|-1> <text>", "itemclear <id> <damage>", "reset".
// All the text together is held to 64 KB.
class ServerItemNames
{
public:
    struct Entry
    {
        std::string name;
        std::vector<std::string> lore;
    };

    void handleLine(const std::string &line);
    void clear();
    void clearWindow(int windowId);
    // Every window but the player's own (0): only one server window is open at a time.
    void clearContainerWindows();
    const Entry *find(int windowId, int slot, int itemId, int damage) const;

private:
    using Key = std::pair<int, int>;
    std::map<Key, Entry> windowNames; // (window, slot)
    std::map<Key, Entry> typeNames;   // (item id, damage or -1)
    std::size_t bytes = 0;            // text held in both maps
};
