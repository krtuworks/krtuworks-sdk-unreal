#include "KrtuworksCore/UiFormat.h"

#include <cctype>
#include <ctime>
#include <set>

#include "Infrastructure.h"

namespace krtu
{
namespace
{
constexpr int oldestYears = 120;
constexpr std::int64_t dayMs = 86400000;

struct Civil
{
    int year;
    int month;
    int day;
};

/// Days since 1970-01-01 of a civil date (proleptic Gregorian).
std::int64_t daysFromCivil(int year, int month, int day)
{
    const std::int64_t y = year - (month <= 2 ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

Civil civilFromDays(std::int64_t days)
{
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    return {year, month, day};
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b)
{
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

Civil civilOf(std::int64_t ms)
{
    return civilFromDays(floorDiv(ms, dayMs));
}

std::string pad(std::int64_t value, int width)
{
    std::string digits = std::to_string(value);
    while (static_cast<int>(digits.size()) < width) digits.insert(digits.begin(), '0');
    return digits;
}

/// `yyyy-MM-dd` as numbers; nothing for any other text.
bool parseIsoDate(const std::string& text, Civil& out)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (i != 4 && i != 7 && !std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    out = {std::stoi(text.substr(0, 4)), std::stoi(text.substr(5, 2)), std::stoi(text.substr(8, 2))};
    return true;
}

std::string monthName(const UiTexts& texts, int month)
{
    return textFor(texts, "month." + std::to_string(month));
}
}  // namespace

DateOrder dateOrder(const std::string& locale)
{
    static const std::set<std::string> mdyRegions = {"US", "PH", "FM", "MH", "PW", "AS", "GU", "MP", "PR", "VI", "UM"};
    static const std::set<std::string> ymdLanguages = {"zh", "ja", "ko", "hu", "lt", "mn"};
    static const std::set<std::string> ymdRegions = {"CN", "TW", "JP", "KR", "KP", "HU", "LT", "MN"};
    std::vector<std::string> parts;
    std::string part;
    for (const char c : locale)
    {
        if (c == '-' || c == '_')
        {
            parts.push_back(part);
            part.clear();
        }
        else
        {
            part += c;
        }
    }
    parts.push_back(part);
    std::string language = parts[0];
    for (char& c : language) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string region;
    for (std::size_t i = 1; i < parts.size() && region.empty(); ++i)
    {
        if (parts[i].size() == 2 && std::isalpha(static_cast<unsigned char>(parts[i][0])) && std::isalpha(static_cast<unsigned char>(parts[i][1])))
        {
            region = parts[i];
            for (char& c : region) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    if (mdyRegions.count(region) > 0 || (language == "en" && region.empty())) return DateOrder::Mdy;
    if (ymdRegions.count(region) > 0 || ymdLanguages.count(language) > 0) return DateOrder::Ymd;
    return DateOrder::Dmy;
}

int daysInMonth(int year, int month)
{
    return static_cast<int>(daysFromCivil(month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1) - daysFromCivil(year, month, 1));
}

std::string isoDate(int year, int month, int day)
{
    return pad(year, 4) + "-" + pad(month, 2) + "-" + pad(day, 2);
}

int ageOn(const std::string& birthDate, std::int64_t nowMs)
{
    Civil born{};
    if (!parseIsoDate(birthDate, born)) return 0;
    const Civil today = civilOf(nowMs);
    return today.year - born.year - (today.month < born.month || (today.month == born.month && today.day < born.day) ? 1 : 0);
}

BirthCheck checkBirth(int year, int month, int day, std::int64_t nowMs)
{
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) return BirthCheck::Invalid;
    const int age = ageOn(isoDate(year, month, day), nowMs);
    if (age < 0 || age > oldestYears) return BirthCheck::Invalid;
    return age < minAge ? BirthCheck::Underage : BirthCheck::Ok;
}

std::vector<int> birthYears(std::int64_t nowMs)
{
    const int now = civilOf(nowMs).year;
    std::vector<int> years;
    for (int i = 0; i <= oldestYears; ++i) years.push_back(now - i);
    return years;
}

std::string turnsThirteen(const std::string& birthDate)
{
    Civil born{};
    if (!parseIsoDate(birthDate, born)) return birthDate;
    const int year = born.year + minAge;
    return born.day > daysInMonth(year, born.month) ? isoDate(year, born.month + 1, 1) : isoDate(year, born.month, born.day);
}

std::string utcDay(std::int64_t nowMs, int plusDays)
{
    const Civil day = civilOf(nowMs + static_cast<std::int64_t>(plusDays) * dayMs);
    return isoDate(day.year, day.month, day.day);
}

std::string shortAddress(const std::string& url)
{
    std::string out = url;
    for (const char* scheme : {"https://", "http://"})
    {
        if (out.rfind(scheme, 0) == 0) out.erase(0, std::string(scheme).size());
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

std::string engineImage(const std::string& url)
{
    const std::string scheme = "https://";
    const std::string folder = "/avatars/";
    const std::string tail = ".webp";
    if (url.rfind(scheme, 0) != 0 || url.size() < tail.size() || url.compare(url.size() - tail.size(), tail.size(), tail) != 0) return url;
    const std::size_t at = url.find(folder, scheme.size());
    if (at == std::string::npos || url.find_first_of("?#", scheme.size()) != std::string::npos) return url;
    if (url.find('/', scheme.size()) != at) return url;
    std::string name = url.substr(at + folder.size(), url.size() - tail.size() - at - folder.size());
    if (name.size() == 68 && name.compare(64, 4, "-128") == 0) name.resize(64);
    if (name.size() != 64) return url;
    for (const char c : name)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return url;
    }
    return url.substr(0, url.size() - tail.size()) + ".png";
}

std::string fileSize(std::int64_t bytes)
{
    constexpr std::int64_t kb = 1024;
    constexpr std::int64_t mb = 1024 * 1024;
    if (bytes < kb) return std::to_string(bytes) + " B";
    if (bytes < mb) return std::to_string((bytes + kb / 2) / kb) + " KB";
    const std::int64_t tenths = (bytes * 10 + mb / 2) / mb;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
}

std::string countdown(std::int64_t remainingMs)
{
    const std::int64_t seconds = remainingMs <= 0 ? 0 : (remainingMs + 999) / 1000;
    return std::to_string(seconds / 60) + ":" + pad(seconds % 60, 2);
}

int systemUtcOffsetMinutes(std::int64_t unixMs)
{
    const std::time_t at = static_cast<std::time_t>(floorDiv(unixMs, 1000));
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &at) != 0) return 0;
#else
    if (localtime_r(&at, &local) == nullptr) return 0;
#endif
    const std::int64_t localSeconds = daysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    return static_cast<int>((localSeconds - static_cast<std::int64_t>(at)) / 60);
}

std::string longDate(std::int64_t unixMs, int utcOffsetMinutes, const std::string& locale, const UiTexts& texts)
{
    const Civil day = civilOf(unixMs + static_cast<std::int64_t>(utcOffsetMinutes) * 60000);
    switch (dateOrder(locale))
    {
    case DateOrder::Mdy:
        return monthName(texts, day.month) + " " + std::to_string(day.day) + ", " + std::to_string(day.year);
    case DateOrder::Ymd:
        return isoDate(day.year, day.month, day.day);
    default:
        return std::to_string(day.day) + " " + monthName(texts, day.month) + " " + std::to_string(day.year);
    }
}

std::string savedWhen(const std::string& iso, std::int64_t nowMs, int utcOffsetMinutes, int nowOffsetMinutes, const std::string& locale, const UiTexts& texts)
{
    const auto at = detail::parseTimeMs(iso);
    if (!at) return "";
    const std::int64_t local = *at + static_cast<std::int64_t>(utcOffsetMinutes) * 60000;
    const std::int64_t minuteOfDay = floorDiv(local, 60000) - floorDiv(local, dayMs) * 1440;
    const std::string time = pad(minuteOfDay / 60, 2) + ":" + pad(minuteOfDay % 60, 2);
    const std::int64_t daysAgo = floorDiv(nowMs + static_cast<std::int64_t>(nowOffsetMinutes) * 60000, dayMs) - floorDiv(local, dayMs);
    if (daysAgo == 0) return textFor(texts, "time.today", {{"time", time}});
    if (daysAgo == 1) return textFor(texts, "time.yesterday", {{"time", time}});
    return longDate(*at, utcOffsetMinutes, locale, texts) + (dateOrder(locale) == DateOrder::Ymd ? " " : ", ") + time;
}
}  // namespace krtu
