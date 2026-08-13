#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/url_types.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <pybind11/pybind11.h>

namespace py = pybind11;
namespace pydantic_core {

// DefinitionsRegistry - stores named validators for recursive schema resolution
// Uses std::shared_ptr<Validator> to avoid circular dependency with CombinedValidator
class DefinitionsRegistry {
public:
    using ValidatorPtr = std::shared_ptr<Validator>;

    void add_definition(const std::string& ref, ValidatorPtr validator) {
        definitions_[ref] = std::move(validator);
    }

    ValidatorPtr get_definition(const std::string& ref) const {
        auto it = definitions_.find(ref);
        if (it != definitions_.end()) {
            return it->second;
        }
        return nullptr;
    }

    bool has_definition(const std::string& ref) const {
        return definitions_.find(ref) != definitions_.end();
    }

private:
    std::unordered_map<std::string, ValidatorPtr> definitions_;
};

// DefinitionRefValidator - resolves recursive schema references
class DefinitionRefValidator : public Validator {
public:
    DefinitionRefValidator() = default;
    
    void set_ref(const std::string& ref) {
        schema_ref_ = ref;
    }
    
    void set_definitions(std::shared_ptr<DefinitionsRegistry> definitions) {
        definitions_ = std::move(definitions);
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (definitions_) {
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                return def->validate(input, state);
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "Definition reference not found: " + schema_ref_
        );
    }

    std::string name() const override {
        if (definitions_) {
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                return def->name();
            }
        }
        return "definition-ref";
    }

    // Delegate expected-class lookup to the resolved definition (e.g. unions
    // use this to prefer the exact-class branch for model instance inputs).
    const py::object& expected_class() const override {
        if (definitions_) {
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                return def->expected_class();
            }
        }
        static const py::object none = py::none();
        return none;
    }

    const std::string& get_ref() const { return schema_ref_; }

private:
    std::string schema_ref_;
    std::shared_ptr<DefinitionsRegistry> definitions_;
};

// DateValidator - validates date values
class DateValidator : public Validator {
public:
    explicit DateValidator(bool strict = false) : strict_(strict) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_date(state.strict_or(strict_));
        if (result.is_err()) {
            // Preserve the specific error kind (date_parsing, date_from_datetime_inexact, etc.)
            return result.error();
        }
        auto match = std::move(result.value());
        // Store as internal Date value (will be converted to Python datetime.date by result_to_python)
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherDate>(std::move(match.value()))
        );
    }

    std::string name() const override { return "date"; }

private:
    bool strict_ = false;
};

// TimeValidator - validates time values
class TimeValidator : public Validator {
public:
    explicit TimeValidator(bool strict = false) : strict_(strict) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_time(state.strict_or(strict_));
        if (result.is_err()) {
            return result.error();
        }
        auto match = std::move(result.value());
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherTime>(std::move(match.value()))
        );
    }

    std::string name() const override { return "time"; }

private:
    bool strict_ = false;
};

// DatetimeValidator - validates datetime values
class DatetimeValidator : public Validator {
public:
    explicit DatetimeValidator(bool strict = false) : strict_(strict) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_datetime(state.strict_or(strict_));
        if (result.is_err()) {
            return result.error();
        }
        auto match = std::move(result.value());
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherDateTime>(std::move(match.value()))
        );
    }

    std::string name() const override { return "datetime"; }

private:
    bool strict_ = false;
};

// TimedeltaValidator - validates timedelta values
class TimedeltaValidator : public Validator {
public:
    explicit TimedeltaValidator(bool strict = false) : strict_(strict) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_timedelta(state.strict_or(strict_));
        if (result.is_err()) {
            return result.error();
        }
        auto match = std::move(result.value());
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherTimedelta>(std::move(match.value()))
        );
    }

    std::string name() const override { return "timedelta"; }

private:
    bool strict_ = false;
};

// UrlValidator - validates URL values
class UrlValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Validate URL using Python's urllib.parse
        py::object input_py = input.as_python_object();
        
        if (!py::isinstance<py::str>(input_py)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::UrlType),
                state.location(),
                input.as_error_value().repr
            );
        }
        
        std::string url_str = py::str(input_py).cast<std::string>();
        
        // Basic URL validation - check for scheme and netloc
        try {
            py::object urllib = py::module_::import("urllib.parse");
            py::object parsed = urllib.attr("urlparse")(url_str);
            
            std::string scheme = py::str(parsed.attr("scheme")).cast<std::string>();
            std::string netloc = py::str(parsed.attr("netloc")).cast<std::string>();
            
            // URL must have a scheme (http, https, ftp, etc.)
            if (scheme.empty()) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::UrlScheme),
                    state.location(),
                    url_str + " (missing scheme)"
                );
            }
            
            // URL must have a netloc (host) for most schemes
            if (netloc.empty() && scheme != "file" && scheme != "data") {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::UrlHost),
                    state.location(),
                    url_str + " (missing host)"
                );
            }
            
            // Valid URL - return Url object
            auto url_obj = std::make_shared<Url>(url_str);
            return ValResult<std::shared_ptr<void>>(
                std::static_pointer_cast<void>(url_obj)
            );
        } catch (py::error_already_set& e) {
            std::string msg = e.what();
            e.restore();
            PyErr_Clear();
            return ValError::line_error(
                ErrorType(ErrorType::Kind::UrlType),
                state.location(),
                "URL parsing failed: " + msg
            );
        }
    }

    std::string name() const override { return "url"; }
};

// MultiHostUrlValidator - validates multi-host URLs (e.g., mongodb://host1,host2,host3/db)
class MultiHostUrlValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        py::object input_py = input.as_python_object();
        
        // Check if already a MultiHostUrl object
        try {
            py::object mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
            py::object mh_class = mod.attr("MultiHostUrl");
            if (py::isinstance(input_py, mh_class)) {
                return ValResult<std::shared_ptr<void>>(
                    std::make_shared<std::string>(py::str(input_py).cast<std::string>())
                );
            }
        } catch (...) {}
        
        if (!py::isinstance<py::str>(input_py)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::UrlType),
                state.location(),
                input.as_error_value().repr
            );
        }
        
        std::string url_str = py::str(input_py).cast<std::string>();
        
        try {
            auto url_obj = std::make_shared<MultiHostUrl>(url_str);
            return ValResult<std::shared_ptr<void>>(
                std::static_pointer_cast<void>(url_obj)
            );
        } catch (const std::invalid_argument& e) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::UrlScheme),
                state.location(),
                e.what()
            );
        } catch (...) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::UrlType),
                state.location(),
                "Multi-host URL parsing failed: " + url_str
            );
        }
    }

    std::string name() const override { return "multi-host-url"; }
};

// UuidValidator - validates UUID values
class UuidValidator : public Validator {
public:
    bool strict = false;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Validate UUID using Python's uuid module
        py::object input_py = input.as_python_object();

        // If already a UUID object, accept it
        try {
            py::object uuid_mod = py::module_::import("uuid");
            py::object uuid_class = uuid_mod.attr("UUID");
            if (py::isinstance(input_py, uuid_class)) {
                return ValResult<std::shared_ptr<void>>(
                    std::make_shared<std::string>(py::str(input_py).cast<std::string>())
                );
            }
        } catch (...) {}

        // In strict mode, only accept UUID objects
        if (state.strict_or(strict)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IsInstanceType, "class", "UUID"),
                state.location(),
                input.as_error_value().repr
            );
        }

        // If string, validate format (lax mode only)
        if (py::isinstance<py::str>(input_py)) {
            std::string uuid_str = py::str(input_py).cast<std::string>();
            try {
                py::object uuid_mod = py::module_::import("uuid");
                py::object uuid_obj = uuid_mod.attr("UUID")(uuid_str);
                return ValResult<std::shared_ptr<void>>(
                    std::make_shared<std::string>(uuid_str)
                );
            } catch (py::error_already_set& e) {
                // Swallow the error: restore() releases the fetched refs so
                // the destructor is a no-op and the error indicator stays clear
                e.restore();
                PyErr_Clear();
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::UuidType),
                    state.location(),
                    "Invalid UUID format: " + uuid_str
                );
            }
        }

        return ValError::line_error(
            ErrorType(ErrorType::Kind::UuidType),
            state.location(),
            input.as_error_value().repr
        );
    }

    std::string name() const override { return "uuid"; }
};

// LiteralValidator - validates literal values
class LiteralValidator : public Validator {
public:
    LiteralValidator() = default;
    explicit LiteralValidator(std::vector<std::string> values, std::string expected_repr = "")
        : values_(std::move(values)), expected_repr_(std::move(expected_repr)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (values_.empty()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::LiteralError, "expected", expected_repr_.empty() ? "" : expected_repr_),
                state.location(),
                input.as_error_value().repr
            );
        }
        auto str_result = input.validate_str(state.strict_or(false), false);
        if (str_result.is_err()) {
            return str_result.error();
        }
        const auto& es = str_result.value().value();
        std::string str_val;
        if (auto* s = std::get_if<std::string>(&es.value)) {
            str_val = *s;
        } else {
            str_val = std::string(std::get<std::string_view>(es.value));
        }
        for (const auto& v : values_) {
            if (v == str_val) {
                return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(str_val));
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::LiteralError, "expected", expected_repr_.empty() ? "" : expected_repr_),
            state.location(),
            input.as_error_value().repr
        );
    }

    std::string name() const override { return "literal"; }

private:
    std::vector<std::string> values_;
    std::string expected_repr_;
};

// EnumValidator - validates enum values
class EnumValidator : public Validator {
public:
    EnumValidator() = default;
    explicit EnumValidator(std::unordered_set<std::string> valid_values)
        : valid_values_(std::move(valid_values)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (valid_values_.empty()) {
            return ValError::line_error(
                PydanticKnownError::enum_error(),
                state.location(),
                input.as_error_value().repr
            );
        }

        // Accept Enum member instances directly (matches Rust's exact-instance check),
        // e.g. `MyEnum.val` passed as input regardless of strict mode.
        try {
            py::object py_in = input.as_python_object();
            if (!py_in.is_none() && py::hasattr(py_in, "_name_") && py::hasattr(py_in, "_value_")) {
                std::string name = py::str(py_in).cast<std::string>();
                if (valid_values_.count(name)) {
                    return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(name));
                }
                auto dot_pos = name.rfind('.');
                std::string short_name = (dot_pos != std::string::npos) ? name.substr(dot_pos + 1) : name;
                if (valid_values_.count(short_name)) {
                    return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(short_name));
                }
                // Value-based match (e.g. int/str-valued enums whose names differ from values)
                py::object val = py::getattr(py_in, "_value_");
                if (py::isinstance<py::str>(val)) {
                    std::string v = val.cast<std::string>();
                    if (valid_values_.count(v)) {
                        return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(v));
                    }
                }
                return ValError::line_error(
                    PydanticKnownError::enum_error(),
                    state.location(),
                    input.as_error_value().repr
                );
            }
        } catch (...) {}

        auto str_result = input.validate_str(state.strict_or(false), false);
        if (str_result.is_err()) {
            return str_result.error();
        }
        auto es = str_result.value().value();
        std::string str_val;
        if (auto* s = std::get_if<std::string>(&es.value)) {
            str_val = *s;
        } else {
            str_val = std::string(std::get<std::string_view>(es.value));
        }
        if (valid_values_.count(str_val)) {
            return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(str_val));
        }
        // Also try matching against the short form (without class prefix)
        for (const auto& vv : valid_values_) {
            auto dot_pos = vv.rfind('.');
            if (dot_pos != std::string::npos && dot_pos + 1 < vv.size()) {
                auto short_name = vv.substr(dot_pos + 1);
                if (short_name == str_val) {
                    return ValResult<std::shared_ptr<void>>(
                        std::make_shared<std::string>(str_val));
                }
            }
        }
        return ValError::line_error(
            PydanticKnownError::enum_error(),
            state.location(),
            input.as_error_value().repr
        );
    }
    
    std::string name() const override { return "enum"; }

private:
    std::unordered_set<std::string> valid_values_;
};

// CustomErrorValidator - wraps an inner validator, replacing its error with a custom one
class CustomErrorValidator : public Validator {
public:
    CustomErrorValidator() = default;
    CustomErrorValidator(std::shared_ptr<Validator> inner, std::string msg, std::string error_type)
        : inner_(std::move(inner)), msg_(std::move(msg)), error_type_(std::move(error_type)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (inner_) {
            auto result = inner_->validate(input, state);
            if (result.is_ok()) return result;
        }
        // Inner failed (or no inner) — raise custom error
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            msg_.empty() ? "Custom error" : msg_
        );
    }

    std::string name() const override {
        if (inner_) return inner_->name();
        return "custom-error";
    }

    std::string effective_result_name() const override {
        if (inner_) return inner_->effective_result_name();
        return "custom-error";
    }

private:
    std::shared_ptr<Validator> inner_;
    std::string msg_;
    std::string error_type_;
};

} // namespace pydantic_core