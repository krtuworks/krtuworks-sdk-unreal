#include "KrtuworksCore/Types.h"

#include <stdexcept>

#include "KrtuworksCore/Crypto.h"

namespace krtu
{
namespace
{
std::string typesStringOr(const Json& json, std::string_view key, std::string fallback = "")
{
    auto value = json.getString(key);
    return value ? *value : fallback;
}

/// A base64 member as its bytes; nothing when it is missing, null or not base64.
std::optional<std::string> typesBytesOf(const Json& json, std::string_view key)
{
    const auto text = json.getString(key);
    if (!text) return std::nullopt;
    try
    {
        return fromBase64(*text);
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

std::optional<FileVersion> typesVersionOf(const Json& json, std::string_view key)
{
    const auto* found = json.find(key);
    if (found == nullptr || !found->isObject()) return std::nullopt;
    return FileVersion::fromJson(*found);
}
}  // namespace

AppConfig AppConfig::fromJson(std::string_view text)
{
    const auto json = Json::parse(text);
    if (!json || !json->isObject()) throw std::invalid_argument("not a krtuworks-config.json");
    AppConfig config;
    config.appId = typesStringOr(*json, "appId");
    config.slug = typesStringOr(*json, "slug");
    config.type = typesStringOr(*json, "type", "GAME");
    config.firstLaunch = typesStringOr(*json, "firstLaunch", "offline");
    const auto* environments = json->find("environments");
    if (environments != nullptr)
    {
        for (const auto& member : environments->members())
        {
            if (!member.second.isObject()) continue;
            EnvironmentClients clients;
            clients.clientId = member.second.getString("clientId");
            clients.deviceClientId = member.second.getString("deviceClientId");
            clients.webClientId = member.second.getString("webClientId");
            config.environments[member.first] = clients;
        }
    }
    return config;
}

Json DeviceInfo::toJson() const
{
    auto json = Json::object();
    if (os) json["os"] = *os;
    if (osVersion) json["osVersion"] = *osVersion;
    if (model) json["model"] = *model;
    if (gpu) json["gpu"] = *gpu;
    if (ramMb) json["ramMb"] = *ramMb;
    if (screenWidth) json["screenWidth"] = *screenWidth;
    if (screenHeight) json["screenHeight"] = *screenHeight;
    return json;
}

StatView StatView::fromJson(const Json& json)
{
    StatView view;
    view.key = typesStringOr(json, "key");
    view.name = typesStringOr(json, "name");
    view.description = json.getString("description");
    view.kind = typesStringOr(json, "kind");
    view.valueType = typesStringOr(json, "valueType");
    view.value = json.getNumber("value");
    view.updatedAt = json.getString("updatedAt");
    for (const auto& member : json["platformIds"].members())
    {
        if (member.second.isString()) view.platformIds[member.first] = member.second.asString();
    }
    return view;
}

BugReportView BugReportView::fromJson(const Json& json)
{
    BugReportView view;
    view.id = typesStringOr(json, "id");
    view.category = typesStringOr(json, "category");
    view.description = json.getString("description");
    view.platform = typesStringOr(json, "platform");
    view.build = typesStringOr(json, "build");
    view.createdAt = typesStringOr(json, "createdAt");
    view.screenshot = json.getBool("screenshot").value_or(false);
    view.status = typesStringOr(json, "status");
    view.fixedIn = json.getString("fixedIn");
    view.fixedAt = json.getString("fixedAt");
    view.duplicate = json.getBool("duplicate").value_or(false);
    return view;
}

AchievementView AchievementView::fromJson(const Json& json)
{
    AchievementView view;
    view.key = typesStringOr(json, "key");
    view.id = typesStringOr(json, "id");
    view.name = json.getString("name");
    view.description = json.getString("description");
    view.hidden = json.getBool("hidden").value_or(false);
    view.retired = json.getBool("retired").value_or(false);
    view.unlocked = json.getBool("unlocked").value_or(false);
    view.unlockedAt = json.getString("unlockedAt");
    for (const auto& member : json["platformIds"].members())
    {
        if (member.second.isString()) view.platformIds[member.first] = member.second.asString();
    }
    view.iconUrl = json.getString("iconUrl");
    view.lockedIconUrl = json.getString("lockedIconUrl");
    return view;
}

ProgressView ProgressView::fromJson(const Json& json)
{
    ProgressView view;
    view.locale = json.getString("locale");
    for (const auto& item : json["stats"].items())
    {
        if (item.isObject()) view.stats.push_back(StatView::fromJson(item));
    }
    for (const auto& item : json["achievements"].items())
    {
        if (item.isObject()) view.achievements.push_back(AchievementView::fromJson(item));
    }
    return view;
}

LeaderboardView LeaderboardView::fromJson(const Json& json)
{
    LeaderboardView view;
    view.key = typesStringOr(json, "key");
    view.name = typesStringOr(json, "name");
    view.description = json.getString("description");
    view.sortOrder = typesStringOr(json, "sortOrder");
    view.display = typesStringOr(json, "display");
    view.uploadMethod = json.getString("uploadMethod");
    view.importFromPlatform = json.getBool("importFromPlatform").value_or(false);
    for (const auto& member : json["platformIds"].members())
    {
        if (member.second.isString()) view.platformIds[member.first] = member.second.asString();
    }
    view.score = json.getInt("score");
    view.details = typesBytesOf(json, "details");
    view.rank = json.getInt("rank");
    view.entries = json.getInt("entries").value_or(0);
    return view;
}

LeaderboardEntry LeaderboardEntry::fromJson(const Json& json)
{
    LeaderboardEntry entry;
    entry.rank = json.getInt("rank").value_or(0);
    entry.subject = json.getString("subject");
    entry.name = json.getString("name");
    entry.score = json.getInt("score").value_or(0);
    entry.details = typesBytesOf(json, "details");
    entry.updatedAt = typesStringOr(json, "updatedAt");
    entry.you = json.getBool("you").value_or(false);
    return entry;
}

LeaderboardPage LeaderboardPage::fromJson(const Json& json)
{
    LeaderboardPage page;
    page.key = typesStringOr(json, "key");
    page.scope = typesStringOr(json, "scope");
    page.sortOrder = typesStringOr(json, "sortOrder");
    page.display = typesStringOr(json, "display");
    page.total = json.getInt("total").value_or(0);
    for (const auto& item : json["entries"].items())
    {
        if (item.isObject()) page.entries.push_back(LeaderboardEntry::fromJson(item));
    }
    return page;
}

Experiment Experiment::fromJson(const Json& json)
{
    Experiment experiment;
    experiment.key = typesStringOr(json, "key");
    experiment.configKey = typesStringOr(json, "configKey");
    experiment.group = typesStringOr(json, "group");
    experiment.value = json["value"];
    return experiment;
}

PlayerProfile PlayerProfile::fromJson(const Json& json)
{
    PlayerProfile profile;
    profile.fields = json["fields"].members();
    profile.updatedAt = json.getString("updatedAt");
    return profile;
}

Json EventPosition::toJson() const
{
    auto json = Json::object({{"space", space}, {"area", area}, {"x", x}, {"y", y}});
    if (z) json["z"] = *z;
    return json;
}

EventPosition EventPosition::fromJson(const Json& json)
{
    EventPosition position;
    position.space = typesStringOr(json, "space", "world");
    position.area = typesStringOr(json, "area");
    position.x = json.getNumber("x").value_or(0);
    position.y = json.getNumber("y").value_or(0);
    position.z = json.getNumber("z");
    return position;
}

FileVersion FileVersion::fromJson(const Json& json)
{
    FileVersion version;
    version.id = typesStringOr(json, "id");
    version.slot = typesStringOr(json, "slot");
    version.device = json.getString("device");
    version.generation = json.getInt("generation").value_or(0);
    version.sizeBytes = json.getInt("sizeBytes").value_or(0);
    version.sha256 = typesStringOr(json, "sha256");
    version.platform = json.getString("platform");
    version.build = json.getString("build");
    version.savedAt = typesStringOr(json, "savedAt");
    version.fromMerge = json.getBool("fromMerge").value_or(false);
    version.platformCloud = json.getString("platformCloud");
    return version;
}

SlotView SlotView::fromJson(const Json& json)
{
    SlotView view;
    view.key = typesStringOr(json, "key");
    view.kind = typesStringOr(json, "kind");
    view.name = typesStringOr(json, "name");
    view.retired = json.getBool("retired").value_or(false);
    view.platformMirror = json.getBool("platformMirror").value_or(false);
    view.file = typesVersionOf(json, "file");
    view.conflict = typesVersionOf(json, "conflict");
    for (const auto& item : json["devices"].items())
    {
        if (item.isObject()) view.devices.push_back(FileVersion::fromJson(item));
    }
    return view;
}

SlotList SlotList::fromJson(const Json& json)
{
    SlotList list;
    for (const auto& item : json["slots"].items())
    {
        if (item.isObject()) list.slots.push_back(SlotView::fromJson(item));
    }
    for (const auto& member : json["limits"].members())
    {
        if (member.second.isInteger()) list.limits[member.first] = member.second.asInt();
    }
    return list;
}
}  // namespace krtu
