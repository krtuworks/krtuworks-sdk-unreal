#pragma once

#include "KrtuworksCore/Export.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "KrtuworksCore/Error.h"
#include "KrtuworksCore/Http.h"
#include "KrtuworksCore/Json.h"
#include "KrtuworksCore/Store.h"

namespace krtu
{
/// One environment's sign-in clients in `krtuworks-config.json`.
struct KRTU_API EnvironmentClients
{
    /// The native sign-in client (desktop, mobile, Steam): `<slug>.<env>.native`.
    std::optional<std::string> clientId;
    /// The console sign-in client (device flow): `<slug>.<env>.device`.
    std::optional<std::string> deviceClientId;
    std::optional<std::string> webClientId;
};

/// `krtuworks-config.json`, downloaded from the app's "Credentials" page: nothing secret, only who the app
/// is, what kind of app it is and its players' sign-in clients per environment.
struct KRTU_API AppConfig
{
    std::string appId;
    std::string slug;
    /// GAME, MOBILE_APP, WEB_APP or DESKTOP_APP. The game-only parts (levels, deaths, Steam) stay off in the
    /// other kinds. A file made before the field existed is a game's.
    std::string type = "GAME";
    /// `offline` (default): the first launch plays without a connection. `online`: the very first launch needs the
    /// server (no account yet and nothing kept from it); without it Gate::firstLaunchOffline is set.
    std::string firstLaunch = "offline";
    std::map<std::string, EnvironmentClients> environments;

    /// Reads the file's text; std::invalid_argument when it is not a krtuworks-config.json.
    static AppConfig fromJson(std::string_view text);
};

/// What a call of the game means at the moment it is made: who plays and what time it is.
struct KRTU_API CallMoment
{
    std::optional<std::string> subject;
    std::string createdAt;
    std::int64_t monotonicMs = 0;
};

/// What the session tells about the device; only the known fields are sent.
struct KRTU_API DeviceInfo
{
    std::optional<std::string> os;
    std::optional<std::string> osVersion;
    std::optional<std::string> model;
    std::optional<std::string> gpu;
    std::optional<int> ramMb;
    std::optional<int> screenWidth;
    std::optional<int> screenHeight;

    Json toJson() const;
};

/// What reading the player's own row of a platform leaderboard found.
enum class PlatformScoreRead
{
    /// The platform could not tell (no answer, the board is not published there): ask again next launch.
    failed,
    /// The platform has no row of this player on the board.
    absent,
    /// The platform has the player's row.
    found,
};

/// The platform's stats, the optional half of what a platform gives the SDK: the server is the record and the
/// SDK sets the platform to the server's values (Steam's user stats). Called from the SDK's threads.
class KRTU_API PlatformStats
{
public:
    virtual ~PlatformStats() = default;
    /// The stat's value on the platform (`type` is `INT` or `FLOAT`); nothing when the platform does not hold it.
    virtual std::optional<double> getStat(const std::string& platformId, const std::string& type) = 0;
    /// Sets the stat on the platform (the value is the platform's already: an INT within 32 bits, a FLOAT in
    /// single precision). False when the platform refuses it (not published there, another type).
    virtual bool setStat(const std::string& platformId, const std::string& type, double value) = 0;
    /// Sends the stats set locally to the platform's servers (Steam's StoreStats).
    virtual void storeStats() = 0;
    /// The player's own row of the named leaderboard on the platform, read once to bring a score the player made
    /// there into the server. Only ever read: the SDK never writes a score to the platform (the server does).
    virtual PlatformScoreRead readOwnScore(const std::string& platformName, std::int64_t& score) = 0;
};

/// A file in the platform's cloud.
struct KRTU_API PlatformCloudFile
{
    std::string data;
    /// When it was last written, Unix seconds; 0 when the platform does not tell.
    std::int64_t writtenAt = 0;
};

/// The platform's per-player cloud (Steam Cloud), the optional half of what a platform gives the SDK: the SDK
/// mirrors the app's mirrored slots there (the server is the record; only a slot's current version is on the
/// platform). Called from the SDK's threads.
class KRTU_API PlatformCloud
{
public:
    virtual ~PlatformCloud() = default;
    /// Whether the player and the game have the platform's cloud on (Steam: the account's and the app's setting).
    virtual bool enabled() = 0;
    /// The file of that name in the platform's cloud, or nothing when there is none. A file that is there but
    /// could not be read throws a retryable Error (`platform_read_failed`): it never reads as absent.
    virtual std::optional<PlatformCloudFile> readFile(const std::string& name) = 0;
    /// Writes the file; false when the platform refused it (no room, an error).
    virtual bool writeFile(const std::string& name, const std::string& data) = 0;
    /// The bytes still free for this player and game in the platform's cloud; nothing when the platform does not tell.
    virtual std::optional<std::int64_t> availableBytes() = 0;
};

/// The platform client's own answer to "does the player own this product", the optional half of what a platform gives
/// the SDK (Steam's `BIsSubscribedApp` for a DLC's AppID). The server's answer comes first; this one stands in when the
/// server did not check in this launch. Called from the SDK's threads.
class KRTU_API PlatformOwnership
{
public:
    virtual ~PlatformOwnership() = default;
    /// True or false as the platform client says; nothing when it cannot tell (the platform client is not running).
    virtual std::optional<bool> ownsProduct(const std::string& platformProductId) = 0;
};

/// One licence of the player in this app: the game itself (sku `base`) or one of its DLCs (the DLC's product key).
struct KRTU_API Entitlement
{
    std::string sku;
    std::string source;
    /// `ACTIVE`, `REVOKED` (owned before, refunded or the sharing ended), or empty: the server never saw a licence
    /// (a DLC the player never owned).
    std::string status;
    /// The platform's id the licence is read for (a Steam AppID); empty when the server has none.
    std::string platformProductId;
    /// The server checked it with the platform in this launch (the session's answer).
    bool checked = false;
};

/// The platform's rich presence, the optional half of what a platform gives the SDK: the text friends see about
/// what the player is doing (Steam's rich presence keys). Called from the SDK's threads, one write at a time.
class KRTU_API PlatformPresence
{
public:
    virtual ~PlatformPresence() = default;
    /// Sets one key; false when the platform refuses it (no such service, a key or value past the platform's limits).
    virtual bool setRichPresence(const std::string& key, const std::string& value) = 0;
    /// Removes every key the SDK set.
    virtual void clearRichPresence() = 0;
};

/// What Client::setPresence did. `reason` says why `applied` is false: no_platform, not_game, module_disabled,
/// hidden, unknown_state, param_missing, states_unknown or platform_rejected.
struct KRTU_API PresenceResult
{
    bool applied = false;
    std::string reason;
};

/// What a platform gives the SDK. The Steam module implements
/// it; a platform without one passes nothing. Called from the SDK's threads.
class KRTU_API PlatformServices
{
public:
    virtual ~PlatformServices() = default;
    /// The platform's rich presence when it has one, else null (the default); same RTTI-free stand-in as stats().
    virtual PlatformPresence* presence() { return nullptr; }
    /// The platform's stats when it has them, else null (the default). The core is built without RTTI, so
    /// this accessor stands where the Unity package asks whether the services also implement IPlatformStats.
    /// The returned object lives as long as these services.
    virtual PlatformStats* stats() { return nullptr; }
    /// The platform's cloud when it has one, else null (the default); same RTTI-free stand-in as stats().
    virtual PlatformCloud* cloud() { return nullptr; }
    /// The platform client's own ownership answer when it has one, else null (the default); same RTTI-free stand-in as stats().
    virtual PlatformOwnership* ownership() { return nullptr; }
    /// A fresh web API ticket for the `krtuworks-id` identity (Steam's GetAuthTicketForWebApi, hex), or nothing.
    virtual std::optional<std::string> webApiTicket() = 0;
    /// A ticket webApiTicket() gave that was never sent to the server (the call failed before it left): it stops being
    /// valid. A ticket that was sent is never cancelled here. Nothing by default.
    virtual void cancelWebApiTicket(const std::string& ticket) { (void)ticket; }
    /// The platform's ids of the achievements the player has unlocked there.
    virtual std::vector<std::string> unlockedAchievements() = 0;
    /// Unlocks an achievement on the platform by its platform id.
    virtual void unlockAchievement(const std::string& platformId) = 0;
    /// Takes an achievement back on the platform by its platform id (support took it back here).
    virtual void clearAchievement(const std::string& platformId) = 0;
};

struct KRTU_API Options
{
    AppConfig config;
    /// TEST or LIVE.
    std::string environment = "LIVE";
    /// STEAM, EPIC, XBOX, PLAYSTATION, NINTENDO, IOS, ANDROID, WEB, WINDOWS, MACOS or LINUX.
    std::string platform = "WINDOWS";
    /// The app's own version label (`1.4.2+381`), sent with every call.
    std::string build;
    std::optional<std::string> clientId;
    /// Queues, the remote config, meta; a store in memory when none is given.
    std::shared_ptr<KeyValueStore> store;
    /// The platform's secret storage (tokens and the device key). Required.
    std::shared_ptr<KeyValueStore> secrets;
    /// Required: the core makes no network connection of its own.
    std::shared_ptr<HttpTransport> http;
    std::shared_ptr<PlatformServices> platformServices;
    std::optional<DeviceInfo> device;
    /// The player's language tag (`tr`, `pt-BR`).
    std::optional<std::string> locale;
    /// Opens a session and keeps it alive.
    bool sessions = true;
    /// How often Client::samplePosition tells where the player is, in whole seconds (1-600).
    int positionSampleSeconds = 10;
    std::string idOrigin = "https://id.krtuworks.com";
    std::string devOrigin = "https://dev.krtuworks.com";
    /// The background work (heartbeats, sending every 30 s, the remote config every 5 min) on a thread of the
    /// SDK's own; off, the owner calls Client::tick() about once a second (tests do).
    bool backgroundLoop = true;
    /// How the SDK waits (polling a console's code, a save still being written); by default it sleeps in short
    /// slices and stops when the token is cancelled (throws Cancelled).
    std::function<void(std::chrono::milliseconds, const CancelToken&)> delay;
};

enum class PlayerKind
{
    Guest,
    Account
};

struct KRTU_API Player
{
    PlayerKind kind = PlayerKind::Guest;
    /// The id this app knows the player by: the same on every device, never the Krtuworks ID itself.
    std::string subject;
    std::optional<std::string> username;
    std::optional<std::string> picture;
};

/// signed_in · guest · signed_out · merged · deletion_pending · sanctioned · merging (the server is merging the
/// player's account: calls wait, told once) · merging_ended (the first call that went through after it, told once).
/// `signed_out` carries `reason` `account_deleted` when the server said the account is gone.
struct KRTU_API IdentityChange
{
    std::string type;
    std::optional<Player> player;
    bool promoted = false;
    bool created = false;
    std::optional<std::string> reason;
    std::optional<std::string> previousSubject;
    std::optional<std::string> mergedInto;
};

/// A console's sign-in: show userCode and verificationUri (or a QR of the complete one).
struct KRTU_API DeviceLink
{
    std::string userCode;
    std::string verificationUri;
    std::string verificationUriComplete;
    int expiresIn = 600;
    int interval = 5;
    std::string deviceCode;
};

/// The owner's switch for a build: `TELEMETRY` stops the statistics, `ALL` stops every call (the game shows the message).
struct KRTU_API BuildSwitch
{
    std::string build;
    std::string mode;
    std::string message;
};

struct KRTU_API Gate
{
    bool maintenance = false;
    std::optional<std::string> maintenanceMessage;
    /// The owner's minimum version is newer than this build: the player must update from the store.
    bool updateRequired = false;
    std::optional<BuildSwitch> switchedOff;
    /// `firstLaunch` is `online` in krtuworks-config.json and the very first launch cannot reach the server: the game
    /// shows "connect to play" and tries again (Client::refreshConfig). Cleared by the first answer of the server.
    bool firstLaunchOffline = false;
};

struct KRTU_API StatView
{
    std::string key;
    std::string name;
    std::optional<std::string> description;
    /// SUM, MAX, MIN or LATEST.
    std::string kind;
    std::string valueType;
    std::optional<double> value;
    std::optional<std::string> updatedAt;
    /// The stat's id on each platform (STEAM: the stat's API name).
    std::map<std::string, std::string> platformIds;

    static StatView fromJson(const Json& json);
};

struct KRTU_API AchievementView
{
    std::string key;
    std::string id;
    std::optional<std::string> name;
    std::optional<std::string> description;
    bool hidden = false;
    bool retired = false;
    bool unlocked = false;
    std::optional<std::string> unlockedAt;
    std::map<std::string, std::string> platformIds;
    std::optional<std::string> iconUrl;
    std::optional<std::string> lockedIconUrl;

    static AchievementView fromJson(const Json& json);
};

struct KRTU_API ProgressView
{
    std::optional<std::string> locale;
    std::vector<StatView> stats;
    std::vector<AchievementView> achievements;

    static ProgressView fromJson(const Json& json);
};

/// A stat, achievement or score op the server refused for good (it left the queue).
struct KRTU_API ProgressRefusal
{
    /// `stat`, `unlock` or `score`.
    std::string type;
    std::string key;
    std::string code;
};

struct KRTU_API ProgressFlushResult
{
    int applied = 0;
    std::optional<std::string> stoppedBy;
    /// The stats' values after the ops the server applied.
    std::map<std::string, double> values;
};

struct KRTU_API FlushResult
{
    int sent = 0;
    std::vector<std::pair<std::string, std::string>> rejected;
    /// Why the queue stopped early (network_error, switched_off …); nothing when it emptied.
    std::optional<std::string> stoppedBy;
};

struct KRTU_API BugDropped
{
    std::string reportId;
    std::string code;
    /// Only the screenshot was refused; the report is in.
    bool screenshotOnly = false;
};

struct KRTU_API BugFlushResult
{
    int sent = 0;
    /// Reports or screenshots the server refused for good; they are not sent again.
    std::vector<BugDropped> dropped;
    std::optional<std::string> stoppedBy;
};

struct KRTU_API SendResult
{
    ProgressFlushResult progress;
    BugFlushResult bugs;
    FlushResult events;
};

struct KRTU_API PlatformSyncResult
{
    std::vector<std::string> unlockedHere;
    std::vector<std::string> toPlatform;
    /// Platform ids support took back here; cleared on the platform too.
    std::vector<std::string> toClear;
    /// Keys of the stats the server took in from the platform's values.
    std::vector<std::string> statsImported;
    /// Platform ids of the stats the server named and the platform was set to (stored at once).
    std::vector<std::string> statsToPlatform;
};

/// A leaderboard of the app with the player's own standing.
struct KRTU_API LeaderboardView
{
    std::string key;
    std::string name;
    std::optional<std::string> description;
    /// ASC (the smaller score ranks first) or DESC.
    std::string sortOrder;
    /// NUMERIC, TIME_SECONDS or TIME_MILLISECONDS.
    std::string display;
    /// KEEP_BEST or FORCE_UPDATE, when the server tells.
    std::optional<std::string> uploadMethod;
    /// The platform's own row of the player may be brought in as a score.
    bool importFromPlatform = false;
    /// The board's name on each platform (STEAM).
    std::map<std::string, std::string> platformIds;
    /// The player's score, details (the bytes the game kept with it) and rank; nothing while the player has no
    /// score there or is not on the board.
    std::optional<std::int64_t> score;
    std::optional<std::string> details;
    std::optional<std::int64_t> rank;
    /// Rows on the board.
    std::int64_t entries = 0;

    static LeaderboardView fromJson(const Json& json);
};

/// One row of a leaderboard.
struct KRTU_API LeaderboardEntry
{
    /// Competition rank: equal scores share one.
    std::int64_t rank = 0;
    /// The player's pairwise id in this app; nothing when the server gives none.
    std::optional<std::string> subject;
    /// The player's Krtuworks username; nothing when there is none (show "Player").
    std::optional<std::string> name;
    std::int64_t score = 0;
    std::optional<std::string> details;
    std::string updatedAt;
    /// The row is the signed-in player's.
    bool you = false;

    static LeaderboardEntry fromJson(const Json& json);
};

/// A page of a leaderboard's rows in its order.
struct KRTU_API LeaderboardPage
{
    std::string key;
    /// `global` or `around`.
    std::string scope;
    std::string sortOrder;
    std::string display;
    std::int64_t total = 0;
    std::vector<LeaderboardEntry> entries;

    static LeaderboardPage fromJson(const Json& json);
};

struct KRTU_API WaitingCounts
{
    int events = 0;
    int progress = 0;
    int bugReports = 0;
};

struct KRTU_API Experiment
{
    std::string key;
    std::string configKey;
    std::string group;
    Json value;

    static Experiment fromJson(const Json& json);
};

struct KRTU_API PlayerProfile
{
    Json::Members fields;
    std::optional<std::string> updatedAt;

    static PlayerProfile fromJson(const Json& json);
};

/// Where an event happened: a world position (x, y, z in a scene) or a screen position.
struct KRTU_API EventPosition
{
    /// `world` or `screen`.
    std::string space = "world";
    /// The scene or screen name.
    std::string area;
    double x = 0;
    double y = 0;
    std::optional<double> z;

    Json toJson() const;
    static EventPosition fromJson(const Json& json);
};

/// A level, with its world and part when the game has those layers.
struct KRTU_API LevelRef
{
    std::string level;
    std::optional<std::string> world;
    std::optional<std::string> part;
};

/// A bug report from the player (the categories are a fixed menu).
struct KRTU_API BugReportInput
{
    /// CRASH, GAMEPLAY, GRAPHICS, AUDIO, PERFORMANCE, CONTROLS, TEXT or OTHER.
    std::string category = "OTHER";
    /// What the player wrote, 1-4000 characters.
    std::string description;
    /// A picture of the screen (the bytes of a PNG, JPEG or WebP, at most 5 MB); the player may leave it out.
    std::optional<std::string> screenshot;
    /// Where the player is; the level in play when not given.
    std::optional<std::string> level;
    std::optional<EventPosition> position;
    /// The frame rate now; the last one the performance family told when not given.
    std::optional<double> fps;
};

/// A report as its player sees it: never the notes, the assignee or the labels.
struct KRTU_API BugReportView
{
    std::string id;
    std::string category;
    std::optional<std::string> description;
    std::string platform;
    std::string build;
    std::string createdAt;
    bool screenshot = false;
    /// NEW, INVESTIGATING, RESOLVED, CANNOT_REPRODUCE or DUPLICATE; a duplicate shows its original's.
    std::string status;
    std::optional<std::string> fixedIn;
    std::optional<std::string> fixedAt;
    bool duplicate = false;

    static BugReportView fromJson(const Json& json);
};

struct KRTU_API FileVersion
{
    std::string id;
    std::string slot;
    std::optional<std::string> device;
    std::int64_t generation = 0;
    std::int64_t sizeBytes = 0;
    std::string sha256;
    std::optional<std::string> platform;
    std::optional<std::string> build;
    std::string savedAt;
    bool fromMerge = false;
    /// The platform whose cloud this copy came from (STEAM): it waits as a slot's `conflict` for the player's choice.
    std::optional<std::string> platformCloud;
    /// Set by Client::saveSettings when the write waits for its turn (a slot is written at most every 30 seconds): the
    /// content is kept on the device and sent later, the newest of the slot first; `id` is empty until then.
    bool queued = false;

    static FileVersion fromJson(const Json& json);
};

struct KRTU_API SlotView
{
    std::string key;
    /// SAVE, PROFILE, SETTINGS or DEVICE_SETTINGS.
    std::string kind;
    std::string name;
    bool retired = false;
    /// The SDK copies the slot to the platform's cloud (Steam Cloud) where the game runs on one; never DEVICE_SETTINGS.
    bool platformMirror = false;
    std::optional<FileVersion> file;
    /// A merged account's version or the platform cloud's copy (`platformCloud`), waiting for the player's choice.
    std::optional<FileVersion> conflict;
    std::vector<FileVersion> devices;

    static SlotView fromJson(const Json& json);
};

/// The platform cloud's copy of a SAVE or PROFILE slot differs from the server's and nothing tells which is newer
/// (the first time this device compares them, or both moved): it waits on the server for the player's choice. The
/// game shows both (times, sizes, its own summary of `platformData`) and calls Client::resolveFile with the one to
/// keep and `server.id`; the kept bytes then go to both sides.
struct KRTU_API PlatformSaveConflict
{
    std::string slot;
    /// The slot's current version.
    FileVersion server;
    /// The platform cloud's copy, waiting in the slot (`platformCloud` set).
    FileVersion platform;
    /// The platform copy's bytes when they are still the ones on the platform; nothing otherwise.
    std::optional<std::string> platformData;
};

struct KRTU_API SlotList
{
    std::vector<SlotView> slots;
    std::map<std::string, std::int64_t> limits;

    static SlotList fromJson(const Json& json);
};

struct KRTU_API FileRead
{
    std::string bytes;
    /// A save of this slot started on this device while the read ran: the read is older than what the player has here.
    bool newerLocalSave = false;
    /// The version read: its id is the base of the next save (nothing when it is a similar device's file).
    std::optional<std::string> versionId;
    std::int64_t generation = 0;
    std::string sha256;
    std::string savedAt;
    /// A similar device's DEVICE_SETTINGS: a starting point, not this device's own.
    bool similarDevice = false;
};

/// DEVICE_SETTINGS: this device's id and description, so a new device starts from the closest one's.
struct KRTU_API SaveDevice
{
    std::string id;
    std::map<std::string, std::string> info;
};

struct KRTU_API ResumedUpload
{
    std::string uploadId;
    std::string slot;
    /// saved, refused, waiting, or replaced (a newer save of the slot took its place).
    std::string outcome;
    std::optional<std::string> code;
};

struct KRTU_API WaitingSave
{
    std::string uploadId;
    std::string slot;
    std::int64_t savedAt = 0;
};

struct KRTU_API MergeSettlement
{
    std::string previousSubject;
    /// Saves the merged account left waiting on this device: the player chooses (adoptWaitingSave / discardWaitingSave).
    std::vector<WaitingSave> waitingSaves;
};

/// Whether the player mirrors (stats, achievements) with the platform account the session proved: ACTIVE
/// (mirrored), PENDING_CONSENT (a second Steam account: ask the player, then Client::answerPlatformConsent),
/// REFUSED (the player said no), LIMIT (a third account; a game mirrors with two at most), MISMATCH (another
/// Krtuworks ID mirrors with it here).
struct KRTU_API PlatformLink
{
    std::string platform;
    /// The player's own platform account (SteamID) the session proved.
    std::string platformAccountId;
    std::string state;
};

struct KRTU_API SessionInfo
{
    std::string id;
    /// The player the session is for.
    std::string owner;
    std::string startedAt;
    /// The platform account (Steam) the session proved and whether this player mirrors with it; empty when none was proved.
    std::optional<PlatformLink> platformLink;
};
}  // namespace krtu
