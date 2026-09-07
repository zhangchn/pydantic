#pragma once

#include <vector>
#include <string>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#ifdef HAS_PYBIND11
#include <pybind11/pybind11.h>
namespace py = pybind11;
#endif
#include "types.hpp"
#include "error_types.hpp"

namespace pydantic_core {

// Single validation line error
struct ValLineError {
    ErrorType error_type;
    Location location;
    std::string input_value;
#ifdef HAS_PYBIND11
    py::object raw_input_obj;  // Original Python object for accurate serialization (Rust parallel)
    py::object raw_error_obj;  // Python exception object for ctx['error'] (value_error/assertion_error)
#endif

    std::string message() const;
};

// Validation error class - matches Rust's ValError enum
class ValError {
public:
    enum class Kind {
        LineErrors,
        InternalErr,
        Omit,
        UseDefault
    };
    
    explicit ValError(Kind kind) : kind_(kind) {}
    
    static ValError line_error(const ErrorType& error_type, const Location& location,
                               const std::string& input_value) {
        ValError err(Kind::LineErrors);
        err.line_errors_.push_back(std::make_shared<ValLineError>(ValLineError{error_type, location, input_value}));
        return err;
    }

    static ValError line_errors(std::vector<std::shared_ptr<ValLineError>> errors) {
        ValError err(Kind::LineErrors);
        err.line_errors_ = std::move(errors);
        return err;
    }
    
    static ValError internal_err(const std::string& message) {
        ValError err(Kind::InternalErr);
        err.internal_message_ = message;
        return err;
    }

#ifdef HAS_PYBIND11
    // Rust convert_err: exceptions other than ValueError/AssertionError
    // become InternalErr carrying the ORIGINAL Python exception so it can be
    // re-raised at the binding boundary (e.g. RuntimeError propagation).
    static ValError internal_err(py::object py_exc) {
        ValError err(Kind::InternalErr);
        err.internal_py_err_ = std::move(py_exc);
        return err;
    }
    bool has_internal_py_err() const { return !internal_py_err_.is_none(); }
    const py::object& internal_py_err() const { return internal_py_err_; }
#endif
    
    static ValError omit() { return ValError(Kind::Omit); }
    static ValError use_default() { return ValError(Kind::UseDefault); }
    
    Kind kind() const { return kind_; }
    bool has_line_errors() const { return kind_ == Kind::LineErrors; }
    bool is_omit() const { return kind_ == Kind::Omit; }
    bool is_use_default() const { return kind_ == Kind::UseDefault; }
    bool is_internal() const { return kind_ == Kind::InternalErr; }
    
    const std::vector<std::shared_ptr<ValLineError>>& line_errors() const { return line_errors_; }
    const std::string& internal_message() const { return internal_message_; }
    
    void merge(ValError&& other) {
        if (kind_ == Kind::LineErrors && other.kind_ == Kind::LineErrors) {
            for (auto& e : other.line_errors_) {
                line_errors_.push_back(e);
            }
        }
    }
    
private:
    Kind kind_;
    std::vector<std::shared_ptr<ValLineError>> line_errors_;
    std::string internal_message_;
#ifdef HAS_PYBIND11
    py::object internal_py_err_ = py::none();
#endif
};

// ValidationError exception
class ValidationError : public std::exception {
public:
    ValidationError(const std::string& title, InputType input_type, const ValError& val_error,
                    bool hide_input = false);
    // Constructor with raw Python input for accurate error serialization
#ifdef HAS_PYBIND11
    ValidationError(const std::string& title, InputType input_type, const ValError& val_error,
                    py::object raw_input, bool hide_input = false);
#endif
    
    const char* what() const noexcept override { return what_message_.c_str(); }
    
    struct ErrorDetails {
        std::string type;
        std::string loc;
        std::vector<LocItem> loc_items;  // raw location items (preserves empty-string keys)
        std::string msg;
        std::string input;
        std::unordered_map<std::string, std::string> ctx;
        bool is_custom = false;
#ifdef HAS_PYBIND11
        bool has_raw_input = false;
        py::object raw_input_obj;
        bool has_raw_error = false;
        py::object raw_error_obj;
#endif
    };
    
    const std::vector<ErrorDetails>& errors() const { return errors_; }
    size_t error_count() const { return errors_.size(); }
    const std::string& title() const { return title_; }
    InputType input_type() const { return input_type_; }
    
    std::string error_count_message() const;
    std::string to_json_string() const;

    // Serialize error details (type/loc/msg/input/ctx) as a JSON list, used to
    // carry structured error info across the register_exception boundary where
    // the C++ object cannot be cast back from Python.
    std::string errors_to_json() const;
    
private:
    std::string title_;
    InputType input_type_;
    std::vector<ErrorDetails> errors_;
    std::string what_message_;
    // When set (config hide_input_in_errors), the display message omits the
    // input_value/input_type segment, matching Rust's pretty() renderer.
    bool hide_input_ = false;

    void build_errors_from_val_error(const ValError& val_error);
};

// SchemaError
class SchemaError : public std::runtime_error {
public:
    explicit SchemaError(const std::string& message) : std::runtime_error(message) {}
};

// PydanticSerializationError
class PydanticSerializationError : public std::exception {
public:
    explicit PydanticSerializationError(const std::string& message) : message_(message) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

// InputValue for error representation
struct InputValue {
    std::string repr;
    InputValue() : repr("None") {}
    explicit InputValue(const std::string& r) : repr(r) {}
    std::string to_string() const { return repr; }
};

} // namespace pydantic_core