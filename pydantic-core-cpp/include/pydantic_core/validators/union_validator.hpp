#pragma once

#include <vector>
#include <memory>

namespace pydantic_core {

// UnionValidator - validates union types (try validators in order)
// Analogous to Rust's UnionValidator in validators/union.rs
struct UnionValidator {
    std::vector<std::shared_ptr<CombinedValidator>> validators;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        // Try each validator in order
        std::vector<ValError> errors;
        
        for (const auto& validator : validators) {
            auto result = validate_combined(*validator, input, state);
            if (result.is_ok()) {
                return result.value();
            }
            
            // Collect error but continue trying
            errors.push_back(result.error());
        }
        
        // All validators failed - combine errors
        // Return the first error as representative
        if (!errors.empty()) {
            // Could combine all errors into a UnionError
            // For now, return first error
            return errors[0];
        }
        
        return ValError::line_error(
            ErrorType(ErrorType::Kind::UnionType),
            Location(),
            input.as_error_value().repr
        );
    }
    
    std::string name() const { return "union"; }
};

} // namespace pydantic_core