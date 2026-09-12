#include "pydantic_core/python_input.hpp"
#include "pydantic_core/py_compat.hpp"
#include "pydantic_core/errors.hpp"
#include <memory>
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

// Match keys by string representation so non-string keys (int, etc.) work too
std::optional<py::object> PythonValidatedDict::get_value(const std::string& key) const {
    for (auto item : dict_) {
        py::object k = py::reinterpret_borrow<py::object>(item.first);
        if (py::str(k).cast<std::string>() == key) {
            return py::reinterpret_borrow<py::object>(item.second);
        }
    }
    return std::nullopt;
}

std::optional<py::object> PythonValidatedDict::get_key(const std::string& key) const {
    for (auto item : dict_) {
        py::object k = py::reinterpret_borrow<py::object>(item.first);
        if (py::str(k).cast<std::string>() == key) {
            return k;
        }
    }
    return std::nullopt;
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
    return py::isinstance<py::bytes>(obj_) || py::isinstance(obj_, py::module_::import("builtins").attr("bytearray"));
}

bool PythonInput::is_dict() const {
    return py::isinstance<py::dict>(obj_);
}

bool PythonInput::as_float_via_number(double* out) const {
    if (!PyNumber_Check(obj_.ptr())) return false;
    double v = PyFloat_AsDouble(obj_.ptr());
    if (v == -1.0 && PyErr_Occurred()) {
        PyErr_Clear();
        return false;
    }
    *out = v;
    return true;
}

PythonInput::StringSource PythonInput::maybe_as_string(std::string* out) const {
    if (is_str()) {
        *out = as_str();
        return StringSource::Ok;
    }
    if (!py::isinstance<py::bytes>(obj_)) return StringSource::NotString;
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(obj_.ptr(), &buf, &len) < 0) {
        PyErr_Clear();
        return StringSource::BadUtf8;
    }
    PyObject* decoded = PyUnicode_DecodeUTF8(buf, len, nullptr);
    if (!decoded) {
        PyErr_Clear();
        return StringSource::BadUtf8;
    }
    Py_ssize_t out_len = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(decoded, &out_len);
    if (!utf8) {
        PyErr_Clear();
        Py_DECREF(decoded);
        return StringSource::BadUtf8;
    }
    out->assign(utf8, static_cast<size_t>(out_len));
    Py_DECREF(decoded);
    return StringSource::Ok;
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
    return is_list() || is_tuple() || py_hasattr(obj_, "__iter__");
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
    return py_hasattr(obj_, "real") && py_hasattr(obj_, "imag");
}

bool PythonInput::is_callable() const {
    return py_hasattr(obj_, "__call__");
}

// Value extraction
std::string PythonInput::as_str() const {
    // For Enum members (both str subclass and regular Enum), use .value instead of str()
    // str(Foo.FOO) gives 'Foo.FOO', but we want 'foo'
    // Only apply the .value extraction for actual Enum instances to avoid
    // accessing .value on arbitrary objects (e.g. recursive model references).
    try {
        if (py::isinstance(obj_, py::module_::import("enum").attr("Enum"))) {
            try {
                py::object val_attr = obj_.attr("value");
                if (!val_attr.is_none()) {
                    return py::str(val_attr).cast<std::string>();
                }
            } catch (py::error_already_set&) {
                PyErr_Clear();
            }
        }
    } catch (py::error_already_set&) {
        PyErr_Clear();
    }
    return py::str(obj_).cast<std::string>();
}

int64_t PythonInput::as_int() const {
    return obj_.cast<int64_t>();
}

double PythonInput::as_float() const {
    return obj_.cast<double>();
}

std::vector<uint8_t> PythonInput::as_bytes() const {
    // bytearray and other buffer types must be copied out; py::bytes' caster
    // rejects them outright.
    if (!PyBytes_Check(obj_.ptr())) {
        PyObject* owned = PyBytes_FromObject(obj_.ptr());
        if (owned) {
            std::string s = py::reinterpret_steal<py::bytes>(owned);
            return std::vector<uint8_t>(s.begin(), s.end());
        }
        PyErr_Clear();
    }
    std::string s = obj_.cast<py::bytes>();
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

    if (is_none()) {
        return type_error(ErrorType::Kind::StringType, *this, this->current_location());
    }

    if (!strict) {
        if (is_bytes()) {
            try {
                // Decode bytes/bytearray to string using UTF-8 (matches Rust/pydantic behavior)
                // py::str() on bytes gives repr like "b'a'", we want actual decoding
                // Convert to py::bytes first (handles both bytes and bytearray)
                py::object bytes_obj;
                if (py::isinstance<py::bytes>(obj_)) {
                    bytes_obj = obj_;
                } else {
                    // bytearray - convert to bytes via constructor
                    bytes_obj = py::module_::import("builtins").attr("bytes")(obj_);
                }
                py::str decoded_str = bytes_obj.attr("decode")("utf-8");
                std::string decoded = decoded_str.cast<std::string>();
                return ValMatch<EitherString>::lax(EitherString(decoded));
            } catch (...) {
                PyErr_Clear();
                // Rust reports undecodable bytes as string_unicode, not string_type.
                return type_error(ErrorType::Kind::StringUnicode, *this, this->current_location());
            }
        }

        // Rust: a plain `str` field accepts only str/bytes/bytearray (it
        // does NOT coerce int/float/bool/Decimal — `hash: str` with `1`
        // raises string_type). Number coercion is opt-in via
        // coerce_numbers_to_str (constrained-str).
        if (coerce_numbers && (is_int() || is_float() || is_decimal())) {
            try {
                return ValMatch<EitherString>::lax(EitherString(py::str(obj_).cast<std::string>()));
            } catch (...) {}
        }
    }

    return type_error(ErrorType::Kind::StringType, *this, this->current_location());
}

ValResult<ValMatch<EitherBytes>> PythonInput::validate_bytes(bool strict) const {
    // Rust casts PyBytes exactly and reaches for bytearray/memoryview only in
    // lax mode, so strict bytes must reject a bytearray.
    if (PyBytes_Check(obj_.ptr())) {
        return ValMatch<EitherBytes>::exact(EitherBytes(as_bytes()));
    }

    if (!strict && is_bytes()) {
        return ValMatch<EitherBytes>::lax(EitherBytes(as_bytes()));
    }

    if (!strict && is_str()) {
        std::string s = as_str();
        return ValMatch<EitherBytes>::lax(
            EitherBytes(std::vector<uint8_t>(s.begin(), s.end()))
        );
    }

    return type_error(ErrorType::Kind::BytesType, *this, this->current_location());
}

ValResult<ValMatch<bool>> PythonInput::validate_bool(bool strict) const {
    if (is_bool()) {
        return ValMatch<bool>::exact(obj_.cast<bool>());
    }

    if (!strict) {
        std::string s;
        StringSource str_src = maybe_as_string(&s);
        if (str_src == StringSource::BadUtf8) {
            return ValError::line_error(ErrorType(ErrorType::Kind::BoolParsing),
                                       this->current_location(), as_error_value().repr);
        }
        if (str_src == StringSource::Ok) {
            // Rust shared.rs::str_as_bool token set (case-insensitive, except 0/1)
            std::string lower = s;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (s == "0" || lower == "f" || lower == "n" || lower == "no" ||
                lower == "off" || lower == "false") {
                return ValMatch<bool>::lax(false);
            }
            if (s == "1" || lower == "t" || lower == "y" || lower == "on" ||
                lower == "yes" || lower == "true") {
                return ValMatch<bool>::lax(true);
            }
            return ValError::line_error(ErrorType(ErrorType::Kind::BoolParsing),
                                       this->current_location(), as_error_value().repr);
        }
        if (is_int()) {
            // Rust shared.rs::int_as_bool — only 0/1 are valid bools
            int64_t v = as_int();
            if (v == 0) return ValMatch<bool>::lax(false);
            if (v == 1) return ValMatch<bool>::lax(true);
            return ValError::line_error(ErrorType(ErrorType::Kind::BoolParsing),
                                       this->current_location(), as_error_value().repr);
        }
        double d = 0;
        if (as_float_via_number(&d)) {
            // Rust: float -> integer value -> int_as_bool
            if (std::isfinite(d) && std::floor(d) == d) {
                int64_t v = static_cast<int64_t>(d);
                if (v == 0) return ValMatch<bool>::lax(false);
                if (v == 1) return ValMatch<bool>::lax(true);
            }
            return ValError::line_error(ErrorType(ErrorType::Kind::BoolParsing),
                                       this->current_location(), as_error_value().repr);
        }
    }

    return type_error(ErrorType::Kind::BoolType, *this, this->current_location());
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
                return type_error(ErrorType::Kind::IntType, *this, this->current_location());
            }
        }
    }

    if (!strict) {
        if (is_float()) {
            double v = as_float();
            if (std::isnan(v) || std::isinf(v)) {
                return type_error(ErrorType::Kind::FiniteNumber, *this, this->current_location());
            }
            if (std::floor(v) == v) {
                int64_t iv = static_cast<int64_t>(v);
                return ValMatch<EitherInt>::lax(EitherInt(iv));
            }
            return type_error(ErrorType::Kind::IntFromFloat, *this, this->current_location());
        }

        std::string s;
        StringSource str_src = maybe_as_string(&s);
        if (str_src == StringSource::BadUtf8) {
            return type_error(ErrorType::Kind::IntParsing, *this, this->current_location());
        }
        if (str_src == StringSource::Ok) {
            try {
                std::string cleaned = s;
                // strip leading/trailing whitespace and underscores (Rust clean_int_str)
                size_t start = cleaned.find_first_not_of(" \t\n");
                if (start == std::string::npos) cleaned = "";
                else {
                    size_t end = cleaned.find_last_not_of(" \t\n");
                    cleaned = cleaned.substr(start, end - start + 1);
                }
                cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '_'), cleaned.end());
                if (cleaned.empty()) {
                    return type_error(ErrorType::Kind::IntParsing, *this, this->current_location());
                }
                errno = 0;
                char* endp = nullptr;
                int64_t v = std::strtoll(cleaned.c_str(), &endp, 10);
                if (errno == ERANGE && v > 0) {
                    // Beyond i64 — fall back to Python's arbitrary-precision int
                    // (Rust EitherInt::BigInt via jiter)
                    try {
                        py::object py_int = py::module_::import("builtins").attr("int")(cleaned);
                        return ValMatch<EitherInt>::lax(EitherInt(py_int));
                    } catch (...) {}
                    ErrorType err(ErrorType::Kind::IntParsingSize);
                    return ValError::line_error(err, this->current_location(),
                                                as_error_value().repr);
                }
                if (errno == ERANGE && v < 0) {
                    try {
                        py::object py_int = py::module_::import("builtins").attr("int")(cleaned);
                        return ValMatch<EitherInt>::lax(EitherInt(py_int));
                    } catch (...) {}
                    ErrorType err(ErrorType::Kind::IntParsingSize);
                    return ValError::line_error(err, this->current_location(),
                                                as_error_value().repr);
                }
                if (endp && *endp != '\0') {
                    // Not a plain integer (e.g. "1.5", "12abc") — Rust tries Python int()
                    // for strings with underscores/whitespace; reject otherwise.
                    return type_error(ErrorType::Kind::IntParsing, *this, this->current_location());
                }
                return ValMatch<EitherInt>::lax(EitherInt(v));
            } catch (...) {
                return type_error(ErrorType::Kind::IntParsing, *this, this->current_location());
            }
        }

        if (is_bool()) {
            int64_t v = obj_.cast<bool>() ? 1 : 0;
            return ValMatch<EitherInt>::lax(EitherInt(v));
        }
        // Rust: validate_decimal -> decimal_as_int, fraction_as_int, then
        // extract::<f64>; all three reduce to a truncating float conversion.
        double n = 0;
        if (as_float_via_number(&n)) {
            if (std::isnan(n) || std::isinf(n)) {
                return type_error(ErrorType::Kind::IntParsing, *this, this->current_location());
            }
            double truncated = std::trunc(n);
            if (truncated != n) {
                return type_error(ErrorType::Kind::IntFromFloat, *this, this->current_location());
            }
            return ValMatch<EitherInt>::lax(EitherInt(static_cast<int64_t>(truncated)));
        }
    }

    return type_error(ErrorType::Kind::IntType, *this, this->current_location());
}

ValResult<ValMatch<EitherFloat>> PythonInput::validate_float(bool strict) const {
    if (is_float()) {
        return ValMatch<EitherFloat>::exact(EitherFloat(as_float()));
    }

    if (!strict) {
        if (is_int()) {
            return ValMatch<EitherFloat>::lax(EitherFloat(static_cast<double>(as_int())));
        }

        std::string s;
        StringSource str_src = maybe_as_string(&s);
        if (str_src == StringSource::BadUtf8) {
            return type_error(ErrorType::Kind::FloatParsing, *this, this->current_location());
        }
        if (str_src == StringSource::Ok) {
            try {
                double v = std::stod(s);
                return ValMatch<EitherFloat>::lax(EitherFloat(v));
            } catch (...) {
                return type_error(ErrorType::Kind::FloatParsing, *this, this->current_location());
            }
        }

        if (is_bool()) {
            return ValMatch<EitherFloat>::lax(EitherFloat(obj_.cast<bool>() ? 1.0 : 0.0));
        }
        double n = 0;
        if (as_float_via_number(&n)) {
            return ValMatch<EitherFloat>::lax(EitherFloat(n));
        }
    }

    return type_error(ErrorType::Kind::FloatType, *this, this->current_location());
}

ValResult<std::unique_ptr<ValidatedDict>> PythonInput::validate_dict(bool strict) const {
    if (is_dict()) {
        auto dict = as_dict();
        std::unique_ptr<ValidatedDict> result = std::make_unique<PythonValidatedDict>(dict);
        return result;
    }

    // Accept pydantic model instances for revalidation (they have __pydantic_validator__)
    if (!strict && py_hasattr(obj_, "__pydantic_validator__") && py_hasattr(obj_, "__dict__")) {
        auto dict = obj_.attr("__dict__").cast<py::dict>();
        std::unique_ptr<ValidatedDict> result = std::make_unique<PythonValidatedDict>(dict);
        return result;
    }

    // Lax mode reads the abstract Mapping protocol through .items(); an
    // exception raised by the mapping methods is a mapping_type error carrying
    // that exception as its context (Rust: iterate_mapping_items).
    if (!strict && py_is_mapping_instance(obj_)) {
        try {
            py::dict collected;
            for (py::handle item : obj_.attr("items")()) {
                PyObject* key = nullptr;
                PyObject* value = nullptr;
                if (PyArg_UnpackTuple(item.ptr(), "items", 2, 2, &key, &value) == 0) {
                    PyErr_Clear();
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::MappingType, "error",
                                  "Mapping items must be tuples of (key, value) pairs"),
                        this->current_location(), this->as_error_value().repr);
                }
                collected[py::reinterpret_borrow<py::object>(key)] =
                    py::reinterpret_borrow<py::object>(value);
            }
            std::unique_ptr<ValidatedDict> result = std::make_unique<PythonValidatedDict>(collected);
            return result;
        } catch (const py::error_already_set& err) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::MappingType, "error", py_caught_exception_string(err)),
                this->current_location(), this->as_error_value().repr);
        }
    }

    return type_error(ErrorType::Kind::DictType, *this, this->current_location());
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
    if (py_hasattr(obj_, "__dict__")) {
        try {
            py::dict d = obj_.attr("__dict__").cast<py::dict>();
            return ValResult<std::unique_ptr<ValidatedDict>>(
                std::make_unique<PythonValidatedDict>(d)
            );
        } catch (...) {}
    }
    
    return type_error(ErrorType::Kind::DictType, *this, this->current_location());
}

ValResult<ArgumentsInput> PythonInput::validate_args() const {
    // ArgsKwargs container (pydantic_core.ArgsKwargs): has args + kwargs attributes
    if (py_hasattr(obj_, "args") && py_hasattr(obj_, "kwargs")) {
        try {
            py::object cls = py::getattr(obj_, "__class__");
            std::string cls_name = py::str(py::getattr(cls, "__name__")).cast<std::string>();
            if (cls_name == "ArgsKwargs") {
                py::tuple args = py::cast<py::tuple>(obj_.attr("args"));
                py::dict kwargs = py::cast<py::dict>(obj_.attr("kwargs"));
                return ValResult<ArgumentsInput>(ArgumentsInput{std::move(args), std::move(kwargs)});
            }
        } catch (py::error_already_set&) {
            PyErr_Clear();
        }
    }

    // Plain dict input is treated as kwargs-only
    if (is_dict()) {
        return ValResult<ArgumentsInput>(ArgumentsInput{py::tuple(), as_dict()});
    }

    // Tuple/list input (including namedtuple instances, which are tuple
    // subclasses) is treated as positional args.
    if (py::isinstance<py::tuple>(obj_) || py::isinstance<py::list>(obj_)) {
        py::tuple args;
        if (py::isinstance<py::tuple>(obj_)) {
            args = py::reinterpret_borrow<py::tuple>(obj_);
        } else {
            args = py::cast<py::tuple>(py::tuple(obj_));
        }
        return ValResult<ArgumentsInput>(ArgumentsInput{std::move(args), py::dict()});
    }

    return type_error(ErrorType::Kind::ArgumentsType, *this, this->current_location());
}

bool PythonInput::is_args_kwargs() const {
    if (!py_hasattr(obj_, "args") || !py_hasattr(obj_, "kwargs")) return false;
    try {
        py::object cls = py::getattr(obj_, "__class__");
        std::string cls_name = py::str(py::getattr(cls, "__name__")).cast<std::string>();
        return cls_name == "ArgsKwargs";
    } catch (py::error_already_set&) {
        PyErr_Clear();
        return false;
    }
}

// from_attributes support methods
bool PythonInput::has_attributes() const {
    // Check if object has __dict__ or is not a built-in type
    if (py_hasattr(obj_, "__dict__")) {
        return true;
    }
    // Check if object has_slots (slots objects can have attributes too)
    if (py_hasattr(obj_, "__slots__")) {
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
            // Check if it's a property or attribute (not a bound method)
            try {
                py::object value = obj_.attr(name.c_str());
                // Skip bound methods (have __self__)
                if (py_hasattr(value, "__self__")) {
                    continue;
                }
                // It's a property or data attribute
                return true;
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
    if (py_hasattr(obj_, "__dict__")) {
        try {
            py::dict d = obj_.attr("__dict__").cast<py::dict>();
            for (auto item : d) {
                std::string key = py::str(item.first).cast<std::string>();
                result[item.first] = item.second;
            }
        } catch (...) {}
    }
    
    // Then, check for slots-defined attributes
    if (py_hasattr(obj_, "__slots__")) {
        try {
            py::object slots = obj_.attr("__slots__");
            if (py::isinstance<py::str>(slots)) {
                std::string slot_name = slots.cast<std::string>();
                try {
                    result[py::str(slot_name)] = obj_.attr(slot_name.c_str());
                } catch (...) {}
            } else {
                py::sequence slot_seq = slots.cast<py::sequence>();
                for (auto slot : slot_seq) {
                    std::string slot_name = py::str(slot).cast<std::string>();
                    try {
                        result[py::str(slot_name)] = obj_.attr(slot_name.c_str());
                    } catch (...) {}
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
                if (py_hasattr(value, "__self__") && py_hasattr(value, "__func__")) {
                    continue;
                }
                // Include the attribute
                result[attr] = value;
            } catch (...) {}
        }
    } catch (...) {}
    
    return result;
}

namespace {

// Rust materialises a lax sequence input through the iterator protocol.  A
// value with no working __iter__ is left to the caller, which reports its own
// type error, while an exception raised part-way through iteration becomes
// iteration_error at the failing index (Rust's any_next_error).
struct SequenceItems {
    bool iterable = false;
    py::list items;
    std::string error;
    size_t error_index = 0;
};

SequenceItems collect_sequence_items(py::handle obj) {
    SequenceItems collected;
    PyObject* iter = PyObject_GetIter(obj.ptr());
    if (!iter) {
        // Consumes the TypeError so the caller can report a type error instead.
        py_fetched_exception_string();
        return collected;
    }
    collected.iterable = true;
    size_t index = 0;
    while (true) {
        PyObject* item = PyIter_Next(iter);
        if (!item) {
            if (PyErr_Occurred()) {
                collected.error = py_fetched_exception_string();
                collected.error_index = index;
            }
            break;
        }
        collected.items.append(py::reinterpret_steal<py::object>(item));
        ++index;
    }
    Py_DECREF(iter);
    return collected;
}

// list/set/frozenset share the sequence path (Rust: extract_sequence_iterable)
// and differ only in the type error they report.
ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_sequence_like(
    const PythonInput& input, bool strict, ErrorType::Kind kind) {
    if (kind == ErrorType::Kind::ListType && input.is_list()) {
        return ValMatch<std::unique_ptr<ValidatedList>>::lax(
            std::make_unique<PythonValidatedList>(input.as_list())
        );
    }

    // Rust casts to the concrete set type before the lax branch, so a set input
    // stays valid for a set field even in strict mode.
    if ((kind == ErrorType::Kind::SetType && input.is_set()) ||
        (kind == ErrorType::Kind::FrozenSetType && input.is_frozenset())) {
        SequenceItems collected = collect_sequence_items(input.py_object());
        if (collected.iterable && collected.error.empty()) {
            return ValMatch<std::unique_ptr<ValidatedList>>::lax(
                std::make_unique<PythonValidatedList>(collected.items)
            );
        }
    }

    // Lax mode: any iterable other than str/bytes/dict-like coerces to a
    // list (Rust accepts arbitrary iterables, rejecting only text types).
    const py::object& obj = input.py_object();
    if (!strict && !py::isinstance<py::str>(obj) && !py::isinstance<py::bytes>(obj) &&
        !py::isinstance<py::bytearray>(obj) && !py::isinstance<py::dict>(obj) &&
        py_hasattr(obj, "__iter__")) {
        SequenceItems collected = collect_sequence_items(obj);
        if (!collected.iterable) {
            return type_error(kind, input, input.current_location());
        }
        if (!collected.error.empty()) {
            Location loc = input.current_location();
            loc.items.push_back(static_cast<int64_t>(collected.error_index));
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IterationError, "error", collected.error),
                loc, input.as_error_value().repr);
        }
        return ValMatch<std::unique_ptr<ValidatedList>>::lax(
            std::make_unique<PythonValidatedList>(collected.items)
        );
    }

    return type_error(kind, input, input.current_location());
}

}  // namespace

ValResult<ValMatch<std::unique_ptr<ValidatedList>>> PythonInput::validate_list(bool strict) const {
    return validate_sequence_like(*this, strict, ErrorType::Kind::ListType);
}

ValResult<ValMatch<std::unique_ptr<ValidatedList>>> PythonInput::validate_set(bool strict) const {
    return validate_sequence_like(*this, strict, ErrorType::Kind::SetType);
}

ValResult<ValMatch<std::unique_ptr<ValidatedList>>> PythonInput::validate_frozenset(bool strict) const {
    return validate_sequence_like(*this, strict, ErrorType::Kind::FrozenSetType);
}

ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> PythonInput::validate_tuple(bool strict) const {
    if (is_tuple()) {
        return ValMatch<std::unique_ptr<ValidatedTuple>>::lax(
            std::make_unique<PythonValidatedTuple>(obj_.cast<py::tuple>())
        );
    }

    // Lax mode mirrors Rust's extract_sequence_iterable: list/tuple/set/frozenset
    // plus any other iterable that is not a text or mapping type. PyMapping_Check
    // is true for lists, so concrete sequence types must be accepted first.
    if (!strict && (is_list() || is_set() || is_frozenset())) {
        try {
            py::tuple tup = py::tuple(obj_);
            return ValMatch<std::unique_ptr<ValidatedTuple>>::lax(
                std::make_unique<PythonValidatedTuple>(tup)
            );
        } catch (const py::error_already_set&) {
            PyErr_Clear();
        }
    }
    if (!strict && !py::isinstance<py::str>(obj_) && !py::isinstance<py::bytes>(obj_) &&
        !py::isinstance<py::bytearray>(obj_) && !py::isinstance<py::dict>(obj_) &&
        !PyMapping_Check(obj_.ptr()) && py_hasattr(obj_, "__iter__")) {
        SequenceItems collected = collect_sequence_items(obj_);
        if (!collected.iterable) {
            return type_error(ErrorType::Kind::TupleType, *this, this->current_location());
        }
        if (!collected.error.empty()) {
            Location loc = current_location();
            loc.items.push_back(static_cast<int64_t>(collected.error_index));
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IterationError, "error", collected.error),
                loc, as_error_value().repr);
        }
        py::tuple tup(collected.items.size());
        for (size_t i = 0; i < collected.items.size(); ++i) {
            tup[i] = collected.items[i];
        }
        return ValMatch<std::unique_ptr<ValidatedTuple>>::lax(
            std::make_unique<PythonValidatedTuple>(tup)
        );
    }

    return type_error(ErrorType::Kind::TupleType, *this, this->current_location());
}

// ============================================================================
// Date/time validation
// ============================================================================
//
// Parsing itself lives in the speedate port (speedate.hpp); these methods only
// decide which representation an input carries and which error kind to report.
// The dispatch order and error kinds mirror pydantic-core's input_python.rs.

namespace {

// str and bytes share one code path: pydantic-core feeds bytes to the same
// parser instead of rejecting them.
std::optional<std::string> python_text_as_bytes(const PythonInput& input) {
    if (input.is_str()) return input.as_str();
    if (input.is_bytes()) return py::bytes(input.as_python_object()).cast<std::string>();
    return std::nullopt;
}

// Python ints wider than int64 must fall through to the float path, matching
// pydantic-core's `extract::<i64>()` followed by `extract::<f64>()`.
bool python_int_as_i64(py::handle obj, int64_t& out) {
    try {
        out = obj.cast<int64_t>();
        return true;
    } catch (py::error_already_set&) {
        PyErr_Clear();
    } catch (py::cast_error&) {
        // pybind11 reports an out-of-range Python int as cast_error, not as a
        // raised Python exception.
    }
    return false;
}

}  // namespace

ValResult<ValMatch<EitherDate>> PythonInput::validate_date(bool strict, TimestampUnit unit) const {
    // datetime subclasses date, so it must be ruled out first: a datetime input
    // is a date_type error here, not a silently-truncated date.
    if (is_datetime()) {
        return type_error(ErrorType::Kind::DateType, *this, this->current_location());
    }
    if (is_date()) {
        py::object py_date = obj_;
        int year = py_date.attr("year").cast<int>();
        int month = py_date.attr("month").cast<int>();
        int day = py_date.attr("day").cast<int>();
        return ValMatch<EitherDate>::exact(EitherDate(Date{year, month, day}));
    }
    if (!strict) {
        if (auto text = python_text_as_bytes(*this)) {
            auto parsed = parse_date_bytes(text->data(), text->size(), unit);
            if (parsed.ok) {
                return ValMatch<EitherDate>::lax(EitherDate(parsed.value));
            }
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateParsing, "error", parsed.error),
                this->current_location(),
                this->as_error_value().repr
            );
        }
    }
    return type_error(ErrorType::Kind::DateType, *this, this->current_location());
}

ValResult<ValMatch<EitherDateTime>> PythonInput::validate_datetime(
    bool strict, TimestampUnit unit) const {
    if (is_datetime()) {
        py::object py_dt = obj_;
        int year = py_dt.attr("year").cast<int>();
        int month = py_dt.attr("month").cast<int>();
        int day = py_dt.attr("day").cast<int>();
        int hour = py_dt.attr("hour").cast<int>();
        int minute = py_dt.attr("minute").cast<int>();
        int second = py_dt.attr("second").cast<int>();
        int microsecond = py_dt.attr("microsecond").cast<int>();

        std::optional<int> tz_offset;
        if (py_hasattr(py_dt, "tzinfo") && !py_dt.attr("tzinfo").is_none()) {
            py::object tzinfo = py_dt.attr("tzinfo");
            if (py_hasattr(tzinfo, "utcoffset")) {
                py::object offset = tzinfo.attr("utcoffset")(py_dt);
                if (!offset.is_none()) {
                    tz_offset = static_cast<int>(offset.attr("total_seconds")().cast<double>() / 60);
                }
            }
        }

        DateTime dt = {Date{year, month, day}, Time{hour, minute, second, microsecond, tz_offset}};
        EitherDateTime edt(dt);
        // Preserve the original Python datetime object so its tzinfo (e.g. a
        // named zone) is kept rather than replaced by a fixed UTC offset.
        edt.original_obj = py_dt;
        return ValMatch<EitherDateTime>::exact(std::move(edt));
    }

    if (!strict) {
        if (auto text = python_text_as_bytes(*this)) {
            auto parsed = parse_datetime_bytes(text->data(), text->size(), unit);
            if (parsed.ok) {
                return ValMatch<EitherDateTime>::lax(EitherDateTime(parsed.value));
            }
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateTimeParsing, "error", parsed.error),
                this->current_location(),
                this->as_error_value().repr
            );
        }
        if (is_int()) {
            int64_t timestamp = 0;
            ParseOutcome<DateTime> parsed =
                python_int_as_i64(obj_, timestamp)
                    ? datetime_from_timestamp(timestamp, 0, unit)
                    // Wider than int64: pydantic-core retries as a float, which
                    // reports the same out-of-range documents.
                    : datetime_from_float(obj_.cast<double>(), unit);
            if (parsed.ok) {
                return ValMatch<EitherDateTime>::lax(EitherDateTime(parsed.value));
            }
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateTimeParsing, "error", parsed.error),
                this->current_location(),
                this->as_error_value().repr
            );
        }
        // Rust retries Decimal and Fraction through extract::<f64>().
        double numeric = 0;
        if (is_float() || is_decimal()) {
            if (!as_float_via_number(&numeric)) {
                return type_error(ErrorType::Kind::DateTimeType, *this, this->current_location());
            }
            auto parsed = datetime_from_float(numeric, unit);
            if (parsed.ok) {
                return ValMatch<EitherDateTime>::lax(EitherDateTime(parsed.value));
            }
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateTimeParsing, "error", parsed.error),
                this->current_location(),
                this->as_error_value().repr
            );
        }
        if (is_date()) {
            // date_as_datetime: the date extended to midnight with no tzinfo.
            py::object py_date = obj_;
            int year = py_date.attr("year").cast<int>();
            int month = py_date.attr("month").cast<int>();
            int day = py_date.attr("day").cast<int>();
            DateTime dt = {Date{year, month, day}, Time{0, 0, 0, 0, std::nullopt}};
            return ValMatch<EitherDateTime>::lax(EitherDateTime(dt));
        }
    }

    return type_error(ErrorType::Kind::DateTimeType, *this, this->current_location());
}

ValResult<ValMatch<EitherTime>> PythonInput::validate_time(bool strict) const {
    if (is_time()) {
        py::object py_time = obj_;
        int hour = py_time.attr("hour").cast<int>();
        int minute = py_time.attr("minute").cast<int>();
        int second = py_time.attr("second").cast<int>();
        int microsecond = py_time.attr("microsecond").cast<int>();
        std::optional<int> tz_offset;
        if (py_hasattr(py_time, "tzinfo") && !py_time.attr("tzinfo").is_none()) {
            py::object tzinfo = py_time.attr("tzinfo");
            if (py_hasattr(tzinfo, "utcoffset")) {
                py::object offset = tzinfo.attr("utcoffset")(py_time);
                if (!offset.is_none()) {
                    tz_offset = static_cast<int>(offset.attr("total_seconds")().cast<double>() / 60);
                }
            }
        }
        return ValMatch<EitherTime>::exact(
            EitherTime(Time{hour, minute, second, microsecond, tz_offset}));
    }

    if (!strict) {
        if (auto text = python_text_as_bytes(*this)) {
            auto parsed = parse_time_bytes(text->data(), text->size());
            if (parsed.ok) {
                return ValMatch<EitherTime>::lax(EitherTime(parsed.value));
            }
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TimeParsing, "error", parsed.error),
                this->current_location(),
                this->as_error_value().repr
            );
        }
        if (is_int()) {
            int64_t seconds = 0;
            ParseOutcome<Time> parsed =
                python_int_as_i64(obj_, seconds)
                    ? time_from_timestamp(seconds, 0)
                    // Wider than int64: pydantic-core retries as a float, which
                    // reports the sign document before the magnitude limit.
                    : time_from_float(obj_.cast<double>());
            if (parsed.ok) {
                return ValMatch<EitherTime>::lax(EitherTime(parsed.value));
            }
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TimeParsing, "error", parsed.error),
                this->current_location(),
                this->as_error_value().repr
            );
        }
        // Rust retries Decimal and Fraction through extract::<f64>().
        double numeric = 0;
        if (is_float() || is_decimal()) {
            if (!as_float_via_number(&numeric)) {
                return type_error(ErrorType::Kind::TimeType, *this, this->current_location());
            }
            auto parsed = time_from_float(numeric);
            if (parsed.ok) {
                return ValMatch<EitherTime>::lax(EitherTime(parsed.value));
            }
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TimeParsing, "error", parsed.error),
                this->current_location(),
                this->as_error_value().repr
            );
        }
    }

    return type_error(ErrorType::Kind::TimeType, *this, this->current_location());
}

ValResult<ValMatch<EitherTimedelta>> PythonInput::validate_timedelta(bool strict) const {
    if (is_timedelta()) {
        // Extract from Python datetime.timedelta object
        py::object py_td = obj_;
        int days = py_td.attr("days").cast<int>();
        int seconds = py_td.attr("seconds").cast<int>();
        int microseconds = py_td.attr("microseconds").cast<int>();
        return ValMatch<EitherTimedelta>::exact(EitherTimedelta(Timedelta{days, seconds, microseconds}));
    }

    if (is_int() && !strict) {
        // Lax mode: treat int as number of seconds (Rust behavior)
        long long seconds = py::int_(obj_).cast<long long>();
        long long days = seconds / 86400;
        long long rem = seconds % 86400;
        return ValMatch<EitherTimedelta>::lax(
            EitherTimedelta(Timedelta{static_cast<int>(days), static_cast<int>(rem), 0}));
    }

    // Lax mode: treat the number of seconds as a float; Rust reaches Decimal and
    // Fraction through extract::<f64>().
    double numeric = 0;
    if (!strict && (is_float() || is_decimal()) && as_float_via_number(&numeric)) {
        double seconds = numeric;
        double days_f = seconds / 86400.0;
        long long days = static_cast<long long>(days_f);
        double rem = seconds - days * 86400.0;
        long long whole = static_cast<long long>(rem);
        long long micros = std::llround((rem - whole) * 1e6);
        return ValMatch<EitherTimedelta>::lax(
            EitherTimedelta(Timedelta{static_cast<int>(days), static_cast<int>(whole), static_cast<int>(micros)}));
    }

    if (is_str() && !strict) {
        // Lax mode: try to parse ISO 8601 duration string
        std::string s = as_str();
        auto parsed = try_parse_timedelta_str(s);
        if (parsed) {
            return ValMatch<EitherTimedelta>::lax(EitherTimedelta(*parsed));
        }
        // A bare number (e.g. "30") expects a "day" identifier (e.g. "30d").
        std::string err_msg = "unable to parse string as an ISO 8601 duration";
        if (is_bare_number(s)) err_msg = "\"day\" identifier";
        return ValError::line_error(
            ErrorType(ErrorType::Kind::TimedeltaParsing, "error", err_msg),
            this->current_location(),
            this->as_error_value().repr
        );
    }

    if (is_bytes() && !strict) {
        // Lax mode: decode bytes as UTF-8 and try to parse a duration
        std::string s = py::bytes(obj_).cast<std::string>();
        auto parsed = try_parse_timedelta_str(s);
        if (parsed) {
            return ValMatch<EitherTimedelta>::lax(EitherTimedelta(*parsed));
        }
        std::string err_msg = "unable to parse string as an ISO 8601 duration";
        if (is_bare_number(s)) err_msg = "\"day\" identifier";
        return ValError::line_error(
            ErrorType(ErrorType::Kind::TimedeltaParsing, "error", err_msg),
            this->current_location(),
            this->as_error_value().repr
        );
    }

    return type_error(ErrorType::Kind::TimedeltaType, *this, this->current_location());
}
