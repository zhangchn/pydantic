#pragma once


namespace pydantic_core {

// FloatValidator - validates float values without constraints
// Analogous to Rust's FloatValidator in validators/float.rs
struct FloatValidator {
    bool strict = false;
    bool allow_inf_nan = false;  // Whether to allow infinity and NaN
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_float(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        double value = result.value().value().as_double();
        
        // Check for inf/nan if not allowed
        if (!allow_inf_nan) {
            if (std::isinf(value) || std::isnan(value)) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::FloatType),
                    Location(),
                    input.as_error_value().repr
                );
            }
        }
        
        return ValidatedValue(value);
    }
    
    std::string name() const { return "float"; }
};

// ConstrainedFloatValidator - validates floats with constraints
struct ConstrainedFloatValidator {
    bool strict = false;
    bool allow_inf_nan = false;
    std::optional<double> multiple_of;
    std::optional<double> le;  // less than or equal
    std::optional<double> lt;  // less than
    std::optional<double> ge;  // greater than or equal
    std::optional<double> gt;  // greater than
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        bool use_strict = state.strict_or(strict);
        
        auto result = input.validate_float(use_strict);
        if (result.is_err()) {
            return result.error();
        }
        
        double value = result.value().value().as_double();
        
        // Check for inf/nan if not allowed
        if (!allow_inf_nan) {
            if (std::isinf(value) || std::isnan(value)) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::FloatType),
                    Location(),
                    input.as_error_value().repr
                );
            }
        }
        
        // Skip constraint checks for inf/nan
        if (std::isinf(value) || std::isnan(value)) {
            return ValidatedValue(value);
        }
        
        // Check multiple_of constraint
        if (multiple_of) {
            double remainder = std::fmod(value, *multiple_of);
            // Allow small floating point error
            if (std::abs(remainder) > 1e-10 && std::abs(remainder - *multiple_of) > 1e-10) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::FloatMultipleOf, *multiple_of),
                    Location(),
                    input.as_error_value().repr
                );
            }
        }
        
        // Check le (less than or equal)
        if (le && value > *le) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FloatLessThanEqual, *le),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check lt (less than)
        if (lt && value >= *lt) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FloatLessThan, *lt),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check ge (greater than or equal)
        if (ge && value < *ge) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FloatGreaterThanEqual, *ge),
                Location(),
                input.as_error_value().repr
            );
        }
        
        // Check gt (greater than)
        if (gt && value <= *gt) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FloatGreaterThan, *gt),
                Location(),
                input.as_error_value().repr
            );
        }
        
        return ValidatedValue(value);
    }
    
    std::string name() const { return "constrained-float"; }
};

} // namespace pydantic_core