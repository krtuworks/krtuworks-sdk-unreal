#pragma once

#include "KrtuworksCore/Export.h"

#include <cstdint>
#include <string>
#include <vector>

#include "KrtuworksCore/UiText.h"

namespace krtu
{
/// Krtuworks ID is for people aged 13 or older; the server refuses younger guests the same way.
constexpr int minAge = 13;

/// The order people write a date in: day first, month first or year first.
enum class DateOrder
{
    Dmy,
    Mdy,
    Ymd
};

/// The order people in `locale` (`en-US`, `ja`, `tr_TR`) write a date in: `en-US` month first, `ja` year first, most day first.
KRTU_API DateOrder dateOrder(const std::string& locale);

KRTU_API int daysInMonth(int year, int month);
/// `2026-06-01`.
KRTU_API std::string isoDate(int year, int month, int day);

/// Whole years between `birthDate` (`yyyy-MM-dd`) and the day `nowMs` (milliseconds since 1970) falls on in UTC, the way the server counts them.
KRTU_API int ageOn(const std::string& birthDate, std::int64_t nowMs);

enum class BirthCheck
{
    Ok,
    Invalid,
    Underage
};

/// The age screen's answer: a real date, no later than today and no more than 120 years back, 13 or older.
KRTU_API BirthCheck checkBirth(int year, int month, int day, std::int64_t nowMs);

/// The years the age screen offers: this year back to 120 years ago, newest first.
KRTU_API std::vector<int> birthYears(std::int64_t nowMs);

/// The first day someone born on `birthDate` is 13 (a 29 February birthday counts from 1 March).
KRTU_API std::string turnsThirteen(const std::string& birthDate);

/// The UTC day `nowMs` falls on, as `yyyy-MM-dd`, `plusDays` later.
KRTU_API std::string utcDay(std::int64_t nowMs, int plusDays = 0);

/// An address as the screen shows it: without `https://` and a closing `/` (`id.krtuworks.com/link`).
KRTU_API std::string shortAddress(const std::string& url);

/// A Krtuworks ID profile picture's address as PNG (the engine cannot read webp); any other address as it is.
KRTU_API std::string engineImage(const std::string& url);

/// `12 KB`, `1.4 MB`.
KRTU_API std::string fileSize(std::int64_t bytes);

/// A code's time left as `m:ss`, counted up to the whole second and never below `0:00`.
KRTU_API std::string countdown(std::int64_t remainingMs);

/// How far this device's local time is ahead of UTC at the moment `unixMs`, in minutes.
KRTU_API int systemUtcOffsetMinutes(std::int64_t unixMs);

/// A date for a sentence: `1 July 2026` (day first), `July 1, 2026` (month first), `2026-07-01` (year first), month names from `texts`.
KRTU_API std::string longDate(std::int64_t unixMs, int utcOffsetMinutes, const std::string& locale, const UiTexts& texts);

/// When a save was made: `Today, 21:40`, `Yesterday, 23:15`, else the date and the time (24-hour) in `locale`'s date order.
/// `utcOffsetMinutes` is the device's offset from UTC at that time. Empty when `iso` is not a time.
KRTU_API std::string savedWhen(const std::string& iso, std::int64_t nowMs, int utcOffsetMinutes, int nowOffsetMinutes, const std::string& locale, const UiTexts& texts);
}  // namespace krtu
