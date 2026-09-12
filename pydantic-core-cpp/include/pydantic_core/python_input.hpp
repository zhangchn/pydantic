#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include "input.hpp"

namespace py = pybind11;

namespace pydantic_core {

// Rust's py_err_string: "QualName: str(exc)", or the bare qualname when str()
// returns an empty string.  Used for iteration_error/mapping_type contexts.
// Rust's py_err_string: "QualName: str(exc)", or the bare qualname when str()
// returns an empty string.  Used for iteration_error/mapping_type contexts.
inline std::string py_exception_type_and_message(PyObject* type, PyObject* value) {
    if (!type) {
        return "Unknown Error";
    }
    std::string out;
    try {
        PyObject* qualname = PyObject_GetAttrString(type, "__qualname__");
        if (!qualname) {
            PyErr_Clear();
            return "Unknown Error";
        }
        out = py::str(py::reinterpret_steal<py::object>(qualname)).cast<std::string>();
        PyObject* message_obj = value ? PyObject_Str(value) : nullptr;
        std::string message;
        if (message_obj) {
            message = py::str(py::reinterpret_steal<py::object>(message_obj)).cast<std::string>();
        } else if (value) {
            PyErr_Clear();
            message = "<exception str() failed>";
        }
        if (!message.empty()) out += ": " + message;
    } catch (...) {
        PyErr_Clear();
        return "Unknown Error";
    }
    return out;
}

inline std::string py_fetched_exception_string() {
    PyObject *type = nullptr, *value = nullptr, *traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    std::string out = py_exception_type_and_message(type, value);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    return out;
}

// Same formatting for an exception already captured by pybind11.
inline std::string py_caught_exception_string(const py::error_already_set& err) {
    return py_exception_type_and_message(err.type().ptr(), err.value().ptr());
}

// Rust reads a field container through the abstract Mapping protocol; text and
// sequence types satisfy PyMapping_Check too, so only Mapping instances pass.
inline bool py_is_mapping_instance(py::handle obj) {
    static py::object mapping_abc = py::module_::import("collections.abc").attr("Mapping");
    int result = PyObject_IsInstance(obj.ptr(), mapping_abc.ptr());
    if (result < 0) {
        PyErr_Clear();
        return false;
    }
    return result == 1;
}

// Rust's from_attributes_applicable: instances of classes defined in these
// modules are never read by attribute, so they report model_attributes_type.
inline bool py_from_attributes_applicable(py::handle obj) {
    // The module lives on the type: an instance only exposes it if its class
    // happens to keep __module__ in its own __dict__.
    PyObject* type = reinterpret_cast<PyObject*>(Py_TYPE(obj.ptr()));
    PyObject* module = PyObject_GetAttrString(type, "__module__");
    if (!module) {
        PyErr_Clear();
        return false;
    }
    bool applicable = true;
    try {
        std::string name = py::str(py::reinterpret_steal<py::object>(module)).cast<std::string>();
        applicable = name != "builtins" && name != "datetime" && name != "collections";
    } catch (...) {
        PyErr_Clear();
        applicable = false;
    }
    return applicable;
}

// Python validated dict implementation
class PythonValidatedDict : public ValidatedDict {
public:
    explicit PythonValidatedDict(py::dict d) : dict_(std::move(d)) {}

    size_t size() const override { return dict_.size(); }
    bool empty() const override { return dict_.size() == 0; }

    std::vector<Entry> entries() const override;
    std::vector<std::string> keys() const override;
    bool has_key(const std::string& key) const override;
    std::optional<Entry> get(const std::string& key) const override;

    // Get raw PyObject for a key (for nested validation)
    std::optional<py::object> get_object(const std::string& key) const;

    std::optional<py::object> get_value(const std::string& key) const override;
    std::optional<py::object> get_key(const std::string& key) const override;

    // Access underlying dict
    const py::dict& dict() const { return dict_; }

private:
    py::dict dict_;
};

// Python validated list implementation
class PythonValidatedList : public ValidatedList {
public:
    explicit PythonValidatedList(py::sequence seq) : seq_(std::move(seq)) {}

    size_t size() const override { return seq_.size(); }
    bool empty() const override { return seq_.size() == 0; }

    std::vector<Entry> entries() const override;

    // Get item at index as PyObject (for nested validation)
    py::object get_item(size_t index) const;

private:
    py::sequence seq_;
};

// Python validated tuple implementation
class PythonValidatedTuple : public ValidatedTuple {
public:
    explicit PythonValidatedTuple(py::tuple t) : tuple_(std::move(t)) {}

    size_t size() const override { return tuple_.size(); }
    bool empty() const override { return tuple_.size() == 0; }

    std::vector<Entry> entries() const override;

    // Get item at index as PyObject
    py::object get_item(size_t index) const;

private:
    py::tuple tuple_;
};

// Python input implementation - wraps pybind11 objects
// This provides direct access to Python objects without JSON round-trip
class PythonInput : public Input {
public:
    // Construct from Python object
    explicit PythonInput(py::object obj) : obj_(std::move(obj)) {}

    InputType input_type() const override { return InputType::Python; }
    InputValue as_error_value() const override;
    bool is_none() const override;
    
    // Get Python object representation - returns the underlying PyObject
    py::object as_python_object() const override { return obj_; }

    // Type validation methods
    ValResult<ValMatch<EitherString>> validate_str(bool strict, bool coerce_numbers = false) const override;
    ValResult<ValMatch<EitherBytes>> validate_bytes(bool strict) const override;
    ValResult<ValMatch<bool>> validate_bool(bool strict) const override;
    ValResult<ValMatch<EitherInt>> validate_int(bool strict) const override;
    ValResult<ValMatch<EitherFloat>> validate_float(bool strict) const override;

    // Date/time validation
    ValResult<ValMatch<EitherDate>> validate_date(bool strict, TimestampUnit unit) const override;
    ValResult<ValMatch<EitherDateTime>> validate_datetime(bool strict, TimestampUnit unit) const override;
    ValResult<ValMatch<EitherTime>> validate_time(bool strict) const override;
    ValResult<ValMatch<EitherTimedelta>> validate_timedelta(bool strict) const override;

    // Container validation
    ValResult<std::unique_ptr<ValidatedDict>> validate_dict(bool strict) const override;
    ValResult<std::unique_ptr<ValidatedDict>> validate_dict_from_attributes(bool strict) const;
    ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_list(bool strict) const override;
    ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> validate_tuple(bool strict) const override;
    ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_set(bool strict) const override;
    ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_frozenset(bool strict) const override;

    // Arguments validation: accepts ArgsKwargs instances and plain dicts (kwargs-only)
    ValResult<ArgumentsInput> validate_args() const override;

    // ArgsKwargs containers vs plain dicts
    bool is_args_kwargs() const override;

    // Access underlying PyObject
    const py::object& py_object() const { return obj_; }
    
    // from_attributes support: check if object has attributes
    bool has_attributes() const;
    bool is_dict_like() const;  // dict or object with attributes
    
    // Get object attributes as Python dict (for from_attributes mode)
    py::dict get_attributes_as_dict() const;

    // Type detection helpers (Python-specific)
    bool is_bool() const;
    bool is_int() const;
    bool is_float() const;
    bool is_str() const;
    bool is_bytes() const;
    bool is_dict() const;
    bool is_list() const;
    bool is_tuple() const;
    bool is_set() const;
    bool is_frozenset() const;
    bool is_sequence() const;

    // Special type detection (Python-specific)
    bool is_datetime() const;
    bool is_date() const;
    bool is_time() const;
    bool is_timedelta() const;
    bool is_uuid() const;
    bool is_decimal() const;
    bool is_complex() const;
    bool is_callable() const;

    // Rust input_python.rs::maybe_as_string: PyBytes is a string input as well,
    // and bytes that are not valid utf-8 must report the caller's own parsing
    // error instead of falling through to a type error.
    enum class StringSource { NotString, Ok, BadUtf8 };
    StringSource maybe_as_string(std::string* out) const;

    // Rust falls back to PyFloat_AsDouble, which accepts Decimal, Fraction and
    // any other object implementing __float__.
    bool as_float_via_number(double* out) const;

    // Value extraction helpers (Python-specific)
    std::string as_str() const;
    int64_t as_int() const;
    double as_float() const;
    std::vector<uint8_t> as_bytes() const;
    py::dict as_dict() const;
    py::list as_list() const;
    py::sequence as_sequence() const;

private:
    py::object obj_;

    // Helper: get Python type name
    std::string type_name() const;

    // Helper: check if object is instance of a Python type
    bool is_instance_of(const char* module, const char* type_name) const;
};

// Helper: create PythonInput from py::object
inline std::unique_ptr<PythonInput> make_python_input(py::object obj) {
    return std::make_unique<PythonInput>(std::move(obj));
}

} // namespace pydantic_core
