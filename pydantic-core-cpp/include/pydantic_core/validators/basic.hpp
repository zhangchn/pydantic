#pragma once

#include "pydantic_core/validator.hpp"
#include <memory>
#include <optional>
#include <cmath>
#include <regex>
#include <algorithm>
#include <cctype>

namespace pydantic_core {

// AnyValidator - accepts any value
class AnyValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Accept any input - return a marker value
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "any"; }
};

// NoneValidator - only accepts None/null
class NoneValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (input.is_none()) {
            return ValResult<std::shared_ptr<void>>(nullptr);
        }
        return ValError::line_error(
            PydanticKnownError::none_required(),
            state.location(),
            input.as_error_value().repr
        );
    }
    
    std::string name() const override { return "none"; }
};

// BoolValidator - validates boolean values
class BoolValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_bool(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<bool>(result.value().value())
        );
    }
    
    std::string name() const override { return "bool"; }
};

// IntValidator - validates integer values
class IntValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_int(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        auto& either_int = result.value().value();
        // Return the actual validated integer
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<int64_t>(either_int.as_i64().value_or(0))
        );
    }
    
    std::string name() const override { return "int"; }
};

// ConstrainedIntValidator - validates int with gt/lt/ge/le/multiple_of constraints
class ConstrainedIntValidator : public Validator {
public:
    bool strict = false;
    std::optional<int64_t> gt;
    std::optional<int64_t> ge;
    std::optional<int64_t> lt;
    std::optional<int64_t> le;
    std::optional<int64_t> multiple_of;

    ConstrainedIntValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_int(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& either_int = result.value().value();
        int64_t int_value = either_int.as_i64().value_or(0);

        if (gt.has_value() && int_value <= gt.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::GreaterThan, gt.value()),
                state.location(),
                std::to_string(int_value)
            );
        }
        if (ge.has_value() && int_value < ge.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::GreaterThanEqual, ge.value()),
                state.location(),
                std::to_string(int_value)
            );
        }
        if (lt.has_value() && int_value >= lt.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::LessThan, lt.value()),
                state.location(),
                std::to_string(int_value)
            );
        }
        if (le.has_value() && int_value > le.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::LessThanEqual, le.value()),
                state.location(),
                std::to_string(int_value)
            );
        }
        if (multiple_of.has_value() && multiple_of.value() != 0) {
            if (int_value % multiple_of.value() != 0) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::MultipleOf, multiple_of.value()),
                    state.location(),
                    std::to_string(int_value)
                );
            }
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<int64_t>(int_value));
    }
    
    std::string name() const override { return "constrained-int"; }
};

// FloatValidator - validates float values
class FloatValidator : public Validator {
public:
    bool strict = false;
    bool allow_inf_nan = true;

    FloatValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_float(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& either_float = result.value().value();
        double f = either_float.as_double();
        if (!allow_inf_nan && !std::isfinite(f)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FiniteNumber),
                state.location(),
                std::to_string(f)
            );
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<double>(f));
    }
    
    std::string name() const override { return "float"; }
};

// ConstrainedFloatValidator - validates float with gt/lt/ge/le/multiple_of constraints
class ConstrainedFloatValidator : public Validator {
public:
    bool strict = false;
    bool allow_inf_nan = true;
    std::optional<double> gt;
    std::optional<double> ge;
    std::optional<double> lt;
    std::optional<double> le;
    std::optional<double> multiple_of;

    ConstrainedFloatValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_float(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& either_float = result.value().value();
        double f = either_float.as_double();

        if (!allow_inf_nan && !std::isfinite(f)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FiniteNumber),
                state.location(),
                std::to_string(f)
            );
        }
        if (gt.has_value() && f <= gt.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::GreaterThan, static_cast<int64_t>(gt.value())),
                state.location(),
                std::to_string(f)
            );
        }
        if (ge.has_value() && f < ge.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::GreaterThanEqual, static_cast<int64_t>(ge.value())),
                state.location(),
                std::to_string(f)
            );
        }
        if (lt.has_value() && f >= lt.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::LessThan, static_cast<int64_t>(lt.value())),
                state.location(),
                std::to_string(f)
            );
        }
        if (le.has_value() && f > le.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::LessThanEqual, static_cast<int64_t>(le.value())),
                state.location(),
                std::to_string(f)
            );
        }
        if (multiple_of.has_value() && multiple_of.value() != 0.0) {
            double tolerance = 1e-9;
            double rounded_div = std::round(f / multiple_of.value());
            double diff = std::abs(f - (rounded_div * multiple_of.value()));
            if (diff > tolerance) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::MultipleOf, static_cast<int64_t>(multiple_of.value())),
                    state.location(),
                    std::to_string(f)
                );
            }
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<double>(f));
    }
    
    std::string name() const override { return "constrained-float"; }
};

// StringValidator - validates string values
class StringValidator : public Validator {
public:
    bool strict = false;
    bool coerce_numbers_to_str = false;

    StringValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_str(state.strict_or(strict), coerce_numbers_to_str);
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<std::string>(result.value().value().to_string())
        );
    }
    
    std::string name() const override { return "str"; }
};

// StrConstrainedValidator - validates string with min_length/max_length/pattern/strip_whitespace/to_lower/to_upper
class StrConstrainedValidator : public Validator {
public:
    bool strict = false;
    bool coerce_numbers_to_str = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::string pattern;
    bool strip_whitespace = false;
    bool to_lower = false;
    bool to_upper = false;
    bool ascii_only = false;

    StrConstrainedValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_str(state.strict_or(strict), coerce_numbers_to_str);
        if (result.is_err()) {
            return result.error();
        }
        std::string str = result.value().value().to_string();

        if (strip_whitespace) {
            // Trim leading/trailing whitespace
            size_t start = str.find_first_not_of(" \t\n\r\f\v");
            if (start == std::string::npos) { str.clear(); }
            else {
                size_t end = str.find_last_not_of(" \t\n\r\f\v");
                str = str.substr(start, end - start + 1);
            }
        }

        // Length check using char count (Unicode-aware approximation)
        size_t char_count = str.size();  // UTF-8 byte count as proxy
        if (min_length.has_value() && char_count < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::StringTooShort, static_cast<int64_t>(min_length.value())),
                state.location(),
                str
            );
        }
        if (max_length.has_value() && char_count > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::StringTooLong, static_cast<int64_t>(max_length.value())),
                state.location(),
                str
            );
        }

        if (ascii_only) {
            for (char c : str) {
                if (static_cast<unsigned char>(c) > 127) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::StringNotAscii),
                        state.location(),
                        str
                    );
                }
            }
        }

        // Apply transformations
        if (to_lower) {
            std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c){ return std::tolower(c); });
        }
        if (to_upper) {
            std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c){ return std::toupper(c); });
        }

        // Pattern matching (basic regex via C++ std::regex)
        if (!pattern.empty()) {
            try {
                std::regex re(pattern);
                if (!std::regex_search(str, re)) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::StringPatternMismatch),
                        state.location(),
                        str
                    );
                }
            } catch (const std::regex_error&) {
                // Invalid regex, skip pattern check
            }
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(str));
    }
    
    std::string name() const override { return "constrained-str"; }
};

// BytesValidator - validates bytes values
class BytesValidator : public Validator {
public:
    bool strict = false;

    BytesValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_bytes(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherBytes>(result.value().value())
        );
    }
    
    std::string name() const override { return "bytes"; }
};

// BytesConstrainedValidator - validates bytes with min_length/max_length
class BytesConstrainedValidator : public Validator {
public:
    bool strict = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;

    BytesConstrainedValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_bytes(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& bytes = result.value().value();
        size_t len = bytes.size();

        if (min_length.has_value() && len < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::BytesTooShort, static_cast<int64_t>(min_length.value())),
                state.location(),
                bytes_repr(bytes)
            );
        }
        if (max_length.has_value() && len > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::BytesTooLong, static_cast<int64_t>(max_length.value())),
                state.location(),
                bytes_repr(bytes)
            );
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<EitherBytes>(bytes));
    }
    
    std::string name() const override { return "constrained-bytes"; }

private:
    static std::string bytes_repr(const EitherBytes& b) {
        auto v = b.to_vector();
        return "<bytes len=" + std::to_string(v.size()) + ">";
    }
};

} // namespace pydantic_core