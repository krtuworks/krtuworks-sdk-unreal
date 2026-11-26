#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

namespace krtu::detail
{
/// Where players linger, the rules of krtuworks-sdk-js/src/positions.ts: the game reports where the player is as
/// often as it likes (every frame is fine); this turns those reports into one sample per interval carrying the
/// active seconds since the one before. Time the game did not report (it stopped sampling for a while, or the app
/// was in the background) belongs to no position and is not told.
class PositionSampler
{
public:
    explicit PositionSampler(int everySeconds);

    /// The seconds a sample told now carries, or nothing when none is due; `active` is the launch's active seconds.
    std::optional<int> due(std::int64_t active);
    /// Starts over: the next report only starts the clock (another player, a new launch).
    void reset();

private:
    const int everySeconds_;
    std::mutex mutex_;
    std::optional<std::int64_t> since_;
};
}  // namespace krtu::detail
