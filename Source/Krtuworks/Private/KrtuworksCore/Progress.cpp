#include "Progress.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

#include "KrtuworksCore/Crypto.h"

namespace krtu::detail
{
namespace
{
constexpr double progressMaxValue = 1e15;
constexpr std::size_t progressMaxOps = 100;
constexpr std::size_t progressMaxDetails = 64;

/// Refusals that say the stat or achievement itself is gone: what waits for it goes too.
bool progressKeyGone(const std::string& code)
{
    return code == "unknown_stat" || code == "stat_retired" || code == "unknown_achievement" || code == "achievement_retired";
}

/// Refusals that say the leaderboard itself is gone: what waits for it goes too.
bool progressBoardGone(const std::string& code)
{
    return code == "unknown_leaderboard" || code == "leaderboard_retired";
}

Json progressOpsJson(const std::vector<FrozenOp>& ops)
{
    auto list = Json::array();
    for (const auto& op : ops) list.push(op.toJson());
    return list;
}

std::vector<FrozenOp> progressOpsFrom(const Json& json)
{
    std::vector<FrozenOp> ops;
    for (const auto& item : json.items())
    {
        FrozenOp op;
        op.opId = item.getString("opId").value_or("");
        op.type = item.getString("type").value_or("");
        op.key = item.getString("key").value_or("");
        op.value = item.getNumber("value");
        op.details = item.getString("details");
        op.fromPlatform = item.getBool("fromPlatform").value_or(false);
        if (!op.opId.empty()) ops.push_back(std::move(op));
    }
    return ops;
}
}  // namespace

Json FrozenOp::toJson() const
{
    auto json = Json::object({{"opId", opId}, {"type", type}, {"key", key}});
    if (value) json["value"] = *value;
    if (details) json["details"] = *details;
    if (fromPlatform) json["fromPlatform"] = true;
    return json;
}

Json ProgressState::toJson() const
{
    auto statsJson = Json::object();
    for (const auto& s : stats)
    {
        auto pendingJson = Json::array();
        for (const auto& c : s.second.pending) pendingJson.push(Json::object({{"op", c.op}, {"value", c.value}}));
        statsJson[s.first] = Json::object({{"inFlight", progressOpsJson(s.second.inFlight)}, {"pending", pendingJson}});
    }
    auto unlocksJson = Json::object();
    for (const auto& u : unlocks) unlocksJson[u.first] = Json::object({{"inFlight", progressOpsJson(u.second.inFlight)}, {"pending", u.second.pending}});
    auto scoresJson = Json::object();
    for (const auto& b : scores)
    {
        auto pendingJson = Json::array();
        for (const auto& c : b.second.pending)
        {
            auto item = Json::object({{"value", c.value}});
            if (c.details) item["details"] = *c.details;
            if (c.fromPlatform) item["fromPlatform"] = true;
            pendingJson.push(item);
        }
        scoresJson[b.first] = Json::object({{"inFlight", progressOpsJson(b.second.inFlight)}, {"pending", pendingJson}});
    }
    return Json::object({{"stats", statsJson}, {"unlocks", unlocksJson}, {"scores", scoresJson}});
}

ProgressState ProgressState::fromJson(const Json& json)
{
    ProgressState state;
    for (const auto& s : json["stats"].members())
    {
        StatEntry entry;
        entry.inFlight = progressOpsFrom(s.second["inFlight"]);
        for (const auto& c : s.second["pending"].items()) entry.pending.push_back(Contribution{c.getString("op").value_or("add"), c.getNumber("value").value_or(0)});
        state.stats[s.first] = std::move(entry);
    }
    for (const auto& u : json["unlocks"].members())
    {
        UnlockEntry entry;
        entry.inFlight = progressOpsFrom(u.second["inFlight"]);
        entry.pending = u.second.getBool("pending").value_or(false);
        state.unlocks[u.first] = std::move(entry);
    }
    for (const auto& b : json["scores"].members())
    {
        ScoreEntry entry;
        entry.inFlight = progressOpsFrom(b.second["inFlight"]);
        for (const auto& c : b.second["pending"].items())
            entry.pending.push_back(ScoreContribution{c.getNumber("value").value_or(0), c.getString("details"), c.getBool("fromPlatform").value_or(false)});
        state.scores[b.first] = std::move(entry);
    }
    return state;
}

ProgressQueue::ProgressQueue(const Resolved& opts, KeyValueStore& store, Api& api, std::function<std::optional<std::string>()> subject)
    : opts_(opts), store_(store), api_(api), subject_(std::move(subject))
{
}

std::string ProgressQueue::key(const std::string& owner) const
{
    return "krtu:" + opts_.clientId + ":progress:" + owner;
}

std::string ProgressQueue::kindsKey() const
{
    return "krtu:" + opts_.clientId + ":statKinds";
}

std::string ProgressQueue::boardsKey() const
{
    return "krtu:" + opts_.clientId + ":boardRules";
}

std::vector<Contribution> ProgressQueue::fold(const std::vector<Contribution>& pending, const std::optional<std::string>& kind)
{
    std::vector<Contribution> output;
    for (const auto& c : pending)
    {
        if (output.empty())
        {
            output.push_back(c);
            continue;
        }
        auto& last = output.back();
        if (kind == "SUM" || (!kind && c.op == "add" && last.op == "add"))
        {
            const double sum = last.value + c.value;
            if (std::fabs(sum) < progressMaxValue)
            {
                last.value = sum;
                last.op = "add";
                continue;
            }
        }
        else if (kind == "MAX")
        {
            last.value = (std::max)(last.value, c.value);
            continue;
        }
        else if (kind == "MIN")
        {
            last.value = (std::min)(last.value, c.value);
            continue;
        }
        else if (kind == "LATEST")
        {
            last.value = c.value;
            last.op = c.op;
            continue;
        }
        output.push_back(c);
    }
    return output;
}

std::vector<ScoreContribution> ProgressQueue::foldScores(const std::vector<ScoreContribution>& pending, const std::optional<BoardRule>& rule)
{
    if (!rule) return pending;
    std::vector<ScoreContribution> output;
    for (const auto& c : pending)
    {
        if (output.empty() || c.fromPlatform || output.back().fromPlatform)
        {
            output.push_back(c);
            continue;
        }
        auto& last = output.back();
        if (rule->uploadMethod == "FORCE_UPDATE")
            last = c;
        else if (rule->sortOrder == "DESC" ? c.value > last.value : c.value < last.value)
            last = c;
    }
    return output;
}

std::optional<BoardRule> ProgressQueue::ruleOf(const std::string& boardKey) const
{
    std::lock_guard<std::mutex> lock(rulesMutex_);
    const auto found = boards_.find(boardKey);
    if (found == boards_.end()) return std::nullopt;
    return found->second;
}

std::optional<std::string> ProgressQueue::kindOf(const std::string& statKey) const
{
    std::lock_guard<std::mutex> lock(rulesMutex_);
    const auto found = kinds_.find(statKey);
    if (found == kinds_.end()) return std::nullopt;
    return found->second;
}

ProgressState ProgressQueue::load(const std::string& owner)
{
    const auto json = getJson(store_, key(owner));
    return json ? ProgressState::fromJson(*json) : ProgressState();
}

void ProgressQueue::save(const std::string& owner, ProgressState& state)
{
    for (auto it = state.stats.begin(); it != state.stats.end();)
    {
        if (it->second.inFlight.empty() && it->second.pending.empty()) it = state.stats.erase(it);
        else ++it;
    }
    for (auto it = state.unlocks.begin(); it != state.unlocks.end();)
    {
        if (it->second.inFlight.empty() && !it->second.pending) it = state.unlocks.erase(it);
        else ++it;
    }
    for (auto it = state.scores.begin(); it != state.scores.end();)
    {
        if (it->second.inFlight.empty() && it->second.pending.empty()) it = state.scores.erase(it);
        else ++it;
    }
    if (!state.stats.empty() || !state.unlocks.empty() || !state.scores.empty()) putJson(store_, key(owner), state.toJson());
    else store_.remove(key(owner));
}

void ProgressQueue::loadKinds()
{
    {
        std::lock_guard<std::mutex> lock(rulesMutex_);
        if (kindsLoaded_) return;
    }
    const auto json = getJson(store_, kindsKey());
    std::lock_guard<std::mutex> lock(rulesMutex_);
    if (kindsLoaded_) return;
    if (json)
    {
        for (const auto& member : json->members())
        {
            if (member.second.isString()) kinds_[member.first] = member.second.asString();
        }
    }
    kindsLoaded_ = true;
}

void ProgressQueue::loadBoards()
{
    {
        std::lock_guard<std::mutex> lock(rulesMutex_);
        if (boardsLoaded_) return;
    }
    const auto json = getJson(store_, boardsKey());
    std::lock_guard<std::mutex> lock(rulesMutex_);
    if (boardsLoaded_) return;
    if (json)
    {
        for (const auto& member : json->members())
        {
            const auto sortOrder = member.second.getString("sortOrder");
            const auto uploadMethod = member.second.getString("uploadMethod");
            if (sortOrder && uploadMethod) boards_[member.first] = BoardRule{*sortOrder, *uploadMethod};
        }
    }
    boardsLoaded_ = true;
}

void ProgressQueue::learnBoards(const std::vector<std::pair<std::string, BoardRule>>& boards)
{
    loadBoards();
    auto json = Json::object();
    {
        std::lock_guard<std::mutex> lock(rulesMutex_);
        for (const auto& board : boards) boards_[board.first] = board.second;
        for (const auto& board : boards_) json[board.first] = Json::object({{"sortOrder", board.second.sortOrder}, {"uploadMethod", board.second.uploadMethod}});
    }
    putJson(store_, boardsKey(), json);
}

void ProgressQueue::learnKinds(const std::vector<std::pair<std::string, std::string>>& stats)
{
    loadKinds();
    auto json = Json::object();
    {
        std::lock_guard<std::mutex> lock(rulesMutex_);
        for (const auto& stat : stats) kinds_[stat.first] = stat.second;
        for (const auto& kind : kinds_) json[kind.first] = kind.second;
    }
    putJson(store_, kindsKey(), json);
}

void ProgressQueue::report(const std::string& type, const std::string& statKey, double value)
{
    if (!isStatKey(statKey)) throw std::invalid_argument("\"" + statKey + "\" is not a stat key (lowercase letters, digits, _ and ., at most 64)");
    if (!std::isfinite(value) || std::fabs(value) >= progressMaxValue) throw std::invalid_argument("a stat value is a finite number below 10^15");
    loadKinds();
    const auto owner = subject_().value_or(anonOwner);
    auto lock = NamedLocks::take(key(owner));
    auto state = load(owner);
    auto& entry = state.stats[statKey];
    entry.pending.push_back(Contribution{type, value});
    entry.pending = fold(entry.pending, kindOf(statKey));
    save(owner, state);
}

void ProgressQueue::unlock(const std::string& achievementKey)
{
    if (!isStatKey(achievementKey)) throw std::invalid_argument("\"" + achievementKey + "\" is not an achievement key");
    const auto owner = subject_().value_or(anonOwner);
    auto lock = NamedLocks::take(key(owner));
    auto state = load(owner);
    auto& entry = state.unlocks[achievementKey];
    if (entry.inFlight.empty()) entry.pending = true;
    save(owner, state);
}

void ProgressQueue::queueScore(const std::string& boardKey, ScoreContribution contribution)
{
    loadBoards();
    const auto owner = subject_().value_or(anonOwner);
    auto lock = NamedLocks::take(key(owner));
    auto state = load(owner);
    auto& entry = state.scores[boardKey];
    entry.pending.push_back(std::move(contribution));
    entry.pending = foldScores(entry.pending, ruleOf(boardKey));
    save(owner, state);
}

void ProgressQueue::submitScore(const std::string& boardKey, std::int64_t value, const std::optional<std::string>& details)
{
    if (!isStatKey(boardKey)) throw std::invalid_argument("\"" + boardKey + "\" is not a leaderboard key (lowercase letters, digits, _ and ., at most 64)");
    if (value <= -static_cast<std::int64_t>(progressMaxValue) || value >= static_cast<std::int64_t>(progressMaxValue))
        throw std::invalid_argument("a score is a whole number below 10^15");
    if (details && (details->empty() || details->size() > progressMaxDetails)) throw std::invalid_argument("score details are 1 to 64 bytes");
    queueScore(boardKey, ScoreContribution{static_cast<double>(value), details ? std::optional<std::string>(base64(*details)) : std::nullopt, false});
}

void ProgressQueue::importScore(const std::string& boardKey, std::int64_t value)
{
    queueScore(boardKey, ScoreContribution{static_cast<double>(value), std::nullopt, true});
}

int ProgressQueue::waiting(const std::string& owner)
{
    const auto state = load(owner);
    std::size_t count = 0;
    for (const auto& s : state.stats) count += s.second.inFlight.size() + s.second.pending.size();
    for (const auto& u : state.unlocks) count += u.second.inFlight.size() + (u.second.pending ? 1 : 0);
    for (const auto& b : state.scores) count += b.second.inFlight.size() + b.second.pending.size();
    return static_cast<int>(count);
}

void ProgressQueue::move(const std::string& from, const std::string& to)
{
    if (from == to) return;
    loadKinds();
    loadBoards();
    const auto& first = from < to ? from : to;
    const auto& second = from < to ? to : from;
    auto firstLock = NamedLocks::take(key(first));
    auto secondLock = NamedLocks::take(key(second));
    auto source = load(from);
    if (freezeAll(source)) save(from, source);
    auto target = load(to);
    const auto add = [](std::vector<FrozenOp>& into, const std::vector<FrozenOp>& ops) {
        for (const auto& op : ops)
        {
            const auto held = std::any_of(into.begin(), into.end(), [&op](const FrozenOp& h) { return h.opId == op.opId; });
            if (!held) into.push_back(op);
        }
    };
    for (const auto& s : source.stats) add(target.stats[s.first].inFlight, s.second.inFlight);
    for (const auto& u : source.unlocks)
    {
        auto& t = target.unlocks[u.first];
        add(t.inFlight, u.second.inFlight);
        if (!t.inFlight.empty()) t.pending = false;
    }
    for (const auto& b : source.scores) add(target.scores[b.first].inFlight, b.second.inFlight);
    save(to, target);
    store_.remove(key(from));
}

bool ProgressQueue::freezeAll(ProgressState& state) const
{
    bool frozen = false;
    for (auto& s : state.stats)
    {
        for (const auto& c : fold(s.second.pending, kindOf(s.first))) s.second.inFlight.push_back(FrozenOp{uuidV7(), "stat", s.first, c.value, std::nullopt, false});
        frozen = frozen || !s.second.pending.empty();
        s.second.pending.clear();
    }
    for (auto& u : state.unlocks)
    {
        if (u.second.pending && u.second.inFlight.empty()) u.second.inFlight.push_back(FrozenOp{uuidV7(), "unlock", u.first, std::nullopt, std::nullopt, false});
        frozen = frozen || u.second.pending;
        u.second.pending = false;
    }
    for (auto& b : state.scores)
    {
        for (const auto& c : foldScores(b.second.pending, ruleOf(b.first))) b.second.inFlight.push_back(FrozenOp{uuidV7(), "score", b.first, c.value, c.details, c.fromPlatform});
        frozen = frozen || !b.second.pending.empty();
        b.second.pending.clear();
    }
    return frozen;
}

std::vector<FrozenOp> ProgressQueue::freeze(ProgressState& state) const
{
    std::vector<FrozenOp> ops;
    for (auto& s : state.stats)
    {
        if (s.second.inFlight.empty() && !s.second.pending.empty())
        {
            s.second.pending = fold(s.second.pending, kindOf(s.first));
            const auto next = s.second.pending.front();
            s.second.pending.erase(s.second.pending.begin());
            s.second.inFlight.push_back(FrozenOp{uuidV7(), "stat", s.first, next.value, std::nullopt, false});
        }
        ops.insert(ops.end(), s.second.inFlight.begin(), s.second.inFlight.end());
    }
    for (auto& u : state.unlocks)
    {
        if (u.second.inFlight.empty() && u.second.pending)
        {
            u.second.inFlight.push_back(FrozenOp{uuidV7(), "unlock", u.first, std::nullopt, std::nullopt, false});
            u.second.pending = false;
        }
        ops.insert(ops.end(), u.second.inFlight.begin(), u.second.inFlight.end());
    }
    for (auto& b : state.scores)
    {
        if (b.second.inFlight.empty() && !b.second.pending.empty())
        {
            b.second.pending = foldScores(b.second.pending, ruleOf(b.first));
            const auto next = b.second.pending.front();
            b.second.pending.erase(b.second.pending.begin());
            FrozenOp op{uuidV7(), "score", b.first, next.value, next.details, next.fromPlatform};
            b.second.inFlight.push_back(std::move(op));
        }
        ops.insert(ops.end(), b.second.inFlight.begin(), b.second.inFlight.end());
    }
    if (ops.size() > progressMaxOps) ops.resize(progressMaxOps);
    return ops;
}

ProgressFlushResult ProgressQueue::flush()
{
    ProgressFlushResult output;
    const auto owner = subject_();
    if (!owner)
    {
        output.stoppedBy = "signed_out";
        return output;
    }
    if (nowMs() < notBefore_.load())
    {
        output.stoppedBy = "waiting";
        return output;
    }
    loadKinds();
    loadBoards();
    for (int round = 0; round < 50; ++round)
    {
        std::vector<FrozenOp> ops;
        {
            auto lock = NamedLocks::take(key(*owner));
            auto state = load(*owner);
            ops = freeze(state);
            save(*owner, state);
        }
        if (ops.empty()) return output;
        Answer answer;
        try
        {
            PlayerCall call;
            call.method = "POST";
            call.json = Json::object({{"platform", opts_.platform}, {"build", opts_.build}, {"ops", progressOpsJson(ops)}});
            call.owner = *owner;
            answer = api_.player("/v1/me/progress", call);
        }
        catch (const Error& err)
        {
            const auto backoff = (std::min)(static_cast<std::int64_t>(300000), (std::max)(static_cast<std::int64_t>(5000), backoffMs_.load() * 2));
            backoffMs_.store(backoff);
            notBefore_.store(nowMs() + (err.retryAfter() ? static_cast<std::int64_t>(*err.retryAfter() * 1000) : backoff));
            output.stoppedBy = err.code();
            return output;
        }
        backoffMs_.store(0);
        int settled = 0;
        {
            auto lock = NamedLocks::take(key(*owner));
            auto state = load(*owner);
            for (const auto& r : answer.body()["results"].items())
            {
                // Results are matched by opId alone: the server's refusals carry the key but no type, and op_id_reused neither.
                const auto opId = r.getString("opId");
                if (!opId) continue;
                const auto holds = [&opId](const std::vector<FrozenOp>& list) {
                    return std::any_of(list.begin(), list.end(), [&opId](const FrozenOp& op) { return op.opId == *opId; });
                };
                std::string type;
                std::string opKey;
                std::vector<FrozenOp>* inFlight = nullptr;
                for (auto& s : state.stats)
                {
                    if (inFlight == nullptr && holds(s.second.inFlight)) std::tie(type, opKey, inFlight) = std::make_tuple(std::string("stat"), s.first, &s.second.inFlight);
                }
                for (auto& u : state.unlocks)
                {
                    if (inFlight == nullptr && holds(u.second.inFlight)) std::tie(type, opKey, inFlight) = std::make_tuple(std::string("unlock"), u.first, &u.second.inFlight);
                }
                for (auto& b : state.scores)
                {
                    if (inFlight == nullptr && holds(b.second.inFlight)) std::tie(type, opKey, inFlight) = std::make_tuple(std::string("score"), b.first, &b.second.inFlight);
                }
                if (inFlight == nullptr) continue;
                // The leaderboards module is off: the score is not refused for good, it waits for the module.
                if (type == "score" && r.getString("status") == std::optional<std::string>("rejected") && r.getString("code") == std::optional<std::string>("module_disabled")) continue;
                inFlight->erase(std::remove_if(inFlight->begin(), inFlight->end(), [&opId](const FrozenOp& op) { return op.opId == *opId; }), inFlight->end());
                ++settled;
                if (r.getString("status") == std::optional<std::string>("rejected"))
                {
                    const auto code = r.getString("code").value_or("rejected");
                    {
                        std::lock_guard<std::mutex> refusalLock(refusalsMutex_);
                        refusals_.push_back(ProgressRefusal{type, opKey, code});
                    }
                    if (type == "score" ? progressBoardGone(code) : progressKeyGone(code))
                    {
                        if (type == "unlock") state.unlocks.erase(opKey);
                        else if (type == "score") state.scores.erase(opKey);
                        else state.stats.erase(opKey);
                    }
                }
                else
                {
                    ++output.applied;
                    const auto value = r.getNumber("value");
                    if (type == "stat" && value) output.values[opKey] = *value;
                }
            }
            save(*owner, state);
        }
        if (settled == 0)
        {
            // An answer that settles none of the ops sent: keep them frozen and try again later.
            output.stoppedBy = "unanswered";
            return output;
        }
    }
    return output;
}

void ProgressQueue::resetBackoff()
{
    notBefore_.store(0);
    backoffMs_.store(0);
}

std::vector<ProgressRefusal> ProgressQueue::refusals() const
{
    std::lock_guard<std::mutex> lock(refusalsMutex_);
    return refusals_;
}
}  // namespace krtu::detail
