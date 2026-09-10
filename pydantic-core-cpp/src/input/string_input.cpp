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
        std::string lower = v;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Rust shared.rs::str_as_bool token set
        if (v == "0" || lower == "f" || lower == "n" || lower == "no" ||
            lower == "off" || lower == "false") {
            return ValMatch<bool>::lax(false);
        }
        if (v == "1" || lower == "t" || lower == "y" || lower == "on" ||
            lower == "yes" || lower == "true") {
            return ValMatch<bool>::lax(true);
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
// Date/time validation
// ============================================================================
//
// Parsing is delegated to the speedate port (speedate.hpp).

ValResult<ValMatch<EitherDate>> StringInput::validate_date(bool strict, TimestampUnit unit) const {
    if (!single_value_) {
        return type_error(ErrorType::Kind::DateType, *this, this->current_location());
    }
    const std::string& s = *single_value_;
    auto parsed = parse_date_bytes(s.data(), s.size(), unit);
    if (parsed.ok) {
        return ValMatch<EitherDate>::lax(EitherDate(parsed.value));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::DateParsing, "error", parsed.error),
        this->current_location(), this->as_error_value().repr
    );
}

ValResult<ValMatch<EitherDateTime>> StringInput::validate_datetime(
    bool strict, TimestampUnit unit) const {
    if (!single_value_) {
        return type_error(ErrorType::Kind::DateTimeType, *this, this->current_location());
    }
    const std::string& s = *single_value_;
    auto parsed = parse_datetime_bytes(s.data(), s.size(), unit);
    if (parsed.ok) {
        return ValMatch<EitherDateTime>::lax(EitherDateTime(parsed.value));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::DateTimeParsing, "error", parsed.error),
        this->current_location(), this->as_error_value().repr
    );
}

ValResult<ValMatch<EitherTime>> StringInput::validate_time(bool strict) const {
    if (!single_value_) {
        return type_error(ErrorType::Kind::TimeType, *this, this->current_location());
    }
    const std::string& s = *single_value_;
    auto parsed = parse_time_bytes(s.data(), s.size());
    if (parsed.ok) {
        return ValMatch<EitherTime>::lax(EitherTime(parsed.value));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::TimeParsing, "error", parsed.error),
        this->current_location(), this->as_error_value().repr
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
        ErrorType(ErrorType::Kind::TimedeltaParsing, "error", "unable to parse string as an ISO 8601 duration"),
        this->current_location(),
        this->as_error_value().repr
    );
}

} // namespace pydantic_core