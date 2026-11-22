#include "CloudMirror.h"

#include <utility>

#include "Infrastructure.h"
#include "KrtuworksCore/Crypto.h"

namespace krtu::detail
{
namespace
{
bool cloudMirrored(const std::string& kind)
{
    return kind == "SAVE" || kind == "PROFILE" || kind == "SETTINGS";
}

/// Whether the platform's copy was written after the server's version was saved.
bool cloudPlatformNewer(const std::optional<PlatformCloudFile>& onPlatform, const std::optional<FileVersion>& server)
{
    if (!onPlatform || onPlatform->writtenAt <= 0 || !server) return false;
    const auto savedAt = parseTimeMs(server->savedAt);
    return savedAt && onPlatform->writtenAt > *savedAt / 1000;
}
}  // namespace

CloudMirror::CloudMirror(PlatformCloud& cloud, Files& files, KeyValueStore& store, std::string clientId, std::function<void(const PlatformSaveConflict&)> conflict,
                         std::function<void(const std::string&, const std::string&)> problem)
    : cloud_(cloud), files_(files), store_(store), clientId_(std::move(clientId)), conflict_(std::move(conflict)), problem_(std::move(problem))
{
}

std::string CloudMirror::fileName(const std::string& slot)
{
    return "krtuworks-" + slot + ".bin";
}

MirrorStep CloudMirror::decide(const std::string& kind, const std::optional<std::string>& h, const std::optional<std::string>& s, const std::optional<std::string>& l, bool platformNewer)
{
    if (s == h) return MirrorStep::Agree;
    if (!h) return MirrorStep::ToServer;
    if (!s) return MirrorStep::ToPlatform;
    if (l && s == l) return MirrorStep::ToPlatform;
    if (l && h == l) return MirrorStep::ToServer;
    if (kind == "SETTINGS") return platformNewer ? MirrorStep::ToServer : MirrorStep::ToPlatform;
    return MirrorStep::Offer;
}

std::string CloudMirror::agreedKey(const std::string& owner, const std::string& account) const
{
    return "krtu:" + clientId_ + ":cloudAgreed:" + owner + ":" + account;
}

CloudMirror::Agreed CloudMirror::loadAgreed(const std::string& key)
{
    Agreed agreed;
    if (const auto kept = getJson(store_, key))
    {
        for (const auto& member : kept->members())
        {
            if (member.second.isString()) agreed[member.first] = member.second.asString();
        }
    }
    return agreed;
}

void CloudMirror::saveAgreed(const std::string& key, const Agreed& agreed)
{
    auto kept = Json::object();
    for (const auto& entry : agreed) kept[entry.first] = entry.second;
    putJson(store_, key, kept);
}

void CloudMirror::tell(const std::vector<PlatformSaveConflict>& raised) const
{
    for (const auto& conflict : raised) conflict_(conflict);
}

void CloudMirror::sync(const std::string& owner, const std::string& account, const std::string& sessionId, const std::optional<std::string>& slot)
{
    std::vector<PlatformSaveConflict> raised;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        syncOnce(owner, account, sessionId, slot, raised);
    }
    // After the sync ended: a game that answers the conflict at once (resolveFile) must not wait on this sync's lock.
    tell(raised);
}

void CloudMirror::syncOnce(const std::string& owner, const std::string& account, const std::string& sessionId, const std::optional<std::string>& only, std::vector<PlatformSaveConflict>& raised)
{
    if (!cloud_.enabled()) return;
    const auto list = files_.list();
    const auto key = agreedKey(owner, account);
    auto agreed = loadAgreed(key);
    for (const auto& slot : list.slots)
    {
        if (only && slot.key != *only) continue;
        if (!slot.platformMirror || slot.retired || !cloudMirrored(slot.kind) || leftAlone_.count(slot.key) != 0) continue;
        try
        {
            syncSlot(slot, agreed, sessionId, raised);
        }
        catch (const Error& err)
        {
            // The slot moved meanwhile, a write still settling, the server away: the next sync compares again.
            // Refused for good (too big, over the player's quota, the link): left alone for the session.
            if (!err.retryable() && err.code() != "file_conflict" && err.code() != "upload_pending" && err.code() != "save_replaced")
            {
                leftAlone_.insert(slot.key);
                problem_("platform_" + err.code(), slot.key);
            }
        }
        saveAgreed(key, agreed);
    }
}

void CloudMirror::syncSlot(const SlotView& slot, Agreed& agreed, const std::string& sessionId, std::vector<PlatformSaveConflict>& raised)
{
    const auto onPlatform = cloud_.readFile(fileName(slot.key));
    const auto s = onPlatform ? std::optional<std::string>(toHex(sha256(onPlatform->data))) : std::nullopt;
    if (slot.conflict)
    {
        // A choice waits: nothing moves until the player made it. The game hears of it once per launch.
        if (slot.conflict->platformCloud && slot.file && told_.insert(slot.conflict->id).second)
            raised.push_back(PlatformSaveConflict{slot.key, *slot.file, *slot.conflict, s == slot.conflict->sha256 ? std::optional<std::string>(onPlatform->data) : std::nullopt});
        return;
    }
    const auto h = slot.file ? std::optional<std::string>(slot.file->sha256) : std::nullopt;
    const auto kept = agreed.find(slot.key);
    const auto l = kept != agreed.end() ? std::optional<std::string>(kept->second) : std::nullopt;
    switch (decide(slot.kind, h, s, l, cloudPlatformNewer(onPlatform, slot.file)))
    {
        case MirrorStep::Agree:
            if (!h) agreed.erase(slot.key);
            else agreed[slot.key] = *h;
            return;
        case MirrorStep::ToPlatform:
            toPlatform(slot.key, *h, onPlatform, agreed);
            return;
        case MirrorStep::ToServer:
        {
            // A SAVE or PROFILE over the version compared (or into the empty slot); SETTINGS: the latest wins.
            const bool conditional = slot.kind != "SETTINGS";
            const auto saved = files_.save(slot.key, onPlatform->data, conditional, conditional && slot.file ? std::optional<std::string>(slot.file->id) : std::nullopt, std::nullopt);
            agreed[slot.key] = saved.sha256;
            return;
        }
        case MirrorStep::Offer:
            offer(slot, *onPlatform, *s, agreed, sessionId, raised);
            return;
    }
}

void CloudMirror::toPlatform(const std::string& slot, const std::string& h, const std::optional<PlatformCloudFile>& onPlatform, Agreed& agreed)
{
    const auto read = files_.read(slot, std::nullopt, std::nullopt);
    // The slot moved since the list was read: the next sync compares the new version.
    if (!read || read->sha256 != h) return;
    const auto free = cloud_.availableBytes();
    const auto existing = onPlatform ? static_cast<std::int64_t>(onPlatform->data.size()) : 0;
    if (free && static_cast<std::int64_t>(read->bytes.size()) > *free + existing)
    {
        leftAlone_.insert(slot);
        problem_("platform_quota", slot);
        return;
    }
    if (!cloud_.writeFile(fileName(slot), read->bytes))
    {
        leftAlone_.insert(slot);
        problem_("platform_rejected", slot);
        return;
    }
    agreed[slot] = h;
}

void CloudMirror::offer(const SlotView& slot, const PlatformCloudFile& onPlatform, const std::string& s, Agreed& agreed, const std::string& sessionId, std::vector<PlatformSaveConflict>& raised)
{
    FileVersion offered;
    try
    {
        offered = files_.offer(slot.key, onPlatform.data, slot.file->id, sessionId);
    }
    catch (const Error& err)
    {
        if (err.code() == "same_as_current")
        {
            agreed[slot.key] = s;
            return;
        }
        const auto* waiting = err.details().find("conflict");
        if (err.code() != "conflict_pending" || waiting == nullptr || !waiting->isObject()) throw;
        offered = FileVersion::fromJson(*waiting);
    }
    if (told_.insert(offered.id).second)
        raised.push_back(PlatformSaveConflict{slot.key, *slot.file, offered, offered.sha256 == s ? std::optional<std::string>(onPlatform.data) : std::nullopt});
}

void CloudMirror::afterChoice(const std::string& owner, const std::string& account, const std::string& sessionId, const std::string& slot)
{
    std::vector<PlatformSaveConflict> raised;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cloud_.enabled()) return;
        const auto onPlatform = cloud_.readFile(fileName(slot));
        const auto key = agreedKey(owner, account);
        auto agreed = loadAgreed(key);
        if (onPlatform) agreed[slot] = toHex(sha256(onPlatform->data));
        saveAgreed(key, agreed);
        leftAlone_.erase(slot);
        syncOnce(owner, account, sessionId, slot, raised);
    }
    tell(raised);
}
}  // namespace krtu::detail
