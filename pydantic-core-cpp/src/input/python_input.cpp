#include "pydantic_core/python_input.hpp"
#include "pydantic_core/error_types.hpp"
#include "pydantic_core/result.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <sstream>

namespace pydantic_core {

namespace py = pybind11;

InputValue PythonInput::as_error_value() const {
    try {
        std::string repr = py::repr(obj_);
        // Truncate if too long
        if (repr.length() > 100) {
            repr = repr.substr(0, 97) + "...";
        }
        return InputValue(repr);
    } catch (...) {
        return InputValue("<error getting repr>");
    }
}

ValResultMatch<EitherString> PythonInput::validate_str(bool strict, bool coerce_numbers) {
    // Check if already a string
    if (py::isinstance<py::str>(obj_)) {
        std::string s = py::cast<std::string>(obj_);
        return ValMatch::exact(EitherString(s));
    }
    
    // In lax mode, try coercion
    if (!strict) {
        // Try to get string representation
        try {
            std::string s = py::cast<std::string>(obj_);
            return ValMatch::lax(EitherString(s));
        } catch (...) {
            // Coercion failed
        }
        
        // Try coerce from numbers if allowed
        if (coerce_numbers) {
            if (py::isinstance<py::int_>(obj_) || py::isinstance<py::float_>(obj_)) {
                std::string s = py::cast<std::string>(obj_);
                return ValMatch::lax(EitherString(s));
            }
        }
    }
    
    // Type error
    return ValError::line_error(PydanticKnownError::string_type(), 
                               Location(), as_error_value().repr);
}

ValResultMatch<EitherBytes> PythonInput::validate_bytes(bool strict) {
    if (py::isinstance<py::bytes>(obj_)) {
        py::bytes b = py::cast<py::bytes>(obj_);
        std::string s = py::cast<std::string>(b);
        return ValMatch::exact(EitherBytes(std::vector<uint8_t>(s.begin(), s.end())));
    }
    
    if (py::isinstance<py::bytearray>(obj_)) {
        py::bytearray b = py::cast<py::bytearray>(obj_);
        std::string s = py::cast<std::string>(b);
        return ValMatch::exact(EitherBytes(std::vector<uint8_t>(s.begin(), s.end())));
    }
    
    // Lax mode - try conversion
    if (!strict) {
        try {
            std::string s = py::cast<std::string>(obj_);
            return ValMatch::lax(EitherBytes(std::vector<uint8_t>(s.begin(), s.end())));
        } catch (...) {}
    }
    
    return ValError::line_error(PydanticKnownError::bytes_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<bool> PythonInput::validate_bool(bool strict) {
    if (py::isinstance<py::bool_>(obj_)) {
        bool b = py::cast<bool>(obj_);
        return ValMatch::exact(b);
    }
    
    // In lax mode, accept int 0/1 as bool
    if (!strict) {
        if (py::isinstance<py::int_>(obj_)) {
            int64_t i = py::cast<int64_t>(obj_);
            if (i == 0 || i == 1) {
                return ValMatch::lax(static_cast<bool>(i));
            }
        }
        // Try string "true"/"false"
        if (py::isinstance<py::str>(obj_)) {
            std::string s = py::cast<std::string>(obj_);
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            if (s == "true" || s == "1" || s == "on" || s == "yes") {
                return ValMatch::lax(true);
            }
            if (s == "false" || s == "0" || s == "off" || s == "no") {
                return ValMatch::lax(false);
            }
        }
    }
    
    return ValError::line_error(PydanticKnownError::bool_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<EitherInt> PythonInput::validate_int(bool strict) {
    if (py::isinstance<py::int_>(obj_)) {
        int64_t i = py::cast<int64_t>(obj_);
        return ValMatch::exact(EitherInt(i));
    }
    
    // Lax mode - try coercion
    if (!strict) {
        // Float to int if exact value
        if (py::isinstance<py::float_>(obj_)) {
            double d = py::cast<double>(obj_);
            if (std::floor(d) == d && std::abs(d) < 1e18) {
                return ValMatch::lax(EitherInt(static_cast<int64_t>(d)));
            }
        }
        // String to int
        if (py::isinstance<py::str>(obj_)) {
            std::string s = py::cast<std::string>(obj_);
            try {
                int64_t i = std::stoll(s);
                return ValMatch::lax(EitherInt(i));
            } catch (...) {}
        }
        // Bool to int
        if (py::isinstance<py::bool_>(obj_)) {
            bool b = py::cast<bool>(obj_);
            return ValMatch::lax(EitherInt(static_cast<int64_t>(b)));
        }
    }
    
    return ValError::line_error(PydanticKnownError::int_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<EitherFloat> PythonInput::validate_float(bool strict) {
    if (py::isinstance<py::float_>(obj_)) {
        double d = py::cast<double>(obj_);
        return ValMatch::exact(EitherFloat(d));
    }
    
    // Int is acceptable as float
    if (py::isinstance<py::int_>(obj_)) {
        int64_t i = py::cast<int64_t>(obj_);
        double d = static_cast<double>(i);
        if (strict) {
            return ValMatch::exact(EitherFloat(d));
        }
        return ValMatch::lax(EitherFloat(d));
    }
    
    // Lax mode - string coercion
    if (!strict) {
        if (py::isinstance<py::str>(obj_)) {
            std::string s = py::cast<std::string>(obj_);
            try {
                double d = std::stod(s);
                return ValMatch::lax(EitherFloat(d));
            } catch (...) {}
        }
    }
    
    return ValError::line_error(PydanticKnownError::float_type(),
                               Location(), as_error_value().repr);
}

ValResult<std::unique_ptr<ValidatedDict>> PythonInput::validate_dict(bool strict) {
    if (py::isinstance<py::dict>(obj_)) {
        py::dict d = py::cast<py::dict>(obj_);
        return std::make_unique<PythonValidatedDict>(d);
    }
    
    // Lax mode - try to convert
    if (!strict) {
        // Try items() method if available
        try {
            py::object items_method = obj_.attr("items");
            py::dict d = py::cast<py::dict>(obj_);
            return std::make_unique<PythonValidatedDict>(d);
        } catch (...) {}
    }
    
    return ValError::line_error(PydanticKnownError::dict_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<std::unique_ptr<ValidatedList>> PythonInput::validate_list(bool strict) {
    if (py::isinstance<py::list>(obj_)) {
        py::list l = py::cast<py::list>(obj_);
        return ValMatch::exact(std::make_unique<PythonValidatedList>(l));
    }
    
    // Lax mode - try sequence conversion
    if (!strict) {
        if (py::isinstance<py::tuple>(obj_) || py::isinstance<py::set>(obj_)) {
            py::list l = py::list(obj_);
            return ValMatch::lax(std::make_unique<PythonValidatedList>(l));
        }
    }
    
    return ValError::line_error(PydanticKnownError::list_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<std::unique_ptr<ValidatedTuple>> PythonInput::validate_tuple(bool strict) {
    if (py::isinstance<py::tuple>(obj_)) {
        py::tuple t = py::cast<py::tuple>(obj_);
        return ValMatch::exact(std::make_unique<PythonValidatedTuple>(t));
    }
    
    // Lax mode - convert list to tuple
    if (!strict) {
        if (py::isinstance<py::list>(obj_)) {
            py::list l = py::cast<py::list>(obj_);
            py::tuple t = py::tuple(l);
            return ValMatch::lax(std::make_unique<PythonValidatedTuple>(t));
        }
    }
    
    return ValError::line_error(PydanticKnownError::tuple_type(),
                               Location(), as_error_value().repr);
}

// PythonValidatedDict implementation
std::vector<ValidatedDict::Entry> PythonValidatedDict::entries() const {
    std::vector<Entry> result;
    for (auto& [key, value] : dict_) {
        Entry e;
        e.key = py::cast<std::string>(key);
        e.value_repr = InputValue(py::repr(value));
        result.push_back(e);
    }
    return result;
}

std::vector<std::string> PythonValidatedDict::keys() const {
    std::vector<std::string> result;
    for (auto& key : dict_.attr("keys")()) {
        result.push_back(py::cast<std::string>(key));
    }
    return result;
}

bool PythonValidatedDict::has_key(const std::string& key) const {
    return dict_.contains(key.c_str());
}

std::optional<ValidatedDict::Entry> PythonValidatedDict::get(const std::string& key) const {
    if (has_key(key)) {
        py::object value = dict_[key.c_str()];
        Entry e;
        e.key = key;
        e.value_repr = InputValue(py::repr(value));
        return e;
    }
    return std::nullopt;
}

// PythonValidatedList implementation
std::vector<ValidatedList::Entry> PythonValidatedList::entries() const {
    std::vector<Entry> result;
    size_t idx = 0;
    for (auto& item : list_) {
        Entry e;
        e.index = idx++;
        e.value_repr = InputValue(py::repr(item));
        result.push_back(e);
    }
    return result;
}

// PythonValidatedTuple implementation
std::vector<ValidatedList::Entry> PythonValidatedTuple::entries() const {
    std::vector<Entry> result;
    size_t idx = 0;
    for (auto& item : tuple_) {
        Entry e;
        e.index = idx++;
        e.value_repr = InputValue(py::repr(item));
        result.push_back(e);
    }
    return result;
}

} // namespace pydantic_core