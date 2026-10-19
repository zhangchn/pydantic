#pragma once

#include <variant>
#include <string>
#include <optional>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include "speedate.hpp"
#include "types.hpp"
#include "error_types.hpp"
#include "errors.hpp"
#include "result.hpp"
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace pydantic_core {

// Forward declarations
class PythonInput;
class JsonInput;
class StringInput;
class ValidationState;
struct Location;

// Rust renders a JSON float with `f64::to_string`: the shortest digits that
// round-trip, always written out positionally ("42", "0.0000001"). Python's
// repr would give "42.0" and "1e-07", so a JSON-sourced float must not be
// stringified with str().
inline std::string rust_float_to_string(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    char buf[40];
    std::string digits;
    long point = 0;
    for (int precision = 0; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*e", precision, value);
        if (std::strtod(buf, nullptr) != value) continue;
        // Split "[-]d.dddde[+-]dd" into its significant digits and the
        // position of the decimal point.
        const char* mantissa = buf[0] == '-' ? buf + 1 : buf;
        long exponent = std::strtol(std::strchr(buf, 'e') + 1, nullptr, 10);
        for (const char* it = mantissa; *it != 'e'; ++it) {
            if (*it == '.') continue;
            digits.push_back(*it);
        }
        point = exponent + 1;
        break;
    }
    if (digits.empty()) {
        std::snprintf(buf, sizeof(buf), "%.17g", value);
        return buf;
    }
    std::string out;
    if (std::signbit(value)) out.push_back('-');
    if (point <= 0) {
        out += "0.";
        out.append(static_cast<size_t>(-point), '0');
        out += digits;
    } else if (point >= static_cast<long>(digits.size())) {
        out += digits;
        out.append(static_cast<size_t>(point - static_cast<long>(digits.size())), '0');
    } else {
        out += digits.substr(0, static_cast<size_t>(point));
        out += ".";
        out += digits.substr(static_cast<size_t>(point));
    }
    return out;
}

// Either types - union types for validated values
// These represent the different possible representations of a value

// String representation
struct EitherString {
    std::variant<std::string, std::string_view> value;
    
    EitherString(const std::string& s) : value(s) {}
    EitherString(std::string_view s) : value(s) {}
    
    std::string to_string() const {
        if (auto* s = std::get_if<std::string>(&value)) {
            return *s;
        }
        return std::string(std::get<std::string_view>(value));
    }
    
    std::string_view as_view() const {
        if (auto* s = std::get_if<std::string>(&value)) {
            return *s;
        }
        return std::get<std::string_view>(value);
    }
    
    std::string as_cow() const { return to_string(); }
};

// Bytes representation
struct EitherBytes {
    std::variant<std::vector<uint8_t>, std::string_view> data;
    
    EitherBytes(const std::vector<uint8_t>& b) : data(b) {}
    EitherBytes(std::string_view b) : data(b) {}
    
    // Set when the value came from a Python bytes subclass: Rust returns the
    // input object itself, so the subclass has to survive to the output.
    // Declared after `data` so the existing layout-based casts keep working.
    py::object original;

    std::vector<uint8_t> to_vector() const {
        if (auto* v = std::get_if<std::vector<uint8_t>>(&data)) {
            return *v;
        }
        auto sv = std::get<std::string_view>(data);
        return std::vector<uint8_t>(sv.begin(), sv.end());
    }
    
    size_t size() const {
        if (auto* v = std::get_if<std::vector<uint8_t>>(&data)) {
            return v->size();
        }
        return std::get<std::string_view>(data).size();
    }
};

// Integer representation
struct EitherInt {
    // The py::object alternative holds a Python int for values beyond
    // uint64 range (Rust EitherInt::BigInt) — arbitrary precision.
    std::variant<int64_t, uint64_t, py::object> value;
    
    EitherInt(int64_t i) : value(i) {}
    EitherInt(uint64_t u) : value(u) {}
    EitherInt(py::object o) : value(std::move(o)) {}
    
    bool is_python() const { return std::holds_alternative<py::object>(value); }
    const py::object& as_python() const { return std::get<py::object>(value); }
    
    // Python int value (any representation) — used for result conversion
    py::object to_python() const {
        if (auto* i = std::get_if<int64_t>(&value)) return py::int_(*i);
        if (auto* u = std::get_if<uint64_t>(&value)) {
            return py::reinterpret_steal<py::object>(PyLong_FromUnsignedLongLong(*u));
        }
        return std::get<py::object>(value);
    }
    
    std::optional<int64_t> as_i64() const {
        if (auto* i = std::get_if<int64_t>(&value)) {
            return *i;
        }
        if (auto* u = std::get_if<uint64_t>(&value)) {
            if (*u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return static_cast<int64_t>(*u);
            }
        }
        return std::nullopt;
    }
    
    std::optional<uint64_t> as_u64() const {
        if (auto* u = std::get_if<uint64_t>(&value)) {
            return *u;
        }
        if (auto* i = std::get_if<int64_t>(&value)) {
            if (*i >= 0) {
                return static_cast<uint64_t>(*i);
            }
        }
        return std::nullopt;
    }
};

// Float representation
struct EitherFloat {
    double value;

    EitherFloat(double f) : value(f) {}

    double as_double() const { return value; }
};

// Either types for date/time validation results.
// Rust's python input hands back the object it was given whenever the input already is
// the right type -- each object arm clones it (input_python.rs:603-607 for time,
// :638-642 for datetime) -- and reads the fields only to answer constraints.  So
// `original_obj` is what the value *is*; the struct fields are the working copy.  A
// subclass goes out as the subclass too, which is why the arms also report strict rather
// than exact for a non-exact type.
struct EitherDate {
    Date value;
    bool is_lax = false;
    py::object original_obj = py::none();

    EitherDate() : value{0, 0, 0} {}
    explicit EitherDate(Date d) : value(d) {}

    Date as_raw() const { return value; }
};

struct EitherTime {
    Time value;
    bool is_lax = false;
    py::object original_obj = py::none();

    EitherTime() : value{0, 0, 0} {}
    explicit EitherTime(Time t) : value(t) {}

    Time as_raw() const { return value; }
};

struct EitherDateTime {
    DateTime value;
    bool is_lax = false;
    // When the input was already a Python datetime object, keep the original
    // object so its tzinfo (e.g. a named zone like America/Los_Angeles) is
    // preserved instead of being replaced by a fixed UTC offset.
    py::object original_obj = py::none();

    EitherDateTime() : value{{0, 0, 0}, {0, 0, 0}} {}
    explicit EitherDateTime(DateTime dt) : value(dt) {}

    DateTime as_raw() const { return value; }
};

struct EitherTimedelta {
    Timedelta value;
    bool is_lax = false;
    py::object original_obj = py::none();

    EitherTimedelta() : value{} {}
    explicit EitherTimedelta(Timedelta td) : value(td) {}

    Timedelta as_raw() const { return value; }
};


// How big a duration may be, measured against speedate 0.17.0:
//   * every quantity is held in a u32 of its own unit -- days for Y/M/W/D, seconds for H/M/S --
//     so a component whose value times that unit passes 2**32-1 is refused as "a numeric value in
//     the duration is too large" however small the total would have come out ('PT1193047H' fails,
//     'PT1193046H' does not).
//   * the total then has to fit the 999,999,999 days a timedelta can hold.
// Skipping either lets the components wrap (the port handed back a negative 579 million-day
// duration for 'P99999999999999999999Y') or makes the timedelta build fail, which the result
// table's catch-all turns into None.
inline constexpr long long TD_MAX_DAYS = 999999999LL;
inline constexpr long long TD_SECONDS_IN_DAY = 86400LL;
inline constexpr long long TD_MAX_SECONDS = (TD_MAX_DAYS + 1) * TD_SECONDS_IN_DAY;  // 1e9 days
inline constexpr const char* TD_TOO_LARGE = "a numeric value in the duration is too large";
inline constexpr const char* TD_TOO_MANY_DAYS = "durations may not exceed 999,999,999 days";
inline constexpr const char* TD_NAN = "NaN values not permitted";

namespace timedelta_detail {

// speedate's DurationParseError descriptions (lib.rs:67-150), which map_timedelta_err
// (input/datetime.rs:684) drops into the "{error}" slot of "Input should be a valid timedelta,
// {error}".  Each one names the step the reader stopped at, so the refusal of 'P1D2' is not the
// refusal of 'P1D ', and a port that collapses them answers with a sentence the reference never
// produces.
inline constexpr const char* TD_TOO_SHORT = "input is too short";
inline constexpr const char* TD_EXTRA_CHARS = "unexpected extra characters at the end of the input";
inline constexpr const char* TD_INVALID_NUMBER = "invalid digit in duration";
inline constexpr const char* TD_T_REPEATED = "`t` character repeated in duration";
inline constexpr const char* TD_INVALID_FRACTION = "quantity fraction invalid in duration";
inline constexpr const char* TD_INVALID_TIME_UNIT = "quantity invalid in time part of duration";
inline constexpr const char* TD_INVALID_DATE_UNIT = "quantity invalid in date part of duration";
inline constexpr const char* TD_INVALID_DAYS =
    "\"day\" identifier in duration not correctly formatted";
inline constexpr const char* TD_HOURS_TOO_LARGE = "durations may not exceed 999,999,999 hours";
inline constexpr const char* TD_CHAR_HOUR = "invalid character in hour";
inline constexpr const char* TD_CHAR_MINUTE = "invalid character in minute";
inline constexpr const char* TD_CHAR_SECOND = "invalid character in second";
inline constexpr const char* TD_CHAR_TIME_SEP = "invalid time separator, expected `:`";
inline constexpr const char* TD_HOUR_RANGE = "hour value is outside expected range of 0-23";
inline constexpr const char* TD_MINUTE_RANGE = "minute value is outside expected range of 0-59";
inline constexpr const char* TD_SECOND_RANGE = "second value is outside expected range of 0-59";
inline constexpr const char* TD_FRACTION_MISSING = "second fraction digits missing after `.`";

// speedate's `checked!` macro: the u32 arithmetic above, refusing rather than wrapping.
inline bool u32_add(unsigned& total, unsigned add, const char** err) {
    if (add > 0xFFFFFFFFu - total) {
        *err = TD_TOO_LARGE;
        return false;
    }
    total += add;
    return true;
}

inline bool u32_mul(unsigned a, unsigned b, unsigned& out, const char** err) {
    if (b != 0 && a > 0xFFFFFFFFu / b) {
        *err = TD_TOO_LARGE;
        return false;
    }
    out = a * b;
    return true;
}

// speedate's parse_number: a digit run in a u32, with `pos` left on the first byte that is not a
// digit -- the designator the caller has to find there is what says which part went wrong.
inline bool read_number(const std::string& b, size_t& pos, unsigned& value, const char** err) {
    if (pos >= b.size()) {
        *err = TD_TOO_SHORT;
        return false;
    }
    if (b[pos] < '0' || b[pos] > '9') {
        *err = TD_INVALID_NUMBER;
        return false;
    }
    value = static_cast<unsigned>(b[pos] - '0');
    ++pos;
    for (;;) {
        if (pos >= b.size() || b[pos] < '0' || b[pos] > '9') return true;
        unsigned shifted = 0;
        if (!u32_mul(value, 10, shifted, err)) return false;
        value = shifted;
        if (!u32_add(value, static_cast<unsigned>(b[pos] - '0'), err)) return false;
        ++pos;
    }
}

// speedate's parse_number_frac: the quantity in front of a designator and the one '.' (or ',')
// that may follow it.  'PT1.S' carries a fraction of nothing, which is why it reads as a second
// while 'PT.5S' is the digit that never arrived.
inline bool read_number_frac(const std::string& b, size_t& pos, unsigned& value,
                             bool& has_fraction, double& fraction, const char** err) {
    if (!read_number(b, pos, value, err)) return false;
    has_fraction = false;
    fraction = 0.0;
    if (pos < b.size() && (b[pos] == '.' || b[pos] == ',')) {
        double decimal = 0.0;
        double denominator = 1.0;
        for (;;) {
            ++pos;
            if (pos >= b.size() || b[pos] < '0' || b[pos] > '9') break;
            decimal = decimal * 10.0 + static_cast<double>(b[pos] - '0');
            denominator *= 10.0;
        }
        has_fraction = true;
        fraction = decimal / denominator;
    }
    return true;
}

// speedate's PureTime::parse (time.rs:492): two digits per field, the fraction belonging to the
// third field only and truncated past the sixth -- pydantic-core asks for Truncate -- with
// `position` left where the read stopped so the caller can call whatever follows extra.
inline bool parse_pure_time(const std::string& b, size_t offset,
                            unsigned& hour, unsigned& minute, unsigned& second, unsigned& micro,
                            size_t& position, const char** err) {
    auto digit = [&](size_t i, const char* which, unsigned& out) -> bool {
        if (i >= b.size()) {
            *err = which;
            return false;
        }
        const char c = b[i];
        if (c < '0' || c > '9') {
            *err = which;
            return false;
        }
        out = static_cast<unsigned>(c - '0');
        return true;
    };
    if (b.size() < offset + 5) {
        *err = TD_TOO_SHORT;
        return false;
    }
    unsigned h1 = 0, h2 = 0, m1 = 0, m2 = 0;
    if (!digit(offset, TD_CHAR_HOUR, h1)) return false;
    if (!digit(offset + 1, TD_CHAR_HOUR, h2)) return false;
    hour = h1 * 10 + h2;
    if (b[offset + 2] != ':') {
        *err = TD_CHAR_TIME_SEP;
        return false;
    }
    if (!digit(offset + 3, TD_CHAR_MINUTE, m1)) return false;
    if (!digit(offset + 4, TD_CHAR_MINUTE, m2)) return false;
    minute = m1 * 10 + m2;
    if (hour > 23) {
        *err = TD_HOUR_RANGE;
        return false;
    }
    if (minute > 59) {
        *err = TD_MINUTE_RANGE;
        return false;
    }

    size_t length = 5;
    second = 0;
    micro = 0;
    if (offset + 5 < b.size() && b[offset + 5] == ':') {
        unsigned s1 = 0, s2 = 0;
        if (!digit(offset + 6, TD_CHAR_SECOND, s1)) return false;
        if (!digit(offset + 7, TD_CHAR_SECOND, s2)) return false;
        second = s1 * 10 + s2;
        if (second > 59) {
            *err = TD_SECOND_RANGE;
            return false;
        }
        length = 8;
        const char sep = offset + 8 < b.size() ? b[offset + 8] : '\0';
        if (sep == '.' || sep == ',') {
            length = 9;
            size_t digits = 0;
            for (;;) {
                const size_t i = offset + length + digits;
                if (i >= b.size() || b[i] < '0' || b[i] > '9') break;
                if (digits < 6) micro = micro * 10 + static_cast<unsigned>(b[i] - '0');
                ++digits;
            }
            if (digits == 0) {
                *err = TD_FRACTION_MISSING;
                return false;
            }
            for (size_t i = digits; i < 6; ++i) micro *= 10;
            length += digits;
        }
    }
    position = offset + length;
    return true;
}

// speedate's parse_time (duration.rs:395): everything up to the first ':' is the hour -- so
// '123:00:00' is five days and three hours, and an hour of ten digits is still a duration -- and
// what follows is read as a time with '00:' put in front of it, which is how '10:10' comes to ten
// hours ten minutes while '1:23:4.5' blames the second field.
inline bool parse_time_form(const std::string& b, unsigned& day, unsigned& second,
                            unsigned& micro, const char** err) {
    if (b.size() < 5) {
        *err = TD_TOO_SHORT;
        return false;
    }
    const size_t colon = b.find(':');
    if (colon == std::string::npos) {
        *err = TD_CHAR_HOUR;
        return false;
    }
    if (colon > 10) {
        *err = TD_HOURS_TOO_LARGE;
        return false;
    }
    long long hour = 0;
    for (size_t i = 0; i < colon; ++i) {
        if (b[i] < '0' || b[i] > '9') {
            *err = TD_CHAR_HOUR;
            return false;
        }
        hour = hour * 10 + (b[i] - '0');
    }
    if (hour > 2400000000LL) {
        *err = TD_HOURS_TOO_LARGE;
        return false;
    }
    const std::string buf = "00:" + b.substr(colon + 1);
    unsigned h = 0, m = 0, s = 0, mu = 0;
    size_t position = 0;
    if (!parse_pure_time(buf, 0, h, m, s, mu, position, err)) return false;
    if (buf.size() > position) {
        *err = TD_EXTRA_CHARS;
        return false;
    }
    day = static_cast<unsigned>(hour) / 24;
    second = h * 3600 + m * 60 + s + (static_cast<unsigned>(hour) % 24) * 3600;
    micro = mu;
    return true;
}

// The reference spells the day unit's tail exactly, in either case: 'ays', 'ay', 'AYS', 'AY'.
inline bool has_word(const std::string& b, size_t at, const char* word) {
    const size_t n = std::strlen(word);
    if (at + n > b.size()) return false;
    for (size_t i = 0; i < n; ++i) {
        if (b[at + i] != word[i]) return false;
    }
    return true;
}

// speedate's parse_days_time (duration.rs:344): "<n> d[ay|ays]" with at most one space before the
// unit and one ',' or ' ' after it, and a time that may be left off entirely.  A bare '1' is
// refused here, by the day identifier it never got.
inline bool parse_days_time(const std::string& b, unsigned& day, unsigned& second,
                            unsigned& micro, const char** err) {
    size_t pos = 0;
    if (!read_number(b, pos, day, err)) return false;
    size_t after = pos;
    if (after < b.size() && b[after] == ' ') ++after;
    if (after >= b.size() || (b[after] != 'd' && b[after] != 'D')) {
        *err = TD_INVALID_DAYS;
        return false;
    }
    ++after;
    if (has_word(b, after, "ays")) {
        after += 3;
    } else if (has_word(b, after, "ay")) {
        after += 2;
    } else if (has_word(b, after, "AYS")) {
        after += 3;
    } else if (has_word(b, after, "AY")) {
        after += 2;
    } else if (after < b.size() && (b[after] == 'a' || b[after] == 'A')) {
        *err = TD_INVALID_DAYS;
        return false;
    }
    if (after + 1 < b.size() && b[after] == ',' && b[after + 1] == ' ') {
        after += 2;
    } else if (after < b.size() && (b[after] == ',' || b[after] == ' ')) {
        after += 1;
    }
    if (after >= b.size()) {
        second = 0;
        micro = 0;
        return true;
    }
    unsigned tail_day = 0;
    if (!parse_time_form(b.substr(after), tail_day, second, micro, err)) return false;
    if (tail_day > 0) {
        // The time after a days prefix carries hours only: '1d,24:00:00' is not two days.
        *err = TD_HOURS_TOO_LARGE;
        return false;
    }
    return true;
}

// speedate's parse_iso_duration (duration.rs:275) over the bytes behind the 'P': one designator
// per component, one 'T' splitting the date part from the time part, a fraction only on the last
// quantity, and at least one component to have earned the name.
inline bool parse_iso(const std::string& b, unsigned& day, unsigned& second, unsigned& micro,
                      const char** err) {
    bool got_t = false;
    bool last_had_fraction = false;
    size_t position = 0;
    day = 0;
    second = 0;
    micro = 0;
    for (;;) {
        if (position >= b.size()) break;
        const char c = b[position];
        if (c == 'T') {
            if (got_t) {
                *err = TD_T_REPEATED;
                return false;
            }
            got_t = true;
        } else {
            unsigned value = 0;
            bool has_fraction = false;
            double fraction = 0.0;
            if (!read_number_frac(b, position, value, has_fraction, fraction, err)) return false;
            if (last_had_fraction) {
                *err = TD_INVALID_FRACTION;
                return false;
            }
            if (has_fraction) last_had_fraction = true;
            const char unit = position < b.size() ? b[position] : '\0';
            if (got_t) {
                unsigned mult = 0;
                switch (unit) {
                    case 'H': mult = 3600; break;
                    case 'M': mult = 60; break;
                    case 'S': mult = 1; break;
                    default:
                        *err = TD_INVALID_TIME_UNIT;
                        return false;
                }
                unsigned scaled = 0;
                if (!u32_mul(mult, value, scaled, err)) return false;
                if (!u32_add(second, scaled, err)) return false;
                if (has_fraction) {
                    const double extra = fraction * static_cast<double>(mult);
                    const double whole = std::trunc(extra);
                    if (!u32_add(second, static_cast<unsigned>(whole), err)) return false;
                    const unsigned micro_extra =
                        static_cast<unsigned>(std::round((extra - whole) * 1000000.0));
                    if (!u32_add(micro, micro_extra, err)) return false;
                }
            } else {
                unsigned mult = 0;
                switch (unit) {
                    case 'Y': mult = 365; break;
                    case 'M': mult = 30; break;
                    case 'W': mult = 7; break;
                    case 'D': mult = 1; break;
                    default:
                        *err = TD_INVALID_DATE_UNIT;
                        return false;
                }
                unsigned scaled = 0;
                if (!u32_mul(mult, value, scaled, err)) return false;
                if (!u32_add(day, scaled, err)) return false;
                if (has_fraction) {
                    const double extra_days = fraction * static_cast<double>(mult);
                    const double whole_days = std::trunc(extra_days);
                    if (!u32_add(day, static_cast<unsigned>(whole_days), err)) return false;
                    const double extra_seconds = (extra_days - whole_days) * 86400.0;
                    const double whole_sec = std::trunc(extra_seconds);
                    if (!u32_add(second, static_cast<unsigned>(whole_sec), err)) return false;
                    // speedate leaves this one addition unchecked.
                    micro += static_cast<unsigned>(
                        std::round((extra_seconds - whole_sec) * 1000000.0));
                }
            }
        }
        ++position;
    }
    if (position < 2) {
        *err = TD_TOO_SHORT;
        return false;
    }
    return true;
}

} // namespace timedelta_detail

// Parse a timedelta string the way speedate 0.17.0 does: an ISO 8601 duration behind a 'P', the
// human "<n>d, HH:MM:SS" form, or a bare "HH:MM[:SS[.ffffff]]", each of them able to carry a
// leading '+' or '-'.  A plain number is *not* seconds on this path -- that is the numeric input's
// job -- which is why '1' is refused for the day identifier it never got.  error_out receives the
// reference's own words for the step that failed.
inline std::optional<Timedelta> try_parse_timedelta_str(const std::string& input,
                                                        std::string* error_out = nullptr) {
    const char* err = timedelta_detail::TD_TOO_SHORT;
    bool negative = false;
    size_t start = 0;
    if (!input.empty() && (input[0] == '-' || input[0] == '+')) {
        negative = input[0] == '-';
        start = 1;
    }
    const std::string b = input.substr(start);

    unsigned day = 0, second = 0, micro = 0;
    bool ok;
    if (b.empty()) {
        ok = false;
    } else if (b[0] == 'P') {
        ok = timedelta_detail::parse_iso(b.substr(1), day, second, micro, &err);
    } else if (b.find('d') != std::string::npos || b.find('D') != std::string::npos ||
               b.size() < 5) {
        // speedate's is_duration_date_format: any d or D anywhere sends the string to the days
        // reader, and anything shorter than five bytes cannot be a time anyway.
        ok = timedelta_detail::parse_days_time(b, day, second, micro, &err);
    } else {
        ok = timedelta_detail::parse_time_form(b, day, second, micro, &err);
    }
    if (!ok) {
        if (error_out) *error_out = err;
        return std::nullopt;
    }

    // speedate's normalize: microseconds into seconds, seconds into days, and the day count is
    // what a timedelta can actually hold.
    const long long abs_total = static_cast<long long>(day) * TD_SECONDS_IN_DAY +
                                static_cast<long long>(second) +
                                static_cast<long long>(micro) / 1000000LL;
    const long long abs_micro = static_cast<long long>(micro) % 1000000LL;
    const long long abs_days = abs_total / TD_SECONDS_IN_DAY;
    long long rem = abs_total % TD_SECONDS_IN_DAY;
    long long micros = abs_micro;
    if (abs_days > TD_MAX_DAYS) {
        if (error_out) *error_out = TD_TOO_MANY_DAYS;
        return std::nullopt;
    }
    long long days = abs_days;
    if (negative) {
        days = -days;
        rem = -rem;
        micros = -micros;
    }
    if (micros < 0) {
        // borrow one second for negative microseconds
        rem -= 1;
        micros += 1000000;
    }
    return Timedelta{static_cast<int>(days), static_cast<int>(rem), static_cast<int>(micros)};
}

// Dict iterator interface
class ValidatedDict {
public:
    virtual ~ValidatedDict() = default;
    
    virtual size_t size() const = 0;
    virtual bool empty() const = 0;
    
    struct Entry {
        std::string key;
        std::string value_repr;  // String representation for error messages
    };
    
    virtual std::vector<Entry> entries() const = 0;
    virtual std::vector<std::string> keys() const = 0;

    virtual bool has_key(const std::string& key) const = 0;
    virtual std::optional<Entry> get(const std::string& key) const = 0;

    // Get the actual value object for a key (for nested validation)
    virtual std::optional<py::object> get_value(const std::string& key) const = 0;

    // Get the actual key object (for key validation; JSON keys are strings)
    virtual std::optional<py::object> get_key(const std::string& key) const = 0;

    // One entry of a mapping, both halves as objects.  A dict node keeps walking entries() and
    // looking each half up by its text; the two subclass mapping nodes ask for the objects, so
    // an int key stays an int in an error location and two keys that spell the same text stay
    // two entries rather than overwriting each other in the result.
    struct Pair {
        py::object key;
        py::object value;
        std::string key_text;  // the str() the string-keyed accessors use
    };

    // The read a document with no key objects (JSON, a string mapping) can offer.
    virtual std::vector<Pair> pairs() const {
        std::vector<Pair> out;
        for (const auto& entry : entries()) {
            Pair p;
            p.key = get_key(entry.key).value_or(py::str(entry.key));
            p.value = get_value(entry.key).value_or(py::none());
            p.key_text = entry.key;
            out.push_back(std::move(p));
        }
        return out;
    }
};

// List iterator interface
class ValidatedList {
public:
    virtual ~ValidatedList() = default;
    
    virtual size_t size() const = 0;
    virtual bool empty() const = 0;
    
    struct Entry {
        size_t index;
        std::string value_repr;  // String representation for error messages
    };

    virtual std::vector<Entry> entries() const = 0;
    virtual py::object get_item(size_t index) const = 0;
};

// Tuple iterator interface
class ValidatedTuple : public ValidatedList {
public:
    // Tuple is same as list but with positional semantics
};

// Validated function arguments (result of Input::validate_args)
struct ArgumentsInput {
    py::tuple args;
    py::dict kwargs;
};

// Input trait - abstract interface for different input sources
class Input {
public:
    virtual ~Input() = default;

    virtual InputType input_type() const = 0;
    virtual InputValue as_error_value() const = 0;
    virtual bool is_none() const { return false; }

    // Get Python object representation (for function validators)
    virtual py::object as_python_object() const = 0;

    /// Set the current validation location for error reporting.
    void set_current_location(const Location& loc) { current_loc_ = &loc; }
    const Location& current_location() const {
        static const Location empty_loc{};
        return current_loc_ ? *current_loc_ : empty_loc;
    }

    // Type validation methods - return ValResult<ValMatch<T>>
    // `json_input` marks the value as JSON-sourced, where a float is
    // stringified Rust-style instead of with Python's str().
    virtual ValResult<ValMatch<EitherString>> validate_str(bool strict, bool coerce_numbers = false,
                                                           bool json_input = false) const = 0;
    virtual ValResult<ValMatch<EitherBytes>> validate_bytes(bool strict) const = 0;
    // Rust reads the schema's `val_json_bytes` inside the input, because that is
    // where a str gets decoded (input_json.rs:135, input_python.rs:196).
    virtual ValResult<ValMatch<EitherBytes>> validate_bytes(
        bool strict, const std::string& val_json_bytes) const {
        (void)val_json_bytes;
        return validate_bytes(strict);
    }
    // `json_document` marks a value read out of a JSON document, where a str is
    // always the encoded payload: strict does not gate the decode, and the
    // result counts as strict because JSON has no str/bytes distinction to
    // coerce across (input_json.rs:135).
    virtual ValResult<ValMatch<EitherBytes>> validate_bytes(
        bool strict, const std::string& val_json_bytes, bool json_document) const {
        (void)json_document;
        return validate_bytes(strict, val_json_bytes);
    }
    virtual ValResult<ValMatch<bool>> validate_bool(bool strict) const = 0;
    virtual ValResult<ValMatch<EitherInt>> validate_int(bool strict) const = 0;
    virtual ValResult<ValMatch<EitherFloat>> validate_float(bool strict) const = 0;

    // Date/time validation methods
    virtual bool is_date() const { return false; }
    virtual bool is_datetime() const { return false; }
    virtual bool is_time() const { return false; }
    // `unit` is the `val_temporal_unit` config: it decides whether a bare
    // number is seconds or milliseconds since the epoch.
    virtual ValResult<ValMatch<EitherDate>> validate_date(
        bool strict, TimestampUnit unit = TimestampUnit::Infer) const = 0;
    virtual ValResult<ValMatch<EitherDateTime>> validate_datetime(
        bool strict, TimestampUnit unit = TimestampUnit::Infer) const = 0;
    virtual ValResult<ValMatch<EitherTime>> validate_time(bool strict) const = 0;
    virtual ValResult<ValMatch<EitherTimedelta>> validate_timedelta(bool strict) const = 0;

    // Container validation
    virtual ValResult<std::unique_ptr<ValidatedDict>> validate_dict(bool strict) const = 0;
    // The two subclass mapping nodes ask an input question of their own
    // (input_abstract.rs:151-176).  Delegating to validate_dict would hand back a refusal in
    // dict_type's name, while Rust refuses a list handed to an ordered-dict as
    // ordered_dict_type -- the name a JSON document also speaks under the "Input should be an
    // object" wording (types.rs:648-654).
    virtual ValResult<std::unique_ptr<ValidatedDict>> validate_ordered_dict(bool strict) const = 0;
    virtual ValResult<std::unique_ptr<ValidatedDict>> validate_counter(bool strict) const = 0;
    virtual ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_list(bool strict) const = 0;
    virtual ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> validate_tuple(bool strict) const = 0;

    // Rust's validate_set/validate_frozenset share the sequence path but report
    // set_type/frozenset_type.  Inputs without a dedicated set accessor keep the
    // list wording.
    virtual ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_set(bool strict) const {
        return validate_list(strict);
    }
    virtual ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_frozenset(bool strict) const {
        return validate_list(strict);
    }
    // Rust's validate_deque reports deque_type; the input deque's maxlen
    // travels separately (PythonInput::deque_maxlen), as nowhere in the
    // sequence path can carry it.
    virtual ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_deque(bool strict) const {
        return validate_list(strict);
    }

    // Arguments validation (ArgsKwargs or dict input).  Only implemented for
    // PythonInput; other input kinds report an arguments_type error.
    virtual ValResult<ArgumentsInput> validate_args() const {
        return ValError::line_error(
            ErrorType(ErrorType::Kind::ArgumentsType),
            current_location(),
            as_error_value().repr
        );
    }

    // Whether the input is an ArgsKwargs container (vs a plain dict) — used by
    // the dataclass validator to distinguish the __init__ self_instance path
    // from nested dict validation.
    virtual bool is_args_kwargs() const { return false; }

protected:
    /// Pointer to current validation location (set by validators before calling validate_*).
    const Location* current_loc_ = nullptr;
};

// Helper to create type error — use input.current_location() when available
inline ValError type_error(ErrorType::Kind kind, const Input& input, const Location& loc) {
    return ValError::line_error(ErrorType(kind), loc, input.as_error_value().repr,
                                 input.as_python_object());
}

// True when the input is a Python str and not a subclass of one: Rust narrows a subclass to
// str before returning it, so a subclass never comes back as itself.
inline bool input_is_exact_str(const Input& input) {
    if (input.input_type() != InputType::Python) return false;
    py::object obj = input.as_python_object();
    return obj.ptr() && PyUnicode_CheckExact(obj.ptr());
}


} // namespace pydantic_core