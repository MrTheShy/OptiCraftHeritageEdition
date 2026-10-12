#include "mods/serverextras/ServerItemNames.h"

#include <utility>

#include "mods/serverextras/LineProtocol.h"

namespace
{
constexpr std::size_t kMaxText = 192;          // bytes: 64 characters of UTF-8 plus colour codes
constexpr std::size_t kMaxLore = 4;
constexpr std::size_t kMaxWindowEntries = 256; // a server can send names without end
constexpr std::size_t kMaxTypeEntries = 512;
constexpr std::size_t kMaxBytes = 64u * 1024u;

std::size_t entryBytes(const ServerItemNames::Entry &entry)
{
    std::size_t total = entry.name.size();
    for (const std::string &line : entry.lore)
        total += line.size();
    return total;
}

// "<a> <b> <text>"
bool splitKeyAndText(const std::string &rest, long highA, long lowB, std::pair<int, int> &key, std::string &text)
{
    const std::string::size_type s1 = rest.find(' ');
    if (s1 == std::string::npos)
        return false;
    const std::string::size_type s2 = rest.find(' ', s1 + 1);
    const std::string second = rest.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
    long a = 0;
    long b = 0;
    if (!LineProtocol::parseInt(rest.substr(0, s1), 0, highA, a) || !LineProtocol::parseInt(second, lowB, 32767, b))
        return false;
    key = {static_cast<int>(a), static_cast<int>(b)};
    text = s2 == std::string::npos ? std::string() : rest.substr(s2 + 1, kMaxText);
    return true;
}
}

void ServerItemNames::handleLine(const std::string &line)
{
    std::string verb;
    std::string rest;
    LineProtocol::splitVerb(line, verb, rest);
    if (verb == "reset")
    {
        clear();
        return;
    }
    if (verb == "clear")
    {
        long windowId = 0;
        if (LineProtocol::parseInt(rest, 0, 255, windowId))
            clearWindow(static_cast<int>(windowId));
        return;
    }

    const bool isType = verb == "item" || verb == "itemlore" || verb == "itemclear";
    Key key;
    std::string text;
    if (!isType && verb != "name" && verb != "lore")
        return; // unknown lines are ignored so the channel can grow
    // Windows are 0..255 (Packet100); item damage -1 means every damage value.
    if (!splitKeyAndText(rest, isType ? 32767 : 255, isType ? -1 : 0, key, text))
        return;
    auto &names = isType ? typeNames : windowNames;
    auto it = names.find(key);
    if (verb == "itemclear")
    {
        if (it != names.end())
        {
            bytes -= entryBytes(it->second);
            names.erase(it);
        }
        return;
    }
    const bool created = it == names.end();
    if (created)
    {
        if (names.size() >= (isType ? kMaxTypeEntries : kMaxWindowEntries))
            return;
        it = names.emplace(key, Entry()).first;
    }
    Entry updated = it->second;
    if (verb == "name" || verb == "item")
    {
        // A new name starts a new entry: lore from an earlier update goes.
        updated.name = text;
        updated.lore.clear();
    }
    else if (updated.lore.size() < kMaxLore)
    {
        updated.lore.push_back(text);
    }
    const std::size_t before = entryBytes(it->second);
    const std::size_t after = entryBytes(updated);
    if (bytes - before + after > kMaxBytes)
    {
        if (created)
            names.erase(it);
        return;
    }
    bytes = bytes - before + after;
    it->second = std::move(updated);
}

void ServerItemNames::clear()
{
    windowNames.clear();
    typeNames.clear();
    bytes = 0;
}

void ServerItemNames::clearWindow(int windowId)
{
    for (auto it = windowNames.begin(); it != windowNames.end();)
    {
        if (it->first.first != windowId)
        {
            ++it;
            continue;
        }
        bytes -= entryBytes(it->second);
        it = windowNames.erase(it);
    }
}

void ServerItemNames::clearContainerWindows()
{
    for (auto it = windowNames.begin(); it != windowNames.end();)
    {
        if (it->first.first == 0)
        {
            ++it;
            continue;
        }
        bytes -= entryBytes(it->second);
        it = windowNames.erase(it);
    }
}

const ServerItemNames::Entry *ServerItemNames::find(int windowId, int slot, int itemId, int damage) const
{
    auto inWindow = windowNames.find({windowId, slot});
    if (inWindow != windowNames.end())
        return &inWindow->second;
    auto it = typeNames.find({itemId, damage});
    if (it == typeNames.end())
        it = typeNames.find({itemId, -1});
    return it == typeNames.end() ? nullptr : &it->second;
}
