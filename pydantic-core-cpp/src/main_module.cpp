#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include "pydantic_core/errors.hpp"
#include "pydantic_core/error_types.hpp"
#include "pydantic_core/types.hpp"
#include "pydantic_core/schema_validator.hpp"
#include "pydantic_core/serialization_config.hpp"

namespace py = pybind11;
using namespace pydantic_core;

std::string get_version() { return "2.46.4"; }

// ---------------------------------------------------------------------------
// Helper: convert a py::object to JSON string
// ---------------------------------------------------------------------------
static std::string pyobj_to_json_str(const py::object& obj) {
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
    // For function serializers
    py::object py_func;
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
        py_func = other.py_func;
        default_val = other.default_val;
        has_default_val = other.has_default_val;
        format_str = other.format_str;
    }

    py::object to_python(const py::object& value, bool json_mode, bool exc_none) const {
        // Type-specific logic
        if (type == "nullable" || type == "nullable-union") {
            if (value.is_none()) return py::none();
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none);
        }
        if (type == "union") {
            for (auto& c : children) {
                try { return c->to_python(value, json_mode, exc_none); } catch (...) {}
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
                        if (it != tagged.end()) return it->second->to_python(value, json_mode, exc_none);
                        break;
                    }
                }
            }
            for (auto& [t, c] : tagged) {
                try { return c->to_python(value, json_mode, exc_none); } catch (...) {}
            }
        }
        if (type == "default" || type == "with-default") {
            if (value.is_none() && has_default_val) return default_val;
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none);
        }
        if (type == "json") {
            if (!children.empty()) {
                auto inner = children[0]->to_python(value, json_mode, exc_none);
                std::string s = inner.cast<std::string>();
                return json_to_pyobj(s);
            }
        }
        if (type == "json-or-python") {
            if (json_mode && !children.empty()) return children[0]->to_python(value, true, exc_none);
            if (children.size() > 1) return children[1]->to_python(value, false, exc_none);
        }
        if (type == "enum") {
            if (py::hasattr(value, "value")) {
                auto ev = py::getattr(value, "value");
                if (!children.empty()) return children[0]->to_python(ev, json_mode, exc_none);
                return ev;
            }
        }
        if (!fields.empty()) {
            return serialize_fields(value, exc_none);
        }
        // Delegate model/dataclass/typed-dict to inner serializer
        if ((type == "model" || type == "dataclass" || type == "typed-dict") && !children.empty()) {
            return children[0]->to_python(value, json_mode, exc_none);
        }
        if (!py_func.is_none()) {
            if (type == "function-plain") return py_func(value);
            if (type == "function-after" || type == "function-before" || type == "function-wrap") {
                auto handler = [this, value, json_mode, exc_none](const py::object& v) {
                    if (!children.empty()) return children[0]->to_python(v, json_mode, exc_none);
                    return v;
                };
                return py_func(value, handler);
            }
        }
        // For list/dict/tuple/containers, serialize children
        if ((type == "list" || type == "set" || type == "frozenset") && !children.empty()) {
            py::list result;
            for (auto item : py::reinterpret_borrow<py::sequence>(value)) {
                result.append(children[0]->to_python(py::reinterpret_borrow<py::object>(item), json_mode, exc_none));
            }
            return std::move(result);
        }
        if (type == "dict" && !children.empty()) {
            py::dict result;
            auto d = value.cast<py::dict>();
            for (auto item : d) {
                auto k = py::reinterpret_borrow<py::object>(item.first);
                auto v = py::reinterpret_borrow<py::object>(item.second);
                auto out_v = children.size() > 1 ? children[1]->to_python(v, json_mode, exc_none) : v;
                result[k] = out_v;
            }
            return std::move(result);
        }
        if (type == "tuple" && !children.empty()) {
            py::list result;
            size_t i = 0;
            for (auto item : py::reinterpret_borrow<py::sequence>(value)) {
                auto v = py::reinterpret_borrow<py::object>(item);
                result.append(i < children.size() ? children[i]->to_python(v, json_mode, exc_none) : children.back()->to_python(v, json_mode, exc_none));
                i++;
            }
            return std::move(result);
        }
        return value;
    }

    std::string to_json(const py::object& value, bool ensure_ascii, int indent) const {
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
            return std::to_string(d);
        }
        if (type == "str" || type == "string" || type == "str-constrained") {
            return json_escape(value.cast<std::string>(), ensure_ascii);
        }
        if (type == "bytes") {
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
        if (type == "nullable" || type == "nullable-union") {
            if (value.is_none()) return "null";
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent);
        }
        if (type == "default" || type == "with-default") {
            if (value.is_none() && has_default_val) {
                if (!children.empty()) return children[0]->to_json(default_val, ensure_ascii, indent);
                return infer_json(default_val, ensure_ascii, indent);
            }
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent);
        }
        if (type == "to-string") {
            py::object inner = !children.empty() ? children[0]->to_python(value, false, false) : value;
            return json_escape(py::str(inner).cast<std::string>(), ensure_ascii);
        }
        if (!fields.empty()) {
            return serialize_fields_json(value, ensure_ascii, indent, false);
        }
        if (!py_func.is_none()) {
            auto result = type == "function-plain" ? py_func(value) : to_python(value, false, false);
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
        return json_escape(py::repr(value).cast<std::string>(), ensure_ascii);
    }

    py::object serialize_fields(const py::object& value, bool exc_none) const {
        py::dict result;
        py::dict main;
        if (py::isinstance<py::dict>(value)) main = value.cast<py::dict>();
        else if (py::hasattr(value, "__dict__")) main = py::getattr(value, "__dict__").cast<py::dict>();

        for (auto& [k, ser] : fields) {
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
            result[py::str(k)] = ser->to_python(fv, false, exc_none);
        }
        // Extra fields
        if (py::hasattr(value, "__pydantic_extra__")) {
            auto extra = py::getattr(value, "__pydantic_extra__");
            if (!extra.is_none()) {
                for (auto item : extra.cast<py::dict>()) {
                    std::string k = py::str(item.first).cast<std::string>();
                    if (fields.find(k) != fields.end()) continue;
                    py::object v = py::reinterpret_borrow<py::object>(item.second);
                    if (exc_none && v.is_none()) continue;
                    result[py::str(k)] = v;
                }
            }
        }
        return std::move(result);
    }

    std::string serialize_fields_json(const py::object& value, bool ensure_ascii, int indent, bool exc_none) const {
        std::string out = "{";
        bool first = true;
        py::dict main;
        if (py::isinstance<py::dict>(value)) main = value.cast<py::dict>();
        else if (py::hasattr(value, "__dict__")) main = py::getattr(value, "__dict__").cast<py::dict>();

        for (auto& [k, ser] : fields) {
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
            if (!first) out += ",";
            first = false;
            out += json_escape(k, ensure_ascii) + ":" + ser->to_json(fv, ensure_ascii, -1);
        }
        if (py::hasattr(value, "__pydantic_extra__")) {
            auto extra = py::getattr(value, "__pydantic_extra__");
            if (!extra.is_none()) {
                for (auto item : extra.cast<py::dict>()) {
                    std::string k = py::str(item.first).cast<std::string>();
                    if (fields.find(k) != fields.end()) continue;
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
    try {
        if (schema.contains("serialization")) {
            auto ser = schema["serialization"].cast<py::dict>();
            if (ser.contains("type")) {
                std::string st = ser["type"].cast<std::string>();
                if (st != "include-exclude-sequence" && st != "include-exclude-dict" && st != "base64")
                    type = st;
            }
        }
    } catch (...) {}

    auto node = std::make_shared<SerNode>();
    node->type = type;

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
        try { node->py_func = schema["function"]; } catch (...) {}
        if (type != "function-plain") { auto c = sub(); if (c) node->children.push_back(c); }
    }

    // model-fields, typed-dict, dataclass-args
    if (type == "model-fields" || type == "typed-dict" || type == "dataclass-args") {
        try {
            for (auto item : schema["fields"].cast<py::dict>()) {
                std::string k = py::str(item.first).cast<std::string>();
                auto fdef = item.second.cast<py::dict>();
                node->fields[k] = build_ser(fdef["schema"].cast<py::dict>(), defs);
            }
        } catch (...) {}
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

    explicit PySchemaSerializer(const py::dict& schema, const std::optional<py::dict>& = std::nullopt) {
        std::unordered_map<std::string, SerRef> defs;
        ser_ = build_ser(schema, defs);
    }

    // Overload that accepts _use_prebuilt (unused but needed for pydantic API)
    explicit PySchemaSerializer(const py::dict& schema, const std::optional<py::dict>& cfg, bool) {
        std::unordered_map<std::string, SerRef> defs;
        ser_ = build_ser(schema, defs);
        (void)cfg;
    }

    py::object to_python(const py::object& value, std::optional<std::string> mode,
                         std::optional<py::object>, std::optional<py::object>,
                         std::optional<bool>, bool, bool, bool exc_none,
                         bool, bool, py::object, std::optional<py::object>,
                         bool, std::optional<bool>, std::optional<py::object>) const {
        if (!ser_) throw std::runtime_error("Serializer not initialized");
        return ser_->to_python(value, mode && *mode == "json", exc_none);
    }

    py::bytes to_json(const py::object& value, std::optional<size_t>, std::optional<bool> ea,
                      std::optional<py::object>, std::optional<py::object>,
                      std::optional<bool>, bool, bool, bool exc_none,
                      bool, bool, py::object, std::optional<py::object>,
                      bool, std::optional<bool>, std::optional<py::object>) const {
        if (!ser_) throw std::runtime_error("Serializer not initialized");
        bool e = ea.value_or(false);
        std::string json = ser_->to_json(value, e, -1);
        return py::bytes(json);
    }

    std::string repr() const {
        return ser_ ? "SchemaSerializer(serializer=" + ser_->type + ")" : "SchemaSerializer()";
    }

private:
    SerRef ser_;
};

// ---------------------------------------------------------------------------
// Standalone to_json
// ---------------------------------------------------------------------------
static py::bytes to_json_fn(const py::object& value, std::optional<size_t>, std::optional<bool> ea,
    std::optional<py::object>, std::optional<py::object>, bool, bool, bool,
    std::string, std::string, std::string, std::string, bool,
    std::optional<py::object>, bool, std::optional<bool>, std::optional<py::object>) {
    SerRef any = std::make_shared<SerNode>();
    any->type = "any";
    return py::bytes(any->to_json(value, ea.value_or(false), -1));
}

static py::object to_jsonable_fn(const py::object& value, std::optional<py::object>, std::optional<py::object>,
    bool, bool, bool, std::string, std::string, std::string, std::string, bool,
    std::optional<py::object>, bool, std::optional<bool>, std::optional<py::object>) {
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

    py::class_<ValidationError>(m, "ValidationError")
        .def(py::init<const std::string&, InputType, const ValError&>())
        .def_property_readonly("title", &ValidationError::title)
        .def_property_readonly("error_count", &ValidationError::error_count)
        .def("errors", &ValidationError::errors).def("to_json", &ValidationError::to_json_string)
        .def("__str__", &ValidationError::to_json_string)
        .def("__repr__", [](const ValidationError& e) { return "ValidationError(" + e.to_json_string() + ")"; });
    py::class_<SchemaError>(m, "SchemaError").def(py::init<const std::string&>()).def("__str__", &SchemaError::what);
    py::class_<PydanticOmit>(m, "PydanticOmit").def(py::init<>());
    py::class_<PydanticUseDefault>(m, "PydanticUseDefault").def(py::init<>());

    // SchemaValidator
    py::class_<SchemaValidator>(m, "SchemaValidator")
        .def(py::init([](const py::object& schema, const py::object& config) {
            std::string sj = pyobj_to_json_str(schema);
            std::string cj = config.is_none() ? "" : pyobj_to_json_str(config);
            auto sv = std::make_unique<SchemaValidator>(sj, cj);
            // Store schema as Python attribute for model construction
            sv->repr();  // Force init
            return sv;
        }), py::arg("schema"), py::arg("config") = py::none())
        .def(py::init([](const py::object& schema, const py::object& config, bool) {
            std::string sj = pyobj_to_json_str(schema);
            std::string cj = config.is_none() ? "" : pyobj_to_json_str(config);
            auto sv = std::make_unique<SchemaValidator>(sj, cj);
            return sv;
        }), py::arg("schema"), py::arg("config") = py::none(), py::arg("_use_prebuilt") = true)
        .def("validate_python", [m](SchemaValidator& self, const py::object& input, py::object strict, py::object context, py::object self_instance,
                                    py::object, py::object, py::object, py::object) -> py::object {
            std::string ij = pyobj_to_json_str(input);
            std::string r = self.validate_python(ij, pyobj_to_bool(strict), std::nullopt);
            py::object validated = json_to_pyobj(r);

            // If self_instance provided, populate and return it
            if (!self_instance.is_none() && py::hasattr(self_instance, "__dict__")) {
                if (py::isinstance<py::dict>(validated)) {
                    py::dict d = self_instance.attr("__dict__");
                    for (auto item : validated.cast<py::dict>()) d[item.first] = item.second;
                }
                return self_instance;
            }

            return validated;
        }, py::arg("object"), py::arg("strict") = py::none(), py::arg("context") = py::none(), py::arg("self_instance") = py::none(),
             py::arg("extra") = py::none(), py::arg("from_attributes") = py::none(), py::arg("by_alias") = py::none(), py::arg("by_name") = py::none())
        .def("validate_json", [](SchemaValidator& self, const py::object& jd, py::object strict) {
            std::string js = py::isinstance<py::bytes>(jd) ? jd.cast<std::string>() : jd.cast<std::string>();
            return json_to_pyobj(self.validate_json(js, pyobj_to_bool(strict)));
        }, py::arg("json_data"), py::arg("strict") = py::none())
        .def("validate_strings", [](SchemaValidator& self, const py::object& sd, py::object strict) {
            return json_to_pyobj(self.validate_strings(pyobj_to_json_str(sd), pyobj_to_bool(strict)));
        }, py::arg("string_data"), py::arg("strict") = py::none())
        .def("isinstance_python", [](SchemaValidator& self, const py::object& input, py::object strict) {
            return self.isinstance_python(pyobj_to_json_str(input), pyobj_to_bool(strict));
        }, py::arg("object"), py::arg("strict") = py::none())
        .def("get_default_value", [](SchemaValidator& self, py::object strict) -> py::object {
            auto r = self.get_default_value(pyobj_to_bool(strict));
            return r ? json_to_pyobj(*r) : py::none();
        }, py::arg("strict") = py::none())
        .def("validate_assignment", [](SchemaValidator& self, const py::object& obj, const std::string& fn, const py::object& fv) {
            return json_to_pyobj(self.validate_assignment(pyobj_to_json_str(obj), fn, pyobj_to_json_str(fv)));
        }, py::arg("object"), py::arg("field_name"), py::arg("field_value"))
        .def_property_readonly("title", &SchemaValidator::title)
        .def("__repr__", &SchemaValidator::repr);

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
        .def("__repr__", &PySchemaSerializer::repr);

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
}
