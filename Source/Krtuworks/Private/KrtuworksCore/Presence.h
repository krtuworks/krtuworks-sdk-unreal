#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Api.h"

namespace krtu::detail
{
/// The player's rich presence on the platform (Steam): the app declares its states on the server (a state is a key
/// with a localization token and the params its line uses), the game says which one is true now, and this writes
/// Steam's keys. Same rules as the JS and Unity packages. Every platform write goes through one mutex, so the
/// keys on Steam are always one call's whole set.
class Presence
{
public:
    /// `platform` is the platform's rich presence; null when the game has none (set() then says no_platform).
    Presence(const Resolved& opts, Api& api, KeyValueStore& store, PlatformPresence* platform, std::function<std::optional<std::string>()> subject, std::function<bool()> moduleOn,
             std::function<std::optional<std::string>()> level, std::function<std::int64_t()> activeSeconds, std::function<void(const std::string&)> log);

    /// Makes `key` the state the player is in. Never throws for a refusal: `reason` says why nothing was written.
    PresenceResult set(const std::string& key, const std::map<std::string, std::string>& params);
    /// Clears the platform's keys and forgets the state, so a later heartbeat writes nothing.
    void clear();
    /// The group the player is in (Steam's join and party display); none removes it. Written with the state, again on every re-apply.
    void setGroup(const std::optional<std::string>& groupId, const std::optional<int>& size);
    /// The player's own switch as last heard from the server; nothing before the first answer.
    std::optional<bool> visible();
    /// The player's switch (PUT). Off clears the platform's keys at once and keeps the state the game wanted; on writes it again.
    void setVisible(bool visible);
    /// Asks the server for the states and the switch (If-None-Match) and brings the platform in line with what came.
    void refresh();
    /// A heartbeat went out: a state that uses a built-in the game did not give is written again.
    void onHeartbeat();
    /// A level started: a state that uses LEVEL the game did not give is written again.
    void onLevelStart();
    /// The player changed (`reset`: the player left; else only when the state belongs to another player): clear and forget.
    void onIdentity(const std::optional<std::string>& subjectNow, bool reset);

private:
    struct State
    {
        std::string key;
        std::string token;
        std::vector<std::string> params;
    };

    struct Cache
    {
        std::string owner;
        std::optional<std::string> etag;
        std::vector<State> states;
        bool visible = true;
    };

    /// What the game asked for, kept so the platform can be written again.
    struct Wanted
    {
        std::string owner;
        std::string key;
        std::map<std::string, std::string> params;
        bool autoLevel = false;
        bool autoMinutes = false;
    };

    /// The states of the server's list (a state without a key or a token is none).
    static std::vector<State> statesFrom(const Json& list);
    std::string cacheKey(const std::string& owner) const;
    /// The cached states and switch of `owner`, read from the store once.
    Cache* cacheFor(const std::string& owner);
    void saveCache(const Cache& cache);
    PresenceResult apply(const std::string& owner, const std::string& key, const std::map<std::string, std::string>& params, bool announce);
    PresenceResult rejected();
    void clearPlatform();
    void forget();
    /// Brings the platform in line after the states or the switch changed.
    void settle();
    void reapply(bool levelOnly);

    const Resolved& opts_;
    Api& api_;
    KeyValueStore& store_;
    PlatformPresence* platform_;
    std::function<std::optional<std::string>()> subject_;
    std::function<bool()> moduleOn_;
    std::function<std::optional<std::string>()> level_;
    std::function<std::int64_t()> activeSeconds_;
    std::function<void(const std::string&)> log_;
    std::mutex mutex_;
    std::optional<Cache> cache_;
    std::optional<Wanted> wanted_;
    std::optional<std::string> group_;
    std::optional<int> groupSize_;
};
}  // namespace krtu::detail
