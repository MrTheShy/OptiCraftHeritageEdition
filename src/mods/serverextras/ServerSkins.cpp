#include "mods/serverextras/ServerSkins.h"

#include <sstream>

#include "mods/serverextras/LineProtocol.h"

namespace
{
constexpr std::size_t kMaxEntities = 256;
constexpr std::size_t kMaxNames = 512;
constexpr long long kMinChangeMs = 2000;

bool asciiDigit(char c) { return c >= '0' && c <= '9'; }
bool asciiAlnum(char c) { return asciiDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
}

bool ServerSkins::validName(const std::string &name)
{
    if (name.empty() || name.size() > 32)
        return false;
    for (char c : name)
        if (!asciiAlnum(c) && c != '_' && c != '-')
            return false;
    return true;
}

bool ServerSkins::validBase(const std::string &base)
{
    if (base.size() < 3 || base.size() > 64 || base[0] != ':' || base.back() != '/')
        return false;
    std::size_t i = 1;
    while (i < base.size() && asciiDigit(base[i]))
        ++i;
    if (i == 1 || i - 1 > 5 || base[i] != '/')
        return false;
    for (; i < base.size(); ++i)
    {
        const char c = base[i];
        if (!asciiAlnum(c) && c != '/' && c != '_' && c != '-' && c != '.')
            return false;
    }
    return base.find("..") == std::string::npos;
}

bool ServerSkins::full() const
{
    return skins.size() >= kMaxEntities;
}

int ServerSkins::handleLine(const std::string &line, long long nowMs)
{
    std::istringstream words(line);
    std::string verb;
    words >> verb;
    if (verb == "reset")
    {
        clear();
        return -1;
    }
    if (verb == "base")
    {
        std::string newBase;
        words >> newBase;
        if (validBase(newBase))
            base = newBase;
        return -1;
    }
    std::string idText;
    std::string name;
    long id = 0;
    if (verb != "e" || !(words >> idText >> name) || !LineProtocol::parseInt(idText, 0, 0x7fffffffL, id) ||
        !validName(name))
        return -1; // unknown or malformed lines are ignored so the channel can grow
    const int entityId = static_cast<int>(id);
    auto known = skins.find(entityId);
    if (known == skins.end() && full())
        return -1;
    if (known != skins.end() && (known->second.name == name || nowMs - known->second.changedMs < kMinChangeMs))
        return -1;
    if (names.count(name) == 0)
    {
        if (names.size() >= kMaxNames)
            return -1;
        names.insert(name);
    }
    Skin &skin = skins[entityId];
    skin.name = name;
    skin.changedMs = nowMs;
    return entityId;
}

std::string ServerSkins::urlFor(const std::string &host, int entityId) const
{
    auto it = skins.find(entityId);
    if (base.empty() || host.empty() || it == skins.end())
        return std::string();
    return "http://" + host + base + it->second.name + ".png";
}

void ServerSkins::clear()
{
    base.clear();
    skins.clear();
    names.clear();
}
