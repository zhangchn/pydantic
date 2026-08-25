#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/validation_state.hpp"
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/validators/model_fields.hpp"
#include <memory>
#include <functional>
#include <optional>
#include <unordered_set>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace pydantic_core {

// Defined in schema_validator.cpp; converts a validated result to a Python
// object by type name (used here to pass the validated value to after-functions).
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name);

// Convert a Python exception raised by a validator function into a ValError,
// mirroring Rust's convert_err: ValueError -> value_error, AssertionError ->
// assertion_error, anything else -> custom_error carrying the message. The
// exception object is attached to the line error so the Python wrapper can
// surface it as ctx['error'].
inline ValError function_error_from_exception(py::error_already_set& e, const Input& input, ValidationState& state) {
    // Keep a reference to the exception object for ctx['error'] — value()
    // returns a new reference, so it stays valid after e.restore().
    py::object exc_value = e.value();
    std::string exc_str;
    try {
        exc_str = py::str(exc_value).cast<std::string>();
    } catch (...) {
        exc_str = "";
    }
    ErrorType::Kind kind;
    if (e.matches(PyExc_ValueError)) {
        kind = ErrorType::Kind::ValueError;
    } else if (e.matches(PyExc_AssertionError)) {
        kind = ErrorType::Kind::AssertionError;
    } else {
        std::string msg = e.what();
        e.restore();
        PyErr_Clear();
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "Function validator failed: " + msg
        );
    }
    ErrorType et(kind);
    et.context()["error"] = exc_str;
    auto err = ValError::line_error(et, state.location(), input.as_error_value().repr);
    err.line_errors()[0]->raw_error_obj = exc_value;
    // Swallow the exception: restore() + PyErr_Clear() leaves the Python error
    // indicator clear so the destructor's restore is a no-op.
    e.restore();
    PyErr_Clear();
    return err;
}

// Build a ValidationInfo object for Python callable validators, mirroring
// Rust's ValidationInfo::new: exposes field_name, the effective strict flag,
// context, and the accumulated validated-field data dict (state.data) so
// V1-style validators can read previously-validated values.
inline py::object make_validation_info(ValidationState& state) {
    py::dict info_dict;
    if (state.field_name().has_value()) {
        info_dict["field_name"] = py::str(*state.field_name());
    }
    info_dict["strict"] = state.strict_or(false);
    if (!state.context_py().is_none()) {
        info_dict["context"] = state.context_py();
    }
    py::object data = state.data();
    if (!data.is_none()) {
        info_dict["data"] = data;
    }
    try {
        // Convert info_dict to an object with attribute access (like
        // ValidationInfo); missing attributes return None.
        py::object info_cls = py::module_::import("pydantic_core_cpp").attr("_ValidationInfo");
        return info_cls(info_dict);
    } catch (...) {
        return py::object(info_dict);
    }
}

// Returns true when this validator chain terminates in a model-fields or
// typed-dict validator through any number of function wrappers (i.e. a V1
// post root-validator position where after-functions exchange fields
// semantics: 3-tuple in, flattened dict out).
inline bool reaches_fields_result(const std::shared_ptr<Validator>& v) {
    if (!v) return false;
    std::string n = v->name();
    if (n == "model-fields" || n.rfind("typed-dict", 0) == 0) return true;
    if (n == "function-after" || n == "function-before" ||
        n == "function-wrap" || n == "function-plain") {
        return reaches_fields_result(v->inner_validator());
    }
    return false;
}

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
            py::object info_obj = make_validation_info(state);

            py::object transformed;
            try {
                // Try with info object (general/no-info-wrapped functions)
                transformed = py_func_(input.as_python_object(), info_obj);
            } catch (py::error_already_set& e1) {
                if (!e1.matches(PyExc_TypeError)) {
                    // Genuine exception raised by the validator function —
                    // convert it (ValueError -> value_error, etc.). Do NOT
                    // retry without info: that would mask the real exception
                    // with the retry's TypeError (e.g. a root_validator raising
                    // ValueError would surface as a bogus argument-count error).
                    return function_error_from_exception(e1, input, state);
                }
                // TypeError: likely a no-info function — retry without info.
                // restore() + PyErr_Clear() swallows the error so the
                // destructor's restore is a no-op.
                e1.restore();
                PyErr_Clear();
                try {
                    transformed = py_func_(input.as_python_object());
                } catch (py::error_already_set& e2) {
                    return function_error_from_exception(e2, input, state);
                }
            }

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

    // The result comes from the inner validator when one is present
    std::string effective_result_name() const override {
        if (inner_) return inner_->effective_result_name();
        return "function-before";
    }

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

        // If the inner validator reused an existing instance (ModelValidator with
        // revalidate_instances='never' returning PyObjectWrapper), the after-function
        // must NOT re-run: the instance is already validated, and re-running would
        // re-execute the model's after validators (fixes #8452: nested
        // model_validator(mode='after') re-executed when a parent model receives an
        // existing child instance). The result is stored as py::object so the
        // "function-after" result type dispatch stays consistent.
        if (inner_->effective_result_name().rfind("maybe_wrapper:", 0) == 0) {
            std::string base_type = inner_->effective_result_name().substr(14);
            if (base_type == "model" || base_type == "model-fields" || base_type == "typed-dict") {
                // Both PyObjectWrapper and ValidatedModelFieldsOutput/TypedDictResult
                // inherit from TypedResult, so the cast is safe.
                try {
                    auto* typed = static_cast<TypedResult*>(result.value().get());
                    if (typed && std::string(typed->result_type()) == "py_object") {
                        auto* wrapper = static_cast<PyObjectWrapper*>(typed);
                        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(wrapper->obj));
                    }
                } catch (...) {}
            } else {
                // Root model with primitive inner type (int, str, etc.): the result is
                // either a PyObjectWrapper (existing instance, revalidate='never') or a
                // primitive C++ value. We can't safely cast to TypedResult for primitive
                // values, so check if the input was an existing instance of the model.
                if (auto* mv = dynamic_cast<ModelValidator*>(inner_.get())) {
                    py::object cls = mv->expected_class();
                    if (!cls.is_none()) {
                        try {
                            py::object obj = input.as_python_object();
                            if (py::isinstance(obj, cls)) {
                                auto* wrapper = static_cast<PyObjectWrapper*>(result.value().get());
                                if (wrapper) {
                                    return ValResult<std::shared_ptr<void>>(
                                        std::make_shared<py::object>(wrapper->obj));
                                }
                            }
                        } catch (...) {}
                    }
                }
            }
        }

        // Convert the validated inner result to a Python object — Rust passes
        // the validated value to the after-function (model validators receive
        // the validated fields dict, field validators the coerced value).
        // NB: default-constructed py::object holds nullptr (not None) in
        // pybind11 >= 2.13, so always seed explicitly with py::none().
        py::object validated_obj = py::none();
        std::string inner_result_name = inner_->effective_result_name();
        bool inner_is_after = inner_ && inner_->name() == "function-after";
        // Rust passes a ModelFieldsValidator result to after-functions as
        // the raw 3-tuple (fields_dict, extra_or_None, fields_set) — this
        // is what V1 post root validators unpack in their shim.  Detect the
        // position structurally: chained after-functions hold a flattened
        // dict (py::object), direct/before-wrapped model-fields hold MFO.
        bool is_fields_result = reaches_fields_result(inner_);
        {
            if (is_fields_result) {
                if (!inner_is_after) {
                    try {
                        auto* mfo = static_cast<ValidatedModelFieldsOutput*>(result.value().get());
                        if (mfo) {
                            py::dict fields_out;
                            for (const auto& key : mfo->field_order) {
                                const auto& fv = mfo->fields.at(key);
                                fields_out[py::str(key)] = value_to_python_with_type(fv.value, fv.type_name);
                            }
                            py::object extra_obj = py::none();
                            if (!mfo->extra.empty()) {
                                py::dict extra_dict;
                                for (const auto& pair : mfo->extra) {
                                    extra_dict[py::str(pair.first)] =
                                        value_to_python_with_type(pair.second.value, pair.second.type_name);
                                }
                                extra_obj = extra_dict;
                            }
                            py::set fields_set;
                            for (const auto& fname : mfo->fields_set) {
                                fields_set.add(py::str(fname));
                            }
                            validated_obj = py::make_tuple(fields_out, extra_obj, fields_set);
                        }
                    } catch (...) {}
                } else {
                    // Chained after-function: the inner function returned the
                    // flattened dict form.  Rebuild the 3-tuple from it so
                    // this validator's shim also sees fields semantics.
                    try {
                        py::dict fd = value_to_python_with_type(result.value(), "py_object")
                                          .cast<py::dict>();
                        py::object extra_obj = fd.attr("pop")("__pydantic_extra__", py::none());
                        py::object fs_obj = fd.attr("pop")("__pydantic_fields_set__", py::none());
                        if (fs_obj.is_none()) {
                            fs_obj = fd.attr("keys")();
                        }
                        validated_obj = py::make_tuple(fd, extra_obj, fs_obj);
                    } catch (...) {}
                }
            }
            if (validated_obj.is_none()) {
                try {
                    // Honest runtime representation: a nested after-function
                    // produced a py::object, anything else its declared type.
                    validated_obj = value_to_python_with_type(
                        result.value(), inner_is_after ? "py_object" : inner_result_name);
                } catch (...) {
                    // Unknown inner result type: pass through the raw input
                    validated_obj = input.as_python_object();
                }
            }
        }

        // If the inner validator is a ModelValidator, construct the model instance
        // before calling the after-function (model_validator(mode='after') expects self)
        if (auto* model_validator = dynamic_cast<ModelValidator*>(inner_.get())) {
            py::object model_cls = model_validator->expected_class();
            if (!model_cls.is_none()) {
                try {
                    // Construct the model instance WITHOUT triggering validation
                    // (to avoid infinite recursion with model_validator)
                    py::object instance;
                    if (model_validator->root_model()) {
                        if (py::hasattr(model_cls, "model_construct")) {
                            instance = model_cls.attr("model_construct")(validated_obj);
                        } else {
                            instance = py::module_::import("builtins").attr("object").attr("__new__")(model_cls);
                            instance.attr("__dict__") = py::dict(py::arg("root") = validated_obj);
                        }
                    } else {
                        // For BaseModel, use model_construct to avoid validation
                        if (py::isinstance<py::dict>(validated_obj)) {
                            py::dict fields_dict = validated_obj.cast<py::dict>();
                            py::object extra = fields_dict.attr("pop")("__pydantic_extra__", py::none());
                            py::object fields_set = fields_dict.attr("pop")("__pydantic_fields_set__", py::set());
                            fields_dict.attr("pop")("__pydantic_defaults__", py::none());

                            instance = py::module_::import("builtins").attr("object").attr("__new__")(model_cls);
                            instance.attr("__dict__").attr("update")(fields_dict);
                            py::setattr(instance, "__pydantic_private__", py::none());
                            py::setattr(instance, "__pydantic_extra__",
                                extra.is_none() ? py::none() : extra);
                            py::setattr(instance, "__pydantic_fields_set__", fields_set);
                        } else {
                            instance = validated_obj;
                        }
                    }
                    validated_obj = instance;
                } catch (...) {
                    // If construction fails, continue with the validated_obj as-is
                }
            }
        }

        try {
            py::object info_obj = make_validation_info(state);

            py::object output;
            try {
                // Try with info object (general/no-info-wrapped functions)
                output = py_func_(validated_obj, info_obj);
            } catch (py::error_already_set& e1) {
                if (!e1.matches(PyExc_TypeError)) {
                    // Genuine exception raised by the validator function —
                    // convert it (ValueError -> value_error, etc.) instead of
                    // retrying without info and masking the real error.
                    return function_error_from_exception(e1, input, state);
                }
                // TypeError: likely a no-info function — retry without info
                // (e.g. attrgetter).  restore() + PyErr_Clear() swallows the
                // error so the destructor's restore is a no-op.
                e1.restore();
                PyErr_Clear();
                try {
                    output = py_func_(validated_obj);
                } catch (py::error_already_set& e2) {
                    if (e2.matches(PyExc_ValueError) || e2.matches(PyExc_AssertionError)) {
                        return function_error_from_exception(e2, input, state);
                    }
                    // Other failures (e.g. attrgetter('value') on a plain string),
                    // use the validated inner result as the output
                    e2.restore();
                    PyErr_Clear();
                    return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(validated_obj));
                }
            }
            // If the inner validator is a model-fields/typed-dict validator
            // (i.e. this is a V1 post root validator position), the shim
            // returns a (fields_dict, extra_or_None, fields_set) tuple.
            // Flatten it back into the plain-dict-with-dunder-keys format
            // downstream expects for "py_object" model results (the shim's
            // _build_model pops these keys when constructing the instance).
            // NB: do NOT return a ValidatedModelFieldsOutput here — models
            // wrapping a function-after declare their result type as
            // "py_object", so the value would be misread and crash.
            if (is_fields_result && py::isinstance<py::tuple>(output)) {
                py::tuple out_tuple = py::reinterpret_borrow<py::tuple>(output);
                py::ssize_t n = py::len(out_tuple);
                if ((n == 2 || n == 3) && !py::isinstance<py::dict>(out_tuple[0])) {
                    // Mirror Rust create_instance: the (invalid) validator
                    // return would be assigned to __dict__ and must raise.
                    std::string tname =
                        py::str(out_tuple[0].get_type().attr("__name__")).cast<std::string>();
                    throw py::type_error(
                        "__dict__ must be set to a dictionary, not a '" + tname + "'");
                }
                try {
                    if (n == 2 || (n == 3 && py::isinstance<py::dict>(out_tuple[0]))) {
                        py::dict flattened = py::reinterpret_borrow<py::dict>(out_tuple[0]);
                        if (n == 3) {
                            if (!out_tuple[1].is_none()) {
                                flattened["__pydantic_extra__"] = out_tuple[1];
                            }
                            flattened["__pydantic_fields_set__"] = out_tuple[2];
                        } else {
                            flattened["__pydantic_fields_set__"] = flattened.attr("keys")();
                        }
                        output = flattened;
                    }
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Clear();
                } catch (...) {}
            }
            // Return the output as a py::object.
            // For root models, the after-function returns the model instance (self),
            // but the binding expects the raw root value to store in d["root"].
            if (auto* mv = dynamic_cast<ModelValidator*>(inner_.get())) {
                if (mv->root_model()) {
                    py::object model_cls = mv->expected_class();
                    if (!model_cls.is_none()) {
                        try {
                            if (py::isinstance(output, model_cls) && py::hasattr(output, "root")) {
                                output = output.attr("root");
                            }
                        } catch (...) {}
                    }
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

    // Delegate to inner so is_root_model() works through function wrappers
    std::string root_model_inner_name() const override {
        return inner_ ? inner_->root_model_inner_name() : "";
    }

    // When the after-function is applied the result is a Python object;
    // otherwise it is the inner validator's result type.
    std::string effective_result_name() const override {
        if (py_func_.is_none() && inner_) return inner_->effective_result_name();
        // After-function always produces a py::object (the function's return
        // value).  Fields positions are detected structurally via
        // reaches_fields_result(), not through this name.
        return "py_object";
    }

    void set_py_func(py::object func) { py_func_ = std::move(func); }

private:
    std::shared_ptr<Validator> inner_;
    py::object py_func_;

public:
    std::shared_ptr<Validator> inner_validator() const override { return inner_; }
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
            py::object info_obj = make_validation_info(state);

            py::object output;
            try {
                // Try with info object (general/no-info-wrapped functions)
                output = py_func_(input.as_python_object(), info_obj);
            } catch (py::error_already_set& e1) {
                if (!e1.matches(PyExc_TypeError)) {
                    return function_error_from_exception(e1, input, state);
                }
                // TypeError: likely a no-info function — retry without info
                e1.restore();
                PyErr_Clear();
                try {
                    output = py_func_(input.as_python_object());
                } catch (py::error_already_set& e2) {
                    return function_error_from_exception(e2, input, state);
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
            py::object info_obj = make_validation_info(state);
            
            py::object handler = py::cpp_function([this, &state](py::object v) -> py::object {
                if (inner_) {
                    auto py_input = std::make_unique<PythonInput>(v);
                    auto result = inner_->validate(*py_input, state);
                    if (result.is_err()) {
                        throw py::value_error("Inner validator failed");
                    }
                    // Convert the validated result to a Python object by its
                    // actual stored type (e.g. EitherDate for date fields).
                    return value_to_python_with_type(result.value(), inner_->effective_result_name());
                }
                return v;
            });
            
            py::object output;
            try {
                // Try with info object (general wrap functions)
                output = py_func_(input.as_python_object(), handler, info_obj);
            } catch (py::error_already_set& e1) {
                if (!e1.matches(PyExc_TypeError)) {
                    // Genuine exception raised by the validator function —
                    // convert it (ValueError -> value_error, etc.) instead of
                    // retrying without info and masking the real error.
                    return function_error_from_exception(e1, input, state);
                }
                // TypeError: likely a no-info wrap function — retry without info
                e1.restore();
                PyErr_Clear();
                try {
                    output = py_func_(input.as_python_object(), handler);
                } catch (py::error_already_set& e2) {
                    return function_error_from_exception(e2, input, state);
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
                "FunctionWrap validator failed: " + msg
            );
        }
    }

    std::string name() const override { return "function-wrap"; }

    // When the wrap-function is applied the result is a Python object;
    // otherwise it is the inner validator's result type.
    std::string effective_result_name() const override {
        if (py_func_.is_none() && inner_) return inner_->effective_result_name();
        return "function-wrap";
    }

    void set_py_func(py::object func) { py_func_ = std::move(func); }

    std::shared_ptr<Validator> inner_validator() const override { return inner_; }

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
        // Compute the raw default as a Python object (Rust returns defaults raw
        // unless validate_default is set)
        py::object raw;
        bool has_raw = false;
        if (default_is_none_) {
            raw = py::none();
            has_raw = true;
        } else if (!default_factory_.is_none()) {
            try {
                raw = default_factory_();
                has_raw = true;
            } catch (py::error_already_set& e) {
                e.restore();
                PyErr_Clear();
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::CustomError),
                    state.location(),
                    "Default factory failed: " + std::string(e.what())
                );
            }
        } else if (default_value_) {
            raw = typed_default_to_py(default_value_, default_type_);
            has_raw = true;
        } else if (!default_value_str_.empty()) {
            auto parse_result = parse_json(default_value_str_);
            if (parse_result.is_ok()) {
                raw = parse_result.value()->as_python_object();
                has_raw = true;
            }
        }

        if (!has_raw) {
            if (inner_) return inner_->default_value(state);
            return ValError::omit();
        }

        if (validate_default_ && inner_) {
            PythonInput in(raw);
            return inner_->validate(in, state);
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(raw)));
    }

    std::string name() const override {
        // Delegate to the inner validator so result conversion uses the
        // actual stored value type instead of the "with-default" wrapper name.
        if (inner_) return inner_->name();
        return "with-default";
    }

    void set_default_is_none(bool v) { default_is_none_ = v; }
    bool has_none_default() const { return default_is_none_; }
    void set_default_factory(py::object f) { default_factory_ = std::move(f); }
    bool has_default_factory() const { return !default_factory_.is_none(); }
    void set_default_type(const std::string& t) { default_type_ = t; }
    void set_validate_default(bool v) { validate_default_ = v; }
    bool validate_default() const { return validate_default_; }

private:
    std::shared_ptr<Validator> inner_;
    std::shared_ptr<void> default_value_;
    std::string default_value_str_;
    std::string default_type_;
    bool default_is_none_ = false;
    bool validate_default_ = false;
    py::object default_factory_ = py::none();

    static py::object typed_default_to_py(const std::shared_ptr<void>& value, const std::string& type) {
        if (!value) return py::none();
        if (type == "str") {
            if (auto* s = static_cast<std::string*>(value.get())) return py::str(*s);
        } else if (type == "int") {
            if (auto* i = static_cast<int64_t*>(value.get())) return py::int_(*i);
            if (auto* i = static_cast<int*>(value.get())) return py::int_(*i);
        } else if (type == "float") {
            if (auto* d = static_cast<double*>(value.get())) return py::float_(*d);
        } else if (type == "bool") {
            if (auto* b = static_cast<bool*>(value.get())) return py::bool_(*b);
        }
        return py::none();
    }
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
    LaxOrStrictValidator() : lax_(nullptr), strict_(nullptr), used_strict_(false) {}
    LaxOrStrictValidator(std::shared_ptr<Validator> lax, std::shared_ptr<Validator> strict)
        : lax_(std::move(lax)), strict_(std::move(strict)), used_strict_(false) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (state.strict_or(false)) {
            if (!strict_) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::CustomError),
                    state.location(),
                    "lax-or-strict: strict validator is null"
                );
            }
            used_strict_ = true;
            return strict_->validate(input, state);
        }
        if (!lax_) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "lax-or-strict: lax validator is null"
            );
        }
        used_strict_ = false;
        return lax_->validate(input, state);
    }

    std::string name() const override { return "lax-or-strict"; }

    std::string effective_result_name() const override {
        if (used_strict_ && strict_) return strict_->effective_result_name();
        if (lax_) return lax_->effective_result_name();
        if (strict_) return strict_->effective_result_name();
        return "lax-or-strict";
    }

private:
    std::shared_ptr<Validator> lax_;
    std::shared_ptr<Validator> strict_;
    mutable bool used_strict_;
};

// JsonOrPythonValidator - uses different validators for JSON/Python input
class JsonOrPythonValidator : public Validator {
public:
    JsonOrPythonValidator() : json_(nullptr), python_(nullptr), used_python_(false) {}
    JsonOrPythonValidator(std::shared_ptr<Validator> json, std::shared_ptr<Validator> python)
        : json_(std::move(json)), python_(std::move(python)), used_python_(false) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (input.input_type() == InputType::Json) {
            if (!json_) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::CustomError),
                    state.location(),
                    "json-or-python: json validator is null"
                );
            }
            used_python_ = false;
            return json_->validate(input, state);
        }
        if (!python_) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "json-or-python: python validator is null"
            );
        }
        used_python_ = true;
        return python_->validate(input, state);
    }

    std::string name() const override { return "json-or-python"; }

    std::string effective_result_name() const override {
        if (used_python_ && python_) return python_->effective_result_name();
        if (json_) return json_->effective_result_name();
        if (python_) return python_->effective_result_name();
        return "json-or-python";
    }

private:
    std::shared_ptr<Validator> json_;
    std::shared_ptr<Validator> python_;
    mutable bool used_python_;
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

// ArgumentsValidator - validates function arguments (positional + keyword).
// Matches Rust's ArgumentsValidator (arguments.rs).  Produces a Python tuple
// of (validated_args, validated_kwargs) ready for a function call.
class ArgumentsValidator : public Validator {
public:
    struct Parameter {
        bool positional = false;       // accepts positional input (positional_only | positional_or_keyword)
        bool positional_only = false;  // positional_only mode
        std::string name;
        std::vector<std::string> validation_aliases;
        std::shared_ptr<Validator> validator;
    };

    std::vector<Parameter> parameters;
    size_t positional_params_count = 0;
    std::shared_ptr<Validator> var_args_validator;   // *args
    std::string var_kwargs_mode = "uniform";          // "uniform" | "unpacked-typed-dict"
    std::shared_ptr<Validator> var_kwargs_validator;  // **kwargs
    ExtraBehavior extra = ExtraBehavior::Forbid;
    bool validate_by_alias = true;
    bool validate_by_name = false;

    std::string name() const override { return "arguments"; }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto args_result = input.validate_args();
        if (args_result.is_err()) {
            return ValResult<std::shared_ptr<void>>(args_result.error());
        }
        ArgumentsInput args_in = std::move(args_result.value());
        py::tuple pos_args = std::move(args_in.args);
        py::dict kw_args = std::move(args_in.kwargs);

        py::list output_args;
        py::dict output_kwargs;
        std::vector<std::shared_ptr<ValLineError>> line_errors;
        std::unordered_set<std::string> used_kwargs;
        py::ssize_t n_pos = py::len(pos_args);

        auto add_error = [&](const ErrorType& et, const Location& loc, const std::string& input_repr) {
            line_errors.push_back(std::make_shared<ValLineError>(ValLineError{et, loc, input_repr}));
        };

        for (size_t index = 0; index < parameters.size(); ++index) {
            const Parameter& p = parameters[index];

            // Value from positional args (by index)
            std::optional<py::object> pos_value;
            if (p.positional && (py::ssize_t)index < n_pos) {
                pos_value = py::reinterpret_borrow<py::object>(pos_args[index]);
            }

            // Value from keyword args.  Matches Rust's LookupPathCollection:
            // aliases are looked up when validate_by_alias; the name is a
            // lookup key only when there is no alias or validate_by_name.
            // positional_only parameters never accept keyword input.
            std::optional<py::object> kw_value;
            if (!p.positional_only) {
                std::vector<std::string> lookup_keys;
                bool has_alias = !p.validation_aliases.empty();
                if (validate_by_alias) {
                    for (const auto& a : p.validation_aliases) {
                        lookup_keys.push_back(a);
                    }
                }
                if (!has_alias || validate_by_name) {
                    lookup_keys.push_back(p.name);
                }
                std::unordered_set<std::string> seen_keys;
                for (const auto& key : lookup_keys) {
                    if (!seen_keys.insert(key).second) continue;
                    if (kw_args.contains(py::str(key))) {
                        kw_value = py::reinterpret_borrow<py::object>(kw_args[py::str(key)]);
                        used_kwargs.insert(key);
                        break;
                    }
                }
            }

            if (pos_value && kw_value) {
                add_error(ErrorType(ErrorType::Kind::MultipleArgumentValues),
                          param_loc(p), py::repr(*kw_value).cast<std::string>());
            } else if (pos_value) {
                state.location().push(static_cast<int64_t>(index));
                PythonInput py_in(*pos_value);
                py_in.set_current_location(state.location());
                auto result = p.validator->validate(py_in, state);
                state.location().pop();
                if (result.is_ok()) {
                    output_args.append(value_to_python(result.value(), p.validator->effective_result_name(), &*pos_value));
                } else {
                    collect_line_errors(result.error(), line_errors);
                }
            } else if (kw_value) {
                state.location().push(p.name);
                PythonInput py_in(*kw_value);
                py_in.set_current_location(state.location());
                auto result = p.validator->validate(py_in, state);
                state.location().pop();
                if (result.is_ok()) {
                    output_kwargs[py::str(p.name)] = value_to_python(result.value(), p.validator->effective_result_name(), &*kw_value);
                } else {
                    collect_line_errors(result.error(), line_errors);
                }
            } else {
                // No value supplied — use default or report missing
                ValResult<std::shared_ptr<void>> def = p.validator->default_value(state);
                if (def.is_ok()) {
                    py::object val = default_to_python(p.validator, def.value());
                    if (p.positional_only) {
                        output_args.append(val);
                    } else {
                        output_kwargs[py::str(p.name)] = val;
                    }
                } else if (p.positional_only) {
                    add_error(ErrorType(ErrorType::Kind::MissingPositionalOnlyArgument),
                              loc_of_index(static_cast<int64_t>(index)), input.as_error_value().repr);
                } else if (p.positional) {
                    add_error(ErrorType(ErrorType::Kind::MissingArgument),
                              param_loc(p), input.as_error_value().repr);
                } else {
                    add_error(ErrorType(ErrorType::Kind::MissingKeywordOnlyArgument),
                              param_loc(p), input.as_error_value().repr);
                }
            }
        }

        // Extra positional args beyond the declared parameters
        if (n_pos > (py::ssize_t)positional_params_count) {
            for (py::ssize_t i = (py::ssize_t)positional_params_count; i < n_pos; ++i) {
                py::object item = py::reinterpret_borrow<py::object>(pos_args[i]);
                if (var_args_validator) {
                    state.location().push(i);
                    PythonInput py_in(item);
                    py_in.set_current_location(state.location());
                    auto result = var_args_validator->validate(py_in, state);
                    state.location().pop();
                    if (result.is_ok()) {
                        output_args.append(value_to_python(result.value(), var_args_validator->effective_result_name(), &item));
                    } else {
                        collect_line_errors(result.error(), line_errors);
                    }
                } else {
                    add_error(ErrorType(ErrorType::Kind::UnexpectedPositionalArgument),
                              loc_of_index(i), py::repr(item).cast<std::string>());
                }
            }
        }

        // Remaining kwargs: var_kwargs validation or forbid/allow handling
        py::dict remaining_kwargs;
        for (auto item : kw_args) {
            std::string key = py::str(item.first).cast<std::string>();
            if (used_kwargs.count(key)) continue;
            py::object value = py::reinterpret_borrow<py::object>(item.second);

            if (var_kwargs_mode == "unpacked-typed-dict") {
                remaining_kwargs[py::str(key)] = value;
            } else if (var_kwargs_validator) {
                state.location().push(key);
                PythonInput py_in(value);
                py_in.set_current_location(state.location());
                auto result = var_kwargs_validator->validate(py_in, state);
                state.location().pop();
                if (result.is_ok()) {
                    output_kwargs[py::str(key)] = value_to_python(result.value(), var_kwargs_validator->effective_result_name(), &value);
                } else {
                    collect_line_errors(result.error(), line_errors);
                }
            } else if (extra == ExtraBehavior::Forbid) {
                add_error(ErrorType(ErrorType::Kind::UnexpectedKeywordArgument),
                          loc_of_name(key), py::repr(value).cast<std::string>());
            }
        }

        if (var_kwargs_mode == "unpacked-typed-dict" && var_kwargs_validator) {
            // Validate the remaining kwargs as a single dict against the
            // typed-dict schema.  No location prefix: the typed-dict validator
            // reports field names directly (loc ('a',), ('b',), ...).
            PythonInput py_in(py::cast<py::object>(remaining_kwargs));
            auto result = var_kwargs_validator->validate(py_in, state);
            if (result.is_ok()) {
                py::object validated = value_to_python(result.value(), var_kwargs_validator->effective_result_name(), nullptr);
                if (py::isinstance<py::dict>(validated)) {
                    for (auto kv : validated.cast<py::dict>()) {
                        std::string key = py::str(kv.first).cast<std::string>();
                        if (key == "__pydantic_extra__") {
                            // Extra items (extra_items=... on the typed dict)
                            // are collected under __pydantic_extra__ — spread
                            // them into the real kwargs.
                            py::object extra_val = py::reinterpret_borrow<py::object>(kv.second);
                            if (py::isinstance<py::dict>(extra_val)) {
                                for (auto ekv : extra_val.cast<py::dict>()) {
                                    output_kwargs[py::str(ekv.first)] = py::reinterpret_borrow<py::object>(ekv.second);
                                }
                            }
                            continue;
                        }
                        // Skip other internal metadata keys — they are not real kwargs
                        if (key.rfind("__pydantic_", 0) == 0) continue;
                        output_kwargs[py::str(key)] = py::reinterpret_borrow<py::object>(kv.second);
                    }
                }
            } else {
                collect_line_errors(result.error(), line_errors);
            }
        }

        if (!line_errors.empty()) {
            return ValError::line_errors(std::move(line_errors));
        }
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<py::object>(py::make_tuple(py::tuple(output_args), output_kwargs))
        );
    }

private:
    static Location loc_of_name(const std::string& name) {
        Location loc;
        loc.push(name);
        return loc;
    }

    static Location loc_of_index(int64_t index) {
        Location loc;
        loc.push(index);
        return loc;
    }

    // Error location for keyword arguments: the first validation alias when
    // present, otherwise the parameter name (matches Rust's error_loc with
    // loc_by_alias).
    static Location param_loc(const Parameter& p) {
        if (!p.validation_aliases.empty()) {
            return loc_of_name(p.validation_aliases.front());
        }
        return loc_of_name(p.name);
    }

    static void collect_line_errors(const ValError& err, std::vector<std::shared_ptr<ValLineError>>& out) {
        for (auto& le : err.line_errors()) {
            out.push_back(le);
        }
    }

    // Convert a default value to a Python object.  WithDefaultValidator
    // returns the raw default as a py::object (unless validate_default is set,
    // in which case the result is the inner validator's validated type).
    static py::object default_to_python(const std::shared_ptr<Validator>& validator,
                                        const std::shared_ptr<void>& value) {
        if (auto* wd = dynamic_cast<WithDefaultValidator*>(validator.get())) {
            if (!wd->validate_default()) {
                try {
                    auto* obj = static_cast<py::object*>(value.get());
                    if (obj) return *obj;
                } catch (...) {}
                return py::none();
            }
        }
        return value_to_python(value, validator->effective_result_name(), nullptr);
    }

    // Convert a validated value to a Python object.  "any"-typed values pass
    // through the original input object (matching Rust's AnyValidator) because
    // the C++ AnyValidator round-trips through a string repr, which loses
    // arbitrary objects (classes, instances, ...).  Defaults for "any" params
    // have no raw input, so fall back to generic typed casts.
    static py::object value_to_python(const std::shared_ptr<void>& value, const std::string& type_name,
                                      const py::object* raw) {
        if (type_name == "any") {
            if (raw != nullptr) {
                return *raw;
            }
            if (value) {
                // AnyValidator round-trips through a string repr ("null", "true",
                // numbers, ...) — map the JSON-ish literals back to Python values.
                try {
                    auto* s = static_cast<std::string*>(value.get());
                    if (s) {
                        if (*s == "null") return py::none();
                        if (*s == "true") return py::bool_(true);
                        if (*s == "false") return py::bool_(false);
                        return py::str(*s);
                    }
                } catch (...) {}
                try { return py::int_(*static_cast<int64_t*>(value.get())); } catch (...) {}
                try { return py::float_(*static_cast<double*>(value.get())); } catch (...) {}
                try { return py::bool_(*static_cast<bool*>(value.get())); } catch (...) {}
                try { return *static_cast<py::object*>(value.get()); } catch (...) {}
            }
            return py::none();
        }
        return value_to_python_with_type(value, type_name);
    }
};

// CallValidator - validates function arguments, calls the function, and
// optionally validates the return value.  Matches Rust's CallValidator (call.rs).
class CallValidator : public Validator {
public:
    CallValidator() : function_(py::none()) {}
    CallValidator(std::shared_ptr<Validator> arguments_validator, py::object function,
                  std::shared_ptr<Validator> return_validator)
        : arguments_validator_(std::move(arguments_validator)),
          function_(std::move(function)),
          return_validator_(std::move(return_validator)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (!arguments_validator_) {
            return ValError::line_error(ErrorType(ErrorType::Kind::CustomError),
                                        state.location(), "Call validator missing arguments validator");
        }

        auto args_result = arguments_validator_->validate(input, state);
        if (args_result.is_err()) return args_result;

        py::object validated;
        try {
            validated = value_to_python_with_type(args_result.value(), arguments_validator_->name());
        } catch (...) {
            validated = py::none();
        }

        py::object result;
        if (py::isinstance<py::tuple>(validated) && py::len(validated) == 2) {
            py::tuple args_tuple = py::reinterpret_borrow<py::tuple>(validated[py::int_(0)]);
            py::dict kwargs_dict = py::reinterpret_borrow<py::dict>(validated[py::int_(1)]);
            PyObject* res = PyObject_Call(function_.ptr(), args_tuple.ptr(), kwargs_dict.ptr());
            if (res == nullptr) {
                throw py::error_already_set();
            }
            result = py::reinterpret_steal<py::object>(res);
        } else if (py::isinstance<py::dict>(validated)) {
            py::dict kwargs_dict = validated.cast<py::dict>();
            PyObject* res = PyObject_Call(function_.ptr(), nullptr, kwargs_dict.ptr());
            if (res == nullptr) {
                throw py::error_already_set();
            }
            result = py::reinterpret_steal<py::object>(res);
        } else {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "Arguments validator should return a tuple of (args, kwargs) or a dict of kwargs"
            );
        }

        if (return_validator_) {
            PythonInput ret_input(result);
            state.location().push("return");
            auto ret_result = return_validator_->validate(ret_input, state);
            state.location().pop();
            return ret_result;
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(result));
    }

    std::string name() const override { return "call"; }

    // The result type matches the return validator when one is present;
    // otherwise the function's own return value (a Python object).
    std::string effective_result_name() const override {
        if (return_validator_) return return_validator_->effective_result_name();
        return "call";
    }

    void set_arguments_validator(std::shared_ptr<Validator> v) { arguments_validator_ = std::move(v); }
    void set_function(py::object f) { function_ = std::move(f); }
    void set_return_validator(std::shared_ptr<Validator> v) { return_validator_ = std::move(v); }

private:
    std::shared_ptr<Validator> arguments_validator_;
    py::object function_;
    std::shared_ptr<Validator> return_validator_;
};

// PyDataclassValidator - validates pydantic dataclasses.
// Matches Rust's DataclassValidator (dataclass.rs).  The args (an
// ArgumentsValidator built from the dataclass-args schema) validates the
// input; positional args are merged back into the keyword dict by field
// order, then either the fields dict is returned (for the self_instance
// path used by the dataclass __init__) or a dataclass instance is
// constructed directly (nested / dict input).
class PyDataclassValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (!args_validator_) {
            return ValError::line_error(ErrorType(ErrorType::Kind::CustomError),
                                        state.location(), "Dataclass validator missing arguments validator");
        }

        auto args_result = args_validator_->validate(input, state);
        if (args_result.is_err()) {
            return ValResult<std::shared_ptr<void>>(args_result.error());
        }

        py::object validated;
        try {
            validated = value_to_python_with_type(args_result.value(), args_validator_->effective_result_name());
        } catch (...) {
            validated = py::none();
        }

        py::dict kwargs_dict;
        if (py::isinstance<py::tuple>(validated) && py::len(validated) == 2) {
            py::tuple args_tuple = py::reinterpret_borrow<py::tuple>(validated[py::int_(0)]);
            kwargs_dict = py::reinterpret_borrow<py::dict>(validated[py::int_(1)]);
            // Merge positional args into the kwargs dict by field order
            py::ssize_t n = py::len(args_tuple);
            for (py::ssize_t i = 0; i < n; ++i) {
                if (i >= (py::ssize_t)field_names_.size()) break;
                kwargs_dict[py::str(field_names_[static_cast<size_t>(i)])] =
                    py::reinterpret_borrow<py::object>(args_tuple[i]);
            }
        } else if (py::isinstance<py::dict>(validated)) {
            kwargs_dict = validated.cast<py::dict>();
        } else {
            return ValError::line_error(ErrorType(ErrorType::Kind::CustomError),
                                        state.location(), "Arguments validator returned unexpected type");
        }

        // Construct the instance when validating a dict input (nested
        // dataclass fields, JSON input).  Top-level calls from the dataclass
        // __init__ pass ArgsKwargs and rely on the binding's self_instance
        // path to populate the instance, so return the fields dict there.
        if (!input.is_args_kwargs() && !class_.is_none()) {
            py::object instance;
            try {
                instance = class_.attr("__new__")(class_);
            } catch (py::error_already_set& e) {
                e.restore();
                PyErr_Clear();
                return ValError::line_error(ErrorType(ErrorType::Kind::CustomError),
                                            state.location(), "Dataclass __new__ failed: " + std::string(e.what()));
            }
            auto setattr = py::module_::import("builtins").attr("object").attr("__setattr__");
            for (auto kv : kwargs_dict) {
                setattr(instance, kv.first, kv.second);
            }
            if (post_init_) {
                try {
                    instance.attr("__post_init__")();
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Clear();
                    return ValError::line_error(ErrorType(ErrorType::Kind::CustomError),
                                                state.location(), "Dataclass __post_init__ failed: " + std::string(e.what()));
                }
            }
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(instance));
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(kwargs_dict));
    }

    std::string name() const override { return "dataclass"; }

    void set_args_validator(std::shared_ptr<Validator> v) { args_validator_ = std::move(v); }
    void set_class(py::object c) { class_ = std::move(c); }
    void set_post_init(bool v) { post_init_ = v; }
    void set_field_names(std::vector<std::string> names) { field_names_ = std::move(names); }

private:
    std::shared_ptr<Validator> args_validator_;
    py::object class_ = py::none();
    bool post_init_ = false;
    std::vector<std::string> field_names_;
};

} // namespace pydantic_core