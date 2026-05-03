#pragma once


namespace pydantic_core {

// BytesValidator - validates bytes values
// Analogous to Rust's BytesValidator in validators/bytes.rs
struct BytesValidator {
    bool strict = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_bytes(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        EitherBytes either_bytes = result.value().value();
        
        // Check min_length
        if (min_length && either_bytes.size() < *min_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::BytesTooShort, static_cast<int64_t>(*min_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check max_length
        if (max_length && either_bytes.size() > *max_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::BytesTooLong, static_cast<int64_t>(*max_length)),
                Location(),
                input.as_error_value().repr
            );
        }
        
        return ValidatedValue(either_bytes.to_vector());
    }
    
    std::string name() const { return "bytes"; }
};

} // namespace pydantic_core