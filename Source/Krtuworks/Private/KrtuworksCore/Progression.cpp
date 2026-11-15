#include "Progression.h"

#include <algorithm>
#include <cmath>

namespace krtu::detail
{
namespace
{
std::string progressionRefOf(const LevelRef& level)
{
    return level.world.value_or("") + std::string(1, '\0') + level.level + std::string(1, '\0') + level.part.value_or("");
}

Json progressionPlace(const LevelRef& level, const std::string& action)
{
    auto place = Json::object({{"level", level.level}});
    if (level.world && !level.world->empty()) place["world"] = *level.world;
    if (level.part && !level.part->empty()) place["part"] = *level.part;
    place["action"] = action;
    return place;
}
}  // namespace

ProgressionTracker::ProgressionTracker(KeyValueStore& store, std::string clientId, std::function<std::string()> owner, std::function<std::int64_t()> activeSeconds, std::function<void(const Json&)> emit)
    : store_(store), clientId_(std::move(clientId)), owner_(std::move(owner)), activeSeconds_(std::move(activeSeconds)), emit_(std::move(emit))
{
}

std::string ProgressionTracker::key() const
{
    return "krtu:" + clientId_ + ":levels:" + owner_();
}

Json ProgressionTracker::times(const Attempt& attempt, Json into) const
{
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - attempt.since).count();
    into["attempt"] = attempt.number;
    into["seconds"] = std::round(seconds * 10) / 10;
    into["activeSeconds"] = (std::max)(static_cast<std::int64_t>(0), activeSeconds_() - attempt.activeAtStart);
    into["deaths"] = attempt.deaths;
    return into;
}

void ProgressionTracker::start(const LevelRef& level)
{
    finish("quit", std::nullopt);
    auto counts = getJson(store_, key()).value_or(Json::object());
    if (!counts.isObject()) counts = Json::object();
    const auto reference = progressionRefOf(level);
    const auto number = counts.getInt(reference).value_or(0) + 1;
    counts[reference] = number;
    putJson(store_, key(), counts);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = Attempt{level, number, std::chrono::steady_clock::now(), activeSeconds_(), 0};
    }
    auto data = progressionPlace(level, "start");
    data["attempt"] = number;
    emit_(data);
}

void ProgressionTracker::finish(const std::string& action, const std::optional<double>& score)
{
    std::optional<Attempt> attempt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        attempt.swap(current_);
    }
    if (!attempt) return;
    auto data = times(*attempt, progressionPlace(attempt->level, action));
    if (score) data["score"] = *score;
    emit_(data);
}

void ProgressionTracker::checkpoint(const std::string& name)
{
    std::optional<Attempt> attempt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        attempt = current_;
    }
    if (!attempt) return;
    auto data = times(*attempt, progressionPlace(attempt->level, "checkpoint"));
    data["checkpoint"] = name;
    emit_(data);
}

void ProgressionTracker::restart()
{
    std::optional<Attempt> attempt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        attempt.swap(current_);
    }
    if (!attempt) return;
    emit_(times(*attempt, progressionPlace(attempt->level, "restart")));
    start(attempt->level);
}

std::optional<LevelRef> ProgressionTracker::noteDeath()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_) return std::nullopt;
    ++current_->deaths;
    return current_->level;
}

std::optional<LevelRef> ProgressionTracker::inPlay() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_) return std::nullopt;
    return current_->level;
}
}  // namespace krtu::detail
