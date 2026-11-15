#include "Bugs.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

#include "KrtuworksCore/Crypto.h"

namespace krtu::detail
{
namespace
{
constexpr std::size_t bugsMaxDescription = 4000;
constexpr std::size_t bugsMaxScreenshot = 5 * 1024 * 1024;
constexpr int bugsMaxWaiting = 20;
constexpr std::size_t bugsRingEvents = 50;
constexpr std::size_t bugsEventChars = 1800;
/// The server takes at most 100 KB of events; the SDK's own part stays under it so it never costs the player's report.
constexpr std::size_t bugsEventsChars = 90000;
constexpr std::size_t bugsLogBytes = 32768;
constexpr std::size_t bugsLogLine = 1000;
constexpr std::int64_t bugsRetryMs = 60000;
constexpr std::size_t bugsToldKept = 200;



/// At most `limit` bytes of `text`, cut where a UTF-8 character starts (never inside one).
std::string bugsCut(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit) return text;
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

/// The first `count` UTF-16 units of `text`, never cutting a character.
std::string bugsFirstUnits(const std::string& text, std::size_t count)
{
    std::size_t units = 0;
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto b = static_cast<unsigned char>(text[i]);
        const std::size_t width = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        const std::size_t take = b >= 0xF0 ? 2 : 1;
        if (units + take > count) break;
        units += take;
        i += width;
    }
    return text.substr(0, (std::min)(i, text.size()));
}

bool bugsFinite(double value)
{
    return std::isfinite(value);
}

/// Statuses where the report can never be taken as it is: dropped rather than sent forever.
bool bugsReportRefused(const Error& e)
{
    return e.status() == 400 || e.status() == 413 || e.code() == "report_id_reused";
}

/// The screenshot can never be taken (too late, too big, not a picture, the report gone); `screenshot_exists` is done.
bool bugsScreenshotRefused(const Error& e)
{
    const int s = e.status();
    return s == 400 || s == 404 || s == 409 || s == 411 || s == 413 || s == 415;
}
}  // namespace

BugReports::BugReports(const Resolved& opts, KeyValueStore& store, Api& api, std::function<std::string()> owner, std::function<std::optional<std::string>()> subject,
                       std::function<bool(const BugReportView&)> fixedTold)
    : opts_(opts), store_(store), api_(api), owner_(std::move(owner)), subject_(std::move(subject)), fixedTold_(std::move(fixedTold))
{
}

const std::vector<std::string>& BugReports::categories()
{
    static const std::vector<std::string> list{"CRASH", "GAMEPLAY", "GRAPHICS", "AUDIO", "PERFORMANCE", "CONTROLS", "TEXT", "OTHER"};
    return list;
}

std::optional<std::string> BugReports::screenshotType(std::string_view b)
{
    const auto at = [&b](std::size_t i) { return static_cast<unsigned char>(b[i]); };
    if (b.size() >= 8 && at(0) == 0x89 && at(1) == 0x50 && at(2) == 0x4e && at(3) == 0x47) return std::string("image/png");
    if (b.size() >= 3 && at(0) == 0xff && at(1) == 0xd8 && at(2) == 0xff) return std::string("image/jpeg");
    if (b.size() >= 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP") return std::string("image/webp");
    return std::nullopt;
}

Json BugReports::reportBody(const BugReportInput& input, const std::string& reportId, const Resolved& opts, const std::optional<std::string>& level,
                            const std::optional<double>& fps, const std::vector<Json>& events, const std::string& log)
{
    const auto& list = categories();
    if (std::find(list.begin(), list.end(), input.category) == list.end()) throw std::invalid_argument("category is one of CRASH, GAMEPLAY, GRAPHICS, AUDIO, PERFORMANCE, CONTROLS, TEXT, OTHER");
    const auto description = trimSpace(input.description);
    if (description.empty() || utf16Length(description) > bugsMaxDescription) throw std::invalid_argument("description is what the player wrote, 1-4000 characters");
    // The level the SDK saw is cut to fit; one the game names must fit.
    const auto place = input.level ? input.level : (level ? std::optional<std::string>(bugsFirstUnits(*level, 200)) : std::nullopt);
    if (place && (trimSpace(*place).empty() || utf16Length(*place) > 200)) throw std::invalid_argument("level is 1-200 characters");
    const auto rate = input.fps ? input.fps : fps;
    if (rate && (!bugsFinite(*rate) || *rate < 0 || *rate > 1000)) throw std::invalid_argument("fps is a number from 0 to 1000");
    const auto& p = input.position;
    if (p && (p->area.empty() || utf16Length(p->area) > 100 || !bugsFinite(p->x) || !bugsFinite(p->y))) throw std::invalid_argument("position is {area, x, y, z?}");

    auto body = Json::object({{"reportId", reportId}, {"category", input.category}, {"description", description}, {"platform", opts.platform}, {"build", opts.build}});
    if (opts.device)
    {
        auto device = opts.device->toJson();
        if (!device.empty()) body["device"] = std::move(device);
    }
    if (opts.locale && !opts.locale->empty()) body["locale"] = *opts.locale;
    if (place) body["level"] = trimSpace(*place);
    if (p)
    {
        // The server's bug position is the place alone (no `space`).
        auto position = Json::object({{"area", p->area}, {"x", p->x}, {"y", p->y}});
        if (p->z) position["z"] = *p->z;
        body["position"] = std::move(position);
    }
    if (rate) body["fps"] = std::round(*rate * 10) / 10;
    std::size_t start = 0;
    auto kept = Json::array();
    for (; start < events.size(); ++start)
    {
        kept = Json::array();
        for (std::size_t i = start; i < events.size(); ++i) kept.push(events[i]);
        if (kept.dump().size() <= bugsEventsChars) break;
    }
    if (start < events.size() && !kept.empty()) body["events"] = std::move(kept);
    if (!log.empty()) body["log"] = log;
    return body;
}

BugReports::Fixed BugReports::newlyFixed(const std::vector<BugReportView>& reports, const std::optional<std::vector<std::string>>& told)
{
    std::vector<std::string> fixedIds;
    for (const auto& r : reports)
        if (r.status == "RESOLVED") fixedIds.push_back(r.id);
    Fixed out;
    if (!told)
    {
        out.told.assign(fixedIds.begin(), fixedIds.begin() + static_cast<std::ptrdiff_t>((std::min)(fixedIds.size(), bugsToldKept)));
        return out;
    }
    const std::set<std::string> seen(told->begin(), told->end());
    for (const auto& r : reports)
        if (r.status == "RESOLVED" && seen.count(r.id) == 0) out.fixed.push_back(r);
    std::set<std::string> added;
    for (const auto& id : fixedIds)
        if (out.told.size() < bugsToldKept && added.insert(id).second) out.told.push_back(id);
    for (const auto& id : *told)
        if (out.told.size() < bugsToldKept && added.insert(id).second) out.told.push_back(id);
    return out;
}

std::string BugReports::prefix(const std::string& who) const
{
    return "krtu:" + opts_.clientId + ":bugs:" + who + ":";
}

std::string BugReports::fixedKey(const std::string& who) const
{
    return "krtu:" + opts_.clientId + ":bugsFixed:" + who;
}

void BugReports::noteEvent(const WireEvent& event)
{
    auto entry = Json::object({{"type", event.type}, {"at", event.createdAt}});
    if (event.position) entry["position"] = event.position->toJson();
    if (event.data && event.data->dump().size() <= bugsEventChars) entry["data"] = *event.data;
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(entry));
    if (events_.size() > bugsRingEvents) events_.erase(events_.begin());
    if (event.type == "performance" && event.data)
    {
        const auto fps = event.data->getNumber("fps");
        if (fps && bugsFinite(*fps)) lastFps_ = *fps;
    }
}

void BugReports::log(const std::string& message)
{
    const auto line = isoNow() + " " + bugsCut(message, bugsLogLine);
    std::lock_guard<std::mutex> lock(mutex_);
    logLines_.push_back(line);
    logBytes_ += line.size() + 1;
    while (logBytes_ > bugsLogBytes && !logLines_.empty())
    {
        logBytes_ -= logLines_.front().size() + 1;
        logLines_.erase(logLines_.begin());
    }
}

std::string BugReports::report(const BugReportInput& input, const std::optional<std::string>& level)
{
    const auto who = owner_();
    if (static_cast<int>(store_.keys(prefix(who)).size()) >= bugsMaxWaiting)
        throw Error("too_many_reports", "At most 20 bug reports wait on this device; they are sent when the app is online.", 429);
    std::optional<std::string> shotType;
    if (input.screenshot)
    {
        shotType = screenshotType(*input.screenshot);
        if (!shotType) throw std::invalid_argument("The screenshot is a PNG, JPEG or WebP picture");
        if (input.screenshot->size() > bugsMaxScreenshot) throw std::invalid_argument("The screenshot is at most 5 MB");
    }
    const auto reportId = uuidV4();
    std::vector<Json> recent;
    std::string logText;
    std::optional<double> fps;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recent = events_;
        fps = lastFps_;
        for (std::size_t i = 0; i < logLines_.size(); ++i) logText += (i ? "\n" : "") + logLines_[i];
    }
    auto body = reportBody(input, reportId, opts_, level, fps, recent, logText);
    auto waiting = Json::object({{"reportId", reportId}, {"madeAt", nowMs()}, {"body", std::move(body)}});
    if (input.screenshot) waiting["screenshot"] = Json::object({{"bytes", base64(*input.screenshot)}, {"type", *shotType}});
    putJson(store_, prefix(who) + reportId, waiting);
    const auto player = subject_();
    if (player && !store_.get(fixedKey(*player))) putJson(store_, fixedKey(*player), Json::object({{"told", Json::array()}}));
    return reportId;
}

int BugReports::waiting()
{
    return static_cast<int>(store_.keys(prefix(owner_())).size());
}

void BugReports::move(const std::string& from, const std::string& to)
{
    if (from == to) return;
    for (const auto& key : store_.keys(prefix(from)))
    {
        const auto w = getJson(store_, key);
        if (w && w->getString("reportId")) putJson(store_, prefix(to) + *w->getString("reportId"), *w);
        store_.remove(key);
    }
}

BugFlushResult BugReports::flush()
{
    BugFlushResult result;
    const auto player = subject_();
    if (!player) return result;
    const auto now = nowMs();
    std::vector<std::pair<std::string, Json>> queue;
    for (const auto& key : store_.keys(prefix(*player)))
    {
        auto w = getJson(store_, key);
        if (w && w->isObject() && w->getString("reportId")) queue.emplace_back(key, std::move(*w));
    }
    std::stable_sort(queue.begin(), queue.end(), [](const auto& a, const auto& b) { return a.second.getInt("madeAt").value_or(0) < b.second.getInt("madeAt").value_or(0); });
    for (auto& [key, w] : queue)
    {
        const auto reportId = *w.getString("reportId");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto until = waitUntil_.find(reportId);
            if (until != waitUntil_.end() && until->second > now) continue;
        }
        try
        {
            if (!w.getString("bugId"))
            {
                PlayerCall post;
                post.method = "POST";
                post.json = w["body"];
                const auto answer = api_.player("/v1/me/bug-reports", post);
                const auto bugId = answer.body()["report"].getString("id");
                if (!bugId) throw Error("invalid_answer", "The server answered without the report.");
                w["bugId"] = *bugId;
                if (w.contains("screenshot")) putJson(store_, key, w);
            }
            if (w.contains("screenshot"))
            {
                const auto& shot = w["screenshot"];
                PlayerCall put;
                put.method = "PUT";
                put.body = fromBase64(shot.getString("bytes").value_or(""));
                put.headers.set("content-type", shot.getString("type").value_or("image/png"));
                try
                {
                    api_.player("/v1/me/bug-reports/" + *w.getString("bugId") + "/screenshot", put);
                }
                catch (const Error& e)
                {
                    // The report is in: a screenshot it already has is done, one refused for good is let go.
                    if (!bugsScreenshotRefused(e)) throw;
                    if (e.code() != "screenshot_exists") result.dropped.push_back(BugDropped{reportId, e.code(), true});
                }
            }
            store_.remove(key);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                waitUntil_.erase(reportId);
            }
            result.sent += 1;
        }
        catch (const Error& e)
        {
            if (!w.getString("bugId") && bugsReportRefused(e))
            {
                store_.remove(key);
                result.dropped.push_back(BugDropped{reportId, e.code(), false});
                continue;
            }
            // Anything else (no answer, a busy or closed server, a player in a merge, a ban, the module off for
            // now) keeps the report for later.
            if (e.code() != "context_changed")
            {
                const auto wait = e.retryAfter() ? static_cast<std::int64_t>(*e.retryAfter() * 1000) : bugsRetryMs;
                std::lock_guard<std::mutex> lock(mutex_);
                waitUntil_[reportId] = now + wait;
            }
            result.stoppedBy = e.code();
            return result;
        }
    }
    return result;
}

std::vector<BugReportView> BugReports::mine()
{
    const auto player = subject_();
    const auto answer = api_.player("/v1/me/bug-reports", PlayerCall());
    std::vector<BugReportView> reports;
    for (const auto& item : answer.body()["reports"].items())
        if (item.isObject()) reports.push_back(BugReportView::fromJson(item));
    if (player) tellFixed(*player, reports);
    return reports;
}

void BugReports::checkFixed()
{
    const auto player = subject_();
    if (player && store_.get(fixedKey(*player))) mine();
}

void BugReports::tellFixed(const std::string& who, const std::vector<BugReportView>& reports)
{
    std::optional<std::vector<std::string>> told;
    if (const auto kept = getJson(store_, fixedKey(who)); kept && kept->isObject())
    {
        told.emplace();
        for (const auto& id : (*kept)["told"].items())
            if (id.isString()) told->push_back(id.asString());
    }
    const auto result = newlyFixed(reports, told);
    std::set<std::string> untold;
    for (const auto& report : result.fixed)
    {
        // A fix counts as told only when the game listens; otherwise it waits for the next look.
        bool heard = false;
        quietly([this, &report, &heard] { heard = fixedTold_ && fixedTold_(report); });
        if (!heard) untold.insert(report.id);
    }
    auto ids = Json::array();
    for (const auto& id : result.told)
        if (untold.count(id) == 0) ids.push(id);
    putJson(store_, fixedKey(who), Json::object({{"told", ids}}));
}

void BugReports::resetBackoff()
{
    std::lock_guard<std::mutex> lock(mutex_);
    waitUntil_.clear();
}
}  // namespace krtu::detail
