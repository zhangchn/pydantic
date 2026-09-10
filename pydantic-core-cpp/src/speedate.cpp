#include "pydantic_core/speedate.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace pydantic_core {

namespace {

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline bool is_leap(int year) {
    return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap(year)) return 29;
    return kDays[month - 1];
}

// floor division: result is rounded towards negative infinity, so pre-epoch
// timestamps land on the correct day/time boundary.
inline int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// ---------------------------------------------------------------------------
// Fixed-width digit fields
// ---------------------------------------------------------------------------
// speedate reads every digit group of a component before it validates any
// range, so a malformed group outranks an out-of-range value in an earlier
// one (`2012-13-0x` reports the day characters, not the month range).

bool parse_digits(const char* d, size_t n, size_t& i, size_t width, const char* field,
                  int& out, std::string& err) {
    if (i + width > n) {
        err = "input is too short";
        return false;
    }
    int value = 0;
    for (size_t k = 0; k < width; ++k) {
        if (!is_digit(d[i + k])) {
            err = std::string("invalid character in ") + field;
            return false;
        }
        value = value * 10 + (d[i + k] - '0');
    }
    out = value;
    i += width;
    return true;
}

bool check_range(int value, int lo, int hi, const char* field, std::string& err) {
    if (value >= lo && value <= hi) return true;
    if (lo == 1 && hi == 12) {
        err = "month value is outside expected range of 1-12";
    } else if (lo == 1 || hi == 9999) {
        err = std::string(field) + " value is outside expected range";
    } else if (hi == 23) {
        err = std::string(field) + " value is outside expected range of 0-23";
    } else {
        err = std::string(field) + " value is outside expected range of 0-59";
    }
    return false;
}

// ---------------------------------------------------------------------------
// Fractional seconds
// ---------------------------------------------------------------------------
// ISO 8601 allows either '.' or ',' as the decimal separator. Extra digits are
// truncated (speedate's MicrosecondsPrecisionOverflowBehavior::Truncate), short
// runs are padded on the right.
bool parse_fraction(const char* d, size_t n, size_t& i, int& microsecond, std::string& err) {
    size_t start = i;  // points at the separator
    size_t p = start + 1;
    size_t digits_begin = p;
    while (p < n && is_digit(d[p])) ++p;
    if (p == digits_begin) {
        err = "second fraction digits missing after `.`";
        return false;
    }
    int value = 0;
    size_t count = 0;
    for (size_t k = digits_begin; k < p && count < 6; ++k, ++count) {
        value = value * 10 + (d[k] - '0');
    }
    while (count < 6) { value *= 10; ++count; }
    microsecond = value;
    i = p;
    return true;
}

// ---------------------------------------------------------------------------
// Timezone suffix
// ---------------------------------------------------------------------------
enum class TzResult { Parsed, Error };

// Accepts `Z`, `z`, or `±HH` followed by mandatory minutes written `MM` or
// `:MM`; a `±HH` with no minutes is an error rather than a short offset. Offsets
// must be strictly under 24 hours. Anything that reaches this function is a
// timezone attempt, so a malformed group reports itself instead of falling back
// to the trailing-characters document.
TzResult parse_tz(const char* d, size_t n, size_t& i, std::optional<int>& tz_minutes,
                  std::string& err) {
    char sign_char = d[i];
    if (sign_char == 'Z' || sign_char == 'z') {
        tz_minutes = 0;
        ++i;
        return TzResult::Parsed;
    }
    if (sign_char != '+' && sign_char != '-') {
        err = "invalid timezone sign";
        return TzResult::Error;
    }
    if (i + 3 > n || !is_digit(d[i + 1]) || !is_digit(d[i + 2])) {
        err = "invalid timezone hour";
        return TzResult::Error;
    }
    int hours = (d[i + 1] - '0') * 10 + (d[i + 2] - '0');
    size_t j = i + 3;
    int minutes = 0;
    if (j < n && d[j] == ':') {
        if (j + 3 > n || !is_digit(d[j + 1]) || !is_digit(d[j + 2])) {
            err = "invalid timezone minute";
            return TzResult::Error;
        }
        minutes = (d[j + 1] - '0') * 10 + (d[j + 2] - '0');
        j += 3;
    } else if (j + 2 <= n && is_digit(d[j]) && is_digit(d[j + 1])) {
        minutes = (d[j] - '0') * 10 + (d[j + 1] - '0');
        j += 2;
    } else {
        err = "invalid timezone minute";
        return TzResult::Error;
    }
    if (minutes > 59) {
        err = "timezone minute value is outside expected range of 0-59";
        return TzResult::Error;
    }
    int total = hours * 60 + minutes;
    if (total >= 24 * 60) {
        err = "timezone offset must be less than 24 hours";
        return TzResult::Error;
    }
    tz_minutes = (sign_char == '-') ? -total : total;
    i = j;
    return TzResult::Parsed;
}

// ---------------------------------------------------------------------------
// Date portion (shared by the datetime and date parsers)
// ---------------------------------------------------------------------------
// speedate reports date-portion failures identically for both parsers; only the
// date parser requires the input to stop here, so that rule stays in the caller.
bool parse_date_part(const char* d, size_t n, size_t& i, Date& out, std::string& err) {
    if (n < 10) {
        err = "input is too short";
        return false;
    }
    int year = 0, month = 0, day = 0;
    if (!parse_digits(d, n, i, 4, "year", year, err)) return false;
    if (d[i] != '-') {
        err = "invalid date separator, expected `-`";
        return false;
    }
    if (!parse_digits(d, n, ++i, 2, "month", month, err)) return false;
    if (d[i] != '-') {
        err = "invalid date separator, expected `-`";
        return false;
    }
    if (!parse_digits(d, n, ++i, 2, "day", day, err)) return false;
    if (!check_range(month, 1, 12, "month", err)) return false;
    if (day < 1 || day > days_in_month(year, month)) {
        err = "day value is outside expected range";
        return false;
    }
    out = Date{year, month, day};
    return true;
}

// ---------------------------------------------------------------------------
// Time of day (shared by the datetime and time parsers)
// ---------------------------------------------------------------------------
// Like the date portion, both digit groups and the separator are read before
// either range is validated, so `9999-12-31` reports the separator and `99:x5`
// reports the minute characters.
bool parse_time_hm(const char* d, size_t n, size_t& i, Time& out, std::string& err) {
    int hour = 0, minute = 0;
    if (!parse_digits(d, n, i, 2, "hour", hour, err)) return false;
    if (d[i] != ':') {
        err = "invalid time separator, expected `:`";
        return false;
    }
    if (!parse_digits(d, n, ++i, 2, "minute", minute, err)) return false;
    if (!check_range(hour, 0, 23, "hour", err)) return false;
    if (!check_range(minute, 0, 59, "minute", err)) return false;
    out.hour = hour;
    out.minute = minute;
    return true;
}

// `i` points just past `HH:MM`. Parses an optional `:SS[.frac]` group and then
// an optional timezone, leaving `i` at the first unconsumed character.
bool parse_time_tail(const char* d, size_t n, size_t& i, Time& out, std::string& err) {
    if (i >= n) return true;

    if (d[i] == ':') {
        ++i;
        int second = 0;
        // Unlike the earlier groups, a truncated seconds group is reported as a
        // bad character rather than a short input.
        if (i + 2 > n) {
            err = "invalid character in second";
            return false;
        }
        if (!parse_digits(d, n, i, 2, "second", second, err)) return false;
        if (!check_range(second, 0, 59, "second", err)) return false;
        out.second = second;
        if (i < n && (d[i] == '.' || d[i] == ',')) {
            int microsecond = 0;
            if (!parse_fraction(d, n, i, microsecond, err)) return false;
            out.microsecond = microsecond;
        }
    }

    if (i >= n) return true;

    std::optional<int> tz;
    switch (parse_tz(d, n, i, tz, err)) {
        case TzResult::Error:
            return false;
        case TzResult::Parsed:
            out.tz_offset = tz;
            break;
    }
    if (i != n) {
        err = "unexpected extra characters at the end of the input";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Numeric strings
// ---------------------------------------------------------------------------
struct NumberToken {
    bool negative = false;
    int64_t int_part = 0;
    std::string frac;  // digits after the decimal point, may be empty
};

// `[+-]? digits+ [ '.' digits* ]`, consuming the whole input. Rejects exponents
// ("1e3" is not a number to speedate) and overflows.
bool parse_number_token(const char* d, size_t n, NumberToken& out) {
    size_t i = 0;
    bool negative = false;
    if (i < n && (d[i] == '+' || d[i] == '-')) {
        negative = (d[i] == '-');
        ++i;
    }
    if (i >= n || !is_digit(d[i])) return false;

    int64_t value = 0;
    while (i < n && is_digit(d[i])) {
        int digit = d[i] - '0';
        if (value > (int64_t(9223372036854775807LL) - digit) / 10) return false;  // overflow
        value = value * 10 + digit;
        ++i;
    }
    if (i < n && (d[i] == '.' || d[i] == ',')) {
        ++i;
        while (i < n && is_digit(d[i])) out.frac.push_back(d[i++]);
    }
    if (i != n) return false;

    out.negative = negative;
    out.int_part = value;
    return true;
}

// A fraction of a `unit` expressed as digits -> microseconds. For seconds the
// digits are millionths; for milliseconds they are thousandths of a millisecond.
int64_t fraction_to_micros(const std::string& frac, TimestampUnit unit) {
    size_t keep = (unit == TimestampUnit::Milliseconds) ? 3 : 6;
    int64_t value = 0;
    size_t count = 0;
    for (size_t k = 0; k < frac.size() && count < keep; ++k, ++count) {
        value = value * 10 + (frac[k] - '0');
    }
    while (count < keep) { value *= 10; ++count; }
    return value;
}

TimestampUnit resolve_unit(int64_t magnitude, TimestampUnit unit) {
    if (unit != TimestampUnit::Infer) return unit;
    return (magnitude > kInferWatershed) ? TimestampUnit::Milliseconds
                                         : TimestampUnit::Seconds;
}

// Turns an integer count of `unit` plus a fractional part into an absolute
// datetime, applying the speedate range documents.
ParseOutcome<DateTime> from_number(const NumberToken& num, TimestampUnit requested_unit) {
    TimestampUnit unit = resolve_unit(num.int_part, requested_unit);
    int64_t scale = (unit == TimestampUnit::Milliseconds) ? 1000 : 1000000;
    int64_t limit = (unit == TimestampUnit::Milliseconds) ? kMaxUnixTimestamp * 1000
                                                          : kMaxUnixTimestamp;
    int64_t floor_limit = (unit == TimestampUnit::Milliseconds) ? kMinUnixTimestamp * 1000
                                                                : kMinUnixTimestamp;
    // num.int_part is the magnitude; the sign lives in num.negative, so the
    // range check has to re-apply it or negative overflow goes unnoticed.
    int64_t signed_value = num.negative ? -num.int_part : num.int_part;
    if (signed_value > limit) {
        return ParseOutcome<DateTime>::failure(
            "dates after 9999 are not supported as unix timestamps");
    }
    if (signed_value < floor_limit) {
        return ParseOutcome<DateTime>::failure(
            "dates before 0000 are not supported as unix timestamps");
    }

    int64_t total_us = num.int_part * scale + fraction_to_micros(num.frac, unit);
    if (num.negative) total_us = -total_us;

    int64_t epoch_seconds = floor_div(total_us, 1000000);
    int64_t micros = total_us - epoch_seconds * 1000000;

    int64_t days = floor_div(epoch_seconds, 86400);
    int64_t second_of_day = epoch_seconds - days * 86400;

    DateTime dt;
    dt.date = civil_from_days(days);
    dt.time.hour = static_cast<int>(second_of_day / 3600);
    dt.time.minute = static_cast<int>((second_of_day % 3600) / 60);
    dt.time.second = static_cast<int>(second_of_day % 60);
    dt.time.microsecond = static_cast<int>(micros);
    dt.time.tz_offset = 0;  // unix timestamps are UTC
    return ParseOutcome<DateTime>::success(dt);
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================

TimestampUnit timestamp_unit_from_string(std::string_view name, bool* ok) {
    if (name == "seconds") {
        if (ok) *ok = true;
        return TimestampUnit::Seconds;
    }
    if (name == "milliseconds") {
        if (ok) *ok = true;
        return TimestampUnit::Milliseconds;
    }
    if (name == "infer") {
        if (ok) *ok = true;
        return TimestampUnit::Infer;
    }
    if (ok) *ok = false;
    return TimestampUnit::Infer;
}

ParseOutcome<DateTime> parse_datetime_bytes(const char* data, size_t len, TimestampUnit unit) {
    // Try the string form first. If it fails but the input reads as a number,
    // use the timestamp reading; if both fail, the string-form error is the
    // one speedate reports.
    std::string str_err;
    DateTime dt;
    size_t i = 0;
    bool str_ok = parse_date_part(data, len, i, dt.date, str_err);

    if (str_ok && i < len) {
        std::string local_err;
        char sep = data[i];
        if (sep != 'T' && sep != 't' && sep != '_' && sep != ' ') {
            str_ok = false;
            str_err = "invalid datetime separator, expected `T`, `t`, `_` or space";
        } else if (len - (i + 1) < 5) {
            str_ok = false;
            str_err = "input is too short";
        } else {
            ++i;
            if (!parse_time_hm(data, len, i, dt.time, local_err)) {
                str_ok = false;
                str_err = local_err;
            } else if (!parse_time_tail(data, len, i, dt.time, local_err)) {
                str_ok = false;
                str_err = local_err;
            }
        }
    }

    if (str_ok) return ParseOutcome<DateTime>::success(dt);

    NumberToken num;
    if (parse_number_token(data, len, num)) {
        return from_number(num, unit);
    }
    return ParseOutcome<DateTime>::failure(str_err);
}

ParseOutcome<DateTime> parse_datetime_string(std::string_view s, TimestampUnit unit) {
    return parse_datetime_bytes(s.data(), s.size(), unit);
}

// speedate's Date::parse_bytes reads the date portion and then requires the
// input to be exhausted, so every time form is rejected here and accepted only
// by the date validator's datetime retry. A bare number is read as a timestamp
// only when it lands exactly on midnight.
ParseOutcome<Date> parse_date_bytes(const char* data, size_t len, TimestampUnit unit) {
    size_t i = 0;
    Date date;
    std::string err;
    if (!parse_date_part(data, len, i, date, err)) {
        NumberToken num;
        if (parse_number_token(data, len, num)) {
            auto dt = from_number(num, unit);
            if (!dt.ok) return ParseOutcome<Date>::failure(std::move(dt.error));
            if (!dt.value.time.is_midnight()) {
                return ParseOutcome<Date>::failure("Timestamp is not an exact date");
            }
            return ParseOutcome<Date>::success(dt.value.date);
        }
        return ParseOutcome<Date>::failure(std::move(err));
    }
    if (i != len) {
        return ParseOutcome<Date>::failure("unexpected extra characters at the end of the input");
    }
    return ParseOutcome<Date>::success(date);
}

ParseOutcome<Date> parse_date_string(std::string_view s, TimestampUnit unit) {
    return parse_date_bytes(s.data(), s.size(), unit);
}

ParseOutcome<Time> parse_time_bytes(const char* data, size_t len) {
    if (len < 5) return ParseOutcome<Time>::failure("input is too short");

    size_t i = 0;
    Time t;
    std::string err;
    if (!parse_time_hm(data, len, i, t, err)) {
        return ParseOutcome<Time>::failure(err);
    }
    if (!parse_time_tail(data, len, i, t, err)) {
        return ParseOutcome<Time>::failure(err);
    }
    return ParseOutcome<Time>::success(t);
}

ParseOutcome<Time> parse_time_string(std::string_view s) {
    return parse_time_bytes(s.data(), s.size());
}

ParseOutcome<DateTime> datetime_from_timestamp(int64_t timestamp, uint32_t microseconds,
                                               TimestampUnit unit) {
    NumberToken num;
    num.negative = timestamp < 0;
    // Negating INT64_MIN is UB; parse_number_token never produces it, and no
    // real timestamp approaches it, so clamp instead.
    num.int_part = (timestamp == INT64_MIN) ? int64_t(9223372036854775807LL)
                                            : (num.negative ? -timestamp : timestamp);
    auto out = from_number(num, unit);
    if (!out.ok) return out;
    out.value.time.microsecond += static_cast<int>(microseconds);
    if (out.value.time.microsecond >= 1000000) {
        out.value.time.microsecond -= 1000000;
        // microseconds only ever push within the same second for valid inputs;
        // re-deriving keeps hour/day consistent if they do not.
        int64_t days = days_from_civil(out.value.date.year, out.value.date.month,
                                       out.value.date.day);
        int64_t secs = int64_t(out.value.time.hour) * 3600 +
                       int64_t(out.value.time.minute) * 60 +
                       out.value.time.second + 1;
        days += secs / 86400;
        secs %= 86400;
        out.value.date = civil_from_days(days);
        out.value.time.hour = static_cast<int>(secs / 3600);
        out.value.time.minute = static_cast<int>((secs % 3600) / 60);
        out.value.time.second = static_cast<int>(secs % 60);
    }
    return out;
}

ParseOutcome<DateTime> datetime_from_float(double value, TimestampUnit unit) {
    if (std::isnan(value)) return ParseOutcome<DateTime>::failure(kNanNotAllowed);
    if (value == std::numeric_limits<double>::infinity()) {
        return ParseOutcome<DateTime>::failure(
            "dates after 9999 are not supported as unix timestamps");
    }
    if (value == -std::numeric_limits<double>::infinity()) {
        return ParseOutcome<DateTime>::failure(
            "dates before 0000 are not supported as unix timestamps");
    }

    int64_t magnitude = (std::fabs(value) > double(kInferWatershed))
                            ? int64_t(kInferWatershed) + 1
                            : int64_t(std::fabs(value));
    TimestampUnit resolved = resolve_unit(magnitude, unit);
    double scaled = (resolved == TimestampUnit::Milliseconds) ? value / 1000.0 : value;

    if (scaled > double(kMaxUnixTimestamp)) {
        return ParseOutcome<DateTime>::failure(
            "dates after 9999 are not supported as unix timestamps");
    }
    if (scaled < double(kMinUnixTimestamp)) {
        return ParseOutcome<DateTime>::failure(
            "dates before 0000 are not supported as unix timestamps");
    }

    int64_t epoch_seconds = static_cast<int64_t>(std::floor(scaled));
    int64_t micros = static_cast<int64_t>(std::llround((scaled - epoch_seconds) * 1e6));
    if (micros >= 1000000) { micros -= 1000000; ++epoch_seconds; }
    if (micros < 0) { micros += 1000000; --epoch_seconds; }

    int64_t days = floor_div(epoch_seconds, 86400);
    int64_t second_of_day = epoch_seconds - days * 86400;

    DateTime dt;
    dt.date = civil_from_days(days);
    dt.time.hour = static_cast<int>(second_of_day / 3600);
    dt.time.minute = static_cast<int>((second_of_day % 3600) / 60);
    dt.time.second = static_cast<int>(second_of_day % 60);
    dt.time.microsecond = static_cast<int>(micros);
    dt.time.tz_offset = 0;
    return ParseOutcome<DateTime>::success(dt);
}

ParseOutcome<Time> time_from_timestamp(int64_t seconds, uint32_t microseconds) {
    if (seconds < 0) {
        return ParseOutcome<Time>::failure("time in seconds should be positive");
    }
    // speedate clamps to u32 before validating, so oversized values report the
    // same document as 86400 itself.
    if (seconds > 4294967295LL || seconds >= 86400) {
        return ParseOutcome<Time>::failure("numeric times may not exceed 86,399 seconds");
    }
    Time t;
    t.hour = static_cast<int>(seconds / 3600);
    t.minute = static_cast<int>((seconds % 3600) / 60);
    t.second = static_cast<int>(seconds % 60);
    t.microsecond = static_cast<int>(microseconds);
    t.tz_offset = 0;
    return ParseOutcome<Time>::success(t);
}

ParseOutcome<Time> time_from_float(double value) {
    if (std::isnan(value)) return ParseOutcome<Time>::failure(kNanNotAllowed);
    // pydantic-core floors the float into the integer path, so every negative
    // value - including -inf, which saturates - reports the non-positive
    // document before the magnitude limit is consulted.
    if (value < 0.0) return ParseOutcome<Time>::failure("time in seconds should be positive");
    if (std::fabs(value) >= double(4294967295LL)) {
        return ParseOutcome<Time>::failure("numeric times may not exceed 86,399 seconds");
    }
    int64_t whole = static_cast<int64_t>(std::floor(value));
    double frac = value - whole;
    uint32_t micros = static_cast<uint32_t>(std::llround(frac * 1e6));
    if (micros >= 1000000) { micros -= 1000000; ++whole; }
    return time_from_timestamp(whole, micros);
}

// ===========================================================================
// Civil <-> epoch days (Howard Hinnant's algorithm)
// ===========================================================================

int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);  // [0, 399]
    const unsigned um = static_cast<unsigned>(m);
    const unsigned ud = static_cast<unsigned>(d);
    const unsigned doy = (153 * (um + (um > 2 ? -3 : 9)) + 2) / 5 + ud - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;              // [0, 146096]
    return era * 146097 + static_cast<int>(doe) - 719468;
}

namespace {
// First day of the era-relative year `yoe`, mirroring the `yoe * 365 + yoe/4 -
// yoe/100` term in days_from_civil.
inline unsigned era_year_start(unsigned yoe) { return 365u * yoe + yoe / 4 - yoe / 100; }
}  // namespace

Date civil_from_days(int64_t z_in) {
    int64_t z = z_in + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);  // [0, 146096]
    // Invert doe = 365*yoe + yoe/4 - yoe/100 + doy. There is no exact closed
    // form for the leap-day correction, so start from doe/365 (an upper bound,
    // since every year is at least 365 days) and walk down; the estimate is at
    // most one year high across the whole 400-year era.
    unsigned yoe = doe / 365;
    if (yoe > 399) yoe = 399;  // doe 146096 is the era's last day, not year 400
    while (era_year_start(yoe) > doe) --yoe;
    const unsigned y = yoe + static_cast<unsigned>(era) * 400;
    const unsigned doy = doe - era_year_start(yoe);  // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                       // [0, 11]
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;               // [1, 31]
    const unsigned m = mp + (mp < 10 ? 3 : static_cast<unsigned>(-9));  // [1, 12]
    return Date{static_cast<int>(y) + (m <= 2 ? 1 : 0), static_cast<int>(m), static_cast<int>(d)};
}

}  // namespace pydantic_core
