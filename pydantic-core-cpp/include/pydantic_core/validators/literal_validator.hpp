#pragma once

#include <vector>

namespace pydantic_core {

// LiteralValidator - validates literal values (allowed values)
// Analogous to Rust's LiteralValidator in validators/literal.rs
struct LiteralValidator {
std::vector<ValidatedValue> allowed_values;  // Allowed literal values
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        // For literal validation, we need to check if the input matches any allowed value
        // This requires comparing the parsed input value
        
        // In lax mode, we may need to parse the input first
        // For now, use string comparison as placeholder
        
        auto input_repr = input.as_error_value().repr;
        
        for (const auto& allowed : allowed_values) {
            if (allowed.repr() == input_repr) {
                return allowed;  // Return the matching literal value
            }
        }
        
        // Build expected values string for error message
        std::string expected = "[";
        for (size_t i = 0; i < allowed_values.size(); ++i) {
            if (i > 0) expected += ", ";
            expected += allowed_values[i].repr();
        }
        expected += "]";
        
        return ValError::line_error(
            ErrorType(ErrorType::Kind::LiteralMismatch),
            Location(),
            input.as_error_value().repr
        );
    }
    
    std::string name() const { return "literal"; }
};

} // namespace pydantic_core