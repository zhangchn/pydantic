#pragma once

#include "pydantic_core/validator.hpp"
#include <memory>
#include <functional>

namespace pydantic_core {

// FunctionBeforeValidator - runs function before validation
class FunctionBeforeValidator : public Validator {
public:
    FunctionBeforeValidator() : inner_(nullptr) {}
    explicit FunctionBeforeValidator(std::shared_ptr<Validator> inner)
        : inner_(std::move(inner)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // In Phase 2, we'll call the Python function here
        // For now, just pass through to inner validator
        if (inner_) {
            return inner_->validate(input, state);
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "function-before"; }
    
private:
    std::shared_ptr<Validator> inner_;
};

// FunctionAfterValidator - runs function after validation
class FunctionAfterValidator : public Validator {
public:
    FunctionAfterValidator() : inner_(nullptr) {}
    explicit FunctionAfterValidator(std::shared_ptr<Validator> inner)
        : inner_(std::move(inner)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // First validate with inner
        auto result = inner_->validate(input, state);
        if (result.is_err()) {
            return result;
        }
        // In Phase 2, we'll call the Python function here
        return result;
    }
    
    std::string name() const override { return "function-after"; }
    
private:
    std::shared_ptr<Validator> inner_;
};

// FunctionPlainValidator - plain function that replaces validation
class FunctionPlainValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // In Phase 2, we'll call the Python function here
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "function-plain"; }
};

// FunctionWrapValidator - wraps validation with custom logic
class FunctionWrapValidator : public Validator {
public:
    FunctionWrapValidator() : inner_(nullptr) {}
    explicit FunctionWrapValidator(std::shared_ptr<Validator> inner)
        : inner_(std::move(inner)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // In Phase 2, we'll call the Python function with handler
        if (inner_) {
            return inner_->validate(input, state);
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "function-wrap"; }
    
private:
    std::shared_ptr<Validator> inner_;
};

// WithDefaultValidator - provides default value if input is missing
class WithDefaultValidator : public Validator {
public:
    WithDefaultValidator() : inner_(nullptr), default_value_(nullptr) {}
    WithDefaultValidator(std::shared_ptr<Validator> inner, std::shared_ptr<void> default_value)
        : inner_(std::move(inner)), default_value_(std::move(default_value)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (input.is_none() && default_value_) {
            return ValResult<std::shared_ptr<void>>(default_value_);
        }
        if (inner_) {
            return inner_->validate(input, state);
        }
        return ValResult<std::shared_ptr<void>>(default_value_);
    }
    
    ValResult<std::shared_ptr<void>> default_value(
        ValidationState& state
    ) override {
        if (default_value_) {
            return ValResult<std::shared_ptr<void>>(default_value_);
        }
        if (inner_) {
            return inner_->default_value(state);
        }
        return ValError::omit();
    }
    
    std::string name() const override { return "with-default"; }
    
private:
    std::shared_ptr<Validator> inner_;
    std::shared_ptr<void> default_value_;
};

// ChainValidator - runs validators in sequence until one succeeds
class ChainValidator : public Validator {
public:
    ChainValidator() = default;
    explicit ChainValidator(std::vector<std::shared_ptr<Validator>> validators)
        : validators_(std::move(validators)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (validators_.empty()) {
            return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
        }
        for (auto& validator : validators_) {
            auto result = validator->validate(input, state);
            if (result.is_ok()) {
                return result;
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "No chain validator succeeded"
        );
    }
    
    std::string name() const override { return "chain"; }
    
private:
    std::vector<std::shared_ptr<Validator>> validators_;
};

// LaxOrStrictValidator - uses different validators for lax/strict mode
class LaxOrStrictValidator : public Validator {
public:
    LaxOrStrictValidator() : lax_(nullptr), strict_(nullptr) {}
    LaxOrStrictValidator(std::shared_ptr<Validator> lax, std::shared_ptr<Validator> strict)
        : lax_(std::move(lax)), strict_(std::move(strict)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (state.strict_or(false)) {
            return strict_->validate(input, state);
        }
        return lax_->validate(input, state);
    }
    
    std::string name() const override { return "lax-or-strict"; }
    
private:
    std::shared_ptr<Validator> lax_;
    std::shared_ptr<Validator> strict_;
};

// JsonOrPythonValidator - uses different validators for JSON/Python input
class JsonOrPythonValidator : public Validator {
public:
    JsonOrPythonValidator() : json_(nullptr), python_(nullptr) {}
    JsonOrPythonValidator(std::shared_ptr<Validator> json, std::shared_ptr<Validator> python)
        : json_(std::move(json)), python_(std::move(python)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (input.input_type() == InputType::Json) {
            return json_->validate(input, state);
        }
        return python_->validate(input, state);
    }
    
    std::string name() const override { return "json-or-python"; }
    
private:
    std::shared_ptr<Validator> json_;
    std::shared_ptr<Validator> python_;
};

// JsonValidator - validates JSON input directly
class JsonValidator : public Validator {
public:
    JsonValidator() = default;
    explicit JsonValidator(std::shared_ptr<Validator> inner) : inner_(std::move(inner)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // If no inner validator, accept any JSON
        if (!inner_) {
            return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(input.as_error_value().repr));
        }
        // TODO: Parse JSON string and validate with inner validator
        return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(input.as_error_value().repr));
    }
    
    std::string name() const override { return "json"; }
    
private:
    std::shared_ptr<Validator> inner_;
};

} // namespace pydantic_core