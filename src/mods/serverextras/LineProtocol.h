#pragma once

#include <cerrno>
#include <cstdlib>
#include <string>

// The OC| channels carry one UTF-8 text line per message: "<verb> <rest>".
namespace LineProtocol
{
inline void splitVerb(const std::string &line, std::string &verb, std::string &rest)
{
    const std::string::size_type space = line.find(' ');
    verb = line.substr(0, space);
    rest = space == std::string::npos ? std::string() : line.substr(space + 1);
}

// The whole of text as a decimal number in [low, high].
inline bool parseInt(const std::string &text, long low, long high, long &out)
{
    if (text.empty())
        return false;
    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || value < low || value > high)
        return false;
    out = value;
    return true;
}
}
