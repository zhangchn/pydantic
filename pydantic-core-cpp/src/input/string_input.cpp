#include "pydantic_core/string_input.hpp"
#include "pydantic_core/error_types.hpp"
#include "pydantic_core/result.hpp"
#include <sstream>
#include <algorithm>

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
    
    return ValError::line_error(PydanticKnownError::bool_type(),
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
    
    return ValError::line_error(PydanticKnownError::int_type(),
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
    
    return ValError::line_error(PydanticKnownError::float_type(),
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

} // namespace pydantic_core