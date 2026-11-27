#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "Api.h"

namespace krtu::detail
{
/// The play session, the rules of krtuworks-sdk-js/src/sessions.ts: opened once per launch and
/// player, kept alive every `heartbeatSeconds` with the time actually in use (foreground only), ended when the
/// app goes away. An open whose answer was lost is sent again with the same `clientSessionId` and gets the
/// same session. No timer of its own: the client's tick asks due().
class SessionKeeper
{
public:
    SessionKeeper(const Resolved& opts, Api& api, std::function<std::optional<std::string>()> subject, std::function<std::optional<std::string>()> platformTicket,
                  std::function<void(const std::string&)> cancelTicket = nullptr);

    /// The session of the player who plays now, if one is open.
    std::optional<SessionInfo> session() const;
    bool foreground() const;
    /// Seconds in use in this launch (foreground only).
    std::int64_t activeSeconds() const;
    /// Seconds in use since the current session began (foreground only): what the session reports. A session
    /// opened after another one (a new player, a timeout) starts from zero, not from the launch's total.
    std::int64_t sessionActiveSeconds() const;
    void setForeground(bool foreground);
    /// Opens the player's session if there is none (one at a time).
    std::optional<SessionInfo> open();
    /// Opens the session again when the last open failed (offline, a busy server, a refusal that can lift) and its
    /// wait is over; never throws. The client's tick calls it.
    void retryOpen();
    /// The last open failed: retryOpen asks again once its wait is over.
    bool openFailed() const;
    /// The network is back or the player changed: the next retryOpen tries at once.
    void resetOpenBackoff();
    /// The server's `platformLink` (the session open's, the consent answer's); empty when it is not one.
    static std::optional<PlatformLink> linkOf(const Json& value);
    /// Keeps the player's answer to a waiting platform account on the open session `id`.
    void setPlatformLink(const std::string& id, const PlatformLink& link);
    /// Told the licence list of a session answer (only an open that sent a platform proof gets one), on the opening thread.
    void setEntitlementsHandler(std::function<void(const Json&)> handler);
    /// Sends the heartbeat when it is due; true when it went (or was tried).
    bool due();
    void heartbeat();
    /// One whole session where every step must answer: open, a heartbeat, end (the validator's round). Throws
    /// the first refusal; the session is ended even when its heartbeat failed.
    void round();
    /// Ends the session.
    void end();
    /// Forgets the session without telling the server (the player changed: it times out there).
    void close();

private:
    /// The server's `platformLink` (the session open's, the consent answer's); empty when it is not one.
    std::optional<SessionInfo> doOpen();
    std::optional<SessionInfo> openOnce();
    std::int64_t elapsedMs() const;
    /// Under `mutex_`.
    std::int64_t launchActiveMsLocked() const;

    const Resolved& opts_;
    Api& api_;
    std::function<std::optional<std::string>()> subject_;
    /// A Steam ticket for the session's ownership check, when the platform gives one.
    std::function<std::optional<std::string>()> platformTicket_;
    /// Cancels a ticket that was asked for and never sent.
    std::function<void(const std::string&)> cancelTicket_;
    std::chrono::steady_clock::time_point started_;
    mutable std::mutex mutex_;
    std::optional<SessionInfo> current_;
    std::optional<std::string> clientSessionId_;
    int heartbeatSeconds_ = 60;
    std::int64_t nextHeartbeatAt_ = 0;
    std::int64_t activeMs_ = 0;
    std::optional<std::int64_t> activeSince_;
    /// The launch's time in use when the current session began: the server counts only what follows.
    std::int64_t sessionBaseMs_ = 0;
    std::mutex openMutex_;
    /// The last open failed: retryOpen tries again when `openNotBeforeMs_` (on this keeper's clock) has passed.
    bool openFailed_ = false;
    std::int64_t openNotBeforeMs_ = 0;
    std::atomic<std::int64_t> openBackoffMs_{0};
    std::function<void(const Json&)> onEntitlements_;
    /// Moves on every close (under `mutex_`): an answer that arrives after it belongs to a session nobody wants any more.
    std::uint64_t generation_ = 0;
};
}  // namespace krtu::detail
