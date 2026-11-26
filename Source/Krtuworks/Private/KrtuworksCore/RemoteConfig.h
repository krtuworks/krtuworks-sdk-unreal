#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "Api.h"

namespace krtu::detail
{
/// The app's remote config, read without a sign-in and revalidated with its ETag. The
/// last good copy is kept in the store, so an offline launch starts with what the owner set last. Same rules
/// as krtuworks-sdk-js/src/remoteConfig.ts.
class RemoteConfig
{
public:
    /// The SDK modules the owner switches per app and environment.
    static const char* const modules[19];

    RemoteConfig(const Resolved& opts, Api& api, KeyValueStore& store);

    void load();
    /// Whether a config from the server is kept on this device (an earlier launch's, or this one's).
    bool hasCopy() const;
    /// Asks the server; false when nothing changed. Throws when it cannot (the kept copy stays).
    bool refresh();
    /// A module the owner switched off makes nothing; a switch never set is on. In an app (not a game) the
    /// game-only modules are off whatever the config says.
    bool moduleOn(const std::string& module) const;
    std::map<std::string, bool> moduleStates() const;
    /// The owner's switch that rules this build, ALL over TELEMETRY.
    std::optional<BuildSwitch> switchForBuild() const;
    /// A value of the app's config; null when the owner set none.
    Json get(const std::string& key) const;
    /// True when `build` is older than `minVersion` (dotted numbers; a build's `+meta` is ignored).
    static bool olderThan(const std::string& build, const std::string& minVersion);

private:
    std::string key() const;

    const Resolved& opts_;
    Api& api_;
    KeyValueStore& store_;
    mutable std::mutex mutex_;
    bool copy_ = false;
    std::optional<std::string> etag_;
    Json value_;
};
}  // namespace krtu::detail
