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
    ValResult<ValMatch<EitherDate>> validate_date(bool strict) const override;
    ValResult<ValMatch<EitherDateTime>> validate_datetime(bool strict) const override;
    ValResult<ValMatch<EitherTime>> validate_time(bool strict) const override;
    ValResult<ValMatch<EitherTimedelta>> validate_timedelta(bool strict) const override;

    // Container validation
    ValResult<std::unique_ptr<ValidatedDict>> validate_dict(bool strict) const override;
    ValResult<std::unique_ptr<ValidatedDict>> validate_dict_from_attributes(bool strict) const;
    ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_list(bool strict) const override;
    ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> validate_tuple(bool strict) const override;

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
