#pragma once


namespace pydantic_core {

// TupleValidator - validates tuple values (fixed-length positional)
// Analogous to Rust's TupleValidator in validators/tuple.rs
struct TupleValidator {
    bool strict = false;
    std::vector<std::shared_ptr<CombinedValidator>> item_validators;  // Validators for each position
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_tuple(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        std::unique_ptr<ValidatedTuple> validated_tuple = result.value().value();
        
        // Check length matches expected
        size_t expected_len = item_validators.size();
        size_t actual_len = validated_tuple->size();
        
        if (actual_len != expected_len) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TupleLengthMismatch, static_cast<int64_t>(expected_len), static_cast<int64_t>(actual_len)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Validate each position
        std::vector<ValidatedValue> output_items;
        auto entries = validated_tuple->entries();
        
        for (size_t i = 0; i < entries.size(); ++i) {
            state.push_index(i);
            
            // Would validate with item_validators[i] here
            // Placeholder
            
            state.pop_location();
        }
        
        return ValidatedValue(output_items);
    }
    
    std::string name() const { return "tuple"; }
};

} // namespace pydantic_core