#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/validation_state.hpp"
#include "pydantic_core/python_input.hpp"
#include <memory>
#include <functional>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace pydantic_core {

// FunctionBeforeValidator - runs Python function before validation
// Python signature: func(input, info) -> transformed_input
class FunctionBeforeValidator : public Validator {
public:
    FunctionBeforeValidator() : inner_(nullptr), py_func_(py::none()) {}
    FunctionBeforeValidator(std::shared_ptr<Validator> inner, py::object py_func)
        : inner_(std::move(inner)), py_func_(std::move(py_func)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (py_func_.is_none()) {
            if (inner_) return inner_->validate(input, state);
            return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
        }
        
        try {
            py::dict info_dict;
            if (state.field_name().has_value()) {
                info_dict["field_name"] = py::str(*state.field_name());
            }
            info_dict["strict"] = state.strict_or(false);
            if (state.context()) {
                info_dict["context"] = py::cast(state.context());
            }
            
            py::object transformed = py_func_(input.as_python_object(), info_dict);
            
            if (inner_) {
                auto py_input = std::make_unique<PythonInput>(transformed);
                return inner_->validate(*py_input, state);
            }
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(transformed));
        } catch (py::error_already_set& e) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "FunctionBefore validator failed: " + std::string(e.what())
            );
        }
    }

    std::string name() const override { return "function-before"; }
    void set_py_func(py::object func) { py_func_ = std::move(func); }

private:
    std::shared_ptr<Validator> inner_;
    py::object py_func_;
};

// FunctionAfterValidator - runs Python function after validation
class FunctionAfterValidator : public Validator {
public:
    FunctionAfterValidator() : inner_(nullptr), py_func_(py::none()) {}
    FunctionAfterValidator(std::shared_ptr<Validator> inner, py::object py_func)
        : inner_(std::move(inner)), py_func_(std::move(py_func)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (!inner_) {
            return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
        }
        auto result = inner_->validate(input, state);
        if (result.is_err()) return result;
        
        if (py_func_.is_none()) return result;
        
        try {
            py::dict info_dict;
            if (state.field_name().has_value()) {
                info_dict["field_name"] = py::str(*state.field_name());
            }
            info_dict["strict"] = state.strict_or(false);
            if (state.context()) {
                info_dict["context"] = py::cast(state.context());
            }
            
            py::object output = py_func_(input.as_python_object(), info_dict);
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(output));
        } catch (py::error_already_set& e) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "FunctionAfter validator failed: " + std::string(e.what())
            );
        }
    }

    std::string name() const override { return "function-after"; }
    void set_py_func(py::object func) { py_func_ = std::move(func); }

private:
    std::shared_ptr<Validator> inner_;
    py::object py_func_;
};

// FunctionPlainValidator - plain Python function that replaces validation
class FunctionPlainValidator : public Validator {
public:
    FunctionPlainValidator() : py_func_(py::none()) {}
    explicit FunctionPlainValidator(py::object py_func) : py_func_(std::move(py_func)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (py_func_.is_none()) {
            return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
        }
        
        try {
            py::dict info_dict;
            if (state.field_name().has_value()) {
                info_dict["field_name"] = py::str(*state.field_name());
            }
            info_dict["strict"] = state.strict_or(false);
            if (state.context()) {
                info_dict["context"] = py::cast(state.context());
            }
            
            py::object output = py_func_(input.as_python_object(), info_dict);
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(output));
        } catch (py::error_already_set& e) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "FunctionPlain validator failed: " + std::string(e.what())
            );
        }
    }

    std::string name() const override { return "function-plain"; }
    void set_py_func(py::object func) { py_func_ = std::move(func); }

private:
    py::object py_func_;
};

// FunctionWrapValidator - wraps validation with custom Python logic
class FunctionWrapValidator : public Validator {
public:
    FunctionWrapValidator() : inner_(nullptr), py_func_(py::none()) {}
    FunctionWrapValidator(std::shared_ptr<Validator> inner, py::object py_func)
        : inner_(std::move(inner)), py_func_(std::move(py_func)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (py_func_.is_none()) {
            if (inner_) return inner_->validate(input, state);
            return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
        }
        
        try {
            py::dict info_dict;
            if (state.field_name().has_value()) {
                info_dict["field_name"] = py::str(*state.field_name());
            }
            info_dict["strict"] = state.strict_or(false);
            if (state.context()) {
                info_dict["context"] = py::cast(state.context());
            }
            
            py::object handler = py::cpp_function([this, &state](py::object v) -> py::object {
                if (inner_) {
                    auto py_input = std::make_unique<PythonInput>(v);
                    auto result = inner_->validate(*py_input, state);
                    if (result.is_err()) {
                        throw py::value_error("Inner validator failed");
                    }
                }
                return v;
            });
            
            py::object output = py_func_(input.as_python_object(), handler, info_dict);
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(output));
        } catch (py::error_already_set& e) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "FunctionWrap validator failed: " + std::string(e.what())
            );
        }
    }

    std::string name() const override { return "function-wrap"; }
    void set_py_func(py::object func) { py_func_ = std::move(func); }

private:
    std::shared_ptr<Validator> inner_;
    py::object py_func_;
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
        if (inner_) return inner_->validate(input, state);
        return ValResult<std::shared_ptr<void>>(default_value_);
    }

    ValResult<std::shared_ptr<void>> default_value(ValidationState& state) override {
        if (default_value_) return ValResult<std::shared_ptr<void>>(default_value_);
        if (inner_) return inner_->default_value(state);
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
        for (auto& v : validators_) {
            auto result = v->validate(input, state);
            if (result.is_ok()) return result;
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
        if (state.strict_or(false)) return strict_->validate(input, state);
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
        if (input.input_type() == InputType::Json) return json_->validate(input, state);
        return python_->validate(input, state);
    }

    std::string name() const override { return "json-or-python"; }

private:
    std::shared_ptr<Validator> json_;
    std::shared_ptr<Validator> python_;
};

// JsonValidator - validates JSON input by converting to Python object
class JsonValidator : public Validator {
public:
    JsonValidator() = default;
    explicit JsonValidator(std::shared_ptr<Validator> inner) : inner_(std::move(inner)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Convert JSON to Python object (handled by as_python_object)
        py::object parsed = input.as_python_object();
        
        if (inner_) {
            auto py_input = std::make_unique<PythonInput>(parsed);
            return inner_->validate(*py_input, state);
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(parsed));
    }

    std::string name() const override { return "json"; }

private:
    std::shared_ptr<Validator> inner_;
};

} // namespace pydantic_core