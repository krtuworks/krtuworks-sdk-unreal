#include "Identity.h"

#include <algorithm>
#include <stdexcept>

#include "KrtuworksCore/Crypto.h"
#include "KrtuworksCore/P256.h"
#include "Loopback.h"

namespace krtu::detail
{
namespace
{
constexpr std::int64_t identityRefreshEarlyMs = 60000;
constexpr std::int64_t identityPendingTtlMs = 10 * 60000;

Error identitySignedOut()
{
    return Error("signed_out", "No player is signed in.");
}

std::optional<std::string> identityParam(const std::vector<std::pair<std::string, std::string>>& query, const std::string& name)
{
    for (const auto& entry : query)
    {
        if (entry.first == name) return entry.second;
    }
    return std::nullopt;
}

void identityAddWaiting(IdentityMeta& meta, const std::string& subject)
{
    if (std::find(meta.waitingOwners.begin(), meta.waitingOwners.end(), subject) == meta.waitingOwners.end()) meta.waitingOwners.push_back(subject);
}

std::string identityWhy(const Error& err)
{
    auto description = err.details().getString("description");
    return description ? *description : err.message();
}
}  // namespace

// ── Stored shapes (the same JSON as the Unity package's) ────────────────

Json StoredTokens::toJson() const
{
    auto json = Json::object({{"kind", kind}, {"subject", subject}});
    if (username) json["username"] = *username;
    if (picture) json["picture"] = *picture;
    json["accessToken"] = accessToken;
    json["accessExpiresAt"] = accessExpiresAt;
    if (refreshToken) json["refreshToken"] = *refreshToken;
    json["via"] = via;
    return json;
}

std::optional<StoredTokens> StoredTokens::fromJson(const Json& json)
{
    if (!json.isObject()) return std::nullopt;
    StoredTokens t;
    t.kind = json.getString("kind").value_or("guest");
    t.subject = json.getString("subject").value_or("");
    t.username = json.getString("username");
    t.picture = json.getString("picture");
    t.accessToken = json.getString("accessToken").value_or("");
    t.accessExpiresAt = json.getInt("accessExpiresAt").value_or(0);
    t.refreshToken = json.getString("refreshToken");
    t.via = json.getString("via").value_or("oidc");
    return t;
}

Json IdentityMeta::toJson() const
{
    auto owners = Json::array();
    for (const auto& owner : waitingOwners) owners.push(owner);
    auto json = Json::object({{"epoch", epoch}, {"waitingOwners", owners}});
    if (guestRequest) json["guestRequest"] = Json::object({{"birthDate", guestRequest->birthDate}, {"acceptedTerms", guestRequest->acceptedTerms}});
    return json;
}

IdentityMeta IdentityMeta::fromJson(const Json& json)
{
    IdentityMeta meta;
    meta.epoch = json.getInt("epoch").value_or(0);
    for (const auto& owner : json["waitingOwners"].items())
    {
        if (owner.isString()) meta.waitingOwners.push_back(owner.asString());
    }
    const auto& request = json["guestRequest"];
    if (request.isObject()) meta.guestRequest = GuestRequest{request.getString("birthDate").value_or(""), request.getString("acceptedTerms").value_or("")};
    return meta;
}

Json PendingSignIn::toJson() const
{
    return Json::object({{"verifier", verifier}, {"nonce", nonce}, {"redirectUri", redirectUri}, {"epoch", epoch}, {"startedAt", startedAt}});
}

std::optional<PendingSignIn> PendingSignIn::fromJson(const Json& json)
{
    if (!json.isObject()) return std::nullopt;
    PendingSignIn p;
    p.verifier = json.getString("verifier").value_or("");
    p.nonce = json.getString("nonce").value_or("");
    p.redirectUri = json.getString("redirectUri").value_or("");
    p.epoch = json.getInt("epoch").value_or(0);
    p.startedAt = json.getInt("startedAt").value_or(0);
    return p;
}

DeviceKey DeviceKey::create()
{
    const auto d = p256::newPrivateKey();
    const auto point = p256::publicKey(d);
    DeviceKey k;
    k.d = base64Url(d);
    k.x = base64Url(point.first);
    k.y = base64Url(point.second);
    k.thumbprint = thumbprintOf(k.x, k.y);
    return k;
}

std::string DeviceKey::thumbprintOf(const std::string& xCoordinate, const std::string& yCoordinate)
{
    return base64Url(sha256("{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"" + xCoordinate + "\",\"y\":\"" + yCoordinate + "\"}"));
}

std::string DeviceKey::signProof(const std::string& nonce, const std::string& clientId, const std::string& purpose, std::int64_t iat) const
{
    const auto header = Json::object({{"alg", "ES256"}, {"typ", "krtu-device-key+jwt"}, {"jwk", Json::object({{"kty", "EC"}, {"crv", "P-256"}, {"x", x}, {"y", y}})}});
    const auto claims = Json::object({{"nonce", nonce}, {"client_id", clientId}, {"purpose", purpose}, {"key_thumbprint", thumbprint}, {"iat", iat}});
    const auto input = base64Url(header.dump()) + "." + base64Url(claims.dump());
    return input + "." + base64Url(p256::sign(fromBase64Url(d), input));
}

Json DeviceKey::toJson() const
{
    auto json = Json::object({{"d", d}, {"x", x}, {"y", y}, {"thumbprint", thumbprint}, {"known", known}});
    if (subject) json["subject"] = *subject;
    return json;
}

std::optional<DeviceKey> DeviceKey::fromJson(const Json& json)
{
    if (!json.isObject()) return std::nullopt;
    DeviceKey k;
    k.d = json.getString("d").value_or("");
    k.x = json.getString("x").value_or("");
    k.y = json.getString("y").value_or("");
    k.thumbprint = json.getString("thumbprint").value_or("");
    k.known = json.getBool("known").value_or(false);
    k.subject = json.getString("subject");
    if (k.d.empty() || k.x.empty() || k.y.empty()) return std::nullopt;
    return k;
}

Json jwtClaims(const std::string& token)
{
    const auto first = token.find('.');
    const auto second = first == std::string::npos ? std::string::npos : token.find('.', first + 1);
    if (first == std::string::npos || second == std::string::npos || token.find('.', second + 1) != std::string::npos) throw Error("invalid_response", "Not a JWT.");
    std::optional<Json> claims;
    try
    {
        claims = Json::parse(fromBase64Url(std::string_view(token).substr(first + 1, second - first - 1)));
    }
    catch (const std::invalid_argument&)
    {
        throw Error("invalid_response", "Not a JWT.");
    }
    if (!claims || !claims->isObject()) throw Error("invalid_response", "Not a JWT.");
    return *claims;
}

std::vector<std::pair<std::string, std::string>> queryOf(const std::string& url)
{
    std::vector<std::pair<std::string, std::string>> q;
    const auto start = url.find('?');
    if (start == std::string::npos) return q;
    const auto end = url.find('#', start);
    const auto query = url.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
    std::size_t at = 0;
    while (at <= query.size())
    {
        auto amp = query.find('&', at);
        if (amp == std::string::npos) amp = query.size();
        const auto part = query.substr(at, amp - at);
        if (!part.empty())
        {
            const auto eq = part.find('=');
            auto k = urlDecode(eq == std::string::npos ? part : part.substr(0, eq), true);
            auto v = eq == std::string::npos ? std::string() : urlDecode(part.substr(eq + 1), true);
            bool replaced = false;
            for (auto& entry : q)
            {
                if (entry.first == k)
                {
                    entry.second = v;
                    replaced = true;
                }
            }
            if (!replaced) q.emplace_back(std::move(k), std::move(v));
        }
        at = amp + 1;
    }
    return q;
}

// ── Identity ────────────────────────────────────────────────────────────

Identity::Identity(const Resolved& opts, KeyValueStore& store, KeyValueStore& secrets, HttpTransport& http, Clock& clock, PlatformServices* platform)
    : opts_(opts), store_(store), secrets_(secrets), http_(http), clock_(clock), platform_(platform)
{
}

void Identity::addListener(Listener listener)
{
    std::lock_guard<std::mutex> lock(listenersMutex_);
    listeners_.push_back(std::move(listener));
}

std::string Identity::key(const std::string& name) const
{
    return "krtu:" + opts_.clientId + ":" + name;
}

std::string Identity::lockName() const
{
    return "krtu:" + opts_.clientId + ":identity";
}

Player Identity::playerOf(const StoredTokens& tokens)
{
    Player p;
    p.kind = tokens.kind == "account" ? PlayerKind::Account : PlayerKind::Guest;
    p.subject = tokens.subject;
    p.username = tokens.username;
    p.picture = tokens.picture;
    return p;
}

std::optional<Player> Identity::load()
{
    const auto loaded = loadTokens();
    std::lock_guard<std::mutex> lock(stateMutex_);
    tokens_ = loaded;
    if (!tokens_) return std::nullopt;
    return playerOf(*tokens_);
}

std::optional<Player> Identity::player() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!tokens_) return std::nullopt;
    return playerOf(*tokens_);
}

void Identity::emit(const IdentityChange& change)
{
    std::vector<Listener> listeners;
    {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        listeners = listeners_;
    }
    for (const auto& listener : listeners)
    {
        try
        {
            listener(change);
        }
        catch (...)
        {
            // A listener's failure is the app's.
        }
    }
}

IdentityMeta Identity::meta()
{
    const auto json = getJson(store_, key("meta"));
    return json ? IdentityMeta::fromJson(*json) : IdentityMeta();
}

void Identity::saveMeta(const IdentityMeta& value)
{
    putJson(store_, key("meta"), value.toJson());
}

std::vector<std::string> Identity::waitingOwners()
{
    return meta().waitingOwners;
}

void Identity::forgetWaitingOwner(const std::string& subject)
{
    auto lock = NamedLocks::take(lockName());
    auto current = meta();
    current.waitingOwners.erase(std::remove(current.waitingOwners.begin(), current.waitingOwners.end(), subject), current.waitingOwners.end());
    saveMeta(current);
    const auto deviceKey = loadDeviceKey();
    if (deviceKey && deviceKey->subject == subject) secrets_.remove(key("deviceKey"));
}

std::optional<StoredTokens> Identity::loadTokens()
{
    const auto json = getJson(secrets_, key("tokens"));
    return json ? StoredTokens::fromJson(*json) : std::nullopt;
}

std::optional<DeviceKey> Identity::loadDeviceKey()
{
    const auto json = getJson(secrets_, key("deviceKey"));
    return json ? DeviceKey::fromJson(*json) : std::nullopt;
}

void Identity::saveDeviceKey(const DeviceKey& deviceKey)
{
    putJson(secrets_, key("deviceKey"), deviceKey.toJson());
}

void Identity::adopt(const std::optional<StoredTokens>& next)
{
    if (next) putJson(secrets_, key("tokens"), next->toJson());
    else secrets_.remove(key("tokens"));
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto before = tokens_ ? std::optional<std::string>(tokens_->subject) : std::nullopt;
    const auto after = next ? std::optional<std::string>(next->subject) : std::nullopt;
    if (before != after) ++generation_;
    tokens_ = next;
}

// ── Tokens ──────────────────────────────────────────────────────────────

std::string Identity::accessToken(bool force)
{
    return credentials(force).accessToken;
}

Credentials Identity::credentials(bool force)
{
    StoredTokens t;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!tokens_) throw identitySignedOut();
        t = *tokens_;
    }
    if (!force && t.accessExpiresAt - identityRefreshEarlyMs > clock_.serverNowMs()) return Credentials{t.accessToken, t.subject};
    const auto used = t.accessToken;
    const auto next = refreshing_.run([this, &used] { return refresh(used); });
    return Credentials{next.accessToken, next.subject};
}

Answer Identity::token(FormFields fields, const std::string& path, const std::optional<std::string>& clientId)
{
    fields.emplace_back("client_id", clientId ? *clientId : opts_.clientId);
    return wire::send(http_, clock_, wire::form(opts_.idOrigin + path, fields));
}

StoredTokens Identity::tokensFrom(const Answer& answer, const std::string& kind, const std::optional<StoredTokens>& previous, const std::string& via) const
{
    const auto access = answer.body().getString("access_token");
    if (!access || access->empty()) throw Error("invalid_response", "The token answer carries no access token.");
    const auto claims = jwtClaims(*access);
    const auto idToken = answer.body().getString("id_token");
    const auto id = idToken ? std::optional<Json>(jwtClaims(*idToken)) : std::nullopt;
    auto subject = claims.getString("sub");
    if (!subject && id) subject = id->getString("sub");
    if (!subject || subject->empty()) throw Error("invalid_response", "The access token names no player.");
    const bool same = previous && previous->subject == *subject;
    StoredTokens t;
    t.kind = kind;
    t.subject = *subject;
    t.username = id ? id->getString("preferred_username") : std::nullopt;
    if (!t.username && same) t.username = previous->username;
    t.picture = id ? id->getString("picture") : std::nullopt;
    if (!t.picture && same) t.picture = previous->picture;
    t.accessToken = *access;
    const auto exp = claims.getInt("exp");
    t.accessExpiresAt = exp ? *exp * 1000 : clock_.serverNowMs() + 600000;
    t.refreshToken = answer.body().getString("refresh_token");
    if (!t.refreshToken && same) t.refreshToken = previous->refreshToken;
    t.via = via;
    return t;
}

StoredTokens Identity::refresh(const std::string& usedAccess)
{
    auto lock = NamedLocks::take(lockName());
    const auto stored = loadTokens();
    if (!stored)
    {
        adopt(std::nullopt);
        throw identitySignedOut();
    }
    if (stored->accessToken != usedAccess && stored->accessExpiresAt - identityRefreshEarlyMs > clock_.serverNowMs())
    {
        // Another instance on the same store refreshed already: its answer is used, the token is not spent twice.
        adopt(stored);
        return *stored;
    }
    if (!stored->refreshToken)
    {
        if (stored->kind == "guest") return guestAgain(*stored);
        adopt(std::nullopt);
        IdentityChange change;
        change.type = "signed_out";
        change.reason = "expired";
        emit(change);
        throw identitySignedOut();
    }
    FormFields fields{{"grant_type", std::string("refresh_token")}, {"refresh_token", stored->refreshToken}};
    Answer answer;
    if (stored->via == "device")
    {
        answer = token(fields, "/v1/device/token", opts_.deviceClientId);
    }
    else
    {
        fields.emplace_back("resource", std::string(Resolved::devResource));
        answer = token(fields);
    }
    if (answer.status == 200)
    {
        auto next = tokensFrom(answer, stored->kind, stored, stored->via);
        adopt(next);
        return next;
    }
    return refused(*stored, wire::errorOf(answer));
}

StoredTokens Identity::refused(const StoredTokens& stored, const Error& err)
{
    if (err.code() != "invalid_grant") throw err;
    const auto why = identityWhy(err);
    if (contains(why, "account_merged")) markMergedLocked(stored.subject, std::nullopt);
    // A frozen app keeps its grants: the same refresh token works again once it is resumed.
    if (contains(why, "app_suspended")) throw err;
    adopt(std::nullopt);
    if (contains(why, "sanctioned"))
    {
        emit(IdentityChange{"sanctioned", std::nullopt, false, false, std::nullopt, std::nullopt, std::nullopt});
        throw Error("sanctioned", "The player is banned from this app.", 403);
    }
    if (contains(why, "account_deletion_pending"))
    {
        emit(IdentityChange{"deletion_pending", std::nullopt, false, false, std::nullopt, std::nullopt, std::nullopt});
        throw Error("account_deletion_pending", "This account is waiting to be deleted.", 403);
    }
    if (stored.kind == "guest") return guestAgain(stored);
    emit(IdentityChange{"signed_out", std::nullopt, false, false, why, std::nullopt, std::nullopt});
    throw identitySignedOut();
}

void Identity::markMergedLocked(const std::string& previousSubject, const std::optional<std::string>& mergedInto)
{
    auto current = meta();
    identityAddWaiting(current, previousSubject);
    saveMeta(current);
    adopt(std::nullopt);
    emit(IdentityChange{"merged", std::nullopt, false, false, std::nullopt, previousSubject, mergedInto});
    throw Error("account_merged", "This account was merged into another one; sign in with it.", 401, std::nullopt, Json::object({{"mergedInto", orNull(mergedInto)}}));
}

void Identity::markMerged(const std::string& previousSubject, const std::optional<std::string>& mergedInto)
{
    auto lock = NamedLocks::take(lockName());
    markMergedLocked(previousSubject, mergedInto);
}

// ── Guests (device key, Steam ticket) ───────────────────────────────────

std::string Identity::challenge(const std::string& purpose)
{
    const auto answer = wire::send(http_, clock_, wire::form(opts_.idOrigin + "/oidc/device-key/challenge", {{"client_id", opts_.clientId}, {"purpose", purpose}}));
    const auto nonce = answer.body().getString("nonce");
    if (answer.status != 200 || !nonce) throw wire::errorOf(answer);
    return *nonce;
}

std::string Identity::proofWith(const DeviceKey& deviceKey, const std::string& purpose)
{
    const auto nonce = challenge(purpose);
    return deviceKey.signProof(nonce, opts_.clientId, purpose, clock_.serverNowMs() / 1000);
}

std::string Identity::guestProof(const std::string& purpose)
{
    const auto deviceKey = loadDeviceKey();
    if (!deviceKey || !deviceKey->known) throw Error("no_guest_key", "This device holds no guest key.");
    return proofWith(*deviceKey, purpose);
}

std::pair<StoredTokens, bool> Identity::guestGrant(const std::optional<GuestRequest>& request, const std::optional<std::string>& steamTicket)
{
    auto deviceKey = loadDeviceKey();
    if (!deviceKey)
    {
        // Stored before the server hears of it: a lost answer must not leave a guest no key can reach.
        deviceKey = DeviceKey::create();
        saveDeviceKey(*deviceKey);
    }
    if (!deviceKey->known && !request) throw Error("birth_date_required", "A new guest needs the age screen: pass the birth date and the accepted terms.", 400);
    const auto assertion = proofWith(*deviceKey, "login");
    FormFields fields{
        {"grant_type", std::string(steamTicket ? Identity::steamTicketGrant : Identity::deviceKeyGrant)},
        {"assertion", assertion},
        {"scope", std::string(Resolved::guestScope)},
        {"resource", std::string(Resolved::devResource)},
        {"ticket", steamTicket},
        {"birth_date", deviceKey->known ? std::nullopt : std::optional<std::string>(request->birthDate)},
        {"accepted_terms", deviceKey->known ? std::nullopt : std::optional<std::string>(request->acceptedTerms)},
    };
    const auto answer = token(fields);
    if (answer.status != 200)
    {
        auto err = wire::errorOf(answer);
        if (err.code() == "terms_required") err.details()["terms"] = orNull(termsIn(err.message()));
        throw err;
    }
    // A Steam ticket of a Steam-linked Krtuworks ID signs in as that account (it has a username).
    const auto idToken = answer.body().getString("id_token");
    const auto id = idToken ? std::optional<Json>(jwtClaims(*idToken)) : std::nullopt;
    const std::string kind = steamTicket && id && id->getString("preferred_username") ? "account" : "guest";
    const bool created = !deviceKey->known;
    auto signedIn = tokensFrom(answer, kind, std::nullopt);
    if (created || (kind == "guest" && deviceKey->subject != signedIn.subject))
    {
        deviceKey->known = true;
        if (kind == "guest") deviceKey->subject = signedIn.subject;
        saveDeviceKey(*deviceKey);
    }
    return {signedIn, created};
}

std::optional<std::string> Identity::termsIn(const std::string& text)
{
    // `\b[A-Z][A-Z_]*:v\d+\b`, each document once, in order.
    std::vector<std::string> named;
    const auto word = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; };
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (!(text[i] >= 'A' && text[i] <= 'Z') || (i > 0 && word(text[i - 1]))) continue;
        std::size_t j = i + 1;
        while (j < text.size() && ((text[j] >= 'A' && text[j] <= 'Z') || text[j] == '_')) ++j;
        if (text.compare(j, 2, ":v") != 0) continue;
        std::size_t k = j + 2;
        while (k < text.size() && text[k] >= '0' && text[k] <= '9') ++k;
        if (k == j + 2 || (k < text.size() && word(text[k]))) continue;
        auto document = text.substr(i, k - i);
        if (std::find(named.begin(), named.end(), document) == named.end()) named.push_back(document);
        i = k - 1;
    }
    if (named.empty()) return std::nullopt;
    std::string joined;
    for (const auto& document : named) joined += (joined.empty() ? "" : " ") + document;
    return joined;
}

StoredTokens Identity::guestAgain(const StoredTokens& stored)
{
    try
    {
        auto signedIn = guestGrant(std::nullopt, std::nullopt).first;
        adopt(signedIn);
        return signedIn;
    }
    catch (const Error& e)
    {
        if (e.code() != "invalid_grant" && e.code() != "birth_date_required") throw;
        const auto why = identityWhy(e);
        if (contains(why, "account_merged")) markMergedLocked(stored.subject, std::nullopt);
        adopt(std::nullopt);
        if (contains(why, "account_deletion_pending"))
        {
            emit(IdentityChange{"deletion_pending", std::nullopt, false, false, std::nullopt, std::nullopt, std::nullopt});
            throw Error("account_deletion_pending", "This account is waiting to be deleted.", 403);
        }
        if (contains(why, "sanctioned"))
        {
            emit(IdentityChange{"sanctioned", std::nullopt, false, false, std::nullopt, std::nullopt, std::nullopt});
            throw Error("sanctioned", "The player is banned from this app.", 403);
        }
        emit(IdentityChange{"signed_out", std::nullopt, false, false, why, std::nullopt, std::nullopt});
        throw identitySignedOut();
    }
}

Player Identity::signInAsGuest(const std::optional<std::string>& birthDate, const std::optional<std::string>& acceptedTerms, bool viaSteam)
{
    auto before = meta();
    const auto epoch = before.epoch;
    if (birthDate)
    {
        before.guestRequest = GuestRequest{*birthDate, acceptedTerms ? *acceptedTerms : std::string(currentTerms)};
        saveMeta(before);
    }
    Player made;
    bool created = false;
    {
        auto lock = NamedLocks::take(lockName());
        const auto current = loadTokens();
        if (current)
        {
            adopt(current);
            return playerOf(*current);
        }
        const auto request = meta().guestRequest;
        std::optional<std::string> ticket;
        if (viaSteam)
        {
            ticket = platform_ != nullptr ? platform_->webApiTicket() : std::nullopt;
            if (!ticket) throw Error("steam_unavailable", "Steam gave no ticket.");
        }
        std::pair<StoredTokens, bool> result;
        try
        {
            result = guestGrant(request, ticket);
        }
        catch (const Error& e)
        {
            if (!ticket || e.code() != "steam_not_configured") throw;
            result = guestGrant(request, std::nullopt);
        }
        auto now = meta();
        if (now.epoch != epoch) throw Error("context_changed", "A sign-in started while the guest was being made; the guest was not used.");
        now.guestRequest.reset();
        saveMeta(now);
        adopt(result.first);
        made = playerOf(result.first);
        created = result.second;
    }
    emit(IdentityChange{made.kind == PlayerKind::Guest ? "guest" : "signed_in", made, false, created, std::nullopt, std::nullopt, std::nullopt});
    return made;
}

// ── Sign-in with Krtuworks ID: authorization code + PKCE in the system browser ──

std::string Identity::beginSignIn(const std::string& redirectUri, bool carryGuest)
{
    std::int64_t epoch = 0;
    {
        auto lock = NamedLocks::take(lockName());
        auto current = meta();
        ++current.epoch;
        saveMeta(current);
        epoch = current.epoch;
    }
    const auto verifier = randomToken(32);
    const auto state = randomToken(16);
    const auto nonce = randomToken(16);
    std::vector<std::pair<std::string, std::string>> query{
        {"client_id", opts_.clientId},
        {"redirect_uri", redirectUri},
        {"response_type", "code"},
        {"scope", Resolved::playerScope},
        // offline_access only counts with prompt=consent (OIDC Core 11); no screen for the app's own players.
        {"prompt", "consent"},
        {"resource", Resolved::devResource},
        {"code_challenge", base64Url(sha256(verifier))},
        {"code_challenge_method", "S256"},
        {"state", state},
        {"nonce", nonce},
    };
    const auto current = player();
    if (current && current->kind == PlayerKind::Guest && carryGuest)
    {
        const auto deviceKey = loadDeviceKey();
        if (deviceKey && deviceKey->known)
        {
            try
            {
                query.emplace_back("krtu_guest", proofWith(*deviceKey, "promote"));
            }
            catch (const Error&)
            {
                // Offline or refused: the sign-in goes on without the guest.
            }
        }
    }
    putJson(secrets_, key("signin:" + state), PendingSignIn{verifier, nonce, redirectUri, epoch, nowMs()}.toJson());
    std::string url = opts_.idOrigin + "/oidc/auth?";
    for (std::size_t i = 0; i < query.size(); ++i) url += (i == 0 ? "" : "&") + urlEncode(query[i].first) + "=" + urlEncode(query[i].second);
    return url;
}

Player Identity::completeSignIn(const std::string& callbackUrl)
{
    const auto q = queryOf(callbackUrl);
    const auto state = identityParam(q, "state");
    if (!state) throw Error("invalid_callback", "The address carries no sign-in.");
    const auto pendingJson = getJson(secrets_, key("signin:" + *state));
    secrets_.remove(key("signin:" + *state));
    const auto pending = pendingJson ? PendingSignIn::fromJson(*pendingJson) : std::nullopt;
    if (!pending || nowMs() - pending->startedAt > identityPendingTtlMs) throw Error("invalid_callback", "This sign-in is unknown or too old: start it again.");
    if (const auto error = identityParam(q, "error")) throw Error(*error, identityParam(q, "error_description").value_or("The sign-in did not finish."), 400);
    const auto iss = identityParam(q, "iss");
    if (iss && *iss != opts_.issuer) throw Error("invalid_callback", "The answer came from another issuer.");
    const auto code = identityParam(q, "code");
    if (!code) throw Error("invalid_callback", "The address carries no code.");
    const auto answer = token({{"grant_type", std::string("authorization_code")},
                               {"code", code},
                               {"redirect_uri", pending->redirectUri},
                               {"code_verifier", pending->verifier},
                               {"resource", std::string(Resolved::devResource)}});
    if (answer.status != 200) throw wire::errorOf(answer);
    checkIdToken(answer, pending->nonce);
    Player made;
    bool promoted = false;
    {
        auto lock = NamedLocks::take(lockName());
        auto current = meta();
        if (current.epoch != pending->epoch) throw Error("context_changed", "Another sign-in started after this one; this one was not used.");
        const auto before = loadTokens();
        const auto next = tokensFrom(answer, "account", before);
        if (before && before->kind == "guest")
        {
            if (before->subject == next.subject)
            {
                // The guest became this Krtuworks ID: same player, its key retired by the server.
                promoted = true;
                secrets_.remove(key("deviceKey"));
            }
            else
            {
                identityAddWaiting(current, before->subject);
            }
        }
        current.guestRequest.reset();
        saveMeta(current);
        adopt(next);
        made = playerOf(next);
    }
    emit(IdentityChange{"signed_in", made, promoted, false, std::nullopt, std::nullopt, std::nullopt});
    return made;
}

void Identity::checkIdToken(const Answer& answer, const std::string& nonce) const
{
    // OIDC Core 3.1.3.7 for a token straight from the token endpoint over TLS: issuer, audience, nonce, time.
    const auto idToken = answer.body().getString("id_token");
    if (!idToken) throw Error("invalid_response", "The sign-in answer carries no ID token.");
    const auto claims = jwtClaims(*idToken);
    bool audience = false;
    const auto& aud = claims["aud"];
    if (aud.isArray())
    {
        for (const auto& item : aud.items()) audience = audience || (item.isString() && item.asString() == opts_.clientId);
    }
    else
    {
        audience = aud.isString() && aud.asString() == opts_.clientId;
    }
    const auto exp = claims.getInt("exp");
    const bool fresh = exp && *exp * 1000 > clock_.serverNowMs() - 60000;
    if (claims.getString("iss") != opts_.issuer || !audience || claims.getString("nonce") != nonce || !fresh) throw Error("invalid_response", "The ID token is not for this sign-in.");
}

Player Identity::signInWithBrowser(const std::function<void(const std::string&)>& openBrowser, const CancelToken& cancel, bool carryGuest)
{
    LoopbackReceiver receiver;
    const auto url = beginSignIn("http://127.0.0.1:" + std::to_string(receiver.port()) + "/callback", carryGuest);
    openBrowser(url);
    const auto callback = receiver.wait(cancel);
    return completeSignIn(callback);
}

std::string Identity::appScheme() const
{
    return "com.krtuworks." + opts_.slug;
}

Player Identity::signInWithApp(const Authenticate& authenticate, const CancelToken& cancel, bool carryGuest)
{
    const auto scheme = appScheme();
    const auto url = beginSignIn(scheme + ":/callback", carryGuest);
    const auto callback = authenticate(url, scheme, cancel);
    return completeSignIn(callback);
}

// ── A console's sign-in (device flow through the broker) ────────────────

DeviceLink Identity::startDeviceLink(const std::optional<std::string>& deviceName)
{
    if (!opts_.deviceClientId) throw std::logic_error("krtuworks-config.json names no device client for this environment");
    const auto answer = token({{"scope", std::string(Resolved::playerScope)}, {"device_name", deviceName}}, "/v1/device/authorize", opts_.deviceClientId);
    if (answer.status != 200) throw wire::errorOf(answer);
    const auto& j = answer.body();
    DeviceLink link;
    link.deviceCode = j.getString("device_code").value_or("");
    link.userCode = j.getString("user_code").value_or("");
    link.verificationUri = j.getString("verification_uri").value_or("");
    link.verificationUriComplete = j.getString("verification_uri_complete").value_or(link.verificationUri);
    link.expiresIn = static_cast<int>(j.getInt("expires_in").value_or(600));
    link.interval = static_cast<int>(j.getInt("interval").value_or(5));
    if (link.deviceCode.empty() || link.userCode.empty()) throw Error("invalid_response", "The device answer carries no code.");
    return link;
}

Player Identity::finishDeviceLink(const DeviceLink& link, const CancelToken& cancel)
{
    int interval = (std::max)(1, link.interval);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(link.expiresIn);
    while (std::chrono::steady_clock::now() < deadline)
    {
        opts_.delay(std::chrono::milliseconds(static_cast<std::int64_t>(interval) * 1000), cancel);
        const auto answer = token({{"grant_type", std::string(deviceCodeGrant)}, {"device_code", link.deviceCode}}, "/v1/device/token", opts_.deviceClientId);
        if (answer.status == 200)
        {
            Player made;
            {
                auto lock = NamedLocks::take(lockName());
                auto current = meta();
                ++current.epoch;
                current.guestRequest.reset();
                const auto before = loadTokens();
                const auto next = tokensFrom(answer, "account", before, "device");
                if (before && before->kind == "guest" && before->subject != next.subject) identityAddWaiting(current, before->subject);
                saveMeta(current);
                adopt(next);
                made = playerOf(next);
            }
            emit(IdentityChange{"signed_in", made, false, false, std::nullopt, std::nullopt, std::nullopt});
            return made;
        }
        const auto err = wire::errorOf(answer);
        if (err.code() == "authorization_pending") continue;
        if (err.code() == "slow_down")
        {
            interval += 5;
            continue;
        }
        throw err;
    }
    throw Error("expired_token", "The code expired before it was approved.", 400);
}

void Identity::signOut()
{
    std::optional<StoredTokens> stored;
    {
        auto lock = NamedLocks::take(lockName());
        auto current = meta();
        ++current.epoch;
        current.guestRequest.reset();
        saveMeta(current);
        stored = loadTokens();
        adopt(std::nullopt);
    }
    if (stored && stored->refreshToken && stored->via == "oidc")
    {
        try
        {
            token({{"token", stored->refreshToken}, {"token_type_hint", std::string("refresh_token")}}, "/oidc/token/revocation");
        }
        catch (const Error&)
        {
            // Offline: the token expires on its own.
        }
    }
    emit(IdentityChange{"signed_out", std::nullopt, false, false, std::string("signed_out"), std::nullopt, std::nullopt});
}
}  // namespace krtu::detail
