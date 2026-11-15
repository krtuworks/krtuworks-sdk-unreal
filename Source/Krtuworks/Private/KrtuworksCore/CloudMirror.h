#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Files.h"
#include "KrtuworksCore/Types.h"

namespace krtu::detail
{
/// What one slot's comparison calls for.
enum class MirrorStep
{
    /// Both sides hold the same (or neither holds anything).
    Agree,
    /// The server's bytes go to the platform.
    ToPlatform,
    /// The platform's bytes become the server's new version.
    ToServer,
    /// The platform's copy is offered for the player's choice.
    Offer,
};

/// The platform's cloud mirrors the app's mirrored slots (Steam sync, Steam Cloud): the server is the record, the
/// platform holds each slot's current version as `krtuworks-<slot>.bin` (the server `steamCloudFile`), Auto-Cloud
/// stays off for those files. SAVE, PROFILE and SETTINGS are mirrored, DEVICE_SETTINGS never. Per slot three
/// hashes decide (decide()): H the server's, S the platform's, L the last both sides agreed on, on this device,
/// for this player and platform account. A slot the platform has no room for (or refused) is left alone for the
/// session; the game never sees an error and the server's copy is never touched by a platform failure. The
/// syncs run one after another (a choice's sync must not be swallowed by one already running). Any thread.
class CloudMirror
{
public:
    /// `conflict`: the game is told a copy waits for the player's choice (once per waiting copy and launch), after
    /// the sync that found it ended. `problem`: a platform failure the game does not see (code, slot), for the
    /// error events.
    CloudMirror(PlatformCloud& cloud, Files& files, KeyValueStore& store, std::string clientId, std::function<void(const PlatformSaveConflict&)> conflict,
                std::function<void(const std::string& code, const std::string& slot)> problem);

    static std::string fileName(const std::string& slot);

    /// The step for one slot from H (the server's current hash), S (the platform's), L (agreed last on this device)
    /// and, for SETTINGS, whether the platform's copy was written after the server's (the latest wins there, no
    /// question). Nothing means nothing is there.
    static MirrorStep decide(const std::string& kind, const std::optional<std::string>& h, const std::optional<std::string>& s, const std::optional<std::string>& l, bool platformNewer);

    /// Compares every mirrored slot (or only `slot`) and does what each calls for.
    void sync(const std::string& owner, const std::string& account, const std::string& sessionId, const std::optional<std::string>& slot);
    /// After the player chose (resolve): the slot's current version is what both sides hold from now on. The
    /// platform's copy counts as agreed, so the comparison writes the kept version there (or finds it there
    /// already) instead of offering the same copy again.
    void afterChoice(const std::string& owner, const std::string& account, const std::string& sessionId, const std::string& slot);

private:
    using Agreed = std::map<std::string, std::string>;

    std::string agreedKey(const std::string& owner, const std::string& account) const;
    Agreed loadAgreed(const std::string& key);
    void saveAgreed(const std::string& key, const Agreed& agreed);
    void syncOnce(const std::string& owner, const std::string& account, const std::string& sessionId, const std::optional<std::string>& only, std::vector<PlatformSaveConflict>& raised);
    void syncSlot(const SlotView& slot, Agreed& agreed, const std::string& sessionId, std::vector<PlatformSaveConflict>& raised);
    void toPlatform(const std::string& slot, const std::string& h, const std::optional<PlatformCloudFile>& onPlatform, Agreed& agreed);
    void offer(const SlotView& slot, const PlatformCloudFile& onPlatform, const std::string& s, Agreed& agreed, const std::string& sessionId, std::vector<PlatformSaveConflict>& raised);
    void tell(const std::vector<PlatformSaveConflict>& raised) const;

    PlatformCloud& cloud_;
    Files& files_;
    KeyValueStore& store_;
    std::string clientId_;
    std::function<void(const PlatformSaveConflict&)> conflict_;
    std::function<void(const std::string&, const std::string&)> problem_;
    /// Held for a whole sync; the sets below are only touched under it.
    std::mutex mutex_;
    std::set<std::string> leftAlone_;
    std::set<std::string> told_;
};
}  // namespace krtu::detail
