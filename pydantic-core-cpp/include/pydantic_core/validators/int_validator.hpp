#pragma once


namespace pydantic_core {

// IntValidator - validates integer values without constraints
// Analogous to Rust's IntValidator in validators/int.rs
struct IntValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_int(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        // Extract the int value from ValMatch<EitherInt>
        EitherInt either_int = result.value().value();
        
        // Convert to int64_t if possible
        auto i64 = either_int.as_i64();
        if (i64) {
            return ValidatedValue(*i64);
        }
        
        // If too large for int64_t, use uint64_t
        auto u64 = either_int.as_u64();
        if (u64) {
            return ValidatedValue(*u64);
        }
        
        // Should not happen if validate_int succeeded
        return ValError::line_error(
            ErrorType(ErrorType::Kind::IntType),
            Location(),
            input.as_error_value().repr
        );
    }
    
    std::string name() const { return "int"; }
};

// ConstrainedIntValidator - validates integers with constraints
// Analogous to Rust's ConstrainedIntValidator in validators/int.rs
struct ConstrainedIntValidator {
    bool strict = false;
    std::optional<int64_t> multiple_of;
    std::optional<int64_t> le;  // less than or equal
    std::optional<int64_t> lt;  // less than
    std::optional<int64_t> ge;  // greater than or equal
    std::optional<int64_t> gt;  // greater than
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_int(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        EitherInt either_int = result.value().value();
        auto i64_val = either_int.as_i64();
        
        if (!i64_val) {
            // Value too large - can't check constraints
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IntType),
                Location(),
                "integer too large for constraint checking"
            );
        }
        
        int64_t value = *i64_val;
        
        // Check multiple_of constraint
        if (multiple_of && value % *multiple_of != 0) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IntMultipleOf, *multiple_of),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check le (less than or equal)
        if (le && value > *le) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IntLessThanEqual, *le),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check lt (less than)
        if (lt && value >= *lt) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IntLessThan, *lt),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check ge (greater than or equal)
        if (ge && value < *ge) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IntGreaterThanEqual, *ge),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check gt (greater than)
        if (gt && value <= *gt) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IntGreaterThan, *gt),
                Location(),
                input.as_error_value().repr
            );
        }
        
        return ValidatedValue(value);
    }
    
    std::string name() const { return "constrained-int"; }
};

} // namespace pydantic_core