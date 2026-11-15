#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace krtu
{
/// HTTP headers by name in any case (kept lowercase), one value each.
class Headers
{
public:
    void set(std::string_view name, std::string value);
    std::optional<std::string> get(std::string_view name) const;
    bool has(std::string_view name) const { return get(name).has_value(); }
    void remove(std::string_view name);
    const std::vector<std::pair<std::string, std::string>>& all() const noexcept { return entries_; }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

/// One HTTP request as the SDK makes it.
struct HttpCall
{
    std::string method = "GET";
    std::string url;
    Headers headers;
    /// The raw body; empty when there is none.
    std::string body;
};

/// One HTTP answer: status, headers and the raw body.
struct HttpAnswer
{
    int status = 0;
    Headers headers;
    std::string body;

    std::optional<std::string> header(std::string_view name) const { return headers.get(name); }
};

/// How the SDK talks HTTP. send() blocks until the answer is complete and is called from the SDK's own
/// threads, never from the game thread; the Unreal layer implements it with the engine's HTTP module.
/// A failure to get any answer throws krtu::Error("network_error").
class HttpTransport
{
public:
    virtual ~HttpTransport() = default;
    virtual HttpAnswer send(const HttpCall& call) = 0;
};

/// `text` percent-encoded the way URIs want it (RFC 3986 unreserved characters kept).
std::string urlEncode(std::string_view text);
/// `text` with %XX decoded and, when `plusIsSpace`, `+` read as a space (form and query encoding).
std::string urlDecode(std::string_view text, bool plusIsSpace);
}  // namespace krtu
