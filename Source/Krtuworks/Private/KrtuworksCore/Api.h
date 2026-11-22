#pragma once

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
