#pragma once


namespace pydantic_core {

// BoolValidator - validates boolean values
// Analogous to Rust's BoolValidator in validators/bool.rs
struct BoolValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_bool(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        // Extract the bool value from ValMatch
        bool value = result.value().value();
        return ValidatedValue(value);
    }
    
    std::string name() const { return "bool"; }
};

} // namespace pydantic_core