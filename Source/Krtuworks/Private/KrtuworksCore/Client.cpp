#include "KrtuworksCore/Client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "Api.h"
#include "Bugs.h"
#include "CloudMirror.h"
#include "Events.h"
#include "Files.h"
#include "Identity.h"
#include "Infrastructure.h"
#include "Entitlements.h"
#include "Presence.h"
#include "Progress.h"
#include "Progression.h"
#include "RemoteConfig.h"
#include "Sessions.h"
#include "StatMirror.h"

namespace krtu
{
namespace
{
constexpr std::int64_t clientFlushMs = 30000;
constexpr std::int64_t clientConfigMs = 5 * 60000;
constexpr std::int64_t clientFlushSoonMs = 2000;
constexpr std::int64_t clientNever = (std::numeric_limits<std::int64_t>::max)();

/// At most `limit` bytes of `text`, cut where a UTF-8 character starts (never inside one).
std::string clientCut(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit) return text;
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}
}  // namespace

using namespace detail;

struct Client::Impl
{
    explicit Impl(Options o)
        : options(Resolved::from(o)),
          store(o.store ? o.store : std::make_shared<MemoryStore>()),
          secrets(o.secrets),
          http(o.http),
          platform(o.platformServices),
          identity(options, *store, *secretsOrThrow(), *httpOrThrow(), clock, gamePlatform()),
          api(options, identity, *http, clock),
          events(options, *store, clock, api, [this] { return subject(); }),
          progress(options, *store, api, [this] { return subject(); }),
          sessions(options, api, [this] { return subject(); }, steamTicketSource(), steamTicketCancel()),
          config(options, api, *store),
          files(
              options, api, *store, [this] { return subject(); }, [this] { return identity.generation(); }, [this] { quietly([this] { remindLinking("first_save"); }); }),
          levels(
              *store, options.clientId, [this] { return subject().value_or(anonOwner); }, [this] { return sessions.activeSeconds(); },
              [this](const Json& data) { track("progression", data, std::nullopt, std::nullopt); }),
          bugs(
              options, *store, api, [this] { return subject().value_or(anonOwner); }, [this] { return subject(); },
              [this](const BugReportView& report) { return bugFixedTold(report); }),
          statMirror(makeStatMirror()),
          cloudMirror(makeCloudMirror()),
          presence(
              options, api, *store, presencePlatform(), [this] { return subject(); }, [this] { return config.moduleOn("presence"); },
              [this] {
                  const auto place = levels.inPlay();
                  return place ? std::optional<std::string>(place->level) : std::nullopt;
              },
              [this] { return sessions.sessionActiveSeconds(); }, [this](const std::string& line) { bugs.log(line); }),
          licences(options, api, *store, ownershipPlatform(), [this] { return subject(); }),
          backgroundLoop(o.backgroundLoop),
          started(std::chrono::steady_clock::now())
    {
        sessions.setEntitlementsHandler([this](const Json& items) { licences.fromSession(items); });
    }

    KeyValueStore* secretsOrThrow() const
    {
        if (!secrets) throw std::invalid_argument("secrets is the platform's secret store (tokens and the device key)");
        return secrets.get();
    }

    HttpTransport* httpOrThrow() const
    {
        if (!http) throw std::invalid_argument("http is the engine's HTTP transport");
        return http.get();
    }

    /// The platform's services, for games only: Steam is a game's.
    PlatformServices* gamePlatform() const
    {
        return options.game() ? platform.get() : nullptr;
    }

    /// The stats mirror, only for a game whose platform services also hold stats.
    std::unique_ptr<StatMirror> makeStatMirror()
    {
        auto* services = gamePlatform();
        PlatformStats* stats = services != nullptr ? services->stats() : nullptr;
        if (stats == nullptr) return nullptr;
        return std::make_unique<StatMirror>(*stats, [this] { return uptimeMs(); });
    }

    /// The cloud mirror, only for a game whose platform services also hold a cloud.
    std::unique_ptr<CloudMirror> makeCloudMirror()
    {
        auto* services = gamePlatform();
        PlatformCloud* cloud = services != nullptr ? services->cloud() : nullptr;
        if (cloud == nullptr) return nullptr;
        return std::make_unique<CloudMirror>(
            *cloud, files, *store, options.clientId, [this](const PlatformSaveConflict& conflict) { platformSaveConflictTold(conflict); },
            [this](const std::string& code, const std::string& slot) { error(options.platform + " cloud: " + code + " (" + slot + ")", "other", "sdk." + code, true); });
    }

    /// The platform's rich presence, only for a game whose platform services also hold one.
    PlatformPresence* presencePlatform() const
    {
        auto* services = gamePlatform();
        return services != nullptr ? services->presence() : nullptr;
    }

    /// The platform client's own ownership answer, only for a game whose platform services also hold one.
    PlatformOwnership* ownershipPlatform() const
    {
        auto* services = gamePlatform();
        return services != nullptr ? services->ownership() : nullptr;
    }

    bool steamOn() const
    {
        return options.platform == "STEAM" && gamePlatform() != nullptr;
    }

    std::function<std::optional<std::string>()> steamTicketSource()
    {
        if (!steamOn()) return nullptr;
        return [this] { return platform->webApiTicket(); };
    }

    std::function<void(const std::string&)> steamTicketCancel()
    {
        if (!steamOn()) return nullptr;
        return [this](const std::string& ticket) { platform->cancelWebApiTicket(ticket); };
    }

    std::optional<std::string> subject() const
    {
        const auto current = identity.player();
        if (!current) return std::nullopt;
        return current->subject;
    }

    std::int64_t uptimeMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    }

    // ── Who plays ────────────────────────────────────────────────────────

    void onIdentityChanged(const IdentityChange& change)
    {
        // A stopped SDK opens nothing again (a late sign-in answer included).
        if (stopping.load()) return;
        // A merge in progress changes nobody: the game is only told.
        if (change.type == "merging" || change.type == "merging_ended")
        {
            std::function<void(const IdentityChange&)> told;
            {
                std::lock_guard<std::mutex> lock(handlersMutex);
                told = identityChanged;
            }
            if (told) quietly([&told, &change] { told(change); });
            return;
        }
        if (change.type == "merged" || change.type == "signed_out" || change.type == "deletion_pending" || change.type == "sanctioned")
        {
            sessions.close();
            presence.onIdentity(std::nullopt, true);
            forgetPlayer();
            std::lock_guard<std::mutex> lock(stateMutex);
            sessionOwner.reset();
        }
        else
        {
            const auto subjectNow = change.player ? std::optional<std::string>(change.player->subject) : std::nullopt;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                // A guest that became a Krtuworks ID is the same player: its session goes on.
                if (sessionOwner != subjectNow)
                {
                    sessions.close();
                    forgetPlayer();
                }
                sessionOwner = subjectNow;
            }
            presence.onIdentity(subjectNow, false);
            events.resetBackoff();
            progress.resetBackoff();
            bugs.resetBackoff();
            settler.post([this] { settle(); });
        }
        std::function<void(const IdentityChange&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlersMutex);
            handler = identityChanged;
        }
        if (handler) quietly([&handler, &change] { handler(change); });
    }

    /// What the last player's answers left in memory: the next player starts from the app's own values.
    void forgetPlayer()
    {
        licences.forget();
        {
            std::lock_guard<std::mutex> lock(experimentsMutex);
            experiments.clear();
        }
        optedOut.store(false);
    }

    Player signInAsGuest(const std::optional<std::string>& birthDate, const std::optional<std::string>& acceptedTerms)
    {
        return identity.signInAsGuest(birthDate, acceptedTerms, steamOn());
    }

    /// What follows a player's arrival: the anonymous queues go to the first player; a merged account's waiting
    /// queues move here once the server confirms the merge; the session opens;
    /// unfinished saves are finished; the queues are sent. Without a player, a guest the server could not make
    /// on an earlier launch is tried again.
    void settle()
    {
        if (stopping.load()) return;
        const auto current = identity.player();
        if (!current)
        {
            // A game that needs the server on its very first launch asks it now, and says so when it cannot.
            if (options.firstLaunchOnline && !config.hasCopy()) quietly([this] { refreshConfig(); });
            if (identity.meta().guestRequest) quietly([this] { signInAsGuest(std::nullopt, std::nullopt); });
            return;
        }
        events.move(anonOwner, current->subject);
        progress.move(anonOwner, current->subject);
        bugs.move(anonOwner, current->subject);
        quietly([this, &current] { settleMerges(current->subject); });
        quietly([this] { readOptOut(); });
        quietly([this] { licences.ensureLoaded(); });
        if (stopping.load()) return;
        if (options.sessions && config.moduleOn("sessions")) quietly([this] { sessions.open(); });
        if (options.game() && options.platform == "STEAM") quietly([this] { licences.refresh(); });
        quietly([this] { presence.refresh(); });
        quietly([this] { files.resume(); });
        quietly([this] { flush(); });
        if (config.moduleOn("bugReports")) quietly([this] { bugs.checkFixed(); });
        if (gamePlatform() != nullptr) quietly([this] { settlePlatformLink(); });
    }

    /// The session's platform account: mirrored, the achievements line up once per launch; waiting for the
    /// player's answer, a third account or another Krtuworks ID's, the game is told. Anything else (refused,
    /// none proved) mirrors nothing, so nothing is sent.
    void settlePlatformLink()
    {
        const auto session = sessions.session();
        if (!session || !session->platformLink) return;
        const auto& link = *session->platformLink;
        if (link.state == "PENDING_CONSENT" || link.state == "LIMIT" || link.state == "MISMATCH")
        {
            std::function<void(const PlatformLink&)> handler;
            {
                std::lock_guard<std::mutex> lock(handlersMutex);
                handler = platformLinkNotice;
            }
            if (handler) handler(link);
            return;
        }
        if (link.state != "ACTIVE" || platformSynced.exchange(true)) return;
        quietly([this] { syncPlatformAchievements(); });
        quietly([this] { importPlatformScores(); });
        mirrorFiles(std::nullopt);
    }

    /// The player and session the platform's cloud is mirrored for: only while the session's platform account is
    /// mirrored for this player and the cloudSave module is on.
    std::optional<std::pair<std::string, SessionInfo>> cloudContext() const
    {
        const auto session = sessions.session();
        const auto owner = subject();
        if (!cloudMirror || !session || !session->platformLink || session->platformLink->state != "ACTIVE" || !owner || !config.moduleOn("cloudSave")) return std::nullopt;
        return std::make_pair(*owner, *session);
    }

    /// The platform's cloud lines up with the mirrored slots (all of them, or one after a save).
    void mirrorFiles(const std::optional<std::string>& slot)
    {
        const auto context = cloudContext();
        if (!context) return;
        quietly([&] { cloudMirror->sync(context->first, context->second.platformLink->platformAccountId, context->second.id, slot); });
    }

    /// After the player chose: the kept version goes to the platform's cloud too.
    void mirrorAfterChoice(const std::string& slot)
    {
        const auto context = cloudContext();
        if (!context) return;
        quietly([&] { cloudMirror->afterChoice(context->first, context->second.platformLink->platformAccountId, context->second.id, slot); });
    }

    void platformSaveConflictTold(const PlatformSaveConflict& conflict)
    {
        std::function<void(const PlatformSaveConflict&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlersMutex);
            handler = platformSaveConflict;
        }
        if (handler) quietly([&handler, &conflict] { handler(conflict); });
    }

    void settleMerges(const std::string& current)
    {
        for (const auto& previous : identity.waitingOwners())
        {
            if (previous == current || !isPairwiseSubject(previous))
            {
                identity.forgetWaitingOwner(previous);
                continue;
            }
            PlayerCall call;
            call.method = "POST";
            call.json = Json::object({{"previousSubject", previous}});
            const auto answer = api.player("/v1/me/merge-check", call);
            if (answer.body().getBool("confirmed") != std::optional<bool>(true)) continue;
            events.move(previous, current);
            progress.move(previous, current);
            bugs.move(previous, current);
            identity.forgetWaitingOwner(previous);
            MergeSettlement settled{previous, files.waitingFor(previous)};
            std::function<void(const MergeSettlement&)> handler;
            {
                std::lock_guard<std::mutex> lock(handlersMutex);
                handler = mergeSettled;
            }
            if (handler) quietly([&handler, &settled] { handler(settled); });
        }
    }

    /// Asks the server for the config; a failed first launch of a game that needs the server is remembered (the
    /// gate says so) until the server answers.
    bool refreshConfig()
    {
        try
        {
            const auto changed = config.refresh();
            firstLaunchOffline.store(false);
            return changed;
        }
        catch (const Error&)
        {
            if (options.firstLaunchOnline && !config.hasCopy() && !identity.player()) firstLaunchOffline.store(true);
            throw;
        }
    }

    /// An account that is gone: nothing of it is sent again.
    void dropOwner(const std::string& owner)
    {
        events.drop(owner);
        progress.drop(owner);
        bugs.drop(owner);
        files.dropOwner(owner);
    }

    // ── Telemetry ────────────────────────────────────────────────────────

    bool telemetryAllowed(const std::string& module) const
    {
        return !optedOut.load() && !config.switchForBuild() && config.moduleOn(module);
    }

    bool track(const std::string& type, const Json& data, const std::optional<EventPosition>& position, const std::optional<int>& schemaVersion)
    {
        if (!isEventType(type)) throw std::invalid_argument("\"" + type + "\" is not an event type (a lowercase letter, then lowercase letters, digits and _; at most 64)");
        if (!data.isNull() && !data.isObject()) throw std::invalid_argument("an event's data is an object");
        if (!telemetryAllowed(moduleOf(type))) return false;
        const auto session = sessions.session();
        bugs.noteEvent(events.add(type, data.isObject() && !data.empty() ? std::optional<Json>(data) : std::nullopt, position,
                                  session ? std::optional<std::string>(session->id) : std::nullopt, schemaVersion));
        return true;
    }

    bool error(const std::string& message, const std::string& kind, const std::optional<std::string>& fingerprint, bool handled)
    {
        bugs.log("error " + message);
        const auto place = levels.inPlay();
        auto data = Json::object({{"kind", kind}, {"message", clientCut(message, 500)}, {"handled", handled}});
        if (fingerprint) data["fingerprint"] = clientCut(*fingerprint, 100);
        if (place && place->world && !place->world->empty()) data["world"] = *place->world;
        if (place && !place->level.empty()) data["level"] = place->level;
        return track("error", data, std::nullopt, std::nullopt);
    }

    std::string optOutKey() const
    {
        return "krtu:" + options.clientId + ":optout:" + subject().value_or(anonOwner);
    }

    void readOptOut()
    {
        const auto key = optOutKey();
        // A player with nothing kept on this device is on until the server says otherwise.
        optedOut.store(store->get(key) == std::optional<std::string>("true"));
        if (!identity.player()) return;
        const auto answer = api.player("/v1/me/telemetry-opt-out", PlayerCall());
        // Another player came in while the answer was on its way: it is not theirs.
        if (optOutKey() != key) return;
        optedOut.store(answer.body().getBool("optedOut") == std::optional<bool>(true));
        store->set(key, optedOut.load() ? "true" : "false");
    }

    bool bugFixedTold(const BugReportView& report)
    {
        std::function<void(const BugReportView&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlersMutex);
            handler = bugFixed;
        }
        if (!handler) return false;
        handler(report);
        return true;
    }

    // ── Progress ─────────────────────────────────────────────────────────

    void scheduleFlush()
    {
        const auto at = uptimeMs() + clientFlushSoonMs;
        auto current = flushSoonAt.load();
        while (at < current && !flushSoonAt.compare_exchange_weak(current, at))
        {
        }
    }

    std::string platformIdsKey() const
    {
        return "krtu:" + options.clientId + ":platformIds:" + options.platform;
    }

    std::string statIdsKey() const
    {
        return "krtu:" + options.clientId + ":statIds:" + options.platform;
    }

    /// The session proved a platform account this player mirrors with: the platform may be written to.
    bool mirroring() const
    {
        const auto session = sessions.session();
        return session && session->platformLink && session->platformLink->state == "ACTIVE";
    }

    /// The stats' platform ids kept by the last getProgress (stat key to id and type); nothing before the first.
    std::optional<std::map<std::string, StatPlatformId>> storedStatIds()
    {
        const auto kept = getJson(*store, statIdsKey());
        if (!kept) return std::nullopt;
        std::map<std::string, StatPlatformId> ids;
        for (const auto& member : kept->members())
        {
            ids[member.first] = StatPlatformId{member.second.getString("id").value_or(""), member.second.getString("type").value_or("")};
        }
        return ids;
    }

    /// The stats the server just gave values to, set on the platform (the server's value, never an increase).
    void mirrorStats(const std::map<std::string, double>& values)
    {
        if (!statMirror || values.empty() || !mirroring()) return;
        const auto ids = storedStatIds();
        if (!ids) return;
        for (const auto& pair : values)
        {
            const auto found = ids->find(pair.first);
            if (found != ids->end()) statMirror->set(found->second, pair.second);
        }
        statMirror->store(false);
    }

    ProgressView getProgress()
    {
        PlayerCall call;
        call.query = {{"locale", options.locale}};
        const auto answer = api.player("/v1/me/progress", call);
        auto view = ProgressView::fromJson(answer.body());
        std::vector<std::pair<std::string, std::string>> kinds;
        for (const auto& stat : view.stats) kinds.emplace_back(stat.key, stat.kind);
        progress.learnKinds(kinds);
        auto ids = Json::object();
        for (const auto& achievement : view.achievements)
        {
            const auto found = achievement.platformIds.find(options.platform);
            if (found != achievement.platformIds.end()) ids[achievement.key] = found->second;
        }
        putJson(*store, platformIdsKey(), ids);
        auto statIds = Json::object();
        for (const auto& stat : view.stats)
        {
            const auto found = stat.platformIds.find(options.platform);
            if (found != stat.platformIds.end()) statIds[stat.key] = Json::object({{"id", found->second}, {"type", stat.valueType == "integer" ? "INT" : "FLOAT"}});
        }
        putJson(*store, statIdsKey(), statIds);
        return view;
    }

    std::vector<LeaderboardView> getLeaderboards(const std::optional<std::string>& locale)
    {
        PlayerCall call;
        call.query = {{"locale", locale ? locale : options.locale}};
        const auto answer = api.player("/v1/me/leaderboards", call);
        std::vector<LeaderboardView> boards;
        std::vector<std::pair<std::string, BoardRule>> rules;
        for (const auto& item : answer.body()["leaderboards"].items())
        {
            if (!item.isObject()) continue;
            boards.push_back(LeaderboardView::fromJson(item));
            const auto& board = boards.back();
            if (board.uploadMethod && !board.sortOrder.empty()) rules.emplace_back(board.key, BoardRule{board.sortOrder, *board.uploadMethod});
        }
        progress.learnBoards(rules);
        return boards;
    }

    LeaderboardPage getLeaderboardEntries(const std::string& boardKey, const std::string& scope, int limit, int offset)
    {
        if (!isStatKey(boardKey)) throw std::invalid_argument("\"" + boardKey + "\" is not a leaderboard key");
        if (scope != "global" && scope != "around") throw std::invalid_argument("scope is global or around");
        if (limit < 1 || limit > 100) throw std::invalid_argument("limit is 1 to 100");
        if (offset < 0 || (scope == "around" && offset != 0)) throw std::invalid_argument("offset is 0 or more, and 0 for the around scope");
        PlayerCall call;
        call.query = {{"scope", scope}, {"limit", std::to_string(limit)}, {"offset", std::to_string(offset)}};
        return LeaderboardPage::fromJson(api.player("/v1/leaderboards/" + boardKey + "/entries", call).body());
    }

    std::string importedKey(const std::string& owner, const std::string& platformAccountId) const
    {
        return "krtu:" + options.clientId + ":lbImported:" + owner + ":" + platformAccountId;
    }

    /// The player's own row of each board that takes the platform's in, read once per board and platform account
    /// and sent as a score from the platform. Only while the session's platform account is mirrored for this
    /// player. A read the platform could not answer is tried again next launch. Nothing is ever written to the platform.
    void importPlatformScores()
    {
        auto* services = gamePlatform();
        PlatformStats* stats = services != nullptr ? services->stats() : nullptr;
        const auto session = sessions.session();
        const auto owner = subject();
        if (stats == nullptr || !mirroring() || !session || !owner || !config.moduleOn("leaderboards")) return;
        const auto markerKey = importedKey(*owner, session->platformLink->platformAccountId);
        auto imported = getJson(*store, markerKey).value_or(Json::object());
        bool queued = false;
        for (const auto& board : getLeaderboards(std::nullopt))
        {
            const auto name = board.platformIds.find(options.platform);
            if (!board.importFromPlatform || name == board.platformIds.end() || imported.contains(board.key)) continue;
            std::int64_t score = 0;
            const auto read = stats->readOwnScore(name->second, score);
            if (read == PlatformScoreRead::failed) continue;
            if (read == PlatformScoreRead::found)
            {
                progress.importScore(board.key, score);
                queued = true;
            }
            imported[board.key] = true;
            putJson(*store, markerKey, imported);
        }
        if (queued) scheduleFlush();
    }

    PlatformSyncResult syncPlatformAchievements()
    {
        auto* services = gamePlatform();
        if (services == nullptr) throw std::logic_error("no platform services were given (or this is not a game)");
        const auto session = sessions.session() ? sessions.session() : (options.sessions && config.moduleOn("sessions") ? sessions.open() : std::nullopt);
        auto unlocked = Json::array();
        for (const auto& id : services->unlockedAchievements()) unlocked.push(id);
        PlayerCall call;
        call.method = "POST";
        auto body = Json::object({{"platform", options.platform}, {"build", options.build}, {"unlocked", unlocked}});
        std::map<std::string, StatPlatformId> statIds;
        if (statMirror)
        {
            getProgress();
            statIds = storedStatIds().value_or(statIds);
            auto held = Json::array();
            for (const auto& entry : statIds)
            {
                const auto value = statMirror->read(entry.second);
                if (value) held.push(Json::object({{"id", entry.second.id}, {"value", *value}}));
            }
            body["stats"] = held;
        }
        if (session) body["sessionId"] = session->id;
        call.json = body;
        const auto answer = api.player("/v1/me/progress/platform-sync", call);
        PlatformSyncResult result;
        for (const auto& id : answer.body()["unlockedHere"].items()) result.unlockedHere.push_back(id.isString() ? id.asString() : "");
        for (const auto& id : answer.body()["toPlatform"].items()) result.toPlatform.push_back(id.isString() ? id.asString() : "");
        for (const auto& id : answer.body()["toClear"].items()) result.toClear.push_back(id.isString() ? id.asString() : "");
        for (const auto& id : result.toPlatform) quietly([services, &id] { services->unlockAchievement(id); });
        for (const auto& id : result.toClear) quietly([services, &id] { services->clearAchievement(id); });
        for (const auto& key : answer.body()["statsImported"].items()) result.statsImported.push_back(key.isString() ? key.asString() : "");
        if (statMirror)
        {
            std::map<std::string, StatPlatformId> byId;
            for (const auto& entry : statIds) byId[entry.second.id] = entry.second;
            for (const auto& item : answer.body()["statsToPlatform"].items())
            {
                const auto id = item.getString("id").value_or("");
                const auto value = item.getNumber("value");
                const auto found = byId.find(id);
                if (found == byId.end() || !value) continue;
                quietly([this, &found, &value] { statMirror->set(found->second, *value); });
                result.statsToPlatform.push_back(id);
            }
            quietly([this] { statMirror->store(true); });
        }
        return result;
    }

    PlatformLink answerPlatformConsent(bool allow)
    {
        const auto session = sessions.session();
        if (!session || !session->platformLink || session->platformLink->state != "PENDING_CONSENT")
            throw Error("consent_not_pending", "No Steam account is waiting for the player's answer in this session.");
        const auto& link = *session->platformLink;
        PlayerCall call;
        call.method = "POST";
        call.json = Json::object({{"platform", link.platform}, {"platformAccountId", link.platformAccountId}, {"allow", allow}, {"sessionId", session->id}});
        const auto answer = api.player("/v1/me/platform-links/consent", call);
        const auto after = SessionKeeper::linkOf(answer.body()["platformLink"]);
        if (!after) throw Error("unknown", "The server's answer carried no platform link.");
        sessions.setPlatformLink(session->id, *after);
        if (after->state == "ACTIVE" && gamePlatform() != nullptr)
        {
            platformSynced.store(true);
            quietly([this] { syncPlatformAchievements(); });
            quietly([this] { importPlatformScores(); });
            mirrorFiles(std::nullopt);
        }
        return *after;
    }

    // ── "Link a Krtuworks ID" for guests ─────────────────

    void remindLinking(const std::string& reason)
    {
        const auto current = identity.player();
        std::function<void(const std::string&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlersMutex);
            handler = linkReminder;
        }
        if (!current || current->kind != PlayerKind::Guest || !handler) return;
        const auto setting = config.get("linkReminder");
        if (setting.getBool("enabled") == std::optional<bool>(false)) return;
        const auto key = "krtu:" + options.clientId + ":linkReminder:" + current->subject;
        auto state = getJson(*store, key).value_or(Json::object());
        if (!state.isObject()) state = Json::object();
        auto playSeconds = state.getInt("playSeconds").value_or(0);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            const auto active = sessions.activeSeconds();
            playSeconds += (std::max)(static_cast<std::int64_t>(0), active - countedActive);
            countedActive = active;
        }
        state["playSeconds"] = playSeconds;
        bool due = false;
        if (reason == "first_achievement" && state.getBool("achievement") != std::optional<bool>(true))
        {
            due = true;
            state["achievement"] = true;
        }
        if (reason == "first_save" && state.getBool("save") != std::optional<bool>(true))
        {
            due = true;
            state["save"] = true;
        }
        if (reason == "play_time") due = static_cast<double>(playSeconds) >= setting.getNumber("afterMinutes").value_or(30) * 60;
        const auto quietMs = static_cast<std::int64_t>(setting.getNumber("everyDays").value_or(3) * 86400000.0);
        const auto now = nowMs();
        const auto shownAt = state.getInt("shownAt");
        const bool show = due && (!shownAt || now - *shownAt >= quietMs);
        if (show) state["shownAt"] = now;
        putJson(*store, key, state);
        if (show) quietly([&handler, &reason] { handler(reason); });
    }

    // ── A guest's own account ────────────────────────────────────────────

    /// One of the guest's own calls, proved by the device key (no e-mail, no account site).
    Json guestCall(const std::string& path, const std::string& purpose, FormFields fields)
    {
        fields.emplace_back("client_id", options.clientId);
        fields.emplace_back("assertion", identity.guestProof(purpose));
        const auto answer = wire::send(*http, clock, wire::form(options.idOrigin + path, fields));
        if (answer.status >= 400) throw wire::errorOf(answer);
        return answer.body();
    }

    // ── Sending and the loop ─────────────────────────────────────────────

    SendResult flush()
    {
        return flushing.run([this] { return flushOnce(); });
    }

    SendResult flushOnce()
    {
        lastFlushAt.store(uptimeMs());
        flushSoonAt.store(clientNever);
        quietly([this] { remindLinking("play_time"); });
        SendResult result;
        result.progress = progress.flush();
        quietly([this, &result] { mirrorStats(result.progress.values); });
        const auto off = config.switchForBuild();
        if (off && off->mode == "ALL")
        {
            result.bugs.stoppedBy = "switched_off";
            result.events.stoppedBy = "switched_off";
        }
        else
        {
            result.bugs = bugs.flush();
            result.events = events.flush(config.moduleOn("errors"));
        }
        return result;
    }

    /// The settings slots a scheduled send just finished are lined up with the platform's cloud.
    void sentSettings(const std::vector<std::string>& slots)
    {
        for (const auto& slot : slots) mirrorFiles(slot);
    }

    void tick()
    {
        const auto now = uptimeMs();
        quietly([this] {
            if (sessions.due()) presence.onHeartbeat();
        });
        if (events.persistDue()) quietly([this] { events.persist(); });
        if (statMirror) quietly([this] { statMirror->store(false); });
        if (options.sessions && config.moduleOn("sessions")) sessions.retryOpen();
        if (files.scheduledDue()) quietly([this] { sentSettings(files.sendScheduled(false)); });
        if (now >= flushSoonAt.load() || now - lastFlushAt.load() >= clientFlushMs) quietly([this] { flush(); });
        if (now - lastConfigAt.load() >= clientConfigMs)
        {
            lastConfigAt.store(now);
            quietly([this] { refreshConfig(); });
            quietly([this] { presence.refresh(); });
        }
    }

    void loop()
    {
        std::unique_lock<std::mutex> lock(loopMutex);
        while (!stopping.load())
        {
            loopWake.wait_for(lock, std::chrono::seconds(1), [this] { return stopping.load(); });
            if (stopping.load()) return;
            lock.unlock();
            tick();
            lock.lock();
        }
    }

    void shutdown()
    {
        if (stopped.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lock(loopMutex);
            stopping.store(true);
        }
        loopWake.notify_all();
        // What waits in memory reaches the store first: the loop thread may be inside a long call, and the caller may not
        // wait for it.
        quietly([this] { events.persist(); });
        if (loopThread.joinable() && loopThread.get_id() != std::this_thread::get_id()) loopThread.join();
        levels.finish("quit", std::nullopt);
        quietly([this] { events.persist(); });
        quietly([this] { files.sendScheduled(true); });
        if (statMirror) quietly([this] { statMirror->store(true); });
        sessions.end();
        settler.stop();
    }

    // Order matters: each member is built from the ones above it.
    Resolved options;
    std::shared_ptr<KeyValueStore> store;
    std::shared_ptr<KeyValueStore> secrets;
    std::shared_ptr<HttpTransport> http;
    std::shared_ptr<PlatformServices> platform;
    Clock clock;
    Identity identity;
    Api api;
    EventQueue events;
    ProgressQueue progress;
    SessionKeeper sessions;
    RemoteConfig config;
    Files files;
    ProgressionTracker levels;
    BugReports bugs;
    std::unique_ptr<StatMirror> statMirror;
    std::unique_ptr<CloudMirror> cloudMirror;
    Presence presence;
    Entitlements licences;
    bool backgroundLoop;
    std::chrono::steady_clock::time_point started;

    std::atomic<bool> optedOut{false};
    std::atomic<bool> firstLaunchOffline{false};
    std::mutex experimentsMutex;
    std::map<std::string, Json> experiments;
    std::mutex stateMutex;
    std::optional<std::string> sessionOwner;
    std::int64_t countedActive = 0;
    std::atomic<std::int64_t> flushSoonAt{clientNever};
    std::atomic<std::int64_t> lastFlushAt{0};
    std::atomic<std::int64_t> lastConfigAt{0};
    std::atomic<bool> platformSynced{false};
    SingleFlight<SendResult> flushing;

    std::mutex handlersMutex;
    std::function<void(const IdentityChange&)> identityChanged;
    std::function<void(const MergeSettlement&)> mergeSettled;
    std::function<void(const std::string&)> linkReminder;
    std::function<void(const BugReportView&)> bugFixed;
    std::function<void(const PlatformLink&)> platformLinkNotice;
    std::function<void(const PlatformSaveConflict&)> platformSaveConflict;

    std::mutex loopMutex;
    std::condition_variable loopWake;
    std::atomic<bool> stopping{false};
    std::atomic<bool> stopped{false};
    Worker settler;
    std::thread loopThread;
};

// ── Client ──────────────────────────────────────────────────────────────

Client::Client(Options options) : impl_(std::make_unique<Impl>(std::move(options)))
{
}

Client::~Client()
{
    if (impl_) impl_->shutdown();
}

std::unique_ptr<Client> Client::initialize(Options options)
{
    std::unique_ptr<Client> client(new Client(std::move(options)));
    auto& impl = *client->impl_;
    const auto loaded = impl.identity.load();
    impl.sessionOwner = loaded ? std::optional<std::string>(loaded->subject) : std::nullopt;
    impl.config.load();
    impl.identity.addListener([&impl](const IdentityChange& change) { impl.onIdentityChanged(change); });
    impl.identity.setDeletedHandler([&impl](const std::string& owner) { impl.dropOwner(owner); });
    const auto now = impl.uptimeMs();
    impl.lastFlushAt.store(now);
    // The server's config is asked for on the first beat of the loop, not before the game can go on.
    impl.lastConfigAt.store(now - clientConfigMs);
    impl.settler.post([&impl] { impl.settle(); });
    if (impl.backgroundLoop) impl.loopThread = std::thread([&impl] { impl.loop(); });
    return client;
}

std::optional<Player> Client::player() const
{
    return impl_->identity.player();
}

const std::string& Client::environment() const
{
    return impl_->options.environment;
}

const std::string& Client::appType() const
{
    return impl_->options.type;
}

void Client::setIdentityChangedHandler(std::function<void(const IdentityChange&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->handlersMutex);
    impl_->identityChanged = std::move(handler);
}

void Client::setMergeSettledHandler(std::function<void(const MergeSettlement&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->handlersMutex);
    impl_->mergeSettled = std::move(handler);
}

void Client::setLinkReminderHandler(std::function<void(const std::string& reason)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->handlersMutex);
    impl_->linkReminder = std::move(handler);
}

Player Client::signInAsGuest(const std::optional<std::string>& birthDate, const std::optional<std::string>& acceptedTerms)
{
    return impl_->signInAsGuest(birthDate, acceptedTerms);
}

Player Client::signInWithBrowser(const std::function<void(const std::string& url)>& openBrowser, const CancelToken& cancel, bool carryGuest)
{
    impl_->events.persist();
    return impl_->identity.signInWithBrowser(openBrowser, cancel, carryGuest);
}

std::string Client::appScheme() const
{
    return impl_->identity.appScheme();
}

Player Client::signInWithApp(const std::function<std::string(const std::string& url, const std::string& scheme, const CancelToken& cancel)>& authenticate, const CancelToken& cancel,
                             bool carryGuest)
{
    impl_->events.persist();
    return impl_->identity.signInWithApp(authenticate, cancel, carryGuest);
}

Player Client::completeSignIn(const std::string& callbackUrl)
{
    return impl_->identity.completeSignIn(callbackUrl);
}

DeviceLink Client::startDeviceLink(const std::optional<std::string>& deviceName)
{
    return impl_->identity.startDeviceLink(deviceName);
}

Player Client::finishDeviceLink(const DeviceLink& link, const CancelToken& cancel)
{
    return impl_->identity.finishDeviceLink(link, cancel);
}

void Client::signOut()
{
    quietly([this] { flush(); });
    impl_->sessions.end();
    impl_->identity.signOut();
}

void Client::ready()
{
    impl_->settler.drain();
}

bool Client::track(const std::string& type, const Json& data, const std::optional<EventPosition>& position, const std::optional<int>& schemaVersion)
{
    return impl_->track(type, data, position, schemaVersion);
}

void Client::levelStart(const LevelRef& level)
{
    if (!impl_->options.game()) return;
    impl_->levels.start(level);
    quietly([this] { impl_->presence.onLevelStart(); });
}

void Client::levelComplete(const std::optional<double>& score)
{
    if (impl_->options.game()) impl_->levels.finish("complete", score);
}

void Client::levelFail(const std::optional<double>& score)
{
    if (impl_->options.game()) impl_->levels.finish("fail", score);
}

void Client::levelQuit()
{
    if (impl_->options.game()) impl_->levels.finish("quit", std::nullopt);
}

void Client::levelCheckpoint(const std::string& name)
{
    if (impl_->options.game()) impl_->levels.checkpoint(name);
}

void Client::levelRestart()
{
    if (impl_->options.game()) impl_->levels.restart();
}

bool Client::death(const std::string& cause, const std::optional<std::string>& enemy, const std::optional<EventPosition>& position)
{
    if (!impl_->options.game()) return false;
    const auto place = impl_->levels.noteDeath();
    auto data = Json::object({{"cause", cause}});
    if (enemy) data["enemy"] = *enemy;
    if (place && place->world && !place->world->empty()) data["world"] = *place->world;
    if (place && !place->level.empty()) data["level"] = place->level;
    return impl_->track("death", data, position, std::nullopt);
}

bool Client::error(const std::string& message, const std::string& kind, const std::optional<std::string>& fingerprint, bool handled)
{
    return impl_->error(message, kind, fingerprint, handled);
}

bool Client::error(const std::exception& err, const std::string& kind, const std::optional<std::string>& fingerprint, bool handled)
{
    return error(std::string(err.what()), kind, fingerprint, handled);
}

const std::vector<std::string>& Client::bugCategories()
{
    return BugReports::categories();
}

std::string Client::reportBug(const BugReportInput& input)
{
    if (!impl_->config.moduleOn("bugReports")) throw Error("module_disabled", "Bug reports are off in this app.", 403);
    std::optional<std::string> level;
    if (const auto place = impl_->levels.inPlay())
    {
        std::string joined;
        for (const auto& part : {place->world.value_or(""), place->level, place->part.value_or("")})
            if (!part.empty()) joined += (joined.empty() ? "" : "/") + part;
        if (!joined.empty()) level = joined;
    }
    auto reportId = impl_->bugs.report(input, level);
    impl_->scheduleFlush();
    return reportId;
}

void Client::log(const std::string& message)
{
    impl_->bugs.log(message);
}

std::vector<BugReportView> Client::myBugReports()
{
    return impl_->bugs.mine();
}

void Client::setBugFixedHandler(std::function<void(const BugReportView&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->handlersMutex);
    impl_->bugFixed = std::move(handler);
}

const std::vector<std::string>& Client::avatarReportReasons()
{
    static const std::vector<std::string> reasons = {"SEXUAL", "HATE", "VIOLENCE", "IMPERSONATION", "SPAM", "OTHER"};
    return reasons;
}

bool Client::reportAvatar(const std::string& subject, const std::string& reason, const std::string& note)
{
    if (!detail::isPairwiseSubject(subject)) throw std::invalid_argument("name the player by the subject the app knows them by (pw_...)");
    const auto& reasons = avatarReportReasons();
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) throw std::invalid_argument("reason is one of SEXUAL, HATE, VIOLENCE, IMPERSONATION, SPAM, OTHER");
    const auto trimmed = detail::trimSpace(note);
    if (detail::utf16Length(trimmed) > 300) throw std::invalid_argument("a note is at most 300 characters");
    PlayerCall call;
    call.method = "POST";
    call.json = Json::object({{"reason", reason}});
    if (!trimmed.empty()) (*call.json)["note"] = trimmed;
    const auto answer = impl_->api.player("/v1/players/" + urlEncode(subject) + "/avatar-report", call);
    const auto& already = answer.body()["already"];
    return already.isBool() && already.asBool();
}

bool Client::getTelemetryOptOut()
{
    impl_->readOptOut();
    return impl_->optedOut.load();
}

void Client::setTelemetryOptOut(bool optOut)
{
    PlayerCall call;
    call.method = "PUT";
    call.json = Json::object({{"optedOut", optOut}});
    impl_->api.player("/v1/me/telemetry-opt-out", call);
    impl_->optedOut.store(optOut);
    impl_->store->set(impl_->optOutKey(), optOut ? "true" : "false");
}

void Client::incrementStat(const std::string& key, double by)
{
    impl_->progress.report("add", key, by);
    impl_->scheduleFlush();
}

void Client::setStat(const std::string& key, double value)
{
    impl_->progress.report("set", key, value);
    impl_->scheduleFlush();
}

void Client::submitScore(const std::string& key, std::int64_t value, const std::optional<std::string>& details)
{
    impl_->progress.submitScore(key, value, details);
    impl_->scheduleFlush();
}

std::vector<LeaderboardView> Client::getLeaderboards(const std::optional<std::string>& locale)
{
    return impl_->getLeaderboards(locale);
}

LeaderboardPage Client::getLeaderboardEntries(const std::string& key, const std::string& scope, int limit, int offset)
{
    return impl_->getLeaderboardEntries(key, scope, limit, offset);
}

void Client::unlockAchievement(const std::string& key)
{
    impl_->progress.unlock(key);
    impl_->scheduleFlush();
    if (auto* services = impl_->gamePlatform(); services != nullptr && impl_->mirroring())
    {
        const auto ids = getJson(*impl_->store, impl_->platformIdsKey());
        const auto platformId = ids ? ids->getString(key) : std::nullopt;
        if (platformId) quietly([services, &platformId] { services->unlockAchievement(*platformId); });
    }
    quietly([this] { impl_->remindLinking("first_achievement"); });
}

ProgressView Client::getProgress()
{
    return impl_->getProgress();
}

PlatformSyncResult Client::syncPlatformAchievements()
{
    return impl_->syncPlatformAchievements();
}

std::optional<PlatformLink> Client::platformLink() const
{
    const auto session = impl_->sessions.session();
    return session ? session->platformLink : std::nullopt;
}

PlatformLink Client::answerPlatformConsent(bool allow)
{
    return impl_->answerPlatformConsent(allow);
}

void Client::setPlatformLinkNoticeHandler(std::function<void(const PlatformLink&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->handlersMutex);
    impl_->platformLinkNotice = std::move(handler);
}

void Client::setPlatformSaveConflictHandler(std::function<void(const PlatformSaveConflict&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->handlersMutex);
    impl_->platformSaveConflict = std::move(handler);
}

void Client::syncPlatformFiles()
{
    impl_->mirrorFiles(std::nullopt);
}

std::vector<ProgressRefusal> Client::progressRefusals() const
{
    return impl_->progress.refusals();
}

PresenceResult Client::setPresence(const std::string& key, const std::map<std::string, std::string>& params)
{
    return impl_->presence.set(key, params);
}

void Client::clearPresence()
{
    impl_->presence.clear();
}

void Client::setPresenceGroup(const std::optional<std::string>& groupId, const std::optional<int>& size)
{
    impl_->presence.setGroup(groupId, size);
}

std::optional<bool> Client::presenceVisible()
{
    return impl_->presence.visible();
}

void Client::setPresenceVisible(bool visible)
{
    impl_->presence.setVisible(visible);
}

std::vector<Entitlement> Client::entitlements()
{
    return impl_->options.game() ? impl_->licences.list() : std::vector<Entitlement>{};
}

std::vector<Entitlement> Client::refreshEntitlements()
{
    return impl_->options.game() ? impl_->licences.refresh() : std::vector<Entitlement>{};
}

bool Client::ownsDlc(const std::string& key)
{
    return impl_->options.game() && impl_->licences.ownsDlc(key);
}

void Client::refreshConfig()
{
    impl_->refreshConfig();
}

Gate Client::getGate() const
{
    const auto maintenance = impl_->config.get("maintenance");
    const auto minVersion = impl_->config.get("minVersion");
    Gate gate;
    gate.maintenance = maintenance.getBool("enabled") == std::optional<bool>(true);
    gate.maintenanceMessage = maintenance.getString("message");
    gate.updateRequired = minVersion.isString() && RemoteConfig::olderThan(impl_->options.build, minVersion.asString());
    gate.switchedOff = impl_->config.switchForBuild();
    gate.firstLaunchOffline = impl_->firstLaunchOffline.load() && !impl_->identity.player();
    return gate;
}

std::map<std::string, bool> Client::moduleStates() const
{
    return impl_->config.moduleStates();
}

std::vector<Experiment> Client::refreshExperiments()
{
    const auto asked = impl_->subject();
    const auto answer = impl_->api.player("/v1/me/experiments", PlayerCall());
    std::vector<Experiment> list;
    for (const auto& item : answer.body()["experiments"].items())
    {
        if (item.isObject()) list.push_back(Experiment::fromJson(item));
    }
    // Another player came in while the answer was on its way: their groups are not these.
    if (impl_->subject() != asked) return list;
    std::lock_guard<std::mutex> lock(impl_->experimentsMutex);
    impl_->experiments.clear();
    for (const auto& experiment : list) impl_->experiments[experiment.configKey] = experiment.value;
    return list;
}

Json Client::value(const std::string& key) const
{
    // A/B values count only while the A/B module is on.
    if (impl_->config.moduleOn("abTests"))
    {
        std::lock_guard<std::mutex> lock(impl_->experimentsMutex);
        const auto found = impl_->experiments.find(key);
        if (found != impl_->experiments.end()) return found->second;
    }
    return impl_->config.get(key);
}

Json Client::getStatus()
{
    PlayerCall call;
    call.query = {{"platform", impl_->options.platform}};
    return impl_->api.player("/v1/me/status", call).body();
}

PlayerProfile Client::getProfile()
{
    return PlayerProfile::fromJson(impl_->api.player("/v1/me/profile", PlayerCall()).body());
}

PlayerProfile Client::updateProfile(const Json& fields)
{
    if (!fields.isObject()) throw std::invalid_argument("the profile's fields are an object");
    PlayerCall call;
    call.method = "PUT";
    call.json = Json::object({{"fields", fields}, {"platform", impl_->options.platform}, {"build", impl_->options.build}});
    return PlayerProfile::fromJson(impl_->api.player("/v1/me/profile", call).body());
}

SlotList Client::listFiles()
{
    return impl_->files.list();
}

std::optional<FileRead> Client::readFile(const std::string& slot, const std::optional<SaveDevice>& device, const std::optional<std::string>& ifNoneMatch)
{
    return impl_->files.read(slot, device, ifNoneMatch);
}

FileVersion Client::saveFile(const std::string& slot, const std::string& data, const std::optional<std::string>& baseVersion)
{
    auto saved = impl_->files.save(slot, data, true, baseVersion, std::nullopt);
    impl_->settler.post([impl = impl_.get(), slot] { impl->mirrorFiles(slot); });
    return saved;
}

FileVersion Client::saveSettings(const std::string& slot, const std::string& data, const std::optional<SaveDevice>& device)
{
    auto saved = impl_->files.saveSettings(slot, data, device);
    if (!device && !saved.queued) impl_->settler.post([impl = impl_.get(), slot] { impl->mirrorFiles(slot); });
    return saved;
}

std::vector<WaitingSave> Client::waitingSaves(const std::string& owner)
{
    return impl_->files.waitingFor(owner);
}

FileVersion Client::adoptWaitingSave(const std::string& uploadId, const std::optional<std::string>& baseVersion)
{
    return impl_->files.adoptWaiting(uploadId, baseVersion);
}

void Client::discardWaitingSave(const std::string& uploadId)
{
    impl_->files.discardWaiting(uploadId);
}

FileVersion Client::resolveFile(const std::string& slot, const std::string& keep, const std::string& current)
{
    auto kept = impl_->files.resolve(slot, keep, current);
    impl_->mirrorAfterChoice(slot);
    return kept;
}

std::string Client::requestGuestDeletion()
{
    quietly([this] { flush(); });
    const auto answer = impl_->guestCall("/v1/guest/deletion", "delete", {{"confirm", std::string("true")}});
    impl_->sessions.close();
    return answer.getString("deletionDate").value_or("");
}

std::string Client::cancelGuestDeletion()
{
    return impl_->guestCall("/v1/guest/deletion/cancel", "cancel_deletion", {}).getString("status").value_or("");
}

Json Client::guestDataExport(bool request)
{
    FormFields fields;
    if (request) fields.emplace_back("request", std::string("true"));
    return impl_->guestCall("/v1/guest/data-export", "export_download", fields);
}

SendResult Client::flush()
{
    return impl_->flush();
}

void Client::sessionRound()
{
    impl_->sessions.round();
}

std::vector<ResumedUpload> Client::resumeFiles()
{
    return impl_->files.resume();
}

WaitingCounts Client::waiting()
{
    const auto owner = impl_->subject().value_or(anonOwner);
    return WaitingCounts{impl_->events.size(owner), impl_->progress.waiting(owner), impl_->bugs.waiting()};
}

void Client::setForeground(bool foreground)
{
    impl_->sessions.setForeground(foreground);
    if (foreground) return;
    quietly([this] { impl_->events.persist(); });
    if (impl_->statMirror) quietly([this] { impl_->statMirror->store(true); });
    // The app may not come back: a settings save that waits for its turn goes now.
    impl_->settler.post([impl = impl_.get()] { quietly([impl] { impl->sentSettings(impl->files.sendScheduled(true)); }); });
}

void Client::online()
{
    impl_->events.resetBackoff();
    impl_->progress.resetBackoff();
    impl_->bugs.resetBackoff();
    impl_->sessions.resetOpenBackoff();
    impl_->settler.post([this] {
        quietly([this] { impl_->sentSettings(impl_->files.sendScheduled(true)); });
        impl_->flush();
    });
}

std::optional<std::int64_t> Client::serverOffsetMs() const
{
    return impl_->clock.serverOffsetMs();
}

void Client::tick()
{
    impl_->tick();
}

void Client::shutdown()
{
    impl_->shutdown();
}
}  // namespace krtu
