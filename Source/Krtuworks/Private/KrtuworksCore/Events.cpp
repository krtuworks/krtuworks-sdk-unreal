#include "Events.h"

#include <algorithm>
#include <iterator>
#include <set>

#include "KrtuworksCore/Gzip.h"

namespace krtu::detail
{
namespace
{
constexpr std::size_t eventsMaxCount = 50000;
constexpr std::size_t eventsMaxBytes = 20u * 1024u * 1024u;
constexpr std::int64_t eventsMaxAgeMs = 30LL * 86400000LL;
constexpr std::size_t eventsBatchCount = 500;
constexpr std::size_t eventsSentLimit = 256u * 1024u;
/// Batches are built under this raw size and halved when gzip still overshoots the server's 256 KB.
constexpr std::size_t eventsRawLimit = 1500000;

std::size_t eventsSizeOf(const WireEvent& e)
{
    return e.toJson().dump().size();
}

Json eventsBatchJson(const FrozenBatch& batch)
{
    auto events = Json::array();
    for (const auto& e : batch.events) events.push(e.toJson());
    return Json::object({{"batchId", batch.batchId}, {"platform", batch.platform}, {"build", batch.build}, {"events", events}});
}

std::optional<FrozenBatch> eventsBatchFrom(const Json& json)
{
    if (!json.isObject()) return std::nullopt;
    FrozenBatch batch;
    batch.batchId = json.getString("batchId").value_or("");
    batch.platform = json.getString("platform").value_or("");
    batch.build = json.getString("build").value_or("");
    for (const auto& item : json["events"].items())
    {
        if (auto e = WireEvent::fromJson(item)) batch.events.push_back(std::move(*e));
    }
    if (batch.batchId.empty()) return std::nullopt;
    return batch;
}
}  // namespace

Json WireEvent::toJson() const
{
    auto json = Json::object({{"id", id}, {"type", type}, {"createdAt", createdAt}, {"launchId", launchId}, {"monotonicMs", monotonicMs}});
    if (serverOffsetMs) json["serverOffsetMs"] = *serverOffsetMs;
    if (sessionId) json["sessionId"] = *sessionId;
    if (build) json["build"] = *build;
    if (schemaVersion) json["schemaVersion"] = *schemaVersion;
    if (position) json["position"] = position->toJson();
    if (data) json["data"] = *data;
    return json;
}

std::optional<WireEvent> WireEvent::fromJson(const Json& json)
{
    if (!json.isObject()) return std::nullopt;
    WireEvent e;
    e.id = json.getString("id").value_or("");
    e.type = json.getString("type").value_or("");
    e.createdAt = json.getString("createdAt").value_or("");
    e.launchId = json.getString("launchId").value_or("");
    e.monotonicMs = json.getInt("monotonicMs").value_or(0);
    e.serverOffsetMs = json.getInt("serverOffsetMs");
    e.sessionId = json.getString("sessionId");
    e.build = json.getString("build");
    if (const auto version = json.getInt("schemaVersion")) e.schemaVersion = static_cast<int>(*version);
    if (const auto* position = json.find("position"); position != nullptr && position->isObject()) e.position = EventPosition::fromJson(*position);
    if (const auto* data = json.find("data"); data != nullptr && data->isObject()) e.data = *data;
    return e;
}

Json EventQueueState::toJson() const
{
    auto list = Json::array();
    for (const auto& e : pending) list.push(e.toJson());
    auto json = Json::object({{"pending", list}});
    if (batch) json["batch"] = eventsBatchJson(*batch);
    json["dropped"] = dropped;
    json["reported"] = reported;
    return json;
}

EventQueueState EventQueueState::fromJson(const Json& json)
{
    EventQueueState state;
    for (const auto& item : json["pending"].items())
    {
        if (auto e = WireEvent::fromJson(item)) state.pending.push_back(std::move(*e));
    }
    state.batch = eventsBatchFrom(json["batch"]);
    state.dropped = json.getInt("dropped").value_or(0);
    state.reported = json.getInt("reported").value_or(0);
    return state;
}

EventQueue::EventQueue(const Resolved& opts, KeyValueStore& store, Clock& clock, Api& api, std::function<std::optional<std::string>()> subject)
    : opts_(opts), store_(store), clock_(clock), api_(api), subject_(std::move(subject))
{
}

std::string EventQueue::key(const std::string& owner) const
{
    return "krtu:" + opts_.clientId + ":events:" + owner;
}

EventQueueState EventQueue::load(const std::string& owner)
{
    const auto json = getJson(store_, key(owner));
    return json ? EventQueueState::fromJson(*json) : EventQueueState();
}

void EventQueue::save(const std::string& owner, const EventQueueState& state)
{
    if (state.pending.empty() && !state.batch && state.dropped == state.reported) store_.remove(key(owner));
    else putJson(store_, key(owner), state.toJson());
}

WireEvent EventQueue::add(const std::string& type, const std::optional<Json>& data, const std::optional<EventPosition>& position, const std::optional<std::string>& sessionId,
                          const std::optional<int>& schemaVersion)
{
    WireEvent made;
    made.id = uuidV7();
    made.type = type;
    made.createdAt = isoNow();
    made.launchId = clock_.launchId();
    made.monotonicMs = clock_.monotonicMs();
    made.serverOffsetMs = clock_.serverOffsetMs();
    made.sessionId = sessionId;
    made.schemaVersion = schemaVersion;
    made.position = position;
    made.data = data;
    const auto owner = subject_().value_or(anon);
    std::lock_guard<std::mutex> lock(bufferMutex_);
    buffer_.emplace_back(owner, made);
    return made;
}

bool EventQueue::persistDue() const
{
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return !buffer_.empty();
}

void EventQueue::persist()
{
    std::vector<std::pair<std::string, WireEvent>> taken;
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        taken.swap(buffer_);
    }
    std::vector<std::pair<std::string, std::vector<WireEvent>>> groups;
    for (auto& entry : taken)
    {
        auto group = std::find_if(groups.begin(), groups.end(), [&entry](const auto& g) { return g.first == entry.first; });
        if (group == groups.end())
        {
            groups.emplace_back(entry.first, std::vector<WireEvent>());
            group = groups.end() - 1;
        }
        group->second.push_back(std::move(entry.second));
    }
    std::vector<std::pair<std::string, WireEvent>> unwritten;
    for (auto& group : groups)
    {
        try
        {
            auto lock = NamedLocks::take(key(group.first));
            auto state = load(group.first);
            for (const auto& e : group.second) state.pending.push_back(e);
            trim(state);
            save(group.first, state);
        }
        catch (...)
        {
            // The store refused the write (full, busy): the events stay in memory for the next persist.
            for (auto& e : group.second) unwritten.emplace_back(group.first, std::move(e));
        }
    }
    if (unwritten.empty()) return;
    std::lock_guard<std::mutex> lock(bufferMutex_);
    buffer_.insert(buffer_.begin(), std::make_move_iterator(unwritten.begin()), std::make_move_iterator(unwritten.end()));
    if (buffer_.size() > eventsMaxCount) buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_.size() - eventsMaxCount));
}

void EventQueue::trim(EventQueueState& state) const
{
    const auto oldest = clock_.serverNowMs() - eventsMaxAgeMs;
    const auto before = state.pending.size();
    state.pending.erase(std::remove_if(state.pending.begin(), state.pending.end(),
                                       [oldest](const WireEvent& e) {
                                           const auto at = parseTimeMs(e.createdAt);
                                           return at && *at + e.serverOffsetMs.value_or(0) < oldest;
                                       }),
                        state.pending.end());
    std::size_t bytes = 0;
    std::size_t count = state.pending.size();
    for (const auto& e : state.pending) bytes += eventsSizeOf(e);
    if (state.batch)
    {
        for (const auto& e : state.batch->events) bytes += eventsSizeOf(e);
        count += state.batch->events.size();
    }
    std::size_t cut = 0;
    while (state.pending.size() > cut && (count > eventsMaxCount || bytes > eventsMaxBytes))
    {
        bytes -= eventsSizeOf(state.pending[cut]);
        --count;
        ++cut;
    }
    state.pending.erase(state.pending.begin(), state.pending.begin() + static_cast<std::ptrdiff_t>(cut));
    state.dropped += static_cast<std::int64_t>(before - state.pending.size());
}

int EventQueue::size(const std::string& owner)
{
    const auto state = load(owner);
    std::size_t inMemory = 0;
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        for (const auto& entry : buffer_) inMemory += entry.first == owner ? 1 : 0;
    }
    return static_cast<int>(state.pending.size() + (state.batch ? state.batch->events.size() : 0) + inMemory);
}

void EventQueue::move(const std::string& from, const std::string& to)
{
    persist();
    if (from == to) return;
    const auto& first = from < to ? from : to;
    const auto& second = from < to ? to : from;
    auto firstLock = NamedLocks::take(key(first));
    auto secondLock = NamedLocks::take(key(second));
    auto source = load(from);
    if (source.pending.empty() && !source.batch && source.dropped == source.reported) return;
    // The dropped count leaves the source before it reaches the target: a move cut short between the writes
    // loses it rather than says it twice.
    const auto unsaid = source.dropped - source.reported;
    if (unsaid > 0)
    {
        source.reported = source.dropped;
        save(from, source);
    }
    auto target = load(to);
    // A move run again after it was cut short brings events the target holds already: they go once.
    std::set<std::string> held;
    for (const auto& e : target.pending) held.insert(e.id);
    if (target.batch)
    {
        for (const auto& e : target.batch->events) held.insert(e.id);
    }
    const auto fresh = [&held](const std::vector<WireEvent>& events) {
        std::vector<WireEvent> out;
        for (const auto& e : events)
        {
            if (held.count(e.id) == 0) out.push_back(e);
        }
        return out;
    };
    if (source.batch && (!target.batch || source.batch->batchId != target.batch->batchId))
    {
        if (target.batch)
        {
            const auto events = fresh(source.batch->events);
            target.pending.insert(target.pending.begin(), events.begin(), events.end());
        }
        else
        {
            target.batch = source.batch;
        }
    }
    const auto events = fresh(source.pending);
    target.pending.insert(target.pending.end(), events.begin(), events.end());
    target.dropped += (std::max)(static_cast<std::int64_t>(0), unsaid);
    trim(target);
    save(to, target);
    store_.remove(key(from));
}

std::optional<FrozenBatch> EventQueue::freeze(EventQueueState& state, std::size_t limit) const
{
    if (state.pending.empty()) return std::nullopt;
    FrozenBatch batch;
    batch.batchId = uuidV7();
    batch.platform = opts_.platform;
    batch.build = opts_.build;
    std::size_t bytes = 0;
    for (const auto& e : state.pending)
    {
        const auto size = eventsSizeOf(e) + 1;
        if (batch.events.size() >= limit || (!batch.events.empty() && bytes + size > eventsRawLimit)) break;
        batch.events.push_back(e);
        bytes += size;
    }
    state.pending.erase(state.pending.begin(), state.pending.begin() + static_cast<std::ptrdiff_t>(batch.events.size()));
    return batch;
}

std::optional<WireEvent> EventQueue::droppedNotice(EventQueueState& state, bool errorsOn) const
{
    const auto unsaid = state.dropped - state.reported;
    if (unsaid <= 0) return std::nullopt;
    state.reported = state.dropped;
    if (!errorsOn) return std::nullopt;
    WireEvent notice;
    notice.id = uuidV7();
    notice.type = "error";
    notice.createdAt = isoNow();
    notice.launchId = clock_.launchId();
    notice.monotonicMs = clock_.monotonicMs();
    notice.data = Json::object({{"kind", "other"},
                                {"message", std::to_string(unsaid) + " events were dropped on this device: the queue was full or too old"},
                                {"fingerprint", "sdk.events_dropped"},
                                {"handled", true}});
    return notice;
}

FlushResult EventQueue::flush(bool errorsOn, int maxBatches)
{
    persist();
    FlushResult result;
    const auto owner = subject_();
    if (!owner)
    {
        result.stoppedBy = "signed_out";
        return result;
    }
    if (nowMs() < notBefore_.load())
    {
        result.stoppedBy = "waiting";
        return result;
    }
    std::size_t limit = eventsBatchCount;
    for (int round = 0; round < maxBatches; ++round)
    {
        std::optional<FrozenBatch> batch;
        {
            auto lock = NamedLocks::take(key(*owner));
            auto state = load(*owner);
            if (!state.batch)
            {
                if (auto notice = droppedNotice(state, errorsOn)) state.pending.push_back(std::move(*notice));
                state.batch = freeze(state, limit);
            }
            save(*owner, state);
            batch = state.batch;
        }
        if (!batch) return result;
        const auto outcome = sendBatch(*owner, *batch, result);
        if (outcome == "split")
        {
            limit = (std::max)(static_cast<std::size_t>(1), batch->events.size() / 2);
            continue;
        }
        if (outcome != "done")
        {
            result.stoppedBy = outcome;
            return result;
        }
        limit = eventsBatchCount;
    }
    return result;
}

std::string EventQueue::sendBatch(const std::string& owner, const FrozenBatch& batch, FlushResult& result)
{
    auto events = Json::array();
    for (const auto& e : batch.events) events.push(e.toJson());
    const auto text = Json::object({{"batchId", batch.batchId},
                                    {"platform", batch.platform},
                                    {"build", batch.build},
                                    {"sentAt", isoNow()},
                                    {"launchId", clock_.launchId()},
                                    {"monotonicMs", clock_.monotonicMs()},
                                    {"events", events}})
                          .dump();
    auto body = gzip(text);
    if (body.size() > eventsSentLimit && batch.events.size() > 1)
    {
        unfreeze(owner, batch);
        return "split";
    }
    Answer answer;
    try
    {
        PlayerCall call;
        call.method = "POST";
        call.body = std::move(body);
        call.headers.set("content-type", "application/json");
        call.headers.set("content-encoding", "gzip");
        call.owner = owner;
        answer = api_.player("/v1/events", call);
    }
    catch (const Error& err)
    {
        return failed(owner, batch, err);
    }
    backoffMs_.store(0);
    const auto& json = answer.body();
    std::set<std::string> retryIds;
    for (const auto& r : json["retry"].items()) retryIds.insert(r.getString("id").value_or(""));
    std::vector<std::pair<std::string, std::string>> rejected;
    for (const auto& r : json["rejected"].items()) rejected.emplace_back(r.getString("id").value_or(""), r.getString("code").value_or(""));
    {
        auto lock = NamedLocks::take(key(owner));
        auto state = load(owner);
        if (state.batch && state.batch->batchId == batch.batchId)
        {
            std::vector<WireEvent> again;
            for (const auto& e : batch.events)
            {
                if (retryIds.count(e.id) > 0) again.push_back(e);
            }
            state.pending.insert(state.pending.begin(), again.begin(), again.end());
            state.batch.reset();
            save(owner, state);
        }
    }
    result.sent += static_cast<int>(batch.events.size()) - static_cast<int>(retryIds.size()) - static_cast<int>(rejected.size());
    result.rejected.insert(result.rejected.end(), rejected.begin(), rejected.end());
    if (!retryIds.empty())
    {
        // `retry` is for now (the app's events of the day, a build cap): wait before the next batch.
        notBefore_.store(nowMs() + 60000);
        return "retry";
    }
    return "done";
}

void EventQueue::unfreeze(const std::string& owner, const FrozenBatch& batch)
{
    // A batch never taken by the server goes back to the front of the queue (only before any answer said otherwise).
    auto lock = NamedLocks::take(key(owner));
    auto state = load(owner);
    if (!state.batch || state.batch->batchId != batch.batchId) return;
    state.pending.insert(state.pending.begin(), batch.events.begin(), batch.events.end());
    state.batch.reset();
    save(owner, state);
}

std::string EventQueue::failed(const std::string& owner, const FrozenBatch& batch, const Error& err)
{
    if (err.code() == "payload_too_large" && batch.events.size() > 1)
    {
        unfreeze(owner, batch);
        return "split";
    }
    if (err.code() == "batch_id_reused")
    {
        // The id was spent on other events (a store restored from another device): same events, new id.
        unfreeze(owner, batch);
        return "done";
    }
    // An error that came before the call (a refused refresh) or a refusal that can lift says nothing against the
    // batch: it stays frozen and goes again later.
    // An app that does not ship on this platform yet keeps the batch too: the events arrive once the platform is added.
    const bool notShippedYet = err.code() == "platform_not_registered";
    if ((err.status() == 400 || err.status() == 413) && !err.waitsForLater() && !notShippedYet)
    {
        // The batch itself can never be taken: drop it rather than block every later event.
        auto lock = NamedLocks::take(key(owner));
        auto state = load(owner);
        if (state.batch && state.batch->batchId == batch.batchId)
        {
            state.dropped += static_cast<std::int64_t>(batch.events.size());
            state.batch.reset();
            save(owner, state);
        }
        return err.code();
    }
    const bool longWait = notShippedYet && !err.retryAfter();
    notBefore_.store(nowMs() + (longWait ? queueLongWaitMs : queueWaitMs(err, backoffMs_)));
    return err.code();
}

void EventQueue::resetBackoff()
{
    notBefore_.store(0);
    backoffMs_.store(0);
}

void EventQueue::drop(const std::string& owner)
{
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        buffer_.erase(std::remove_if(buffer_.begin(), buffer_.end(), [&owner](const auto& entry) { return entry.first == owner; }), buffer_.end());
    }
    auto lock = NamedLocks::take(key(owner));
    store_.remove(key(owner));
}
}  // namespace krtu::detail
