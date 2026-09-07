#pragma once

#include <variant>
#include <string>
#include <optional>
#include <vector>
#include <cmath>
#include <limits>
#include <memory>
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

// Date representation (year-month-day without timezone)
struct Date {
    int year;
    int month;
    int day;
};

// Time representation (hour-minute-second with optional microsecond and timezone offset)
struct Time {
    int hour;
    int minute;
    int second;
    int microsecond = 0;
    std::optional<int> tz_offset;  // UTC offset in minutes, None = unknown
};

// DateTime representation (Date + Time + optional timezone)
struct DateTime {
    Date date;
    Time time;

    int year() const { return date.year; }
    int month() const { return date.month; }
    int day() const { return date.day; }
    int hour() const { return time.hour; }
    int minute() const { return time.minute; }
    int second() const { return time.second; }
    int microsecond() const { return time.microsecond; }
    std::optional<int> tz_offset() const { return time.tz_offset; }
};

// Either types for date/time validation results
struct EitherDate {
    Date value;
    bool is_lax = false;

    EitherDate() : value{0, 0, 0} {}
    explicit EitherDate(Date d) : value(d) {}

    Date as_raw() const { return value; }
};

struct EitherTime {
    Time value;
    bool is_lax = false;

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

// Timedelta representation (days/seconds/microseconds, like Python's timedelta)
struct Timedelta {
    int days = 0;
    int seconds = 0;
    int microseconds = 0;
};

struct EitherTimedelta {
    Timedelta value;
    bool is_lax = false;

    EitherTimedelta() : value{} {}
    explicit EitherTimedelta(Timedelta td) : value(td) {}

    Timedelta as_raw() const { return value; }
};

// Parse a timedelta string: ISO 8601 duration (P4Y/P4M/P4W/P4D/P0.5D/PT5H...),
// HH:MM:SS[.frac] with optional "[Nd,]HH:MM:SS" days prefix, or either form
// with a leading '-'. Returns nullopt when the string is not a valid duration.
inline std::optional<Timedelta> try_parse_timedelta_str(const std::string& input) {
    std::string s = input;
    bool negative = false;
    if (!s.empty() && s[0] == '-') {
        negative = true;
        s = s.substr(1);
    }
    try {
        long long total_seconds = 0;
        long long micros = 0;
        bool parsed_any = false;

        // ISO 8601 duration: P[nY][nM][nD][T[nH][nM][nS]] / P[nW]
        if (!s.empty() && s[0] == 'P') {
            bool in_time = false;
            std::string num;
            size_t i = 1;
            while (i < s.size()) {
                char c = s[i];
                if ((c >= '0' && c <= '9') || c == '.') {
                    num += c;
                } else if (c == 'T') {
                    in_time = true;
                    num.clear();
                } else {
                    if (num.empty()) return std::nullopt;
                    double val = std::stod(num);
                    switch (c) {
                        case 'Y': if (!in_time) { total_seconds += static_cast<long long>(val * 365.0 * 86400.0); parsed_any = true; } break;
                        case 'M': if (!in_time) { total_seconds += static_cast<long long>(val * 30.0 * 86400.0); parsed_any = true; } else { total_seconds += static_cast<long long>(val * 60.0); parsed_any = true; } break;
                        case 'D': if (!in_time) { total_seconds += static_cast<long long>(val * 86400.0); parsed_any = true; } break;
                        case 'H': if (in_time) { total_seconds += static_cast<long long>(val * 3600.0); parsed_any = true; } break;
                        case 'S': if (in_time) { double whole = 0.0; double frac = std::modf(val, &whole); total_seconds += static_cast<long long>(whole); micros += std::llround(frac * 1e6); parsed_any = true; } break;
                        case 'W': if (!in_time) { total_seconds += static_cast<long long>(val * 7.0 * 86400.0); parsed_any = true; } break;
                        default: return std::nullopt;
                    }
                    num.clear();
                }
                i++;
            }
            if (!parsed_any) return std::nullopt;
        } else {
            // [Nd,]HH:MM:SS[.frac]
            long long day_part = 0;
            std::string hms = s;
            size_t comma = s.find(',');
            if (comma != std::string::npos) {
                std::string daystr = s.substr(0, comma);
                if (daystr.size() >= 2 && daystr.back() == 'd') {
                    day_part = std::stoll(daystr.substr(0, daystr.size() - 1));
                    hms = s.substr(comma + 1);
                } else {
                    return std::nullopt;
                }
            }
            size_t c1 = hms.find(':');
            if (c1 == std::string::npos) return std::nullopt;
            size_t c2 = hms.find(':', c1 + 1);
            if (c2 == std::string::npos) return std::nullopt;
            // Third colon means seconds contain a ':' — not valid here
            if (hms.find(':', c2 + 1) != std::string::npos) return std::nullopt;
            long long h = std::stoll(hms.substr(0, c1));
            long long m = std::stoll(hms.substr(c1 + 1, c2 - c1 - 1));
            double sv = std::stod(hms.substr(c2 + 1));
            double sw = 0.0;
            double sf = std::modf(sv, &sw);
            total_seconds = day_part * 86400LL + h * 3600LL + m * 60LL + static_cast<long long>(sw);
            micros = std::llround(sf * 1e6);
            parsed_any = true;
        }

        if (!parsed_any) return std::nullopt;
        if (negative) {
            total_seconds = -total_seconds;
            micros = -micros;
        }
        // Normalize into days/seconds/microseconds
        long long days = total_seconds / 86400;
        long long rem = total_seconds % 86400;
        if (micros < 0) {
            // borrow one second for negative microseconds
            rem -= 1;
            micros += 1000000;
        }
        return Timedelta{static_cast<int>(days), static_cast<int>(rem), static_cast<int>(micros)};
    } catch (...) {
        return std::nullopt;
    }
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
    virtual ValResult<ValMatch<EitherString>> validate_str(bool strict, bool coerce_numbers = false) const = 0;
    virtual ValResult<ValMatch<EitherBytes>> validate_bytes(bool strict) const = 0;
    virtual ValResult<ValMatch<bool>> validate_bool(bool strict) const = 0;
    virtual ValResult<ValMatch<EitherInt>> validate_int(bool strict) const = 0;
    virtual ValResult<ValMatch<EitherFloat>> validate_float(bool strict) const = 0;

    // Date/time validation methods
    virtual bool is_date() const { return false; }
    virtual bool is_datetime() const { return false; }
    virtual bool is_time() const { return false; }
    virtual ValResult<ValMatch<EitherDate>> validate_date(bool strict) const = 0;
    virtual ValResult<ValMatch<EitherDateTime>> validate_datetime(bool strict) const = 0;
    virtual ValResult<ValMatch<EitherTime>> validate_time(bool strict) const = 0;
    virtual ValResult<ValMatch<EitherTimedelta>> validate_timedelta(bool strict) const = 0;

    // Container validation
    virtual ValResult<std::unique_ptr<ValidatedDict>> validate_dict(bool strict) const = 0;
    virtual ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_list(bool strict) const = 0;
    virtual ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> validate_tuple(bool strict) const = 0;

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
    return ValError::line_error(ErrorType(kind), loc, input.as_error_value().repr);
}

} // namespace pydantic_core