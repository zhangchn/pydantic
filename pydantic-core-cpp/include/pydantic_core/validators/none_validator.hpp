#pragma once


namespace pydantic_core {

// NoneValidator - validates None/null values
// Analogous to Rust's NoneValidator in validators/none.rs
struct NoneValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        if (input.is_none()) {
            return ValidatedValue(std::monostate{});  // Return None
        }
        
        // In strict mode, only accept actual None
        // In lax mode, could accept more (e.g., empty strings for some inputs)
        if (use_strict) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::NoneType),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // For lax mode, still require is_none() for NoneValidator
        // (Unlike Rust, we keep it simple here)
        return ValError::line_error(
            ErrorType(ErrorType::Kind::NoneType),
            Location(),
            input.as_error_value().repr
        );
    }
    
    std::string name() const { return "none"; }
};

} // namespace pydantic_core