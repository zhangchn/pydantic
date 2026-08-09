#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/validation_state.hpp"
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/json_input.hpp"
#include <memory>
#include <functional>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace pydantic_core {

// Defined in schema_validator.cpp; converts a validated result to a Python
// object by type name (used here to pass the validated value to after-functions).
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name);

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
            if (!state.context_py().is_none()) {
                info_dict["context"] = state.context_py();
            }

            py::object transformed = py_func_(input.as_python_object(), info_dict);
            
            if (inner_) {
                auto py_input = std::make_unique<PythonInput>(transformed);
                return inner_->validate(*py_input, state);
            }
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(transformed));
        } catch (py::error_already_set& e) {
            // Extract the message, then swallow the error: restore() releases
            // the fetched refs so the destructor's restore is a no-op and the
            // error indicator stays clear (avoids a stale-indicator double
            // fetch that corrupts pybind11's error state).
            std::string msg = e.what();
            e.restore();
            PyErr_Clear();
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "FunctionBefore validator failed: " + msg
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

        // Convert the validated inner result to a Python object — Rust passes
        // the validated value to the after-function (model validators receive
        // the validated fields dict, field validators the coerced value).
        py::object validated_obj;
        try {
            validated_obj = value_to_python_with_type(result.value(), inner_->effective_result_name());
        } catch (...) {
            // Unknown inner result type: pass through the raw input
            validated_obj = input.as_python_object();
        }

        try {
            py::dict info_dict;
            if (state.field_name().has_value()) {
                info_dict["field_name"] = py::str(*state.field_name());
            }
            info_dict["strict"] = state.strict_or(false);
            if (!state.context_py().is_none()) {
                info_dict["context"] = state.context_py();
            }

            py::object output;
            try {
                // Convert info_dict to an object with attribute access (like ValidationInfo)
                // pydantic's field_validator accesses info.context, info.field_name via attributes
                // Missing attributes should return None (not raise AttributeError)
                py::object info_obj;
                try {
                    // Create a dict subclass that returns None for missing attribute access
                    py::object info_cls = py::module_::import("pydantic_core_cpp").attr("_ValidationInfo");
                    info_obj = info_cls(info_dict);
                } catch (...) {
                    info_obj = info_dict;
                }
                // Try with info object (general/no-info-wrapped functions)
                output = py_func_(validated_obj, info_obj);
            } catch (py::error_already_set& e1) {
                // If fails with info dict, try without info dict (no-info
                // functions like attrgetter).  restore() + PyErr_Clear()
                // swallows the error so the destructor's restore is a no-op.
                e1.restore();
                PyErr_Clear();
                try {
                    output = py_func_(validated_obj);
                } catch (py::error_already_set& e2) {
                    // If the function fails (e.g. attrgetter('value') on a plain string),
                    // use the validated inner result as the output
                    e2.restore();
                    PyErr_Clear();
                    return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(validated_obj));
                }
            }
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(output));
        } catch (py::error_already_set& e) {
            std::string msg = e.what();
            e.restore();
            PyErr_Clear();
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "FunctionAfter validator failed: " + msg
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
            if (!state.context_py().is_none()) {
                info_dict["context"] = state.context_py();
            }

            py::object output;
            try {
                // Try with info dict (general/no-info-wrapped functions)
                output = py_func_(input.as_python_object(), info_dict);
            } catch (py::error_already_set& e1) {
                // If fails, try without info dict (no-info functions like class constructors)
                e1.restore();
                PyErr_Clear();
                try {
                    output = py_func_(input.as_python_object());
                } catch (py::error_already_set& e2) {
                    std::string msg = e2.what();
                    e2.restore();
                    PyErr_Clear();
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::CustomError),
                        state.location(),
                        "FunctionPlain validator failed: " + msg
                    );
                }
            }
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(output));
        } catch (py::error_already_set& e) {
            std::string msg = e.what();
            e.restore();
            PyErr_Clear();
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "FunctionPlain validator failed: " + msg
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
                    // Extract validated result - try py::object first
                    auto* obj = static_cast<py::object*>(result.value().get());
                    if (obj) return *obj;
                }
                return v;
            });
            
            py::object output = py_func_(input.as_python_object(), handler, info_dict);
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(output));
        } catch (py::error_already_set& e) {
            std::string msg = e.what();
            e.restore();
            PyErr_Clear();
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "FunctionWrap validator failed: " + msg
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
    WithDefaultValidator(std::shared_ptr<Validator> inner, std::shared_ptr<void> default_value, std::string default_value_str = "")
        : inner_(std::move(inner)), default_value_(std::move(default_value)), default_value_str_(std::move(default_value_str)) {}

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
        if (!default_value_str_.empty()) {
            // Parse complex default and validate through inner
            auto parse_result = parse_json(default_value_str_);
            if (parse_result.is_ok() && inner_) {
                auto default_result = inner_->validate(*parse_result.value(), state);
                if (default_result.is_ok()) {
                    return default_result;
                }
            }
        }
        if (inner_) return inner_->default_value(state);
        return ValError::omit();
    }

    std::string name() const override {
        // Delegate to the inner validator so result conversion uses the
        // actual stored value type instead of the "with-default" wrapper name.
        if (inner_) return inner_->name();
        return "with-default";
    }

private:
    std::shared_ptr<Validator> inner_;
    std::shared_ptr<void> default_value_;
    std::string default_value_str_;
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