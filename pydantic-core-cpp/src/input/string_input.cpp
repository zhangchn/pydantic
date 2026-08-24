#include "pydantic_core/string_input.hpp"
#include "pydantic_core/error_types.hpp"
#include "pydantic_core/result.hpp"
#include <algorithm>
#include <memory>
#include <sstream>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace pydantic_core {

InputValue StringInput::as_error_value() const {
    if (single_value_) {
        std::string repr = "'" + *single_value_ + "'";
        if (repr.length() > 100) {
            repr = "'" + single_value_->substr(0, 97) + "...'";
        }
        return InputValue(repr);
    }
    return InputValue("{string mapping}");
}

bool StringInput::is_none() const {
    if (single_value_) {
        std::string v = *single_value_;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        return v == "none" || v == "null" || v == "" || v == "~";
    }
    return false;
}

py::object StringInput::as_python_object() const {
    if (single_value_) {
        return py::str(*single_value_);
    }
    // Return mapping as Python dict
    py::dict d;
    for (const auto& [key, value] : mapping_) {
        d[py::str(key)] = py::str(value);
    }
    return d;
}

ValResult<ValMatch<EitherString>> StringInput::validate_str(bool strict, bool coerce_numbers) const {
    if (single_value_) {
        // String input is always a string (exact match)
        return ValMatch<EitherString>::exact(EitherString(*single_value_));
    }
    
    // Mapping is not a string
    return ValError::line_error(PydanticKnownError::string_type(),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<EitherBytes>> StringInput::validate_bytes(bool strict) const {
    if (single_value_) {
        // Treat string as bytes in lax mode
        if (!strict) {
            return ValMatch<EitherBytes>::lax(EitherBytes(std::string_view(*single_value_)));
        }
    }
    
    return ValError::line_error(PydanticKnownError::bytes_type(),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<bool>> StringInput::validate_bool(bool strict) const {
    if (single_value_) {
        std::string v = *single_value_;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        
        // Check boolean strings
        if (v == "true" || v == "1" || v == "on" || v == "yes") {
            return ValMatch<bool>::lax(true);
        }
        if (v == "false" || v == "0" || v == "off" || v == "no") {
            return ValMatch<bool>::lax(false);
        }
    }
    
    return ValError::line_error(ErrorType(ErrorType::Kind::BoolParsing),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<EitherInt>> StringInput::validate_int(bool strict) const {
    if (single_value_) {
        std::string v = *single_value_;
        try {
            // Try parsing as integer
            if (v.find('.') == std::string::npos) {
                int64_t i = std::stoll(v);
                return ValMatch<EitherInt>::lax(EitherInt(i));
            }
        } catch (...) {}
    }

    return ValError::line_error(ErrorType(ErrorType::Kind::IntParsing),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<EitherFloat>> StringInput::validate_float(bool strict) const {
    if (single_value_) {
        std::string v = *single_value_;
        try {
            double d = std::stod(v);
            return ValMatch<EitherFloat>::lax(EitherFloat(d));
        } catch (...) {}
    }

    return ValError::line_error(ErrorType(ErrorType::Kind::FloatParsing),
                               Location(), as_error_value().repr);
}

ValResult<std::unique_ptr<ValidatedDict>> StringInput::validate_dict(bool strict) const {
    if (is_mapping()) {
        return ValResult<std::unique_ptr<ValidatedDict>>(
            std::unique_ptr<ValidatedDict>(std::make_unique<StringValidatedDict>(mapping_).release()));
    }
    
    return ValError::line_error(PydanticKnownError::dict_type(),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<std::unique_ptr<ValidatedList>>> StringInput::validate_list(bool strict) const {
    // String input can't be a list
    return ValError::line_error(PydanticKnownError::list_type(),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> StringInput::validate_tuple(bool strict) const {
    // String input can't be a tuple
    return ValError::line_error(PydanticKnownError::tuple_type(),
                               Location(), as_error_value().repr);
}

// StringValidatedDict implementation
std::vector<ValidatedDict::Entry> StringValidatedDict::entries() const {
    std::vector<Entry> result;
    for (const auto& [key, value] : mapping_) {
        Entry e;
        e.key = key;
        e.value_repr = "'" + value + "'";
        result.push_back(e);
    }
    return result;
}

std::vector<std::string> StringValidatedDict::keys() const {
    std::vector<std::string> result;
    for (const auto& [key, _] : mapping_) {
        result.push_back(key);
    }
    return result;
}

bool StringValidatedDict::has_key(const std::string& key) const {
    return mapping_.find(key) != mapping_.end();
}

std::optional<ValidatedDict::Entry> StringValidatedDict::get(const std::string& key) const {
    auto it = mapping_.find(key);
    if (it != mapping_.end()) {
        Entry e;
        e.key = key;
        e.value_repr = "'" + it->second + "'";
        return e;
    }
    return std::nullopt;
}

std::optional<py::object> StringValidatedDict::get_value(const std::string& key) const {
    auto it = mapping_.find(key);
    if (it != mapping_.end()) {
        return py::str(it->second);
    }
    return std::nullopt;
}

// ============================================================================
// ISO 8601 parsing helper
// ============================================================================

static std::optional<DateTime> try_parse_iso8601(const std::string& s) {
    try {
        if (s.size() < 19) return std::nullopt;
        if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') {
            return std::nullopt;
        }
        int year = std::stoi(s.substr(0, 4));
        int month = std::stoi(s.substr(5, 2));
        int day = std::stoi(s.substr(8, 2));
        int hour = std::stoi(s.substr(11, 2));
        int minute = std::stoi(s.substr(14, 2));
        int second = std::stoi(s.substr(17, 2));

        int microsecond = 0;
        size_t pos = 19;

        if (pos < s.size() && s[pos] == '.') {
            std::string frac;
            pos++;
            while (pos < s.size() && std::isdigit(s[pos])) {
                frac += s[pos];
                pos++;
            }
            if (frac.size() > 6) frac = frac.substr(0, 6);
            while (frac.size() < 6) frac += '0';
            if (!frac.empty()) microsecond = std::stoi(frac);
        }

        std::optional<int> tz_offset;
        if (pos < s.size()) {
            if (s[pos] == 'Z') {
                tz_offset = 0;
            } else if (s[pos] == '+' || s[pos] == '-') {
                char sign = s[pos];
                pos++;
                if (pos + 4 < s.size() && s[pos + 2] == ':') {
                    int tz_hour = std::stoi(s.substr(pos, 2));
                    int tz_min = std::stoi(s.substr(pos + 3, 2));
                    int offset = tz_hour * 60 + tz_min;
                    tz_offset = (sign == '-') ? -offset : offset;
                }
            }
        }

        return DateTime{Date{year, month, day}, Time{hour, minute, second, microsecond, tz_offset}};
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// Date/time validation
// ============================================================================

ValResult<ValMatch<EitherDate>> StringInput::validate_date(bool strict) const {
    if (!single_value_) {
        return type_error(ErrorType::Kind::DateType, *this, this->current_location());
    }
    const std::string& s = *single_value_;

    // Try to parse as ISO 8601 date
    try {
        if (s.size() >= 10 && s[4] == '-' && s[7] == '-') {
            int year = std::stoi(s.substr(0, 4));
            int month = std::stoi(s.substr(5, 2));
            int day = std::stoi(s.substr(8, 2));
            if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
                if (s.size() > 10) {
                    // Has time component: not a pure date string.
                    // Strict mode rejects datetime strings entirely.
                    if (strict) {
                        return ValError::line_error(
                            ErrorType(ErrorType::Kind::DateParsing),
                            this->current_location(),
                            this->as_error_value().repr
                        );
                    }
                    // Lax mode: midnight datetimes coerce to date; others are inexact
                    auto dt = try_parse_iso8601(s);
                    if (dt && dt->time.hour == 0 && dt->time.minute == 0 &&
                        dt->time.second == 0 && dt->time.microsecond == 0) {
                        return ValMatch<EitherDate>::lax(EitherDate(dt->date));
                    }
                    if (dt) {
                        return ValError::line_error(
                            ErrorType(ErrorType::Kind::DateFromDatetimeInexact),
                            this->current_location(),
                            this->as_error_value().repr
                        );
                    }
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::DateParsing),
                        this->current_location(),
                        this->as_error_value().repr
                    );
                }
                return ValMatch<EitherDate>::lax(EitherDate(Date{year, month, day}));
            }
        }
    } catch (...) {}
    return ValError::line_error(
        ErrorType(ErrorType::Kind::DateParsing),
        this->current_location(),
        this->as_error_value().repr
    );
}

ValResult<ValMatch<EitherDateTime>> StringInput::validate_datetime(bool strict) const {
    if (!single_value_) {
        return type_error(ErrorType::Kind::DateTimeType, *this, this->current_location());
    }
    const std::string& s = *single_value_;

    auto parsed = try_parse_iso8601(s);
    if (parsed) {
        return ValMatch<EitherDateTime>::lax(EitherDateTime(*parsed));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::DateTimeParsing),
        this->current_location(),
        this->as_error_value().repr
    );
}

ValResult<ValMatch<EitherTime>> StringInput::validate_time(bool strict) const {
    if (!single_value_) {
        return type_error(ErrorType::Kind::TimeType, *this, this->current_location());
    }
    const std::string& s = *single_value_;

    try {
        if (s.size() >= 8 && s[2] == ':' && s[5] == ':') {
            int hour = std::stoi(s.substr(0, 2));
            int minute = std::stoi(s.substr(3, 2));
            int second = std::stoi(s.substr(6, 2));
            int microsecond = 0;
            if (s.size() > 8 && s[8] == '.') {
                std::string frac = s.substr(9);
                if (frac.size() > 6) frac = frac.substr(0, 6);
                while (frac.size() < 6) frac += '0';
                microsecond = std::stoi(frac);
            }
            return ValMatch<EitherTime>::lax(EitherTime(Time{hour, minute, second, microsecond, std::nullopt}));
        }
    } catch (...) {}
    return ValError::line_error(
        ErrorType(ErrorType::Kind::TimeParsing),
        this->current_location(),
        this->as_error_value().repr
    );
}

ValResult<ValMatch<EitherTimedelta>> StringInput::validate_timedelta(bool strict) const {
    if (!single_value_) {
        return type_error(ErrorType::Kind::TimedeltaType, *this, this->current_location());
    }
    const std::string& s = *single_value_;
    auto parsed = try_parse_timedelta_str(s);
    if (parsed) {
        return ValMatch<EitherTimedelta>::lax(EitherTimedelta(*parsed));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::TimedeltaParsing),
        this->current_location(),
        this->as_error_value().repr
    );
}

} // namespace pydantic_core