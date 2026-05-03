#pragma once

#include <memory>

namespace pydantic_core {

// NullableValidator - validates Optional[T] (None or T)
// Analogous to Rust's NullableValidator in validators/nullable.rs
struct NullableValidator {
    std::shared_ptr<CombinedValidator> inner_validator;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        // If input is None, return None immediately
        if (input.is_none()) {
            return ValidatedValue(std::monostate{});  // None
        }
        
        // Otherwise, validate with inner validator
        if (inner_validator) {
            return validate_combined(*inner_validator, input, state);
        }
        
        // No inner validator - accept any non-None
        return ValidatedValue(std::monostate{});  // Placeholder
    }
    
    std::string name() const { return "nullable"; }
};

} // namespace pydantic_core