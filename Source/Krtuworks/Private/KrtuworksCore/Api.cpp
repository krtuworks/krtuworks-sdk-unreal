#include "Api.h"

#include <algorithm>

namespace krtu::detail
{
namespace
{
bool apiAccepted(const PlayerCall& call, int status)
{
    return std::find(call.accept.begin(), call.accept.end(), status) != call.accept.end();
}
}  // namespace

Api::Api(const Resolved& opts, Identity& identity, HttpTransport& http, Clock& clock) : opts_(opts), identity_(identity), http_(http), clock_(clock)
{
}

std::string Api::url(const std::string& path, const QueryFields& query) const
{
    auto out = opts_.devOrigin + path;
    bool first = true;
    for (const auto& field : query)
    {
        if (!field.second) continue;
        out += (first ? "?" : "&") + urlEncode(field.first) + "=" + urlEncode(*field.second);
        first = false;
    }
    return out;
}

Answer Api::open(const std::string& path, const PlayerCall& call)
{
    HttpCall request;
    request.method = call.method;
    request.url = url(path, call.query);
    for (const auto& header : call.headers.all()) request.headers.set(header.first, header.second);
    auto answer = wire::send(http_, clock_, request);
    if (answer.status >= 400 && !apiAccepted(call, answer.status)) throw wire::errorOf(answer);
    return answer;
}

Answer Api::player(const std::string& path, const PlayerCall& call)
{
    const auto generation = identity_.generation();
    const auto current = identity_.player();
    auto answer = attempt(path, call, false);
    if (answer.status == 401)
    {
        const auto err = wire::errorOf(answer);
        if (err.code() == "account_merged" && current) identity_.markMerged(current->subject, err.details().getString("mergedInto"));
        if (err.code() == "token_expired" || err.code() == "token_revoked" || err.code() == "invalid_token") answer = attempt(path, call, true);
    }
    if (identity_.generation() != generation) throw Error("context_changed", "The player changed while the call ran; its answer was dropped.");
    if ((answer.status >= 300 || answer.status < 200) && !apiAccepted(call, answer.status)) throw wire::errorOf(answer);
    return answer;
}

Answer Api::attempt(const std::string& path, const PlayerCall& call, bool forceRefresh)
{
    const auto token = identity_.accessToken(forceRefresh);
    HttpCall request;
    request.method = call.method;
    request.url = url(path, call.query);
    request.headers.set("authorization", "Bearer " + token);
    request.headers.set("x-krtu-build", opts_.build);
    for (const auto& header : call.headers.all()) request.headers.set(header.first, header.second);
    if (call.body) request.body = *call.body;
    if (call.json)
    {
        request.headers.set("content-type", "application/json");
        request.body = call.json->dump();
    }
    return wire::send(http_, clock_, request);
}
}  // namespace krtu::detail
