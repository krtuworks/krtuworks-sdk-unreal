#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "KrtuworksCore/Error.h"
#include "KrtuworksCore/Http.h"
#include "KrtuworksCore/Json.h"
#include "KrtuworksCore/Store.h"
#include "KrtuworksCore/Types.h"

namespace krtu::detail
{
/// The owner of what is made before any player: that queue goes to the first player who signs in.
constexpr const char* anonOwner = "anon";

// ── Time ────────────────────────────────────────────────────────────────

/// Milliseconds since 1970 on this device's wall clock.
std::int64_t nowMs();
/// `2026-10-29T10:00:00.000Z`.
std::string isoFromMs(std::int64_t ms);
std::string isoNow();
/// An ISO 8601 time (a time without a zone is UTC) or an HTTP date (`Thu, 29 Oct 2026 10:00:00 GMT`), as
/// milliseconds since 1970; nothing when the text is neither.
std::optional<std::int64_t> parseTimeMs(std::string_view text);

/// Waits `duration` in short slices, so a cancelled token ends the wait at once (throws Cancelled).
void sleepFor(std::chrono::milliseconds duration, const CancelToken& cancel);

// ── Ids ─────────────────────────────────────────────────────────────────

/// RFC 9562 UUIDv7: 48 bits of Unix milliseconds, then random. Made once per event, op and batch.
std::string uuidV7(std::optional<std::int64_t> atMs = std::nullopt);
std::string uuidV4();
/// A URL-safe random string (PKCE verifier, state, nonce).
std::string randomToken(std::size_t bytes = 32);

// ── Grammar checks (the server's patterns, without std::regex) ──────────

/// `^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$`
bool isBuildLabel(std::string_view text);
/// `^[a-z][a-z0-9_]{0,63}$`
bool isEventType(std::string_view text);
/// `^[a-z][a-z0-9_.]{0,63}$`
bool isStatKey(std::string_view text);
/// `^pw_[A-Za-z0-9_-]{8,120}$`
bool isPairwiseSubject(std::string_view text);
bool startsWith(std::string_view text, std::string_view prefix);
bool contains(std::string_view text, std::string_view part);
/// The text without spaces, tabs and line breaks at either end.
std::string trimSpace(const std::string& text);
/// The text's length as the server counts it (JavaScript's: UTF-16 units; a character past U+FFFF is two).
std::size_t utf16Length(const std::string& text);

// ── JSON in a store ─────────────────────────────────────────────────────

/// The value kept under `key`, parsed; nothing when there is none or it does not parse.
std::optional<Json> getJson(KeyValueStore& store, const std::string& key);
void putJson(KeyValueStore& store, const std::string& key, const Json& value);
Json orNull(const std::optional<std::string>& value);

// ── Concurrency ─────────────────────────────────────────────────────────

/// One-at-a-time sections by name, shared by every client in this process (two clients on one store
/// guard the same records).
class NamedLocks
{
public:
    static std::unique_lock<std::mutex> take(const std::string& name);
};

/// One run at a time: callers that come while a run is going wait for it and get its result (or its
/// failure); the next caller after it ended starts a fresh run.
template <class T>
class SingleFlight
{
public:
    T run(const std::function<T()>& start)
    {
        std::shared_future<T> joined;
        std::promise<T> mine;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_.valid()) joined = current_;
            else current_ = mine.get_future().share();
        }
        if (joined.valid()) return joined.get();
        try
        {
            T value = start();
            clear();
            mine.set_value(value);
            return value;
        }
        catch (...)
        {
            clear();
            mine.set_exception(std::current_exception());
            throw;
        }
    }

private:
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = std::shared_future<T>();
    }

    std::mutex mutex_;
    std::shared_future<T> current_;
};

/// A thread that runs posted work one piece at a time, in order (what follows a player's arrival). A
/// failure of one piece does not stop the next.
class Worker
{
public:
    Worker();
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void post(std::function<void()> task);
    /// Waits until everything posted before this call has run (at once when called from the worker itself).
    void drain();
    /// Runs what is queued, then ends the thread.
    void stop();

private:
    void loop();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::deque<std::function<void()>> queue_;
    std::uint64_t posted_ = 0;
    std::uint64_t finished_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

/// Time as the server reads it: the device's wall clock, this launch's id, the time since
/// the launch (monotonic) and, once known, how far the device clock is from the server's.
class Clock
{
public:
    Clock();
    const std::string& launchId() const noexcept { return launchId_; }
    std::int64_t monotonicMs() const;
    std::optional<std::int64_t> serverOffsetMs() const;
    std::int64_t serverNowMs() const;
    void noteServerTime(const std::optional<std::string>& value);

private:
    std::string launchId_;
    std::chrono::steady_clock::time_point started_;
    std::atomic<bool> offsetKnown_{false};
    std::atomic<std::int64_t> offsetMs_{0};
};

// ── The wire ────────────────────────────────────────────────────────────

/// A parsed answer: the status, the raw answer and its JSON object body (nothing when there is none).
struct Answer
{
    int status = 0;
    HttpAnswer raw;
    std::optional<Json> json;

    const Json& body() const;
};

using FormFields = std::vector<std::pair<std::string, std::optional<std::string>>>;

namespace wire
{
Answer send(HttpTransport& http, Clock& clock, const HttpCall& call);
/// The error an answer carries: the server and the /v1/guest calls answer `{ error: { code, message } }`, the
/// token endpoint and the device-key challenge OAuth's `{ error, error_description }`.
Error errorOf(const Answer& answer);
/// A form body (the token endpoint wants one); fields without a value are left out.
HttpCall form(const std::string& url, const FormFields& fields);
}  // namespace wire

// ── Options resolved once ───────────────────────────────────────────────

struct Resolved
{
    std::string appId;
    std::string slug;
    std::string type;
    std::string environment;
    std::string platform;
    std::string build;
    std::string clientId;
    std::optional<std::string> deviceClientId;
    std::string idOrigin;
    std::string devOrigin;
    std::string issuer;
    std::optional<DeviceInfo> device;
    std::optional<std::string> locale;
    bool sessions = true;
    std::function<void(std::chrono::milliseconds, const CancelToken&)> delay;

    static constexpr const char* devResource = "https://dev.krtuworks.com";
    static constexpr const char* playerScope = "openid offline_access profile app:player";
    static constexpr const char* guestScope = "openid offline_access app:player";

    /// A game (and not an app): levels, deaths and Steam are for games only.
    bool game() const noexcept { return type == "GAME"; }

    static Resolved from(const Options& options);
};

/// Which module a ready-made event family belongs to; any other type is the app's own (`customEvents`).
std::string moduleOf(const std::string& eventType);

/// Runs `work` and swallows its failure: background work that the next tick tries again.
void quietly(const std::function<void()>& work);
}  // namespace krtu::detail
