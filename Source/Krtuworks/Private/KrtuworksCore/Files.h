#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
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
    /// `file_conflict` (`details()["current"]`: the version now in the slot) when another one came first.
    /// `conditional`: whether `baseVersion` is meant (none = the slot must be empty); SETTINGS-like slots need neither.
    FileVersion save(const std::string& slot, const std::string& data, bool conditional, const std::optional<std::string>& baseVersion, const std::optional<SaveDevice>& device);
    /// Saves left unfinished by an earlier launch (an answer lost, a 202): finishes the signed-in player's.
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
};
}  // namespace krtu::detail
