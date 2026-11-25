#pragma once

#include "KrtuworksCore/Export.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "KrtuworksCore/Client.h"
#include "KrtuworksCore/Error.h"
#include "KrtuworksCore/Store.h"
#include "KrtuworksCore/Types.h"
#include "KrtuworksCore/UiFormat.h"
#include "KrtuworksCore/UiText.h"

namespace krtu
{
/// The legal pages the age and terms screens link to.
constexpr const char* uiTermsUrl = "https://id.krtuworks.com/legal/terms/latest";
constexpr const char* uiPrivacyUrl = "https://id.krtuworks.com/legal/privacy/latest";
/// The account site the account chip's "Manage account" opens.
constexpr const char* uiAccountUrl = "https://id.krtuworks.com/";

/// What the game can show about a save, read from its own bytes by UiOptions::describeSave.
struct KRTU_API SaveSummary
{
    std::optional<std::string> title;
    std::vector<std::string> lines;
    /// A picture of the save (an https address), shown above the lines.
    std::optional<std::string> image;
};

struct KRTU_API SaveCard
{
    std::string label;
    bool newer = false;
    std::string when;
    std::string size;
    std::optional<SaveSummary> summary;
};

enum class UiScreenKind
{
    Age,
    Terms,
    Underage,
    Conflict,
    Error,
    /// The browser is open on the sign-in page; the player comes back here.
    Waiting,
    /// A console's or TV's sign-in: a code and an address (and a QR of the complete address).
    Device
};

/// Why the age screen is shown again.
enum class AgeProblem
{
    None,
    Invalid,
    Accept
};

/// A screen the ready-made components draw; a game that draws its own gets the same models. Which fields a screen
/// uses is noted per kind; the others stay empty.
struct KRTU_API UiScreen
{
    UiScreenKind kind = UiScreenKind::Error;
    // Age
    DateOrder order = DateOrder::Dmy;
    /// The years the age screen offers, newest first.
    std::vector<int> years;
    AgeProblem problem = AgeProblem::None;
    /// Whether the screen offers "Sign in with Krtuworks ID".
    bool signIn = false;
    // Age and Terms
    std::string termsUrl;
    std::string privacyUrl;
    // Conflict: the slot's name and the two saves, the one this device holds (or the server's) first.
    std::string slot;
    std::array<SaveCard, 2> cards;
    // Error
    std::string message;
    bool retry = false;
    // Waiting: the sign-in address that was opened.
    std::string url;
    // Device: the code, the address to go to, the complete address (with the code) the QR holds, the QR's dark
    // modules (empty when the address does not fit one), the moment the code stops working (milliseconds since
    // 1970 on this device's clock) and whether it already did (only New code and Cancel then).
    std::string code;
    std::string uri;
    std::string uriComplete;
    std::vector<std::vector<bool>> qr;
    std::int64_t expiresAtMs = 0;
    bool expired = false;
};

enum class UiAnswerKind
{
    /// Age: a birth date and whether the terms are accepted.
    Birth,
    Accept,
    SignIn,
    Offline,
    /// Conflict: keep card `card`.
    Keep,
    Retry,
    Continue,
    /// Waiting: open the sign-in address again and keep waiting.
    Reopen,
    /// Waiting and Device: stop signing in.
    Cancel,
    /// Device: ask for a new code.
    NewCode,
    /// The host could not show the screen or it was dismissed; nobody answered.
    Closed
};

struct KRTU_API UiAnswer
{
    UiAnswerKind kind = UiAnswerKind::Closed;
    int year = 0;
    int month = 0;
    int day = 0;
    bool accepted = false;
    int card = 0;
};

enum class UiReminderAnswer
{
    Link,
    Later
};

/// Draws the screens. The controller calls it from the threads its own calls run on, never from a game's main
/// thread; an implementation marshals to the thread that draws. Calls may come from several threads at once.
class KRTU_API UiHost
{
public:
    virtual ~UiHost() = default;
    /// Shows `screen` and blocks until the player answers it, or `dismiss` is cancelled (the answer is then Closed).
    /// When a screen of the same kind is already open it is reused: its content is replaced and no screen closes
    /// and opens again. The screen stays open after the answer until close(): Reopen and NewCode answers keep waiting.
    virtual UiAnswer show(const UiScreen& screen, const CancelToken& dismiss) = 0;
    /// Replaces what the open screen shows while a show() on it is still waiting (a code that expired).
    virtual void update(const UiScreen& screen) = 0;
    /// Closes the open screen; nothing happens when none is open.
    virtual void close() = 0;
    /// The "link a Krtuworks ID" prompt in a corner of the screen; it never stops the game. Blocks until the
    /// player answers it or it times out (Later).
    virtual UiReminderAnswer remind() = 0;
};

/// How the player signs in with Krtuworks ID, chosen by the engine layer from the platform.
enum class UiSignInMode
{
    /// The system browser and a loopback address (desktops).
    Browser,
    /// The phone's own authentication session (iOS, Android).
    App,
    /// A code and an address on another device (consoles, TVs).
    Device,
    /// Signing in is not offered.
    None
};

struct KRTU_API UiOptions
{
    /// The game's own sentences (any subset, any language), over the English ones.
    UiTexts texts;
    /// The device's language tag, for the date order and times.
    std::string locale = "en";
    /// Plays as a guest when nobody signs in; false starts the sign-in instead.
    bool guests = true;
    /// Shows the "link a Krtuworks ID" prompt when remind() is called.
    bool linkReminder = true;
    /// Shows the conflict screen for a copy the platform's cloud (Steam Cloud) holds, see resolvePlatformSaveConflict().
    bool platformSaveConflicts = true;
    UiSignInMode signInMode = UiSignInMode::Browser;
    /// Opens an address in the system browser (required for Browser). Called from the SDK's threads.
    std::function<void(const std::string& url)> openUrl;
    /// The phone's authentication session (required for App), as Client::signInWithApp takes it.
    std::function<std::string(const std::string& url, const std::string& scheme, const CancelToken& cancel)> authenticate;
    /// The name a console's sign-in shows on the approving device.
    std::optional<std::string> deviceName;
    /// Reads a save's bytes into what its card shows (chapter, play time, a picture). May return nothing.
    std::function<std::optional<SaveSummary>(const std::string& slot, const std::string& bytes)> describeSave;
    /// Where the components keep their small values (the day a child turns 13): the SDK's own store. In memory when none is given.
    std::shared_ptr<KeyValueStore> memory;
    /// Milliseconds since 1970 on this device's clock.
    std::function<std::int64_t()> now;
    /// How far this device's local time is ahead of UTC at a moment (default: the system's).
    std::function<int(std::int64_t unixMs)> utcOffsetMinutes;
};

enum class UiStatus
{
    SignedIn,
    /// The player closed the sign-in (signIn() only); the player stays who they were.
    Cancelled,
    /// No online player: the game keeps going on this device.
    Offline
};

enum class UiOfflineReason
{
    None,
    /// Under 13.
    Underage,
    /// No connection, a refusal, a gate, or the player chose to keep playing.
    Unavailable
};

struct KRTU_API UiResult
{
    UiStatus status = UiStatus::Offline;
    std::optional<Player> player;
    UiOfflineReason reason = UiOfflineReason::None;
};

/// Which save the player kept: this device's (`keptThere` false, the version just saved) or the other one's
/// (the version and its bytes, for the game to load).
struct KRTU_API SaveChoice
{
    bool keptThere = false;
    FileVersion version;
    std::string bytes;
};

struct KRTU_API ResolvedSave
{
    std::string slot;
    FileVersion kept;
};

/// The sentence an error screen shows and whether trying again can help.
struct KRTU_API ErrorScreen
{
    std::string message;
    bool retry = true;
};

/// The error screen for a failure the SDK reported (`utcOffsetMinutes` is the device's offset from UTC, for a ban's end date).
KRTU_API ErrorScreen errorScreen(const Error& err, const UiTexts& texts, const std::string& locale, int utcOffsetMinutes = 0);
/// The error screen for a failure that is not the SDK's.
KRTU_API ErrorScreen errorScreen(const UiTexts& texts);

/// The ready-made screens' decisions: which one to show, what it says, what the SDK does with the answer (the
/// rules of the JS package's KrtuworksUi, plus the sign-in modes a desktop, a phone and a console need). Every call
/// blocks until it is done and must not be made from a game's main thread. One screen flow runs at a time: another
/// call waits for it (the reminder is skipped while one runs).
class KRTU_API UiController
{
public:
    /// Throws std::invalid_argument when the mode needs an opener the options do not give.
    UiController(std::shared_ptr<Client> client, std::shared_ptr<UiHost> host, UiOptions options = UiOptions());
    UiController(const UiController&) = delete;
    UiController& operator=(const UiController&) = delete;

    /// Makes sure someone is playing: the signed-in player, the guest on this device, or a new guest after the age
    /// screen. Never throws for a failure: without an online player the result says so and the game keeps going
    /// offline. Never Cancelled.
    UiResult ensurePlayer();

    /// Signs the player in with Krtuworks ID the way the mode says; a guest playing here is carried along.
    /// `cancel` stops it from outside (the result is Cancelled). Offline when signing in is not offered or it failed
    /// (the error screen was shown).
    UiResult signIn(const CancelToken& cancel = CancelToken());
    /// Whether signing in is offered (any mode but None).
    bool signInOffered() const;
    void signOut();

    /// Saves `bytes` to `slot` over `base` (the version the game read); when another device saved it first, shows
    /// both saves and does what the player picks. `name` is the slot's name on the screen.
    SaveChoice save(const std::string& slot, const std::string& bytes, const std::optional<std::string>& base, const std::string& name = "");
    /// Two saves of `slot`: this device's `bytes` and the server's `current`.
    SaveChoice chooseSave(const std::string& slot, const std::string& bytes, const FileVersion& current, const std::string& name = "");
    /// After two accounts were merged: every slot holding two versions asks the player which to keep.
    std::vector<ResolvedSave> resolveMergedSaves();
    /// The copy the platform's cloud holds differs from the server's: the player picks which to keep.
    void resolvePlatformSaveConflict(const PlatformSaveConflict& conflict);

    /// Shows the "link a Krtuworks ID" prompt; Link starts the sign-in. One at a time; nothing for an account.
    void remind();

    /// The game's sentence for `key`, else the English one, with `{name}` filled from `vars`.
    std::string t(const std::string& key, const std::map<std::string, std::string>& vars = {}) const;
    /// A platform's display name (`STEAM` is Steam); the code itself when it has none.
    std::string platformName(const std::string& platform) const;
    const UiTexts& texts() const { return options_.texts; }
    const std::string& locale() const { return options_.locale; }

private:
    struct HostClosed
    {
    };
    struct AgeOutcome;

    std::optional<std::string> playerSubject() const;
    /// The player who opened a save screen is the one who answers it; otherwise their bytes or choice would reach another account.
    void assertSamePlayer(const std::optional<std::string>& owner) const;
    Player signInBrowser(const CancelToken& external);
    Player signInDevice(const CancelToken& external);
    UiResult failOffline(const ErrorScreen& screen);
    AgeOutcome askAge(AgeProblem first);
    UiResult guestFlow();
    std::optional<ErrorScreen> gateProblem() const;
    void rememberUnderage(const std::string& birthDate);
    bool underageNow() const;
    UiScreen deviceScreen(const DeviceLink& link, bool expired) const;
    SaveCard card(const std::string& label, const std::string& savedAt, std::int64_t sizeBytes, const std::string& slot, const std::optional<std::string>& bytes) const;
    UiAnswer once(const UiScreen& screen);
    std::int64_t nowMs() const;
    int offsetAt(std::int64_t unixMs) const;

    std::shared_ptr<Client> client_;
    std::shared_ptr<UiHost> host_;
    UiOptions options_;
    /// One screen flow at a time; a flow that starts another (the reminder's Link, the age screen's sign-in) holds it already.
    std::recursive_mutex flow_;
    std::atomic<bool> reminding_{false};
};
}  // namespace krtu
