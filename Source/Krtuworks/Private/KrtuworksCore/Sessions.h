#pragma once

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
    SessionKeeper(const Resolved& opts, Api& api, std::function<std::optional<std::string>()> subject, std::function<std::optional<std::string>()> platformTicket);

    /// The session of the player who plays now, if one is open.
    std::optional<SessionInfo> session() const;
    bool foreground() const;
    /// Seconds in use in this launch (foreground only).
    std::int64_t activeSeconds() const;
    void setForeground(bool foreground);
    /// Opens the player's session if there is none (one at a time).
    std::optional<SessionInfo> open();
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
    std::int64_t elapsedMs() const;

    const Resolved& opts_;
    Api& api_;
    std::function<std::optional<std::string>()> subject_;
    /// A Steam ticket for the session's ownership check, when the platform gives one.
    std::function<std::optional<std::string>()> platformTicket_;
    std::chrono::steady_clock::time_point started_;
    mutable std::mutex mutex_;
    std::optional<SessionInfo> current_;
    std::optional<std::string> clientSessionId_;
    int heartbeatSeconds_ = 60;
    std::int64_t nextHeartbeatAt_ = 0;
    std::int64_t activeMs_ = 0;
    std::optional<std::int64_t> activeSince_;
    std::mutex openMutex_;
    std::function<void(const Json&)> onEntitlements_;
};
}  // namespace krtu::detail
