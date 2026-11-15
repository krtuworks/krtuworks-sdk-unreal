#include "KrtuworksCore/Http.h"

#include <algorithm>

namespace krtu
{
namespace
{
std::string httpLower(std::string_view text)
{
    std::string out(text);
    for (auto& c : out)
    {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

int httpHexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

void Headers::set(std::string_view name, std::string value)
{
    auto key = httpLower(name);
    for (auto& entry : entries_)
    {
        if (entry.first == key)
        {
            entry.second = std::move(value);
            return;
        }
    }
    entries_.emplace_back(std::move(key), std::move(value));
}

std::optional<std::string> Headers::get(std::string_view name) const
{
    const auto key = httpLower(name);
    for (const auto& entry : entries_)
    {
        if (entry.first == key) return entry.second;
    }
    return std::nullopt;
}

void Headers::remove(std::string_view name)
{
    const auto key = httpLower(name);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&key](const auto& entry) { return entry.first == key; }), entries_.end());
}

std::string urlEncode(std::string_view text)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char c : text)
    {
        const auto b = static_cast<unsigned char>(c);
        const bool unreserved = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-' || b == '.' || b == '_' || b == '~';
        if (unreserved)
        {
            out.push_back(c);
            continue;
        }
        out.push_back('%');
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

std::string urlDecode(std::string_view text, bool plusIsSpace)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '+' && plusIsSpace)
        {
            out.push_back(' ');
            continue;
        }
        if (c == '%' && i + 2 < text.size())
        {
            const int high = httpHexValue(text[i + 1]);
            const int low = httpHexValue(text[i + 2]);
            if (high >= 0 && low >= 0)
            {
                out.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}
}  // namespace krtu
