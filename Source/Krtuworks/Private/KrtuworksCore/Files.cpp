#include "Files.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

#include "KrtuworksCore/Crypto.h"

namespace krtu::detail
{
namespace
{
constexpr auto filesPollLimit = std::chrono::seconds(60);
/// How long after its last send an upload the server does not know can still arrive (an edge holds a whole
/// request before it reaches the server). Past it, a save no one heard of is gone.
constexpr std::int64_t filesArrivalWindowMs = 15LL * 60LL * 1000LL;

Error filesReplaced()
{
    return Error("save_replaced", "A newer save of this slot took the place of this one before it was sent.", 409);
}

std::string filesRevisionKey(const std::string& slot, const std::optional<std::string>& device)
{
    return slot + std::string(1, '\0') + device.value_or("");
}

std::int64_t filesInteger(const std::optional<std::string>& text)
{
    if (!text || text->empty()) return 0;
    std::int64_t n = 0;
    for (const char c : *text)
    {
        if (c < '0' || c > '9') return 0;
        n = n * 10 + (c - '0');
    }
    return n;
}

void filesAddDevice(QueryFields& query, const std::optional<std::map<std::string, std::string>>& info)
{
    if (!info) return;
    for (const auto& entry : *info) query.emplace_back(entry.first, entry.second);
}
}  // namespace

Json PendingUpload::toJson() const
{
    auto json = Json::object({{"uploadId", uploadId}, {"owner", owner}, {"slot", slot}});
    if (device) json["device"] = *device;
    if (base) json["base"] = *base;
    json["conditional"] = conditional;
    json["sha256"] = sha256;
    json["bytes"] = krtu::base64(bytes);
    if (deviceInfo)
    {
        auto info = Json::object();
        for (const auto& entry : *deviceInfo) info[entry.first] = entry.second;
        json["deviceInfo"] = info;
    }
    json["savedAt"] = savedAt;
    json["seq"] = seq;
    if (v2) json["v"] = 2;
    return json;
}

std::optional<PendingUpload> PendingUpload::fromJson(const Json& json)
{
    if (!json.isObject()) return std::nullopt;
    PendingUpload u;
    u.uploadId = json.getString("uploadId").value_or("");
    u.owner = json.getString("owner").value_or("");
    u.slot = json.getString("slot").value_or("");
    u.device = json.getString("device");
    u.base = json.getString("base");
    u.conditional = json.getBool("conditional").value_or(false);
    u.sha256 = json.getString("sha256").value_or("");
    try
    {
        u.bytes = fromBase64(json.getString("bytes").value_or(""));
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
    if (const auto* info = json.find("deviceInfo"); info != nullptr && info->isObject())
    {
        std::map<std::string, std::string> values;
        for (const auto& member : info->members())
        {
            if (member.second.isString()) values[member.first] = member.second.asString();
        }
        u.deviceInfo = values;
    }
    u.savedAt = json.getInt("savedAt").value_or(0);
    u.seq = json.getInt("seq").value_or(0);
    u.v2 = json.getInt("v") == std::optional<std::int64_t>(2);
    if (u.uploadId.empty() || u.slot.empty()) return std::nullopt;
    return u;
}

Files::Files(const Resolved& opts, Api& api, KeyValueStore& store, std::function<std::optional<std::string>()> subject, std::function<int()> generation, std::function<void()> onSaved)
    : opts_(opts), api_(api), store_(store), subject_(std::move(subject)), generation_(std::move(generation)), onSaved_(std::move(onSaved))
{
}

std::string Files::filesPath() const
{
    return "/v1/apps/" + urlEncode(opts_.slug) + "/files";
}

std::string Files::slotPath(const std::string& slot) const
{
    return filesPath() + "/" + urlEncode(slot);
}

std::string Files::uploadKey(const std::string& uploadId) const
{
    return uploadPrefix() + uploadId;
}

std::string Files::uploadPrefix() const
{
    return "krtu:" + opts_.clientId + ":upload:";
}

std::string Files::sentKey(const std::string& uploadId) const
{
    return "krtu:" + opts_.clientId + ":uploadSent:" + uploadId;
}

std::string Files::laneLock(const PendingUpload& upload) const
{
    return "krtu:" + opts_.clientId + ":uploadLane:" + Json::array({upload.owner, upload.slot, orNull(upload.device)}).dump();
}

void Files::forget(const std::string& uploadId)
{
    store_.remove(uploadKey(uploadId));
    store_.remove(sentKey(uploadId));
}

std::optional<std::int64_t> Files::sentAt(const PendingUpload& upload)
{
    const auto at = getJson(store_, sentKey(upload.uploadId));
    if (at && at->isNumber()) return static_cast<std::int64_t>(at->asDouble());
    if (upload.v2) return std::nullopt;
    return upload.savedAt;
}

bool Files::before(const PendingUpload& a, const PendingUpload& b)
{
    if (a.savedAt != b.savedAt) return a.savedAt < b.savedAt;
    if (a.seq != b.seq) return a.seq < b.seq;
    return a.uploadId < b.uploadId;
}

std::vector<PendingUpload> Files::lane(const PendingUpload& upload)
{
    std::vector<PendingUpload> output;
    for (const auto& name : store_.keys(uploadPrefix()))
    {
        const auto json = getJson(store_, name);
        auto held = json ? PendingUpload::fromJson(*json) : std::nullopt;
        if (held && held->owner == upload.owner && held->slot == upload.slot && held->device == upload.device) output.push_back(std::move(*held));
    }
    std::sort(output.begin(), output.end(), &Files::before);
    return output;
}

int Files::revision(const std::string& revisionKey) const
{
    std::lock_guard<std::mutex> lock(revisionsMutex_);
    const auto found = revisions_.find(revisionKey);
    return found == revisions_.end() ? 0 : found->second;
}

SlotList Files::list()
{
    return listFor(std::nullopt);
}

SlotList Files::listFor(const std::optional<std::string>& owner)
{
    PlayerCall call;
    call.owner = owner;
    return SlotList::fromJson(api_.player(filesPath(), call).body());
}

std::optional<FileRead> Files::read(const std::string& slot, const std::optional<SaveDevice>& device, const std::optional<std::string>& ifNoneMatch)
{
    const auto revisionKey = filesRevisionKey(slot, device ? std::optional<std::string>(device->id) : std::nullopt);
    const auto before = revision(revisionKey);
    PlayerCall call;
    call.query = {{"device", device ? std::optional<std::string>(device->id) : std::nullopt}, {"platform", opts_.platform}};
    if (device) filesAddDevice(call.query, device->info);
    if (ifNoneMatch) call.headers.set("if-none-match", *ifNoneMatch);
    call.accept = {304};
    const auto answer = api_.player(slotPath(slot), call);
    if (answer.status == 304) return std::nullopt;
    FileRead result;
    result.bytes = answer.raw.body;
    result.newerLocalSave = revision(revisionKey) != before;
    result.similarDevice = answer.raw.header("x-krtu-similar-device") == std::optional<std::string>("true");
    if (auto etag = answer.raw.header("etag"))
    {
        if (startsWith(*etag, "W/")) etag = etag->substr(2);
        etag->erase(std::remove(etag->begin(), etag->end(), '"'), etag->end());
        result.versionId = *etag;
    }
    result.generation = filesInteger(answer.raw.header("x-krtu-generation"));
    result.sha256 = answer.raw.header("x-krtu-sha256").value_or("");
    result.savedAt = answer.raw.header("x-krtu-saved-at").value_or("");
    return result;
}

FileVersion Files::save(const std::string& slot, const std::string& data, bool conditional, const std::optional<std::string>& baseVersion, const std::optional<SaveDevice>& device)
{
    const auto owner = subject_();
    if (!owner) throw Error("signed_out", "No player is signed in.");
    {
        std::lock_guard<std::mutex> lock(revisionsMutex_);
        ++revisions_[filesRevisionKey(slot, device ? std::optional<std::string>(device->id) : std::nullopt)];
    }
    PendingUpload upload;
    upload.uploadId = uuidV4();
    upload.owner = *owner;
    upload.slot = slot;
    if (device)
    {
        upload.device = device->id;
        upload.deviceInfo = device->info;
    }
    upload.base = baseVersion;
    upload.conditional = conditional;
    upload.sha256 = toHex(sha256(data));
    upload.bytes = data;
    upload.savedAt = nowMs();
    upload.seq = ++seq_;
    upload.v2 = true;
    putJson(store_, uploadKey(upload.uploadId), upload.toJson());
    return pump(upload, [](const PendingUpload&, const std::string&, const std::optional<std::string>&) {});
}

FileVersion Files::pump(const PendingUpload& target, const std::function<void(const PendingUpload&, const std::string&, const std::optional<std::string>&)>& tell)
{
    auto lock = NamedLocks::take(laneLock(target));
    const auto lane = this->lane(target);
    const auto at = std::find_if(lane.begin(), lane.end(), [&target](const PendingUpload& u) { return u.uploadId == target.uploadId; });
    if (at == lane.end()) return settledElsewhere(target);
    // The earlier ones never sent go first: this save replaces them whatever became of the others.
    std::vector<PendingUpload> sentEarlier;
    for (auto earlier = lane.begin(); earlier != at; ++earlier)
    {
        if (sentAt(*earlier)) sentEarlier.push_back(*earlier);
        else
        {
            forget(earlier->uploadId);
            tell(*earlier, "replaced", std::nullopt);
        }
    }
    for (const auto& earlier : sentEarlier)
    {
        const auto ended = settleEarlier(earlier);
        tell(earlier, ended.first, ended.second);
    }
    if (at + 1 != lane.end() && !sentAt(target))
    {
        forget(target.uploadId);
        throw filesReplaced();
    }
    return send(target);
}

std::pair<std::string, std::optional<std::string>> Files::settleEarlier(const PendingUpload& upload)
{
    Json state;
    try
    {
        state = api_.player(slotPath(upload.slot) + "/uploads/" + upload.uploadId, [&upload] {
                        PlayerCall call;
                        call.owner = upload.owner;
                        return call;
                    }())
                    .body();
    }
    catch (const Error& err)
    {
        if (err.code() != "upload_not_found") throw;
        if (upload.conditional || nowMs() - sentAt(upload).value_or(0) >= filesArrivalWindowMs)
        {
            // It never arrived and no longer can (or, with a version check, would be refused if it did): the newer save takes its place.
            forget(upload.uploadId);
            return {"replaced", std::nullopt};
        }
        // It may still be on its way: sent again with its id, the server takes both as one upload.
        try
        {
            send(upload);
            return {"saved", std::nullopt};
        }
        catch (const Error& sent)
        {
            if (!sent.retryable() && sent.code() != "upload_pending") return {"refused", sent.code()};
            throw;
        }
    }
    const auto status = state.getString("status");
    if (status == std::optional<std::string>("ACTIVATED"))
    {
        forget(upload.uploadId);
        onSaved_();
        return {"saved", std::nullopt};
    }
    if (status == std::optional<std::string>("REJECTED"))
    {
        forget(upload.uploadId);
        return {"refused", state.getString("code").value_or("write_failed")};
    }
    throw Error("upload_pending", "An earlier save of this slot is still being written; this one goes after it.", 202, std::nullopt, Json::object({{"uploadId", upload.uploadId}}));
}

FileVersion Files::settledElsewhere(const PendingUpload& upload)
{
    Json state;
    try
    {
        PlayerCall call;
        call.owner = upload.owner;
        state = api_.player(slotPath(upload.slot) + "/uploads/" + upload.uploadId, call).body();
    }
    catch (const Error& err)
    {
        if (err.code() == "upload_not_found") throw filesReplaced();
        throw;
    }
    return settle(upload, std::move(state));
}

FileVersion Files::send(const PendingUpload& upload)
{
    PlayerCall call;
    call.method = "PUT";
    call.headers.set("idempotency-key", upload.uploadId);
    call.headers.set("content-type", "application/octet-stream");
    if (upload.conditional)
    {
        if (upload.base) call.headers.set("if-match", *upload.base);
        else call.headers.set("if-none-match", "*");
    }
    call.query = {{"platform", opts_.platform}, {"build", opts_.build}, {"device", upload.device}};
    filesAddDevice(call.query, upload.deviceInfo);
    call.body = upload.bytes;
    call.accept = {202};
    call.owner = upload.owner;
    // The sent time is written before the send: a call cut short must still count as possibly arrived.
    putJson(store_, sentKey(upload.uploadId), Json(nowMs()));
    Answer answer;
    try
    {
        answer = api_.player(slotPath(upload.slot), call);
    }
    catch (const Error& err)
    {
        if (!err.retryable()) forget(upload.uploadId);
        throw;
    }
    if (answer.status == 201)
    {
        const auto* file = answer.body().find("file");
        return done(upload, file != nullptr && file->isObject() ? std::optional<FileVersion>(FileVersion::fromJson(*file)) : std::nullopt, std::nullopt);
    }
    return settle(upload, answer.body());
}

FileVersion Files::settle(const PendingUpload& upload, Json state)
{
    const auto started = std::chrono::steady_clock::now();
    const auto generation = generation_();
    std::int64_t wait = 1000;
    while (true)
    {
        const auto status = state.getString("status");
        if (status == std::optional<std::string>("ACTIVATED")) return done(upload, std::nullopt, state["file"].getString("id"));
        if (status == std::optional<std::string>("REJECTED"))
        {
            forget(upload.uploadId);
            throw Error(state.getString("code").value_or("write_failed"), "The save was refused.", 409, std::nullopt, Json::object({{"upload", state}}));
        }
        if (std::chrono::steady_clock::now() - started > filesPollLimit)
            throw Error("upload_pending", "The save is still being written; it is finished on a later launch.", 202, std::nullopt, Json::object({{"uploadId", upload.uploadId}}));
        opts_.delay(std::chrono::milliseconds(wait), CancelToken());
        wait = (std::min)(static_cast<std::int64_t>(8000), wait * 2);
        // Another player now: this upload is asked about with its own player's token on a later sign-in.
        if (generation_() != generation) throw Error("context_changed", "The player changed while the save was being written; it is finished when that player signs in again.");
        PlayerCall poll;
        poll.owner = upload.owner;
        state = api_.player(slotPath(upload.slot) + "/uploads/" + upload.uploadId, poll).body();
    }
}

FileVersion Files::done(const PendingUpload& upload, const std::optional<FileVersion>& file, const std::optional<std::string>& versionId)
{
    forget(upload.uploadId);
    onSaved_();
    if (file) return *file;
    // An answer replayed from the upload's record names the version; read the slot for the whole of it.
    const auto slots = listFor(upload.owner);
    for (const auto& slot : slots.slots)
    {
        if (slot.key != upload.slot) continue;
        std::vector<FileVersion> versions;
        if (slot.file) versions.push_back(*slot.file);
        versions.insert(versions.end(), slot.devices.begin(), slot.devices.end());
        for (const auto& version : versions)
        {
            if (versionId && version.id == *versionId) return version;
        }
        if (!versions.empty()) return versions.front();
    }
    throw Error("file_not_found", "The saved version is no longer the slot's.", 404);
}

std::vector<ResumedUpload> Files::resume()
{
    std::vector<ResumedUpload> output;
    const auto owner = subject_();
    if (!owner) return output;
    // A sent-time key whose upload is gone is an orphan.
    std::set<std::string> ids;
    for (const auto& name : store_.keys(uploadPrefix())) ids.insert(name.substr(uploadPrefix().size()));
    const auto sentPrefix = sentKey("");
    for (const auto& name : store_.keys(sentPrefix))
    {
        if (ids.count(name.substr(sentPrefix.size())) == 0) store_.remove(name);
    }
    std::vector<std::pair<std::string, PendingUpload>> newest;
    for (const auto& id : ids)
    {
        const auto json = getJson(store_, uploadKey(id));
        const auto upload = json ? PendingUpload::fromJson(*json) : std::nullopt;
        if (!upload || upload->owner != *owner) continue;
        const auto lane = laneLock(*upload);
        const auto held = std::find_if(newest.begin(), newest.end(), [&lane](const auto& entry) { return entry.first == lane; });
        if (held == newest.end()) newest.emplace_back(lane, *upload);
        else if (before(held->second, *upload)) held->second = *upload;
    }
    const auto tell = [&output](const PendingUpload& u, const std::string& outcome, const std::optional<std::string>& code) { output.push_back(ResumedUpload{u.uploadId, u.slot, outcome, code}); };
    for (const auto& entry : newest)
    {
        const auto& upload = entry.second;
        try
        {
            // Sent again with the same id and bytes: the server answers what the first one did.
            pump(upload, tell);
            tell(upload, "saved", std::nullopt);
        }
        catch (const Error& err)
        {
            const auto outcome = err.code() == "save_replaced" ? "replaced" : err.code() == "upload_pending" || err.retryable() ? "waiting" : "refused";
            tell(upload, outcome, err.code());
        }
    }
    return output;
}

std::vector<WaitingSave> Files::waitingFor(const std::string& owner)
{
    std::vector<WaitingSave> output;
    for (const auto& name : store_.keys(uploadPrefix()))
    {
        const auto json = getJson(store_, name);
        const auto upload = json ? PendingUpload::fromJson(*json) : std::nullopt;
        if (upload && upload->owner == owner) output.push_back(WaitingSave{upload->uploadId, upload->slot, upload->savedAt});
    }
    return output;
}

FileVersion Files::adoptWaiting(const std::string& uploadId, const std::optional<std::string>& baseVersion)
{
    const auto json = getJson(store_, uploadKey(uploadId));
    const auto waiting = json ? PendingUpload::fromJson(*json) : std::nullopt;
    if (!waiting) throw Error("upload_not_found", "No such waiting save on this device.", 404);
    std::optional<SaveDevice> device;
    if (waiting->device) device = SaveDevice{*waiting->device, waiting->deviceInfo.value_or(std::map<std::string, std::string>())};
    auto saved = save(waiting->slot, waiting->bytes, true, baseVersion, device);
    forget(uploadId);
    return saved;
}

void Files::discardWaiting(const std::string& uploadId)
{
    forget(uploadId);
}

FileVersion Files::offer(const std::string& slot, const std::string& data, const std::string& current, const std::string& sessionId)
{
    PlayerCall call;
    call.method = "PUT";
    call.headers.set("idempotency-key", uuidV4());
    call.headers.set("content-type", "application/octet-stream");
    call.headers.set("if-match", current);
    call.query = {{"platform", opts_.platform}, {"sessionId", sessionId}, {"build", opts_.build}};
    call.body = data;
    call.accept = {202};
    auto state = api_.player(slotPath(slot) + "/offer", call).body();
    const auto started = std::chrono::steady_clock::now();
    std::int64_t wait = 1000;
    while (state.getString("status") != std::optional<std::string>("ACTIVATED"))
    {
        if (state.getString("status") == std::optional<std::string>("REJECTED"))
            throw Error(state.getString("code").value_or("write_failed"), "The offer was refused.", 409, std::nullopt, Json::object({{"upload", state}}));
        if (std::chrono::steady_clock::now() - started > filesPollLimit)
            throw Error("upload_pending", "The offer is still being written; the next sync looks again.", 202);
        opts_.delay(std::chrono::milliseconds(wait), CancelToken());
        wait = (std::min)(static_cast<std::int64_t>(8000), wait * 2);
        state = api_.player(slotPath(slot) + "/uploads/" + state.getString("uploadId").value_or(""), PlayerCall()).body();
    }
    for (const auto& view : list().slots)
    {
        if (view.key == slot && view.conflict) return *view.conflict;
    }
    throw Error("file_not_found", "The offered copy is no longer waiting in the slot.", 404);
}

FileVersion Files::resolve(const std::string& slot, const std::string& keep, const std::string& current)
{
    PlayerCall call;
    call.method = "POST";
    call.json = Json::object({{"keep", keep}, {"current", current}});
    const auto answer = api_.player(slotPath(slot) + "/resolve", call);
    const auto* file = answer.body().find("file");
    if (file == nullptr || !file->isObject()) throw Error("invalid_response", "The answer names no file.");
    return FileVersion::fromJson(*file);
}
}  // namespace krtu::detail
