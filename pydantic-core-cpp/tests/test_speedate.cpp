#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pydantic_core/speedate.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace pydantic_core;

namespace {

constexpr int kNoTz = -(1 << 30);

inline Time make_time(int h, int mi, int s = 0, int us = 0, int tz = kNoTz) {
    Time t;
    t.hour = h;
    t.minute = mi;
    t.second = s;
    t.microsecond = us;
    if (tz != kNoTz) t.tz_offset = tz;
    return t;
}

inline DateTime make_dt(int y, int mo, int d, int h = 0, int mi = 0, int s = 0, int us = 0,
                        int tz = kNoTz) {
    DateTime r;
    r.date = Date{y, mo, d};
    r.time = make_time(h, mi, s, us, tz);
    return r;
}

// Asserts a successful parse against an expected value, reporting the input so
// a failure names the case rather than just the field that differed.
inline void expect_dt(std::string_view input, const DateTime& expected,
                      TimestampUnit unit = TimestampUnit::Infer) {
    auto r = parse_datetime_string(input, unit);
    INFO("input: " << std::string(input));
    INFO("error: " << r.error);
    CHECK(r.ok);
    CHECK(r.value == expected);
}

// Numeric overload: an int input goes through the timestamp path, not the
// string parser, so it needs its own entry point.
inline void expect_dt(int64_t input, const DateTime& expected,
                      TimestampUnit unit = TimestampUnit::Infer) {
    auto r = datetime_from_timestamp(input, 0, unit);
    INFO("input: " << input);
    INFO("error: " << r.error);
    CHECK(r.ok);
    CHECK(r.value == expected);
}

inline void expect_dt_error(std::string_view input, const std::string& expected_error,
                            TimestampUnit unit = TimestampUnit::Infer) {
    auto r = parse_datetime_string(input, unit);
    INFO("input: " << std::string(input));
    CHECK_FALSE(r.ok);
    CHECK(r.error == expected_error);
}

inline void expect_time(std::string_view input, const Time& expected) {
    auto r = parse_time_string(input);
    INFO("input: " << std::string(input));
    INFO("error: " << r.error);
    CHECK(r.ok);
    CHECK(r.value == expected);
}

inline void expect_time_error(std::string_view input, const std::string& expected_error) {
    auto r = parse_time_string(input);
    INFO("input: " << std::string(input));
    CHECK_FALSE(r.ok);
    CHECK(r.error == expected_error);
}

}  // namespace

TEST_SUITE_BEGIN("speedate");

// ===========================================================================
// Civil <-> epoch-day arithmetic
// ===========================================================================

TEST_CASE("civil arithmetic anchors") {
    CHECK(days_from_civil(1970, 1, 1) == 0);
    CHECK(civil_from_days(0) == Date{1970, 1, 1});

    // Bounds that speedate's timestamp range check depends on.
    CHECK(days_from_civil(9999, 12, 31) * 86400 + 86399 == kMaxUnixTimestamp);
    CHECK(days_from_civil(0, 1, 1) * 86400 == kMinUnixTimestamp);

    // Leap-year rules: divisible by 4, except centuries unless /400.
    CHECK(days_from_civil(2012, 3, 1) - days_from_civil(2012, 2, 1) == 29);
    CHECK(days_from_civil(2013, 3, 1) - days_from_civil(2013, 2, 1) == 28);
    CHECK(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 1) == 29);
    CHECK(days_from_civil(1900, 3, 1) - days_from_civil(1900, 2, 1) == 28);
}

TEST_CASE("civil arithmetic round-trips every day in range") {
    // Exhaustive over the whole span speedate accepts (year 0..9999).
    int64_t checked = 0;
    for (int64_t z = days_from_civil(0, 1, 1); z <= days_from_civil(9999, 12, 31); ++z) {
        Date d = civil_from_days(z);
        CHECK(days_from_civil(d.year, d.month, d.day) == z);
        ++checked;
    }
    CHECK(checked == 3652425);  // 10000 years incl. 2425 leap days
}

// ===========================================================================
// Date-only strings
// ===========================================================================

TEST_CASE("date strings: valid") {
    CHECK(parse_date_string("2012-04-23").value == Date{2012, 4, 23});
    CHECK(parse_date_string("9999-12-31").value == Date{9999, 12, 31});
    CHECK(parse_date_string("0001-01-01").value == Date{1, 1, 1});
    CHECK(parse_date_string("2012-02-29").value == Date{2012, 2, 29});
    CHECK(parse_date_string("2012-01-31").value == Date{2012, 1, 31});
}

TEST_CASE("date strings: anything past the date is extra characters") {
    // Date::parse_bytes stops after `YYYY-MM-DD`, so every time form below is
    // rejected here and accepted only by the date validator's datetime retry.
    // The last two groups also show that time-part documents from the datetime
    // parser (offset too large, tz minutes) never leak into this document.
    const char* extra = "unexpected extra characters at the end of the input";
    const char* with_time[] = {
        "2012-04-23T00:00:00",   "2012-04-23 00:00:00",     "2012-04-23_00:00:00",
        "2012-04-23t00:00:00",   "2012-04-23T00:00",        "2012-04-23T00:00:00.000",
        "2012-04-23T00:00:00.0000001", "2012-04-23T00:00:00z", "2012-04-23T00:00:00+23:59",
        "2012-04-23T11:05:00-25:00",    "2012-04-23T11:05:00+00:60",
        "2012-04-23T11:05:00+",         "2012-04-23T11:05:00Y",
        "2012-04-23T11:05:00+ab:00",    "2012-04-23T11:05:00+00:00:30",
        "2012-04-23T11",          "2012-04-23 ",             "2012-04-23x",
    };
    for (const char* s : with_time) {
        auto out = parse_date_string(s);
        CHECK_FALSE(out.ok);
        CHECK(out.error == extra);
    }
}

TEST_CASE("date strings: date-portion documents") {
    // Date-portion documents match the datetime parser's; only the trailing
    // rule above differs.
    const char* too_short = "input is too short";
    CHECK(parse_date_string("").error == too_short);
    CHECK(parse_date_string("x").error == too_short);
    CHECK(parse_date_string("x20120423").error == too_short);
    CHECK(parse_date_string("infinity").error == too_short);
    CHECK(parse_date_string("2012-04").error == too_short);
    CHECK(parse_date_string("2012-4-23").error == too_short);
    // Numeric strings are read as timestamps, so they report the exactness
    // document rather than a date-portion one.
    const char* not_exact = "Timestamp is not an exact date";
    CHECK(parse_date_string("2012042").error == not_exact);
    CHECK(parse_date_string("20120423").error == not_exact);
    CHECK(parse_date_string("86400").value == Date{1970, 1, 2});
    CHECK(parse_date_string("2012-13-01").error == "month value is outside expected range of 1-12");
    CHECK(parse_date_string("2012-04-56").error == "day value is outside expected range");
    CHECK(parse_date_string("2012-02-30").error == "day value is outside expected range");
    CHECK(parse_date_string("1234567890").error == not_exact);
}

TEST_CASE("date strings: error documents") {
    // Documents below are speedate's ParseError documentation strings, which
    // pydantic-core interpolates into "{error}" and ctx["error"].
    expect_dt_error("", "input is too short");
    expect_dt_error("x", "input is too short");
    expect_dt_error("x20120423", "input is too short");
    expect_dt_error("infinity", "input is too short");
    expect_dt_error("2012-", "input is too short");
    expect_dt_error("2012-04", "input is too short");
    expect_dt_error("2012-04-", "input is too short");
    expect_dt_error("2012-04-3", "input is too short");
    expect_dt_error("0001-1-1", "input is too short");
    expect_dt_error("1e3", "input is too short");

    expect_dt_error("2012-04-32", "day value is outside expected range");
    expect_dt_error("2012-04-00", "day value is outside expected range");
    expect_dt_error("2012-04-31", "day value is outside expected range");
    expect_dt_error("2012-06-31", "day value is outside expected range");
    expect_dt_error("2013-02-29", "day value is outside expected range");

    expect_dt_error("2012-13-01", "month value is outside expected range of 1-12");
    expect_dt_error("2012-00-01", "month value is outside expected range of 1-12");
    expect_dt_error("2012-00-31", "month value is outside expected range of 1-12");

    expect_dt_error("2012X04-23", "invalid date separator, expected `-`");
    expect_dt_error("2012-04X23", "invalid date separator, expected `-`");
    expect_dt_error("99999-01-01", "invalid date separator, expected `-`");
    expect_dt_error("99999999999999999999", "invalid date separator, expected `-`");

    expect_dt_error("+2012-04-23", "invalid character in year");
    expect_dt_error("2012-ab-23", "invalid character in month");
    expect_dt_error("2012-04-ab", "invalid character in day");
    expect_dt_error("2012-04-23Tab:00:00", "invalid character in hour");
    expect_dt_error("2012-04-23T0a:00:00", "invalid character in hour");
    expect_dt_error("2012-04-23T00:ab:00", "invalid character in minute");
    expect_dt_error("2012-04-23T00:00:ab", "invalid character in second");

    expect_dt_error("2012-04-23T25:00:00", "hour value is outside expected range of 0-23");
    expect_dt_error("2012-04-23T00:60:00", "minute value is outside expected range of 0-59");
    expect_dt_error("2012-04-23T00:00:60", "second value is outside expected range of 0-59");

    expect_dt_error("2012-04-23extra",
                    "invalid datetime separator, expected `T`, `t`, `_` or space");
    expect_dt_error("2012-04-23T00X00:00", "invalid time separator, expected `:`");
    expect_dt_error("2012-04-23T00:00X00", "invalid timezone sign");
    expect_dt_error("2012-04-23T00:00:00X", "invalid timezone sign");
    expect_dt_error("2012-04-23T00:00:00:00", "invalid timezone sign");
    expect_dt_error("2012-04-23T00:00:00+", "invalid timezone hour");
    expect_dt_error("2012-04-23T00:00:00+0", "invalid timezone hour");
    expect_dt_error("2012-04-23T00:00:00+0060",
                    "timezone minute value is outside expected range of 0-59");
    expect_dt_error("2012-04-23T00:00:00+00:60",
                    "timezone minute value is outside expected range of 0-59");
    expect_dt_error("2012-04-23T00:00:00+24:00",
                    "timezone offset must be less than 24 hours");
    expect_dt_error("2012-04-23T00:00:00.", "second fraction digits missing after `.`");
    expect_dt_error("2012-04-23T00:00:00.Z", "second fraction digits missing after `.`");
    expect_dt_error("2012-04-23T00:00:00Zextra",
                    "unexpected extra characters at the end of the input");
    expect_dt_error("2012-04-23T00:00:00+00:00:59",
                    "unexpected extra characters at the end of the input");
    expect_dt_error("2012-04-23T00:00:00+ab:00", "invalid timezone hour");
    expect_dt_error("2012-04-23T00:00:00+2", "invalid timezone hour");
    expect_dt_error("2012-04-23T00:00:00+", "invalid timezone hour");
    // Minutes are mandatory, so a bare `±HH` never becomes a short offset.
    expect_dt_error("2012-04-23T00:00:00+00ab", "invalid timezone minute");
    expect_dt_error("2012-04-23T00:00:00+00:", "invalid timezone minute");
    expect_dt_error("2012-04-23T00:00:00+00:0", "invalid timezone minute");
    expect_dt_error("2012-04-23T00:00:00+23", "invalid timezone minute");
    expect_dt_error("2012-04-23T00:00:00+00:00Z",
                    "unexpected extra characters at the end of the input");
}

TEST_CASE("digit groups are read before any range is checked") {
    // speedate consumes a component's digit groups and separators first, so a
    // malformed group outranks an out-of-range value from an earlier one.
    expect_dt_error("2012-13-0x", "invalid character in day");
    expect_dt_error("2012-0x-01", "invalid character in month");
    expect_dt_error("2012-13-45", "month value is outside expected range of 1-12");
    expect_dt_error("2012-04-00", "day value is outside expected range");

    expect_time_error("9999-12-31", "invalid time separator, expected `:`");
    expect_time_error("110:00", "invalid time separator, expected `:`");
    expect_time_error("99:x5", "invalid character in minute");
    expect_time_error("99:00", "hour value is outside expected range of 0-23");
    expect_time_error("11:65x", "minute value is outside expected range of 0-59");
    expect_dt_error("2012-04-23T9999-12-31", "invalid time separator, expected `:`");
    expect_dt_error("2012-04-23T1:05:00", "invalid character in hour");

    // A truncated seconds group is a bad character, not a short input.
    expect_time_error("11:05:", "invalid character in second");
    expect_time_error("11:05:0", "invalid character in second");
    expect_time_error("11:0", "input is too short");
}

TEST_CASE("negative numeric times report the sign document") {
    CHECK_FALSE(time_from_float(-1.0).ok);
    CHECK(time_from_float(-1.0).error == "time in seconds should be positive");
    CHECK(time_from_float(-std::numeric_limits<double>::infinity()).error ==
          "time in seconds should be positive");
    CHECK(time_from_timestamp(-1, 0).error == "time in seconds should be positive");
}

// ===========================================================================
// Date-time strings
// ===========================================================================

TEST_CASE("datetime strings: valid") {
    expect_dt("2012-04-23T09:15:00", make_dt(2012, 4, 23, 9, 15));
    expect_dt("2012-04-23T09:15:00Z", make_dt(2012, 4, 23, 9, 15, 0, 0, 0));
    expect_dt("2012-04-23T10:20:30.400+02:30", make_dt(2012, 4, 23, 10, 20, 30, 400000, 150));
    expect_dt("2012-04-23T10:20:30.400+02:00", make_dt(2012, 4, 23, 10, 20, 30, 400000, 120));
    expect_dt("2012-04-23T10:20:30.400-02:00", make_dt(2012, 4, 23, 10, 20, 30, 400000, -120));
    expect_dt("2012-04-23 00:00:00Z", make_dt(2012, 4, 23, 0, 0, 0, 0, 0));
    expect_dt("2012-04-23t00:00:00", make_dt(2012, 4, 23, 0, 0));
    expect_dt("9999-12-31T23:59:59", make_dt(9999, 12, 31, 23, 59, 59));
    // Fraction is truncated to 6 digits, padded on the right.
    expect_dt("2012-04-23T00:00:00.1", make_dt(2012, 4, 23, 0, 0, 0, 100000));
    expect_dt("2012-04-23T00:00:00.1234567", make_dt(2012, 4, 23, 0, 0, 0, 123456));
    // ISO 8601 also permits a comma as the decimal separator.
    expect_dt("2012-04-23T00:00:00,5", make_dt(2012, 4, 23, 0, 0, 0, 500000));
    expect_dt("2012-04-23T00:00:00+0000", make_dt(2012, 4, 23, 0, 0, 0, 0, 0));
    expect_dt("2012-04-23T00:00:00-00:30", make_dt(2012, 4, 23, 0, 0, 0, 0, -30));
    expect_dt("2012-04-23T00:00:00.5+01:00", make_dt(2012, 4, 23, 0, 0, 0, 500000, 60));
}

TEST_CASE("datetime strings: year 0 parses, conversion rejects it") {
    // speedate parses year 0; the "year 0 is out of range" document belongs to
    // the Python-object conversion, not to parsing.
    auto r = parse_datetime_string("0000-01-01T00:00:00");
    CHECK(r.ok);
    CHECK(r.value.date == Date{0, 1, 1});
}

// ===========================================================================
// Time strings
// ===========================================================================

TEST_CASE("time strings: valid") {
    expect_time("09:15:00", make_time(9, 15));
    expect_time("10:10", make_time(10, 10));
    expect_time("10:20:30.400", make_time(10, 20, 30, 400000));
    expect_time("11:05:00Z", make_time(11, 5, 0, 0, 0));
    expect_time("11:05:00z", make_time(11, 5, 0, 0, 0));
    expect_time("11:05:00+00:00", make_time(11, 5, 0, 0, 0));
    expect_time("11:05:00-05:30", make_time(11, 5, 0, 0, -330));
    expect_time("11:05:00-0530", make_time(11, 5, 0, 0, -330));
    expect_time("11:05-06:00", make_time(11, 5, 0, 0, -360));
    expect_time("11:05+06:00", make_time(11, 5, 0, 0, 360));
    expect_time("11:05:00.5", make_time(11, 5, 0, 500000));
    expect_time("00:00:00.1", make_time(0, 0, 0, 100000));
    expect_time("00:00:00.1234567", make_time(0, 0, 0, 123456));
    expect_time("23:59:59", make_time(23, 59, 59));
}

TEST_CASE("time strings: error documents") {
    expect_time_error("", "input is too short");
    expect_time_error("x", "input is too short");
    expect_time_error("0", "input is too short");
    expect_time_error("1:", "input is too short");
    expect_time_error("1:2", "input is too short");
    expect_time_error("01:0", "input is too short");

    expect_time_error("4:8:16", "invalid character in hour");
    expect_time_error("ab:00:00", "invalid character in hour");
    expect_time_error("00:ab:00", "invalid character in minute");
    expect_time_error("00:00:ab", "invalid character in second");

    expect_time_error("24:00", "hour value is outside expected range of 0-23");
    expect_time_error("24:00:00", "hour value is outside expected range of 0-23");
    expect_time_error("00:60", "minute value is outside expected range of 0-59");
    expect_time_error("00:00:60", "second value is outside expected range of 0-59");

    expect_time_error("091500", "invalid time separator, expected `:`");
    expect_time_error("11:05.5", "invalid timezone sign");
    expect_time_error("11:05.5Z", "invalid timezone sign");
    expect_time_error("11:05:00Y", "invalid timezone sign");
    expect_time_error("11:05:00:00", "invalid timezone sign");
    expect_time_error("01:02:03extra", "invalid timezone sign");
    expect_time_error("11:05:00+", "invalid timezone hour");
    expect_time_error("11:05:00+0", "invalid timezone hour");
    expect_time_error("11:05:00-25:00", "timezone offset must be less than 24 hours");
    expect_time_error("11:05:00-0060",
                      "timezone minute value is outside expected range of 0-59");
    expect_time_error("11:05:00+00:60",
                      "timezone minute value is outside expected range of 0-59");
    expect_time_error("01:02:03.", "second fraction digits missing after `.`");
    expect_time_error("11:05:00Zextra", "unexpected extra characters at the end of the input");
    expect_time_error("11:05:00+00:00:30",
                      "unexpected extra characters at the end of the input");
}

// ===========================================================================
// Numeric inputs
// ===========================================================================

TEST_CASE("datetime from int timestamps") {
    expect_dt(1494012444, make_dt(2017, 5, 5, 19, 27, 24, 0, 0));
    expect_dt(0, make_dt(1970, 1, 1, 0, 0, 0, 0, 0));
    expect_dt(1549316052, make_dt(2019, 2, 4, 21, 34, 12, 0, 0));
    // Watershed: <= 2e10 is seconds, > 2e10 is milliseconds.
    expect_dt(19999999999LL, make_dt(2603, 10, 11, 11, 33, 19, 0, 0));
    expect_dt(20000000000LL, make_dt(2603, 10, 11, 11, 33, 20, 0, 0));
    expect_dt(20000000001LL, make_dt(1970, 8, 20, 11, 33, 20, 1000, 0));
    expect_dt(1494012444000LL, make_dt(2017, 5, 5, 19, 27, 24, 0, 0));
    expect_dt(1549316052104LL, make_dt(2019, 2, 4, 21, 34, 12, 104000, 0));
    // Pre-epoch stays on the correct side of midnight.
    expect_dt(-1, make_dt(1969, 12, 31, 23, 59, 59, 0, 0));
}

TEST_CASE("datetime from float timestamps") {
    auto check = [](double v, const DateTime& expected) {
        auto r = datetime_from_float(v);
        INFO("value: " << v);
        INFO("error: " << r.error);
        CHECK(r.ok);
        CHECK(r.value == expected);
    };
    check(1494012444.883309, make_dt(2017, 5, 5, 19, 27, 24, 883309, 0));
    check(8640000000.0, make_dt(2243, 10, 17, 0, 0, 0, 0, 0));
    check(92534400000.0, make_dt(1972, 12, 7, 0, 0, 0, 0, 0));
    // Negative millisecond timestamps borrow a whole second.
    auto r = datetime_from_float(-1494012444000.883309);
    CHECK(r.ok);
    CHECK(r.value == make_dt(1922, 8, 29, 4, 32, 35, 999117, 0));
}

TEST_CASE("numeric out-of-range documents") {
    auto err = [](double v) {
        auto r = datetime_from_float(v);
        return r.ok ? std::string("<unexpected success>") : r.error;
    };
    CHECK(err(std::nan("")) == "NaN values not permitted");
    CHECK(err(std::numeric_limits<double>::infinity()) ==
          "dates after 9999 are not supported as unix timestamps");
    CHECK(err(-std::numeric_limits<double>::infinity()) ==
          "dates before 0000 are not supported as unix timestamps");
    CHECK(err(1e50) == "dates after 9999 are not supported as unix timestamps");

    auto err_i = [](int64_t v) {
        auto r = datetime_from_timestamp(v, 0);
        return r.ok ? std::string("<unexpected success>") : r.error;
    };
    CHECK(err_i(1549316052104324LL) ==
          "dates after 9999 are not supported as unix timestamps");
    CHECK(err_i(1549316052104324096LL) ==
          "dates after 9999 are not supported as unix timestamps");
}

TEST_CASE("timestamp range boundaries") {
    auto ok_hi = datetime_from_timestamp(kMaxUnixTimestamp, 0, TimestampUnit::Seconds);
    CHECK(ok_hi.ok);
    CHECK(ok_hi.value == make_dt(9999, 12, 31, 23, 59, 59, 0, 0));

    auto bad_hi = datetime_from_timestamp(kMaxUnixTimestamp + 1, 0, TimestampUnit::Seconds);
    CHECK_FALSE(bad_hi.ok);
    CHECK(bad_hi.error == "dates after 9999 are not supported as unix timestamps");

    // The lower bound parses to year 0; rejecting year 0 is the conversion's job.
    auto lo = datetime_from_timestamp(kMinUnixTimestamp, 0, TimestampUnit::Seconds);
    CHECK(lo.ok);
    CHECK(lo.value.date == Date{0, 1, 1});

    auto bad_lo = datetime_from_timestamp(kMinUnixTimestamp - 1, 0, TimestampUnit::Seconds);
    CHECK_FALSE(bad_lo.ok);
    CHECK(bad_lo.error == "dates before 0000 are not supported as unix timestamps");
}

TEST_CASE("numeric strings are timestamps; string errors win when both fail") {
    expect_dt(1493942400LL, make_dt(2017, 5, 5, 0, 0, 0, 0, 0));
    expect_dt(1493942400000LL, make_dt(2017, 5, 5, 0, 0, 0, 0, 0));
    // A numeric string is read as a timestamp in the requested unit; the
    // fraction is a fraction of that unit.
    expect_dt("1494012444", make_dt(2017, 5, 5, 19, 27, 24, 0, 0));
    expect_dt("1494012444.883309", make_dt(2017, 5, 5, 19, 27, 24, 883309, 0));
    expect_dt("1494012444000.883309", make_dt(2017, 5, 5, 19, 27, 24, 883, 0));
    expect_dt("0", make_dt(1970, 1, 1, 0, 0, 0, 0, 0));
    expect_dt("-0", make_dt(1970, 1, 1, 0, 0, 0, 0, 0));
    // 20120423 is a timestamp, not a compact date.
    expect_dt("20120423", make_dt(1970, 8, 21, 21, 0, 23, 0, 0));
}

// ===========================================================================
// val_temporal_unit
// ===========================================================================

TEST_CASE("timestamp_unit_from_string") {
    bool ok = false;
    CHECK(timestamp_unit_from_string("seconds", &ok) == TimestampUnit::Seconds);
    CHECK(ok);
    CHECK(timestamp_unit_from_string("milliseconds", &ok) == TimestampUnit::Milliseconds);
    CHECK(ok);
    CHECK(timestamp_unit_from_string("infer", &ok) == TimestampUnit::Infer);
    CHECK(ok);
    CHECK(timestamp_unit_from_string("nonsense", &ok) == TimestampUnit::Infer);
    CHECK_FALSE(ok);
}

TEST_CASE("val_temporal_unit: datetime") {
    struct Case { const char* unit; int64_t input; DateTime expected; };
    const std::vector<Case> cases = {
        {"seconds", 1654646400, make_dt(2022, 6, 8, 0, 0, 0, 0, 0)},
        {"seconds", 1654646400, make_dt(2022, 6, 8, 0, 0, 0, 0, 0)},
        {"seconds", 8640000000LL, make_dt(2243, 10, 17, 0, 0, 0, 0, 0)},
        {"seconds", 92534400000LL, make_dt(4902, 4, 20, 0, 0, 0, 0, 0)},
        {"milliseconds", 1654646400, make_dt(1970, 1, 20, 3, 37, 26, 400000, 0)},
        {"milliseconds", 1654646400123LL, make_dt(2022, 6, 8, 0, 0, 0, 123000, 0)},
        {"milliseconds", 8640000000LL, make_dt(1970, 4, 11, 0, 0, 0, 0, 0)},
        {"milliseconds", 92534400000LL, make_dt(1972, 12, 7, 0, 0, 0, 0, 0)},
        {"infer", 1654646400, make_dt(2022, 6, 8, 0, 0, 0, 0, 0)},
        {"infer", 1654646400123LL, make_dt(2022, 6, 8, 0, 0, 0, 123000, 0)},
        {"infer", 8640000000LL, make_dt(2243, 10, 17, 0, 0, 0, 0, 0)},
        {"infer", 92534400000LL, make_dt(1972, 12, 7, 0, 0, 0, 0, 0)},
    };
    for (const auto& c : cases) {
        INFO("unit: " << c.unit << " input: " << c.input);
        bool ok = false;
        TimestampUnit u = timestamp_unit_from_string(c.unit, &ok);
        REQUIRE(ok);
        auto r = datetime_from_timestamp(c.input, 0, u);
        INFO("error: " << r.error);
        CHECK(r.ok);
        CHECK(r.value == c.expected);
    }
}

TEST_CASE("val_temporal_unit: date") {
    struct Case { const char* unit; int64_t input; Date expected; };
    const std::vector<Case> cases = {
        {"seconds", 1654646400, Date{2022, 6, 8}},
        {"seconds", 8640000000LL, Date{2243, 10, 17}},
        {"seconds", 92534400000LL, Date{4902, 4, 20}},
        {"milliseconds", 1654646400000LL, Date{2022, 6, 8}},
        {"milliseconds", 8640000000LL, Date{1970, 4, 11}},
        {"milliseconds", 92534400000LL, Date{1972, 12, 7}},
        {"infer", 1654646400, Date{2022, 6, 8}},
        {"infer", 1654646400000LL, Date{2022, 6, 8}},
        {"infer", 8640000000LL, Date{2243, 10, 17}},
        {"infer", 92534400000LL, Date{1972, 12, 7}},
    };
    for (const auto& c : cases) {
        INFO("unit: " << c.unit << " input: " << c.input);
        bool ok = false;
        TimestampUnit u = timestamp_unit_from_string(c.unit, &ok);
        REQUIRE(ok);
        auto r = datetime_from_timestamp(c.input, 0, u);
        INFO("error: " << r.error);
        CHECK(r.ok);
        CHECK(r.value.date == c.expected);
    }
}

// ===========================================================================
// Time-of-day from numbers
// ===========================================================================

TEST_CASE("time from numbers") {
    auto check = [](int64_t secs, const Time& expected) {
        auto r = time_from_timestamp(secs);
        INFO("seconds: " << secs);
        INFO("error: " << r.error);
        CHECK(r.ok);
        CHECK(r.value == expected);
    };
    check(0, make_time(0, 0, 0, 0, 0));
    check(3610, make_time(1, 0, 10, 0, 0));
    check(86399, make_time(23, 59, 59, 0, 0));

    auto rf = [](double v) {
        auto r = time_from_float(v);
        return r.ok ? r.value.microsecond : -1;
    };
    CHECK(time_from_float(3600.5).value == make_time(1, 0, 0, 500000, 0));
    CHECK(time_from_float(86399.5).value == make_time(23, 59, 59, 500000, 0));
    CHECK(rf(9.9e-05) == 99);

    auto err = [](int64_t v) {
        auto r = time_from_timestamp(v);
        return r.ok ? std::string("<unexpected success>") : r.error;
    };
    CHECK(err(-1) == "time in seconds should be positive");
    CHECK(err(86400) == "numeric times may not exceed 86,399 seconds");
    CHECK(err(10000000000LL) == "numeric times may not exceed 86,399 seconds");
    CHECK(time_from_float(std::nan("")).error == "NaN values not permitted");
}

// ===========================================================================
// Bytes and str share a path
// ===========================================================================

TEST_CASE("bytes input parses identically to str") {
    const char raw[] = "2012-04-23";
    auto from_bytes = parse_date_bytes(raw, sizeof(raw) - 1);
    auto from_str = parse_date_string("2012-04-23");
    CHECK(from_bytes.ok);
    CHECK(from_str.ok);
    CHECK(from_bytes.value == from_str.value);

    const char t_raw[] = "10:20:30.400";
    auto t_bytes = parse_time_bytes(t_raw, sizeof(t_raw) - 1);
    CHECK(t_bytes.ok);
    CHECK(t_bytes.value == make_time(10, 20, 30, 400000));
}

TEST_SUITE_END();
