#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Api.h"

namespace krtu::detail
{
/// What the player owns (the server `entitlements`; krtuworks-sdk-js/src/entitlements.ts has the same rules): the game's licence
/// and its DLCs as the server last said, kept per player and environment in the store. ownsDlc reads, in this
/// order: what the server confirmed with the platform in this launch (a session answer item with `checked`), then
/// the platform client's own answer (Steam took the money and is authoritative when the server could not check),
/// then the last answer the server gave, kept from an earlier launch or fetched.
class Entitlements
{
public:
    /// `platform` is the platform client's ownership answer; null when the game has none.
    Entitlements(const Resolved& opts, Api& api, KeyValueStore& store, PlatformOwnership* platform, std::function<std::optional<std::string>()> subject);

    /// Reads the current player's kept list unless this player's is already held (a launch's confirmed answers stay).
    void ensureLoaded();
    /// The player changed or signed out: nothing of the old player stays in memory.
    void forget();
    /// The licences as known now: the server's last answer, with `checked` on what it confirmed in this launch.
    std::vector<Entitlement> list();
    /// The session answer's list: its checked items are this launch's confirmed answers, the whole list the server's last word.
    void fromSession(const Json& items);
    /// `GET /v1/me/entitlements`: the stored licences replace what the kept list says (a DLC it no longer holds stays
    /// as unowned, for its id). Throws when the server cannot be reached.
    std::vector<Entitlement> refresh();
    /// Whether the player owns the DLC with this product key.
    bool ownsDlc(const std::string& key);

private:
    /// The server's items (the `entitlements` of a session answer, the list of `GET /v1/me/entitlements`); anything else in the list is ignored.
    static std::vector<Entitlement> itemsFrom(const Json& list);
    std::string key(const std::string& owner) const;
    /// Keeps `list` as the server's last word (`checked` is not kept: it only means something in the launch that saw it).
    void keep(const std::string& owner, std::vector<Entitlement> list);
    std::vector<Entitlement> listLocked() const;

    const Resolved& opts_;
    Api& api_;
    KeyValueStore& store_;
    PlatformOwnership* platform_;
    std::function<std::optional<std::string>()> subject_;
    std::mutex mutex_;
    std::optional<std::string> owner_;
    std::vector<Entitlement> stored_;
    /// The items the server confirmed with the platform in this launch.
    std::vector<Entitlement> confirmed_;
};
}  // namespace krtu::detail
