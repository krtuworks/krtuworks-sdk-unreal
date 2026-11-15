#include "Infrastructure.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>

#include "KrtuworksCore/Crypto.h"

namespace krtu::detail
{
namespace
{
// Howard Hinnant's civil calendar algorithms: exact for every date, no time zone database.
std::int64_t infraDaysFromCivil(std::int64_t year, int month, int day)
{
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<std::int64_t>(year - era * 400);
    const std::int64_t dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const std::int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

void infraCivilFromDays(std::int64_t days, std::int64_t& year, int& month, int& day)
{
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t dayOfEra = days - era * 146097;
    const std::int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    year = yearOfEra + era * 400;
    const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const std::int64_t mp = (5 * dayOfYear + 2) / 153;
    day = static_cast<int>(dayOfYear - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    if (month <= 2) ++year;
}

/// Reads exactly `count` digits at `at`; false when they are not there.
bool infraDigits(std::string_view text, std::size_t& at, std::size_t count, int& value)
{
    if (at + count > text.size()) return false;
    value = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const char c = text[at + i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    at += count;
    return true;
}

bool infraTake(std::string_view text, std::size_t& at, char c)
{
    if (at < text.size() && text[at] == c)
    {
        ++at;
        return true;
    }
    return false;
}

std::optional<std::int64_t> infraParseIso(std::string_view text)
{
    std::size_t at = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    if (!infraDigits(text, at, 4, year) || !infraTake(text, at, '-') || !infraDigits(text, at, 2, month) || !infraTake(text, at, '-') || !infraDigits(text, at, 2, day)) return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    int hour = 0;
    int minute = 0;
    int second = 0;
    std::int64_t millis = 0;
    std::int64_t offsetMinutes = 0;
    if (at < text.size())
    {
        if (!infraTake(text, at, 'T') && !infraTake(text, at, 't') && !infraTake(text, at, ' ')) return std::nullopt;
        if (!infraDigits(text, at, 2, hour) || !infraTake(text, at, ':') || !infraDigits(text, at, 2, minute)) return std::nullopt;
        if (infraTake(text, at, ':') && !infraDigits(text, at, 2, second)) return std::nullopt;
        if (infraTake(text, at, '.') || infraTake(text, at, ','))
        {
            std::int64_t scale = 100;
            bool any = false;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
            {
                millis += (text[at] - '0') * scale;
                scale /= 10;
                any = true;
                ++at;
            }
            if (!any) return std::nullopt;
        }
        if (at < text.size())
        {
            const char sign = text[at];
            if (sign == 'Z' || sign == 'z')
            {
                ++at;
            }
            else if (sign == '+' || sign == '-')
            {
                ++at;
                int offsetHours = 0;
                int offsetMins = 0;
                if (!infraDigits(text, at, 2, offsetHours)) return std::nullopt;
                infraTake(text, at, ':');
                if (at < text.size() && !infraDigits(text, at, 2, offsetMins)) return std::nullopt;
                offsetMinutes = (sign == '+' ? 1 : -1) * (offsetHours * 60 + offsetMins);
            }
            else
            {
                return std::nullopt;
            }
        }
        if (at != text.size() || hour > 23 || minute > 59 || second > 60) return std::nullopt;
    }
    const auto days = infraDaysFromCivil(year, month, day);
    return ((days * 24 + hour) * 60 + minute - offsetMinutes) * 60000 + static_cast<std::int64_t>(second) * 1000 + millis;
}

std::optional<std::int64_t> infraParseHttpDate(std::string_view text)
{
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto comma = text.find(", ");
    if (comma == std::string_view::npos) return std::nullopt;
    std::size_t at = comma + 2;
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!infraDigits(text, at, 2, day) || !infraTake(text, at, ' ') || at + 3 > text.size()) return std::nullopt;
    int month = 0;
    for (int i = 0; i < 12; ++i)
    {
        if (text.substr(at, 3) == months[i]) month = i + 1;
    }
    at += 3;
    if (month == 0 || !infraTake(text, at, ' ') || !infraDigits(text, at, 4, year) || !infraTake(text, at, ' ')) return std::nullopt;
    if (!infraDigits(text, at, 2, hour) || !infraTake(text, at, ':') || !infraDigits(text, at, 2, minute) || !infraTake(text, at, ':') || !infraDigits(text, at, 2, second)) return std::nullopt;
    if (text.substr(at) != " GMT" || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return std::nullopt;
    const auto days = infraDaysFromCivil(year, month, day);
    return ((days * 24 + hour) * 60 + minute) * 60000 + static_cast<std::int64_t>(second) * 1000;
}

std::string infraUuid(const std::string& b)
{
    const auto hex = toHex(b);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

bool infraLower(char c) { return c >= 'a' && c <= 'z'; }
bool infraUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool infraDigit(char c) { return c >= '0' && c <= '9'; }
}  // namespace

// ── Time ────────────────────────────────────────────────────────────────

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string isoFromMs(std::int64_t ms)
{
    std::int64_t days = ms / 86400000;
    std::int64_t rest = ms % 86400000;
    if (rest < 0)
    {
        rest += 86400000;
        --days;
    }
    std::int64_t year = 0;
    int month = 0;
    int day = 0;
    infraCivilFromDays(days, year, month, day);
    char out[40];
    std::snprintf(out, sizeof out, "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<long long>(year), month, day, static_cast<int>(rest / 3600000), static_cast<int>(rest / 60000 % 60),
                  static_cast<int>(rest / 1000 % 60), static_cast<int>(rest % 1000));
    return out;
}

std::string isoNow()
{
    return isoFromMs(nowMs());
}

std::optional<std::int64_t> parseTimeMs(std::string_view text)
{
    if (text.empty()) return std::nullopt;
    if (infraDigit(text[0])) return infraParseIso(text);
    return infraParseHttpDate(text);
}

void sleepFor(std::chrono::milliseconds duration, const CancelToken& cancel)
{
    const auto until = std::chrono::steady_clock::now() + duration;
    while (true)
    {
        cancel.throwIfCancelled();
        const auto now = std::chrono::steady_clock::now();
        if (now >= until) return;
        std::this_thread::sleep_for((std::min)(std::chrono::duration_cast<std::chrono::milliseconds>(until - now), std::chrono::milliseconds(50)));
    }
}

// ── Ids ─────────────────────────────────────────────────────────────────

std::string uuidV7(std::optional<std::int64_t> atMs)
{
    auto b = randomBytes(16);
    auto ts = static_cast<std::uint64_t>(atMs ? *atMs : nowMs());
    for (int i = 5; i >= 0; --i)
    {
        b[static_cast<std::size_t>(i)] = static_cast<char>(ts & 0xff);
        ts >>= 8;
    }
    b[6] = static_cast<char>((static_cast<unsigned char>(b[6]) & 0x0f) | 0x70);
    b[8] = static_cast<char>((static_cast<unsigned char>(b[8]) & 0x3f) | 0x80);
    return infraUuid(b);
}

std::string uuidV4()
{
    auto b = randomBytes(16);
    b[6] = static_cast<char>((static_cast<unsigned char>(b[6]) & 0x0f) | 0x40);
    b[8] = static_cast<char>((static_cast<unsigned char>(b[8]) & 0x3f) | 0x80);
    return infraUuid(b);
}

std::string randomToken(std::size_t bytes)
{
    return base64Url(randomBytes(bytes));
}

// ── Grammar checks ──────────────────────────────────────────────────────

bool isBuildLabel(std::string_view text)
{
    if (text.empty() || text.size() > 64 || !(infraLower(text[0]) || infraUpper(text[0]) || infraDigit(text[0]))) return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return infraLower(c) || infraUpper(c) || infraDigit(c) || c == '.' || c == '_' || c == '+' || c == '-'; });
}

bool isEventType(std::string_view text)
{
    if (text.empty() || text.size() > 64 || !infraLower(text[0])) return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return infraLower(c) || infraDigit(c) || c == '_'; });
}

bool isStatKey(std::string_view text)
{
    if (text.empty() || text.size() > 64 || !infraLower(text[0])) return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return infraLower(c) || infraDigit(c) || c == '_' || c == '.'; });
}

bool isPairwiseSubject(std::string_view text)
{
    if (!startsWith(text, "pw_")) return false;
    const auto rest = text.substr(3);
    if (rest.size() < 8 || rest.size() > 120) return false;
    return std::all_of(rest.begin(), rest.end(), [](char c) { return infraLower(c) || infraUpper(c) || infraDigit(c) || c == '_' || c == '-'; });
}

bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

std::string trimSpace(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos) return "";
    const auto last = text.find_last_not_of(" \t\r\n\f\v");
    return text.substr(first, last - first + 1);
}

std::size_t utf16Length(const std::string& text)
{
    std::size_t units = 0;
    for (const char c : text)
    {
        const auto b = static_cast<unsigned char>(c);
        if ((b & 0xC0) == 0x80) continue;
        units += b >= 0xF0 ? 2 : 1;
    }
    return units;
}

// ── JSON in a store ─────────────────────────────────────────────────────

std::optional<Json> getJson(KeyValueStore& store, const std::string& key)
{
    const auto text = store.get(key);
    if (!text) return std::nullopt;
    return Json::parse(*text);
}

void putJson(KeyValueStore& store, const std::string& key, const Json& value)
{
    store.set(key, value.dump());
}

Json orNull(const std::optional<std::string>& value)
{
    return value ? Json(*value) : Json();
}

// ── Concurrency ─────────────────────────────────────────────────────────

std::unique_lock<std::mutex> NamedLocks::take(const std::string& name)
{
    static std::mutex registryMutex;
    static std::map<std::string, std::unique_ptr<std::mutex>> registry;
    std::mutex* gate = nullptr;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto& slot = registry[name];
        if (!slot) slot = std::make_unique<std::mutex>();
        gate = slot.get();
    }
    return std::unique_lock<std::mutex>(*gate);
}

Worker::Worker() : thread_([this] { loop(); })
{
}

Worker::~Worker()
{
    stop();
}

void Worker::post(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(task));
        ++posted_;
    }
    wake_.notify_one();
}

void Worker::drain()
{
    if (std::this_thread::get_id() == thread_.get_id()) return;
    std::unique_lock<std::mutex> lock(mutex_);
    const auto target = posted_;
    done_.wait(lock, [this, target] { return finished_ >= target; });
}

void Worker::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) thread_.join();
}

void Worker::loop()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        quietly(task);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++finished_;
        }
        done_.notify_all();
    }
}

Clock::Clock() : launchId_(uuidV7()), started_(std::chrono::steady_clock::now())
{
}

std::int64_t Clock::monotonicMs() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
}

std::optional<std::int64_t> Clock::serverOffsetMs() const
{
    if (!offsetKnown_.load()) return std::nullopt;
    return offsetMs_.load();
}

std::int64_t Clock::serverNowMs() const
{
    return nowMs() + (offsetKnown_.load() ? offsetMs_.load() : 0);
}

void Clock::noteServerTime(const std::optional<std::string>& value)
{
    if (!value || value->empty()) return;
    const auto at = parseTimeMs(*value);
    if (!at) return;
    offsetMs_.store(*at - nowMs());
    offsetKnown_.store(true);
}

// ── The wire ────────────────────────────────────────────────────────────

const Json& Answer::body() const
{
    static const Json empty = Json::object();
    return json ? *json : empty;
}

namespace wire
{
Answer send(HttpTransport& http, Clock& clock, const HttpCall& call)
{
    Answer answer;
    answer.raw = http.send(call);
    answer.status = answer.raw.status;
    clock.noteServerTime(answer.raw.header("date"));
    const auto type = answer.raw.header("content-type").value_or("");
    if (contains(type, "json") && !answer.raw.body.empty())
    {
        auto parsed = Json::parse(answer.raw.body);
        if (parsed && parsed->isObject()) answer.json = std::move(*parsed);
    }
    if (answer.json) clock.noteServerTime(answer.json->getString("serverTime"));
    return answer;
}

Error errorOf(const Answer& answer)
{
    Json json = answer.json ? *answer.json : Json::object();
    auto code = "http_" + std::to_string(answer.status);
    auto message = "The server answered " + std::to_string(answer.status) + ".";
    const auto* error = json.find("error");
    if (error != nullptr && error->isObject())
    {
        if (auto c = error->getString("code")) code = *c;
        if (auto m = error->getString("message")) message = *m;
    }
    else if (error != nullptr && error->isString())
    {
        code = error->asString();
        if (auto description = json.getString("error_description"))
        {
            message = *description;
            json["description"] = *description;
        }
    }
    json.erase("error");
    json.erase("error_description");
    std::optional<double> retryAfter = json.getNumber("retryAfter");
    if (!retryAfter)
    {
        if (const auto header = answer.raw.header("retry-after"))
        {
            const auto parsed = Json::parse(*header);
            if (parsed && parsed->isNumber()) retryAfter = parsed->asDouble();
        }
    }
    return Error(code, message, answer.status, retryAfter, std::move(json));
}

HttpCall form(const std::string& url, const FormFields& fields)
{
    HttpCall call;
    call.method = "POST";
    call.url = url;
    for (const auto& field : fields)
    {
        if (!field.second) continue;
        if (!call.body.empty()) call.body += "&";
        call.body += urlEncode(field.first) + "=" + urlEncode(*field.second);
    }
    call.headers.set("content-type", "application/x-www-form-urlencoded");
    return call;
}
}  // namespace wire

// ── Options ─────────────────────────────────────────────────────────────

Resolved Resolved::from(const Options& o)
{
    if (o.config.appId.empty() || o.config.slug.empty()) throw std::invalid_argument("config must be the app's krtuworks-config.json");
    if (o.environment != "TEST" && o.environment != "LIVE") throw std::invalid_argument("environment is TEST or LIVE");
    if (!isBuildLabel(o.build)) throw std::invalid_argument("build is the app's version label, like 1.4.2+381");
    const auto env = o.config.environments.find(o.environment);
    const EnvironmentClients* clients = env == o.config.environments.end() ? nullptr : &env->second;
    std::optional<std::string> clientId = o.clientId;
    if (!clientId && clients != nullptr) clientId = clients->clientId;
    if (!clientId || clientId->empty()) throw std::invalid_argument("krtuworks-config.json names no client for " + o.environment);
    Resolved r;
    r.appId = o.config.appId;
    r.slug = o.config.slug;
    r.type = o.config.type.empty() ? "GAME" : o.config.type;
    r.environment = o.environment;
    r.platform = o.platform;
    r.build = o.build;
    r.clientId = *clientId;
    if (clients != nullptr) r.deviceClientId = clients->deviceClientId;
    r.idOrigin = o.idOrigin;
    while (!r.idOrigin.empty() && r.idOrigin.back() == '/') r.idOrigin.pop_back();
    r.devOrigin = o.devOrigin;
    while (!r.devOrigin.empty() && r.devOrigin.back() == '/') r.devOrigin.pop_back();
    r.issuer = r.idOrigin + "/oidc";
    r.device = o.device;
    r.locale = o.locale;
    r.sessions = o.sessions;
    r.delay = o.delay ? o.delay : [](std::chrono::milliseconds duration, const CancelToken& cancel) { sleepFor(duration, cancel); };
    return r;
}

std::string moduleOf(const std::string& eventType)
{
    static const std::map<std::string, std::string> families = {
        {"screen_view", "ui"}, {"button", "ui"},           {"tutorial", "ui"},           {"progression", "progression"},
        {"death", "combat"},   {"damage", "combat"},       {"economy", "economy"},       {"balance", "economy"},
        {"item", "items"},     {"interaction", "items"},   {"performance", "performance"}, {"settings", "settings"},
        {"error", "errors"},
    };
    const auto found = families.find(eventType);
    return found == families.end() ? "customEvents" : found->second;
}

void quietly(const std::function<void()>& work)
{
    try
    {
        work();
    }
    catch (...)
    {
        // Background work: the next tick tries again.
    }
}
}  // namespace krtu::detail
