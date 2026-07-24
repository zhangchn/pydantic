#pragma once

#include <variant>
#include <string>
#include <optional>
#include <vector>
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
    std::variant<int64_t, uint64_t> value;
    
    EitherInt(int64_t i) : value(i) {}
    EitherInt(uint64_t i) : value(i) {}
    
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

    EitherDateTime() : value{{0, 0, 0}, {0, 0, 0}} {}
    explicit EitherDateTime(DateTime dt) : value(dt) {}

    DateTime as_raw() const { return value; }
};

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

    // Container validation
    virtual ValResult<std::unique_ptr<ValidatedDict>> validate_dict(bool strict) const = 0;
    virtual ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_list(bool strict) const = 0;
    virtual ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> validate_tuple(bool strict) const = 0;

protected:
    /// Pointer to current validation location (set by validators before calling validate_*).
    const Location* current_loc_ = nullptr;
};

// Helper to create type error — use input.current_location() when available
inline ValError type_error(ErrorType::Kind kind, const Input& input, const Location& loc) {
    return ValError::line_error(ErrorType(kind), loc, input.as_error_value().repr);
}

} // namespace pydantic_core