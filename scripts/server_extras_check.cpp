// Host-side check for the Server Extras parsers: what a hostile server can and
// cannot make the client do. Not part of the game build.
//   c++ -std=c++17 -Isrc scripts/server_extras_check.cpp src/mods/serverextras/ServerSkins.cpp \
//       src/mods/serverextras/ServerBook.cpp src/mods/serverextras/ServerItemNames.cpp && ./a.out
#include <cassert>
#include <cstdio>
#include <string>

#include "mods/serverextras/ServerBook.h"
#include "mods/serverextras/ServerItemNames.h"
#include "mods/serverextras/ServerSkins.h"

static void skins()
{
    assert(ServerSkins::validBase(":8080/skins/"));
    assert(ServerSkins::validBase(":80/a/b.c/"));
    const char *bad[] = {"", ":", ":/x/", ":80/x", "@evil.com/", ":80@evil.com/", ":123456/x/", ":80/../x/",
                         ":80/a b/", ":80/%2e/", ":80/a?b/", ":80/a\\b/", "//evil.com/", ":80/a#/"};
    for (const char *base : bad)
        assert(!ServerSkins::validBase(base));
    assert(ServerSkins::validName("Steve_1-x"));
    assert(!ServerSkins::validName("a/b") && !ServerSkins::validName("a.png") && !ServerSkins::validName(""));
    assert(!ServerSkins::validName(std::string(33, 'a')));

    ServerSkins s;
    assert(s.handleLine("e 5 alex", 0) == 5);
    assert(s.urlFor("mc.example.org", 5).empty()); // no base yet
    s.handleLine("base :8080/skins/", 0);
    s.handleLine("base @evil.com/", 0); // ignored, the good base stays
    assert(s.urlFor("mc.example.org", 5) == "http://mc.example.org:8080/skins/alex.png");
    assert(s.urlFor("", 5).empty());
    assert(s.handleLine("e 5 alex", 5000) == -1);  // same skin: nothing to do
    assert(s.handleLine("e 5 steve", 1000) == -1); // under 2 s since the last change
    assert(s.handleLine("e 5 steve", 2000) == 5);
    assert(s.handleLine("e -1 x", 0) == -1 && s.handleLine("e 99999999999 x", 0) == -1);
    assert(s.handleLine("e 6 ../x", 0) == -1 && s.handleLine("unknown 6 x", 0) == -1);

    // At most 256 entities; prune drops the ones no longer in the world.
    for (int id = 100; !s.full(); ++id)
        s.handleLine("e " + std::to_string(id) + " n" + std::to_string(id % 8), 0);
    assert(s.handleLine("e 9999 alex", 0) == -1);
    s.prune([](int id) { return id < 200; });
    assert(!s.full() && s.handleLine("e 9999 alex", 0) == 9999);

    // At most 512 different skin names per connection.
    s.clear();
    int accepted = 0;
    for (int i = 0; i < 600; ++i)
    {
        s.prune([](int) { return false; });
        if (s.handleLine("e 1 name" + std::to_string(i), 0) == 1)
            ++accepted;
    }
    assert(accepted == 512);
    s.handleLine("reset", 0);
    assert(s.handleLine("e 1 fresh", 0) == 1);
}

static void book()
{
    ServerBook b;
    ServerBook::Book opened;
    assert(!b.handleLine("page 1 orphan", opened) && !b.handleLine("open", opened)); // no header yet
    assert(!b.handleLine("book x 0", opened) && !b.handleLine("book x 51", opened) && !b.handleLine("open", opened));
    b.handleLine("book rules 2", opened);
    b.handleLine("title The Rules", opened);
    b.handleLine("by Admin", opened);
    b.handleLine("page 1 First", opened);
    b.handleLine("page 2 " + std::string(5000, 'x'), opened);
    b.handleLine("page 3 out of range", opened);
    b.handleLine("page 0 out of range", opened);
    assert(b.handleLine("open 2", opened));
    assert(opened.id == "rules" && opened.title == "The Rules" && opened.by == "Admin");
    assert(opened.pages.size() == 2 && opened.pages[0] == "First" && opened.pages[1].size() == 600);
    assert(opened.startPage == 2);
    assert(!b.handleLine("open", opened)); // the assembler starts over
    b.handleLine("book b 1", opened);
    assert(b.handleLine("open 9", opened) && opened.startPage == 1);
}

static void itemNames()
{
    ServerItemNames n;
    n.handleLine("item 276 -1 Excalibur");
    n.handleLine("name 3 10 Shop: Sword");
    n.handleLine("lore 3 10 Costs 5 gold");
    assert(n.find(3, 10, 276, 0)->name == "Shop: Sword"); // the slot wins over the type
    assert(n.find(3, 10, 276, 0)->lore.size() == 1);
    assert(n.find(3, 11, 276, 7)->name == "Excalibur");   // any damage
    assert(n.find(3, 11, 1, 0) == nullptr);

    n.handleLine("name 3 10 Renamed"); // a new name drops the old lore
    assert(n.find(3, 10, 0, 0)->lore.empty());
    for (int i = 0; i < 10; ++i)
        n.handleLine("lore 3 10 line");
    assert(n.find(3, 10, 0, 0)->lore.size() == 4);

    n.handleLine("foo 4 1 x");     // unknown verb: no entry
    n.handleLine("name 300 1 x");  // windows are 0..255
    n.handleLine("name 4 -1 x");
    assert(n.find(4, 1, 0, 0) == nullptr && n.find(300, 1, 0, 0) == nullptr);

    n.handleLine("name 0 36 Hotbar");
    n.clearContainerWindows();
    assert(n.find(3, 10, 0, 0) == nullptr && n.find(0, 36, 0, 0)->name == "Hotbar");

    // 64 KB of text at most, whatever the entry counts.
    ServerItemNames full;
    const std::string text(190, 'x');
    for (int slot = 0; slot < 256; ++slot)
        for (int line = 0; line < 5; ++line)
            full.handleLine((line == 0 ? "name 1 " : "lore 1 ") + std::to_string(slot) + " " + text);
    std::size_t total = 0;
    for (int slot = 0; slot < 256; ++slot)
        if (const ServerItemNames::Entry *e = full.find(1, slot, -5, 0))
        {
            total += e->name.size();
            for (const std::string &l : e->lore)
                total += l.size();
        }
    assert(total > 60000 && total <= 64 * 1024);
    full.clearWindow(1);
    full.handleLine("name 1 0 back");
    assert(full.find(1, 0, 0, 0)->name == "back");
}

int main()
{
    skins();
    book();
    itemNames();
    std::puts("server extras checks passed");
}
