#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <unordered_set>

#include "pydantic_core/errors.hpp"
#include "pydantic_core/error_types.hpp"
#include "pydantic_core/types.hpp"
#include "pydantic_core/schema_validator.hpp"
#include "pydantic_core/serialization_config.hpp"
#include "pydantic_core/url_types.hpp"

namespace py = pybind11;
using namespace pydantic_core;

std::string get_version() { return "2.47.0"; }

// ---------------------------------------------------------------------------
// SerializationInfo — Python-visible info object for custom serializer functions
// ---------------------------------------------------------------------------
struct PySerializationInfo {
    bool round_trip;
    std::string mode;
    PySerializationInfo(bool round_trip_, std::string mode_ = "python") : round_trip(round_trip_), mode(std::move(mode_)) {}
};

// ---------------------------------------------------------------------------
// Helper: convert a py::object to JSON string
// Helper: convert Python object to JSON string, handling string input specially
static std::string pyobj_to_json_str(const py::object& obj) {
    // If already a string, assume it's JSON and return directly
    if (py::isinstance<py::str>(obj)) {
        return obj.cast<std::string>();
    }
    // Otherwise convert via json.dumps
    py::object json_mod = py::module_::import("json");
    auto default_fn = py::cpp_function([](py::handle o) -> py::object {
        if (py::hasattr(o, "__dict__")) {
            py::object d = py::getattr(o, "__dict__");
            return d;
        }
        return py::str(py::repr(o));
    });
    return json_mod.attr("dumps")(obj, py::arg("default") = default_fn).cast<std::string>();
}

static py::object json_to_pyobj(const std::string& json_str) {
    py::object json_mod = py::module_::import("json");
    return json_mod.attr("loads")(json_str);
}

static std::optional<bool> pyobj_to_bool(const py::object& obj) {
    if (obj.is_none()) return std::nullopt;
    return obj.cast<bool>();
}

// ---------------------------------------------------------------------------
// Serializer tree node — built from schema
// ---------------------------------------------------------------------------
struct SerNode;
using SerRef = std::shared_ptr<SerNode>;

struct SerNode {
    std::string type;
    std::vector<SerRef> children;
    // For tagged-union: map from tag -> serializer
    std::unordered_map<std::string, SerRef> tagged;
    // For model-fields: map from field_name -> serializer
    std::unordered_map<std::string, SerRef> fields;
    // Fields in declaration order (matches Rust — iterate this, not fields directly)
    std::vector<std::string> field_order;
    // Field aliases: map from field_name -> alias (for serialization)
    std::unordered_map<std::string, std::string> field_aliases;
    // Exclude-if callables: field_name -> Python callable (for serialization)
    std::unordered_map<std::string, py::object> field_exclude_if;
    // Set of computed field names (excluded when round_trip=True)
    std::unordered_set<std::string> computed_fields_;
    // For function serializers
    py::object py_func;
    bool info_arg = false;
    // For default
    py::object default_val;
    bool has_default_val = false;
    // For format
    std::string format_str;

    // Copy content from another node into this one (preserves shared_ptr identity)
    void copy_from(const SerNode& other) {
        type = other.type;
        children = other.children;
        tagged = other.tagged;
        fields = other.fields;
        field_order = other.field_order;
        field_aliases = other.field_aliases;
        field_exclude_if = other.field_exclude_if;
        computed_fields_ = other.computed_fields_;
        py_func = other.py_func;
        info_arg = other.info_arg;
        default_val = other.default_val;
        has_default_val = other.has_default_val;
        format_str = other.format_str;
    }

    py::object to_python(const py::object& value, bool json_mode, bool exc_none, bool round_trip = false,
                         const std::optional<std::unordered_set<std::string>>& include_fields = std::nullopt,
                         const std::optional<std::unordered_set<std::string>>& exclude_fields = std::nullopt,
                         bool by_alias = false,
                         bool exclude_unset = false,
                         bool exclude_defaults = false) const {
        // Type-specific logic
        if (type == "lax-or-strict") {
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "is-instance" || type == "is-subclass") {
            return value;
        }
        if (type == "nullable" || type == "nullable-union") {
            if (value.is_none()) return py::none();
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "union") {
            for (auto& c : children) {
                try { return c->to_python(value, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults); } catch (...) {}
            }
        }
        if (type == "tagged-union" && !tagged.empty()) {
            if (py::isinstance<py::dict>(value)) {
                auto d = value.cast<py::dict>();
                for (auto item : d) {
                    auto k = py::str(item.first);
                    std::string ks = k.cast<std::string>();
                    if (ks == "type" || ks == "discriminator") {
                        std::string tag = py::str(item.second).cast<std::string>();
                        auto it = tagged.find(tag);
                        if (it != tagged.end()) return it->second->to_python(value, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
                        break;
                    }
                }
            }
            for (auto& [t, c] : tagged) {
                try { return c->to_python(value, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults); } catch (...) {}
            }
        }
        if (type == "default" || type == "with-default") {
            if (value.is_none() && has_default_val) return default_val;
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "json") {
            if (round_trip) {
                // Serialize value to JSON string, return as Python string
                std::string json_str;
                if (!children.empty()) {
                    json_str = children[0]->to_json(value, false, -1, round_trip);
                } else {
                    json_str = infer_json(value, false, -1);
                }
                return py::cast(json_str);
            }
            // Non-round-trip: delegate to inner serializer
            if (!children.empty()) {
                return children[0]->to_python(value, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
            }
        }
        if (type == "json-or-python") {
            if (json_mode && !children.empty()) return children[0]->to_python(value, true, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
            if (children.size() > 1) return children[1]->to_python(value, false, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "enum") {
            if (py::hasattr(value, "value")) {
                auto ev = py::getattr(value, "value");
                if (!children.empty()) return children[0]->to_python(ev, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
                return ev;
            }
        }
        if (!fields.empty()) {
            return serialize_fields(value, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        // Delegate model/dataclass/typed-dict to inner serializer
        if ((type == "model" || type == "dataclass" || type == "typed-dict") && !children.empty()) {
            return children[0]->to_python(value, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        if (!py_func.is_none()) {
            if (type == "function-plain") {
                if (info_arg) {
                    PySerializationInfo info(round_trip, json_mode ? "json" : "python");
                    return py_func(value, py::cast(info));
                }
                return py_func(value);
            }
            if (type == "function-after" || type == "function-before" || type == "function-wrap") {
                py::object handler = py::cpp_function([this, value, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults](const py::object& v) -> py::object {
                    if (!children.empty()) return children[0]->to_python(v, json_mode, exc_none, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
                    return v;
                });
                if (type == "function-wrap") {
                    PySerializationInfo info(round_trip, json_mode ? "json" : "python");
                    return py_func(value, handler, py::cast(info));
                }
                try {
                    return py_func(value, handler);
                } catch (...) {
                    PyErr_Clear();
                    try {
                        return py_func(value);
                    } catch (...) {
                        PyErr_Clear();
                        return value;
                    }
                }
            }
        }
        // For list/dict/tuple/containers, serialize children
        if ((type == "list" || type == "set" || type == "frozenset") && !children.empty()) {
            py::list result;
            for (auto item : py::reinterpret_borrow<py::sequence>(value)) {
                result.append(children[0]->to_python(py::reinterpret_borrow<py::object>(item), json_mode, exc_none, round_trip));
            }
            return std::move(result);
        }
        if (type == "dict" && !children.empty()) {
            py::dict result;
            auto d = value.cast<py::dict>();
            for (auto item : d) {
                auto k = py::reinterpret_borrow<py::object>(item.first);
                auto v = py::reinterpret_borrow<py::object>(item.second);
                auto out_k = children[0]->to_python(k, json_mode, exc_none, round_trip);
                auto out_v = children.size() > 1 ? children[1]->to_python(v, json_mode, exc_none, round_trip) : v;
                result[out_k] = out_v;
            }
            return std::move(result);
        }
        if (type == "tuple" && !children.empty()) {
            py::list result;
            size_t i = 0;
            for (auto item : py::reinterpret_borrow<py::sequence>(value)) {
                auto v = py::reinterpret_borrow<py::object>(item);
                result.append(i < children.size() ? children[i]->to_python(v, json_mode, exc_none, round_trip) : children.back()->to_python(v, json_mode, exc_none, round_trip));
                i++;
            }
            return std::move(result);
        }
        return value;
    }

    std::string to_json(const py::object& value, bool ensure_ascii, int indent, bool round_trip = false,
                         const std::optional<std::unordered_set<std::string>>& include_fields = std::nullopt,
                         const std::optional<std::unordered_set<std::string>>& exclude_fields = std::nullopt,
                         bool by_alias = false,
                         bool exclude_unset = false,
                         bool exclude_defaults = false) const {
        if (type == "lax-or-strict") {
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "is-instance" || type == "is-subclass") {
            return infer_json(value, ensure_ascii, indent);
        }
        if (type == "none" || type == "is-none") return "null";
        if (type == "bool" || py::isinstance<py::bool_>(value)) {
            return value.cast<bool>() ? "true" : "false";
        }
        if (type == "int" || type == "int-constrained") {
            if (py::isinstance<py::bool_>(value)) return value.cast<bool>() ? "true" : "false";
            return std::to_string(value.cast<int64_t>());
        }
        if (type == "float" || type == "float-constrained") {
            double d = value.cast<double>();
            if (std::isnan(d)) return "NaN";
            if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
            // Strip trailing zeros: 10.2 -> "10.2", not "10.200000"
            std::string s = std::to_string(d);
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                auto last = s.find_last_not_of('0');
                if (last > dot) {
                    s.erase(last + 1);
                } else {
                    // Only zeros after decimal point, keep one trailing zero for "10.0"
                    s.erase(dot + 2);
                }
            }
            return s;
        }
        if (type == "str" || type == "string" || type == "str-constrained") {
            return json_escape(value.cast<std::string>(), ensure_ascii);
        }
        if (type == "bytes") {
            // Default: decode bytes as UTF-8 string (matching Rust pydantic-core behavior)
            try {
                std::string b = value.cast<std::string>();
                return json_escape(b, ensure_ascii);
            } catch (...) {
                return "\"<bytes>\"";
            }
        }
        if (type == "json") {
            if (round_trip) {
                // round_trip: serialize value to JSON, wrap as escaped JSON string
                std::string inner_json;
                if (!children.empty()) {
                    inner_json = children[0]->to_json(value, ensure_ascii, indent, round_trip, include_fields, exclude_fields, by_alias);
                } else {
                    inner_json = infer_json(value, ensure_ascii, indent);
                }
                return json_escape(inner_json, ensure_ascii);
            }
            if (!children.empty()) {
                return children[0]->to_json(value, ensure_ascii, indent, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
            }
        }
        if (type == "nullable" || type == "nullable-union") {
            if (value.is_none()) return "null";
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "default" || type == "with-default") {
            if (value.is_none() && has_default_val) {
                if (!children.empty()) return children[0]->to_json(default_val, ensure_ascii, indent, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
                return infer_json(default_val, ensure_ascii, indent);
            }
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "to-string") {
            py::object inner = !children.empty() ? children[0]->to_python(value, false, false, round_trip) : value;
            return json_escape(py::str(inner).cast<std::string>(), ensure_ascii);
        }
        // Types that serialize as their str() representation
        if (type == "uuid" || type == "decimal" || type == "ipaddress" ||
            type == "ipv4address" || type == "ipv6address" ||
            type == "ipv4interface" || type == "ipv6interface" ||
            type == "ipv4network" || type == "ipv6network") {
            return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
        }
        // datetime/time: call .isoformat()
        if (type == "datetime" || type == "time") {
            try {
                py::object iso = value.attr("isoformat")();
                std::string s = py::str(iso).cast<std::string>();
                // Replace +00:00 with Z for UTC datetimes
                if (s.size() >= 6 && s.substr(s.size() - 6) == "+00:00") {
                    s = s.substr(0, s.size() - 6) + "Z";
                }
                return json_escape(s, ensure_ascii);
            } catch (...) {
                return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
            }
        }
        // timedelta: ISO 8601 duration format
        if (type == "timedelta") {
            try {
                long days = value.attr("days").cast<long>();
                long seconds = value.attr("seconds").cast<long>();
                long microseconds = value.attr("microseconds").cast<long>();
                // Compute total seconds to determine sign
                double total_seconds = days * 86400.0 + seconds + microseconds / 1000000.0;
                bool negative = total_seconds < 0;
                if (negative) {
                    days = -days;
                    seconds = -seconds;
                    microseconds = -microseconds;
                    // Normalize: borrow from days to make seconds/microseconds positive
                    if (microseconds < 0) { microseconds += 1000000; seconds--; }
                    if (seconds < 0) { seconds += 86400; days--; }
                    if (days < 0) { days = 0; seconds = 0; microseconds = 0; }
                }
                std::string result = negative ? "-P" : "P";
                if (days > 0) result += std::to_string(days) + "D";
                if (seconds > 0 || microseconds > 0) {
                    result += "T";
                    if (seconds > 0) {
                        if (microseconds > 0) {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%ld.%06ld", seconds, microseconds);
                            std::string s(buf);
                            auto last = s.find_last_not_of('0');
                            if (last != std::string::npos) s.erase(last + 1);
                            if (s.back() == '.') s.pop_back();
                            result += s + "S";
                        } else {
                            result += std::to_string(seconds) + "S";
                        }
                    } else if (microseconds > 0) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "0.%06ld", microseconds);
                        std::string s(buf);
                        auto last = s.find_last_not_of('0');
                        if (last != std::string::npos) s.erase(last + 1);
                        if (s.back() == '.') s.pop_back();
                        result += s + "S";
                    }
                }
                if (result == "P" || result == "-P") result = "PT0S";
                return json_escape(result, ensure_ascii);
            } catch (...) {
                return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
            }
        }
        // set/frozenset: serialize as JSON array
        if (type == "set" || type == "frozenset") {
            std::string out = "[";
            bool first = true;
            for (auto item : py::reinterpret_borrow<py::iterable>(value)) {
                if (!first) out += ",";
                first = false;
                py::object obj = py::reinterpret_borrow<py::object>(item);
                if (!children.empty()) {
                    out += children[0]->to_json(obj, ensure_ascii, -1, round_trip);
                } else {
                    out += infer_json(obj, ensure_ascii, -1);
                }
            }
            out += "]";
            return out;
        }
        if (!fields.empty()) {
            return serialize_fields_json(value, ensure_ascii, indent, false, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        // Delegate model/dataclass/typed-dict to inner serializer
        if ((type == "model" || type == "dataclass" || type == "typed-dict") && !children.empty()) {
            return children[0]->to_json(value, ensure_ascii, indent, round_trip, include_fields, exclude_fields, by_alias, exclude_unset, exclude_defaults);
        }
        if (!py_func.is_none()) {
            py::object result;
            if (type == "function-plain") {
                if (info_arg) {
                    PySerializationInfo info(round_trip, "json");
                    result = py_func(value, py::cast(info));
                } else {
                    result = py_func(value);
                }
            } else {
                result = to_python(value, false, false, round_trip);
            }
            return infer_json(result, ensure_ascii, indent);
        }
        return infer_json(value, ensure_ascii, indent);
    }

    // Get the default value for this serializer node (for default/with-default types)
    py::object get_default_value() const {
        if (type == "default" || type == "with-default") {
            return default_val;
        }
        return py::none();
    }

    // Check if this serializer has a default value
    bool has_default() const {
        if (type == "default" || type == "with-default") {
            return has_default_val;
        }
        return false;
    }

private:
    static std::string json_escape(const std::string& s, bool ensure_ascii) {
        std::string out = "\"";
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (ensure_ascii && c > 127) {
                        char buf[8]; snprintf(buf, 8, "\\u%04x", c); out += buf;
                    } else out += (char)c;
                    break;
            }
        }
        out += "\"";
        return out;
    }

    static std::string infer_json(const py::object& value, bool ensure_ascii, int indent) {
        if (value.is_none()) return "null";
        if (py::isinstance<py::bool_>(value)) return value.cast<bool>() ? "true" : "false";
        if (py::isinstance<py::int_>(value)) return py::str(py::repr(value)).cast<std::string>();
        if (py::isinstance<py::float_>(value)) {
            double d = value.cast<double>();
            if (std::isnan(d)) return "NaN";
            if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
            return py::str(py::repr(value)).cast<std::string>();
        }
        if (py::isinstance<py::str>(value)) return json_escape(value.cast<std::string>(), ensure_ascii);
        if (py::isinstance<py::bytes>(value)) {
            std::string b = value.cast<std::string>();
            std::string enc;
            static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (size_t i = 0; i < b.size(); i += 3) {
                uint32_t n = ((uint8_t)b[i] << 16);
                if (i+1 < b.size()) n |= ((uint8_t)b[i+1] << 8);
                if (i+2 < b.size()) n |= (uint8_t)b[i+2];
                enc += b64[(n>>18)&0x3F]; enc += b64[(n>>12)&0x3F];
                enc += (i+1<b.size()) ? b64[(n>>6)&0x3F] : '=';
                enc += (i+2<b.size()) ? b64[n&0x3F] : '=';
            }
            return "\"" + enc + "\"";
        }
        if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
            std::string out = "[";
            bool first = true;
            for (auto item : py::reinterpret_borrow<py::sequence>(value)) {
                if (!first) out += ",";
                first = false;
                out += infer_json(py::reinterpret_borrow<py::object>(item), ensure_ascii, -1);
            }
            out += "]";
            return out;
        }
        if (py::isinstance<py::dict>(value)) {
            std::string out = "{";
            bool first = true;
            auto d = value.cast<py::dict>();
            for (auto item : d) {
                if (!first) out += ",";
                first = false;
                out += json_escape(py::str(item.first).cast<std::string>(), ensure_ascii);
                out += ":";
                out += infer_json(py::reinterpret_borrow<py::object>(item.second), ensure_ascii, -1);
            }
            out += "}";
            return out;
        }
        if (py::hasattr(value, "__dict__")) return infer_json(value.attr("__dict__"), ensure_ascii, indent);

        // Check for Url and MultiHostUrl objects
        try {
            py::object url_mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
            py::object url_cls = url_mod.attr("Url");
            py::object murl_cls = url_mod.attr("MultiHostUrl");
            if (py::isinstance(value, url_cls) || py::isinstance(value, murl_cls)) {
                return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
            }
        } catch (...) {}

        return json_escape(py::repr(value).cast<std::string>(), ensure_ascii);
    }

    py::object serialize_fields(const py::object& value, bool exc_none, bool round_trip = false,
                                 const std::optional<std::unordered_set<std::string>>& include_fields = std::nullopt,
                                 const std::optional<std::unordered_set<std::string>>& exclude_fields = std::nullopt,
                                 bool by_alias = false,
                                 bool exclude_unset = false,
                                 bool exclude_defaults = false) const {
        py::dict result;
        py::dict main;
        if (py::isinstance<py::dict>(value)) main = value.cast<py::dict>();
        else if (py::hasattr(value, "__dict__")) main = py::getattr(value, "__dict__").cast<py::dict>();

        for (const auto& k : field_order) {
            const auto& ser = fields.at(k);
            // Skip internal metadata keys
            if (k == "__pydantic_fields_set__" || k == "__pydantic_defaults__") continue;
            
            // Skip computed fields when round_trip=True
            if (round_trip && computed_fields_.count(k)) continue;
            
            // Apply include/exclude filters
            if (include_fields && !include_fields->count(k)) continue;
            if (exclude_fields && exclude_fields->count(k)) continue;
            
            // exclude_unset: skip fields that were not explicitly set in input
            if (exclude_unset) {
                py::object fs = py::none();
                if (py::isinstance<py::dict>(value)) {
                    py::dict vd = value.cast<py::dict>();
                    py::str fs_key("__pydantic_fields_set__");
                    if (vd.contains(fs_key)) fs = vd[fs_key];
                } else if (py::hasattr(value, "__pydantic_fields_set__")) {
                    fs = py::getattr(value, "__pydantic_fields_set__");
                }
                if (!fs.is_none() && py::isinstance<py::set>(fs)) {
                    if (!fs.cast<py::set>().contains(py::str(k))) continue;
                }
            }
            
            // exclude_defaults: skip fields whose value equals their default
            if (exclude_defaults) {
                py::object defaults = py::none();
                if (py::isinstance<py::dict>(value)) {
                    py::dict vd = value.cast<py::dict>();
                    py::str d_key("__pydantic_defaults__");
                    if (vd.contains(d_key)) defaults = vd[d_key];
                } else if (py::hasattr(value, "__pydantic_defaults__")) {
                    defaults = py::getattr(value, "__pydantic_defaults__");
                }
                if (!defaults.is_none() && py::isinstance<py::dict>(defaults)) {
                    py::str key(k);
                    py::dict dd = defaults.cast<py::dict>();
                    if (dd.contains(key)) {
                        py::object def_val = dd[key];
                        if (main.contains(key)) {
                            py::object cur = main[key];
                            try {
                                if (cur.equal(def_val)) continue;
                            } catch (...) {}
                        }
                    }
                }
            }
            
            // Determine output key name
            std::string output_key = k;
            if (by_alias) {
                auto alias_it = field_aliases.find(k);
                if (alias_it != field_aliases.end()) {
                    output_key = alias_it->second;
                }
            }
            
            py::str key(k);
            py::object fv;
            bool has_value = true;
            if (main.contains(key)) {
                fv = main[key];
            } else if (ser->has_default()) {
                fv = ser->get_default_value();
            } else {
                has_value = false;
            }
            if (!has_value) continue;
            if (exc_none && fv.is_none()) continue;

            // Apply exclude_if callable
            {
                auto eif_it = field_exclude_if.find(k);
                if (eif_it != field_exclude_if.end() && !eif_it->second.is_none()) {
                    try {
                        py::object result = eif_it->second(fv);
                        if (result.cast<bool>()) continue;
                    } catch (...) {}
                }
            }

            result[py::str(output_key)] = ser->to_python(fv, false, exc_none, round_trip);
        }
        // Extra fields - also apply include/exclude if they match by name
        if (py::hasattr(value, "__pydantic_extra__")) {
            auto extra = py::getattr(value, "__pydantic_extra__");
            if (!extra.is_none()) {
                for (auto item : extra.cast<py::dict>()) {
                    std::string k = py::str(item.first).cast<std::string>();
                    if (fields.find(k) != fields.end()) continue;
                    // Apply include/exclude filters to extra fields too
                    if (include_fields && !include_fields->count(k)) continue;
                    if (exclude_fields && exclude_fields->count(k)) continue;
                    py::object v = py::reinterpret_borrow<py::object>(item.second);
                    if (exc_none && v.is_none()) continue;
                    result[py::str(k)] = v;
                }
            }
        }
        return std::move(result);
    }

    std::string serialize_fields_json(const py::object& value, bool ensure_ascii, int indent, bool exc_none, bool round_trip = false,
                                       const std::optional<std::unordered_set<std::string>>& include_fields = std::nullopt,
                                       const std::optional<std::unordered_set<std::string>>& exclude_fields = std::nullopt,
                                       bool by_alias = false,
                                       bool exclude_unset = false,
                                       bool exclude_defaults = false) const {
        std::string out = "{";
        bool first = true;
        py::dict main;
        if (py::isinstance<py::dict>(value)) main = value.cast<py::dict>();
        else if (py::hasattr(value, "__dict__")) main = py::getattr(value, "__dict__").cast<py::dict>();

        for (const auto& k : field_order) {
            const auto& ser = fields.at(k);
            // Skip internal metadata keys
            if (k == "__pydantic_fields_set__" || k == "__pydantic_defaults__") continue;
            
            // Skip computed fields when round_trip=True
            if (round_trip && computed_fields_.count(k)) continue;
            
            // Apply include/exclude filters
            if (include_fields && !include_fields->count(k)) continue;
            if (exclude_fields && exclude_fields->count(k)) continue;
            
            // exclude_unset: skip fields that were not explicitly set in input
            if (exclude_unset) {
                py::object fs = py::none();
                if (py::isinstance<py::dict>(value)) {
                    py::dict vd = value.cast<py::dict>();
                    py::str fs_key("__pydantic_fields_set__");
                    if (vd.contains(fs_key)) fs = vd[fs_key];
                } else if (py::hasattr(value, "__pydantic_fields_set__")) {
                    fs = py::getattr(value, "__pydantic_fields_set__");
                }
                if (!fs.is_none() && py::isinstance<py::set>(fs)) {
                    if (!fs.cast<py::set>().contains(py::str(k))) continue;
                }
            }
            
            // exclude_defaults: skip fields whose value equals their default
            if (exclude_defaults) {
                py::object defaults = py::none();
                if (py::isinstance<py::dict>(value)) {
                    py::dict vd = value.cast<py::dict>();
                    py::str d_key("__pydantic_defaults__");
                    if (vd.contains(d_key)) defaults = vd[d_key];
                } else if (py::hasattr(value, "__pydantic_defaults__")) {
                    defaults = py::getattr(value, "__pydantic_defaults__");
                }
                if (!defaults.is_none() && py::isinstance<py::dict>(defaults)) {
                    py::str key(k);
                    py::dict dd = defaults.cast<py::dict>();
                    if (dd.contains(key)) {
                        py::object def_val = dd[key];
                        if (main.contains(key)) {
                            py::object cur = main[key];
                            try {
                                if (cur.equal(def_val)) continue;
                            } catch (...) {}
                        }
                    }
                }
            }
            
            // Determine output key name
            std::string output_key = k;
            if (by_alias) {
                auto alias_it = field_aliases.find(k);
                if (alias_it != field_aliases.end()) {
                    output_key = alias_it->second;
                }
            }
            
            py::str key(k);
            py::object fv;
            bool has_value = true;
            if (main.contains(key)) {
                fv = main[key];
            } else if (ser->has_default()) {
                fv = ser->get_default_value();
            } else {
                has_value = false;
            }
            if (!has_value) continue;
            if (exc_none && fv.is_none()) continue;

            // Apply exclude_if callable
            {
                auto eif_it = field_exclude_if.find(k);
                if (eif_it != field_exclude_if.end() && !eif_it->second.is_none()) {
                    try {
                        py::object result = eif_it->second(fv);
                        if (result.cast<bool>()) continue;
                    } catch (...) {}
                }
            }

            if (!first) out += ",";
            first = false;
            out += json_escape(output_key, ensure_ascii) + ":" + ser->to_json(fv, ensure_ascii, -1, round_trip);
        }
        if (py::hasattr(value, "__pydantic_extra__")) {
            auto extra = py::getattr(value, "__pydantic_extra__");
            if (!extra.is_none()) {
                for (auto item : extra.cast<py::dict>()) {
                    std::string k = py::str(item.first).cast<std::string>();
                    if (fields.find(k) != fields.end()) continue;
                    // Apply include/exclude filters to extra fields too
                    if (include_fields && !include_fields->count(k)) continue;
                    if (exclude_fields && exclude_fields->count(k)) continue;
                    py::object v = py::reinterpret_borrow<py::object>(item.second);
                    if (exc_none && v.is_none()) continue;
                    if (!first) out += ",";
                    first = false;
                    out += json_escape(k, ensure_ascii) + ":" + infer_json(v, ensure_ascii, -1);
                }
            }
        }
        out += "}";
        return out;
    }
};

// ---------------------------------------------------------------------------
// Build serializer from schema
// ---------------------------------------------------------------------------
static SerRef build_ser_impl(const py::dict& schema,
                        std::unordered_map<std::string, SerRef>& defs);

static SerRef build_ser(const py::dict& schema,
                        std::unordered_map<std::string, SerRef>& defs) {
    return build_ser_impl(schema, defs);
}

static SerRef build_ser_impl(const py::dict& schema,
                        std::unordered_map<std::string, SerRef>& defs) {
    std::string type;
    try { type = schema["type"].cast<std::string>(); } catch (...) { type = "any"; }

    // Check serialization override
    py::dict ser_dict;
    try {
        if (schema.contains("serialization")) {
            ser_dict = schema["serialization"].cast<py::dict>();
            if (ser_dict.contains("type")) {
                std::string st = ser_dict["type"].cast<std::string>();
                if (st != "include-exclude-sequence" && st != "include-exclude-dict" && st != "base64")
                    type = st;
            }
        }
    } catch (...) {}

    auto node = std::make_shared<SerNode>();
    node->type = type;

    // When serialization overrides the function too, store it for later use
    // (the function extraction below will use the main schema's function by default)
    py::object ser_func = py::none();
    bool ser_info_arg = false;
    try {
        if (!ser_dict.is_none() && ser_dict.contains("function")) {
            ser_func = ser_dict["function"];
        }
        if (!ser_dict.is_none() && ser_dict.contains("info_arg")) {
            ser_info_arg = ser_dict["info_arg"].cast<bool>();
        }
    } catch (...) {}

    auto sub = [&](const char* key = "schema") -> SerRef {
        try { return build_ser(schema[key].cast<py::dict>(), defs); } catch (...) { return nullptr; }
    };

    if (type == "definitions") {
        try {
            auto defs_list = schema["definitions"].cast<py::list>();
            // Pass 1: register stub SerNodes for all definition refs
            for (auto item : defs_list) {
                auto d = item.cast<py::dict>();
                std::string ref = d["ref"].cast<std::string>();
                defs[ref] = std::make_shared<SerNode>();
                defs[ref]->type = "__stub__";
            }
            // Pass 2: build each definition, copying content into stub
            for (auto item : defs_list) {
                auto d = item.cast<py::dict>();
                std::string ref = d["ref"].cast<std::string>();
                auto actual = build_ser_impl(d["schema"].cast<py::dict>(), defs);
                // Copy actual content into the stub (preserves shared_ptr identity)
                defs[ref]->copy_from(*actual);
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "Error building definitions: %s\n", e.what());
        }
        try { return build_ser_impl(schema["schema"].cast<py::dict>(), defs); } catch (...) {}
        return node;
    }
    if (type == "definition-ref") {
        try {
            std::string ref = schema["schema_ref"].cast<std::string>();
            auto it = defs.find(ref);
            if (it != defs.end()) return it->second;  // Return stub (will be populated)
        } catch (...) {}
        return node;
    }

    // lax-or-strict: parse both schemas, use lax for serialization
    if (type == "lax-or-strict") {
        try { node->children.push_back(build_ser(schema["lax_schema"].cast<py::dict>(), defs)); } catch (...) {}
        try { node->children.push_back(build_ser(schema["strict_schema"].cast<py::dict>(), defs)); } catch (...) {}
    }

    // is-instance: no-op for serialization (passthrough)
    // No children to build — type stays as-is

    // Types with inner schema (schema key)
    if (type == "nullable" || type == "nullable-union" || type == "default" || type == "with-default" ||
        type == "json" || type == "format" || type == "to-string" || type == "enum") {
        auto c = sub();
        if (c) node->children.push_back(c);
    }

    // Types with items_schema key
    if (type == "list" || type == "set" || type == "frozenset" || type == "generator") {
        try {
            auto c = build_ser_impl(schema["items_schema"].cast<py::dict>(), defs);
            if (c) node->children.push_back(c);
        } catch (...) {}
    }

    if (type == "dict") {
        auto ks = [&](const char* k) -> SerRef { try { return build_ser(schema[k].cast<py::dict>(), defs); } catch (...) { return nullptr; } };
        auto key_ser = ks("keys_schema");
        auto val_ser = ks("values_schema");
        if (!key_ser) { key_ser = std::make_shared<SerNode>(); key_ser->type = "str"; }
        if (!val_ser) { val_ser = std::make_shared<SerNode>(); val_ser->type = "any"; }
        node->children.push_back(key_ser);
        node->children.push_back(val_ser);
    }

    if (type == "tuple") {
        try {
            auto items = schema["items_schema"];
            if (py::isinstance<py::list>(items)) {
                for (auto it : items.cast<py::list>())
                    node->children.push_back(build_ser(it.cast<py::dict>(), defs));
            } else node->children.push_back(build_ser(items.cast<py::dict>(), defs));
        } catch (...) {}
    }

    if (type == "union") {
        try {
            for (auto ch : schema["choices"].cast<py::list>()) {
                if (py::isinstance<py::tuple>(ch)) {
                    auto t = ch.cast<py::tuple>();
                    node->children.push_back(build_ser(t[0].cast<py::dict>(), defs));
                } else node->children.push_back(build_ser(ch.cast<py::dict>(), defs));
            }
        } catch (...) {}
    }

    if (type == "tagged-union") {
        try {
            for (auto item : schema["choices"].cast<py::dict>()) {
                std::string tag = py::str(item.first).cast<std::string>();
                node->tagged[tag] = build_ser(item.second.cast<py::dict>(), defs);
            }
        } catch (...) {}
    }

    if (type == "json-or-python") {
        auto js = sub("json_schema");
        auto ps = sub("python_schema");
        if (js) node->children.push_back(js);
        if (ps) node->children.push_back(ps);
    }

    if (type == "default" || type == "with-default") {
        try { node->default_val = schema["default"]; node->has_default_val = true; } catch (...) {}
    }
    if (type == "format") {
        try { node->format_str = schema["formatting"].cast<std::string>(); } catch (...) {}
    }

    if (type == "function-plain" || type == "function-after" || type == "function-before" || type == "function-wrap") {
        try {
            // Use serialization function if available (overrides main schema's function)
            py::object func = ser_func.is_none() ? schema["function"] : ser_func;
            // function may be a dict like {'function': actual_callable, 'type': 'no-info'}
            if (py::isinstance<py::dict>(func)) {
                py::dict func_dict = func.cast<py::dict>();
                if (func_dict.contains("function")) {
                    func = func_dict["function"];
                }
            }
            node->py_func = func;
            node->info_arg = ser_info_arg;
        } catch (...) {}
        if (type != "function-plain") { auto c = sub(); if (c) node->children.push_back(c); }
    }

    // model-fields, typed-dict, dataclass-args
    if (type == "model-fields" || type == "typed-dict" || type == "dataclass-args") {
        try {
            if (schema.contains("fields")) {
                auto fields_dict = schema["fields"].cast<py::dict>();
                for (auto item : fields_dict) {
                    std::string k = py::str(item.first).cast<std::string>();
                    auto fdef = item.second.cast<py::dict>();
                    // Handle both formats:
                    // 1. {'schema': {'type': 'str'}} - pydantic-core format
                    // 2. {'type': 'str'} - simplified format
                    py::dict field_schema;
                    if (fdef.contains("schema")) {
                        field_schema = fdef["schema"].cast<py::dict>();
                        // Check for alias in field definition (pydantic-core format)
                        if (fdef.contains("alias")) {
                            node->field_aliases[k] = fdef["alias"].cast<std::string>();
                        }
                    } else {
                        field_schema = fdef;  // Use fdef directly as schema
                        // Check for alias directly in schema
                        if (field_schema.contains("alias")) {
                            node->field_aliases[k] = field_schema["alias"].cast<std::string>();
                        }
                    }
                    // Extract serialization_exclude_if callable (for exclude_if support)
                    if (fdef.contains("serialization_exclude_if")) {
                        py::object eif = fdef["serialization_exclude_if"];
                        if (py::isinstance<py::function>(eif) || py::hasattr(eif, "__call__")) {
                            node->field_exclude_if[k] = eif;
                        }
                    }
                    node->fields[k] = build_ser(field_schema, defs);
                    node->field_order.push_back(k);
                }
            }
            // Collect computed field names (excluded when round_trip=True)
            // Also add them to fields map with their return_schema serializer
            if (schema.contains("computed_fields")) {
                for (auto cf_item : schema["computed_fields"].cast<py::list>()) {
                    auto cf = cf_item.cast<py::dict>();
                    std::string prop = cf["property_name"].cast<std::string>();
                    node->computed_fields_.insert(prop);
                    try {
                        node->fields[prop] = build_ser(cf["return_schema"].cast<py::dict>(), defs);
                        node->field_order.push_back(prop);
                    } catch (...) {
                        // If no return_schema, fall back to any
                        py::dict any_schema;
                        any_schema["type"] = "any";
                        node->fields[prop] = build_ser(any_schema, defs);
                    }
                    node->field_order.push_back(prop);
                }
            }
        } catch (const std::exception& e) {
            // Schema field parsing failed silently
            (void)e;
        } catch (...) {
            // Unknown exception during field parsing
        }
    }

    // model, typed-dict, dataclass — wrap inner
    if (type == "model" || type == "typed-dict" || type == "dataclass") {
        auto c = sub();
        if (c) node->children.push_back(c);
    }

    return node;
}

// ---------------------------------------------------------------------------
// PySchemaSerializer
// ---------------------------------------------------------------------------
class PySchemaSerializer {
public:
    PySchemaSerializer() = default;

    explicit PySchemaSerializer(const py::dict& schema, const std::optional<py::dict>& = std::nullopt)
        : schema_(schema) {
        std::unordered_map<std::string, SerRef> defs;
        ser_ = build_ser(schema, defs);
    }

    // Overload that accepts _use_prebuilt (unused but needed for pydantic API)
    explicit PySchemaSerializer(const py::dict& schema, const std::optional<py::dict>& cfg, bool)
        : schema_(schema) {
        std::unordered_map<std::string, SerRef> defs;
        ser_ = build_ser(schema, defs);
        (void)cfg;
    }

    py::object to_python(const py::object& value, std::optional<std::string> mode,
                         std::optional<py::object> include, std::optional<py::object> exclude,
                         std::optional<bool> by_alias, bool exclude_unset, bool exclude_defaults, bool exc_none,
                         bool, bool round_trip, py::object, std::optional<py::object>,
                         bool, std::optional<bool>, std::optional<py::object>) const {
        if (!ser_) throw std::runtime_error("Serializer not initialized");

        // Parse include/exclude from Python objects to C++ sets
        std::optional<std::unordered_set<std::string>> include_fields;
        std::optional<std::unordered_set<std::string>> exclude_fields;
        bool use_alias = by_alias.value_or(false);
        
        if (include && !include->is_none()) {
            include_fields = std::unordered_set<std::string>();
            py::object inc_obj = *include;  // Get the actual py::object
            if (py::isinstance<py::set>(inc_obj) || py::isinstance<py::list>(inc_obj) || py::isinstance<py::tuple>(inc_obj)) {
                for (auto item : inc_obj) {
                    include_fields->insert(py::str(item).cast<std::string>());
                }
            } else if (py::isinstance<py::dict>(inc_obj)) {
                for (auto item : inc_obj.cast<py::dict>()) {
                    include_fields->insert(py::str(item.first).cast<std::string>());
                }
            }
        }
        
        if (exclude && !exclude->is_none()) {
            exclude_fields = std::unordered_set<std::string>();
            py::object exc_obj = *exclude;  // Get the actual py::object
            if (py::isinstance<py::set>(exc_obj) || py::isinstance<py::list>(exc_obj) || py::isinstance<py::tuple>(exc_obj)) {
                for (auto item : exc_obj) {
                    exclude_fields->insert(py::str(item).cast<std::string>());
                }
            } else if (py::isinstance<py::dict>(exc_obj)) {
                for (auto item : exc_obj.cast<py::dict>()) {
                    exclude_fields->insert(py::str(item.first).cast<std::string>());
                }
            }
        }
        
        return ser_->to_python(value, mode && *mode == "json", exc_none, round_trip, include_fields, exclude_fields, use_alias, exclude_unset, exclude_defaults);
    }

    py::bytes to_json(const py::object& value, std::optional<size_t>, std::optional<bool> ea,
                      std::optional<py::object> include, std::optional<py::object> exclude,
                      std::optional<bool> by_alias, bool exclude_unset, bool exclude_defaults, bool exc_none,
                      bool, bool round_trip, py::object, std::optional<py::object>,
                      bool, std::optional<bool>, std::optional<py::object>) const {
        if (!ser_) throw std::runtime_error("Serializer not initialized");
        
        // Parse include/exclude from Python objects to C++ sets
        std::optional<std::unordered_set<std::string>> include_fields;
        std::optional<std::unordered_set<std::string>> exclude_fields;
        bool use_alias = by_alias.value_or(false);
        
        if (include && !include->is_none()) {
            include_fields = std::unordered_set<std::string>();
            py::object inc_obj = *include;
            if (py::isinstance<py::set>(inc_obj) || py::isinstance<py::list>(inc_obj) || py::isinstance<py::tuple>(inc_obj)) {
                for (auto item : inc_obj) {
                    include_fields->insert(py::str(item).cast<std::string>());
                }
            } else if (py::isinstance<py::dict>(inc_obj)) {
                for (auto item : inc_obj.cast<py::dict>()) {
                    include_fields->insert(py::str(item.first).cast<std::string>());
                }
            }
        }
        
        if (exclude && !exclude->is_none()) {
            exclude_fields = std::unordered_set<std::string>();
            py::object exc_obj = *exclude;
            if (py::isinstance<py::set>(exc_obj) || py::isinstance<py::list>(exc_obj) || py::isinstance<py::tuple>(exc_obj)) {
                for (auto item : exc_obj) {
                    exclude_fields->insert(py::str(item).cast<std::string>());
                }
            } else if (py::isinstance<py::dict>(exc_obj)) {
                for (auto item : exc_obj.cast<py::dict>()) {
                    exclude_fields->insert(py::str(item.first).cast<std::string>());
                }
            }
        }
        
        bool e = ea.value_or(false);
        std::string json = ser_->to_json(value, e, -1, round_trip, include_fields, exclude_fields, use_alias, exclude_unset, exclude_defaults);
        return py::bytes(json);
    }

    std::string repr() const {
        return ser_ ? "SchemaSerializer(serializer=" + ser_->type + ")" : "SchemaSerializer()";
    }

    const py::object& get_schema() const { return schema_; }

private:
    SerRef ser_;
    py::object schema_;  // Store schema for pickle support
};

// ---------------------------------------------------------------------------
// Standalone to_json
// ---------------------------------------------------------------------------
static py::bytes to_json_fn(const py::object& value, std::optional<size_t>, std::optional<bool> ea,
    std::optional<py::object>, std::optional<py::object>, bool, bool, bool round_trip,
    std::string, std::string, std::string, std::string, bool,
    std::optional<py::object>, bool, std::optional<bool>, std::optional<py::object>) {
    SerRef any = std::make_shared<SerNode>();
    any->type = "any";
    return py::bytes(any->to_json(value, ea.value_or(false), -1, round_trip));
}

static py::object to_jsonable_fn(const py::object& value, std::optional<py::object>, std::optional<py::object>,
    bool, bool, bool round_trip, std::string, std::string, std::string, std::string, bool,
    std::optional<py::object>, bool, std::optional<bool>, std::optional<py::object>) {
    (void)round_trip;
    return py::reinterpret_borrow<py::object>(value);
}

PYBIND11_MODULE(_pydantic_core_cpp, m) {
    m.doc() = "pydantic-core C++ implementation";
    m.attr("__version__") = get_version();

    py::enum_<InputType>(m, "InputType")
        .value("python", InputType::Python).value("json", InputType::Json).value("string", InputType::String);
    py::enum_<ExtraBehavior>(m, "ExtraBehavior")
        .value("allow", ExtraBehavior::Allow).value("forbid", ExtraBehavior::Forbid).value("ignore", ExtraBehavior::Ignore);
    py::enum_<StringCacheMode>(m, "StringCacheMode")
        .value("all", StringCacheMode::All).value("keys", StringCacheMode::Keys).value("none", StringCacheMode::None);
    py::enum_<SerMode>(m, "SerMode").value("python", SerMode::Python).value("json", SerMode::Json);
    py::enum_<TemporalMode>(m, "TemporalMode")
        .value("iso8601", TemporalMode::Iso8601).value("seconds", TemporalMode::Seconds).value("milliseconds", TemporalMode::Milliseconds);
    py::enum_<BytesMode>(m, "BytesMode")
        .value("utf8", BytesMode::Utf8).value("base64", BytesMode::Base64).value("hex", BytesMode::Hex);
    py::enum_<InfNanMode>(m, "InfNanMode")
        .value("null", InfNanMode::Null).value("constants", InfNanMode::Constants).value("strings", InfNanMode::Strings);

    // SerializationInfo - passed to custom serializer functions
    py::class_<PySerializationInfo>(m, "SerializationInfo")
        .def(py::init<bool, std::string>(), py::arg("round_trip"), py::arg("mode") = "python")
        .def_readonly("round_trip", &PySerializationInfo::round_trip)
        .def_readonly("mode", &PySerializationInfo::mode);

    // Register ValidationError as a proper Python exception (inherits from ValueError like Rust)
    py::register_exception<ValidationError>(m, "ValidationError", PyExc_ValueError);

    // SchemaError as a proper Python exception
    py::register_exception<SchemaError>(m, "SchemaError", PyExc_ValueError);

    // Add custom methods to ValidationError (register_exception creates bare Exception subclass)
    {   // title property
        auto ve_cls = m.attr("ValidationError");
        py::setattr(ve_cls, "title",
            py::cpp_function([](py::object self) -> std::string {
                return self.cast<const ValidationError&>().title();
            }, py::is_method(ve_cls))
        );
        py::setattr(ve_cls, "error_count",
            py::cpp_function([](py::object self) -> int {
                return self.cast<const ValidationError&>().error_count();
            }, py::is_method(ve_cls))
        );
        py::setattr(ve_cls, "errors",
            py::cpp_function([](py::object self) -> py::list {
                // Build the error dicts manually: ErrorDetails is not a bound
                // type, so casting std::vector<ErrorDetails> to a list would be
                // undefined behavior.
                const auto& errors = self.cast<const ValidationError&>().errors();
                py::list result;
                for (const auto& err : errors) {
                    py::dict d;
                    d["type"] = err.type;
                    // loc: "x" / "x.0" / "a.b" -> ('x',) / ('x', 0) / ('a', 'b')
                    py::list loc_list;
                    std::string cur;
                    auto flush = [&]() {
                        if (cur.empty()) return;
                        bool is_index = !cur.empty();
                        for (char c : cur) {
                            if (!(c >= '0' && c <= '9') && c != '-') { is_index = false; break; }
                        }
                        if (is_index) {
                            try { loc_list.append(py::int_(std::stoll(cur))); }
                            catch (...) { loc_list.append(cur); }
                        } else {
                            loc_list.append(cur);
                        }
                        cur.clear();
                    };
                    for (char c : err.loc) {
                        if (c == '.') { flush(); } else { cur += c; }
                    }
                    flush();
                    d["loc"] = py::tuple(loc_list);
                    d["msg"] = err.msg;
                    // input: parse the stored repr into a real Python value
                    py::object input_val;
                    try {
                        input_val = py::module_::import("ast").attr("literal_eval")(err.input);
                    } catch (...) {
                        input_val = py::str(err.input);
                    }
                    d["input"] = input_val;
                    if (!err.ctx.empty()) {
                        py::dict ctx;
                        for (const auto& [k, v] : err.ctx) {
                            ctx[py::str(k)] = v;
                        }
                        d["ctx"] = ctx;
                    }
                    d["url"] = "https://errors.pydantic.dev/2.14/v/" + err.type;
                    result.append(d);
                }
                return result;
            }, py::is_method(ve_cls))
        );
        py::setattr(ve_cls, "to_json",
            py::cpp_function([](py::object self) {
                return self.cast<const ValidationError&>().to_json_string();
            }, py::is_method(ve_cls))
        );
    }
    py::class_<PydanticOmit>(m, "PydanticOmit").def(py::init<>());
    py::class_<PydanticUseDefault>(m, "PydanticUseDefault").def(py::init<>());

    // SchemaValidator
    py::class_<SchemaValidator>(m, "SchemaValidator")
        // Primary constructor: takes Python dict directly (Rust-style)
        .def(py::init([](const py::dict& schema, const py::dict& config) {
            return std::make_unique<SchemaValidator>(schema, config);
        }), py::arg("schema"), py::arg("config") = py::none())
        // Legacy constructor with bool flag for backwards compat
        .def(py::init([](const py::dict& schema, const py::dict& config, bool) {
            return std::make_unique<SchemaValidator>(schema, config);
        }), py::arg("schema"), py::arg("config") = py::none(), py::arg("_use_prebuilt") = true)
        .def("validate_python", [](SchemaValidator& self, const py::object& input, py::object strict, py::object context, py::object self_instance,
                                    py::object extra, py::object from_attributes, py::object by_alias, py::object by_name) -> py::object {
            // NEW: Use native PythonInput - no JSON round-trip!
            (void)by_alias; (void)by_name;
            std::optional<bool> fa_opt;
            if (!from_attributes.is_none()) {
                fa_opt = pyobj_to_bool(from_attributes);
            }
            py::object validated = self.validate_python_object(input, pyobj_to_bool(strict), std::nullopt, fa_opt, context);

            // If self_instance provided, populate and return it
            if (!self_instance.is_none() && py::hasattr(self_instance, "__dict__")) {
                try {
                    if (py::isinstance<py::dict>(validated)) {
                        py::dict d = self_instance.attr("__dict__");
                        py::dict validated_dict = validated.cast<py::dict>();

                        // Extract special keys before copying to __dict__
                        py::object extra_fields = py::none();
                        py::object fields_set = py::set();
                        py::object defaults = py::dict();

                        if (validated_dict.contains("__pydantic_extra__")) {
                            extra_fields = validated_dict["__pydantic_extra__"];
                            validated_dict.attr("pop")("__pydantic_extra__");
                        }
                        if (validated_dict.contains("__pydantic_fields_set__")) {
                            fields_set = validated_dict["__pydantic_fields_set__"];
                            validated_dict.attr("pop")("__pydantic_fields_set__");
                        }
                        if (validated_dict.contains("__pydantic_defaults__")) {
                            defaults = validated_dict["__pydantic_defaults__"];
                            validated_dict.attr("pop")("__pydantic_defaults__");
                        }

                        // Copy only declared fields to __dict__
                        for (auto item : validated_dict) {
                            d[item.first] = item.second;
                        }

                        // Set pydantic slot attributes
                        if (!py::hasattr(self_instance, "__pydantic_private__")) {
                            py::setattr(self_instance, "__pydantic_private__", py::dict());
                        }
                        py::setattr(self_instance, "__pydantic_extra__",
                            extra_fields.is_none() ? py::none() : extra_fields);
                        py::setattr(self_instance, "__pydantic_fields_set__", fields_set);
                    }
                } catch (const std::exception& e) {
                    // If anything fails, just return validated as-is
                    py::print("validate_python self_instance error:", py::str(e.what()));
                }
                return self_instance;
            }

            // If validated result is a simple value (like int for dict size), return original input
            if (py::isinstance<py::int_>(validated) && !py::isinstance<py::bool_>(input)) {
                return input;
            }

            return validated;
        }, py::arg("object"), py::arg("strict") = py::none(), py::arg("context") = py::none(), py::arg("self_instance") = py::none(),
             py::arg("extra") = py::none(), py::arg("from_attributes") = py::none(), py::arg("by_alias") = py::none(), py::arg("by_name") = py::none())
        .def("validate_json", [](SchemaValidator& self, const py::object& jd, py::object strict) {
            std::string js = py::isinstance<py::bytes>(jd) ? jd.cast<std::string>() : jd.cast<std::string>();
            return json_to_pyobj(self.validate_json(js, pyobj_to_bool(strict)));
        }, py::arg("json_data"), py::arg("strict") = py::none())
        .def("validate_strings", [](SchemaValidator& self, const py::object& sd, py::object strict) {
            // NEW: Use native PythonInput - no JSON round-trip!
            return self.validate_strings_object(sd, pyobj_to_bool(strict));
        }, py::arg("string_data"), py::arg("strict") = py::none())
        .def("isinstance_python", [](SchemaValidator& self, const py::object& input, py::object strict) {
            // NEW: Use native PythonInput - no JSON round-trip!
            return self.isinstance_python_object(input, pyobj_to_bool(strict));
        }, py::arg("object"), py::arg("strict") = py::none())
        .def("get_default_value", [](SchemaValidator& self, py::object strict) -> py::object {
            auto r = self.get_default_value(pyobj_to_bool(strict));
            return r ? json_to_pyobj(*r) : py::none();
        }, py::arg("strict") = py::none())
        .def("validate_assignment", [](SchemaValidator& self, const py::object& obj, const std::string& fn, const py::object& fv) {
            // NEW: Use native PythonInput - no JSON round-trip!
            return self.validate_assignment_object(obj, fn, fv);
        }, py::arg("object"), py::arg("field_name"), py::arg("field_value"))
        .def_property_readonly("title", &SchemaValidator::title)
        .def("__repr__", &SchemaValidator::repr)
        // Pickle support: __reduce__ returns (cls, (schema_json, config_json))
        .def("__reduce__", [](const SchemaValidator& self) -> py::tuple {
            // Get the class from the Python module
            py::object mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
            py::object cls = mod.attr("SchemaValidator");
            // Parse schema_json back to a dict for reconstruction
            py::object json_mod = py::module_::import("json");
            py::object schema_dict = json_mod.attr("loads")(self.schema_json());
            py::object config_dict = self.config_json().empty() ? py::none() : json_mod.attr("loads")(self.config_json());
            return py::make_tuple(cls, py::make_tuple(schema_dict, config_dict));
        });

    // SchemaSerializer
    py::class_<PySchemaSerializer>(m, "SchemaSerializer")
        .def(py::init<const py::dict&, const std::optional<py::dict>&>(),
             py::arg("schema"), py::arg("config") = py::none())
        .def(py::init<const py::dict&, const std::optional<py::dict>&, bool>(),
             py::arg("schema"), py::arg("config") = py::none(), py::arg("_use_prebuilt") = true)
        .def("to_python", &PySchemaSerializer::to_python,
             py::arg("value"), py::kw_only(),
             py::arg("mode") = py::none(), py::arg("include") = py::none(), py::arg("exclude") = py::none(),
             py::arg("by_alias") = py::none(), py::arg("exclude_unset") = false, py::arg("exclude_defaults") = false,
             py::arg("exclude_none") = false, py::arg("exclude_computed_fields") = false, py::arg("round_trip") = false,
             py::arg("warnings") = "warn", py::arg("fallback") = py::none(), py::arg("serialize_as_any") = false,
             py::arg("polymorphic_serialization") = py::none(), py::arg("context") = py::none())
        .def("to_json", &PySchemaSerializer::to_json,
             py::arg("value"), py::kw_only(),
             py::arg("indent") = py::none(), py::arg("ensure_ascii") = py::none(), py::arg("include") = py::none(),
             py::arg("exclude") = py::none(), py::arg("by_alias") = py::none(), py::arg("exclude_unset") = false,
             py::arg("exclude_defaults") = false, py::arg("exclude_none") = false, py::arg("exclude_computed_fields") = false,
             py::arg("round_trip") = false, py::arg("warnings") = "warn", py::arg("fallback") = py::none(),
             py::arg("serialize_as_any") = false, py::arg("polymorphic_serialization") = py::none(), py::arg("context") = py::none())
        .def("__repr__", &PySchemaSerializer::repr)
        // Pickle support: __reduce__ returns (cls, (schema, config))
        .def("__reduce__", [](const PySchemaSerializer& self) -> py::tuple {
            py::object mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
            py::object cls = mod.attr("SchemaSerializer");
            return py::make_tuple(cls, py::make_tuple(self.get_schema(), py::none()));
        });

    m.def("to_json", &to_json_fn,
          py::arg("value"), py::kw_only(), py::arg("indent") = py::none(), py::arg("ensure_ascii") = py::none(),
          py::arg("include") = py::none(), py::arg("exclude") = py::none(), py::arg("by_alias") = true,
          py::arg("exclude_none") = false, py::arg("round_trip") = false,
          py::arg("timedelta_mode") = "iso8601", py::arg("temporal_mode") = "iso8601",
          py::arg("bytes_mode") = "utf8", py::arg("inf_nan_mode") = "constants",
          py::arg("serialize_unknown") = false, py::arg("fallback") = py::none(),
          py::arg("serialize_as_any") = false, py::arg("polymorphic_serialization") = py::none(),
          py::arg("context") = py::none());

    m.def("to_jsonable_python", &to_jsonable_fn,
          py::arg("value"), py::kw_only(), py::arg("include") = py::none(), py::arg("exclude") = py::none(),
          py::arg("by_alias") = true, py::arg("exclude_none") = false, py::arg("round_trip") = false,
          py::arg("timedelta_mode") = "iso8601", py::arg("temporal_mode") = "iso8601",
          py::arg("bytes_mode") = "utf8", py::arg("inf_nan_mode") = "constants",
          py::arg("serialize_unknown") = false, py::arg("fallback") = py::none(),
          py::arg("serialize_as_any") = false, py::arg("polymorphic_serialization") = py::none(),
          py::arg("context") = py::none());

    // Url class - URL type with parsing and validation
    py::class_<Url>(m, "Url")
        .def(py::init([](const std::string& url_str) {
            return Url(url_str);
        }), py::arg("url"))
        .def("__repr__", [](const Url& u) { return "Url('" + u.str() + "')"; })
        .def("__str__", &Url::str)
        .def("__eq__", [](const Url& u, const py::object& other) {
            if (py::isinstance<Url>(other)) return u.str() == other.cast<Url>().str();
            if (py::isinstance<py::str>(other)) return u.str() == other.cast<std::string>();
            return false;
        })
        .def("__hash__", [](const Url& u) { return py::hash(py::str(u.str())); })
        .def_property_readonly("scheme", &Url::scheme)
        .def_property_readonly("host", &Url::host)
        .def_property_readonly("port", [](const Url& u) -> py::object {
            auto port = u.port();
            return port ? py::cast(*port) : py::none();
        })
        .def_property_readonly("path", &Url::path)
        .def_property_readonly("query", &Url::query)
        .def_property_readonly("fragment", &Url::fragment)
        .def_property_readonly("username", [](const Url& u) -> py::object {
            auto user = u.user();
            return user && !user->empty() ? py::cast(*user) : py::none();
        })
        .def_property_readonly("password", [](const Url& u) -> py::object {
            auto pw = u.password();
            return pw && !pw->empty() ? py::cast(*pw) : py::none();
        })
        .def_property_readonly("url", &Url::str);

    // MultiHostUrl class - URL with multiple hosts
    py::class_<MultiHostUrl>(m, "MultiHostUrl")
        .def(py::init([](const std::string& url_str) {
            return MultiHostUrl(url_str);
        }), py::arg("url"))
        .def("__repr__", [](const MultiHostUrl& u) { return "MultiHostUrl('" + u.str() + "')"; })
        .def("__str__", &MultiHostUrl::str)
        .def("__eq__", [](const MultiHostUrl& u, const py::object& other) {
            if (py::isinstance<MultiHostUrl>(other)) return u.str() == other.cast<MultiHostUrl>().str();
            if (py::isinstance<py::str>(other)) return u.str() == other.cast<std::string>();
            return false;
        })
        .def("__hash__", [](const MultiHostUrl& u) { return py::hash(py::str(u.str())); })
        .def_property_readonly("scheme", &MultiHostUrl::scheme)
        .def_property_readonly("hosts", [](const MultiHostUrl& u) {
            py::list result;
            for (const auto& h : u.hosts()) {
                py::dict host_dict;
                host_dict["host"] = h.host;
                if (h.port) host_dict["port"] = *h.port;
                else host_dict["port"] = py::none();
                result.append(host_dict);
            }
            return result;
        })
        .def_property_readonly("path", &MultiHostUrl::path)
        .def_property_readonly("query", &MultiHostUrl::query)
        .def_property_readonly("fragment", &MultiHostUrl::fragment)
        .def_property_readonly("username", [](const MultiHostUrl& u) -> py::object {
            auto user = u.user();
            return user && !user->empty() ? py::cast(*user) : py::none();
        })
        .def_property_readonly("password", [](const MultiHostUrl& u) -> py::object {
            auto pw = u.password();
            return pw && !pw->empty() ? py::cast(*pw) : py::none();
        })
        .def_property_readonly("url", &MultiHostUrl::str);
}
