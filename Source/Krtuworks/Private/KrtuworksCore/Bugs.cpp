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
/// The server's JSON body limit is 256 KiB: the log as written into the body (control characters take six bytes each) also stays under a budget, next to the events' 90 KB.
constexpr std::size_t bugsLogBodyBytes = 90000;
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

/// Statuses where the report can never be taken as it is: dropped rather than sent forever. An error that came
/// before the call (a refused refresh) or a refusal that can lift is never a verdict on the report.
bool bugsReportRefused(const Error& e)
{
    if (e.waitsForLater()) return false;
    return e.status() == 400 || e.status() == 413 || e.code() == "report_id_reused";
}

/// How long a report waits after `e`: the server's Retry-After; a failure only a change elsewhere lifts waits long.
std::int64_t bugsWaitMs(const Error& e)
{
    if (e.retryAfter()) return static_cast<std::int64_t>(*e.retryAfter() * 1000);
    if (Error::isWaitingCode(e.code()) || (e.authLayer() && !e.retryable())) return queueLongWaitMs;
    return bugsRetryMs;
}

/// The screenshot can never be taken (too late, too big, not a picture, the report gone); `screenshot_exists` is
/// done. A 409 that is only a pause (`app_transferring`, `account_merging`) and anything that can lift keeps it.
bool bugsScreenshotRefused(const Error& e)
{
    if (e.waitsForLater() || e.code() == "app_transferring") return false;
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
    // The log keeps its newest lines.
    std::string keptLog = log;
    while (!keptLog.empty() && Json(keptLog).dump().size() > bugsLogBodyBytes)
    {
        const auto cut = keptLog.find('\n');
        keptLog = cut == std::string::npos ? std::string() : keptLog.substr(cut + 1);
    }
    if (!keptLog.empty()) body["log"] = keptLog;
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
    const auto who = owner_();
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(NotedEvent{who, std::move(entry)});
    if (events_.size() > bugsRingEvents) events_.erase(events_.begin());
    if (event.type == "performance" && event.data)
    {
        const auto fps = event.data->getNumber("fps");
        if (fps && bugsFinite(*fps)) lastFps_ = NotedFps{who, *fps};
    }
}

void BugReports::log(const std::string& message)
{
    const auto line = isoNow() + " " + bugsCut(message, bugsLogLine);
    const auto who = owner_();
    std::lock_guard<std::mutex> lock(mutex_);
    logLines_.push_back(NotedLine{who, line});
    logBytes_ += line.size() + 1;
    while (logBytes_ > bugsLogBytes && !logLines_.empty())
    {
        logBytes_ -= logLines_.front().line.size() + 1;
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
        for (const auto& e : events_)
            if (e.owner == who) recent.push_back(e.entry);
        if (lastFps_ && lastFps_->owner == who) fps = lastFps_->fps;
        for (const auto& l : logLines_)
            if (l.owner == who) logText += (logText.empty() ? "" : "\n") + l.line;
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : events_)
            if (e.owner == from) e.owner = to;
        for (auto& l : logLines_)
            if (l.owner == from) l.owner = to;
        if (lastFps_ && lastFps_->owner == from) lastFps_->owner = to;
    }
    for (const auto& key : store_.keys(prefix(from)))
    {
        const auto w = getJson(store_, key);
        if (w && w->getString("reportId")) putJson(store_, prefix(to) + *w->getString("reportId"), *w);
        store_.remove(key);
    }
}

bool BugReports::afterFailure(const Error& e, const std::string& key, const Json& w, std::int64_t now, BugFlushResult& result)
{
    const auto reportId = *w.getString("reportId");
    if (!w.getString("bugId") && bugsReportRefused(e))
    {
        store_.remove(key);
        result.dropped.push_back(BugDropped{reportId, e.code(), false});
        return true;
    }
    // Anything else (no answer, a busy or closed server, a player in a merge, a ban, the module off for now) keeps
    // the report for later.
    if (e.code() != "context_changed")
    {
        std::lock_guard<std::mutex> lock(mutex_);
        waitUntil_[reportId] = now + bugsWaitMs(e);
    }
    result.stoppedBy = e.code();
    return false;
}

BugFlushResult BugReports::flush()
{
    BugFlushResult result;
    const auto player = subject_();
    if (!player) return result;
    const auto now = nowMs();
    {
        // Waits that ran out say nothing more: the map holds only reports still waiting.
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = waitUntil_.begin(); it != waitUntil_.end();)
        {
            if (it->second <= now) it = waitUntil_.erase(it);
            else ++it;
        }
    }
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
            std::optional<std::string> shot;
            if (w.contains("screenshot"))
            {
                try
                {
                    shot = fromBase64(w["screenshot"].getString("bytes").value_or(""));
                }
                catch (const std::invalid_argument&)
                {
                    // A picture that cannot be read can never be sent: the report goes without it.
                    w.erase("screenshot");
                    putJson(store_, key, w);
                    result.dropped.push_back(BugDropped{reportId, "screenshot_unreadable", true});
                }
            }
            if (!w.getString("bugId"))
            {
                PlayerCall post;
                post.method = "POST";
                post.json = w["body"];
                post.owner = *player;
                const auto answer = api_.player("/v1/me/bug-reports", post);
                const auto bugId = answer.body()["report"].getString("id");
                if (!bugId) throw Error("invalid_answer", "The server answered without the report.");
                w["bugId"] = *bugId;
                if (w.contains("screenshot")) putJson(store_, key, w);
            }
            if (shot)
            {
                PlayerCall put;
                put.method = "PUT";
                put.owner = *player;
                put.body = *shot;
                put.headers.set("content-type", w["screenshot"].getString("type").value_or("image/png"));
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
            if (!afterFailure(e, key, w, now, result)) return result;
        }
        catch (const std::exception& other)
        {
            // Not the SDK's own failure: read as no answer, the report stays.
            if (!afterFailure(Error("network_error", other.what()), key, w, now, result)) return result;
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

void BugReports::drop(const std::string& owner)
{
    for (const auto& key : store_.keys(prefix(owner))) store_.remove(key);
    store_.remove(fixedKey(owner));
    std::lock_guard<std::mutex> lock(mutex_);
    events_.erase(std::remove_if(events_.begin(), events_.end(), [&owner](const NotedEvent& e) { return e.owner == owner; }), events_.end());
    logLines_.erase(std::remove_if(logLines_.begin(), logLines_.end(), [&owner](const NotedLine& l) { return l.owner == owner; }), logLines_.end());
    logBytes_ = 0;
    for (const auto& l : logLines_) logBytes_ += l.line.size() + 1;
    if (lastFps_ && lastFps_->owner == owner) lastFps_.reset();
}

void BugReports::resetBackoff()
{
    std::lock_guard<std::mutex> lock(mutex_);
    waitUntil_.clear();
}
}  // namespace krtu::detail
