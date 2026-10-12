#include "mods/serverextras/ServerBook.h"

#include <sstream>
#include <utility>

#include "mods/serverextras/LineProtocol.h"

namespace
{
constexpr long kMaxPages = 50;
constexpr std::size_t kMaxPageBytes = 600;
constexpr std::size_t kMaxLineBytes = 192;
}

bool ServerBook::handleLine(const std::string &line, Book &opened)
{
    std::string verb;
    std::string rest;
    LineProtocol::splitVerb(line, verb, rest);
    if (verb == "book")
    {
        std::istringstream words(rest);
        std::string id;
        std::string countText;
        long count = 0;
        if (!(words >> id >> countText) || id.size() > 32 || !LineProtocol::parseInt(countText, 1, kMaxPages, count))
            return false;
        book = Book();
        book.id = id;
        book.pages.assign(static_cast<std::size_t>(count), std::string());
        return false;
    }
    if (book.id.empty())
        return false; // pieces of a book whose header never arrived
    if (verb == "title")
    {
        book.title = rest.substr(0, kMaxLineBytes);
    }
    else if (verb == "by")
    {
        book.by = rest.substr(0, kMaxLineBytes);
    }
    else if (verb == "page")
    {
        const std::string::size_type gap = rest.find(' ');
        long n = 0;
        if (LineProtocol::parseInt(rest.substr(0, gap), 1, static_cast<long>(book.pages.size()), n))
            book.pages[static_cast<std::size_t>(n - 1)] =
                gap == std::string::npos ? std::string() : rest.substr(gap + 1, kMaxPageBytes);
    }
    else if (verb == "open")
    {
        long start = 1;
        if (!LineProtocol::parseInt(rest, 1, static_cast<long>(book.pages.size()), start))
            start = 1;
        book.startPage = static_cast<int>(start);
        opened = std::move(book);
        book = Book();
        return true;
    }
    return false;
}

void ServerBook::clear()
{
    book = Book();
}
