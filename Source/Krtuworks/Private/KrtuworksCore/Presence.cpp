#include "Presence.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace krtu::detail
{
namespace
{
constexpr std::size_t presenceValueBytes = 256;
constexpr const char* presenceDisplayKey = "steam_display";
constexpr const char* presenceGroupKey = "steam_player_group";
constexpr const char* presenceGroupSizeKey = "steam_player_group_size";

/// At most `limit` bytes of `text`, cut where a UTF-8 character starts (never inside one).
std::string presenceCut(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit) return text;
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}
}  // namespace

Presence::Presence(const Resolved& opts, Api& api, KeyValueStore& store, PlatformPresence* platform, std::function<std::optional<std::string>()> subject, std::function<bool()> moduleOn,
                   std::function<std::optional<std::string>()> level, std::function<std::int64_t()> activeSeconds, std::function<void(const std::string&)> log)
    : opts_(opts),
      api_(api),
      store_(store),
      platform_(platform),
      subject_(std::move(subject)),
      moduleOn_(std::move(moduleOn)),
      level_(std::move(level)),
      activeSeconds_(std::move(activeSeconds)),
      log_(std::move(log))
{
}

std::string Presence::cacheKey(const std::string& owner) const
{
    return "krtu:" + opts_.clientId + ":presence:" + owner;
}

std::vector<Presence::State> Presence::statesFrom(const Json& list)
{
    std::vector<State> states;
    for (const auto& item : list.items())
    {
        State state{item.getString("key").value_or(""), item.getString("token").value_or(""), {}};
        for (const auto& param : item["params"].items())
        {
            if (param.isString()) state.params.push_back(param.asString());
        }
        if (!state.key.empty() && !state.token.empty()) states.push_back(std::move(state));
    }
    return states;
}

Presence::Cache* Presence::cacheFor(const std::string& owner)
{
    if (cache_ && cache_->owner == owner) return &*cache_;
    cache_.reset();
    const auto kept = getJson(store_, cacheKey(owner));
    if (!kept || !kept->isObject()) return nullptr;
    Cache cache;
    cache.owner = owner;
    cache.etag = kept->getString("etag");
    cache.visible = kept->getBool("visible") != std::optional<bool>(false);
    cache.states = statesFrom((*kept)["states"]);
    cache_ = std::move(cache);
    return &*cache_;
}

void Presence::saveCache(const Cache& cache)
{
    auto states = Json::array();
    for (const auto& state : cache.states)
    {
        auto params = Json::array();
        for (const auto& param : state.params) params.push(param);
        states.push(Json::object({{"key", state.key}, {"token", state.token}, {"params", params}}));
    }
    auto kept = Json::object({{"states", states}, {"visible", cache.visible}});
    if (cache.etag) kept["etag"] = *cache.etag;
    putJson(store_, cacheKey(cache.owner), kept);
}

void Presence::clearPlatform()
{
    if (platform_ != nullptr) platform_->clearRichPresence();
}

PresenceResult Presence::rejected()
{
    clearPlatform();
    wanted_.reset();
    return PresenceResult{false, "platform_rejected"};
}

void Presence::forget()
{
    clearPlatform();
    wanted_.reset();
    group_.reset();
    groupSize_.reset();
}

PresenceResult Presence::apply(const std::string& owner, const std::string& key, const std::map<std::string, std::string>& params, bool announce)
{
    if (!moduleOn_())
    {
        clearPlatform();
        wanted_.reset();
        return PresenceResult{false, "module_disabled"};
    }
    const Cache* cache = cacheFor(owner);
    if (cache == nullptr) return PresenceResult{false, "states_unknown"};
    if (!cache->visible)
    {
        // The player's switch is off: nothing shows, and the state waits for the switch to come on.
        clearPlatform();
        wanted_ = Wanted{owner, key, params, false, false};
        return PresenceResult{false, "hidden"};
    }
    const auto state = std::find_if(cache->states.begin(), cache->states.end(), [&key](const State& candidate) { return candidate.key == key; });
    if (state == cache->states.end()) return PresenceResult{false, "unknown_state"};

    std::vector<std::pair<std::string, std::string>> values;
    bool autoLevel = false;
    bool autoMinutes = false;
    for (const auto& name : state->params)
    {
        const auto given = params.find(name);
        std::string value = given != params.end() ? presenceCut(given->second, presenceValueBytes) : std::string();
        if (value.empty() && name == "LEVEL")
        {
            if (const auto level = level_()) value = presenceCut(*level, presenceValueBytes);
            autoLevel = !value.empty();
        }
        else if (value.empty() && name == "SESSION_MINUTES")
        {
            value = std::to_string(activeSeconds_() / 60);
            autoMinutes = true;
        }
        if (value.empty()) return PresenceResult{false, "param_missing"};
        values.emplace_back(name, std::move(value));
    }
    if (announce)
    {
        for (const auto& given : params)
        {
            if (std::find(state->params.begin(), state->params.end(), given.first) == state->params.end())
                log_("presence: \"" + given.first + "\" is not a param of the state \"" + key + "\" and was dropped");
        }
    }

    // Order: clear, the params, the group, then the display token last (Steam shows the line once it is there).
    clearPlatform();
    for (const auto& value : values)
    {
        if (!platform_->setRichPresence(value.first, value.second)) return rejected();
    }
    if (group_)
    {
        if (!platform_->setRichPresence(presenceGroupKey, presenceCut(*group_, presenceValueBytes))) return rejected();
        if (groupSize_ && !platform_->setRichPresence(presenceGroupSizeKey, std::to_string(*groupSize_))) return rejected();
    }
    if (!platform_->setRichPresence(presenceDisplayKey, state->token)) return rejected();
    wanted_ = Wanted{owner, key, params, autoLevel, autoMinutes};
    return PresenceResult{true, ""};
}

PresenceResult Presence::set(const std::string& key, const std::map<std::string, std::string>& params)
{
    if (!opts_.game()) return PresenceResult{false, "not_game"};
    if (platform_ == nullptr) return PresenceResult{false, "no_platform"};
    const auto owner = subject_();
    if (!owner) return PresenceResult{false, "states_unknown"};
    std::lock_guard<std::mutex> lock(mutex_);
    if (wanted_ && wanted_->owner != *owner) forget();
    return apply(*owner, key, params, true);
}

void Presence::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    clearPlatform();
    wanted_.reset();
}

void Presence::setGroup(const std::optional<std::string>& groupId, const std::optional<int>& size)
{
    if (groupId && groupId->empty()) throw std::invalid_argument("a group id is not empty (leave it out to leave the group)");
    if (size && !groupId) throw std::invalid_argument("a group size needs a group id");
    if (size && *size < 1) throw std::invalid_argument("a group size is 1 or more");
    std::lock_guard<std::mutex> lock(mutex_);
    group_ = groupId;
    groupSize_ = groupId ? size : std::nullopt;
    if (!wanted_ || platform_ == nullptr) return;
    const Wanted again = *wanted_;
    apply(again.owner, again.key, again.params, false);
}

std::optional<bool> Presence::visible()
{
    const auto owner = subject_();
    if (!owner || !opts_.game()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    const Cache* cache = cacheFor(*owner);
    return cache != nullptr ? std::optional<bool>(cache->visible) : std::nullopt;
}

void Presence::setVisible(bool visible)
{
    PlayerCall call;
    call.method = "PUT";
    call.json = Json::object({{"visible", visible}});
    const auto owner = subject_();
    const auto answer = api_.player("/v1/me/presence-visibility", call);
    if (!owner || subject_() != owner) return;
    const bool now = answer.body().getBool("visible").value_or(visible);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (Cache* cache = cacheFor(*owner))
        {
            cache->visible = now;
            saveCache(*cache);
            settle();
            return;
        }
    }
    // No states heard yet: asking now brings the states along with the switch.
    refresh();
}

void Presence::settle()
{
    if (platform_ == nullptr || !wanted_) return;
    const Wanted again = *wanted_;
    const auto result = apply(again.owner, again.key, again.params, false);
    if (result.reason == "unknown_state")
    {
        // The state was retired on the server: what it showed goes.
        clearPlatform();
        wanted_.reset();
    }
}

void Presence::refresh()
{
    if (!opts_.game()) return;
    const auto owner = subject_();
    if (!owner) return;
    PlayerCall call;
    call.accept = {304};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!moduleOn_())
        {
            clearPlatform();
            wanted_.reset();
            return;
        }
        const Cache* cache = cacheFor(*owner);
        if (cache != nullptr && cache->etag) call.headers.set("if-none-match", *cache->etag);
    }
    Answer answer;
    try
    {
        answer = api_.player("/v1/me/presence-states", call);
    }
    catch (const Error& err)
    {
        if (err.code() != "module_disabled") throw;
        std::lock_guard<std::mutex> lock(mutex_);
        clearPlatform();
        wanted_.reset();
        return;
    }
    if (answer.status == 304 || subject_() != owner) return;
    Cache fresh;
    fresh.owner = *owner;
    fresh.etag = answer.raw.header("etag");
    fresh.visible = answer.body().getBool("visible") != std::optional<bool>(false);
    fresh.states = statesFrom(answer.body()["states"]);
    std::lock_guard<std::mutex> lock(mutex_);
    saveCache(fresh);
    cache_ = std::move(fresh);
    settle();
}

void Presence::reapply(bool levelOnly)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (platform_ == nullptr || !wanted_) return;
    if (wanted_->owner != subject_())
    {
        forget();
        return;
    }
    if (levelOnly ? !wanted_->autoLevel : !(wanted_->autoLevel || wanted_->autoMinutes)) return;
    const Wanted again = *wanted_;
    apply(again.owner, again.key, again.params, false);
}

void Presence::onHeartbeat()
{
    reapply(false);
}

void Presence::onLevelStart()
{
    reapply(true);
}

void Presence::onIdentity(const std::optional<std::string>& subjectNow, bool reset)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (reset || (wanted_ && wanted_->owner != subjectNow)) forget();
}
}  // namespace krtu::detail
