#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Infrastructure.h"

namespace krtu::detail
{
struct StoredTokens
{
    std::string kind = "guest";
    std::string subject;
    std::optional<std::string> username;
    std::optional<std::string> picture;
    std::string accessToken;
    /// Milliseconds on the server's clock.
    std::int64_t accessExpiresAt = 0;
    std::optional<std::string> refreshToken;
    /// `oidc` (the token endpoint) or `device` (a console: refreshed through the device broker).
    std::string via = "oidc";

    Json toJson() const;
    static std::optional<StoredTokens> fromJson(const Json& json);
};

struct GuestRequest
{
    std::string birthDate;
    std::string acceptedTerms;
};

struct IdentityMeta
{
    std::int64_t epoch = 0;
    std::vector<std::string> waitingOwners;
    std::optional<GuestRequest> guestRequest;

    Json toJson() const;
    static IdentityMeta fromJson(const Json& json);
};

struct PendingSignIn
{
    std::string verifier;
    std::string nonce;
    std::string redirectUri;
    std::int64_t epoch = 0;
    std::int64_t startedAt = 0;

    Json toJson() const;
    static std::optional<PendingSignIn> fromJson(const Json& json);
};

/// The device's own key: a P-256 pair made here and kept in the platform's secret store
///. A guest's account is bound to it; it never leaves the device. `known` once the
/// server has taken it; `subject` the guest it signs in.
struct DeviceKey
{
    std::string d;
    std::string x;
    std::string y;
    std::string thumbprint;
    bool known = false;
    std::optional<std::string> subject;

    static DeviceKey create();
    /// RFC 7638: the required members only, in lexicographic order, no whitespace.
    static std::string thumbprintOf(const std::string& x, const std::string& y);
    /// The compact JWS a device-key call carries: header `{ alg: ES256, typ: krtu-device-key+jwt, jwk }`,
    /// claims `{ nonce, client_id, purpose, key_thumbprint, iat }`. `iat` is the server's clock as far as it
    /// is known: a device whose clock is off still signs inside the server's minute.
    std::string signProof(const std::string& nonce, const std::string& clientId, const std::string& purpose, std::int64_t iat) const;

    Json toJson() const;
    static std::optional<DeviceKey> fromJson(const Json& json);
};

/// An access token together with the player it belongs to, read from the same stored tokens.
struct Credentials
{
    std::string accessToken;
    std::string subject;
};

/// A JWT's claims, unverified: the SDK reads its own tokens, the servers judge them.
Json jwtClaims(const std::string& token);

/// The query of a return address, read from the text (a phone's return has the app's own scheme).
std::vector<std::pair<std::string, std::string>> queryOf(const std::string& url);

/// Who plays: the tokens, the device's guest key, and every way a player arrives or leaves. One token chain
/// per sign-in client; every write of it under one lock, and a refresh writes its answer only over the
/// refresh token it spent. Same rules as the JS SDK (krtuworks-sdk-js/src/identity.ts).
class Identity
{
public:
    static constexpr const char* deviceKeyGrant = "urn:krtuworks:params:oauth:grant-type:device-key";
    static constexpr const char* steamTicketGrant = "urn:krtuworks:params:oauth:grant-type:steam-ticket";
    static constexpr const char* deviceCodeGrant = "urn:ietf:params:oauth:grant-type:device_code";
    static constexpr const char* currentTerms = "TERMS:v1 PRIVACY:v1";

    using Listener = std::function<void(const IdentityChange&)>;
    using Authenticate = std::function<std::string(const std::string& url, const std::string& scheme, const CancelToken& cancel)>;

    Identity(const Resolved& opts, KeyValueStore& store, KeyValueStore& secrets, HttpTransport& http, Clock& clock, PlatformServices* platform);

    /// Listeners hear every change, on the thread that made it, sometimes while the identity is locked: they
    /// must not wait for the SDK.
    void addListener(Listener listener);

    std::optional<Player> load();
    std::optional<Player> player() const;
    /// Bumped whenever the signed-in player changes; an answer made for another one is dropped.
    int generation() const noexcept { return generation_.load(); }

    IdentityMeta meta();
    std::vector<std::string> waitingOwners();
    /// The merge of `subject` is settled: it no longer waits, and its guest key (retired by the server) goes.
    void forgetWaitingOwner(const std::string& subject);

    /// A valid access token, refreshed first when it is about to expire (or `force`). One refresh at a time.
    std::string accessToken(bool force = false);
    /// The access token with the player it belongs to, taken from one copy of the stored tokens under the state
    /// lock: a refresh may hand back another player's tokens (another instance on the same store signed in
    /// meanwhile), and only the pair says whose.
    Credentials credentials(bool force = false);
    /// The server said the player's account went into another one (401 account_merged). Always throws account_merged.
    [[noreturn]] void markMerged(const std::string& previousSubject, const std::optional<std::string>& mergedInto);

    /// The server said the player's account is gone (401 account_deleted): the player signs out (reason
    /// `account_deleted`) and the deleted-account handler drops that owner's queues. Always throws account_deleted.
    [[noreturn]] void markDeleted(const std::string& subject);
    /// Heard when an account is gone, for that owner (the current player or one a queued call was made for).
    void setDeletedHandler(std::function<void(const std::string&)> handler);
    /// The server answered 409 account_merging for the player's call: the game is told once per episode.
    void noteMerging(const std::string& subject);
    /// A call of the player's went through after a merging episode: the game is told once that it ended.
    void noteMergeEnded(const std::string& subject);

    /// A proof by this device's guest key for one of the guest's own calls (deletion, data copy).
    std::string guestProof(const std::string& purpose);
    /// Plays as a guest (or, on Steam, as the Steam account's player). A new guest needs the age screen's
    /// answers (13 or older, the current terms); they are kept and tried again on later launches when the
    /// server could not make the guest now. A sign-in the player starts meanwhile wins.
    Player signInAsGuest(const std::optional<std::string>& birthDate, const std::optional<std::string>& acceptedTerms, bool viaSteam);
    /// `terms_required` names the documents in its text: `Accept the current terms: TERMS:v2 PRIVACY:v1.`
    static std::optional<std::string> termsIn(const std::string& text);

    /// The address to send the player to (RFC 8252: the system browser). A guest playing here is carried
    /// along (`krtu_guest`). Starting a sign-in bumps the epoch.
    std::string beginSignIn(const std::string& redirectUri, bool carryGuest = true);
    /// Finishes a sign-in from the address the browser came back to.
    Player completeSignIn(const std::string& callbackUrl);
    /// The whole desktop sign-in: a loopback listener on 127.0.0.1 (any port, RFC 8252 §7.3), the system
    /// browser opened on the sign-in page, the code exchanged when the browser comes back.
    Player signInWithBrowser(const std::function<void(const std::string&)>& openBrowser, const CancelToken& cancel, bool carryGuest = true);
    /// A phone's return address: the app's own scheme (RFC 8252 §7.1), registered for every app's native client.
    std::string appScheme() const;
    /// The phone sign-in: the platform's authentication session opens the sign-in page and hands back the
    /// address it returned to.
    Player signInWithApp(const Authenticate& authenticate, const CancelToken& cancel, bool carryGuest = true);

    DeviceLink startDeviceLink(const std::optional<std::string>& deviceName);
    /// Waits for the player to approve the code on another device (RFC 8628 polling, slow_down respected).
    Player finishDeviceLink(const DeviceLink& link, const CancelToken& cancel);

    /// Forgets the player on this device (a guest's key stays: signing in as the guest again reaches it).
    void signOut();

private:
    std::string key(const std::string& name) const;
    std::string lockName() const;
    void emit(const IdentityChange& change);
    void saveMeta(const IdentityMeta& meta);
    std::optional<StoredTokens> loadTokens();
    std::optional<DeviceKey> loadDeviceKey();
    void saveDeviceKey(const DeviceKey& deviceKey);
    void adopt(const std::optional<StoredTokens>& next);
    Answer token(FormFields fields, const std::string& path = "/oidc/token", const std::optional<std::string>& clientId = std::nullopt);
    StoredTokens tokensFrom(const Answer& answer, const std::string& kind, const std::optional<StoredTokens>& previous, const std::string& via = "oidc") const;
    /// Tokens another instance wrote for a different player are taken up like a sign-in here: the app and the client are told.
    void adoptFromAnotherInstance(const StoredTokens& stored);
    StoredTokens refresh(const std::string& usedAccess);
    StoredTokens refused(const StoredTokens& stored, const Error& err);
    [[noreturn]] void markMergedLocked(const std::string& previousSubject, const std::optional<std::string>& mergedInto);
    [[noreturn]] void markDeletedLocked(const std::string& subject);
    std::string challenge(const std::string& purpose);
    std::string proofWith(const DeviceKey& deviceKey, const std::string& purpose);
    std::pair<StoredTokens, bool> guestGrant(const std::optional<GuestRequest>& request, const std::optional<std::string>& steamTicket);
    StoredTokens guestAgain(const StoredTokens& stored);
    /// A Steam ticket that was asked for and never sent is cancelled.
    void discardTicket(const std::optional<std::string>& ticket);
    void checkIdToken(const Answer& answer, const std::string& nonce) const;
    static Player playerOf(const StoredTokens& tokens);

    const Resolved& opts_;
    KeyValueStore& store_;
    KeyValueStore& secrets_;
    HttpTransport& http_;
    Clock& clock_;
    PlatformServices* platform_;
    mutable std::mutex stateMutex_;
    std::optional<StoredTokens> tokens_;
    std::atomic<int> generation_{0};
    SingleFlight<StoredTokens> refreshing_;
    std::mutex listenersMutex_;
    std::vector<Listener> listeners_;
    std::function<void(const std::string&)> deletedHandler_;
    std::atomic<bool> merging_{false};
};
}  // namespace krtu::detail
