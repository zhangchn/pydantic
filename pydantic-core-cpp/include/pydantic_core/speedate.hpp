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
ParseOutcome<DateTime> parse_datetime_bytes(const char* data, size_t len,
                                            TimestampUnit unit = TimestampUnit::Infer);
ParseOutcome<DateTime> parse_datetime_string(std::string_view s,
                                             TimestampUnit unit = TimestampUnit::Infer);

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

}  // namespace pydantic_core
