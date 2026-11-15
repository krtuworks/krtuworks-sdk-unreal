#include "StatMirror.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace krtu::detail
{
StatMirror::StatMirror(PlatformStats& platform, std::function<std::int64_t()> nowMs) : platform_(platform), nowMs_(std::move(nowMs))
{
}

double StatMirror::platformValue(const std::string& type, double value)
{
    if (type == "INT")
    {
        return (std::max)(static_cast<double>((std::numeric_limits<std::int32_t>::min)()),
                          (std::min)(static_cast<double>((std::numeric_limits<std::int32_t>::max)()), std::trunc(value)));
    }
    return static_cast<double>(static_cast<float>(value));
}

bool StatMirror::isRefused(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return refused_.count(id) != 0;
}

std::optional<double> StatMirror::read(const StatPlatformId& stat)
{
    if (isRefused(stat.id)) return std::nullopt;
    return platform_.getStat(stat.id, stat.type);
}

void StatMirror::set(const StatPlatformId& stat, double serverValue)
{
    if (isRefused(stat.id)) return;
    const bool took = platform_.setStat(stat.id, stat.type, platformValue(stat.type, serverValue));
    std::lock_guard<std::mutex> lock(mutex_);
    if (took)
        dirty_ = true;
    else
        refused_.insert(stat.id);
}

void StatMirror::store(bool force)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dirty_) return;
        const auto now = nowMs_();
        if (!force && lastStoreAt_ && now - *lastStoreAt_ < storeEveryMs) return;
        dirty_ = false;
        lastStoreAt_ = now;
    }
    platform_.storeStats();
}
}  // namespace krtu::detail
