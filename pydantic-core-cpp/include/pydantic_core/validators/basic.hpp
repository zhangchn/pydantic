#pragma once

#include "pydantic_core/validator.hpp"
#include <memory>

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
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
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
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "int"; }
};

// FloatValidator - validates float values
class FloatValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_float(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "float"; }
};

// StringValidator - validates string values
class StringValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_str(state.strict_or(false), false);
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "str"; }
};

// BytesValidator - validates bytes values
class BytesValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_bytes(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "bytes"; }
};

} // namespace pydantic_core