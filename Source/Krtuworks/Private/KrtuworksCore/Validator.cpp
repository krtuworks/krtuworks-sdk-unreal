#include "KrtuworksCore/Validator.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <thread>

#include "KrtuworksCore/Client.h"
#include "KrtuworksCore/Error.h"
#include "Infrastructure.h"

namespace krtu
{
namespace
{
const std::vector<std::string> appTypes = {"GAME", "MOBILE_APP", "WEB_APP", "DESKTOP_APP"};
const std::vector<std::string> platforms = {"STEAM", "EPIC", "XBOX", "PLAYSTATION", "NINTENDO", "IOS", "ANDROID", "WEB", "WINDOWS", "MACOS", "LINUX"};
/// Consoles sign in with a code shown on the TV: the device client of each environment.
const std::vector<std::string> consoles = {"XBOX", "PLAYSTATION", "NINTENDO"};
constexpr int sends = 5;
/// The server takes one write per slot per 10 s; the validator sits out a wait of at most writeWaitSeconds.
constexpr double writeLimitSeconds = 10;
constexpr double writeWaitSeconds = 15;

const std::vector<std::pair<std::string, std::string>> titles = {
    {"config", "krtuworks-config.json names the app and its sign-in clients"},
    {"build", "The build is a version label the servers take"},
    {"platform", "The platform and its services are set"},
    {"sign_in", "Signed in as a TEST player"},
    {"session", "Opened a session, sent a heartbeat and ended it"},
    {"events", "An event batch was accepted"},
    {"remote_config", "Read the remote config"},
    {"cloud_save", "Wrote a save and read it back"},
    {"progress", "Sent a progress operation"},
    {"bug_report", "Sent a test bug report"},
    {"sentry", "A test error reached Sentry"},
};
const std::vector<std::string> runKeys = {"sign_in", "session", "events", "remote_config", "cloud_save", "progress", "bug_report"};

bool among(const std::vector<std::string>& list, const std::string& value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

bool lowerOrDigit(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/// `^app_[0-9a-z]{20}$`
bool isAppId(const std::string& text)
{
    return text.size() == 24 && detail::startsWith(text, "app_") && std::all_of(text.begin() + 4, text.end(), lowerOrDigit);
}

/// `^[a-z0-9][a-z0-9-]{1,38}[a-z0-9]$`
bool isSlug(const std::string& text)
{
    if (text.size() < 3 || text.size() > 40 || !lowerOrDigit(text.front()) || !lowerOrDigit(text.back())) return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return lowerOrDigit(c) || c == '-'; });
}

std::string join(const std::vector<std::string>& parts, const std::string& by)
{
    std::string out;
    for (const auto& part : parts) out += (out.empty() ? "" : by) + part;
    return out;
}

ValidationCheck check(const std::string& key, const std::string& status, std::optional<std::string> detail = std::nullopt)
{
    const auto title = std::find_if(titles.begin(), titles.end(), [&](const auto& t) { return t.first == key; });
    return ValidationCheck{key, title->second, status, std::move(detail)};
}

/// Why the exception being handled was thrown (call it inside a catch block; RTTI is off in Unreal).
std::string whyNow()
{
    try
    {
        throw;
    }
    catch (const Error& err)
    {
        return err.code() + ": " + err.message();
    }
    catch (const std::exception& err)
    {
        return err.what();
    }
    catch (...)
    {
        return "unknown error";
    }
}

struct Sent
{
    SendResult total;
    WaitingCounts left;
};

/// Sends until nothing waits (another send may be running when this starts) and adds up what came back, with
/// what still waits at the end: the game's own SDK on the same store may have sent the validator's item first.
Sent sendAll(Client& krtu)
{
    Sent out;
    for (int round = 0; round < sends; ++round)
    {
        const auto r = krtu.flush();
        out.total.progress.applied += r.progress.applied;
        out.total.progress.stoppedBy = r.progress.stoppedBy;
        out.total.bugs.sent += r.bugs.sent;
        out.total.bugs.dropped.insert(out.total.bugs.dropped.end(), r.bugs.dropped.begin(), r.bugs.dropped.end());
        out.total.bugs.stoppedBy = r.bugs.stoppedBy;
        out.total.events.sent += r.events.sent;
        out.total.events.rejected.insert(out.total.events.rejected.end(), r.events.rejected.begin(), r.events.rejected.end());
        out.total.events.stoppedBy = r.events.stoppedBy;
        out.left = krtu.waiting();
        if (out.left.events == 0 && out.left.progress == 0 && out.left.bugReports == 0) break;
        if (r.progress.stoppedBy || r.bugs.stoppedBy || r.events.stoppedBy) break;
    }
    return out;
}

/// The event the validator sends: the first family whose module is on.
struct Probe
{
    const char* module;
    const char* type;
    Json data;
};

std::vector<Probe> probes()
{
    return {
        {"ui", "screen_view", Json::object({{"screen", "krtuworks-validator"}})},
        {"performance", "performance", Json::object({{"fps", 60}})},
        {"settings", "settings", Json::object({{"language", "en"}})},
        {"errors", "error", Json::object({{"kind", "other"}, {"message", "Krtuworks validator"}, {"handled", true}})},
        {"economy", "balance", Json::object({{"currency", "validator"}, {"amount", 0}})},
        {"items", "interaction", Json::object({{"kind", "other"}, {"name", "krtuworks-validator"}})},
        {"progression", "progression", Json::object({{"action", "checkpoint"}, {"level", "krtuworks-validator"}, {"checkpoint", "validator"}})},
        {"combat", "damage", Json::object({{"amount", 0}})},
    };
}

std::optional<ValidationCheck> failedWith(const std::string& key, const std::optional<std::string>& reason)
{
    if (!reason) return std::nullopt;
    return check(key, "FAILED", reason);
}
}  // namespace

std::vector<std::string> configProblems(const AppConfig& config, const std::string& platform, bool web)
{
    std::vector<std::string> problems;
    if (!isAppId(config.appId)) problems.push_back("appId is not an app_ id");
    if (!isSlug(config.slug)) problems.push_back("slug is not the app's address name");
    if (!among(appTypes, config.type)) problems.push_back("type is one of " + join(appTypes, ", "));
    std::vector<std::pair<std::string, std::string>> wanted = {web ? std::make_pair(std::string("webClientId"), std::string("web")) : std::make_pair(std::string("clientId"), std::string("native"))};
    if (among(consoles, platform)) wanted.emplace_back("deviceClientId", "device");
    for (const char* name : {"TEST", "LIVE"})
    {
        const std::string env = name;
        const auto clients = config.environments.find(env);
        for (const auto& [field, kind] : wanted)
        {
            std::optional<std::string> id;
            if (clients != config.environments.end())
                id = field == "webClientId" ? clients->second.webClientId : field == "deviceClientId" ? clients->second.deviceClientId : clients->second.clientId;
            std::string lower = env;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(c - 'A' + 'a'); });
            const auto want = config.slug + "." + lower + "." + kind;
            if (!id || id->empty()) problems.push_back("environments." + env + " has no " + field);
            else if (*id != want) problems.push_back("environments." + env + "." + field + " is " + *id + ", not " + want);
        }
    }
    return problems;
}

std::vector<std::string> platformProblems(const std::string& platform, bool hasPlatformServices)
{
    std::vector<std::string> problems;
    if (!among(platforms, platform)) problems.push_back("platform is one of " + join(platforms, ", "));
    if (platform == "STEAM" && !hasPlatformServices) problems.push_back("STEAM needs the Steamworks services (platformServices) for its ticket and achievements");
    return problems;
}

std::optional<std::string> sentOrWhy(int sent, int waiting, const std::optional<std::string>& stoppedBy)
{
    if (sent > 0 || (waiting == 0 && !stoppedBy)) return std::nullopt;
    return stoppedBy.value_or("not sent");
}

ValidationReport validate(const Options& options, const ValidateOptions& settings)
{
    std::map<std::string, ValidationCheck> results;
    const auto report = [&] {
        ValidationReport out;
        out.build = options.build;
        out.ok = true;
        for (const auto& [key, title] : titles)
        {
            const auto found = results.find(key);
            out.checks.push_back(found != results.end() ? found->second : check(key, "SKIPPED", std::string("Not reached.")));
            if (out.checks.back().status == "FAILED") out.ok = false;
        }
        return out;
    };
    const auto config = configProblems(options.config, options.platform, settings.web);
    results["config"] = config.empty() ? check("config", "PASSED") : check("config", "FAILED", join(config, "; "));
    const bool buildOk = detail::isBuildLabel(options.build);
    results["build"] = buildOk ? check("build", "PASSED") : check("build", "FAILED", std::string("build is the app version, like 1.4.2 or 1.4.2+381 (letters, digits, . _ + -, at most 64)"));
    const auto platform = platformProblems(options.platform, options.platformServices != nullptr);
    results["platform"] = platform.empty() ? check("platform", "PASSED") : check("platform", "FAILED", join(platform, "; "));
    results["sentry"] = check("sentry", "SKIPPED", std::string("Sentry is not set up yet."));
    if (!config.empty() || !buildOk)
    {
        for (const auto& key : runKeys) results[key] = check(key, "SKIPPED", std::string("Fix the config and the build first."));
        return report();
    }

    std::unique_ptr<Client> krtu;
    try
    {
        Options test = options;
        test.environment = "TEST";
        test.sessions = false;
        if (settings.web) test.clientId = options.config.environments.at("TEST").webClientId;
        krtu = Client::initialize(test);
        auto& k = *krtu;
        try
        {
            if (!k.player()) k.signInAsGuest(settings.birthDate.value_or("2000-01-01"), settings.acceptedTerms.value_or(Client::currentTerms));
            k.ready();
            results["sign_in"] = check("sign_in", "PASSED");
        }
        catch (const std::exception&)
        {
            results["sign_in"] = check("sign_in", "FAILED", whyNow());
            for (std::size_t i = 1; i < runKeys.size(); ++i) results[runKeys[i]] = check(runKeys[i], "SKIPPED", std::string("No TEST player could sign in."));
            k.shutdown();
            return report();
        }
        const auto step = [&](const std::string& key, const std::function<std::optional<ValidationCheck>()>& body) {
            try
            {
                const auto out = body();
                results[key] = out ? *out : check(key, "PASSED");
            }
            catch (const std::exception&)
            {
                results[key] = check(key, "FAILED", whyNow());
            }
        };

        // The modules the owner left on in TEST; the read that counts for the report comes last, once the build is known there.
        try
        {
            k.refreshConfig();
        }
        catch (const Error&)
        {
            // The kept copy tells the modules.
        }
        const auto modules = k.moduleStates();
        const auto on = [&](const std::string& module) {
            const auto found = modules.find(module);
            return found != modules.end() && found->second;
        };

        step("session", [&]() -> std::optional<ValidationCheck> {
            if (!on("sessions")) return check("session", "SKIPPED", std::string("The sessions module is off in TEST."));
            k.sessionRound();
            return std::nullopt;
        });
        step("events", [&]() -> std::optional<ValidationCheck> {
            const auto all = probes();
            const auto probe = std::find_if(all.begin(), all.end(), [&](const Probe& p) { return on(p.module); });
            if (probe == all.end()) return check("events", "SKIPPED", std::string("Every telemetry module the validator can use is off in TEST."));
            if (!k.track(probe->type, probe->data))
                return check("events", "FAILED", std::string("The event was not kept: statistics are off for this player, or the owner switched this build off."));
            const auto sent = sendAll(k);
            if (!sent.total.events.rejected.empty())
            {
                std::vector<std::string> codes;
                for (const auto& r : sent.total.events.rejected) codes.push_back(r.second);
                return check("events", "FAILED", join(codes, ", "));
            }
            return failedWith("events", sentOrWhy(sent.total.events.sent, sent.left.events, sent.total.events.stoppedBy));
        });
        step("cloud_save", [&]() -> std::optional<ValidationCheck> {
            if (!on("cloudSave")) return check("cloud_save", "SKIPPED", std::string("The cloudSave module is off in TEST."));
            std::vector<SlotView> usable;
            for (const auto& s : k.listFiles().slots)
                if (!s.retired && s.kind != "DEVICE_SETTINGS") usable.push_back(s);
            const auto slot = settings.saveSlot ? std::find_if(usable.begin(), usable.end(), [&](const SlotView& s) { return s.key == *settings.saveSlot; }) : usable.begin();
            if (slot == usable.end())
                return settings.saveSlot ? check("cloud_save", "FAILED", "The app has no slot " + *settings.saveSlot + " in TEST.") : check("cloud_save", "SKIPPED", std::string("The app has no save slot in TEST."));
            const auto bytes = "krtuworks-validator " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            const bool conditional = slot->kind == "SAVE" || slot->kind == "PROFILE";
            try
            {
                if (conditional) k.saveFile(slot->key, bytes, slot->file ? std::optional<std::string>(slot->file->id) : std::nullopt);
                else k.saveSettings(slot->key, bytes);
            }
            catch (const Error& err)
            {
                const double wait = err.code() == "too_many_writes" ? err.retryAfter().value_or(writeLimitSeconds) : writeWaitSeconds + 1;
                if (wait > writeWaitSeconds) throw;
                // A slot takes one write per 10 seconds: a run right after another waits and sends the same save again.
                const auto ms = std::chrono::milliseconds(static_cast<std::int64_t>(wait * 1000) + 250);
                if (options.delay) options.delay(ms, CancelToken());
                else std::this_thread::sleep_for(ms);
                std::vector<ResumedUpload> resent;
                // An older save of this slot left by another run is replaced by this one; only this one's outcome counts.
                for (const auto& u : k.resumeFiles())
                    if (u.slot == slot->key && u.outcome != "replaced") resent.push_back(u);
                const auto refused = std::find_if(resent.begin(), resent.end(), [](const ResumedUpload& u) { return u.outcome != "saved"; });
                if (refused != resent.end() || resent.empty())
                    return check("cloud_save", "FAILED", refused != resent.end() ? refused->code.value_or("not sent") : std::string("not sent"));
            }
            const auto back = k.readFile(slot->key);
            if (back && back->bytes == bytes) return std::nullopt;
            return check("cloud_save", "FAILED", "The save read back from " + slot->key + " is not the one written.");
        });
        step("progress", [&]() -> std::optional<ValidationCheck> {
            if (!on("achievements")) return check("progress", "SKIPPED", std::string("The achievements module is off in TEST."));
            const auto view = k.getProgress();
            const auto achievement = std::find_if(view.achievements.begin(), view.achievements.end(), [](const AchievementView& a) { return !a.retired; });
            if (achievement == view.achievements.end()) return check("progress", "SKIPPED", std::string("The app has no achievement in TEST."));
            const auto before = k.progressRefusals().size();
            k.unlockAchievement(achievement->key);
            const auto sent = sendAll(k);
            const auto refusals = k.progressRefusals();
            for (auto r = refusals.begin() + static_cast<std::ptrdiff_t>((std::min)(before, refusals.size())); r != refusals.end(); ++r)
                if (r->key == achievement->key) return check("progress", "FAILED", r->code);
            return failedWith("progress", sentOrWhy(sent.total.progress.applied, sent.left.progress, sent.total.progress.stoppedBy));
        });
        step("bug_report", [&]() -> std::optional<ValidationCheck> {
            if (!on("bugReports")) return check("bug_report", "SKIPPED", std::string("The bugReports module is off in TEST."));
            BugReportInput input;
            input.category = "OTHER";
            input.description = "A test report from the Krtuworks validator.";
            const auto reportId = k.reportBug(input);
            const auto sent = sendAll(k);
            for (const auto& d : sent.total.bugs.dropped)
                if (d.reportId == reportId) return check("bug_report", "FAILED", d.code);
            return failedWith("bug_report", sentOrWhy(sent.total.bugs.sent, sent.left.bugReports, sent.total.bugs.stoppedBy));
        });
        step("remote_config", [&]() -> std::optional<ValidationCheck> {
            k.refreshConfig();
            return std::nullopt;
        });
        k.shutdown();
    }
    catch (const std::exception&)
    {
        const auto reason = whyNow();
        for (const auto& key : runKeys)
            if (results.find(key) == results.end()) results[key] = check(key, "FAILED", reason);
        if (krtu)
        {
            try
            {
                krtu->shutdown();
            }
            catch (const std::exception&)
            {
                // The report is what matters.
            }
        }
    }
    return report();
}
}  // namespace krtu
