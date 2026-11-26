#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Identity.h"
#include "Infrastructure.h"

namespace krtu::detail
{
using QueryFields = std::vector<std::pair<std::string, std::optional<std::string>>>;

struct PlayerCall
{
    std::string method = "GET";
    QueryFields query;
    /// A JSON body (sent as application/json).
    std::optional<Json> json;
    /// A raw body with its own content type in `headers`.
    std::optional<std::string> body;
    Headers headers;
    /// Statuses that are answers, not failures (304, 202 …); 2xx always are.
    std::vector<int> accept;
    /// The player the call is made for (a queue's owner, a stored save's). Default: whoever plays when the call
    /// starts. The call goes only with that player's token; with any other it is not sent.
    std::optional<std::string> owner;
};

/// How long a queue waits after `err` before its next send: the server's Retry-After; else a failure that only a
/// change elsewhere lifts (a waiting code, a refused refresh) waits long; else it doubles from 5 seconds up to 5
/// minutes (`backoffMs` holds the doubling and is updated).
std::int64_t queueWaitMs(const Error& err, std::atomic<std::int64_t>& backoffMs);
/// The long wait after a failure only a change elsewhere lifts (a frozen app, a ban, a merge in progress).
constexpr std::int64_t queueLongWaitMs = 15LL * 60LL * 1000LL;

/// The player's calls to the server (`/v1`, Bearer + X-Krtu-Build). Same rules as krtuworks-sdk-js/src/api.ts.
class Api
{
public:
    Api(const Resolved& opts, Identity& identity, HttpTransport& http, Clock& clock);

    std::string url(const std::string& path, const QueryFields& query) const;
    /// A call without a player (the remote config).
    Answer open(const std::string& path, const PlayerCall& call);
    /// A player's call, for `call.owner` (or whoever plays now). Every attempt sends only with a token of that
    /// player: when the token in hand is another one's (the player changed while it was fetched, or before the
    /// retry after a 401), nothing is sent (`context_changed`). An expired or revoked token is refreshed once and
    /// the call made again; a merged account is reported to the identity; an answer that arrives after the
    /// player changed is dropped too, so it can never land in another player's state.
    Answer player(const std::string& path, const PlayerCall& call);

private:
    Answer attempt(const std::string& path, const PlayerCall& call, bool forceRefresh, const std::string& owner);

    const Resolved& opts_;
    Identity& identity_;
    HttpTransport& http_;
    Clock& clock_;
};
}  // namespace krtu::detail
