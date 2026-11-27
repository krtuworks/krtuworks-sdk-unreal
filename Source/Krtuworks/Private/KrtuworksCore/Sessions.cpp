#include "Sessions.h"

#include <algorithm>
#include <exception>
#include <limits>

namespace krtu::detail
{
namespace
{
constexpr std::int64_t sessionsNever = (std::numeric_limits<std::int64_t>::max)();
}  // namespace

SessionKeeper::SessionKeeper(const Resolved& opts, Api& api, std::function<std::optional<std::string>()> subject, std::function<std::optional<std::string>()> platformTicket,
                             std::function<void(const std::string&)> cancelTicket)
    : opts_(opts),
      api_(api),
      subject_(std::move(subject)),
      platformTicket_(std::move(platformTicket)),
      cancelTicket_(std::move(cancelTicket)),
      started_(std::chrono::steady_clock::now()), activeSince_(0)
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

std::int64_t SessionKeeper::launchActiveMsLocked() const
{
    return activeMs_ + (activeSince_ ? elapsedMs() - *activeSince_ : 0);
}

std::int64_t SessionKeeper::activeSeconds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return launchActiveMsLocked() / 1000;
}

std::int64_t SessionKeeper::sessionActiveSeconds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return (std::max)(static_cast<std::int64_t>(0), launchActiveMsLocked() - sessionBaseMs_) / 1000;
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
    return openOnce();
}

std::optional<SessionInfo> SessionKeeper::openOnce()
{
    std::uint64_t startedIn = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        startedIn = generation_;
    }
    try
    {
        auto opened = doOpen();
        std::lock_guard<std::mutex> lock(mutex_);
        openFailed_ = false;
        openBackoffMs_.store(0);
        return opened;
    }
    catch (const Error& err)
    {
        // Whatever the reason, the session is asked for again later: never given up for the rest of the run.
        const auto wait = queueWaitMs(err, openBackoffMs_);
        std::lock_guard<std::mutex> lock(mutex_);
        // A close since this open began ended the wish for a session: no retry waits for it.
        if (generation_ != startedIn) throw;
        openFailed_ = true;
        openNotBeforeMs_ = elapsedMs() + wait;
        throw;
    }
}

void SessionKeeper::retryOpen()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!openFailed_ || elapsedMs() < openNotBeforeMs_) return;
    }
    // An open already running is the retry.
    std::unique_lock<std::mutex> one(openMutex_, std::try_to_lock);
    if (!one.owns_lock() || session()) return;
    try
    {
        openOnce();
    }
    catch (const Error&)
    {
        // The wait grew; the next tick after it tries again.
    }
}

bool SessionKeeper::openFailed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return openFailed_;
}

void SessionKeeper::resetOpenBackoff()
{
    std::lock_guard<std::mutex> lock(mutex_);
    openNotBeforeMs_ = 0;
    openBackoffMs_.store(0);
}

std::optional<SessionInfo> SessionKeeper::doOpen()
{
    const auto owner = subject_();
    if (!owner) return std::nullopt;
    std::string clientSessionId;
    std::uint64_t openedIn = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_ && current_->owner != *owner)
        {
            current_.reset();
            clientSessionId_.reset();
            nextHeartbeatAt_ = sessionsNever;
        }
        openedIn = generation_;
        if (!clientSessionId_)
        {
            // A new session (not an open sent again after a lost answer): its time in use starts now.
            clientSessionId_ = uuidV4();
            sessionBaseMs_ = launchActiveMsLocked();
        }
        clientSessionId = *clientSessionId_;
    }
    auto body = Json::object({{"clientSessionId", clientSessionId}, {"platform", opts_.platform}, {"build", opts_.build}});
    if (opts_.device) body["device"] = opts_.device->toJson();
    if (opts_.locale) body["locale"] = *opts_.locale;
    std::optional<std::string> ticket;
    if (platformTicket_) ticket = platformTicket_();
    if (ticket) body["platformProof"] = Json::object({{"steamTicket", *ticket}});
    PlayerCall call;
    call.method = "POST";
    call.json = body;
    call.owner = *owner;
    Answer answer;
    try
    {
        answer = api_.player("/v1/sessions", call);
    }
    catch (const Error& err)
    {
        // A ticket the call never carried (the credentials failed first) is not left valid.
        if (err.authLayer() && ticket && cancelTicket_) quietly([this, &ticket] { cancelTicket_(*ticket); });
        throw;
    }
    const auto& session = answer.body()["session"];
    if (!session.isObject()) return std::nullopt;
    std::optional<SessionInfo> opened;
    bool late = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        late = generation_ != openedIn;
    }
    if (late)
    {
        // Closed while the open was on its way (the SDK shut down, the player left): the server's session ends now,
        // whoever plays by the time the answer comes.
        const auto lateId = session.getString("id").value_or("");
        if (!lateId.empty())
        {
            quietly([this, &lateId, &owner] {
                PlayerCall end;
                end.method = "POST";
                end.json = Json::object({{"activeSeconds", 0}});
                end.owner = *owner;
                api_.player("/v1/sessions/" + lateId + "/end", end);
            });
        }
        return std::nullopt;
    }
    if (subject_() != owner) return std::nullopt;
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
    std::uint64_t beatIn = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nextHeartbeatAt_ = elapsedMs() + static_cast<std::int64_t>(heartbeatSeconds_) * 1000;
        beatIn = generation_;
    }
    try
    {
        PlayerCall call;
        call.method = "POST";
        call.json = Json::object({{"activeSeconds", sessionActiveSeconds()}, {"foreground", foreground()}});
        call.owner = current->owner;
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
        // The server closed it (30 minutes without a heartbeat): a new one, with a new id; not after a close.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation_ != beatIn) return;
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
        call.json = Json::object({{"activeSeconds", sessionActiveSeconds()}, {"foreground", foreground()}});
        call.owner = current->owner;
        api_.player("/v1/sessions/" + current->id + "/heartbeat", call);
    }
    catch (const Error& err)
    {
        failure = err;
    }
    const auto active = sessionActiveSeconds();
    close();
    PlayerCall call;
    call.method = "POST";
    call.json = Json::object({{"activeSeconds", active}});
    call.owner = current->owner;
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
    const auto active = sessionActiveSeconds();
    close();
    if (!current) return;
    try
    {
        PlayerCall call;
        call.method = "POST";
        call.json = Json::object({{"activeSeconds", active}});
        call.owner = current->owner;
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
    ++generation_;
    current_.reset();
    clientSessionId_.reset();
    nextHeartbeatAt_ = sessionsNever;
    openFailed_ = false;
    openNotBeforeMs_ = 0;
    openBackoffMs_.store(0);
}
}  // namespace krtu::detail
