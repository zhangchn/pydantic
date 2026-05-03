#pragma once

#include <memory>

namespace pydantic_core {

// ListValidator - validates list/array values
// Analogous to Rust's ListValidator in validators/list.rs
struct ListValidator {
    bool strict = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<CombinedValidator> item_validator;  // Validator for each item
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_list(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        std::unique_ptr<ValidatedList> validated_list = result.value().value();
        
        // Check min_length
        if (min_length && validated_list->size() < *min_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::ListTooShort, static_cast<int64_t>(*min_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check max_length
        if (max_length && validated_list->size() > *max_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::ListTooLong, static_cast<int64_t>(*max_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Validate each item
        std::vector<ValidatedValue> output_items;
        auto entries = validated_list->entries();
        
        for (const auto& entry : entries) {
            // Push location for this item
            state.push_index(entry.index);
            
            // Note: For now, we need to re-validate the item representation
            // In a full implementation, we'd have the actual Input for each item
            // This is a placeholder - actual recursive validation requires
            // the input to provide item access
            
            // For JSON input, we'd use the JSON element directly
            // For Python input, we'd use the Python object directly
            
            // Pop location after validation
            state.pop_location();
            
            // Placeholder: just store the string representation
            // Real implementation would validate recursively with item_validator
        }
        
        // If no item_validator, return the raw items
        if (!item_validator) {
            // Placeholder output
            return ValidatedValue(output_items);
        }
        
        // Would validate items here with item_validator
        // For now, return empty list as placeholder
        return ValidatedValue(output_items);
    }
    
    std::string name() const { return "list"; }
};

} // namespace pydantic_core