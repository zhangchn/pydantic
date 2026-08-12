#pragma once

#include "pydantic_core/validator.hpp"
#include <memory>
#include <optional>
#include <cmath>
#include <regex>
#include <algorithm>
#include <cctype>

namespace pydantic_core {

// AnyValidator - accepts any value
class AnyValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Accept any input - return the original Python object to preserve identity and type
        py::object obj = input.as_python_object();
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<py::object>(std::move(obj))
        );
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
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<bool>(result.value().value())
        );
    }
    
    std::string name() const override { return "bool"; }
};

// IntValidator - validates integer values
class IntValidator : public Validator {
public:
    bool strict = false;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_int(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& either_int = result.value().value();
        // Return the actual validated integer
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<int64_t>(either_int.as_i64().value_or(0))
        );
    }

    std::string name() const override { return "int"; }
};

// ConstrainedIntValidator - validates int with gt/lt/ge/le/multiple_of constraints
class ConstrainedIntValidator : public Validator {
public:
    bool strict = false;
    std::optional<int64_t> gt;
    std::optional<int64_t> ge;
    std::optional<int64_t> lt;
    std::optional<int64_t> le;
    std::optional<int64_t> multiple_of;

    ConstrainedIntValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_int(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& either_int = result.value().value();
        int64_t int_value = either_int.as_i64().value_or(0);

        if (gt.has_value() && int_value <= gt.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::GreaterThan, "gt", std::to_string(gt.value())),
                state.location(),
                std::to_string(int_value)
            );
        }
        if (ge.has_value() && int_value < ge.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::GreaterThanEqual, "ge", std::to_string(ge.value())),
                state.location(),
                std::to_string(int_value)
            );
        }
        if (lt.has_value() && int_value >= lt.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::LessThan, "lt", std::to_string(lt.value())),
                state.location(),
                std::to_string(int_value)
            );
        }
        if (le.has_value() && int_value > le.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::LessThanEqual, "le", std::to_string(le.value())),
                state.location(),
                std::to_string(int_value)
            );
        }
        if (multiple_of.has_value() && multiple_of.value() != 0) {
            if (int_value % multiple_of.value() != 0) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::MultipleOf, "multiple_of", std::to_string(multiple_of.value())),
                    state.location(),
                    std::to_string(int_value)
                );
            }
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<int64_t>(int_value));
    }
    
    std::string name() const override { return "constrained-int"; }
};

// FloatValidator - validates float values
class FloatValidator : public Validator {
public:
    bool strict = false;
    bool allow_inf_nan = true;

    FloatValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_float(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& either_float = result.value().value();
        double f = either_float.as_double();
        if (!allow_inf_nan && !std::isfinite(f)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FiniteNumber),
                state.location(),
                std::to_string(f)
            );
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<double>(f));
    }
    
    std::string name() const override { return "float"; }
};

// ConstrainedFloatValidator - validates float with gt/lt/ge/le/multiple_of constraints
class ConstrainedFloatValidator : public Validator {
public:
    bool strict = false;
    bool allow_inf_nan = true;
    std::optional<double> gt;
    std::optional<double> ge;
    std::optional<double> lt;
    std::optional<double> le;
    std::optional<double> multiple_of;

    ConstrainedFloatValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_float(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& either_float = result.value().value();
        double f = either_float.as_double();

        if (!allow_inf_nan && !std::isfinite(f)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FiniteNumber),
                state.location(),
                std::to_string(f)
            );
        }
        if (gt.has_value() && f <= gt.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::GreaterThan, "gt", std::to_string(gt.value())),
                state.location(),
                std::to_string(f)
            );
        }
        if (ge.has_value() && f < ge.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::GreaterThanEqual, "ge", std::to_string(ge.value())),
                state.location(),
                std::to_string(f)
            );
        }
        if (lt.has_value() && f >= lt.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::LessThan, "lt", std::to_string(lt.value())),
                state.location(),
                std::to_string(f)
            );
        }
        if (le.has_value() && f > le.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::LessThanEqual, "le", std::to_string(le.value())),
                state.location(),
                std::to_string(f)
            );
        }
        if (multiple_of.has_value() && multiple_of.value() != 0.0) {
            double tolerance = 1e-9;
            double rounded_div = std::round(f / multiple_of.value());
            double diff = std::abs(f - (rounded_div * multiple_of.value()));
            if (diff > tolerance) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::MultipleOf, "multiple_of", std::to_string(multiple_of.value())),
                    state.location(),
                    std::to_string(f)
                );
            }
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<double>(f));
    }
    
    std::string name() const override { return "constrained-float"; }
};

// StringValidator - validates string values
class StringValidator : public Validator {
public:
    bool strict = false;
    bool coerce_numbers_to_str = false;

    StringValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_str(state.strict_or(strict), coerce_numbers_to_str);
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<std::string>(result.value().value().to_string())
        );
    }
    
    std::string name() const override { return "str"; }
};

// StrConstrainedValidator - validates string with min_length/max_length/pattern/strip_whitespace/to_lower/to_upper
class StrConstrainedValidator : public Validator {
public:
    bool strict = false;
    bool coerce_numbers_to_str = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::string pattern;
    bool strip_whitespace = false;
    bool to_lower = false;
    bool to_upper = false;
    bool ascii_only = false;

    StrConstrainedValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_str(state.strict_or(strict), coerce_numbers_to_str);
        if (result.is_err()) {
            return result.error();
        }
        std::string str = result.value().value().to_string();

        if (strip_whitespace) {
            // Trim leading/trailing whitespace
            size_t start = str.find_first_not_of(" \t\n\r\f\v");
            if (start == std::string::npos) { str.clear(); }
            else {
                size_t end = str.find_last_not_of(" \t\n\r\f\v");
                str = str.substr(start, end - start + 1);
            }
        }

        // Length check using char count (Unicode-aware approximation)
        size_t char_count = str.size();  // UTF-8 byte count as proxy
        if (min_length.has_value() && char_count < min_length.value()) {
            std::string s = min_length.value() == 1 ? "" : "s";
            return ValError::line_error(
                ErrorType(ErrorType::Kind::StringTooShort, "min_length", std::to_string(min_length.value()), "s", s),
                state.location(),
                str
            );
        }
        if (max_length.has_value() && char_count > max_length.value()) {
            std::string s = max_length.value() == 1 ? "" : "s";
            return ValError::line_error(
                ErrorType(ErrorType::Kind::StringTooLong, "max_length", std::to_string(max_length.value()), "s", s),
                state.location(),
                str
            );
        }

        if (ascii_only) {
            for (char c : str) {
                if (static_cast<unsigned char>(c) > 127) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::StringNotAscii),
                        state.location(),
                        str
                    );
                }
            }
        }

        // Apply transformations
        if (to_lower) {
            std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c){ return std::tolower(c); });
        }
        if (to_upper) {
            std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c){ return std::toupper(c); });
        }

        // Pattern matching (basic regex via C++ std::regex)
        if (!pattern.empty()) {
            try {
                std::regex re(pattern);
                if (!std::regex_search(str, re)) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::StringPatternMismatch),
                        state.location(),
                        str
                    );
                }
            } catch (const std::regex_error&) {
                // Invalid regex, skip pattern check
            }
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(str));
    }
    
    std::string name() const override { return "constrained-str"; }
};

// BytesValidator - validates bytes values
class BytesValidator : public Validator {
public:
    bool strict = false;

    BytesValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_bytes(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherBytes>(result.value().value())
        );
    }
    
    std::string name() const override { return "bytes"; }
};

// BytesConstrainedValidator - validates bytes with min_length/max_length
class BytesConstrainedValidator : public Validator {
public:
    bool strict = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;

    BytesConstrainedValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_bytes(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& bytes = result.value().value();
        size_t len = bytes.size();

        if (min_length.has_value() && len < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::BytesTooShort, static_cast<int64_t>(min_length.value())),
                state.location(),
                bytes_repr(bytes)
            );
        }
        if (max_length.has_value() && len > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::BytesTooLong, "max_length", std::to_string(max_length.value())),
                state.location(),
                bytes_repr(bytes)
            );
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<EitherBytes>(bytes));
    }
    
    std::string name() const override { return "constrained-bytes"; }

private:
    static std::string bytes_repr(const EitherBytes& b) {
        auto v = b.to_vector();
        return "<bytes len=" + std::to_string(v.size()) + ">";
    }
};

// IsInstanceValidator - validates that input is an instance of a given Python class
class IsInstanceValidator : public Validator {
public:
    IsInstanceValidator() : py_class_(py::none()) {}
    IsInstanceValidator(std::string class_name, py::object py_class)
        : class_name_(std::move(class_name)), py_class_(std::move(py_class)) {}

    void set_class_name(const std::string& name) { class_name_ = name; }
    void set_py_class(py::object cls) { py_class_ = std::move(cls); }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Check if input is an instance of the specified Python class
        if (!py_class_.is_none()) {
            try {
                py::object input_py = input.as_python_object();
                if (!input_py) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                        state.location(),
                        input.as_error_value().repr
                    );
                }
                if (!py::isinstance(input_py, py_class_)) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                        state.location(),
                        input.as_error_value().repr
                    );
                }
                // Store as PyObject* — leak the reference to avoid GIL issues
                PyObject* raw = input_py.inc_ref().ptr();
                return ValResult<std::shared_ptr<void>>(
                    std::shared_ptr<void>(raw, [](void*){})
                );
            } catch (const std::exception& e) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                    state.location(),
                    std::string("Error checking isinstance: ") + e.what()
                );
            } catch (...) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                    state.location(),
                    "Unknown error checking isinstance"
                );
            }
        }
        // Fallback: if class name was specified but we don't have the Python class,
        // return error so UnionValidator can try other choices
        if (!class_name_.empty() && py_class_.is_none()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                state.location(),
                input.as_error_value().repr
            );
        }
        // Accept if no class info at all
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }

    std::string name() const override { return "py_raw_object"; }

private:
    std::string class_name_;
    py::object py_class_;
};

// IsSubclassValidator - validates that input is a subclass of a given Python class
class IsSubclassValidator : public Validator {
public:
    IsSubclassValidator() : py_class_(py::none()) {}
    IsSubclassValidator(std::string class_name, py::object py_class)
        : class_name_(std::move(class_name)), py_class_(std::move(py_class)) {}

    void set_class_name(const std::string& name) { class_name_ = name; }
    void set_py_class(py::object cls) { py_class_ = std::move(cls); }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Check if input is a subclass of the specified Python class
        // Input must be a type/class itself
        if (!py_class_.is_none()) {
            py::object input_py = input.as_python_object();
            try {
                py::object type_obj = py::module_::import("builtins").attr("type");
                if (!py::isinstance(input_py, type_obj)) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::IsSubclassType),
                        state.location(),
                        "Input must be a class/type, not an instance"
                    );
                }
                // Check subclass relationship using Python's issubclass()
                py::object builtins = py::module_::import("builtins");
                py::bool_ is_subclass = builtins.attr("issubclass")(input_py, py_class_);
                if (!is_subclass.cast<bool>()) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::IsSubclassType),
                        state.location(),
                        "Input is not a subclass of " + class_name_
                    );
                }
                // Store as PyObject* — leak the reference to avoid GIL issues
                // during shared_ptr destruction. The py::object returned by
                // result_to_python_with_type will hold its own reference.
                PyObject* raw = input_py.inc_ref().ptr();
                return ValResult<std::shared_ptr<void>>(
                    std::shared_ptr<void>(raw, [](void*){})
                );
            } catch (py::error_already_set& e) {
                std::string msg = e.what();
                e.restore();
                PyErr_Clear();
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::IsSubclassType),
                    state.location(),
                    "Subclass check failed: " + msg
                );
            }
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }

    std::string name() const override { return "py_raw_object"; }

private:
    std::string class_name_;
    py::object py_class_;
};

// CallableValidator - validates that input is callable
class CallableValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Check if input is callable
        py::object input_py = input.as_python_object();
        if (!py::hasattr(input_py, "__call__")) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CallableType),
                state.location(),
                "Input is not callable"
            );
        }
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<py::object>(input_py)
        );
    }

    std::string name() const override { return "callable"; }
};

} // namespace pydantic_core