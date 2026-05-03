#pragma once

#include "pydantic_core/validator.hpp"
#include <memory>

namespace pydantic_core {

// ListValidator - validates list/array values
class ListValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_list(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "list"; }
};

// DictValidator - validates dict/object values
class DictValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_dict(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "dict"; }
};

// SetValidator - validates set values
class SetValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_list(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "set"; }
};

// FrozenSetValidator - validates frozenset values
class FrozenSetValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_list(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "frozenset"; }
};

// TupleValidator - validates tuple values
class TupleValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_tuple(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "tuple"; }
};

} // namespace pydantic_core