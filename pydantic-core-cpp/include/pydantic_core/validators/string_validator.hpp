#pragma once

#include <regex>

namespace pydantic_core {

// StringValidator - validates string values without constraints
// Analogous to Rust's StrValidator in validators/string.rs
struct StringValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_str(use_strict, false);
        if (result.is_err()) {
            return result.error();
        }
        
        std::string value = result.value().value().to_string();
        return ValidatedValue(value);
    }
    
    std::string name() const { return "str"; }
};

// ConstrainedStringValidator - validates strings with constraints
// Analogous to Rust's StrConstrainedValidator in validators/string.rs
struct ConstrainedStringValidator {
    bool strict = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::optional<std::regex> pattern;
    bool to_lower = false;
    bool to_upper = false;
    bool to_title = false;  // Title case (first letter of each word uppercase)
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_str(use_strict, false);
        if (result.is_err()) {
            return result.error();
        }
        
        std::string value = result.value().value().to_string();
        
        // Check min_length
        if (min_length && value.length() < *min_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::StringTooShort, static_cast<int64_t>(*min_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check max_length
        if (max_length && value.length() > *max_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::StringTooLong, static_cast<int64_t>(*max_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check pattern (regex)
        if (pattern) {
            if (!std::regex_match(value, *pattern)) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::StringPatternMismatch),
                    Location(),
                    input.as_error_value().repr
                );
            }
        }
        
        // Apply transformations
        if (to_lower) {
            std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        } else if (to_upper) {
            std::transform(value.begin(), value.end(), value.begin(), ::toupper);
        } else if (to_title) {
            // Simple title case: first letter of each word uppercase
            bool prev_space = true;
            for (auto& c : value) {
                if (prev_space && std::isalpha(c)) {
                    c = std::toupper(c);
                }
                prev_space = std::isspace(c);
            }
        }
        
        return ValidatedValue(value);
    }
    
    std::string name() const { return "constrained-str"; }
};

} // namespace pydantic_core