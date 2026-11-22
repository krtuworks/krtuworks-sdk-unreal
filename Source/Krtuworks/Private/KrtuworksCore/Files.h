#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Api.h"

namespace krtu::detail
{
/// An upload written to the store before it is sent, so a relaunch finishes it with the same id.
struct PendingUpload
{
    std::string uploadId;
    std::string owner;
    std::string slot;
    std::optional<std::string> device;
    std::optional<std::string> base;
    /// Whether the save named a base (If-Match / If-None-Match: *); a resend sends exactly the same.
    bool conditional = false;
    std::string sha256;
    std::string bytes;
    std::optional<std::map<std::string, std::string>> deviceInfo;
    std::int64_t savedAt = 0;
    /// Order of saves made in the same millisecond on this device.
    std::int64_t seq = 0;
    /// Version 2: when it was last sent is in its own key (none = never sent). An older record was sent when it was made.
    bool v2 = false;

    Json toJson() const;
    static std::optional<PendingUpload> fromJson(const Json& json);
};

/// The player's files in the app's slots, the rules of
/// krtuworks-sdk-js/src/files.ts: a save is written to the store first, sent with its upload id as the Idempotency-Key
/// and asked about until it is final; a conflict comes back with the slot's current version for the game's
/// choice screen.
class Files
{
public:
    Files(const Resolved& opts, Api& api, KeyValueStore& store, std::function<std::optional<std::string>()> subject, std::function<int()> generation, std::function<void()> onSaved);

    SlotList list();
    /// A slot's current version; nothing when `ifNoneMatch` (a version id held) is still current.
    std::optional<FileRead> read(const std::string& slot, const std::optional<SaveDevice>& device, const std::optional<std::string>& ifNoneMatch);
    /// Saves a new version of `slot`. Returns the new version once the server activated it; throws
    /// `file_conflict` (`details()["current"]`: the version now in the slot) when another one came first, and
    /// `save_replaced` when a newer save of the slot took its place before it was sent.
    /// `conditional`: whether `baseVersion` is meant (none = the slot must be empty); SETTINGS-like slots need neither.
    FileVersion save(const std::string& slot, const std::string& data, bool conditional, const std::optional<std::string>& baseVersion, const std::optional<SaveDevice>& device);
    /// Saves left unfinished by an earlier launch (an answer lost, a 202, made offline): finishes the signed-in
    /// player's, the newest of each slot (and device) last; an older one never sent is replaced.
    std::vector<ResumedUpload> resume();
    /// Saves of another player waiting on this device (a merged account's): the game asks the player what to keep.
    std::vector<WaitingSave> waitingFor(const std::string& owner);
    /// A save another player left waiting (a merged account's) that the player chose to keep:
    /// saved now as this player's, over `baseVersion` (the slot's version the player saw), with a new upload id.
    FileVersion adoptWaiting(const std::string& uploadId, const std::optional<std::string>& baseVersion);
    /// A waiting save the player chose not to keep.
    void discardWaiting(const std::string& uploadId);
    /// Puts the platform cloud's copy `data` in the slot as a version waiting for the player's choice (over the
    /// slot's version `current`; `sessionId` proves the platform account). Returns the waiting version. Not kept
    /// in the store: an offer lost is made again on the next sync, and the server answers `conflict_pending` for
    /// one already waiting.
    FileVersion offer(const std::string& slot, const std::string& data, const std::string& current, const std::string& sessionId);
    /// After a merge: keep `keep` (the slot's version or the merged one) over `current` (the one the player saw).
    FileVersion resolve(const std::string& slot, const std::string& keep, const std::string& current);

private:
    std::string filesPath() const;
    std::string slotPath(const std::string& slot) const;
    std::string uploadKey(const std::string& uploadId) const;
    std::string uploadPrefix() const;
    int revision(const std::string& revisionKey) const;
    std::string sentKey(const std::string& uploadId) const;
    /// One lane per player, slot and device: its saves go one at a time, oldest first.
    std::string laneLock(const PendingUpload& upload) const;
    void forget(const std::string& uploadId);
    /// When the upload was last sent; nothing when it never was.
    std::optional<std::int64_t> sentAt(const PendingUpload& upload);
    static bool before(const PendingUpload& a, const PendingUpload& b);
    /// The store's uploads of `upload`'s lane, oldest first.
    std::vector<PendingUpload> lane(const PendingUpload& upload);
    SlotList listFor(const std::optional<std::string>& owner);
    /// Sends `target` in its lane's order. An earlier save of the lane that was never sent is replaced by the
    /// newer one; an earlier save that was sent is settled first (asked how it ended, or sent again with its
    /// id), so an older copy never lands after a newer one. `target` itself is replaced when a newer save of
    /// the lane waits behind it, unsent. `tell` hears what became of the earlier ones.
    FileVersion pump(const PendingUpload& target, const std::function<void(const PendingUpload&, const std::string&, const std::optional<std::string>&)>& tell);
    /// An earlier save of a lane that was sent and whose outcome is not known here: how it ended, before
    /// anything newer goes. Throws when it has not ended yet (or the server cannot be asked): the lane waits.
    std::pair<std::string, std::optional<std::string>> settleEarlier(const PendingUpload& upload);
    /// The save's record is gone: another instance or a resume finished or replaced it; the server says how it ended.
    FileVersion settledElsewhere(const PendingUpload& upload);
    FileVersion send(const PendingUpload& upload);
    FileVersion settle(const PendingUpload& upload, Json state);
    FileVersion done(const PendingUpload& upload, const std::optional<FileVersion>& file, const std::optional<std::string>& versionId);

    const Resolved& opts_;
    Api& api_;
    KeyValueStore& store_;
    std::function<std::optional<std::string>()> subject_;
    std::function<int()> generation_;
    std::function<void()> onSaved_;
    /// Per slot (and device), bumped by every save started here: a read compares it before and after.
    mutable std::mutex revisionsMutex_;
    std::map<std::string, int> revisions_;
    /// Orders the saves started in one millisecond.
    std::atomic<std::int64_t> seq_{0};
};
}  // namespace krtu::detail
