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

std::int64_t queueWaitMs(const Error& err, std::atomic<std::int64_t>& backoffMs)
{
    if (err.retryAfter()) return static_cast<std::int64_t>(*err.retryAfter() * 1000);
    if (Error::isWaitingCode(err.code()) || (err.authLayer() && !err.retryable())) return queueLongWaitMs;
    const auto next = (std::min)(static_cast<std::int64_t>(300000), (std::max)(static_cast<std::int64_t>(5000), backoffMs.load() * 2));
    backoffMs.store(next);
    return next;
}

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
    std::string owner;
    if (call.owner)
    {
        owner = *call.owner;
    }
    else
    {
        const auto current = identity_.player();
        if (!current) throw Error("signed_out", "No player is signed in.").asAuthLayer();
        owner = current->subject;
    }
    auto answer = attempt(path, call, false, owner);
    if (answer.status == 401)
    {
        const auto err = wire::errorOf(answer);
        if (err.code() == "account_merged") identity_.markMerged(owner, err.details().getString("mergedInto"));
        if (err.code() == "account_deleted") identity_.markDeleted(owner);
        if (err.code() == "token_expired" || err.code() == "token_revoked" || err.code() == "invalid_token") answer = attempt(path, call, true, owner);
    }
    if (identity_.generation() != generation) throw Error("context_changed", "The player changed while the call ran; its answer was dropped.");
    if ((answer.status >= 300 || answer.status < 200) && !apiAccepted(call, answer.status))
    {
        auto err = wire::errorOf(answer);
        if (err.code() == "account_merging") identity_.noteMerging(owner);
        throw err;
    }
    identity_.noteMergeEnded(owner);
    return answer;
}

Answer Api::attempt(const std::string& path, const PlayerCall& call, bool forceRefresh, const std::string& owner)
{
    Credentials credentials;
    try
    {
        credentials = identity_.credentials(forceRefresh);
    }
    catch (Error& err)
    {
        // The credentials failed before the endpoint was called: no queue may read this as a refusal of its body.
        err.asAuthLayer();
        throw;
    }
    if (credentials.subject != owner) throw Error("context_changed", "The player changed while the call ran; it was dropped.").asAuthLayer();
    HttpCall request;
    request.method = call.method;
    request.url = url(path, call.query);
    request.headers.set("authorization", "Bearer " + credentials.accessToken);
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
