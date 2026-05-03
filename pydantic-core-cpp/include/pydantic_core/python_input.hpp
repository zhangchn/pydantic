#pragma once

#include <string>
#include "input.hpp"

namespace pybind11 {
    class object;
    class dict;
    class list;
    class tuple;
    class bool_;
    class str;
    class bytes;
    class bytearray;
    class int_;
    class float_;
}

namespace pydantic_core {

// Python input implementation
// Wraps pybind11 Python object for validation
class PythonInput : public Input {
public:
    explicit PythonInput(py::object obj) : obj_(obj) {}
    
    InputType input_type() const override { return InputType::Python; }
    
    InputValue as_error_value() const override;
    
    bool is_none() const override { obj_.is_none(); }
    
    // Type validation implementations
    ValResultMatch<EitherString> validate_str(bool strict, bool coerce_numbers = false) override;
    ValResultMatch<EitherBytes> validate_bytes(bool strict) override;
    ValResultMatch<bool> validate_bool(bool strict) override;
    ValResultMatch<EitherInt> validate_int(bool strict) override;
    ValResultMatch<EitherFloat> validate_float(bool strict) override;
    
    ValResult<std::unique_ptr<ValidatedDict>> validate_dict(bool strict) override;
    ValResultMatch<std::unique_ptr<ValidatedList>> validate_list(bool strict) override;
    ValResultMatch<std::unique_ptr<ValidatedTuple>> validate_tuple(bool strict) override;
    
    // Access underlying Python object
    py::object py_object() const { return obj_; }
    
private:
    py::object obj_;
};

// Validated dict for Python dict objects
class PythonValidatedDict : public ValidatedDict {
public:
    explicit PythonValidatedDict(py::dict dict) : dict_(dict) {}
    
    size_t size() const override { return dict_.size(); }
    bool empty() const override { return dict_.empty(); }
    
    std::vector<Entry> entries() const override;
    std::vector<std::string> keys() const override;
    
    bool has_key(const std::string& key) const override;
    std::optional<Entry> get(const std::string& key) const override;
    
private:
    py::dict dict_;
};

// Validated list for Python list objects
class PythonValidatedList : public ValidatedList {
public:
    explicit PythonValidatedList(py::list list) : list_(list) {}
    
    size_t size() const override { return list_.size(); }
    bool empty() const override { return list_.empty(); }
    
    std::vector<Entry> entries() const override;
    
private:
    py::list list_;
};

// Validated tuple for Python tuple objects
class PythonValidatedTuple : public ValidatedTuple {
public:
    explicit PythonValidatedTuple(py::tuple tuple) : tuple_(tuple) {}
    
    size_t size() const override { return tuple_.size(); }
    bool empty() const override { return tuple_.empty(); }
    
    std::vector<Entry> entries() const override;
    
private:
    py::tuple tuple_;
};

} // namespace pydantic_core