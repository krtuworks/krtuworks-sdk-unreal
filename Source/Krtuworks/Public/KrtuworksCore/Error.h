#pragma once

#include "KrtuworksCore/Export.h"

#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#include "KrtuworksCore/Json.h"

namespace krtu
{
/// Every failure the SDK reports. code() is the server's code (the server's `error.code`, Krtuworks ID's
/// OAuth `error`) or one of the SDK's own: `network_error` (no answer), `context_changed` (the player
/// changed while the call ran; its answer was dropped), `signed_out`, `birth_date_required`.
/// Wrong arguments from the game are std::invalid_argument instead, a wrong setup std::logic_error.
class KRTU_API Error : public std::exception
{
public:
    Error(std::string code, std::string message, int status = 0, std::optional<double> retryAfter = std::nullopt, Json details = Json::object());

    const std::string& code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    /// HTTP status; 0 when nothing came back.
    int status() const noexcept { return status_; }
    /// Seconds to wait before trying again, when the server said so.
    std::optional<double> retryAfter() const noexcept { return retryAfter_; }
    /// The rest of the server's answer (`mergedInto`, `current`, `endsAt`, `terms`, `description` …).
    const Json& details() const noexcept { return details_; }
    Json& details() noexcept { return details_; }
    const char* what() const noexcept override { return what_.c_str(); }

    /// True when the same call can succeed later (no answer, a busy or briefly closed server).
    bool retryable() const noexcept { return status_ == 0 || status_ == 429 || status_ >= 500 || code_ == "account_merging"; }

private:
    std::string code_;
    std::string message_;
    int status_;
    std::optional<double> retryAfter_;
    Json details_;
    std::string what_;
};

/// A wait the caller cancelled through its CancelToken (a sign-in the player closed, the app stopping).
class KRTU_API Cancelled : public std::exception
{
public:
    const char* what() const noexcept override { return "cancelled"; }
};

/// A flag the caller sets to stop a long wait (a sign-in in the browser, a console's code). Copies share the flag.
class KRTU_API CancelToken
{
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() const noexcept { flag_->store(true); }
    bool cancelled() const noexcept { return flag_->load(); }
    /// Throws Cancelled once the token is cancelled. Defined out of line: a game module that includes this
    /// header may be built without exceptions.
    void throwIfCancelled() const;

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};
}  // namespace krtu
