#include "pydantic_core/python_input.hpp"
#include "pydantic_core/errors.hpp"
#include <string>
#include <vector>
#include <cmath>

namespace py = pybind11;
using namespace pydantic_core;

// ============================================================================
// PythonValidatedDict implementation
// ============================================================================

std::vector<ValidatedDict::Entry> PythonValidatedDict::entries() const {
    std::vector<Entry> result;
    for (auto item : dict_) {
        Entry e;
        e.key = py::str(item.first).cast<std::string>();
        e.value_repr = py::repr(item.second).cast<std::string>();
        result.push_back(std::move(e));
    }
    return result;
}

std::vector<std::string> PythonValidatedDict::keys() const {
    std::vector<std::string> result;
    for (auto item : dict_) {
        result.push_back(py::str(item.first).cast<std::string>());
    }
    return result;
}

bool PythonValidatedDict::has_key(const std::string& key) const {
    return dict_.contains(key.c_str());
}

std::optional<ValidatedDict::Entry> PythonValidatedDict::get(const std::string& key) const {
    try {
        py::object value = dict_[py::str(key)];
        Entry e;
        e.key = key;
        e.value_repr = py::repr(value).cast<std::string>();
        return e;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<py::object> PythonValidatedDict::get_object(const std::string& key) const {
    try {
        return dict_[py::str(key)];
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// PythonValidatedList implementation
// ============================================================================

std::vector<ValidatedList::Entry> PythonValidatedList::entries() const {
    std::vector<Entry> result;
    size_t i = 0;
    for (auto item : seq_) {
        Entry e;
        e.index = i++;
        e.value_repr = py::repr(py::reinterpret_borrow<py::object>(item)).cast<std::string>();
        result.push_back(std::move(e));
    }
    return result;
}

py::object PythonValidatedList::get_item(size_t index) const {
    return py::reinterpret_borrow<py::object>(seq_[index]);
}

// ============================================================================
// PythonValidatedTuple implementation
// ============================================================================

std::vector<ValidatedTuple::Entry> PythonValidatedTuple::entries() const {
    std::vector<Entry> result;
    size_t i = 0;
    for (auto item : tuple_) {
        Entry e;
        e.index = i++;
        e.value_repr = py::repr(py::reinterpret_borrow<py::object>(item)).cast<std::string>();
        result.push_back(std::move(e));
    }
    return result;
}

py::object PythonValidatedTuple::get_item(size_t index) const {
    return py::reinterpret_borrow<py::object>(tuple_[index]);
}

// ============================================================================
// PythonInput implementation
// ============================================================================

InputValue PythonInput::as_error_value() const {
    InputValue value;
    try {
        value.repr = py::repr(obj_).cast<std::string>();
    } catch (...) {
        value.repr = "<repr failed>";
    }
    return value;
}

bool PythonInput::is_none() const {
    return obj_.is_none();
}

// Type detection
bool PythonInput::is_bool() const {
    return py::isinstance<py::bool_>(obj_);
}

bool PythonInput::is_int() const {
    return py::isinstance<py::int_>(obj_) && !is_bool();
}

bool PythonInput::is_float() const {
    return py::isinstance<py::float_>(obj_);
}

bool PythonInput::is_str() const {
    return py::isinstance<py::str>(obj_);
}

bool PythonInput::is_bytes() const {
    return py::isinstance<py::bytes>(obj_);
}

bool PythonInput::is_dict() const {
    return py::isinstance<py::dict>(obj_);
}

bool PythonInput::is_list() const {
    return py::isinstance<py::list>(obj_);
}

bool PythonInput::is_tuple() const {
    return py::isinstance<py::tuple>(obj_);
}

bool PythonInput::is_set() const {
    return py::isinstance<py::set>(obj_);
}

bool PythonInput::is_frozenset() const {
    return py::isinstance<py::frozenset>(obj_);
}

bool PythonInput::is_sequence() const {
    return is_list() || is_tuple() || py::hasattr(obj_, "__iter__");
}

// Special type detection
bool PythonInput::is_datetime() const {
    return is_instance_of("datetime", "datetime");
}

bool PythonInput::is_date() const {
    return is_instance_of("datetime", "date");
}

bool PythonInput::is_time() const {
    return is_instance_of("datetime", "time");
}

bool PythonInput::is_timedelta() const {
    return is_instance_of("datetime", "timedelta");
}

bool PythonInput::is_uuid() const {
    return is_instance_of("uuid", "UUID");
}

bool PythonInput::is_decimal() const {
    return is_instance_of("decimal", "Decimal");
}

bool PythonInput::is_complex() const {
    return py::hasattr(obj_, "real") && py::hasattr(obj_, "imag");
}

bool PythonInput::is_callable() const {
    return py::hasattr(obj_, "__call__");
}

// Value extraction
std::string PythonInput::as_str() const {
    return py::str(obj_).cast<std::string>();
}

int64_t PythonInput::as_int() const {
    return obj_.cast<int64_t>();
}

double PythonInput::as_float() const {
    return obj_.cast<double>();
}

std::vector<uint8_t> PythonInput::as_bytes() const {
    py::bytes b = obj_.cast<py::bytes>();
    std::string s = b;
    return std::vector<uint8_t>(s.begin(), s.end());
}

py::dict PythonInput::as_dict() const {
    return obj_.cast<py::dict>();
}

py::list PythonInput::as_list() const {
    return obj_.cast<py::list>();
}

py::sequence PythonInput::as_sequence() const {
    return obj_.cast<py::sequence>();
}

// Helper implementations
std::string PythonInput::type_name() const {
    try {
        py::handle type = obj_.get_type();
        return py::str(type).cast<std::string>();
    } catch (...) {
        return "<unknown>";
    }
}

bool PythonInput::is_instance_of(const char* module, const char* type_name) const {
    try {
        py::object mod = py::module_::import(module);
        py::object type = mod.attr(type_name);
        return py::isinstance(obj_, type);
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Type validation implementations
// ============================================================================

ValResult<ValMatch<EitherString>> PythonInput::validate_str(bool strict, bool coerce_numbers) const {
    if (is_str()) {
        return ValMatch<EitherString>::exact(EitherString(as_str()));
    }

    if (!strict) {
        if (is_bytes()) {
            try {
                return ValMatch<EitherString>::lax(EitherString(as_str()));
            } catch (...) {
                return type_error(ErrorType::Kind::StringType, *this);
            }
        }

        if (coerce_numbers && (is_int() || is_float())) {
            return ValMatch<EitherString>::lax(EitherString(py::str(obj_).cast<std::string>()));
        }

        try {
            return ValMatch<EitherString>::lax(EitherString(as_str()));
        } catch (...) {}
    }

    return type_error(ErrorType::Kind::StringType, *this);
}

ValResult<ValMatch<EitherBytes>> PythonInput::validate_bytes(bool strict) const {
    if (is_bytes()) {
        return ValMatch<EitherBytes>::exact(EitherBytes(as_bytes()));
    }

    if (!strict && is_str()) {
        std::string s = as_str();
        return ValMatch<EitherBytes>::lax(
            EitherBytes(std::vector<uint8_t>(s.begin(), s.end()))
        );
    }

    return type_error(ErrorType::Kind::BytesType, *this);
}

ValResult<ValMatch<bool>> PythonInput::validate_bool(bool strict) const {
    if (is_bool()) {
        return ValMatch<bool>::exact(obj_.cast<bool>());
    }

    if (!strict) {
        if (is_int()) {
            int64_t v = as_int();
            return ValMatch<bool>::lax(v != 0);
        }

        if (is_str()) {
            std::string s = as_str();
            if (s == "true" || s == "1" || s == "True") {
                return ValMatch<bool>::lax(true);
            }
            if (s == "false" || s == "0" || s == "False") {
                return ValMatch<bool>::lax(false);
            }
        }
    }

    return type_error(ErrorType::Kind::BoolType, *this);
}

ValResult<ValMatch<EitherInt>> PythonInput::validate_int(bool strict) const {
    if (is_int()) {
        try {
            int64_t v = as_int();
            return ValMatch<EitherInt>::exact(EitherInt(v));
        } catch (...) {
            try {
                uint64_t v = obj_.cast<uint64_t>();
                return ValMatch<EitherInt>::exact(EitherInt(v));
            } catch (...) {
                return type_error(ErrorType::Kind::IntType, *this);
            }
        }
    }

    if (!strict) {
        if (is_float()) {
            double v = as_float();
            if (std::floor(v) == v && !std::isinf(v) && !std::isnan(v)) {
                int64_t iv = static_cast<int64_t>(v);
                return ValMatch<EitherInt>::lax(EitherInt(iv));
            }
            return type_error(ErrorType::Kind::IntType, *this);
        }

        if (is_str()) {
            try {
                std::string s = as_str();
                int64_t v = std::stoll(s);
                return ValMatch<EitherInt>::lax(EitherInt(v));
            } catch (...) {
                return type_error(ErrorType::Kind::IntType, *this);
            }
        }

        if (is_bool()) {
            int64_t v = obj_.cast<bool>() ? 1 : 0;
            return ValMatch<EitherInt>::lax(EitherInt(v));
        }
    }

    return type_error(ErrorType::Kind::IntType, *this);
}

ValResult<ValMatch<EitherFloat>> PythonInput::validate_float(bool strict) const {
    if (is_float()) {
        return ValMatch<EitherFloat>::exact(EitherFloat(as_float()));
    }

    if (!strict) {
        if (is_int()) {
            return ValMatch<EitherFloat>::lax(EitherFloat(static_cast<double>(as_int())));
        }

        if (is_str()) {
            try {
                std::string s = as_str();
                double v = std::stod(s);
                return ValMatch<EitherFloat>::lax(EitherFloat(v));
            } catch (...) {
                return type_error(ErrorType::Kind::FloatType, *this);
            }
        }

        if (is_bool()) {
            return ValMatch<EitherFloat>::lax(EitherFloat(obj_.cast<bool>() ? 1.0 : 0.0));
        }
    }

    return type_error(ErrorType::Kind::FloatType, *this);
}

ValResult<std::unique_ptr<ValidatedDict>> PythonInput::validate_dict(bool strict) const {
    if (is_dict()) {
        auto dict = as_dict();
        std::unique_ptr<ValidatedDict> result = std::make_unique<PythonValidatedDict>(dict);
        return result;
    }

    if (!strict && py::hasattr(obj_, "__dict__")) {
        auto dict = obj_.attr("__dict__").cast<py::dict>();
        std::unique_ptr<ValidatedDict> result = std::make_unique<PythonValidatedDict>(dict);
        return result;
    }

    return type_error(ErrorType::Kind::DictType, *this);
}

ValResult<std::unique_ptr<ValidatedDict>> PythonInput::validate_dict_from_attributes(bool strict) const {
    // First try as dict
    if (is_dict()) {
        return validate_dict(strict);
    }
    
    // Then try to get attributes from object
    py::dict attrs_dict = get_attributes_as_dict();
    if (!attrs_dict.empty()) {
        return ValResult<std::unique_ptr<ValidatedDict>>(
            std::make_unique<PythonValidatedDict>(attrs_dict)
        );
    }
    
    // Try __dict__ attribute
    if (py::hasattr(obj_, "__dict__")) {
        try {
            py::dict d = obj_.attr("__dict__").cast<py::dict>();
            return ValResult<std::unique_ptr<ValidatedDict>>(
                std::make_unique<PythonValidatedDict>(d)
            );
        } catch (...) {}
    }
    
    return type_error(ErrorType::Kind::DictType, *this);
}

// from_attributes support methods
bool PythonInput::has_attributes() const {
    // Check if object has __dict__ or is not a built-in type
    if (py::hasattr(obj_, "__dict__")) {
        return true;
    }
    // Check if object has_slots (slots objects can have attributes too)
    if (py::hasattr(obj_, "__slots__")) {
        return true;
    }
    // Use dir() to check for attributes beyond built-in methods
    try {
        py::list attrs = obj_.attr("__dir__")().cast<py::list>();
        for (auto attr : attrs) {
            std::string name = py::str(attr).cast<std::string>();
            // Skip private/dunder attributes and methods
            if (name.size() > 2 && name.substr(0, 2) == "__" && name.substr(name.size()-2) == "__") {
                continue;
            }
            if (name.size() > 1 && name[0] == '_') {
                continue;
            }
            // Check if it's a property or attribute (not a bound method)
            try {
                py::object value = obj_.attr(name.c_str());
                if (!py::hasattr(value, "__call__") || py::hasattr(value, "__self__")) {
                    // It's a property or data attribute, not a method
                    return true;
                }
            } catch (...) {}
        }
    } catch (...) {}
    return false;
}

bool PythonInput::is_dict_like() const {
    return is_dict() || has_attributes();
}

py::dict PythonInput::get_attributes_as_dict() const {
    py::dict result;
    
    // First, try __dict__ if it exists
    if (py::hasattr(obj_, "__dict__")) {
        try {
            py::dict d = obj_.attr("__dict__").cast<py::dict>();
            for (auto item : d) {
                std::string key = py::str(item.first).cast<std::string>();
                // Skip private attributes
                if (key.size() > 0 && key[0] == '_') {
                    continue;
                }
                result[item.first] = item.second;
            }
        } catch (...) {}
    }
    
    // Then, check for slots-defined attributes
    if (py::hasattr(obj_, "__slots__")) {
        try {
            py::object slots = obj_.attr("__slots__");
            if (py::isinstance<py::str>(slots)) {
                std::string slot_name = slots.cast<std::string>();
                if (slot_name.size() > 0 && slot_name[0] != '_') {
                    try {
                        result[py::str(slot_name)] = obj_.attr(slot_name.c_str());
                    } catch (...) {}
                }
            } else {
                py::sequence slot_seq = slots.cast<py::sequence>();
                for (auto slot : slot_seq) {
                    std::string slot_name = py::str(slot).cast<std::string>();
                    if (slot_name.size() > 0 && slot_name[0] != '_') {
                        try {
                            result[py::str(slot_name)] = obj_.attr(slot_name.c_str());
                        } catch (...) {}
                    }
                }
            }
        } catch (...) {}
    }
    
    // Finally, iterate over dir() for property-like attributes
    try {
        py::list attrs = obj_.attr("__dir__")().cast<py::list>();
        for (auto attr : attrs) {
            std::string name = py::str(attr).cast<std::string>();
            // Skip private/dunder attributes
            if (name.size() > 2 && name.substr(0, 2) == "__") {
                continue;
            }
            if (name.size() > 0 && name[0] == '_') {
                continue;
            }
            // Skip if already in result
            if (result.contains(attr)) {
                continue;
            }
            // Get the attribute
            try {
                py::object value = obj_.attr(name.c_str());
                // Skip bound methods (but allow properties which might have __call__)
                if (py::hasattr(value, "__self__") && py::hasattr(value, "__func__")) {
                    continue;
                }
                // Include the attribute
                result[attr] = value;
            } catch (...) {}
        }
    } catch (...) {}
    
    return result;
}

ValResult<ValMatch<std::unique_ptr<ValidatedList>>> PythonInput::validate_list(bool strict) const {
    if (is_list()) {
        return ValMatch<std::unique_ptr<ValidatedList>>::lax(
            std::make_unique<PythonValidatedList>(as_list())
        );
    }

    return type_error(ErrorType::Kind::ListType, *this);
}

ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> PythonInput::validate_tuple(bool strict) const {
    if (is_tuple()) {
        return ValMatch<std::unique_ptr<ValidatedTuple>>::lax(
            std::make_unique<PythonValidatedTuple>(obj_.cast<py::tuple>())
        );
    }

    return type_error(ErrorType::Kind::TupleType, *this);
}
