#include "RemoteConfig.h"

#include <algorithm>
#include <vector>

namespace krtu::detail
{
namespace
{
std::vector<long long> configVersionParts(const std::string& version)
{
    const auto plain = version.substr(0, version.find('+'));
    std::vector<long long> parts;
    std::size_t at = 0;
    while (at <= plain.size())
    {
        auto end = plain.find_first_of(".-", at);
        if (end == std::string::npos) end = plain.size();
        long long n = 0;
        for (std::size_t i = at; i < end && plain[i] >= '0' && plain[i] <= '9'; ++i) n = n * 10 + (plain[i] - '0');
        parts.push_back(n);
        at = end + 1;
    }
    return parts;
}
}  // namespace

const char* const RemoteConfig::modules[18] = {
    "sessions", "progression", "combat", "economy", "items", "ui", "performance", "settings", "errors", "bugReports", "achievements", "stats", "cloudSave", "remoteConfig", "abTests", "customEvents",
    "leaderboards", "presence",
};

RemoteConfig::RemoteConfig(const Resolved& opts, Api& api, KeyValueStore& store) : opts_(opts), api_(api), store_(store), value_(Json::object())
{
}

std::string RemoteConfig::key() const
{
    return "krtu:" + opts_.clientId + ":config:" + opts_.platform;
}

void RemoteConfig::load()
{
    const auto kept = getJson(store_, key());
    std::lock_guard<std::mutex> lock(mutex_);
    if (!kept || !kept->isObject()) return;
    etag_ = kept->getString("etag");
    const auto& value = (*kept)["value"];
    value_ = value.isObject() ? value : Json::object();
    copy_ = true;
}

bool RemoteConfig::hasCopy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return copy_;
}

bool RemoteConfig::refresh()
{
    PlayerCall call;
    // The build rides in the query, as the JS SDK sends it; the read counts for the build's release report.
    call.query = {{"environment", opts_.environment}, {"platform", opts_.platform}, {"build", opts_.build}};
    call.accept = {304};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (etag_) call.headers.set("if-none-match", *etag_);
    }
    const auto answer = api_.open("/v1/apps/" + urlEncode(opts_.slug) + "/config", call);
    if (answer.status == 304) return false;
    // A 2xx that is not a JSON object (a captive portal's page, a proxy's answer) is a failed fetch: the kept copy stays.
    if (!answer.json) throw Error("invalid_response", "The config answer is not a JSON object.", answer.status);
    const auto etag = answer.raw.header("etag");
    const auto& value = *answer.json;
    auto stored = Json::object();
    if (etag) stored["etag"] = *etag;
    stored["value"] = value;
    putJson(store_, key(), stored);
    std::lock_guard<std::mutex> lock(mutex_);
    etag_ = etag;
    value_ = value;
    copy_ = true;
    return true;
}

bool RemoteConfig::moduleOn(const std::string& module) const
{
    if (!opts_.game() && (module == "progression" || module == "combat")) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& setting = value_["config"]["modules"][module];
    return !setting.isBool() || setting.asBool();
}

std::map<std::string, bool> RemoteConfig::moduleStates() const
{
    std::map<std::string, bool> states;
    for (const char* module : modules) states[module] = moduleOn(module);
    return states;
}

std::optional<BuildSwitch> RemoteConfig::switchForBuild() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<BuildSwitch> telemetry;
    for (const auto& item : value_["switches"].items())
    {
        BuildSwitch s{item.getString("build").value_or(""), item.getString("mode").value_or(""), item.getString("message").value_or("")};
        if (s.build != "*" && s.build != opts_.build) continue;
        if (s.mode == "ALL") return s;
        if (s.mode == "TELEMETRY" && !telemetry) telemetry = s;
    }
    return telemetry;
}

Json RemoteConfig::get(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* found = value_["config"].find(name);
    return found != nullptr ? *found : Json();
}

bool RemoteConfig::olderThan(const std::string& build, const std::string& minVersion)
{
    const auto a = configVersionParts(build);
    const auto b = configVersionParts(minVersion);
    for (std::size_t i = 0; i < (std::max)(a.size(), b.size()); ++i)
    {
        const long long d = (i < a.size() ? a[i] : 0) - (i < b.size() ? b[i] : 0);
        if (d != 0) return d < 0;
    }
    return false;
}
}  // namespace krtu::detail
