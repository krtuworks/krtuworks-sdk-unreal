#include "Positions.h"

#include <algorithm>

namespace krtu::detail
{
PositionSampler::PositionSampler(int everySeconds) : everySeconds_(everySeconds) {}

std::optional<int> PositionSampler::due(std::int64_t active)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!since_ || active < *since_)
    {
        since_ = active;
        return std::nullopt;
    }
    const auto elapsed = active - *since_;
    if (elapsed > static_cast<std::int64_t>(everySeconds_) * 3)
    {
        since_ = active;
        return std::nullopt;
    }
    if (elapsed < everySeconds_) return std::nullopt;
    since_ = active;
    return static_cast<int>((std::min)(static_cast<std::int64_t>(600), (std::max)(static_cast<std::int64_t>(1), elapsed)));
}

void PositionSampler::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    since_.reset();
}
}  // namespace krtu::detail
