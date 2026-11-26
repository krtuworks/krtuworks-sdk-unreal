#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "Infrastructure.h"

namespace krtu::detail
{
/// Levels as the progression family tells them, the rules of krtuworks-sdk-js/src/progression.ts: the
/// game says start, complete, fail, quit, checkpoint and restart; the SDK adds the attempt number (per level
/// and player, kept across launches), the attempt's wall and active seconds and its deaths. An attempt still
/// open when the app goes away is told as a quit.
class ProgressionTracker
{
public:
    ProgressionTracker(KeyValueStore& store, std::string clientId, std::function<std::string()> owner, std::function<std::int64_t()> activeSeconds, std::function<void(const Json&)> emit);

    void start(const LevelRef& level);
    /// Ends the attempt in play: complete, fail or quit.
    void finish(const std::string& action, const std::optional<double>& score);
    void checkpoint(const std::string& name);
    /// The level starts over: the attempt in play ends as a restart and a new one begins.
    void restart();
    /// A death counts toward the attempt in play; returns where it happened.
    std::optional<LevelRef> noteDeath();
    std::optional<LevelRef> inPlay() const;

private:
    struct Attempt
    {
        LevelRef level;
        /// The player the attempt belongs to; the anonymous owner until the first player exists.
        std::string owner;
        std::int64_t number = 0;
        std::chrono::steady_clock::time_point since;
        std::int64_t activeAtStart = 0;
        int deaths = 0;
    };

    std::string key() const;
    /// Under `mutex_`: the attempt in play when it is the current player's. One that belongs to another player is
    /// dropped without a word (nothing is told under the new owner); one made before any player moves to the first.
    std::optional<Attempt>& ownedLocked() const;
    Json times(const Attempt& attempt, Json into) const;

    KeyValueStore& store_;
    std::string clientId_;
    std::function<std::string()> owner_;
    std::function<std::int64_t()> activeSeconds_;
    std::function<void(const Json&)> emit_;
    mutable std::mutex mutex_;
    mutable std::optional<Attempt> current_;
};
}  // namespace krtu::detail
