#pragma once

#include <variant>
#include <string>
#include <memory>
#include <optional>
#include <cstdint>
#include <utility>
#include <vector>
#include <regex>
#include <cmath>
#include <algorithm>
#include <sstream>
#include "../types.hpp"
#include "../result.hpp"
#include "../input.hpp"
#include "../validation_state.hpp"
#include "../error_types.hpp"

namespace pydantic_core {

// ============================================================================
// ValidatedValue - the validated output value
// ============================================================================
struct ValidatedValue {
    std::variant<
        std::monostate,
        bool,
        int64_t,
        uint64_t,
        double,
        std::string,
        std::vector<uint8_t>,
        std::vector<ValidatedValue>,
        std::vector<std::pair<std::string, ValidatedValue>>
    > data;
    
    ValidatedValue() : data(std::monostate{}) {}
    explicit ValidatedValue(std::monostate) : data(std::monostate{}) {}
    explicit ValidatedValue(bool b) : data(b) {}
    explicit ValidatedValue(int64_t i) : data(i) {}
    explicit ValidatedValue(uint64_t i) : data(i) {}
    explicit ValidatedValue(double f) : data(f) {}
    explicit ValidatedValue(const std::string& s) : data(s) {}
    explicit ValidatedValue(std::string_view s) : data(std::string(s)) {}
    explicit ValidatedValue(const std::vector<uint8_t>& b) : data(b) {}
    explicit ValidatedValue(const std::vector<ValidatedValue>& l) : data(l) {}
    explicit ValidatedValue(const std::vector<std::pair<std::string, ValidatedValue>>& d) : data(d) {}
    
    bool is_none() const { return std::holds_alternative<std::monostate>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_int() const { return std::holds_alternative<int64_t>(data) || std::holds_alternative<uint64_t>(data); }
    bool is_float() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_bytes() const { return std::holds_alternative<std::vector<uint8_t>>(data); }
    bool is_list() const { return std::holds_alternative<std::vector<ValidatedValue>>(data); }
    bool is_dict() const { return std::holds_alternative<std::vector<std::pair<std::string, ValidatedValue>>>(data); }
    
    std::string repr() const {
        if (is_none()) return "None";
        if (is_bool()) return std::get<bool>(data) ? "True" : "False";
        if (auto* i = std::get_if<int64_t>(&data)) return std::to_string(*i);
        if (auto* u = std::get_if<uint64_t>(&data)) return std::to_string(*u);
        if (auto* f = std::get_if<double>(&data)) return std::to_string(*f);
        if (auto* s = std::get_if<std::string>(&data)) return "\"" + *s + "\"";
        if (auto* b = std::get_if<std::vector<uint8_t>>(&data)) {
            std::string r = "b'";
            for (auto c : *b) r += static_cast<char>(c);
            r += "'";
            return r;
        }
        if (auto* l = std::get_if<std::vector<ValidatedValue>>(&data)) {
            std::string r = "[";
            for (size_t i = 0; i < l->size(); ++i) {
                if (i > 0) r += ", ";
                r += l->at(i).repr();
            }
            r += "]";
            return r;
        }
        if (auto* d = std::get_if<std::vector<std::pair<std::string, ValidatedValue>>>(&data)) {
            std::string r = "{";
            for (size_t i = 0; i < d->size(); ++i) {
                if (i > 0) r += ", ";
                r += "\"" + d->at(i).first + "\": " + d->at(i).second.repr();
            }
            r += "}";
            return r;
        }
        return "?";
    }
    
    // Equality for LiteralValidator
    bool operator==(const ValidatedValue& other) const {
        return data == other.data;
    }
};

// ============================================================================
// Forward declaration of CombinedValidator and visitor
// ============================================================================
struct ValidateVisitor;

using CombinedValidator = std::variant<
    struct NoneValidator,
    struct BoolValidator,
    struct IntValidator,
    struct ConstrainedIntValidator,
    struct FloatValidator,
    struct ConstrainedFloatValidator,
    struct StringValidator,
    struct ConstrainedStringValidator,
    struct BytesValidator,
    struct ListValidator,
    struct DictValidator,
    struct SetValidator,
    struct FrozenSetValidator,
    struct TupleValidator,
    struct LiteralValidator,
    struct NullableValidator,
    struct UnionValidator,
    struct DateValidator,
    struct TimeValidator,
    struct DateTimeValidator
>;

// ============================================================================
// Simple validators
// ============================================================================

struct NoneValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        if (input.is_none()) {
            return ValidatedValue(std::monostate{});
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::NoneType),
            Location(),
            input.as_error_value().repr
        );
    }
    std::string name() const { return "none"; }
};

struct BoolValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_bool(state.strict_or(strict));
        if (result.is_err()) return result.error();
        return ValidatedValue(result.value().value());
    }
    std::string name() const { return "bool"; }
};

struct IntValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_int(state.strict_or(strict));
        if (result.is_err()) return result.error();
        
        auto i64 = result.value().value().as_i64();
        if (i64) return ValidatedValue(*i64);
        auto u64 = result.value().value().as_u64();
        if (u64) return ValidatedValue(*u64);
        
        return ValError::line_error(ErrorType(ErrorType::Kind::IntType), Location(), input.as_error_value().repr);
    }
    std::string name() const { return "int"; }
};

struct ConstrainedIntValidator {
    bool strict = false;
    std::optional<int64_t> multiple_of;
    std::optional<int64_t> le, lt, ge, gt;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_int(state.strict_or(strict));
        if (result.is_err()) return result.error();
        
        auto i64_val = result.value().value().as_i64();
        if (!i64_val) {
            return ValError::line_error(ErrorType(ErrorType::Kind::IntType), Location(), "integer too large");
        }
        
        int64_t value = *i64_val;
        
        if (multiple_of && value % *multiple_of != 0) {
            return ValError::line_error(ErrorType(ErrorType::Kind::IntMultipleOf, *multiple_of), Location(), input.as_error_value().repr);
        }
        if (le && value > *le) {
            return ValError::line_error(ErrorType(ErrorType::Kind::IntLessThanEqual, *le), Location(), input.as_error_value().repr);
        }
        if (lt && value >= *lt) {
            return ValError::line_error(ErrorType(ErrorType::Kind::IntLessThan, *lt), Location(), input.as_error_value().repr);
        }
        if (ge && value < *ge) {
            return ValError::line_error(ErrorType(ErrorType::Kind::IntGreaterThanEqual, *ge), Location(), input.as_error_value().repr);
        }
        if (gt && value <= *gt) {
            return ValError::line_error(ErrorType(ErrorType::Kind::IntGreaterThan, *gt), Location(), input.as_error_value().repr);
        }
        
        return ValidatedValue(value);
    }
    std::string name() const { return "constrained-int"; }
};

struct FloatValidator {
    bool strict = false;
    bool allow_inf_nan = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_float(state.strict_or(strict));
        if (result.is_err()) return result.error();
        
        double value = result.value().value().as_double();
        
        if (!allow_inf_nan && (std::isinf(value) || std::isnan(value))) {
            return ValError::line_error(ErrorType(ErrorType::Kind::FloatType), Location(), input.as_error_value().repr);
        }
        
        return ValidatedValue(value);
    }
    std::string name() const { return "float"; }
};

struct ConstrainedFloatValidator {
    bool strict = false;
    bool allow_inf_nan = false;
    std::optional<double> multiple_of, le, lt, ge, gt;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_float(state.strict_or(strict));
        if (result.is_err()) return result.error();
        
        double value = result.value().value().as_double();
        
        if (!allow_inf_nan && (std::isinf(value) || std::isnan(value))) {
            return ValError::line_error(ErrorType(ErrorType::Kind::FloatType), Location(), input.as_error_value().repr);
        }
        
        if (std::isinf(value) || std::isnan(value)) return ValidatedValue(value);
        
        if (multiple_of) {
            double remainder = std::fmod(value, *multiple_of);
            if (std::abs(remainder) > 1e-10 && std::abs(remainder - *multiple_of) > 1e-10) {
                return ValError::line_error(ErrorType(ErrorType::Kind::FloatMultipleOf, *multiple_of), Location(), input.as_error_value().repr);
            }
        }
        if (le && value > *le) return ValError::line_error(ErrorType(ErrorType::Kind::FloatLessThanEqual, *le), Location(), input.as_error_value().repr);
        if (lt && value >= *lt) return ValError::line_error(ErrorType(ErrorType::Kind::FloatLessThan, *lt), Location(), input.as_error_value().repr);
        if (ge && value < *ge) return ValError::line_error(ErrorType(ErrorType::Kind::FloatGreaterThanEqual, *ge), Location(), input.as_error_value().repr);
        if (gt && value <= *gt) return ValError::line_error(ErrorType(ErrorType::Kind::FloatGreaterThan, *gt), Location(), input.as_error_value().repr);
        
        return ValidatedValue(value);
    }
    std::string name() const { return "constrained-float"; }
};

struct StringValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_str(state.strict_or(strict), false);
        if (result.is_err()) return result.error();
        return ValidatedValue(result.value().value().to_string());
    }
    std::string name() const { return "str"; }
};

struct ConstrainedStringValidator {
    bool strict = false;
    std::optional<size_t> min_length, max_length;
    std::optional<std::regex> pattern;
    bool to_lower = false, to_upper = false, to_title = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_str(state.strict_or(strict), false);
        if (result.is_err()) return result.error();
        
        std::string value = result.value().value().to_string();
        
        if (min_length && value.length() < *min_length) {
            std::string s = *min_length == 1 ? "" : "s";
            return ValError::line_error(ErrorType(ErrorType::Kind::StringTooShort, "min_length", std::to_string(*min_length), "s", s), Location(), input.as_error_value().repr);
        }
        if (max_length && value.length() > *max_length) {
            std::string s = *max_length == 1 ? "" : "s";
            return ValError::line_error(ErrorType(ErrorType::Kind::StringTooLong, "max_length", std::to_string(*max_length), "s", s), Location(), input.as_error_value().repr);
        }
        if (pattern && !std::regex_match(value, *pattern)) {
            return ValError::line_error(ErrorType(ErrorType::Kind::StringPatternMismatch), Location(), input.as_error_value().repr);
        }
        
        if (to_lower) std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        else if (to_upper) std::transform(value.begin(), value.end(), value.begin(), ::toupper);
        else if (to_title) {
            bool prev_space = true;
            for (auto& c : value) {
                if (prev_space && std::isalpha(static_cast<unsigned char>(c))) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                prev_space = std::isspace(static_cast<unsigned char>(c));
            }
        }
        
        return ValidatedValue(value);
    }
    std::string name() const { return "constrained-str"; }
};

struct BytesValidator {
    bool strict = false;
    std::optional<size_t> min_length, max_length;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_bytes(state.strict_or(strict));
        if (result.is_err()) return result.error();
        
        EitherBytes either_bytes = result.value().value();
        
        if (min_length && either_bytes.size() < *min_length) {
            return ValError::line_error(ErrorType(ErrorType::Kind::BytesTooShort, "min_length", std::to_string(*min_length)), Location(), input.as_error_value().repr);
        }
        if (max_length && either_bytes.size() > *max_length) {
            return ValError::line_error(ErrorType(ErrorType::Kind::BytesTooLong, "max_length", std::to_string(*max_length)), Location(), input.as_error_value().repr);
        }
        
        return ValidatedValue(either_bytes.to_vector());
    }
    std::string name() const { return "bytes"; }
};

// ============================================================================
// Container validators with recursive validation
// ============================================================================

struct ListValidator {
    bool strict = false;
    std::shared_ptr<CombinedValidator> item_validator;
    std::optional<size_t> min_length, max_length;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_list(state.strict_or(strict));
        if (result.is_err()) return result.error();
        
        auto validated_list = std::move(result.value().value());
        size_t actual_length = validated_list->size();
        
        // Length checks
        if (min_length && actual_length < *min_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::ListTooShort, static_cast<int64_t>(*min_length)),
                Location(), input.as_error_value().repr
            );
        }
        if (max_length && actual_length > *max_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::ListTooLong, static_cast<int64_t>(*max_length)),
                Location(), input.as_error_value().repr
            );
        }
        
        // Validate items
        std::vector<ValidatedValue> output;
        auto entries = validated_list->entries();
        
        for (size_t i = 0; i < entries.size(); ++i) {
            // Push location
            Location item_loc = state.location();
            item_loc.push(static_cast<int64_t>(i));
            
            // For now, placeholder - actual recursive validation needs Input iteration
            // This requires extending Input to provide per-item Input objects
            output.emplace_back(std::monostate{});  // Placeholder
        }
        
        return ValidatedValue(output);
    }
    std::string name() const { return "list"; }
};

struct DictValidator {
    bool strict = false;
    std::shared_ptr<CombinedValidator> keys_validator;
    std::shared_ptr<CombinedValidator> values_validator;
    std::optional<size_t> min_length, max_length;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_dict(state.strict_or(strict));
        if (result.is_err()) return result.error();
        
        auto validated_dict = std::move(result.value());
        size_t actual_length = validated_dict->size();
        
        // Length checks
        if (min_length && actual_length < *min_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DictTooShort, static_cast<int64_t>(*min_length)),
                Location(), input.as_error_value().repr
            );
        }
        if (max_length && actual_length > *max_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DictTooLong, static_cast<int64_t>(*max_length)),
                Location(), input.as_error_value().repr
            );
        }
        
        std::vector<std::pair<std::string, ValidatedValue>> output;
        return ValidatedValue(output);
    }
    std::string name() const { return "dict"; }
};

struct SetValidator {
    bool strict = false;
    std::shared_ptr<CombinedValidator> item_validator;
    std::optional<size_t> min_length, max_length;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_list(state.strict_or(strict));
        if (result.is_err()) return result.error();
        
        auto validated_list = std::move(result.value().value());
        size_t actual_length = validated_list->size();
        
        if (min_length && actual_length < *min_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::SetTooShort, static_cast<int64_t>(*min_length)),
                Location(), input.as_error_value().repr
            );
        }
        if (max_length && actual_length > *max_length) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::SetTooLong, static_cast<int64_t>(*max_length)),
                Location(), input.as_error_value().repr
            );
        }
        
        std::vector<ValidatedValue> output;
        return ValidatedValue(output);
    }
    std::string name() const { return "set"; }
};

struct FrozenSetValidator {
    bool strict = false;
    std::shared_ptr<CombinedValidator> item_validator;
    std::optional<size_t> min_length, max_length;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        SetValidator set_val{strict, item_validator, min_length, max_length};
        return set_val.validate(input, state);
    }
    std::string name() const { return "frozenset"; }
};

struct TupleValidator {
    bool strict = false;
    std::vector<std::shared_ptr<CombinedValidator>> item_validators;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_tuple(state.strict_or(strict));
        if (result.is_err()) return result.error();
        
        auto validated_tuple = std::move(result.value().value());
        size_t expected_len = item_validators.size();
        size_t actual_len = validated_tuple->size();
        
        if (actual_len != expected_len) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TupleLengthMismatch, static_cast<int64_t>(expected_len), static_cast<int64_t>(actual_len)),
                Location(), input.as_error_value().repr
            );
        }
        
        std::vector<ValidatedValue> output;
        return ValidatedValue(output);
    }
    std::string name() const { return "tuple"; }
};

struct LiteralValidator {
    std::vector<ValidatedValue> allowed_values;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& /*state*/) const {
        auto input_repr = input.as_error_value().repr;
        for (const auto& allowed : allowed_values) {
            if (allowed.repr() == input_repr) return allowed;
        }
        return ValError::line_error(ErrorType(ErrorType::Kind::LiteralMismatch), Location(), input.as_error_value().repr);
    }
    std::string name() const { return "literal"; }
};

// ============================================================================
// Nullable and Union validators
// ============================================================================

struct NullableValidator {
    std::shared_ptr<CombinedValidator> inner_validator;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const;
    std::string name() const { return "nullable"; }
};

struct UnionValidator {
    std::vector<std::shared_ptr<CombinedValidator>> validators;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const;
    std::string name() const { return "union"; }
};

// ============================================================================
// Date/Time validators
// ============================================================================

struct DateValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        // Date format: YYYY-MM-DD
        auto result = input.validate_str(state.strict_or(strict), false);
        if (result.is_err()) return result.error();
        
        std::string value = result.value().value().to_string();
        
        // Simple validation: check format with regex
        std::regex date_pattern(R"(^\d{4}-\d{2}-\d{2}$)");
        if (!std::regex_match(value, date_pattern)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateType),
                Location(), input.as_error_value().repr
            );
        }
        
        // Basic validation: check parts are valid
        int year = std::stoi(value.substr(0, 4));
        int month = std::stoi(value.substr(5, 2));
        int day = std::stoi(value.substr(8, 2));
        
        if (month < 1 || month > 12) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateType),
                Location(), input.as_error_value().repr
            );
        }
        if (day < 1 || day > 31) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateType),
                Location(), input.as_error_value().repr
            );
        }
        
        return ValidatedValue(value);
    }
    std::string name() const { return "date"; }
};

struct TimeValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_str(state.strict_or(strict), false);
        if (result.is_err()) return result.error();
        
        std::string value = result.value().value().to_string();
        
        // Time formats: HH:MM:SS or HH:MM:SS.microseconds
        std::regex time_pattern(R"(\d{2}:\d{2}:\d{2}(\.\d+)?)");
        if (!std::regex_match(value, time_pattern)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TimeType),
                Location(), input.as_error_value().repr
            );
        }
        
        return ValidatedValue(value);
    }
    std::string name() const { return "time"; }
};

struct DateTimeValidator {
    bool strict = false;
    
    ValResult<ValidatedValue> validate(Input& input, ValidationState& state) const {
        auto result = input.validate_str(state.strict_or(strict), false);
        if (result.is_err()) return result.error();
        
        std::string value = result.value().value().to_string();
        
        // DateTime format: YYYY-MM-DDTHH:MM:SS or similar
        std::regex datetime_pattern(R"(\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:?\d{2})?)");
        if (!std::regex_match(value, datetime_pattern)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateTimeType),
                Location(), input.as_error_value().repr
            );
        }
        
        return ValidatedValue(value);
    }
    std::string name() const { return "datetime"; }
};

// ============================================================================
// Visitor and helper functions
// ============================================================================

struct ValidateVisitor {
    Input& input;
    ValidationState& state;
    
    template<typename T>
    ValResult<ValidatedValue> operator()(const T& validator) {
        return validator.validate(input, state);
    }
};

inline ValResult<ValidatedValue> validate_combined(
    const CombinedValidator& validator,
    Input& input,
    ValidationState& state
) {
    return std::visit(ValidateVisitor{input, state}, validator);
}

// Inline implementations for NullableValidator and UnionValidator
inline ValResult<ValidatedValue> NullableValidator::validate(Input& input, ValidationState& state) const {
    if (input.is_none()) return ValidatedValue(std::monostate{});
    if (inner_validator) {
        return std::visit(ValidateVisitor{input, state}, *inner_validator);
    }
    return ValidatedValue(std::monostate{});  // No inner validator - accept any non-None
}

inline ValResult<ValidatedValue> UnionValidator::validate(Input& input, ValidationState& state) const {
    std::vector<ValError> errors;
    for (const auto& validator : validators) {
        auto result = std::visit(ValidateVisitor{input, state}, *validator);
        if (result.is_ok()) return result;
        errors.push_back(result.error());
    }
    if (!errors.empty()) return errors[0];
    return ValError::line_error(ErrorType(ErrorType::Kind::UnionType), Location(), input.as_error_value().repr);
}

} // namespace pydantic_core