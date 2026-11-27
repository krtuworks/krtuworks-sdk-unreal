#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <map>
#include <string>

#include "KrtuworksCore/Types.h"

namespace krtu::detail
{
/// A stat's id and type on the platform (from `GET /v1/me/progress`).
struct StatPlatformId
{
    std::string id;
    /// `INT` (an integer stat) or `FLOAT`.
    std::string type;
};

/// The stats mirror on the platform (Steam sync, stats). The server is the record: the platform is set to the
/// server's value as soon as the server has it (never an increase, so nothing counts twice), and the
/// platform's servers get the values at most every 2 minutes, on pause and on exit. A stat the platform refuses
/// (not published there, another type) is left alone for 5 minutes, then tried again (a stat published since is then
/// mirrored); the rounds in between do not ask the platform again. Any thread.
class StatMirror
{
public:
    static constexpr std::int64_t storeEveryMs = 120000;
    static constexpr std::int64_t refusedForMs = 300000;

    StatMirror(PlatformStats& platform, std::function<std::int64_t()> nowMs);

    /// The value the platform holds for the server's: an INT clamped to 32 bits, a FLOAT in single precision.
    static double platformValue(const std::string& type, double value);

    /// What the platform holds for the stat; nothing when it does not (or refused the stat earlier).
    std::optional<double> read(const StatPlatformId& stat);
    /// Sets the platform's stat to the server's value.
    void set(const StatPlatformId& stat, double serverValue);
    /// Sends what was set to the platform's servers when 2 minutes passed since the last time, or now (`force`:
    /// pause, exit, the launch's sync).
    void store(bool force);

private:
    bool isRefused(const std::string& id);

    PlatformStats& platform_;
    std::function<std::int64_t()> nowMs_;
    std::mutex mutex_;
    std::map<std::string, std::int64_t> refused_;
    bool dirty_ = false;
    std::optional<std::int64_t> lastStoreAt_;
};
}  // namespace krtu::detail
