#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Api.h"

namespace krtu::detail
{
/// An event as `/v1/events` takes it.
struct WireEvent
{
    std::string id;
    std::string type;
    std::string createdAt;
    std::string launchId;
    std::int64_t monotonicMs = 0;
    std::optional<std::int64_t> serverOffsetMs;
    std::optional<std::string> sessionId;
    std::optional<std::string> build;
    std::optional<int> schemaVersion;
    std::optional<EventPosition> position;
    std::optional<Json> data;

    Json toJson() const;
    static std::optional<WireEvent> fromJson(const Json& json);
};

/// A batch frozen before its first send: the same id always carries the same platform, build and events.
struct FrozenBatch
{
    std::string batchId;
    std::string platform;
    std::string build;
    std::vector<WireEvent> events;
};

struct EventQueueState
{
    std::vector<WireEvent> pending;
    std::optional<FrozenBatch> batch;
    /// Events dropped here (queue full, too old, refused); the count is reported once with an error event.
    std::int64_t dropped = 0;
    std::int64_t reported = 0;

    Json toJson() const;
    static EventQueueState fromJson(const Json& json);
};

/// Telemetry events, the rules of krtuworks-sdk-js/src/events.ts: every
/// event is made with its id and bound to the player it is about (`anon` before any account; that queue goes
/// to the first player). Queued events are written to the store before they are sent; a batch is frozen
/// before its first send and sent again unchanged after a failure (the server hashes platform + build +
/// events); each event's answer is read on its own: accepted, duplicate and rejected leave the queue, retry
/// goes back with its id into a new batch.
class EventQueue
{
public:
    static constexpr const char* anon = anonOwner;

    EventQueue(const Resolved& opts, KeyValueStore& store, Clock& clock, Api& api, std::function<std::optional<std::string>()> subject);

    /// Makes an event now, for whoever plays now. Kept in memory until the next persist (at most a second
    /// later on the background loop).
    WireEvent add(const std::string& type, const std::optional<Json>& data, const std::optional<EventPosition>& position, const std::optional<std::string>& sessionId,
                  const std::optional<int>& schemaVersion);
    /// Whether events wait in memory for the store.
    bool persistDue() const;
    /// Writes the events made since the last write; drops the oldest when the queue is full or too old.
    void persist();
    /// Events waiting for `owner` (written and not yet written).
    int size(const std::string& owner);
    /// Moves `from`'s queue to `to`: the events made before any account go to the first player; a merged
    /// account's go to the one it went into (the server counts each event once by its id, and a batch it
    /// took under the old account answers the same under the new one).
    void move(const std::string& from, const std::string& to);
    /// Sends the signed-in player's queue, batch by batch, until it is empty or a send fails (the frozen
    /// batch then waits, unchanged, for the next flush).
    FlushResult flush(bool errorsOn, int maxBatches = 20);
    /// Lets the next flush try at once (back online, or the player changed).
    void resetBackoff();

private:
    std::string key(const std::string& owner) const;
    EventQueueState load(const std::string& owner);
    void save(const std::string& owner, const EventQueueState& state);
    static void trim(EventQueueState& state);
    std::optional<FrozenBatch> freeze(EventQueueState& state, std::size_t limit) const;
    std::optional<WireEvent> droppedNotice(EventQueueState& state, bool errorsOn) const;
    std::string sendBatch(const std::string& owner, const FrozenBatch& batch, FlushResult& result);
    void unfreeze(const std::string& owner, const FrozenBatch& batch);
    std::string failed(const std::string& owner, const FrozenBatch& batch, const Error& err);

    const Resolved& opts_;
    KeyValueStore& store_;
    Clock& clock_;
    Api& api_;
    std::function<std::optional<std::string>()> subject_;
    mutable std::mutex bufferMutex_;
    std::vector<std::pair<std::string, WireEvent>> buffer_;
    std::atomic<std::int64_t> notBefore_{0};
    std::atomic<std::int64_t> backoffMs_{0};
};
}  // namespace krtu::detail
