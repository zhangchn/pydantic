#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/url_types.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <pybind11/pybind11.h>
#include <cstdio>
#include <stdexcept>

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

    std::string effective_result_name() const override {
        if (definitions_) {
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                return def->effective_result_name();
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
    std::optional<size_t> max_length;
    std::vector<std::string> allowed_schemes;
    bool preserve_empty_path = false;
    std::optional<bool> host_required;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Accept an already-validated Url object (Rust downcast_python_input::<PyUrl>)
        py::object input_py = input.as_python_object();
        try {
            py::object mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
            py::object url_class = mod.attr("Url");
            if (py::isinstance(input_py, url_class)) {
                if (max_length.has_value()) {
                    std::string s = py::str(input_py).cast<std::string>();
                    if (s.size() > max_length.value()) {
                        ErrorType err(ErrorType::Kind::UrlTooLong);
                        err.context()["max_length"] = std::to_string(max_length.value());
                        err.context()["s"] = max_length.value() == 1 ? "" : "s";
                        return ValError::line_error(err, state.location(),
                                                    input.as_error_value().repr);
                    }
                }
                return ValResult<std::shared_ptr<void>>(
                    std::make_shared<py::object>(std::move(input_py)));
            }
        } catch (...) {}

        if (!py::isinstance<py::str>(input_py) && !py::isinstance<py::bytes>(input_py)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::UrlType),
                state.location(),
                input.as_error_value().repr
            );
        }

        // Lax bytes -> str coercion (matches input.validate_str).
        std::string url_str = py::cast<std::string>(input_py);
        // url crate trims leading/trailing C0 control + space before parsing.
        strip_url_whitespace(url_str);

        // Length check happens BEFORE parsing (Rust UrlValidator::check_length)
        if (max_length.has_value() && url_str.size() > max_length.value()) {
            ErrorType err(ErrorType::Kind::UrlTooLong);
            err.context()["max_length"] = std::to_string(max_length.value());
            err.context()["s"] = max_length.value() == 1 ? "" : "s";
            return ValError::line_error(err, state.location(), url_str);
        }

        auto url_parsing_err = [&](const std::string& error) {
            ErrorType err(ErrorType::Kind::UrlParsing);
            err.context()["error"] = error;
            return ValError::line_error(err, state.location(), url_str);
        };
        auto syntax_violation_err = [&](const std::string& error) {
            ErrorType err(ErrorType::Kind::UrlSyntaxViolation);
            err.context()["error"] = error;
            return ValError::line_error(err, state.location(), url_str);
        };

        // Parse using Python's urllib (mimics url::Url::parse + syntax violations)
        try {
            py::object urllib = py::module_::import("urllib.parse");
            py::object parsed = urllib.attr("urlparse")(url_str);

            std::string scheme = py::str(parsed.attr("scheme")).cast<std::string>();
            std::string netloc = py::str(parsed.attr("netloc")).cast<std::string>();

            // URL must have a scheme (Rust: ParseError::RelativeUrlWithoutBase -> UrlParsing)
            if (scheme.empty()) {
                return url_parsing_err("relative URL without a base");
            }

            // Scheme allow-list (Rust: UrlScheme with expected_schemes)
            if (!allowed_schemes.empty()) {
                bool ok = false;
                for (const auto& s : allowed_schemes) {
                    if (scheme == s) { ok = true; break; }
                }
                if (!ok) {
                    ErrorType err(ErrorType::Kind::UrlScheme);
                    std::string expected;
                    for (size_t i = 0; i < allowed_schemes.size(); ++i) {
                        if (i > 0) expected += i == allowed_schemes.size() - 1 ? " or " : ", ";
                        expected += "'" + allowed_schemes[i] + "'";
                    }
                    err.context()["expected_schemes"] = expected;
                    return ValError::line_error(err, state.location(), url_str);
                }
            }

            // Host requirements (Rust: special schemes need a host;
            // strict mode reports an empty host as a syntax violation)
            bool host_req = host_required.value_or(is_special_scheme(scheme) && scheme != "file");
            if (netloc.empty() && host_req) {
                if (state.strict_or(false)) {
                    return syntax_violation_err("empty host");
                }
                return url_parsing_err("empty host");
            }

            // Valid URL - return Url object
            auto url_obj = std::make_shared<Url>(url_str, preserve_empty_path);
            return ValResult<std::shared_ptr<void>>(
                std::static_pointer_cast<void>(url_obj)
            );
        } catch (py::error_already_set& e) {
            std::string msg = e.what();
            e.restore();
            PyErr_Clear();
            return url_parsing_err(msg);
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

// DecimalValidator - validates decimal values
class DecimalValidator : public Validator {
public:
    bool strict = false;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        py::object input_py = input.as_python_object();
        py::object decimal_mod = py::module_::import("decimal");
        py::object decimal_cls = decimal_mod.attr("Decimal");

        if (py::isinstance(input_py, decimal_cls)) {
            return ValResult<std::shared_ptr<void>>(
                std::make_shared<py::object>(std::move(input_py))
            );
        }

        if (state.strict_or(strict)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IsInstanceType, "class", "Decimal"),
                state.location(),
                input.as_error_value().repr
            );
        }

        try {
            py::object result;
            if (py::isinstance<py::str>(input_py) || py::isinstance<py::int_>(input_py) || py::isinstance<py::float_>(input_py)) {
                result = decimal_cls(input_py);
            } else {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::DecimalType),
                    state.location(),
                    input.as_error_value().repr
                );
            }
            return ValResult<std::shared_ptr<void>>(
                std::make_shared<py::object>(std::move(result))
            );
        } catch (py::error_already_set& e) {
            e.restore();
            PyErr_Clear();
            // String/int/float that failed to parse -> DecimalParsing (Rust)
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DecimalParsing),
                state.location(),
                input.as_error_value().repr
            );
        }
    }

    std::string name() const override { return "decimal"; }
    std::string effective_result_name() const override { return "py_object"; }
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

        // In strict mode (python input), only accept UUID objects (Rust: IsInstanceOf)
        if (state.strict_or(strict)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IsInstanceType, "class", "UUID"),
                state.location(),
                input.as_error_value().repr
            );
        }

        // String: parse it (Rust: UuidParsing on failure)
        if (py::isinstance<py::str>(input_py)) {
            std::string uuid_str = py::str(input_py).cast<std::string>();
            try {
                py::object uuid_mod = py::module_::import("uuid");
                py::object uuid_obj = uuid_mod.attr("UUID")(uuid_str);
                return ValResult<std::shared_ptr<void>>(
                    std::make_shared<std::string>(py::str(uuid_obj).cast<std::string>())
                );
            } catch (py::error_already_set& e) {
                std::string msg = e.what();
                // Swallow the error: restore() releases the fetched refs so
                // the destructor is a no-op and the error indicator stays clear
                e.restore();
                PyErr_Clear();
                ErrorType err(ErrorType::Kind::UuidParsing);
                err.context()["error"] = msg;
                return ValError::line_error(err, state.location(), uuid_str);
            }
        }

        // Bytes: parse (Rust: UuidType when not bytes, UuidParsing when invalid)
        if (py::isinstance<py::bytes>(input_py)) {
            std::string b = input_py.cast<std::string>();
            try {
                py::object uuid_mod = py::module_::import("uuid");
                py::object uuid_obj;
                try {
                    uuid_obj = uuid_mod.attr("UUID")(py::bytes(b));
                } catch (py::error_already_set& e1) {
                    e1.restore();
                    PyErr_Clear();
                    try {
                        uuid_obj = uuid_mod.attr("UUID")(b);
                    } catch (py::error_already_set& e2) {
                        std::string msg = e2.what();
                        e2.restore();
                        PyErr_Clear();
                        ErrorType err(ErrorType::Kind::UuidParsing);
                        err.context()["error"] = msg;
                        return ValError::line_error(err, state.location(),
                                                    input.as_error_value().repr);
                    }
                }
                return ValResult<std::shared_ptr<void>>(
                    std::make_shared<std::string>(py::str(uuid_obj).cast<std::string>())
                );
            } catch (...) {}
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

    static bool is_exact_str(const py::object& o) {
        return Py_TYPE(o.ptr()) == &PyUnicode_Type;
    }

    // Build from Python expected values, mirroring Rust LiteralLookup::new:
    // exact bool/int/str maps plus a hash-based dict for everything else
    // (enum members hash-equal their underlying value, so a plain-string
    // input matches a StrEnum literal and yields the MEMBER back).
    explicit LiteralValidator(py::sequence expected, std::string expected_repr = "")
        : expected_repr_(std::move(expected_repr)) {
        size_t id = 0;
        for (auto handle : expected) {
            py::object k = py::reinterpret_borrow<py::object>(handle);
            if (py::isinstance<py::bool_>(k)) {
                bool_ids_.push_back(id);
            } else if (py::isinstance<py::int_>(k)) {
                try {
                    int_ids_.emplace(k.cast<long long>(), id);
                } catch (...) {}
            } else if (is_exact_str(k)) {
                str_ids_.emplace(k.cast<std::string>(), id);
            }
            try {
                expected_dict_[k] = py::int_(static_cast<long long>(id));
            } catch (...) {
                // Unhashable expected value: linear equality scan instead.
                eq_ids_.push_back(id);
            }
            values_.push_back(k);
            ++id;
        }
    }

    // Legacy convenience ctor (JSON-schema path): wrap plain strings.
    explicit LiteralValidator(std::vector<std::string> values, std::string expected_repr = "")
        : expected_repr_(std::move(expected_repr)) {
        py::list items;
        for (const auto& v : values) items.append(py::str(v));
        *this = LiteralValidator(items, expected_repr_);
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Build a literal_error carrying the ORIGINAL Python input object so
        // error serialization matches Rust (which keeps the raw input rather
        // than its repr).
        auto literal_error = [&]() -> ValError {
            ValError err = ValError::line_error(
                ErrorType(ErrorType::Kind::LiteralError, "expected", expected_repr_.empty() ? "" : expected_repr_),
                state.location(),
                input.as_error_value().repr
            );
#ifdef HAS_PYBIND11
            if (!err.line_errors().empty()) {
                err.line_errors()[0]->raw_input_obj = input.as_python_object();
            }
#endif
            return err;
        };

        if (values_.empty()) {
            return literal_error();
        }
        auto make_result = [&](size_t id) {
            return ValResult<std::shared_ptr<void>>(
                std::make_shared<py::object>(values_[id]));
        };

        py::object py_in = input.as_python_object();

        // 1. Exact bool match (before int: bool subclasses int).
        if (py::isinstance<py::bool_>(py_in)) {
            bool b = py_in.cast<bool>();
            for (size_t id : bool_ids_) {
                if (values_[id].cast<bool>() == b) return make_result(id);
            }
        } else {
            // 2. Exact int match (no lax str/float coercion, unlike Rust's
            //    generic str validation — literals are strict about type).
            if (py::isinstance<py::int_>(py_in)) {
                try {
                    long long i = py_in.cast<long long>();
                    auto it = int_ids_.find(i);
                    if (it != int_ids_.end()) return make_result(it->second);
                } catch (...) {}
            }
            // 3. Exact (non-subclass) str match.
            if (is_exact_str(py_in)) {
                auto it = str_ids_.find(py_in.cast<std::string>());
                if (it != str_ids_.end()) return make_result(it->second);
            }
        }
        // 4. Hash-based lookup: covers enum-member literals matched by their
        //    plain value (StrEnum members hash/eq like their string value)
        //    and any other hashable expected objects.
        bool hashed = false;
        try {
            hashed = expected_dict_.contains(py_in);
        } catch (py::error_already_set&) {
            PyErr_Clear();  // unhashable input
        }
        if (hashed) {
            return make_result(expected_dict_[py_in].cast<size_t>());
        }
        // 5. Equality scan for unhashable expected values.
        for (size_t id : eq_ids_) {
            try {
                if (values_[id].equal(py_in)) return make_result(id);
            } catch (...) {}
        }
        return literal_error();
    }

    std::string name() const override { return "literal"; }

private:
    std::vector<py::object> values_;
    std::vector<size_t> bool_ids_;
    std::unordered_map<long long, size_t> int_ids_;
    std::unordered_map<std::string, size_t> str_ids_;
    py::dict expected_dict_;
    std::vector<size_t> eq_ids_;
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
        // Inner failed (or no inner) — raise custom error.
        // Build an ErrorType whose type_name matches error_type_, and whose
        // message is the rendered msg_ (no template processing needed).
        std::string type_key = error_type_.empty() ? "custom_error" : error_type_;
        std::string rendered_msg = msg_.empty() ? "Validation error" : msg_;
        auto error_type = ErrorType(type_key, rendered_msg);
        ValError val_err = ValError::line_error(
            std::move(error_type),
            state.location(),
            input.as_error_value().repr
        );
        // Preserve original Python object for accurate serialization (Rust parallel:
        // as_val_error(input) where input holds Py<PyAny>).
#ifdef HAS_PYBIND11
        if (!val_err.line_errors().empty()) {
            val_err.line_errors()[0]->raw_input_obj = input.as_python_object();
        }
#endif
        return val_err;
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