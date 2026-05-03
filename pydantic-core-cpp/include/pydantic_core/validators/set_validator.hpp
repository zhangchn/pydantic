#pragma once


namespace pydantic_core {

// SetValidator - validates set values (unique list)
// Analogous to Rust's SetValidator in validators/set.rs
struct SetValidator {
    bool strict = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<CombinedValidator> item_validator;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        // Sets are validated like lists, but with uniqueness constraint
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_list(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        std::unique_ptr<ValidatedList> validated_list = result.value().value();
        
        // Check min_length
        if (min_length && validated_list->size() < *min_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::SetTooShort, static_cast<int64_t>(*min_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check max_length
        if (max_length && validated_list->size() > *max_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::SetTooLong, static_cast<int64_t>(*max_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // For set, we'd also need to check uniqueness
        // Placeholder: return as list
        std::vector<ValidatedValue> output_items;
        return ValidatedValue(output_items);
    }
    
    std::string name() const { return "set"; }
};

// FrozenSetValidator - validates frozenset values (immutable unique list)
// Analogous to Rust's FrozenSetValidator in validators/frozenset.rs
struct FrozenSetValidator {
    bool strict = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<CombinedValidator> item_validator;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        // Same as SetValidator
        SetValidator set_val{strict, min_length, max_length, item_validator};
        return set_val.validate(input, state);
    }
    
    std::string name() const { return "frozenset"; }
};

} // namespace pydantic_core