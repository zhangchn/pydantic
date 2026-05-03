#pragma once

#include <memory>

namespace pydantic_core {

// DictValidator - validates dict/object values
// Analogous to Rust's DictValidator in validators/dict.rs
struct DictValidator {
    bool strict = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<CombinedValidator> keys_validator;   // Validator for keys
    std::shared_ptr<CombinedValidator> values_validator; // Validator for values
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_dict(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        std::unique_ptr<ValidatedDict> validated_dict = result.value();
        
        // Check min_length
        if (min_length && validated_dict->size() < *min_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DictTooShort, static_cast<int64_t>(*min_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check max_length
        if (max_length && validated_dict->size() > *max_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DictTooLong, static_cast<int64_t>(*max_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Validate keys and values
        std::vector<std::pair<std::string, ValidatedValue>> output_items;
        auto entries = validated_dict->entries();
        
        for (const auto& entry : entries) {
            // Push location for this key
            state.push_key(entry.key);
            
            // Validate key with keys_validator (if present)
            // Validate value with values_validator (if present)
            // Placeholder for recursive validation
            
            state.pop_location();
            
            // Placeholder: store raw key-value
            output_items.emplace_back(entry.key, ValidatedValue(entry.value_repr));
        }
        
        return ValidatedValue(output_items);
    }
    
    std::string name() const { return "dict"; }
};

} // namespace pydantic_core