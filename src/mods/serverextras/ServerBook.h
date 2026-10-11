#pragma once

#include <string>
#include <vector>

// "OC|Book": a book sent a line at a time, shown when "open" arrives.
// Lines: "book <id> <pages>", "title <text>", "by <text>", "page <n> <text>",
// "open [<page>]". The client answers "done <id>" once the last page is shown.
class ServerBook
{
public:
    struct Book
    {
        std::string id;
        std::string title;
        std::string by;
        std::vector<std::string> pages;
        int startPage = 1;
    };

    // True when the line was "open" for a complete header: the book is moved
    // into opened and the assembler starts over.
    bool handleLine(const std::string &line, Book &opened);
    void clear();

private:
    Book book;
};
