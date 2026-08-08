#pragma once

#include "pydantic_core/validator.hpp"
#include <memory>
#include <vector>
#include <string>

namespace pydantic_core {

// NullableValidator - wraps another validator and allows None
class NullableValidator : public Validator {
public:
    NullableValidator() : inner_(nullptr) {}
    explicit NullableValidator(std::shared_ptr<Validator> inner)
        : inner_(std::move(inner)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (input.is_none()) {
            return ValResult<std::shared_ptr<void>>(nullptr);
        }
        return inner_->validate(input, state);
    }
    
    std::string name() const override {
        // Delegate to the inner validator so result conversion uses the
        // actual stored value type instead of the "nullable" wrapper name.
        if (inner_) return inner_->name();
        return "nullable";
    }
    
private:
    std::shared_ptr<Validator> inner_;
};

// UnionValidator - tries multiple validators in order
class UnionValidator : public Validator {
public:
    UnionValidator() = default;
    explicit UnionValidator(std::vector<std::shared_ptr<Validator>> validators)
        : validators_(std::move(validators)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // If the input is a Python model instance, prefer the branch whose
        // expected class matches (smart union exact match).  The instance is
        // reused as-is (it is already valid), matching Rust's model validator
        // behavior.  Fall back to left-to-right validation otherwise.
        if (input.input_type() == InputType::Python) {
            const auto& py_input = static_cast<const PythonInput&>(input);
            for (auto& v : validators_) {
                const py::object& cls = v->expected_class();
                if (!cls.is_none() && py::isinstance(py_input.py_object(), cls)) {
                    last_type_name_ = "py_object";
                    return ValResult<std::shared_ptr<void>>(
                        std::make_shared<py::object>(py_input.py_object()));
                }
            }
        }
        for (auto& validator : validators_) {
            auto result = validator->validate(input, state);
            if (result.is_ok()) {
                // Record which inner validator matched so result conversion
                // can dispatch on the real value type (avoiding unsafe
                // blind casts of the type-erased shared_ptr<void>).
                last_type_name_ = validator->name();
                return result;
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "No union variant matched"
        );
    }

    std::string name() const override {
        // After validation, report the inner validator that matched so result
        // conversion dispatches on the real value type instead of falling
        // into the unsafe try_all casts for "union".
        return last_type_name_.empty() ? "union" : last_type_name_;
    }

private:
    std::vector<std::shared_ptr<Validator>> validators_;
    mutable std::string last_type_name_;
};

// TaggedUnionValidator - union with discriminator tag
class TaggedUnionValidator : public Validator {
public:
    TaggedUnionValidator() = default;
    TaggedUnionValidator(std::string tag, std::vector<std::shared_ptr<Validator>> validators)
        : tag_(std::move(tag)), validators_(std::move(validators)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // For now, just try all validators (Phase 2 will implement proper tag matching)
        for (auto& validator : validators_) {
            auto result = validator->validate(input, state);
            if (result.is_ok()) {
                return result;
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "No tagged union variant matched"
        );
    }
    
    std::string name() const override { return "tagged-union"; }
    
private:
    std::string tag_;
    std::vector<std::shared_ptr<Validator>> validators_;
};

} // namespace pydantic_core