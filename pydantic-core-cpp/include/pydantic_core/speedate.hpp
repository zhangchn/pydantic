#pragma once

// speedate-compatible temporal parsing rules.
//
// This is a behavioural port of the subset of the Rust `speedate` crate that
// pydantic-core relies on (see pydantic-core/src/input/datetime.rs). It is the
// single source of truth for how date/time/datetime strings and numbers are
// interpreted, replacing the three divergent hand-rolled parsers that used to
// live in src/input/{python,json,string}_input.cpp.
//
// Deliberately free of Python bindings: the parsing rules are pure arithmetic,
// so they can be unit-tested without importing CPython.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace pydantic_core {

// ---------------------------------------------------------------------------
// Temporal value types
// ---------------------------------------------------------------------------
//
// Time::tz_offset is expressed in MINUTES rather than speedate's seconds.
// speedate rejects sub-minute UTC offsets outright ("+00:00:30" fails with
// "unexpected extra characters at the end of the input"), so whole minutes are
// lossless, and every Python-object conversion in this codebase already builds
// `datetime.timedelta(minutes=...)`.

struct Date {
    int year = 0;
    int month = 0;
    int day = 0;

    bool operator==(const Date& o) const {
        return year == o.year && month == o.month && day == o.day;
    }
    bool operator!=(const Date& o) const { return !(*this == o); }
};

struct Time {
    int hour = 0;
    int minute = 0;
    int second = 0;
    int microsecond = 0;
    std::optional<int> tz_offset;  // minutes east of UTC; nullopt = no timezone

    bool operator==(const Time& o) const {
        return hour == o.hour && minute == o.minute && second == o.second &&
               microsecond == o.microsecond && tz_offset == o.tz_offset;
    }
    bool operator!=(const Time& o) const { return !(*this == o); }

    bool is_midnight() const {
        return hour == 0 && minute == 0 && second == 0 && microsecond == 0;
    }
};

struct DateTime {
    Date date;
    Time time;

    bool operator==(const DateTime& o) const {
        return date == o.date && time == o.time;
    }
    bool operator!=(const DateTime& o) const { return !(*this == o); }
};

struct Timedelta {
    int days = 0;
    int seconds = 0;
    int microseconds = 0;

    bool operator==(const Timedelta& o) const {
        return days == o.days && seconds == o.seconds && microseconds == o.microseconds;
    }
    bool operator!=(const Timedelta& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
// speedate's Display (date.rs:38-46, time.rs:42-75, datetime.rs:37-44).  This is the text a
// violated temporal bound names its constraint by, in the message and in `ctx` alike, which is
// not what Python's str() writes for the same value: Python puts a space where speedate puts the
// `T` and `UTC+08:00` where speedate puts `+08:00`.  A microsecond field of zero is left out
// entirely; otherwise it takes six digits.  `Time::tz_offset` is minutes here and seconds in
// speedate, so the split into hours and minutes happens at this end.

inline std::string display_date(const Date& d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", d.year, d.month, d.day);
    return std::string(buf);
}

inline std::string display_time(const Time& t) {
    char buf[32];
    if (t.microsecond != 0) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06d", t.hour, t.minute, t.second,
                      t.microsecond);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.minute, t.second);
    }
    std::string out(buf);
    if (t.tz_offset.has_value()) {
        const int total_minutes = *t.tz_offset;
        if (total_minutes == 0) {
            out += "Z";
        } else {
            const int hours = total_minutes / 60;
            const int minutes = total_minutes % 60;
            char tzbuf[8];
            std::snprintf(tzbuf, sizeof(tzbuf), "%c%02d:%02d", total_minutes < 0 ? '-' : '+',
                          hours < 0 ? -hours : hours, minutes < 0 ? -minutes : minutes);
            out += tzbuf;
        }
    }
    return out;
}

inline std::string display_datetime(const DateTime& dt) {
    return display_date(dt.date) + "T" + display_time(dt.time);
}

// How a bare number should be interpreted: as seconds or milliseconds since
// the unix epoch, or left to `Infer`'s watershed. Driven by the
// `val_temporal_unit` model config.
enum class TimestampUnit { Seconds, Milliseconds, Infer };

// Maps the `val_temporal_unit` config string. `ok` is set false for an
// unrecognised name. Defaults to Infer, matching pydantic's own default.
TimestampUnit timestamp_unit_from_string(std::string_view name, bool* ok = nullptr);

// ---------------------------------------------------------------------------
// Result type
// ---------------------------------------------------------------------------
// `error` carries speedate's ParseError::get_documentation() text, which
// pydantic-core interpolates into the `{error}` slot of a message template and
// into `ctx["error"]`. It is empty when `ok` is true.
template <class T>
struct ParseOutcome {
    bool ok = false;
    T value{};
    std::string error;

    static ParseOutcome success(T v) {
        ParseOutcome r;
        r.ok = true;
        r.value = v;
        return r;
    }
    static ParseOutcome failure(std::string message) {
        ParseOutcome r;
        r.error = std::move(message);
        return r;
    }
};

// ---------------------------------------------------------------------------
// Unix timestamp range
// ---------------------------------------------------------------------------
// speedate accepts timestamps inside this inclusive range and reports the two
// documents below for anything outside it. Note the lower bound is 0000-01-01,
// which parses successfully but is later rejected by the Python-object
// conversion with kYearZeroOutOfRange — parsing itself does not fail.
inline constexpr int64_t kMinUnixTimestamp = -62167219200LL;   // 0000-01-01T00:00:00
inline constexpr int64_t kMaxUnixTimestamp =  253402300799LL;  // 9999-12-31T23:59:59

inline constexpr const char* kYearZeroOutOfRange = "year 0 is out of range";
inline constexpr const char* kNanNotAllowed = "NaN values not permitted";

// The watershed used by TimestampUnit::Infer: a value whose magnitude exceeds
// this is milliseconds, otherwise seconds.
inline constexpr int64_t kInferWatershed = 20000000000LL;

// ---------------------------------------------------------------------------
// String parsing
// ---------------------------------------------------------------------------
// The byte-pointer overloads exist so `str` and `bytes` inputs share one code
// path; pydantic-core coerces bytes to the same parser rather than rejecting
// them. None of these accept Python objects — that stays in the Input layer.

// Full date-time: `YYYY-MM-DD[T|t|_| ]HH:MM[:SS[.frac]][Z|±HH[:MM]]`.
// A bare number (or a numeric string) is interpreted as a unix timestamp in
// `unit`; when both readings fail, the string-form error is reported.
// `require_time` asks the stricter question pydantic-core asks of a *text* datetime input
// (bytes_as_datetime → parse_bytes_rfc3339): the time has to be in the text, so a bare
// `2026-05-01` is refused. It changes nothing for a datetime *value*, which is accepted as
// midnight a step later, by the validator's date fallback (datetime.rs:73-79) -- only a caller
// that cannot reach that fallback, such as the reader of a `gt` bound, asks it.
ParseOutcome<DateTime> parse_datetime_bytes(const char* data, size_t len,
                                            TimestampUnit unit = TimestampUnit::Infer,
                                            bool require_time = false);
ParseOutcome<DateTime> parse_datetime_string(std::string_view s,
                                             TimestampUnit unit = TimestampUnit::Infer,
                                             bool require_time = false);

// Date-only convenience over parse_datetime_bytes: returns the date component.
// Callers that must distinguish "not an exact date" should use
// parse_datetime_bytes and inspect DateTime::time directly.
ParseOutcome<Date> parse_date_bytes(const char* data, size_t len,
                                    TimestampUnit unit = TimestampUnit::Infer);
ParseOutcome<Date> parse_date_string(std::string_view s,
                                     TimestampUnit unit = TimestampUnit::Infer);

// Time-only: `HH:MM[:SS[.frac]][Z|±HH[:MM]]`. Unlike date/datetime, a numeric
// string is never treated as a timestamp here — numeric times arrive as int or
// float objects and go through time_from_timestamp.
ParseOutcome<Time> parse_time_bytes(const char* data, size_t len);
ParseOutcome<Time> parse_time_string(std::string_view s);

// ---------------------------------------------------------------------------
// Numeric parsing
// ---------------------------------------------------------------------------
// `microseconds` is always a fraction of a *second* (0..999_999); `unit` only
// qualifies the integer `timestamp`.

ParseOutcome<DateTime> datetime_from_timestamp(int64_t timestamp, uint32_t microseconds,
                                               TimestampUnit unit = TimestampUnit::Infer);
ParseOutcome<DateTime> datetime_from_float(double value,
                                           TimestampUnit unit = TimestampUnit::Infer);

// Time-of-day from seconds since midnight, forced to UTC (speedate's
// `unix_timestamp_offset: Some(0)`).
ParseOutcome<Time> time_from_timestamp(int64_t seconds, uint32_t microseconds = 0);
ParseOutcome<Time> time_from_float(double value);

// ---------------------------------------------------------------------------
// Civil <-> epoch-day arithmetic (proleptic Gregorian)
// ---------------------------------------------------------------------------
// Day 0 is 1970-01-01. Implemented directly rather than via
// `datetime.fromtimestamp`, which cannot represent pre-1970 values portably and
// loses microseconds.
int64_t days_from_civil(int year, int month, int day);
Date civil_from_days(int64_t days);

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------
// speedate's three `PartialOrd` impls, which are not the orderings Python puts on
// `date`/`time`/`datetime`. Two values that both carry an offset compare as instants
// (seconds since the epoch minus the offset, tie-broken on microsecond); if either side
// has no offset, the naive one is taken to share the other's zone, which speedate gets by
// comparing the date and then the wall-clock time-of-day and never looking at an offset
// (datetime.rs:125-133, time.rs:129-140, date.rs `Ord`). Comparing the Python objects
// instead is not the same question: Python refuses a naive/aware pair outright.
// `Time::tz_offset` is minutes here and seconds in speedate, so the offset enters the
// arithmetic scaled at this end.

inline int cmp_date(const Date& a, const Date& b) {
    if (a.year != b.year) return a.year < b.year ? -1 : 1;
    if (a.month != b.month) return a.month < b.month ? -1 : 1;
    if (a.day != b.day) return a.day < b.day ? -1 : 1;
    return 0;
}

inline int cmp_time(const Time& a, const Time& b) {
    const int64_t sa = a.hour * 3600LL + a.minute * 60LL + a.second;
    const int64_t sb = b.hour * 3600LL + b.minute * 60LL + b.second;
    int64_t va = sa, vb = sb;
    if (a.tz_offset && b.tz_offset) {
        va -= *a.tz_offset * 60LL;
        vb -= *b.tz_offset * 60LL;
    }
    if (va != vb) return va < vb ? -1 : 1;
    if (a.microsecond != b.microsecond) return a.microsecond < b.microsecond ? -1 : 1;
    return 0;
}

inline int cmp_datetime(const DateTime& a, const DateTime& b) {
    if (a.time.tz_offset && b.time.tz_offset) {
        const int64_t ta = days_from_civil(a.date.year, a.date.month, a.date.day) * 86400LL +
                            a.time.hour * 3600LL + a.time.minute * 60LL + a.time.second -
                            *a.time.tz_offset * 60LL;
        const int64_t tb = days_from_civil(b.date.year, b.date.month, b.date.day) * 86400LL +
                            b.time.hour * 3600LL + b.time.minute * 60LL + b.time.second -
                            *b.time.tz_offset * 60LL;
        if (ta != tb) return ta < tb ? -1 : 1;
        if (a.time.microsecond != b.time.microsecond)
            return a.time.microsecond < b.time.microsecond ? -1 : 1;
        return 0;
    }
    int by_date = cmp_date(a.date, b.date);
    if (by_date) return by_date;
    return cmp_time(a.time, b.time);
}

// Overload set for code that is generic over which temporal type it holds.
inline int temporal_cmp(const Date& a, const Date& b) { return cmp_date(a, b); }
inline int temporal_cmp(const Time& a, const Time& b) { return cmp_time(a, b); }
inline int temporal_cmp(const DateTime& a, const DateTime& b) { return cmp_datetime(a, b); }

}  // namespace pydantic_core
