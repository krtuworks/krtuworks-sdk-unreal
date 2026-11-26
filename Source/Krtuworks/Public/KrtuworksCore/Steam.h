#pragma once

#include "KrtuworksCore/Export.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "KrtuworksCore/Types.h"

namespace krtu
{
/// The Steam calls the SDK makes, and no more. The Unreal Steam plugin is the real one (Steamworks);
/// the tests give SteamAchievementSync a fake, so the decisions run without Steam.
class KRTU_API SteamBackend
{
public:
    virtual ~SteamBackend() = default;
    /// `SteamUser()` is not null.
    virtual bool userAvailable() = 0;
    /// `SteamUserStats()` is not null.
    virtual bool statsAvailable() = 0;
    /// `GetAuthTicketForWebApi(identity)`: the handle the answer will carry (0 when Steam refused to start one).
    /// The answer arrives later, as a callback the plugin forwards to SteamAchievementSync::onTicketAnswer.
    virtual std::uint32_t requestWebApiTicket(const std::string& identity) = 0;
    /// `CancelAuthTicket(handle)`: a ticket nobody will use stops being valid.
    virtual void cancelAuthTicket(std::uint32_t handle) = 0;
    /// `GetNumAchievements()`.
    virtual std::uint32_t numAchievements() = 0;
    /// `GetAchievementName(index)`; nothing when Steam gives none.
    virtual std::optional<std::string> achievementName(std::uint32_t index) = 0;
    /// `GetAchievement(name, &achieved)`: false when the call itself failed.
    virtual bool getAchievement(const std::string& name, bool& achieved) = 0;
    /// `SetAchievement(name)`: true when Steam took it.
    virtual bool setAchievement(const std::string& name) = 0;
    /// `ClearAchievement(name)`: true when Steam took it.
    virtual bool clearAchievement(const std::string& name) = 0;
    /// `GetStat(name, &value)` for an INT stat: false when Steam has no such INT stat.
    virtual bool getStatInt(const std::string& name, std::int32_t& value) = 0;
    /// `GetStat(name, &value)` for a FLOAT stat: false when Steam has no such FLOAT stat.
    virtual bool getStatFloat(const std::string& name, float& value) = 0;
    /// `SetStat(name, value)` for an INT stat: true when Steam took it.
    virtual bool setStatInt(const std::string& name, std::int32_t value) = 0;
    /// `SetStat(name, value)` for a FLOAT stat: true when Steam took it.
    virtual bool setStatFloat(const std::string& name, float value) = 0;
    /// `StoreStats()`.
    virtual void storeStats() = 0;
    /// The local user's own row of the leaderboard named `name`: `FindLeaderboard(name)`, then
    /// `DownloadLeaderboardEntriesForUsers` for the local user only, waited for like a ticket. Only ever a read:
    /// no leaderboard upload call exists here, because the server writes Steam's boards, never the SDK.
    virtual PlatformScoreRead ownLeaderboardEntry(const std::string& name, std::int32_t& score) = 0;
    /// `SteamRemoteStorage()` is not null.
    virtual bool cloudAvailable() = 0;
    /// `IsCloudEnabledForAccount() && IsCloudEnabledForApp()`.
    virtual bool cloudEnabled() = 0;
    /// `FileExists(name)`.
    virtual bool fileExists(const std::string& name) = 0;
    /// `FileRead(name, ...)` over `GetFileSize(name)` bytes; nothing when the read failed.
    virtual std::optional<std::string> fileRead(const std::string& name) = 0;
    /// `GetFileTimestamp(name)`: Unix seconds.
    virtual std::int64_t fileTimestamp(const std::string& name) = 0;
    /// `FileWrite(name, data, size)`: true when Steam took it.
    virtual bool fileWrite(const std::string& name, const std::string& data) = 0;
    /// `GetQuota(&total, &available)`: false when Steam could not tell.
    virtual bool getQuota(std::uint64_t& total, std::uint64_t& available) = 0;
    /// `SteamApps()->BIsSubscribedApp(appId)`: the Steam client's own answer for a game or a DLC's AppID; nothing
    /// when `SteamApps()` is null.
    virtual std::optional<bool> isSubscribedApp(std::uint32_t appId) = 0;
    /// `SteamFriends()->SetRichPresence(key, value)`: true when Steam took it; false when `SteamFriends()` is null.
    virtual bool setRichPresence(const std::string& key, const std::string& value) = 0;
    /// `SteamFriends()->ClearRichPresence()`; nothing when `SteamFriends()` is null.
    virtual void clearRichPresence() = 0;
};

/// The Steam half of PlatformServices, PlatformStats, PlatformCloud, PlatformPresence and PlatformOwnership over a SteamBackend: the
/// web API ticket for the `krtuworks-id` identity (the Steam guest and every session's licence check), the
/// achievements, the user stats Steam holds, the Steam Cloud files, the rich presence keys and the Steam client's own
/// ownership answer for a DLC.
class KRTU_API SteamAchievementSync : public PlatformServices, public PlatformStats, public PlatformCloud, public PlatformPresence, public PlatformOwnership
{
public:
    /// How long a ticket's answer is waited for.
    static constexpr std::chrono::milliseconds defaultTicketWait{10000};
    static constexpr std::size_t maxPresenceKeyBytes = 64;
    static constexpr std::size_t maxPresenceValueBytes = 256;
    static constexpr std::size_t maxPresenceKeys = 20;

    explicit SteamAchievementSync(std::unique_ptr<SteamBackend> steam, std::chrono::milliseconds ticketWait = defaultTicketWait);

    std::optional<std::string> webApiTicket() override;
    /// Cancels a ticket webApiTicket() gave that never left for the server (a ticket that was sent stays valid).
    void cancelWebApiTicket(const std::string& ticket) override;
    std::vector<std::string> unlockedAchievements() override;
    void unlockAchievement(const std::string& platformId) override;
    void clearAchievement(const std::string& platformId) override;
    PlatformStats* stats() override { return this; }
    PlatformCloud* cloud() override { return this; }
    PlatformPresence* presence() override { return this; }
    PlatformOwnership* ownership() override { return this; }

    std::optional<double> getStat(const std::string& platformId, const std::string& type) override;
    /// The value is the platform's already (an INT within 32 bits, a FLOAT in single precision). A set is not
    /// stored by itself: the SDK stores every 2 minutes (storeStats).
    bool setStat(const std::string& platformId, const std::string& type, double value) override;
    void storeStats() override;
    PlatformScoreRead readOwnScore(const std::string& platformName, std::int64_t& score) override;

    bool enabled() override;
    /// A file Steam lists but could not read throws `platform_read_failed` (retryable) instead of reading as absent:
    /// writing over it could lose another PC's newer save, so the slot waits for the next sync.
    std::optional<PlatformCloudFile> readFile(const std::string& name) override;
    bool writeFile(const std::string& name, const std::string& data) override;
    std::optional<std::int64_t> availableBytes() override;

    /// `BIsSubscribedApp` for the AppID in `platformProductId`; nothing when it is not an AppID or Steam cannot tell.
    std::optional<bool> ownsProduct(const std::string& platformProductId) override;

    /// Steam's limits are kept here: a key of at most 64 bytes, a value of at most 256 and 20 keys at most between
    /// two clears; a write past them is refused (false) without asking Steam.
    bool setRichPresence(const std::string& key, const std::string& value) override;
    void clearRichPresence() override;

    /// Steam's answer to a ticket (`GetTicketForWebApiResponse_t`): the first `length` bytes of `ticket`, or a null
    /// `ticket` when Steam's result was not OK. An answer nobody waits for is dropped. Any thread.
    void onTicketAnswer(std::uint32_t handle, const std::uint8_t* ticket, int length);

private:
    std::unique_ptr<SteamBackend> steam_;
    std::chrono::milliseconds ticketWait_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    /// Tickets asked for and not yet answered; an empty answer is Steam's refusal.
    std::map<std::uint32_t, std::optional<std::string>> waiting_;
    /// The rich presence keys set since the last clear (a key set twice counts once).
    std::set<std::string> presenceKeys_;
    /// The newest tickets given out (hex and Steam's handle), so one that was never sent can be cancelled.
    std::deque<std::pair<std::string, std::uint32_t>> issued_;
};
}  // namespace krtu
