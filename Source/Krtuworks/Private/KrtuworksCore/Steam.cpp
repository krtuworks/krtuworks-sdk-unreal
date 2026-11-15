#include "KrtuworksCore/Steam.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace krtu
{
namespace
{
constexpr const char* ticketIdentity = "krtuworks-id";
}

SteamAchievementSync::SteamAchievementSync(std::unique_ptr<SteamBackend> steam, std::chrono::milliseconds ticketWait)
    : steam_(std::move(steam)), ticketWait_(ticketWait)
{
}

std::optional<std::string> SteamAchievementSync::webApiTicket()
{
    if (!steam_->userAvailable())
    {
        return std::nullopt;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint32_t handle = steam_->requestWebApiTicket(ticketIdentity);
    if (handle == 0)
    {
        return std::nullopt;
    }
    waiting_[handle] = std::nullopt;
    const bool answered = arrived_.wait_for(lock, ticketWait_, [this, handle] { return waiting_[handle].has_value(); });
    std::optional<std::string> ticket = answered ? waiting_[handle] : std::nullopt;
    waiting_.erase(handle);
    if (ticket && !ticket->empty())
    {
        return ticket;
    }
    // A ticket that came too late, was refused or came back empty is never sent: cancel it so it is not left valid.
    lock.unlock();
    steam_->cancelAuthTicket(handle);
    return std::nullopt;
}

void SteamAchievementSync::onTicketAnswer(std::uint32_t handle, const std::uint8_t* ticket, int length)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = waiting_.find(handle);
        if (found == waiting_.end())
        {
            return;
        }
        std::string hex;
        if (ticket != nullptr)
        {
            static const char* digits = "0123456789abcdef";
            hex.reserve(static_cast<std::size_t>(length) * 2);
            for (int index = 0; index < length; ++index)
            {
                hex.push_back(digits[ticket[index] >> 4]);
                hex.push_back(digits[ticket[index] & 15]);
            }
        }
        found->second = hex;
    }
    arrived_.notify_all();
}

std::vector<std::string> SteamAchievementSync::unlockedAchievements()
{
    std::vector<std::string> unlocked;
    if (!steam_->statsAvailable())
    {
        return unlocked;
    }
    const std::uint32_t count = steam_->numAchievements();
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const std::optional<std::string> name = steam_->achievementName(index);
        bool achieved = false;
        if (name && steam_->getAchievement(*name, achieved) && achieved)
        {
            unlocked.push_back(*name);
        }
    }
    return unlocked;
}

void SteamAchievementSync::unlockAchievement(const std::string& platformId)
{
    if (steam_->statsAvailable() && steam_->setAchievement(platformId))
    {
        steam_->storeStats();
    }
}

void SteamAchievementSync::clearAchievement(const std::string& platformId)
{
    if (steam_->statsAvailable() && steam_->clearAchievement(platformId))
    {
        steam_->storeStats();
    }
}

std::optional<double> SteamAchievementSync::getStat(const std::string& platformId, const std::string& type)
{
    if (!steam_->statsAvailable())
    {
        return std::nullopt;
    }
    if (type == "INT")
    {
        std::int32_t whole = 0;
        return steam_->getStatInt(platformId, whole) ? std::optional<double>(static_cast<double>(whole)) : std::nullopt;
    }
    float real = 0;
    return steam_->getStatFloat(platformId, real) ? std::optional<double>(static_cast<double>(real)) : std::nullopt;
}

bool SteamAchievementSync::setStat(const std::string& platformId, const std::string& type, double value)
{
    if (!steam_->statsAvailable())
    {
        return false;
    }
    return type == "INT" ? steam_->setStatInt(platformId, static_cast<std::int32_t>(value)) : steam_->setStatFloat(platformId, static_cast<float>(value));
}

void SteamAchievementSync::storeStats()
{
    if (steam_->statsAvailable())
    {
        steam_->storeStats();
    }
}

PlatformScoreRead SteamAchievementSync::readOwnScore(const std::string& platformName, std::int64_t& score)
{
    if (!steam_->statsAvailable())
    {
        return PlatformScoreRead::failed;
    }
    std::int32_t value = 0;
    const PlatformScoreRead read = steam_->ownLeaderboardEntry(platformName, value);
    if (read == PlatformScoreRead::found)
    {
        score = value;
    }
    return read;
}

bool SteamAchievementSync::enabled()
{
    return steam_->cloudAvailable() && steam_->cloudEnabled();
}

std::optional<PlatformCloudFile> SteamAchievementSync::readFile(const std::string& name)
{
    if (!steam_->cloudAvailable() || !steam_->fileExists(name))
    {
        return std::nullopt;
    }
    std::optional<std::string> data = steam_->fileRead(name);
    if (!data)
    {
        throw Error("platform_read_failed", "Steam Cloud could not read " + name + ".");
    }
    return PlatformCloudFile{std::move(*data), steam_->fileTimestamp(name)};
}

bool SteamAchievementSync::writeFile(const std::string& name, const std::string& data)
{
    return steam_->cloudAvailable() && steam_->fileWrite(name, data);
}

std::optional<std::int64_t> SteamAchievementSync::availableBytes()
{
    std::uint64_t total = 0;
    std::uint64_t available = 0;
    if (!steam_->cloudAvailable() || !steam_->getQuota(total, available))
    {
        return std::nullopt;
    }
    return static_cast<std::int64_t>((std::min)(available, static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())));
}

std::optional<bool> SteamAchievementSync::ownsProduct(const std::string& platformProductId)
{
    // A Steam AppID: 1 to 10 digits that fit 32 bits.
    if (platformProductId.empty() || platformProductId.size() > 10) return std::nullopt;
    std::uint64_t appId = 0;
    for (const char c : platformProductId)
    {
        if (c < '0' || c > '9') return std::nullopt;
        appId = appId * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (appId > (std::numeric_limits<std::uint32_t>::max)()) return std::nullopt;
    return steam_->isSubscribedApp(static_cast<std::uint32_t>(appId));
}

bool SteamAchievementSync::setRichPresence(const std::string& key, const std::string& value)
{
    if (key.empty() || key.size() > maxPresenceKeyBytes || value.size() > maxPresenceValueBytes) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (presenceKeys_.count(key) == 0 && presenceKeys_.size() >= maxPresenceKeys) return false;
    }
    if (!steam_->setRichPresence(key, value)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    presenceKeys_.insert(key);
    return true;
}

void SteamAchievementSync::clearRichPresence()
{
    steam_->clearRichPresence();
    std::lock_guard<std::mutex> lock(mutex_);
    presenceKeys_.clear();
}
}  // namespace krtu
