#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/python_input.hpp"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace pydantic_core {

// Forward declaration
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name);

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

    // The result comes straight from the inner validator
    std::string effective_result_name() const override {
        if (inner_) return inner_->effective_result_name();
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
                last_type_name_ = validator->effective_result_name();
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

    std::string effective_result_name() const override {
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
    // Callable discriminator constructor: maps tag string -> validator
    TaggedUnionValidator(py::object discriminator,
                         std::unordered_map<std::string, std::shared_ptr<Validator>> choice_map)
        : discriminator_(std::move(discriminator)), choice_map_(std::move(choice_map)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // If we have a callable discriminator and choice map, use them
        if (!discriminator_.is_none() && !choice_map_.empty()) {
            py::object tag_value;
            try {
                // Get the raw Python input for the discriminator call
                auto* py_input = dynamic_cast<const PythonInput*>(&input);
                if (!py_input) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::CustomError),
                        state.location(),
                        "tagged-union: callable discriminator requires Python input"
                    );
                }
                tag_value = discriminator_(py_input->py_object());
            } catch (py::error_already_set& e) {
                e.restore();
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::CustomError),
                    state.location(),
                    "tagged-union: discriminator call failed"
                );
            } catch (...) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::CustomError),
                    state.location(),
                    "tagged-union: discriminator call failed"
                );
            }
            std::string tag_str;
            try { tag_str = tag_value.cast<std::string>(); } catch (...) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::CustomError),
                    state.location(),
                    "tagged-union: discriminator did not return a string"
                );
            }
            auto it = choice_map_.find(tag_str);
            if (it == choice_map_.end()) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::InvalidJsonValue),
                    state.location(),
                    input.as_error_value().repr
                );
            }
            auto result = it->second->validate(input, state);
            if (result.is_ok()) {
                auto py_obj = value_to_python_with_type(result.value(), it->second->effective_result_name());
                return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(py_obj)));
            }
            return result;
        }
        // Fallback: try all validators
        for (auto& validator : validators_) {
            auto result = validator->validate(input, state);
            if (result.is_ok()) {
                auto py_obj = value_to_python_with_type(result.value(), validator->effective_result_name());
                return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(py_obj)));
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "No tagged union variant matched"
        );
    }

    std::string name() const override { return "tagged-union"; }

    // The stored value is always a py::object (converted at validate time).
    std::string effective_result_name() const override { return "py_object"; }

private:
    std::string tag_;
    std::vector<std::shared_ptr<Validator>> validators_;
    py::object discriminator_ = py::none();
    std::unordered_map<std::string, std::shared_ptr<Validator>> choice_map_;
};

} // namespace pydantic_core