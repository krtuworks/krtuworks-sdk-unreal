#pragma once

#include "KrtuworksCore/Export.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "KrtuworksCore/Error.h"
#include "KrtuworksCore/Json.h"
#include "KrtuworksCore/Types.h"

namespace krtu
{
/// The Krtuworks SDK's core: one instance per app and environment, the rules of the JS package
/// (krtuworks-sdk-js/src/client.ts) and the Unity package with the native sign-ins (system browser + loopback, a
/// phone's own scheme, a console's code, Steam). Every call blocks until it is done and may be made from any
/// thread; the Unreal layer runs them off the game thread and hands the results back to it.
class KRTU_API Client
{
public:
    /// The terms a new guest accepts on the age screen (Krtuworks ID Terms + Privacy, the versions this SDK shows).
    static constexpr const char* currentTerms = "TERMS:v1 PRIVACY:v1";

    /// Makes the SDK ready: the player and the remote config this device kept; then, in the background, what
    /// follows a player's arrival (queues, session, unfinished saves) and the server's config.
    static std::unique_ptr<Client> initialize(Options options);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // ── Who plays ────────────────────────────────────────────────────────

    std::optional<Player> player() const;
    const std::string& environment() const;
    /// GAME, MOBILE_APP, WEB_APP or DESKTOP_APP, from krtuworks-config.json.
    const std::string& appType() const;

    /// The player changed: signed_in · guest · signed_out · merged · deletion_pending · sanctioned. Called on the
    /// SDK's thread that made the change; it must not wait for the SDK.
    void setIdentityChangedHandler(std::function<void(const IdentityChange&)> handler);
    /// A merged account's waiting queues moved to the signed-in player (after the server confirmed the merge).
    void setMergeSettledHandler(std::function<void(const MergeSettlement&)> handler);
    /// A guest has something to lose (first_achievement, first_save or play_time) and the app's `linkReminder`
    /// setting says it is time: the game shows its own prompt and, if the player agrees, starts a sign-in.
    /// Remote config `linkReminder`: `{ enabled, afterMinutes, everyDays }` (default on, 30, 3).
    void setLinkReminderHandler(std::function<void(const std::string& reason)> handler);

    /// Plays as a guest on this device; a new guest needs the age screen's answers (birth date `yyyy-MM-dd`,
    /// currentTerms). On Steam (games only) the Steam account's player.
    Player signInAsGuest(const std::optional<std::string>& birthDate = std::nullopt, const std::optional<std::string>& acceptedTerms = std::nullopt);
    /// Desktop sign-in with Krtuworks ID (RFC 8252): `openBrowser(url)` opens the system browser; the call
    /// returns when it came back to this app. A guest playing here is carried along unless `carryGuest` is false.
    Player signInWithBrowser(const std::function<void(const std::string& url)>& openBrowser, const CancelToken& cancel = CancelToken(), bool carryGuest = true);
    /// The scheme a phone app registers so the sign-in page can come back to it (`com.krtuworks.<slug>`).
    std::string appScheme() const;
    /// Phone sign-in: `authenticate(url, scheme, cancel)` is the platform's authentication session; it opens
    /// `url` and returns the address the browser came back to on `scheme`.
    Player signInWithApp(const std::function<std::string(const std::string& url, const std::string& scheme, const CancelToken& cancel)>& authenticate,
                         const CancelToken& cancel = CancelToken(), bool carryGuest = true);
    /// Finishes a sign-in from the address the browser came back to (a phone app the system closed while the
    /// browser was open, started again by that address).
    Player completeSignIn(const std::string& callbackUrl);
    /// A console's or TV's sign-in: show the code and the address, then finishDeviceLink.
    DeviceLink startDeviceLink(const std::optional<std::string>& deviceName = std::nullopt);
    /// Waits until the player approved the code on another device.
    Player finishDeviceLink(const DeviceLink& link, const CancelToken& cancel = CancelToken());
    void signOut();
    /// Waits for the work that follows a player's arrival (tests, or a game that wants to know it is done).
    void ready();

    // ── Telemetry ────────────────────────────────────────────────────────

    /// Records an event (a ready-made family like `screen_view` or `economy`, or the app's own type) with an
    /// object of data. Nothing is made when its module is off, the owner switched this build's telemetry off,
    /// or the player turned statistics off. Returns whether the event was kept. std::invalid_argument for a
    /// type outside the grammar (a lowercase letter, then lowercase letters, digits and _; at most 64).
    bool track(const std::string& type, const Json& data = Json(), const std::optional<EventPosition>& position = std::nullopt, const std::optional<int>& schemaVersion = std::nullopt);
    /// A level starts: the SDK counts the attempt and its times and deaths. Levels and deaths are for games only.
    void levelStart(const LevelRef& level);
    void levelComplete(const std::optional<double>& score = std::nullopt);
    void levelFail(const std::optional<double>& score = std::nullopt);
    void levelQuit();
    void levelCheckpoint(const std::string& name);
    void levelRestart();
    /// A death, counted toward the level in play and told with its place.
    bool death(const std::string& cause, const std::optional<std::string>& enemy = std::nullopt, const std::optional<EventPosition>& position = std::nullopt);
    /// An error the game caught, as the errors family tells it (kind: exception, crash, assert, other).
    bool error(const std::string& message, const std::string& kind = "exception", const std::optional<std::string>& fingerprint = std::nullopt, bool handled = true);
    bool error(const std::exception& err, const std::string& kind = "exception", const std::optional<std::string>& fingerprint = std::nullopt, bool handled = true);

    // ── Bug reports ──────────────────────────────────────────────────────

    /// The categories of a bug report, the same menu in every app.
    static const std::vector<std::string>& bugCategories();
    /// A bug report from the player. The SDK adds the platform, build, device, language, level in play, frame
    /// rate, the last 50 events and the log lines. The report is kept on this device first and sent when it can
    /// be, its screenshot right after it; a player sends at most 5 an hour and 20 a day in an app. Throws
    /// `module_disabled` when the owner turned bug reports off, `too_many_reports` when 20 wait on the device,
    /// std::invalid_argument when the input can never be taken. Returns the report's id on this device.
    std::string reportBug(const BugReportInput& input);
    /// A line for the bug reports' log (kept in memory, the last 32 KB; sent only inside a report the player makes).
    void log(const std::string& message);
    /// The player's bug reports in this app, newest first, with their status and the build a fix shipped in.
    std::vector<BugReportView> myBugReports();
    /// Called for each of the player's reports fixed since the app last looked (at a player's arrival, and on
    /// myBugReports): the game may tell the player "fixed in <fixedIn>". It must not wait for the SDK.
    void setBugFixedHandler(std::function<void(const BugReportView&)> handler);

    // ── Other players ────────────────────────────────────────────────────

    /// The reasons a profile picture is reported for: SEXUAL, HATE, VIOLENCE, IMPERSONATION, SPAM, OTHER.
    static const std::vector<std::string>& avatarReportReasons();
    /// Reports another player's profile picture: `subject` is the id the app knows them by (another player of
    /// this app, e.g. from the game's server), `reason` one of avatarReportReasons(), an optional note of at most
    /// 300 characters. Krtuworks reviews it; the reporter is never told an outcome. Once per player and picture
    /// (true = reported already), at most 10 a day (`too_many_reports`). Needs a connection: nothing waits on the
    /// device. Throws std::invalid_argument when it can never be taken.
    bool reportAvatar(const std::string& subject, const std::string& reason, const std::string& note = "");

    /// The player's statistics choice in this app (on by default). Off: no event is made or kept.
    bool getTelemetryOptOut();
    void setTelemetryOptOut(bool optOut);

    // ── Progress ─────────────────────────────────────────────────────────

    /// Adds to a SUM stat. Kept on the device until the server has it; never lost, never counted twice.
    void incrementStat(const std::string& key, double by = 1);
    /// Reports a value to a MAX, MIN or LATEST stat.
    void setStat(const std::string& key, double value);
    /// Submits a score to a leaderboard. Kept on the device until the server has it, like a stat: never lost,
    /// never counted twice (the board's own rule decides what a new score does to the stored one). `details` are
    /// 1 to 64 bytes the game keeps with the score. std::invalid_argument for a key that is none, a value of
    /// 10^15 or more, or details of another size. While the owner has the leaderboards module off, the score waits.
    void submitScore(const std::string& key, std::int64_t value, const std::optional<std::string>& details = std::nullopt);
    /// The app's leaderboards in `locale` (else the SDK's) with the player's own score and rank. 403 `module_disabled`
    /// while the leaderboards module is off.
    std::vector<LeaderboardView> getLeaderboards(const std::optional<std::string>& locale = std::nullopt);
    /// A leaderboard's rows in its order. `scope`: `global` reads from `offset`; `around` centres the page on the
    /// player (offset 0). `limit` is 1 to 100. Names are Krtuworks usernames.
    LeaderboardPage getLeaderboardEntries(const std::string& key, const std::string& scope = "global", int limit = 25, int offset = 0);
    /// Unlocks an achievement here (kept until the server has it) and on the platform when its id there is known.
    void unlockAchievement(const std::string& key);
    /// The app's stats and achievements with the player's values (names in the SDK's locale where the app has them).
    ProgressView getProgress();
    /// Lines achievements up with the platform's (Steam): what the platform has is unlocked here, and what this
    /// player has here is unlocked there. Runs once per launch on its own; call it after a platform sign-in.
    PlatformSyncResult syncPlatformAchievements();
    /// The platform account (Steam) the open session proved and whether this player mirrors with it; empty when none was proved.
    std::optional<PlatformLink> platformLink() const;
    /// The player's answer, from the game's own dialog, for the Steam account the session proved while it waits
    /// (PENDING_CONSENT, see setPlatformLinkNoticeHandler): allowed, the progress mirrors with it in every game (the
    /// achievements line up at once); refused, it is never asked again (the account site at id.krtuworks.com/apps
    /// changes either). Nothing waiting: throws `consent_not_pending`, nothing sent.
    PlatformLink answerPlatformConsent(bool allow);
    /// Called at a player's arrival when the Steam account the session proved is not mirrored and the game
    /// should say why: PENDING_CONSENT (ask the player, then answerPlatformConsent), LIMIT (a game uses at most
    /// two Steam accounts: remove one at id.krtuworks.com/apps), MISMATCH (the account belongs to another
    /// Krtuworks ID). platformLink() says the same at any time. It must not wait for the SDK.
    void setPlatformLinkNoticeHandler(std::function<void(const PlatformLink&)> handler);
    /// The platform cloud (Steam Cloud) holds a copy of a mirrored SAVE or PROFILE slot that differs from the server's,
    /// and nothing tells which is newer (the first time this device compares them, or both moved since). The copy
    /// waits on the server; the game asks the player which to keep and calls resolveFile (`keep`: the chosen
    /// version's id, `current`: `server.id`). Told once per waiting copy and launch; nothing in that slot is
    /// mirrored until the player chose. It must not wait for the SDK.
    void setPlatformSaveConflictHandler(std::function<void(const PlatformSaveConflict&)> handler);
    /// Lines the platform's cloud (Steam Cloud) up with the mirrored slots now. It runs on its own when a mirrored
    /// session starts, after each save of a mirrored slot and after the player's choice; a copy that waits for the
    /// player is told through setPlatformSaveConflictHandler. Never throws: a platform failure leaves the slot alone.
    void syncPlatformFiles();
    /// Progress the server refused for good (a retired stat, a wrong value): for the game's own diagnostics.
    std::vector<ProgressRefusal> progressRefusals() const;

    // ── What the player owns (the game and its DLCs) ─────────────────────

    /// The player's licences as the server last said them (kept on this device, so an earlier launch's answer is here
    /// when the server cannot be reached): the game (sku `base`) first, then the DLCs. `checked` marks what the server
    /// confirmed with the platform in this launch. Empty for an app that is not a game.
    std::vector<Entitlement> entitlements();
    /// Asks the server for the stored licences now (`GET /v1/me/entitlements`) and keeps the answer. Throws when the server cannot be reached.
    std::vector<Entitlement> refreshEntitlements();
    /// Whether the player owns the DLC with this product key. The server's answer for this launch first (its check of
    /// the session's Steam ticket); when the server could not check, the platform client's own answer (Steam took the
    /// money, so Steam is authoritative); without that, the last answer the server gave on an earlier launch. False for
    /// an unknown key and for an app that is not a game.
    bool ownsDlc(const std::string& key);

    // ── Rich presence ────────────────────────────────────────────────────

    /// Tells the platform (Steam) what the player is doing: `key` is one of the states the app declared on the
    /// server, `params` the values of the params that state's line uses. LEVEL (the level in play) and SESSION_MINUTES
    /// (minutes played this session) fill themselves when the game gives none, and a state that uses one is written
    /// again on every heartbeat (LEVEL also when a level starts); a param the state does not declare is dropped. A value
    /// is cut to 256 bytes. Never throws for a refusal: `reason` is no_platform, not_game, module_disabled, hidden
    /// (the player's switch is off), unknown_state, param_missing, states_unknown (the server's states were never
    /// heard) or platform_rejected (the platform refused a key: nothing is left showing).
    PresenceResult setPresence(const std::string& key, const std::map<std::string, std::string>& params = {});
    /// Clears the platform's keys and forgets the state (the group stays, see setPresenceGroup).
    void clearPresence();
    /// The group the player is in (Steam's `steam_player_group` and `steam_player_group_size`), written with every
    /// state and kept across states; no id leaves the group. std::invalid_argument for an empty id, a size without
    /// an id or a size below 1.
    void setPresenceGroup(const std::optional<std::string>& groupId, const std::optional<int>& size = std::nullopt);
    /// The player's switch for showing presence, as the server last said; nothing before it did (or in an app that is not a game).
    std::optional<bool> presenceVisible();
    /// Sets the player's switch on the server. Off clears the platform's keys at once and keeps the state the game
    /// wanted, so on writes it again. Throws `module_disabled` while the presence module is off.
    void setPresenceVisible(bool visible);

    // ── The app's settings, A/B tests, status ────────────────────────────

    void refreshConfig();
    /// The owner's gates for this build: maintenance, a newer minimum version, a switch.
    Gate getGate() const;
    /// The modules the owner left on (all of them until the config says otherwise; in an app the game-only ones are off).
    std::map<std::string, bool> moduleStates() const;
    /// The player's A/B groups; their values take over the config's (value()).
    std::vector<Experiment> refreshExperiments();
    /// A remote config value, the player's A/B group's when one varies it; null when the owner set none.
    Json value(const std::string& key) const;
    /// A remote config value as `T` (bool, an integer type, a floating type, std::string or Json), or
    /// `fallback` when the owner set none or it is not a `T`.
    template <class T>
    T value(const std::string& key, const T& fallback) const
    {
        const Json v = value(key);
        if constexpr (std::is_same_v<T, bool>)
            return v.isBool() ? v.asBool() : fallback;
        else if constexpr (std::is_integral_v<T>)
            return v.isInteger() && v.asInt() >= static_cast<std::int64_t>((std::numeric_limits<T>::min)()) && v.asInt() <= static_cast<std::int64_t>((std::numeric_limits<T>::max)())
                       ? static_cast<T>(v.asInt())
                       : fallback;
        else if constexpr (std::is_floating_point_v<T>)
            return v.isNumber() ? static_cast<T>(v.asDouble()) : fallback;
        else if constexpr (std::is_same_v<T, std::string>)
            return v.isString() ? v.asString() : fallback;
        else
            return v.isNull() ? fallback : T(v);
    }
    Json getStatus();

    // ── The player's card ────────────────────────────────────────────────

    PlayerProfile getProfile();
    /// Writes the named fields of the player's card (a null value clears one).
    PlayerProfile updateProfile(const Json& fields);

    // ── Files ────────────────────────────────────────────────────────────

    SlotList listFiles();
    /// A slot's current version; nothing when `ifNoneMatch` (a version id held) is still current.
    std::optional<FileRead> readFile(const std::string& slot, const std::optional<SaveDevice>& device = std::nullopt, const std::optional<std::string>& ifNoneMatch = std::nullopt);
    /// Saves a new version of a SAVE or PROFILE slot over `baseVersion` (the version the game read, nothing for
    /// an empty slot). Throws `file_conflict` (`details()["current"]`) when another device saved first.
    FileVersion saveFile(const std::string& slot, const std::string& data, const std::optional<std::string>& baseVersion);
    /// Saves a SETTINGS slot (the latest wins), or DEVICE_SETTINGS with this device's id and description.
    FileVersion saveSettings(const std::string& slot, const std::string& data, const std::optional<SaveDevice>& device = std::nullopt);
    std::vector<WaitingSave> waitingSaves(const std::string& owner);
    FileVersion adoptWaitingSave(const std::string& uploadId, const std::optional<std::string>& baseVersion);
    void discardWaitingSave(const std::string& uploadId);
    /// The player's choice between a slot's version and the one waiting (a merged account's, or the platform cloud's
    /// copy, see setPlatformSaveConflictHandler): keep `keep` over `current` (the one the player saw). A mirrored
    /// slot's kept version then goes to the platform's cloud too.
    FileVersion resolveFile(const std::string& slot, const std::string& keep, const std::string& current);

    // ── A guest's own account (no e-mail, no account site: the device key proves it) ──

    /// Asks to delete this guest account (90 days; signing in as the guest is then only for cancelling or the
    /// data copy). Returns the deletion date.
    std::string requestGuestDeletion();
    std::string cancelGuestDeletion();
    /// The guest's data copy: asks for a new one with `request`; a ready copy comes with a 15-minute download address.
    Json guestDataExport(bool request = false);

    // ── Sending and the app's life ───────────────────────────────────────

    /// Sends what waits: progress first (it must not wait behind telemetry), then bug reports, then events. One send at a time.
    SendResult flush();
    /// What waits on this device for the signed-in player (or the anonymous queue).
    WaitingCounts waiting();
    /// The validator's round (Validator.h): a session opened, kept alive once and ended, every step answered.
    void sessionRound();
    /// Sends again the saves a refusal left waiting (same upload id, same bytes); what became of each.
    std::vector<ResumedUpload> resumeFiles();
    /// The app went to the background or came back.
    void setForeground(bool foreground);
    /// The network came back: the queues try at once (in the background).
    void online();
    /// One beat of the background work (heartbeats, sending every 30 s, the remote config every 5 min); the
    /// SDK's own thread calls it every second unless Options::backgroundLoop is off.
    void tick();
    /// Stops the SDK: the level in play ends as a quit, the session ends, what waits is written to the store.
    void shutdown();

private:
    struct Impl;
    explicit Client(Options options);
    std::unique_ptr<Impl> impl_;
};
}  // namespace krtu
