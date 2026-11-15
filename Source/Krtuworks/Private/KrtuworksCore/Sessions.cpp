#include "Sessions.h"

#include <exception>

#include <limits>

namespace krtu::detail
{
namespace
{
constexpr std::int64_t sessionsNever = (std::numeric_limits<std::int64_t>::max)();
}  // namespace

SessionKeeper::SessionKeeper(const Resolved& opts, Api& api, std::function<std::optional<std::string>()> subject, std::function<std::optional<std::string>()> platformTicket)
    : opts_(opts), api_(api), subject_(std::move(subject)), platformTicket_(std::move(platformTicket)), started_(std::chrono::steady_clock::now()), activeSince_(0)
{
    nextHeartbeatAt_ = sessionsNever;
}

std::int64_t SessionKeeper::elapsedMs() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
}

std::optional<SessionInfo> SessionKeeper::session() const
{
    const auto owner = subject_();
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_ && owner && current_->owner == *owner) return current_;
    return std::nullopt;
}

bool SessionKeeper::foreground() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return activeSince_.has_value();
}

std::int64_t SessionKeeper::activeSeconds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return (activeMs_ + (activeSince_ ? elapsedMs() - *activeSince_ : 0)) / 1000;
}

void SessionKeeper::setForeground(bool foreground)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (foreground && !activeSince_) activeSince_ = elapsedMs();
    if (!foreground && activeSince_)
    {
        activeMs_ += elapsedMs() - *activeSince_;
        activeSince_.reset();
    }
}

std::optional<SessionInfo> SessionKeeper::open()
{
    if (auto existing = session()) return existing;
    std::lock_guard<std::mutex> one(openMutex_);
    if (auto existing = session()) return existing;
    return doOpen();
}

std::optional<SessionInfo> SessionKeeper::doOpen()
{
    const auto owner = subject_();
    if (!owner) return std::nullopt;
    std::string clientSessionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_ && current_->owner != *owner)
        {
            current_.reset();
            clientSessionId_.reset();
            nextHeartbeatAt_ = sessionsNever;
        }
        if (!clientSessionId_) clientSessionId_ = uuidV4();
        clientSessionId = *clientSessionId_;
    }
    auto body = Json::object({{"clientSessionId", clientSessionId}, {"platform", opts_.platform}, {"build", opts_.build}});
    if (opts_.device) body["device"] = opts_.device->toJson();
    if (opts_.locale) body["locale"] = *opts_.locale;
    if (platformTicket_)
    {
        if (const auto ticket = platformTicket_()) body["platformProof"] = Json::object({{"steamTicket", *ticket}});
    }
    PlayerCall call;
    call.method = "POST";
    call.json = body;
    const auto answer = api_.player("/v1/sessions", call);
    const auto& session = answer.body()["session"];
    if (!session.isObject() || subject_() != owner) return std::nullopt;
    std::optional<SessionInfo> opened;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = SessionInfo{session.getString("id").value_or(""), *owner, session.getString("startedAt").value_or(""), linkOf(answer.body()["platformLink"])};
        if (const auto seconds = answer.body().getInt("heartbeatSeconds")) heartbeatSeconds_ = static_cast<int>(*seconds);
        nextHeartbeatAt_ = elapsedMs() + static_cast<std::int64_t>(heartbeatSeconds_) * 1000;
        opened = current_;
    }
    if (onEntitlements_)
    {
        try
        {
            onEntitlements_(answer.body()["entitlements"]);
        }
        catch (const std::exception&)
        {
            // What the licences kept is not worth the session.
        }
    }
    return opened;
}

void SessionKeeper::setEntitlementsHandler(std::function<void(const Json&)> handler)
{
    onEntitlements_ = std::move(handler);
}

std::optional<PlatformLink> SessionKeeper::linkOf(const Json& value)
{
    const auto accountId = value.getString("platformAccountId");
    const auto state = value.getString("state");
    if (!value.isObject() || !accountId || !state) return std::nullopt;
    return PlatformLink{value.getString("platform").value_or(""), *accountId, *state};
}

void SessionKeeper::setPlatformLink(const std::string& id, const PlatformLink& link)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_ && current_->id == id) current_->platformLink = link;
}

bool SessionKeeper::due()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (elapsedMs() < nextHeartbeatAt_) return false;
    }
    heartbeat();
    return true;
}

void SessionKeeper::heartbeat()
{
    const auto current = session();
    if (!current) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nextHeartbeatAt_ = elapsedMs() + static_cast<std::int64_t>(heartbeatSeconds_) * 1000;
    }
    try
    {
        PlayerCall call;
        call.method = "POST";
        call.json = Json::object({{"activeSeconds", activeSeconds()}, {"foreground", foreground()}});
        const auto answer = api_.player("/v1/sessions/" + current->id + "/heartbeat", call);
        const auto next = answer.body().getInt("nextHeartbeatSeconds");
        if (next && *next > 0)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            heartbeatSeconds_ = static_cast<int>(*next);
            nextHeartbeatAt_ = elapsedMs() + static_cast<std::int64_t>(heartbeatSeconds_) * 1000;
        }
    }
    catch (const Error& err)
    {
        if (err.code() != "session_ended" && err.code() != "session_not_found") return;
        // The server closed it (30 minutes without a heartbeat): a new one, with a new id.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_.reset();
            clientSessionId_.reset();
        }
        try
        {
            open();
        }
        catch (const Error&)
        {
            // The next tick tries again.
        }
    }
}

void SessionKeeper::round()
{
    const auto current = open();
    if (!current) throw Error("signed_out", "No player is signed in.");
    std::optional<Error> failure;
    try
    {
        PlayerCall call;
        call.method = "POST";
        call.json = Json::object({{"activeSeconds", activeSeconds()}, {"foreground", foreground()}});
        api_.player("/v1/sessions/" + current->id + "/heartbeat", call);
    }
    catch (const Error& err)
    {
        failure = err;
    }
    const auto active = activeSeconds();
    close();
    PlayerCall call;
    call.method = "POST";
    call.json = Json::object({{"activeSeconds", active}});
    try
    {
        api_.player("/v1/sessions/" + current->id + "/end", call);
    }
    catch (const Error&)
    {
        if (!failure) throw;
    }
    if (failure) throw *failure;
}

void SessionKeeper::end()
{
    const auto current = session();
    const auto active = activeSeconds();
    close();
    if (!current) return;
    try
    {
        PlayerCall call;
        call.method = "POST";
        call.json = Json::object({{"activeSeconds", active}});
        api_.player("/v1/sessions/" + current->id + "/end", call);
    }
    catch (const Error&)
    {
        // The server times it out.
    }
}

void SessionKeeper::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    current_.reset();
    clientSessionId_.reset();
    nextHeartbeatAt_ = sessionsNever;
}
}  // namespace krtu::detail
