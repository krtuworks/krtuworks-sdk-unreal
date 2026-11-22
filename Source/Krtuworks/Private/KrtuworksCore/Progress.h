#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Api.h"

namespace krtu::detail
{
struct FrozenOp
{
    std::string opId;
    /// `stat`, `unlock` or `score`.
    std::string type;
    std::string key;
    std::optional<double> value;
    /// Score ops only: base64 of 1 to 64 bytes the game keeps with the score.
    std::optional<std::string> details;
    /// Score ops only: the platform's own row of the player.
    bool fromPlatform = false;

    Json toJson() const;
};

/// A value the game reported and not yet frozen into an op: an increase (`add`), or a value to keep (`set`).
struct Contribution
{
    std::string op = "add";
    double value = 0;
};

/// A score the game submitted (or the platform's own row, `fromPlatform`) and not yet frozen into an op.
struct ScoreContribution
{
    double value = 0;
    /// Base64 of the details bytes.
    std::optional<std::string> details;
    bool fromPlatform = false;
};

/// What folding a leaderboard's waiting scores needs, from `GET /v1/me/leaderboards`: `sortOrder` is `ASC` or
/// `DESC`, `uploadMethod` is `KEEP_BEST` or `FORCE_UPDATE`.
struct BoardRule
{
    std::string sortOrder;
    std::string uploadMethod;
};

struct StatEntry
{
    std::vector<FrozenOp> inFlight;
    std::vector<Contribution> pending;
};

struct UnlockEntry
{
    std::vector<FrozenOp> inFlight;
    bool pending = false;
};

struct ScoreEntry
{
    std::vector<FrozenOp> inFlight;
    std::vector<ScoreContribution> pending;
};

/// Per stat, achievement or leaderboard: the ops frozen with their ids (normally one; a merge can bring a second) and what waits behind them.
struct ProgressState
{
    std::map<std::string, StatEntry> stats;
    std::map<std::string, UnlockEntry> unlocks;
    std::map<std::string, ScoreEntry> scores;

    Json toJson() const;
    static ProgressState fromJson(const Json& json);
};

/// Stats, achievements and leaderboard scores, the rules of krtuworks-sdk-js/src/progress.ts:
/// progress is never lost and never counted twice. Every report is written to the store at once; per stat at
/// most one op is in flight, frozen with its opId until it has an answer (sent again unchanged after a
/// failure), and the rest waits folded behind it. Ops have no age limit and do not depend on the statistics
/// choice.
class ProgressQueue
{
public:
    ProgressQueue(const Resolved& opts, KeyValueStore& store, Api& api, std::function<std::optional<std::string>()> subject);

    /// Folds what waits for one stat into as few values as its kind allows: SUM adds, MAX keeps
    /// the largest, MIN the smallest, LATEST the last. A stat whose kind is not known yet only folds its runs
    /// of increases; values to keep wait one by one, in order.
    static std::vector<Contribution> fold(const std::vector<Contribution>& pending, const std::optional<std::string>& kind);

    /// Folds what waits for one leaderboard into as few scores as its upload method allows: KEEP_BEST keeps the
    /// better one (DESC the larger, ASC the smaller; an equal score keeps the earlier, as the server keeps the
    /// stored one), FORCE_UPDATE the last. A board whose rule is not known folds nothing: every score waits, in
    /// order. A platform's own row (`fromPlatform`) never folds with another score: the server treats it apart.
    static std::vector<ScoreContribution> foldScores(const std::vector<ScoreContribution>& pending, const std::optional<BoardRule>& rule);

    /// Learns the stats' kinds (from `GET /v1/me/progress`): what waits folds tighter from now on.
    void learnKinds(const std::vector<std::pair<std::string, std::string>>& stats);
    /// Learns the boards' rules (from `GET /v1/me/leaderboards`): what waits folds from now on.
    void learnBoards(const std::vector<std::pair<std::string, BoardRule>>& boards);
    /// `type`: `add` (SUM increase) or `set` (MAX/MIN/LATEST value).
    void report(const std::string& type, const std::string& key, double value);
    void unlock(const std::string& key);
    /// A score for a leaderboard; `details` are 1 to 64 bytes. std::invalid_argument for a key that is none, a
    /// value of 10^15 or more, or details of another size.
    void submitScore(const std::string& key, std::int64_t value, const std::optional<std::string>& details);
    /// The platform's own row of the player for a leaderboard, to be sent once as `fromPlatform`.
    void importScore(const std::string& key, std::int64_t value);
    /// How many ops wait for `owner` (in flight or not yet frozen).
    int waiting(const std::string& owner);
    /// Moves `from`'s waiting progress to `to` (the first player after anon play, or a merge target).
    /// Everything that waits for `from` is first frozen into ops with ids and written back; only then do the
    /// ops move, and an op the target already holds is not added again. The target and the source are two
    /// writes: a move cut short between them (the app closed, the delete failed) and run again brings the
    /// same ids, so nothing is added twice. One the server already applied under the old account answers
    /// `duplicate` under the new one (the merge moved its receipt).
    void move(const std::string& from, const std::string& to);
    /// Sends the signed-in player's ops until nothing waits or a send fails. Answers per op: applied and
    /// duplicate end the op; a refusal for good ends it too (and everything waiting for a stat, achievement or
    /// leaderboard that no longer exists); `module_disabled` on a score and anything else keep it frozen for
    /// the next flush.
    ProgressFlushResult flush();
    void resetBackoff();
    std::vector<ProgressRefusal> refusals() const;

private:
    std::string key(const std::string& owner) const;
    std::string kindsKey() const;
    std::string boardsKey() const;
    ProgressState load(const std::string& owner);
    void save(const std::string& owner, ProgressState& state);
    void loadKinds();
    void loadBoards();
    void queueScore(const std::string& boardKey, ScoreContribution contribution);
    std::optional<std::string> kindOf(const std::string& statKey) const;
    std::optional<BoardRule> ruleOf(const std::string& boardKey) const;
    std::vector<FrozenOp> freeze(ProgressState& state) const;
    /// Freezes everything that waits in `state` into ops with ids (a move's first step); whether anything was.
    bool freezeAll(ProgressState& state) const;

    const Resolved& opts_;
    KeyValueStore& store_;
    Api& api_;
    std::function<std::optional<std::string>()> subject_;
    mutable std::mutex rulesMutex_;
    std::map<std::string, std::string> kinds_;
    bool kindsLoaded_ = false;
    std::map<std::string, BoardRule> boards_;
    bool boardsLoaded_ = false;
    std::atomic<std::int64_t> notBefore_{0};
    std::atomic<std::int64_t> backoffMs_{0};
    mutable std::mutex refusalsMutex_;
    std::vector<ProgressRefusal> refusals_;
};
}  // namespace krtu::detail
