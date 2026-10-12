#pragma once

#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>

// "OC|Skin": skins the server hosts itself, on the host the client is
// connected to. Lines: "base :<port>/<path>/", "e <entityId> <skin>", "reset".
// Every skin is an HTTP download, so changes are rate limited: one per entity
// every 2 s and at most 512 different skins per connection.
class ServerSkins
{
public:
    // Returns the entity whose skin changed, or -1.
    int handleLine(const std::string &line, long long nowMs);
    // Empty when the server sent no skin for this entity.
    std::string urlFor(const std::string &host, int entityId) const;
    void clear();

    // The entity table is full; prune() drops the entities no longer in the world.
    bool full() const;
    template <class IsInWorld> void prune(IsInWorld isInWorld)
    {
        for (auto it = skins.begin(); it != skins.end();)
            it = isInWorld(it->first) ? std::next(it) : skins.erase(it);
    }

    // Only ":<port>/<path>/": the server can pick a port and a path, never a host.
    static bool validBase(const std::string &base);
    // Names become part of a URL: [A-Za-z0-9_-], up to 32 characters.
    static bool validName(const std::string &name);

private:
    struct Skin
    {
        std::string name;
        long long changedMs = 0;
    };
    std::string base;
    std::unordered_map<int, Skin> skins;
    std::unordered_set<std::string> names; // every skin asked for since the last clear
};
