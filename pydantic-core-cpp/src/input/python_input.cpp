#include "pydantic_core/python_input.hpp"
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
                return type_error(ErrorType::Kind::StringType, *this, this->current_location());
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
    if (is_bytes()) {
        return ValMatch<EitherBytes>::exact(EitherBytes(as_bytes()));
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

        if (is_str()) {
            try {
                std::string s = as_str();
                int64_t v = std::stoll(s);
                return ValMatch<EitherInt>::lax(EitherInt(v));
            } catch (...) {
                return type_error(ErrorType::Kind::IntParsing, *this, this->current_location());
            }
        }

        if (is_bool()) {
            int64_t v = obj_.cast<bool>() ? 1 : 0;
            return ValMatch<EitherInt>::lax(EitherInt(v));
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

        if (is_str()) {
            try {
                std::string s = as_str();
                double v = std::stod(s);
                return ValMatch<EitherFloat>::lax(EitherFloat(v));
            } catch (...) {
                return type_error(ErrorType::Kind::FloatParsing, *this, this->current_location());
            }
        }

        if (is_bool()) {
            return ValMatch<EitherFloat>::lax(EitherFloat(obj_.cast<bool>() ? 1.0 : 0.0));
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
    if (!strict && py::hasattr(obj_, "__pydantic_validator__") && py::hasattr(obj_, "__dict__")) {
        auto dict = obj_.attr("__dict__").cast<py::dict>();
        std::unique_ptr<ValidatedDict> result = std::make_unique<PythonValidatedDict>(dict);
        return result;
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
    if (py::hasattr(obj_, "__dict__")) {
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
    if (py::hasattr(obj_, "args") && py::hasattr(obj_, "kwargs")) {
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

    return type_error(ErrorType::Kind::ArgumentsType, *this, this->current_location());
}

bool PythonInput::is_args_kwargs() const {
    if (!py::hasattr(obj_, "args") || !py::hasattr(obj_, "kwargs")) return false;
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
            // Check if it's a property or attribute (not a bound method)
            try {
                py::object value = obj_.attr(name.c_str());
                // Skip bound methods (have __self__)
                if (py::hasattr(value, "__self__")) {
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
    if (py::hasattr(obj_, "__dict__")) {
        try {
            py::dict d = obj_.attr("__dict__").cast<py::dict>();
            for (auto item : d) {
                std::string key = py::str(item.first).cast<std::string>();
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

    // Lax mode: any iterable other than str/bytes/dict-like coerces to a
    // list (Rust accepts arbitrary iterables, rejecting only text types).
    if (!strict && !py::isinstance<py::str>(obj_) && !py::isinstance<py::bytes>(obj_) &&
        !py::isinstance<py::bytearray>(obj_) && !py::isinstance<py::dict>(obj_) &&
        py::hasattr(obj_, "__iter__")) {
        try {
            py::list items = py::list(obj_);
            return ValMatch<std::unique_ptr<ValidatedList>>::lax(
                std::make_unique<PythonValidatedList>(items)
            );
        } catch (const py::error_already_set&) {
            PyErr_Clear();
        }
    }

    return type_error(ErrorType::Kind::ListType, *this, this->current_location());
}

ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> PythonInput::validate_tuple(bool strict) const {
    if (is_tuple()) {
        return ValMatch<std::unique_ptr<ValidatedTuple>>::lax(
            std::make_unique<PythonValidatedTuple>(obj_.cast<py::tuple>())
        );
    }

    if (!strict && is_list()) {
        py::tuple tup = py::tuple(obj_);
        return ValMatch<std::unique_ptr<ValidatedTuple>>::lax(
            std::make_unique<PythonValidatedTuple>(tup)
        );
    }

    return type_error(ErrorType::Kind::TupleType, *this, this->current_location());
}

// ============================================================================
// ISO 8601 parsing helpers
// ============================================================================

// Parse up to `max_digits` decimal digits starting at `pos`.
// Advances `pos` and sets `out`. Returns false if no digit was consumed.
static bool parse_int_field(const std::string& s, size_t& pos, int max_digits, int& out) {
    if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
    int val = 0;
    int digits = 0;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) && digits < max_digits) {
        val = val * 10 + (s[pos] - '0');
        pos++;
        digits++;
    }
    out = val;
    return true;
}

/// Parse an ISO 8601 datetime string (speedate-compatible subset).
/// Accepts:
///   YYYY-MM-DD
///   YYYY-MM-DD<T|space>HH:MM[:SS[.ffffff]][Z|±HH[:MM[:SS]]]
/// Seconds and the time component are optional (matching speedate/Rust).
static std::optional<DateTime> try_parse_iso8601(const std::string& s) {
    size_t pos = 0;
    // Optional leading '+' (speedate allows it)
    if (pos < s.size() && s[pos] == '+') pos++;

    int year, month, day;
    if (!parse_int_field(s, pos, 4, year)) return std::nullopt;
    if (pos >= s.size() || s[pos] != '-') return std::nullopt;
    pos++;
    if (!parse_int_field(s, pos, 2, month)) return std::nullopt;
    if (pos >= s.size() || s[pos] != '-') return std::nullopt;
    pos++;
    if (!parse_int_field(s, pos, 2, day)) return std::nullopt;

    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;

    int hour = 0, minute = 0, second = 0, microsecond = 0;
    std::optional<int> tz_offset;

    if (pos < s.size()) {
        // Date/time separator: T, t, or space
        char sep = s[pos];
        if (sep != 'T' && sep != 't' && sep != ' ') return std::nullopt;
        pos++;
        if (!parse_int_field(s, pos, 2, hour)) return std::nullopt;
        if (pos >= s.size() || s[pos] != ':') return std::nullopt;
        pos++;
        if (!parse_int_field(s, pos, 2, minute)) return std::nullopt;
        if (hour > 23 || minute > 59) return std::nullopt;

        if (pos < s.size() && s[pos] == ':') {
            pos++;
            if (!parse_int_field(s, pos, 2, second)) return std::nullopt;
            if (second > 59) return std::nullopt;
            // Optional fractional seconds
            if (pos < s.size() && s[pos] == '.') {
                pos++;
                std::string frac;
                while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
                    frac += s[pos];
                    pos++;
                }
                if (frac.empty()) return std::nullopt;
                if (frac.size() > 6) frac = frac.substr(0, 6);
                while (frac.size() < 6) frac += '0';
                microsecond = std::stoi(frac);
            }
        }

        // Optional timezone
        if (pos < s.size()) {
            char c = s[pos];
            if (c == 'Z' || c == 'z') {
                tz_offset = 0;
            } else if (c == '+' || c == '-') {
                char sign = c;
                pos++;
                int tzh, tzm = 0, tzs = 0;
                if (!parse_int_field(s, pos, 2, tzh)) return std::nullopt;
                if (pos < s.size() && s[pos] == ':') pos++;
                if (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
                    if (!parse_int_field(s, pos, 2, tzm)) return std::nullopt;
                    if (pos < s.size() && s[pos] == ':') {
                        pos++;
                        if (!parse_int_field(s, pos, 2, tzs)) return std::nullopt;
                    }
                }
                // C++ convention: tz_offset is in MINUTES (consumers build
                // timedelta(minutes=...)), matching the rest of this codebase.
                int offset = tzh * 60 + tzm + tzs / 60;
                tz_offset = (sign == '-') ? -offset : offset;
            } else {
                return std::nullopt;
            }
        }
    }

    if (pos != s.size()) return std::nullopt;

    return DateTime{Date{year, month, day}, Time{hour, minute, second, microsecond, tz_offset}};
}

// ============================================================================
// Date/time validation
// ============================================================================

ValResult<ValMatch<EitherDate>> PythonInput::validate_date(bool strict) const {
    if (is_date()) {
        // Extract year/month/day from a Python datetime.date object
        py::object py_date = obj_;
        int year = py_date.attr("year").cast<int>();
        int month = py_date.attr("month").cast<int>();
        int day = py_date.attr("day").cast<int>();
        return ValMatch<EitherDate>::exact(EitherDate(Date{year, month, day}));
    }

    if (is_datetime() && !strict) {
        // Lax mode: extract date part from datetime
        py::object py_dt = obj_;
        int year = py_dt.attr("year").cast<int>();
        int month = py_dt.attr("month").cast<int>();
        int day = py_dt.attr("day").cast<int>();
        int hour = py_dt.attr("hour").cast<int>();
        int minute = py_dt.attr("minute").cast<int>();
        int second = py_dt.attr("second").cast<int>();
        int microsecond = py_dt.attr("microsecond").cast<int>();

        // Check if time component is zero (midnight)
        if (hour == 0 && minute == 0 && second == 0 && microsecond == 0) {
            return ValMatch<EitherDate>::lax(EitherDate(Date{year, month, day}));
        }
        // Non-zero time component
        return ValError::line_error(
            ErrorType(ErrorType::Kind::DateFromDatetimeInexact),
            this->current_location(),
            this->as_error_value().repr
        );
    }

    if (is_str() && !strict) {
        // Lax mode: try to parse ISO 8601 date string
        std::string s = as_str();
        try {
            // Parse YYYY-MM-DD
            if (s.size() >= 10 && s[4] == '-' && s[7] == '-') {
                int year = std::stoi(s.substr(0, 4));
                int month = std::stoi(s.substr(5, 2));
                int day = std::stoi(s.substr(8, 2));
                // Validate month/day ranges
                if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
                    // Check for trailing time component
                    if (s.size() > 10) {
                        // Has time component - validate it's midnight
                        if (s.size() >= 19) {
                            int hour = std::stoi(s.substr(11, 2));
                            int min = std::stoi(s.substr(14, 2));
                            int sec = std::stoi(s.substr(17, 2));
                            if (hour == 0 && min == 0 && sec == 0) {
                                return ValMatch<EitherDate>::lax(EitherDate(Date{year, month, day}));
                            }
                        }
                        // Try full datetime parsing, then check it's exact date
                        // Try to parse as full ISO datetime
                        auto dt_result = try_parse_iso8601(s);
                        if (dt_result) {
                            auto& dt = *dt_result;
                            if (dt.time.hour == 0 && dt.time.minute == 0 &&
                                dt.time.second == 0 && dt.time.microsecond == 0) {
                                return ValMatch<EitherDate>::lax(EitherDate(dt.date));
                            }
                            return ValError::line_error(
                                ErrorType(ErrorType::Kind::DateFromDatetimeInexact),
                                this->current_location(),
                                this->as_error_value().repr
                            );
                        }
                    }
                    return ValMatch<EitherDate>::lax(EitherDate(Date{year, month, day}));
                }
            }
        } catch (...) {}
        return ValError::line_error(
            ErrorType(ErrorType::Kind::DateParsing),
            this->current_location(),
            this->as_error_value().repr
        );
    }

    return type_error(ErrorType::Kind::DateType, *this, this->current_location());
}

ValResult<ValMatch<EitherDateTime>> PythonInput::validate_datetime(bool strict) const {
    if (is_datetime()) {
        // Extract from Python datetime.datetime object
        py::object py_dt = obj_;
        int year = py_dt.attr("year").cast<int>();
        int month = py_dt.attr("month").cast<int>();
        int day = py_dt.attr("day").cast<int>();
        int hour = py_dt.attr("hour").cast<int>();
        int minute = py_dt.attr("minute").cast<int>();
        int second = py_dt.attr("second").cast<int>();
        int microsecond = 0;
        if (py::hasattr(py_dt, "microsecond")) {
            microsecond = py_dt.attr("microsecond").cast<int>();
        }

        // Extract timezone offset if available
        std::optional<int> tz_offset;
        if (py::hasattr(py_dt, "tzinfo") && !py_dt.attr("tzinfo").is_none()) {
            py::object tzinfo = py_dt.attr("tzinfo");
            if (py::hasattr(tzinfo, "utcoffset")) {
                py::object offset = tzinfo.attr("utcoffset")(py_dt);
                if (!offset.is_none()) {
                    // Convert timedelta to minutes
                    tz_offset = static_cast<int>(offset.attr("total_seconds")().cast<double>() / 60);
                }
            }
        }

        DateTime dt = {Date{year, month, day}, Time{hour, minute, second, microsecond, tz_offset}};
        return ValMatch<EitherDateTime>::exact(EitherDateTime(dt));
    }

    if (is_date() && !strict) {
        // Lax mode: extend date with time 00:00:00
        py::object py_date = obj_;
        int year = py_date.attr("year").cast<int>();
        int month = py_date.attr("month").cast<int>();
        int day = py_date.attr("day").cast<int>();
        DateTime dt = {Date{year, month, day}, Time{0, 0, 0, 0, std::nullopt}};
        return ValMatch<EitherDateTime>::lax(EitherDateTime(dt));
    }

    if ((is_int() || is_float()) && !strict) {
        // Lax mode: unix timestamp -> UTC datetime
        double ts = is_int() ? static_cast<double>(as_int()) : as_float();
        try {
            py::object dt_mod = py::module_::import("datetime");
            py::object utc = dt_mod.attr("timezone").attr("utc");
            py::object py_dt = dt_mod.attr("datetime").attr("fromtimestamp")(ts, utc);
            int year = py_dt.attr("year").cast<int>();
            int month = py_dt.attr("month").cast<int>();
            int day = py_dt.attr("day").cast<int>();
            int hour = py_dt.attr("hour").cast<int>();
            int minute = py_dt.attr("minute").cast<int>();
            int second = py_dt.attr("second").cast<int>();
            int microsecond = py_dt.attr("microsecond").cast<int>();
            DateTime dt = {Date{year, month, day}, Time{hour, minute, second, microsecond, 0}};
            return ValMatch<EitherDateTime>::lax(EitherDateTime(dt));
        } catch (py::error_already_set&) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateTimeParsing),
                this->current_location(),
                this->as_error_value().repr
            );
        }
    }

    if (is_str() && !strict) {
        // Lax mode: try to parse ISO 8601 datetime string
        std::string s = as_str();
        auto parsed = try_parse_iso8601(s);
        if (parsed) {
            return ValMatch<EitherDateTime>::lax(EitherDateTime(*parsed));
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::DateTimeParsing),
            this->current_location(),
            this->as_error_value().repr
        );
    }

    return type_error(ErrorType::Kind::DateTimeType, *this, this->current_location());
}

ValResult<ValMatch<EitherTime>> PythonInput::validate_time(bool strict) const {
    if (is_time()) {
        // Extract from Python datetime.time object
        py::object py_time = obj_;
        int hour = py_time.attr("hour").cast<int>();
        int minute = py_time.attr("minute").cast<int>();
        int second = py_time.attr("second").cast<int>();
        int microsecond = 0;
        if (py::hasattr(py_time, "microsecond")) {
            microsecond = py_time.attr("microsecond").cast<int>();
        }
        std::optional<int> tz_offset;
        if (py::hasattr(py_time, "tzinfo") && !py_time.attr("tzinfo").is_none()) {
            py::object tzinfo = py_time.attr("tzinfo");
            if (py::hasattr(tzinfo, "utcoffset")) {
                py::object offset = tzinfo.attr("utcoffset")(py_time);
                if (!offset.is_none()) {
                    tz_offset = static_cast<int>(offset.attr("total_seconds")().cast<double>() / 60);
                }
            }
        }
        return ValMatch<EitherTime>::exact(EitherTime(Time{hour, minute, second, microsecond, tz_offset}));
    }

    if (is_str() && !strict) {
        // Lax mode: try to parse ISO 8601 time string (HH:MM:SS or HH:MM:SS.mmmmmm)
        std::string s = as_str();
        try {
            // Parse HH:MM:SS[.microseconds]
            if (s.size() >= 8 && s[2] == ':' && s[5] == ':') {
                int hour = std::stoi(s.substr(0, 2));
                int minute = std::stoi(s.substr(3, 2));
                int second = std::stoi(s.substr(6, 2));
                int microsecond = 0;
                if (s.size() > 8 && s[8] == '.') {
                    std::string frac = s.substr(9);
                    // Pad or truncate to 6 digits
                    if (frac.size() > 6) frac = frac.substr(0, 6);
                    while (frac.size() < 6) frac += '0';
                    microsecond = std::stoi(frac);
                }
                return ValMatch<EitherTime>::lax(EitherTime(Time{hour, minute, second, microsecond, std::nullopt}));
            }
        } catch (...) {}
        return ValError::line_error(
            ErrorType(ErrorType::Kind::TimeParsing),
            this->current_location(),
            this->as_error_value().repr
        );
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

    if (is_float() && !strict) {
        // Lax mode: treat float as number of seconds
        double seconds = py::float_(obj_).cast<double>();
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
        return ValError::line_error(
            ErrorType(ErrorType::Kind::TimedeltaParsing),
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
        return ValError::line_error(
            ErrorType(ErrorType::Kind::TimedeltaParsing),
            this->current_location(),
            this->as_error_value().repr
        );
    }

    return type_error(ErrorType::Kind::TimedeltaType, *this, this->current_location());
}
