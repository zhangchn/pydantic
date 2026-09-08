#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <set>
#include <cstdio>

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
    std::string field_name;
    py::object context;
    PySerializationInfo(bool round_trip_, std::string mode_ = "python", std::string field_name_ = "", py::object context_ = py::none()) 
        : round_trip(round_trip_), mode(std::move(mode_)), field_name(std::move(field_name_)), context(std::move(context_)) {}
};

// Runtime polymorphic-serialization flag for the current top-level
// to_python/to_json call (mirrors Rust SerializationExtra::polymorphic_serialization).
static thread_local std::optional<bool> g_polymorphic_serialization{};
static thread_local bool g_exclude_computed_fields = false;
// Recursion guard for the polymorphism trampoline.
static thread_local int g_trampoline_depth = 0;

// Polymorphism trampoline (mirrors Rust's PolymorphismTrampoline): when
// polymorphic serialization is enabled (runtime kwarg or schema config) and
// `value` is a strict subclass of `cls` carrying its own
// __pydantic_serializer__, return that serializer's output instead.
static bool try_polymorphic_trampoline(const py::object& value, const py::object& cls,
                                       bool enabled,
                                       bool want_json, bool ensure_ascii,
                                       const py::object& include, const py::object& exclude,
                                       bool by_alias, bool exclude_unset, bool exclude_defaults,
                                       bool exc_none, bool round_trip,
                                       std::optional<py::object>* out) {
    if (!enabled) return false;
    if (g_trampoline_depth >= 8) return false;
    if (!cls.ptr() || cls.is_none()) return false;
    try {
        if (py::type::of(value).equal(cls) || !py::hasattr(value, "__pydantic_serializer__")) {
            return false;
        }
        ++g_trampoline_depth;
        struct DepthGuard { ~DepthGuard() { --g_trampoline_depth; } } guard;
        py::object sub_ser = py::getattr(value, "__pydantic_serializer__");
        py::dict kw;
        kw["include"] = include;
        kw["exclude"] = exclude;
        kw["by_alias"] = by_alias;
        kw["exclude_unset"] = exclude_unset;
        kw["exclude_defaults"] = exclude_defaults;
        kw["exclude_none"] = exc_none;
        kw["round_trip"] = round_trip;
        if (g_polymorphic_serialization.has_value()) {
            kw["polymorphic_serialization"] = *g_polymorphic_serialization;
        }
        py::object res;
        if (want_json) {
            kw["ensure_ascii"] = ensure_ascii;
            res = sub_ser.attr("to_json")(value, **kw);
        } else {
            res = sub_ser.attr("to_python")(value, **kw);
        }
        *out = res;
        return true;
        return true;
    } catch (py::error_already_set&) {
        throw;
    } catch (...) {
        return false;
    }
}

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

// Partial JSON parsing (Rust: jiter::JsonValue::parse_with_config with
// allow_partial).  Tries json.loads first; on failure, repairs truncated JSON
// by closing open brackets/braces/quotes and dropping incomplete trailing
// key-value pairs, then retries.
static py::object json_to_pyobj_partial(const std::string& json_str) {
    py::object json_mod = py::module_::import("json");
    try {
        return json_mod.attr("loads")(json_str);
    } catch (const py::error_already_set&) {
        // Fall through to repair
    }

    // Walk the string tracking bracket/brace/quote state.  Build a repaired
    // string by closing any open containers and dropping an incomplete
    // trailing key-value pair (a key with no complete value, or a value that
    // is an unterminated string).
    std::string repaired;
    repaired.reserve(json_str.size() + 8);
    std::vector<char> open_stack;  // '{' or '['
    bool in_string = false;
    bool string_terminated = true;  // whether the last string was closed
    size_t i = 0;
    size_t n = json_str.size();
    // Track the position where the last complete value ended (for dropping
    // incomplete trailing pairs).
    size_t last_complete_end = 0;

    while (i < n) {
        char c = json_str[i];
        if (in_string) {
            if (c == '\\') {
                repaired += c;
                if (i + 1 < n) {
                    repaired += json_str[i + 1];
                    i += 2;
                    continue;
                }
                // Truncated escape sequence — drop the rest
                break;
            } else if (c == '"') {
                in_string = false;
                string_terminated = true;
                repaired += c;
            } else {
                repaired += c;
            }
        } else {
            if (c == '"') {
                in_string = true;
                string_terminated = false;
                repaired += c;
            } else if (c == '{' || c == '[') {
                open_stack.push_back(c);
                repaired += c;
            } else if (c == '}' || c == ']') {
                if (!open_stack.empty()) open_stack.pop_back();
                repaired += c;
                last_complete_end = repaired.size();
            } else if (c == ',' || c == ':') {
                repaired += c;
            } else {
                repaired += c;
                // Track end of a scalar value (number, true, false, null)
                if ((c >= '0' && c <= '9') || c == '-' || c == '.' ||
                    c == 't' || c == 'f' || c == 'n') {
                    last_complete_end = repaired.size();
                }
            }
        }
        i++;
    }

    // If we ended inside an unterminated string, drop back to the last
    // complete value (the key before the incomplete value).
    if (in_string) {
        // Find the last ':' or ',' before the unterminated string and truncate.
        // The incomplete value is the last key-value pair; drop it.
        size_t truncate_at = 0;
        // Walk back to find the last ',' or '{' that starts the incomplete pair.
        for (size_t j = repaired.size(); j > 0; j--) {
            if (repaired[j - 1] == ',' || repaired[j - 1] == '{') {
                truncate_at = (repaired[j - 1] == ',') ? (j - 1) : j;
                break;
            }
        }
        repaired = repaired.substr(0, truncate_at);
    }

    // Close any open containers.
    while (!open_stack.empty()) {
        char c = open_stack.back();
        open_stack.pop_back();
        repaired += (c == '{') ? '}' : ']';
    }

    // Trim trailing whitespace and a dangling comma.
    auto trim_trailing = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n' ||
                              s.back() == '\t' || s.back() == '\r')) {
            s.pop_back();
        }
        if (!s.empty() && s.back() == ',') {
            s.pop_back();
        }
    };
    trim_trailing(repaired);

    try {
        return json_mod.attr("loads")(repaired);
    } catch (const py::error_already_set&) {
        // If the repaired JSON is still invalid (e.g., a key with no value
        // like {"a": 1, "b": }), drop the last incomplete key-value pair and
        // retry.
        // Find the last ',' or '{' and truncate.
        for (size_t j = repaired.size(); j > 0; j--) {
            if (repaired[j - 1] == ',' || repaired[j - 1] == '{') {
                std::string truncated = (repaired[j - 1] == ',')
                    ? repaired.substr(0, j - 1)
                    : repaired.substr(0, j);
                // Re-close any open containers in the truncated string.
                std::vector<char> stack2;
                bool in_str2 = false;
                for (size_t k = 0; k < truncated.size(); k++) {
                    char c = truncated[k];
                    if (in_str2) {
                        if (c == '\\') { k++; continue; }
                        if (c == '"') in_str2 = false;
                    } else {
                        if (c == '"') in_str2 = true;
                        else if (c == '{' || c == '[') stack2.push_back(c);
                        else if (c == '}' || c == ']') { if (!stack2.empty()) stack2.pop_back(); }
                    }
                }
                while (!stack2.empty()) {
                    char c = stack2.back();
                    stack2.pop_back();
                    truncated += (c == '{') ? '}' : ']';
                }
                trim_trailing(truncated);
                try {
                    return json_mod.attr("loads")(truncated);
                } catch (const py::error_already_set&) {
                    // Continue trying earlier truncation points
                }
                break;
            }
        }
        // If all repair attempts failed, re-raise the original error
        throw;
    }
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

// ---------------------------------------------------------------------------
// Shared JSON-mode conversion helpers
// ---------------------------------------------------------------------------

static std::string b64_encode_string(const std::string& b) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    for (size_t i = 0; i < b.size(); i += 3) {
        uint32_t n = ((uint8_t)b[i] << 16);
        if (i+1 < b.size()) n |= ((uint8_t)b[i+1] << 8);
        if (i+2 < b.size()) n |= (uint8_t)b[i+2];
        enc += b64[(n>>18)&0x3F]; enc += b64[(n>>12)&0x3F];
        enc += (i+1<b.size()) ? b64[(n>>6)&0x3F] : '=';
        enc += (i+2<b.size()) ? b64[n&0x3F] : '=';
    }
    return enc;
}

// Convert one typed leaf value for JSON-mode serialization (SchemaSerializer
// mode="json" and to_jsonable_python). Returns true and sets `out` when the
// (type, value) pair has a JSON-compatible form; false otherwise.
static bool json_leaf_convert(const std::string& type, const py::object& value,
                              const std::string& ser_json_bytes,
                              const std::string& ser_json_timedelta,
                              py::object& out) {
    try {
        if (type == "bytes" && py::isinstance<py::bytes>(value)) {
            std::string b = value.cast<std::string>();
            if (ser_json_bytes == "base64") { out = py::str(b64_encode_string(b)); return true; }
            if (ser_json_bytes == "hex") {
                static const char* hex_chars = "0123456789abcdef";
                std::string enc;
                for (unsigned char c : b) {
                    enc += hex_chars[c >> 4];
                    enc += hex_chars[c & 0x0F];
                }
                out = py::str(enc);
                return true;
            }
            // utf8: decodes UTF-8; invalid input raises and the caller keeps
            // handling the value (or reports an error).
            out = py::str(value.cast<py::bytes>().operator std::string());
            return true;
        }
        if (type == "decimal" || type == "uuid" || type == "ipaddress" ||
            type == "ipv4address" || type == "ipv6address" ||
            type == "ipv4interface" || type == "ipv6interface" ||
            type == "ipv4network" || type == "ipv6network") {
            out = py::str(value);
            return true;
        }
        if (type == "datetime" || type == "date" || type == "time") {
            py::object iso = value.attr("isoformat")();
            std::string s = py::str(iso).cast<std::string>();
            if (s.size() >= 6 && s.substr(s.size() - 6) == "+00:00") {
                s = s.substr(0, s.size() - 6) + "Z";
            }
            out = py::str(s);
            return true;
        }
        if (type == "timedelta") {
            long days = value.attr("days").cast<long>();
            long seconds = value.attr("seconds").cast<long>();
            long microseconds = value.attr("microseconds").cast<long>();
            if (ser_json_timedelta == "float") {
                double total = days * 86400.0 + seconds + microseconds / 1000000.0;
                out = py::float_(total);
                return true;
            }
            double total_seconds = days * 86400.0 + seconds + microseconds / 1000000.0;
            bool negative = total_seconds < 0;
            if (negative) {
                days = -days; seconds = -seconds; microseconds = -microseconds;
                if (microseconds < 0) { microseconds += 1000000; seconds--; }
                if (seconds < 0) { seconds += 86400; days--; }
                if (days < 0) { days = 0; seconds = 0; microseconds = 0; }
            }
            long hours = seconds / 3600;
            seconds = seconds % 3600;
            long mins = seconds / 60;
            long secs = seconds % 60;
            std::string result = negative ? "-P" : "P";
            if (days > 0) result += std::to_string(days) + "D";
            bool has_time = hours > 0 || mins > 0 || secs > 0 || microseconds > 0;
            if (has_time) {
                result += "T";
                if (hours > 0) result += std::to_string(hours) + "H";
                if (mins > 0) result += std::to_string(mins) + "M";
                if (secs > 0 || microseconds > 0) {
                    if (microseconds > 0) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%ld.%06ld", secs, microseconds);
                        std::string s(buf);
                        auto last = s.find_last_not_of('0');
                        if (last != std::string::npos) s.erase(last + 1);
                        if (s.back() == '.') s.pop_back();
                        result += s + "S";
                    } else {
                        result += std::to_string(secs) + "S";
                    }
                }
            }
            if (result == "P" || result == "-P") result = "PT0S";
            out = py::str(result);
            return true;
        }
    } catch (...) {
        PyErr_Clear();
    }
    return false;
}

// ============================================================================
// include/exclude filter helpers (mirror pydantic-core serializers/filter.rs)
// ============================================================================

// Whether a filter value is Ellipsis or True (pydantic V1 compat)
static bool is_ellipsis_like(const py::object& v) {
    if (v.is(py::ellipsis())) return true;
    if (py::isinstance<py::bool_>(v)) return v.cast<bool>();
    return false;
}

// Convert a filter value (dict or set) to a dict: {item: Ellipsis} for sets
static py::dict filter_as_dict(const py::object& v) {
    if (py::isinstance<py::dict>(v)) {
        return v.cast<py::dict>().attr("copy")().cast<py::dict>();
    }
    if (py::isinstance<py::set>(v)) {
        py::dict d;
        for (auto item : v) {
            d[item] = py::ellipsis();
        }
        return d;
    }
    throw py::type_error(
        "`include` and `exclude` must be of type `dict[str | int, <recursive> | ...] | set[str | int | ...]`");
}

// Merge an item-specific filter with the "__all__" filter (pydantic V1 rules)
static py::object merge_dicts(const py::object& item_value, const py::object& all_value);

// Look up `key` and "__all__" in a filter dict, merging per V1 rules.
// Returns py::none() when neither key is present.  Note: avoid ternaries
// between py::object and py::none — py::none's implicit object ctor throws
// on non-None values, and ternary common-type resolution can pick it.
static py::object merge_all_value(const py::dict& d, const py::object& key) {
    bool has_item = d.contains(key);
    bool has_all = d.contains(py::str("__all__"));
    if (!has_item && !has_all) return py::none();

    py::object item_value;
    if (has_item) {
        item_value = d[key];
    } else {
        item_value = py::none();
    }
    py::object all_value;
    if (has_all) {
        all_value = d[py::str("__all__")];
    } else {
        all_value = py::none();
    }

    if (has_item && has_all) {
        if (is_ellipsis_like(item_value) || is_ellipsis_like(all_value)) {
            return item_value;
        }
        return merge_dicts(item_value, all_value);
    }
    if (has_item) return item_value;
    return all_value;
}

static py::object merge_dicts(const py::object& item_value, const py::object& all_value) {
    py::dict item_dict = filter_as_dict(item_value);
    if (py::isinstance<py::dict>(all_value)) {
        for (auto kv : all_value.cast<py::dict>()) {
            py::object all_key = py::reinterpret_borrow<py::object>(kv.first);
            py::object all_val = py::reinterpret_borrow<py::object>(kv.second);
            if (item_dict.contains(all_key)) {
                py::object iv = item_dict[all_key];
                if (is_ellipsis_like(iv)) continue;
                if (!is_ellipsis_like(all_val)) {
                    item_dict[all_key] = merge_dicts(iv, all_val);
                }
            } else {
                item_dict[all_key] = all_val;
            }
        }
    } else if (py::isinstance<py::set>(all_value)) {
        for (auto item : all_value) {
            if (!item_dict.contains(item)) {
                item_dict[item] = py::ellipsis();
            }
        }
    } else {
        throw py::type_error(
            "'__all__' key of `include` and `exclude` must be of type `dict[str | int, <recursive> | ...] | set[str | int | ...]`");
    }
    return item_dict;
}

// ---------------------------------------------------------------------------
// Serializer warnings (Rust: serializers/extra.rs CollectWarnings + final_check).
// Warnings collected per top-level to_python/to_json call are emitted as a
// UserWarning (mode "warn") or raised as PydanticSerializationError ("error")
// when that call finishes.
// ---------------------------------------------------------------------------
enum class SerWarnMode { None, Warn, Error };

struct SerWarnFrame {
    bool enabled = false;
    bool as_error = false;
    std::vector<std::string> items;
};

static std::vector<SerWarnFrame>& ser_warn_stack() {
    static thread_local std::vector<SerWarnFrame> stack;
    return stack;
}

static SerWarnMode ser_warnings_mode(const py::object& warnings) {
    if (warnings.is_none()) return SerWarnMode::None;
    if (py::isinstance<py::bool_>(warnings)) return warnings.cast<bool>() ? SerWarnMode::Warn : SerWarnMode::None;
    if (py::isinstance<py::str>(warnings)) {
        std::string s = warnings.cast<std::string>();
        if (s == "none") return SerWarnMode::None;
        if (s == "error") return SerWarnMode::Error;
        return SerWarnMode::Warn;  // "warn" (default)
    }
    return SerWarnMode::Warn;
}

static void ser_warn_enter(const py::object& warnings) {
    SerWarnMode m = ser_warnings_mode(warnings);
    ser_warn_stack().push_back(SerWarnFrame{m != SerWarnMode::None, m == SerWarnMode::Error, {}});
}

static void ser_warn_leave(bool discard) {
    auto& stack = ser_warn_stack();
    if (stack.empty()) return;
    SerWarnFrame frame = std::move(stack.back());
    stack.pop_back();
    if (discard || frame.items.empty()) return;
    std::string message = "Pydantic serializer warnings:\n  ";
    for (size_t i = 0; i < frame.items.size(); ++i) {
        if (i) message += "\n  ";
        message += frame.items[i];
    }
    if (frame.as_error) {
        throw PydanticSerializationError(message);
    }
    // Warn as UserWarning; if a filter turns it into an error, propagate it.
    if (PyErr_WarnEx(PyExc_UserWarning, message.c_str(), 1) < 0) {
        throw py::error_already_set();
    }
}

static void ser_warn_register(const std::string& text) {
    auto& stack = ser_warn_stack();
    if (stack.empty() || !stack.back().enabled) return;
    stack.back().items.push_back(text);
}

static std::string ser_warn_input_type(const py::object& value) {
    try {
        py::object t = py::reinterpret_borrow<py::object>(value.get_type());
        return py::str(t.attr("__name__")).cast<std::string>();
    } catch (...) { return "<unknown>"; }
}

static std::string ser_warn_input_repr(const py::object& value) {
    try {
        std::string r = py::repr(value).cast<std::string>();
        // Rust truncate_safe_repr caps very long reprs at ~100 chars.
        if (r.size() > 100) r = r.substr(0, 99) + "\u2026";
        return r;
    } catch (...) { return "<unrepresentable>"; }
}

static void ser_warn_unexpected_value(const std::string& field_name,
                                      const std::string& field_type,
                                      const py::object& value) {
    if (value.is_none()) return;  // Rust special-cases None: no warning
    std::string type_name = ser_warn_input_type(value);
    std::string value_str = ser_warn_input_repr(value);
    std::string msg = "Expected `" + field_type + "` - serialized value may not be as expected";
    if (field_name.empty()) {
        msg += " [input_value=" + value_str + ", input_type=" + type_name + "]";
    } else {
        msg += " [field_name='" + field_name + "', input_value=" + value_str + ", input_type=" + type_name + "]";
    }
    ser_warn_register("PydanticSerializationUnexpectedValue(" + msg + ")");
}

// Result of applying an include/exclude filter to a key/index
struct SerFilterResult {
    bool omit = false;                 // true = drop the field/item
    py::object include = py::none();   // sub-filter for the nested value
    py::object exclude = py::none();   // sub-filter for the nested value
};

// Map a negative index to a positive one: key % len (Python mod semantics)
static py::object map_negative_index(const py::object& key, py::ssize_t len) {
    if (py::isinstance<py::int_>(key)) {
        py::ssize_t i = key.cast<py::ssize_t>();
        if (i < 0) return py::int_(((i % len) + len) % len);
    }
    return key;
}

// Map all negative keys/items in an include/exclude object (dict or set)
static py::object map_negative_indices(const py::object& obj, py::ssize_t len) {
    if (py::isinstance<py::dict>(obj)) {
        py::dict out;
        for (auto kv : obj.cast<py::dict>()) {
            py::object k = py::reinterpret_borrow<py::object>(kv.first);
            py::object v = py::reinterpret_borrow<py::object>(kv.second);
            out[map_negative_index(k, len)] = v;
        }
        return out;
    }
    if (py::isinstance<py::set>(obj)) {
        py::set out;
        for (auto item : obj) {
            out.add(map_negative_index(py::reinterpret_borrow<py::object>(item), len));
        }
        return out;
    }
    return obj;
}

// Apply call-time include/exclude to a key (or index).  Mirrors the Rust
// FilterLogic::filter with default_filter = true (no schema-level filter).
static SerFilterResult apply_ser_filter(const py::object& key, const py::object& include,
                                        const py::object& exclude) {
    SerFilterResult out;
    py::object next_exclude = py::none();

    // Exclude handling
    if (!exclude.is_none()) {
        if (py::isinstance<py::dict>(exclude)) {
            py::object exc_value = merge_all_value(exclude.cast<py::dict>(), key);
            if (!exc_value.is_none()) {
                if (is_ellipsis_like(exc_value)) {
                    out.omit = true;
                    return out;
                }
                next_exclude = exc_value;
            }
        } else if (py::isinstance<py::set>(exclude)) {
            py::set eset = exclude.cast<py::set>();
            if (eset.contains(key) || eset.contains(py::str("__all__"))) {
                out.omit = true;
                return out;
            }
        } else if (py::hasattr(exclude, "__contains__")) {
            bool c1 = false, c2 = false;
            try { c1 = py::cast<bool>(exclude.attr("__contains__")(key)); } catch (...) {}
            try { c2 = py::cast<bool>(exclude.attr("__contains__")(py::str("__all__"))); } catch (...) {}
            if (c1 || c2) {
                out.omit = true;
                return out;
            }
        } else {
            throw py::type_error("`exclude` argument must be a set or dict.");
        }
    }

    // Include handling
    if (!include.is_none()) {
        if (py::isinstance<py::dict>(include)) {
            py::dict incd = include.cast<py::dict>();
            // A key that is present -- even when its value is None (meaning
            // "include the whole item") -- must include the field; only a key
            // that is absent should omit it (Rust merge_all_value returns
            // Some(None) for a stored None value, distinct from absent).
            bool present = incd.contains(key) || incd.contains(py::str("__all__"));
            if (present) {
                py::object inc_value = merge_all_value(incd, key);
                if (inc_value.is_none() || is_ellipsis_like(inc_value)) {
                    out.include = py::none();
                } else {
                    out.include = inc_value;
                }
                out.exclude = next_exclude;
                return out;
            }
            out.omit = true;  // key not in include
            return out;
        }
        if (py::isinstance<py::set>(include)) {
            py::set iset = include.cast<py::set>();
            if (iset.contains(key) || iset.contains(py::str("__all__"))) {
                out.include = py::none();
                out.exclude = next_exclude;
                return out;
            }
            out.omit = true;  // key not in include
            return out;
        }
        if (py::hasattr(include, "__contains__")) {
            bool c1 = false, c2 = false;
            try { c1 = py::cast<bool>(include.attr("__contains__")(key)); } catch (...) {}
            try { c2 = py::cast<bool>(include.attr("__contains__")(py::str("__all__"))); } catch (...) {}
            if (c1 || c2) {
                out.include = py::none();
                out.exclude = next_exclude;
                return out;
            }
            out.omit = true;
            return out;
        }
        throw py::type_error("`include` argument must be a set or dict.");
    }

    // No include filter: keep the item, propagate the exclude sub-filter
    if (!next_exclude.is_none()) {
        out.exclude = next_exclude;
    }
    return out;
}

// Thread-local recursion guard mirroring Rust's RecursionState
// (recursion_guard.rs): re-entering the same (object, definition node)
// pair is a cycle; depth over the limit is too deep.  Both surface as
// ValueError in Rust (serializers/extra.rs).
struct SerRecursionState {
    std::set<std::pair<const void*, const void*>> active;
    size_t depth = 0;
    static SerRecursionState& get() {
        static thread_local SerRecursionState s;
        return s;
    }
};

// The MISSING sentinel object, shared with pydantic_core_cpp.MISSING. Used by
// the serializer to omit MISSING-valued fields (Rust exclude_field_by_value)
// and to validate standalone 'missing-sentinel' values.
static py::object missing_sentinel_obj() {
    return py::module_::import("pydantic_core_cpp").attr("MISSING");
}

struct SerNode {
    std::string type;
    // Rust-style serializer display name, e.g. "list[int]" (warnings).
    static std::string type_name_for_warning(const SerRef& n);
    // Whether a Python value is compatible with this node's declared type.
    static bool value_matches_type(const SerRef& n, const py::object& v);
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
    // Fields excluded at schema level (Field(exclude=True))
    std::unordered_set<std::string> field_excluded;
    // Set of computed field names (excluded when round_trip=True)
    std::unordered_set<std::string> computed_fields_;
    // For function serializers
    py::object py_func;
    bool info_arg = false;
    bool is_field_serializer = false;
    std::string when_used = "always";  // "always", "unless-none", "json", "json-unless-none"
    // For default
    py::object default_val;
    bool has_default_val = false;
    // For default: default_factory (Rust DefaultType::DefaultFactory)
    py::object default_factory;
    bool default_factory_takes_data = false;
    // For format
    std::string format_str;
    // For root models
    bool root_model = false;
    // For model/dataclass serializers: expected Python class (union discrimination)
    py::object class_;
    // Polymorphic serialization enabled via schema config (Rust enabled_from_config)
    bool polymorphic_from_config = false;
    // For inf/nan serialization mode: "constants" (default) or "strings"
    std::string inf_nan_mode = "constants";
    // For bytes serialization: "utf8" (default), "base64", or "hex"
    std::string ser_json_bytes = "utf8";
    // For timedelta serialization: "iso8601" (default) or "float"
    std::string ser_json_timedelta = "iso8601";

    // Copy content from another node into this one (preserves shared_ptr identity)
    void copy_from(const SerNode& other) {
        type = other.type;
        children = other.children;
        tagged = other.tagged;
        fields = other.fields;
        field_order = other.field_order;
        field_aliases = other.field_aliases;
        field_exclude_if = other.field_exclude_if;
        field_excluded = other.field_excluded;
        computed_fields_ = other.computed_fields_;
        py_func = other.py_func;
        info_arg = other.info_arg;
        is_field_serializer = other.is_field_serializer;
        when_used = other.when_used;
        default_val = other.default_val;
        has_default_val = other.has_default_val;
        default_factory = other.default_factory;
        default_factory_takes_data = other.default_factory_takes_data;
        format_str = other.format_str;
        root_model = other.root_model;
        class_ = other.class_;
        polymorphic_from_config = other.polymorphic_from_config;
        inf_nan_mode = other.inf_nan_mode;
        ser_json_bytes = other.ser_json_bytes;
        ser_json_timedelta = other.ser_json_timedelta;
    }

    py::object to_python(const py::object& value, bool json_mode, bool exc_none, bool round_trip = false,
                         const py::object& include = py::none(),
                         const py::object& exclude = py::none(),
                         bool by_alias = false,
                         bool exclude_unset = false,
                         bool exclude_defaults = false,
                         const py::object& context = py::none()) const {
        // Type-specific logic
        // definition-ref: apply the recursion guard (Rust definitions.rs:
        // state.recursion_guard(value, definition.id())), then delegate.
        if (type == "definition-ref" && !children.empty()) {
            auto& g = SerRecursionState::get();
            auto pair = std::make_pair(value.ptr(), children[0].get());
            if (!g.active.insert(pair).second) {
                throw py::value_error("Circular reference detected (id repeated)");
            }
            ++g.depth;
            if (g.depth > 255) {
                --g.depth;
                g.active.erase(pair);
                throw py::value_error("Circular reference detected (depth exceeded)");
            }
            struct GuardPop {
                SerRecursionState* g; std::pair<const void*, const void*> pair;
                ~GuardPop() { --g->depth; g->active.erase(pair); }
            } pop{&g, pair};
            return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
        }
        if (type == "lax-or-strict") {
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "is-instance" || type == "is-subclass") {
            return value;
        }
        if (type == "missing-sentinel") {
            py::object missing = missing_sentinel_obj();
            if (value.is(missing)) return value;
            py::object exc_type = py::module_::import("pydantic_core_cpp").attr("PydanticSerializationUnexpectedValue");
            PyErr_SetString(exc_type.ptr(), "Expected 'MISSING' sentinel");
            throw py::error_already_set();
        }
        if (type == "nullable" || type == "nullable-union") {
            if (value.is_none()) return py::none();
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "union") {
            for (auto& c : children) {
                try { return c->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults); } catch (...) {}
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
                        if (it != tagged.end()) return it->second->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
                        break;
                    }
                }
            }
            for (auto& [t, c] : tagged) {
                try { return c->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults); } catch (...) {}
            }
        }
        if (type == "default" || type == "with-default") {
            if (value.is_none() && has_default_val) return default_val;
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "json") {
            if (round_trip) {
                // Serialize value to JSON string, return as Python string
                std::string json_str;
                if (!children.empty()) {
                    json_str = children[0]->to_json(value, false, -1, round_trip, py::none(), py::none(), false, false, false, exc_none);
                } else {
                    json_str = infer_json(value, false, -1);
                }
                return py::cast(json_str);
            }
            // Non-round-trip: delegate to inner serializer
            if (!children.empty()) {
                return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
            }
        }
        if (type == "json-or-python") {
            if (json_mode && !children.empty()) return children[0]->to_python(value, true, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
            if (children.size() > 1) return children[1]->to_python(value, false, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
        }
        if (type == "enum") {
            if (!json_mode) {
                // Python mode: return the Enum member as-is
                return value;
            }
            if (py::hasattr(value, "value")) {
                auto ev = py::getattr(value, "value");
                if (!children.empty()) return children[0]->to_python(ev, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
                return ev;
            }
        }
        if (!fields.empty() || type == "model-fields" || type == "typed-dict" || type == "dataclass-args") {
            // A model/dataclass/typed-dict always serializes to a dict, even
            // when it declares no fields (empty model -> {}).
            return serialize_fields(value, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context, json_mode);
        }
        // Polymorphism trampoline (Rust PolymorphismTrampoline): a strict
        // subclass instance with its own __pydantic_serializer__ is serialized
        // through that serializer when enabled. Applies to model/dataclass
        // nodes and to function wrappers around them (model_serializer etc.).
        if (class_.ptr() && !class_.is_none() &&
            (type == "model" || type == "dataclass" || type == "function-plain" ||
             type == "function-after" || type == "function-before" || type == "function-wrap")) {
            std::optional<py::object> poly_out;
            if (try_polymorphic_trampoline(value, class_,
                                           g_polymorphic_serialization.value_or(polymorphic_from_config),
                                           false, false, include, exclude,
                                           by_alias, exclude_unset, exclude_defaults,
                                           exc_none, round_trip, &poly_out) && poly_out) {
                return *poly_out;
            }
        }
        // Delegate model/dataclass/typed-dict to inner serializer
        if ((type == "model" || type == "dataclass" || type == "typed-dict") && !children.empty()) {
            // Union discrimination: a model serializer only accepts instances of
            // its expected class; otherwise raise so the union tries next branch
            if (!class_.is_none() && !py::isinstance(value, class_)) {
                throw std::runtime_error("Value is not an instance of the expected model class");
            }
            // For root models, extract the 'root' attribute before delegating
            if (root_model && py::hasattr(value, "root")) {
                auto root_val = py::getattr(value, "root");
                // If the child is a field serializer, pass the model instance
                if (!children.empty() && children[0]->is_field_serializer && children[0]->py_func.ptr() && !children[0]->py_func.is_none()) {
                    auto child = children[0];
                    // Create handler for wrap mode
                    if (child->type == "function-wrap") {
                        py::object handler = py::cpp_function([child, root_val, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context](const py::object& v) -> py::object {
                            if (!child->children.empty()) {
                                return child->children[0]->to_python(v, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
                            }
                            return v;
                        });
                        if (child->info_arg) {
                            PySerializationInfo info(round_trip, json_mode ? "json" : "python", "root", context);
                            return child->py_func(value, root_val, handler, py::cast(info));
                        } else {
                            return child->py_func(value, root_val, handler);
                        }
                    } else {
                        // function-plain
                        if (child->info_arg) {
                            PySerializationInfo info(round_trip, json_mode ? "json" : "python", "root", context);
                            return child->py_func(value, root_val, py::cast(info));
                        } else {
                            return child->py_func(value, root_val);
                        }
                    }
                }
                return children[0]->to_python(root_val, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
            }
            return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
        }
        // Note: py_func may be a default-constructed py::object (null handle)
        // when no serialization function was assigned, so check ptr() rather
        // than is_none() (which is false for a null handle).
        if (py_func.ptr() && !py_func.is_none()) {
            // Check when_used
            bool skip_serializer = false;
            if (when_used == "json" || when_used == "json-unless-none") {
                if (!json_mode) skip_serializer = true;
            }
            if ((when_used == "unless-none" || when_used == "json-unless-none") && value.is_none()) {
                skip_serializer = true;
            }
            if (skip_serializer) {
                if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
                return value;
            }
            if (type == "function-plain") {
                if (info_arg) {
                    PySerializationInfo info(round_trip, json_mode ? "json" : "python", "", context);
                    return py_func(value, py::cast(info));
                }
                return py_func(value);
            }
            if (type == "function-after" || type == "function-before" || type == "function-wrap") {
                py::object handler = py::cpp_function([this, value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context](const py::object& v) -> py::object {
                    if (!children.empty()) return children[0]->to_python(v, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
                    return v;
                });
                if (type == "function-wrap") {
                    if (info_arg) {
                        PySerializationInfo info(round_trip, json_mode ? "json" : "python", "", context);
                        return py_func(value, handler, py::cast(info));
                    } else {
                        return py_func(value, handler);
                    }
                }
                try {
                    if (info_arg) {
                        PySerializationInfo info(round_trip, json_mode ? "json" : "python", "", context);
                        return py_func(value, handler, py::cast(info));
                    } else {
                        return py_func(value, handler);
                    }
                } catch (...) {
                    PyErr_Clear();
                    try {
                        if (info_arg) {
                            PySerializationInfo info(round_trip, json_mode ? "json" : "python", "", context);
                            return py_func(value);
                        } else {
                            return py_func(value);
                        }
                    } catch (...) {
                        PyErr_Clear();
                        return value;
                    }
                }
            }
        }
        // For list/dict/tuple/containers, serialize children
        if ((type == "list" || type == "set" || type == "frozenset" || type == "generator") && !children.empty()) {
            py::iterable seq = py::reinterpret_borrow<py::iterable>(value);
            py::ssize_t len = py::len(seq);
            py::object inc;
            if (include.is_none()) {
                inc = py::none();
            } else {
                inc = map_negative_indices(include, len);
            }
            py::object exc;
            if (exclude.is_none()) {
                exc = py::none();
            } else {
                exc = map_negative_indices(exclude, len);
            }
            py::ssize_t idx = 0;
            if (type == "set") {
                if (json_mode) {
                    // JSON has no set type: serialize as an array
                    py::list jresult;
                    for (auto item : seq) {
                        auto next = apply_ser_filter(py::int_(idx), inc, exc);
                        if (!next.omit) {
                            jresult.append(children[0]->to_python(py::reinterpret_borrow<py::object>(item), json_mode, exc_none, round_trip, next.include, next.exclude));
                        }
                        idx++;
                    }
                    return std::move(jresult);
                }
                py::set result;
                for (auto item : seq) {
                    auto next = apply_ser_filter(py::int_(idx), inc, exc);
                    if (!next.omit) {
                        result.add(children[0]->to_python(py::reinterpret_borrow<py::object>(item), json_mode, exc_none, round_trip, next.include, next.exclude));
                    }
                    idx++;
                }
                return std::move(result);
            } else if (type == "frozenset") {
                if (json_mode) {
                    // JSON has no frozenset type: serialize as an array
                    py::list jtemp;
                    for (auto item : seq) {
                        auto next = apply_ser_filter(py::int_(idx), inc, exc);
                        if (!next.omit) {
                            jtemp.append(children[0]->to_python(py::reinterpret_borrow<py::object>(item), json_mode, exc_none, round_trip, next.include, next.exclude));
                        }
                        idx++;
                    }
                    return std::move(jtemp);
                }
                py::set temp;
                for (auto item : seq) {
                    auto next = apply_ser_filter(py::int_(idx), inc, exc);
                    if (!next.omit) {
                        temp.add(children[0]->to_python(py::reinterpret_borrow<py::object>(item), json_mode, exc_none, round_trip, next.include, next.exclude));
                    }
                    idx++;
                }
                return py::frozenset(temp);
            } else {
                py::list result;
                for (auto item : seq) {
                    auto next = apply_ser_filter(py::int_(idx), inc, exc);
                    if (!next.omit) {
                        result.append(children[0]->to_python(py::reinterpret_borrow<py::object>(item), json_mode, exc_none, round_trip, next.include, next.exclude));
                    }
                    idx++;
                }
                return std::move(result);
            }
        }
        if (type == "dict" && !children.empty()) {
            py::dict result;
            auto d = value.cast<py::dict>();
            for (auto item : d) {
                auto k = py::reinterpret_borrow<py::object>(item.first);
                auto v = py::reinterpret_borrow<py::object>(item.second);
                auto next = apply_ser_filter(k, include, exclude);
                if (next.omit) continue;
                auto out_k = children[0]->to_python(k, json_mode, exc_none, round_trip, next.include, next.exclude);
                auto out_v = children.size() > 1 ? children[1]->to_python(v, json_mode, exc_none, round_trip, next.include, next.exclude) : v;
                result[out_k] = out_v;
            }
            return std::move(result);
        }
        if (type == "tuple" && !children.empty()) {
            py::list temp;
            auto seq = py::reinterpret_borrow<py::sequence>(value);
            py::ssize_t len = py::len(seq);
            py::object inc;
            if (include.is_none()) {
                inc = py::none();
            } else {
                inc = map_negative_indices(include, len);
            }
            py::object exc;
            if (exclude.is_none()) {
                exc = py::none();
            } else {
                exc = map_negative_indices(exclude, len);
            }
            size_t i = 0;
            for (auto item : seq) {
                auto next = apply_ser_filter(py::int_(static_cast<py::ssize_t>(i)), inc, exc);
                if (!next.omit) {
                    auto v = py::reinterpret_borrow<py::object>(item);
                    temp.append(i < children.size() ? children[i]->to_python(v, json_mode, exc_none, round_trip, next.include, next.exclude) : children.back()->to_python(v, json_mode, exc_none, round_trip, next.include, next.exclude));
                }
                i++;
            }
            return py::tuple(temp);
        }
        // In json mode, convert leaf values to their JSON-compatible form
        // (bytes→str, decimal/uuid→str, datetime→ISO string, timedelta→ISO
        // duration or float, etc.), mirroring Rust's mode="json" behavior.
        if (json_mode) {
            py::object converted;
            if (json_leaf_convert(type, value, ser_json_bytes, ser_json_timedelta, converted)) {
                return converted;
            }
            // Enum members serialize as their value
            if (type == "enum" && py::hasattr(value, "value")) {
                auto ev = py::getattr(value, "value");
                if (!children.empty()) {
                    return children[0]->to_python(ev, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
                }
                return ev;
            }
            // Un-typed set/generator: JSON has no set type, emit a plain list
            if ((type == "set" || type == "frozenset" || type == "generator") && children.empty()) {
                py::list out;
                for (auto item : py::reinterpret_borrow<py::iterable>(value)) {
                    out.append(py::reinterpret_borrow<py::object>(item));
                }
                return std::move(out);
            }
        }
        return value;
    }

    std::string to_json(const py::object& value, bool ensure_ascii, int indent, bool round_trip = false,
                         const py::object& include = py::none(),
                         const py::object& exclude = py::none(),
                         bool by_alias = false,
                         bool exclude_unset = false,
                         bool exclude_defaults = false,
                         bool exc_none = false,
                         const py::object& context = py::none()) const {
        // definition-ref: recursion guard (Rust definitions.rs), then delegate.
        if (type == "definition-ref" && !children.empty()) {
            auto& g = SerRecursionState::get();
            auto pair = std::make_pair(value.ptr(), children[0].get());
            if (!g.active.insert(pair).second) {
                throw py::value_error("Circular reference detected (id repeated)");
            }
            ++g.depth;
            if (g.depth > 255) {
                --g.depth;
                g.active.erase(pair);
                throw py::value_error("Circular reference detected (depth exceeded)");
            }
            struct GuardPop {
                SerRecursionState* g; std::pair<const void*, const void*> pair;
                ~GuardPop() { --g->depth; g->active.erase(pair); }
            } pop{&g, pair};
            return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
        }
        if (type == "lax-or-strict") {
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
        }
        if (type == "union") {
            for (auto& c : children) {
                try { return c->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none); } catch (...) {}
            }
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
            if (std::isnan(d)) {
                if (inf_nan_mode == "strings") return "\"NaN\"";
                return "NaN";
            }
            if (std::isinf(d)) {
                if (inf_nan_mode == "strings") return d > 0 ? "\"Infinity\"" : "\"-Infinity\"";
                return d > 0 ? "Infinity" : "-Infinity";
            }
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
            std::string b = value.cast<std::string>();
            if (ser_json_bytes == "base64") {
                static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string enc;
                for (size_t i = 0; i < b.size(); i += 3) {
                    uint32_t n = ((uint8_t)b[i] << 16);
                    if (i+1 < b.size()) n |= ((uint8_t)b[i+1] << 8);
                    if (i+2 < b.size()) n |= (uint8_t)b[i+2];
                    enc += b64[(n>>18)&0x3F]; enc += b64[(n>>12)&0x3F];
                    enc += (i+1<b.size()) ? b64[(n>>6)&0x3F] : '=';
                    enc += (i+2<b.size()) ? b64[n&0x3F] : '=';
                }
                return "\"" + enc + "\"";
            } else if (ser_json_bytes == "hex") {
                static const char* hex = "0123456789abcdef";
                std::string enc;
                for (unsigned char c : b) {
                    enc += hex[c >> 4];
                    enc += hex[c & 0x0F];
                }
                return "\"" + enc + "\"";
            }
            // Default: UTF-8
            try {
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
                    inner_json = children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
                } else {
                    inner_json = infer_json(value, ensure_ascii, indent);
                }
                return json_escape(inner_json, ensure_ascii);
            }
            if (!children.empty()) {
                return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
            }
        }
        if (type == "nullable" || type == "nullable-union") {
            if (value.is_none()) return "null";
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
        }
        if (type == "default" || type == "with-default") {
            if (value.is_none() && has_default_val) {
                if (!children.empty()) return children[0]->to_json(default_val, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
                return infer_json(default_val, ensure_ascii, indent);
            }
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
        }
        if (type == "to-string") {
            py::object inner = !children.empty() ? children[0]->to_python(value, false, false, round_trip) : value;
            return json_escape(py::str(inner).cast<std::string>(), ensure_ascii);
        }
        if (type == "enum") {
            if (py::hasattr(value, "value")) {
                auto ev = py::getattr(value, "value");
                if (!children.empty()) return children[0]->to_json(ev, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
                return infer_json(ev, ensure_ascii, indent);
            }
            return infer_json(value, ensure_ascii, indent);
        }
        // Types that serialize as their str() representation
        if (type == "uuid" || type == "decimal" || type == "ipaddress" ||
            type == "ipv4address" || type == "ipv6address" ||
            type == "ipv4interface" || type == "ipv6interface" ||
            type == "ipv4network" || type == "ipv6network") {
            return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
        }
        // datetime/date/time: call .isoformat()
        if (type == "datetime" || type == "date" || type == "time") {
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
        // timedelta: ISO 8601 duration format or float
        if (type == "timedelta") {
            try {
                if (ser_json_timedelta == "float") {
                    long days = value.attr("days").cast<long>();
                    long seconds = value.attr("seconds").cast<long>();
                    long microseconds = value.attr("microseconds").cast<long>();
                    double total = days * 86400.0 + seconds + microseconds / 1000000.0;
                    // Format as number without trailing zeros
                    std::string s = std::to_string(total);
                    auto dot = s.find('.');
                    if (dot != std::string::npos) {
                        auto last = s.find_last_not_of('0');
                        if (last > dot) s.erase(last + 1);
                        else s.erase(dot + 2);
                    }
                    return s;
                }
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
                // Break seconds into hours, minutes, seconds
                long hours = seconds / 3600;
                seconds = seconds % 3600;
                long mins = seconds / 60;
                long secs = seconds % 60;

                std::string result = negative ? "-P" : "P";
                if (days > 0) result += std::to_string(days) + "D";

                // Build time part - omit zero components
                bool has_time = hours > 0 || mins > 0 || secs > 0 || microseconds > 0;
                if (has_time) {
                    result += "T";
                    if (hours > 0) result += std::to_string(hours) + "H";
                    if (mins > 0) result += std::to_string(mins) + "M";
                    if (secs > 0 || microseconds > 0) {
                        if (microseconds > 0) {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%ld.%06ld", secs, microseconds);
                            std::string s(buf);
                            auto last = s.find_last_not_of('0');
                            if (last != std::string::npos) s.erase(last + 1);
                            if (s.back() == '.') s.pop_back();
                            result += s + "S";
                        } else {
                            result += std::to_string(secs) + "S";
                        }
                    }
                }
                if (result == "P" || result == "-P") result = "PT0S";
                return json_escape(result, ensure_ascii);
            } catch (...) {
                return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
            }
        }
        // set/frozenset/generator: serialize as JSON array
        if (type == "set" || type == "frozenset" || type == "generator") {
            std::string out = "[";
            bool first = true;
            for (auto item : py::reinterpret_borrow<py::iterable>(value)) {
                if (!first) out += ",";
                first = false;
                py::object obj = py::reinterpret_borrow<py::object>(item);
                if (!children.empty()) {
                    out += children[0]->to_json(obj, ensure_ascii, -1, round_trip, py::none(), py::none(), false, false, false, exc_none);
                } else {
                    out += infer_json(obj, ensure_ascii, -1);
                }
            }
            out += "]";
            return out;
        }
        if (!fields.empty() || type == "model-fields" || type == "typed-dict" || type == "dataclass-args") {
            // A model/dataclass/typed-dict always serializes to a dict, even
            // when it declares no fields (empty model -> {}).
            return serialize_fields_json(value, ensure_ascii, indent, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults);
        }
        // Polymorphism trampoline (Rust PolymorphismTrampoline), json mode.
        if (class_.ptr() && !class_.is_none() &&
            (type == "model" || type == "dataclass" || type == "function-plain" ||
             type == "function-after" || type == "function-before" || type == "function-wrap")) {
            std::optional<py::object> poly_out;
            if (try_polymorphic_trampoline(value, class_,
                                           g_polymorphic_serialization.value_or(polymorphic_from_config),
                                           true, ensure_ascii, include, exclude,
                                           by_alias, exclude_unset, exclude_defaults,
                                           exc_none, round_trip, &poly_out) && poly_out) {
                return poly_out->cast<std::string>();
            }
        }
        // Delegate model/dataclass/typed-dict to inner serializer
        if ((type == "model" || type == "dataclass" || type == "typed-dict") && !children.empty()) {
            // Union discrimination: a model serializer only accepts instances of
            // its expected class; otherwise raise so the union tries next branch
            if (!class_.is_none() && !py::isinstance(value, class_)) {
                throw std::runtime_error("Value is not an instance of the expected model class");
            }
            // For root models, extract the 'root' attribute before delegating
            if (root_model && py::hasattr(value, "root")) {
                auto root_val = py::getattr(value, "root");
                // If the child is a field serializer, pass the model instance
                if (!children.empty() && children[0]->is_field_serializer && children[0]->py_func.ptr() && !children[0]->py_func.is_none()) {
                    auto child = children[0];
                    // For field serializers, call the function directly
                    if (child->type == "function-wrap") {
                        // Create handler that serializes to JSON
                        py::object handler = py::cpp_function([child, root_val, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none](const py::object& v) -> py::object {
                            if (!child->children.empty()) {
                                return py::str(child->children[0]->to_json(v, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none));
                            }
                            return py::str(v);
                        });
                        if (child->info_arg) {
                            PySerializationInfo info(round_trip, "json", "root");
                            return py::str(child->py_func(value, root_val, handler, py::cast(info)));
                        } else {
                            return py::str(child->py_func(value, root_val, handler));
                        }
                    } else {
                        // function-plain
                        py::object result;
                        if (child->info_arg) {
                            PySerializationInfo info(round_trip, "json", "root");
                            result = child->py_func(value, root_val, py::cast(info));
                        } else {
                            result = child->py_func(value, root_val);
                        }
                        return infer_json(result, ensure_ascii, indent);
                    }
                }
                return children[0]->to_json(root_val, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
            }
            return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
        }
        // Note: py_func may be a default-constructed py::object (null handle)
        // when no serialization function was assigned, so check ptr() rather
        // than is_none() (which is false for a null handle).
        if (py_func.ptr() && !py_func.is_none()) {
            // Check when_used
            bool skip_serializer = false;
            if (when_used == "json" || when_used == "json-unless-none") {
                // json mode is always true in to_json
            }
            if ((when_used == "unless-none" || when_used == "json-unless-none") && value.is_none()) {
                skip_serializer = true;
            }
            if (skip_serializer) {
                if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
                return infer_json(value, ensure_ascii, indent);
            }
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

    // Default used for the exclude_defaults comparison. Mirrors Rust
    // WithDefaultSerializer::get_default: a factory taking data yields no
    // default; a plain factory is called; otherwise the stored default is
    // returned (a None default still counts as "has default").
    bool ser_default(py::object& out) const {
        if (type != "default" && type != "with-default") return false;
        if (has_default_val) {
            out = default_val;
            return true;
        }
        // Note: default_factory may be a default-constructed py::object (null
        // handle) when the schema had no factory, so check ptr() rather than
        // is_none() (which is false for a null handle).
        if (default_factory.ptr() && !default_factory.is_none() && !default_factory_takes_data) {
            try {
                out = default_factory();
            } catch (...) {
                return false;
            }
            return true;
        }
        return false;
    }

private:
    static std::string json_escape(const std::string& s, bool ensure_ascii) {
        std::string out = "\"";
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            switch (c) {
                case '"': out += "\\\""; ++i; break;
                case '\\': out += "\\\\"; ++i; break;
                case '\n': out += "\\n"; ++i; break;
                case '\r': out += "\\r"; ++i; break;
                case '\t': out += "\\t"; ++i; break;
                default:
                    if (c < 0x20) {
                        char buf[8]; snprintf(buf, 8, "\\u%04x", c); out += buf;
                        ++i;
                    } else if (ensure_ascii && c > 127) {
                        // Decode UTF-8 to get the Unicode codepoint
                        uint32_t cp = 0;
                        int extra_bytes = 0;
                        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra_bytes = 1; }
                        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra_bytes = 2; }
                        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra_bytes = 3; }
                        else { out += (char)c; ++i; break; } // invalid UTF-8, pass through
                        for (int j = 0; j < extra_bytes && i + 1 < s.size(); ++j) {
                            ++i;
                            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
                        }
                        ++i;
                        if (cp > 0xFFFF) {
                            // Surrogate pair for characters above BMP
                            cp -= 0x10000;
                            char buf[16];
                            snprintf(buf, 16, "\\u%04x\\u%04x",
                                     0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
                            out += buf;
                        } else {
                            char buf[8]; snprintf(buf, 8, "\\u%04x", cp); out += buf;
                        }
                    } else {
                        out += (char)c;
                        ++i;
                    }
                    break;
            }
        }
        out += "\"";
        return out;
    }

    // Thread-local stack of object addresses currently being serialized,
    // mirroring Rust's RecursionGuard: re-entering an ancestor object means
    // a reference cycle (raise), excessive depth is a safety net (raise).
    static std::vector<const void*>& json_rec_stack() {
        static thread_local std::vector<const void*> stack;
        return stack;
    }

    static std::string infer_json(const py::object& value, bool ensure_ascii, int indent) {
        std::vector<const void*>& st = json_rec_stack();
        const void* p = value.ptr();
        for (const void* q : st) {
            if (q == p) {
                throw PydanticSerializationError("Error serializing to JSON: ValueError: Circular reference detected (id repeated)");
            }
        }
        if (st.size() >= 255) {
            throw PydanticSerializationError("Error serializing to JSON: ValueError: Circular reference detected (depth exceeded)");
        }
        st.push_back(p);
        struct StackPop {
            std::vector<const void*>& s;
            ~StackPop() { s.pop_back(); }
        } popper{st};
        return infer_json_body(value, ensure_ascii, indent);
    }

    static std::string infer_json_body(const py::object& value, bool ensure_ascii, int indent) {
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
        // URL objects serialize as their string form, not their __dict__.
        // This must run before the __dict__ fallback below: both the pybind11
        // Url/MultiHostUrl and Python wrappers (HttpUrl, AnyUrl, ...) expose a
        // __dict__ that would otherwise be emitted as a JSON object.
        try {
            py::object url_mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
            py::object url_cls = url_mod.attr("Url");
            py::object murl_cls = url_mod.attr("MultiHostUrl");
            if (py::isinstance(value, url_cls) || py::isinstance(value, murl_cls)) {
                return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
            }
            // Python URL wrapper classes hold the pybind11 Url in a _url attribute.
            if (py::hasattr(value, "_url")) {
                auto inner = py::getattr(value, "_url");
                if (py::isinstance(inner, url_cls) || py::isinstance(inner, murl_cls)) {
                    return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
                }
            }
        } catch (...) {}

        if (py::hasattr(value, "__dict__")) return infer_json(value.attr("__dict__"), ensure_ascii, indent);

        return json_escape(py::repr(value).cast<std::string>(), ensure_ascii);
    }

    // Recursively serialize an arbitrary Python value for use in extra fields
    // (mirrors Rust's infer_to_python: models → to_python, dicts → recurse, lists → recurse).
    // Cyclic values return the innermost occurrence as-is (Rust infer_to_python
    // semantics) instead of recursing forever.
    static std::vector<const void*>& py_rec_stack() {
        static thread_local std::vector<const void*> stack;
        return stack;
    }

    static py::object serialize_any_value(const py::object& v, bool exc_none, bool round_trip) {
        if (v.is_none()) return py::none();
        if (py::isinstance<py::dict>(v) || py::isinstance<py::list>(v) || py::isinstance<py::tuple>(v) || py::hasattr(v, "__pydantic_serializer__")) {
            std::vector<const void*>& st = py_rec_stack();
            const void* p = v.ptr();
            for (const void* q : st) {
                if (q == p) return v;  // cycle: stop recursing, mirror Rust
            }
            if (st.size() >= 255) return v;
            st.push_back(p);
            struct StackPop {
                std::vector<const void*>& s;
                ~StackPop() { s.pop_back(); }
            } popper{st};
            return serialize_any_value_inner(v, exc_none, round_trip);
        }
        return serialize_any_value_inner(v, exc_none, round_trip);
    }

    static py::object serialize_any_value_inner(const py::object& v, bool exc_none, bool round_trip) {
        if (v.is_none()) return py::none();
        // Model / dataclass instances: use __pydantic_serializer__ if available
        if (py::hasattr(v, "__pydantic_serializer__")) {
            auto ser = py::getattr(v, "__pydantic_serializer__");
            try {
                return ser.attr("to_python")(v, py::arg("mode") = "python",
                    py::arg("exclude_none") = exc_none, py::arg("round_trip") = round_trip);
            } catch (const py::error_already_set&) {
                PyErr_Clear();
                return v;
            }
        }
        if (py::isinstance<py::dict>(v)) {
            py::dict out;
            auto d = v.cast<py::dict>();
            for (auto item : d) {
                auto k = py::reinterpret_borrow<py::object>(item.first);
                auto val = py::reinterpret_borrow<py::object>(item.second);
                out[k] = serialize_any_value(val, exc_none, round_trip);
            }
            return std::move(out);
        }
        if (py::isinstance<py::list>(v)) {
            py::list out;
            auto lst = v.cast<py::list>();
            for (auto item : lst) {
                out.append(serialize_any_value(py::reinterpret_borrow<py::object>(item), exc_none, round_trip));
            }
            return std::move(out);
        }
        if (py::isinstance<py::tuple>(v)) {
            py::list temp;
            auto t = v.cast<py::tuple>();
            for (auto item : t) {
                temp.append(serialize_any_value(py::reinterpret_borrow<py::object>(item), exc_none, round_trip));
            }
            return py::tuple(temp);
        }
        return v;
    }

    py::object serialize_fields(const py::object& value, bool exc_none, bool round_trip = false,
                                 const py::object& include = py::none(),
                                 const py::object& exclude = py::none(),
                                 bool by_alias = false,
                                 bool exclude_unset = false,
                                 bool exclude_defaults = false,
                                 const py::object& context = py::none(),
                                 bool json_mode = false) const {
        py::dict result;
        py::dict main;
        if (py::isinstance<py::dict>(value)) main = value.cast<py::dict>();
        else if (py::hasattr(value, "__dict__")) main = py::getattr(value, "__dict__").cast<py::dict>();
        py::object missing_obj = missing_sentinel_obj();

        for (const auto& k : field_order) {
            const auto& ser = fields.at(k);
            // Skip internal metadata keys
            if (k == "__pydantic_fields_set__" || k == "__pydantic_defaults__") continue;

            // Skip fields excluded at schema level (Field(exclude=True))
            if (field_excluded.count(k)) continue;

            // Skip computed fields when round_trip=True or exclude_computed_fields
            if ((round_trip || g_exclude_computed_fields) && computed_fields_.count(k)) continue;

            // Apply include/exclude filters (supports nested dict filters)
            auto next = apply_ser_filter(py::str(k), include, exclude);
            if (next.omit) continue;

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

            // exclude_defaults: skip fields whose value equals their default.
            // Rust: exclude_default reads the default from the field's own
            // serializer (WithDefaultSerializer::get_default) — never from the
            // value or the instance.
            if (exclude_defaults) {
                py::object def_val = py::none();
                if (ser && ser->ser_default(def_val)) {
                    py::str key(k);
                    if (main.contains(key)) {
                        py::object cur = main[key];
                        try {
                            if (cur.equal(def_val)) continue;
                        } catch (...) {}
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
            } else if (computed_fields_.count(k)) {
                // Computed field: read the property from the model instance
                try {
                    fv = py::getattr(value, key);
                } catch (...) {
                    has_value = false;
                }
            } else if (ser->has_default()) {
                fv = ser->get_default_value();
            } else {
                has_value = false;
            }
            if (!has_value) continue;
            if (exc_none && fv.is_none()) continue;
            // Rust exclude_field_by_value: omit fields whose value is the MISSING sentinel.
            if (fv.is(missing_obj)) continue;

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

            bool ser_type_mismatch = false;
            if (computed_fields_.count(k) && ser &&
                ser->type != "function-plain" && ser->type != "function-wrap" &&
                ser->type != "function-after" && ser->type != "function-before" &&
                !fv.is_none() && !SerNode::value_matches_type(ser, fv)) {
                ser_type_mismatch = true;
                ser_warn_unexpected_value(k, SerNode::type_name_for_warning(ser), fv);
            }

            py::object serialized;
            // Check if we should use the custom field serializer based on when_used
            bool use_field_serializer = false;
            if ((ser->type == "function-plain" || ser->type == "function-wrap") && !ser->py_func.is_none()) {
                use_field_serializer = true;
                // Check when_used conditions
                if (ser->when_used == "unless-none" || ser->when_used == "json-unless-none") {
                    if (fv.is_none()) {
                        use_field_serializer = false;
                    }
                }
                // Note: "json" and "json-unless-none" only apply in JSON mode
                if ((ser->when_used == "json" || ser->when_used == "json-unless-none") && !json_mode) {
                    use_field_serializer = false;
                }
            }

            if (ser_type_mismatch) {
                // Computed field value does not match its declared return type:
                // warn and fall back to infer serialization (Rust behavior).
                serialized = serialize_any_value(fv, exc_none, round_trip);
            } else if (use_field_serializer) {
                // Try field serializer call with model instance first
                PySerializationInfo info(round_trip, "python", k, context);
                bool tried = false;
                try {
                    if (ser->type == "function-wrap") {
                        // For wrap mode, create a handler function
                        py::object handler = py::cpp_function([ser, fv, exc_none, round_trip, next, by_alias, exclude_unset, exclude_defaults, context](const py::object& v) -> py::object {
                            if (!ser->children.empty()) {
                                return ser->children[0]->to_python(v, false, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context);
                            }
                            return v;
                        });
                        if (ser->is_field_serializer) {
                            if (ser->info_arg) {
                                serialized = ser->py_func(value, fv, handler, py::cast(info));
                            } else {
                                serialized = ser->py_func(value, fv, handler);
                            }
                        } else {
                            if (ser->info_arg) {
                                serialized = ser->py_func(fv, handler, py::cast(info));
                            } else {
                                serialized = ser->py_func(fv, handler);
                            }
                        }
                    } else {
                        // function-plain
                        if (ser->is_field_serializer) {
                            if (ser->info_arg) {
                                serialized = ser->py_func(value, fv, py::cast(info));
                            } else {
                                serialized = ser->py_func(value, fv);
                            }
                        } else {
                            if (ser->info_arg) {
                                serialized = ser->py_func(fv, py::cast(info));
                            } else {
                                serialized = ser->py_func(fv);
                            }
                        }
                    }
                    tried = true;
                } catch (const py::error_already_set&) {
                    PyErr_Clear();
                    tried = false;
                }
                if (!tried) {
                    // Fallback to inner schema if available, otherwise use default serialization
                    if (!ser->children.empty()) {
                        serialized = ser->children[0]->to_python(fv, json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context);
                    } else {
                        serialized = ser->to_python(fv, json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context);
                    }
                }
            } else {
                // Use the field serializer directly (it handles list/dict iteration, etc.)
                serialized = ser->to_python(fv, json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context);
            }
            result[py::str(output_key)] = serialized;
        }
        // Extra fields - also apply include/exclude if they match by name
        if (py::hasattr(value, "__pydantic_extra__")) {
            auto extra = py::getattr(value, "__pydantic_extra__");
            if (!extra.is_none()) {
                for (auto item : extra.cast<py::dict>()) {
                    std::string k = py::str(item.first).cast<std::string>();
                    if (fields.find(k) != fields.end()) continue;
                    // Apply include/exclude filters to extra fields too
                    auto next = apply_ser_filter(py::str(k), include, exclude);
                    if (next.omit) continue;
                    py::object v = py::reinterpret_borrow<py::object>(item.second);
                    if (exc_none && v.is_none()) continue;
                    result[py::str(k)] = serialize_any_value(v, exc_none, round_trip);
                }
            }
        }
        return std::move(result);
    }

    std::string serialize_fields_json(const py::object& value, bool ensure_ascii, int indent, bool exc_none, bool round_trip = false,
                                       const py::object& include = py::none(),
                                       const py::object& exclude = py::none(),
                                       bool by_alias = false,
                                       bool exclude_unset = false,
                                       bool exclude_defaults = false) const {
        std::string out = "{";
        bool first = true;
        py::dict main;
        if (py::isinstance<py::dict>(value)) main = value.cast<py::dict>();
        else if (py::hasattr(value, "__dict__")) main = py::getattr(value, "__dict__").cast<py::dict>();
        py::object missing_obj = missing_sentinel_obj();

        for (const auto& k : field_order) {
            const auto& ser = fields.at(k);
            // Skip internal metadata keys
            if (k == "__pydantic_fields_set__" || k == "__pydantic_defaults__") continue;

            // Skip fields excluded at schema level (Field(exclude=True))
            if (field_excluded.count(k)) continue;

            // Skip computed fields when round_trip=True or exclude_computed_fields
            if ((round_trip || g_exclude_computed_fields) && computed_fields_.count(k)) continue;

            // Apply include/exclude filters (supports nested dict filters)
            auto next = apply_ser_filter(py::str(k), include, exclude);
            if (next.omit) continue;
            
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
            
            // exclude_defaults: skip fields whose value equals their default.
            // Rust: exclude_default reads the default from the field's own
            // serializer (WithDefaultSerializer::get_default) — never from the
            // value or the instance.
            if (exclude_defaults) {
                py::object def_val = py::none();
                if (ser && ser->ser_default(def_val)) {
                    py::str key(k);
                    if (main.contains(key)) {
                        py::object cur = main[key];
                        try {
                            if (cur.equal(def_val)) continue;
                        } catch (...) {}
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
            } else if (computed_fields_.count(k)) {
                // Computed field: read the property from the model instance
                try {
                    fv = py::getattr(value, key);
                } catch (...) {
                    has_value = false;
                }
            } else if (ser->has_default()) {
                fv = ser->get_default_value();
            } else {
                has_value = false;
            }
            if (!has_value) continue;
            if (exc_none && fv.is_none()) continue;
            // Rust exclude_field_by_value: omit fields whose value is the MISSING sentinel.
            if (fv.is(missing_obj)) continue;

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

            bool ser_type_mismatch = false;
            if (computed_fields_.count(k) && ser &&
                ser->type != "function-plain" && ser->type != "function-wrap" &&
                ser->type != "function-after" && ser->type != "function-before" &&
                !fv.is_none() && !SerNode::value_matches_type(ser, fv)) {
                ser_type_mismatch = true;
                ser_warn_unexpected_value(k, SerNode::type_name_for_warning(ser), fv);
            }

            std::string field_json;
            // Check if we should use the custom field serializer based on when_used
            bool use_field_serializer = false;
            if ((ser->type == "function-plain" || ser->type == "function-wrap") && !ser->py_func.is_none()) {
                use_field_serializer = true;
                // Check when_used conditions
                if (ser->when_used == "unless-none" || ser->when_used == "json-unless-none") {
                    if (fv.is_none()) {
                        use_field_serializer = false;
                    }
                }
                // Note: "json" and "json-unless-none" are already in JSON mode, so no additional check needed
            }

            if (use_field_serializer) {
                // Try field serializer call with model instance first
                PySerializationInfo info(round_trip, "json", k);
                bool tried = false;
                try {
                    py::object result;
                    if (ser->type == "function-wrap") {
                        // For wrap mode, create a handler function
                        py::object handler = py::cpp_function([ser, fv, ensure_ascii, round_trip, next, by_alias, exclude_unset, exclude_defaults, exc_none](const py::object& v) -> py::object {
                            if (!ser->children.empty()) {
                                // Call to_python and let infer_json handle the conversion
                                auto py_result = ser->children[0]->to_python(v, true, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults);
                                return py_result;
                            }
                            return v;
                        });
                        if (ser->is_field_serializer) {
                            if (ser->info_arg) {
                                result = ser->py_func(value, fv, handler, py::cast(info));
                            } else {
                                result = ser->py_func(value, fv, handler);
                            }
                        } else {
                            if (ser->info_arg) {
                                result = ser->py_func(fv, handler, py::cast(info));
                            } else {
                                result = ser->py_func(fv, handler);
                            }
                        }
                    } else {
                        // function-plain
                        if (ser->is_field_serializer) {
                            if (ser->info_arg) {
                                result = ser->py_func(value, fv, py::cast(info));
                            } else {
                                result = ser->py_func(value, fv);
                            }
                        } else {
                            if (ser->info_arg) {
                                result = ser->py_func(fv, py::cast(info));
                            } else {
                                result = ser->py_func(fv);
                            }
                        }
                    }
                    field_json = infer_json(result, ensure_ascii, -1);
                    tried = true;
                } catch (const py::error_already_set&) {
                    PyErr_Clear();
                    tried = false;
                }
                if (!tried) {
                    // Fallback to field serializer (handles list/dict iteration, etc.)
                    field_json = ser->to_json(fv, ensure_ascii, -1, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
                }
            } else if (ser_type_mismatch) {
                // Computed field value does not match its declared return type:
                // warn and fall back to infer serialization (Rust behavior).
                field_json = infer_json(fv, ensure_ascii, -1);
            } else {
                // Use the field serializer directly (handles list/dict iteration, etc.)
                field_json = ser->to_json(fv, ensure_ascii, -1, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, exc_none);
            }

            if (!first) out += ",";
            first = false;
            out += json_escape(output_key, ensure_ascii) + ":" + field_json;
        }
        if (py::hasattr(value, "__pydantic_extra__")) {
            auto extra = py::getattr(value, "__pydantic_extra__");
            if (!extra.is_none()) {
                for (auto item : extra.cast<py::dict>()) {
                    std::string k = py::str(item.first).cast<std::string>();
                    if (fields.find(k) != fields.end()) continue;
                    // Apply include/exclude filters to extra fields too
                    auto next = apply_ser_filter(py::str(k), include, exclude);
                    if (next.omit) continue;
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

// Rust-style serializer display name used in "Expected `X`" warnings.
std::string SerNode::type_name_for_warning(const SerRef& n) {
    if (!n) return "any";
    const std::string& t = n->type;
    auto child0 = [&]() -> std::string {
        return n->children.empty() ? "any" : type_name_for_warning(n->children[0]);
    };
    if (t == "int" || t == "int-constrained") return "int";
    if (t == "float" || t == "float-constrained") return "float";
    if (t == "bool") return "bool";
    if (t == "str" || t == "string" || t == "str-constrained") return "str";
    if (t == "bytes") return "bytes";
    if (t == "datetime") return "datetime";
    if (t == "date") return "date";
    if (t == "time") return "time";
    if (t == "list") return "list[" + child0() + "]";
    if (t == "set") return "set[" + child0() + "]";
    if (t == "frozenset") return "frozenset[" + child0() + "]";
    if (t == "tuple") return "tuple[" + child0() + "]";
    if (t == "dict") {
        std::string keyn = n->children.size() > 0 ? type_name_for_warning(n->children[0]) : "any";
        std::string valn = n->children.size() > 1 ? type_name_for_warning(n->children[1]) : "any";
        return "dict[" + keyn + ", " + valn + "]";
    }
    if (t == "none" || t == "is-none") return "None";
    if (t == "any") return "any";
    return t;
}

// Whether a Python value is compatible with the node's declared Python type.
// Mirrors Rust's ObType::is_type / IsType::False -> warn fallback.  Unknown or
// permissive node types always return true (no warning).
bool SerNode::value_matches_type(const SerRef& n, const py::object& v) {
    if (!n || v.is_none()) return true;
    const std::string& t = n->type;
    if (t == "any" || t == "is-instance" || t == "is-subclass") return true;
    if (t == "int" || t == "int-constrained") return py::isinstance<py::int_>(v);   // bool is an int subclass
    if (t == "float" || t == "float-constrained") return py::isinstance<py::float_>(v) || py::isinstance<py::int_>(v);
    if (t == "bool") return py::isinstance<py::bool_>(v);
    if (t == "str" || t == "string" || t == "str-constrained") return py::isinstance<py::str>(v);
    if (t == "bytes") return py::isinstance<py::bytes>(v);
    if (t == "list") return py::isinstance<py::list>(v);
    if (t == "set") return py::isinstance<py::set>(v);
    if (t == "frozenset") return py::isinstance<py::frozenset>(v);
    if (t == "tuple") return py::isinstance<py::tuple>(v);
    if (t == "dict") return py::isinstance<py::dict>(v);
    return true;
}

// ---------------------------------------------------------------------------
// Build serializer from schema
// ---------------------------------------------------------------------------
static SerRef build_ser_impl(const py::dict& schema,
                        std::unordered_map<std::string, SerRef>& defs);

static SerRef build_ser(const py::dict& schema,
                        std::unordered_map<std::string, SerRef>& defs) {
    return build_ser_impl(schema, defs);
}

static thread_local int _build_ser_depth = 0;

static SerRef build_ser_impl(const py::dict& schema,
                        std::unordered_map<std::string, SerRef>& defs) {
    _build_ser_depth++;
    if (_build_ser_depth > 200) {
        std::string t = "unknown";
        try { t = schema["type"].cast<std::string>(); } catch (...) {}
        fprintf(stderr, "ERROR: build_ser_impl recursion depth exceeded 200, type=%s\n", t.c_str());
        _build_ser_depth--;
        return std::make_shared<SerNode>();
    }
    struct DepthGuard { ~DepthGuard() { _build_ser_depth--; } } _guard;

    std::string type;
    try { type = schema["type"].cast<std::string>(); } catch (...) { type = "any"; }
    std::string original_type = type;  // Save original type before serialization override

    // Check serialization override
    py::dict ser_dict;
    bool has_ser_dict = false;
    try {
        if (schema.contains("serialization")) {
            ser_dict = schema["serialization"].cast<py::dict>();
            has_ser_dict = true;
            if (ser_dict.contains("type")) {
                std::string st = ser_dict["type"].cast<std::string>();
                if (st != "include-exclude-sequence" && st != "include-exclude-dict" && st != "base64")
                    type = st;
            }
        }
    } catch (...) {}

    auto node = std::make_shared<SerNode>();
    node->type = type;

    // Handle model-field wrapper: unwrap to inner schema
    if (type == "model-field") {
        try {
            auto inner = build_ser_impl(schema["schema"].cast<py::dict>(), defs);
            if (inner) {
                node->copy_from(*inner);
                return node;
            }
        } catch (...) {}
    }

    // When serialization overrides the function too, store it for later use
    // (the function extraction below will use the main schema's function by default)
    py::object ser_func = py::none();
    bool ser_info_arg = false;
    bool ser_is_field_serializer = false;
    std::string ser_when_used = "always";
    try {
        if (!ser_dict.is_none() && ser_dict.contains("function")) {
            ser_func = ser_dict["function"];
        }
        if (!ser_dict.is_none() && ser_dict.contains("info_arg")) {
            ser_info_arg = ser_dict["info_arg"].cast<bool>();
        }
        if (!ser_dict.is_none() && ser_dict.contains("is_field_serializer")) {
            ser_is_field_serializer = ser_dict["is_field_serializer"].cast<bool>();
        }
        if (!ser_dict.is_none() && ser_dict.contains("when_used")) {
            ser_when_used = ser_dict["when_used"].cast<std::string>();
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
                SerRef actual;
                if (d.contains("schema")) {
                    actual = build_ser_impl(d["schema"].cast<py::dict>(), defs);
                } else {
                    // Definitions without inner schema (e.g. enum) — build from the definition itself
                    actual = build_ser_impl(d, defs);
                }
                // Copy actual content into the stub (preserves shared_ptr identity)
                defs[ref]->copy_from(*actual);
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "Error building definitions: %s\n", e.what());
        }
        try { return build_ser_impl(schema["schema"].cast<py::dict>(), defs); } catch (...) {}
        return node;
    }
    if (original_type == "definition-ref") {
        try {
            std::string ref = schema["schema_ref"].cast<std::string>();
            auto it = defs.find(ref);
            if (it != defs.end()) {
                // If this definition-ref has a serialization override, apply it
                if (has_ser_dict && ser_dict.contains("function")) {
                    auto wrapped = std::make_shared<SerNode>();
                    wrapped->type = ser_dict["type"].cast<std::string>();
                    wrapped->py_func = ser_dict["function"];
                    wrapped->info_arg = ser_info_arg;
                    wrapped->is_field_serializer = ser_is_field_serializer;
                    wrapped->when_used = ser_when_used;
                    // Add the resolved definition as a child (not copy its fields)
                    wrapped->children.push_back(it->second);
                    return wrapped;
                }
                // Wrap in a definition-ref node so the recursion guard
                // (Rust definitions.rs) can detect reference cycles.
                auto wrapper = std::make_shared<SerNode>();
                wrapper->type = "definition-ref";
                wrapper->children.push_back(it->second);  // stub (will be populated)
                return wrapper;
            }
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
        try { node->default_factory = schema["default_factory"]; } catch (...) {}
        try { node->default_factory_takes_data = schema["default_factory_takes_data"].cast<bool>(); } catch (...) {}
    }
    if (type == "format") {
        try { node->format_str = schema["formatting"].cast<std::string>(); } catch (...) {}
    }

    if (type == "function-plain" || type == "function-after" || type == "function-before" || type == "function-wrap") {
        // Without a serialization override, function-before/after/wrap are
        // validation-only wrappers: serialize the inner schema directly,
        // matching Rust's FunctionBefore/After/WrapSerializerBuilder (which
        // builds from `schema.schema`). Otherwise model_dump would re-run
        // validators (e.g. root_validator returning a fixed dict) and produce
        // wrong output.
        if ((type == "function-before" || type == "function-after" || type == "function-wrap") && !has_ser_dict) {
            auto inner = sub();
            if (inner) return inner;
        }
        // Only treat the schema's function as a serializer when a serialization
        // override exists (e.g. PlainSerializer/WrapSerializer/field serializers).
        if (has_ser_dict) {
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
                // Determine info_arg: prefer serialization override, fall back to schema
                bool info_arg = ser_info_arg;
                if (!has_ser_dict || !ser_dict.contains("info_arg")) {
                    try { if (schema.contains("info_arg")) info_arg = schema["info_arg"].cast<bool>(); } catch (...) {}
                }
                node->info_arg = info_arg;
                // Determine is_field_serializer: prefer serialization override, fall back to schema
                bool is_field_serializer = ser_is_field_serializer;
                if (!has_ser_dict || !ser_dict.contains("is_field_serializer")) {
                    try { if (schema.contains("is_field_serializer")) is_field_serializer = schema["is_field_serializer"].cast<bool>(); } catch (...) {}
                }
                node->is_field_serializer = is_field_serializer;
                // Determine when_used: prefer serialization override, fall back to schema
                std::string when_used = ser_when_used;
                if (!has_ser_dict || !ser_dict.contains("when_used")) {
                    try { if (schema.contains("when_used")) when_used = schema["when_used"].cast<std::string>(); } catch (...) {}
                }
                node->when_used = when_used;
            } catch (...) {}
        }
        // For function-plain with serialization override, build children from original schema for fallback
        if (type != "function-plain" || has_ser_dict) { auto c = sub(); if (c) node->children.push_back(c); }
    }

    // model-fields, typed-dict, dataclass-args
    if (type == "model-fields" || type == "typed-dict" || type == "dataclass-args") {
        try {
            if (schema.contains("fields")) {
                // Normalize to (name, fielddef) pairs: model-fields/typed-dict
                // use a dict, while dataclass-args uses a list of
                // {'name': ..., 'schema': ...} dicts.
                std::vector<std::pair<std::string, py::dict>> field_defs;
                py::object fields_obj = schema["fields"];
                if (py::isinstance<py::dict>(fields_obj)) {
                    for (auto item : fields_obj.cast<py::dict>()) {
                        field_defs.emplace_back(py::str(item.first).cast<std::string>(), item.second.cast<py::dict>());
                    }
                } else if (!fields_obj.is_none()) {
                    for (auto item : py::reinterpret_borrow<py::iterable>(fields_obj)) {
                        auto f = py::reinterpret_borrow<py::dict>(item);
                        std::string name;
                        if (f.contains("name")) name = f["name"].cast<std::string>();
                        field_defs.emplace_back(std::move(name), f);
                    }
                }
                for (auto& [k, fdef] : field_defs) {
                    // Handle both formats:
                    // 1. {'schema': {'type': 'str'}} - pydantic-core format
                    // 2. {'type': 'str'} - simplified format
                    py::dict field_schema;
                    if (fdef.contains("schema")) {
                        field_schema = fdef["schema"].cast<py::dict>();
                        // Check for serialization alias (pydantic-core format)
                        if (fdef.contains("serialization_alias")) {
                            node->field_aliases[k] = fdef["serialization_alias"].cast<std::string>();
                        } else if (fdef.contains("alias")) {
                            node->field_aliases[k] = fdef["alias"].cast<std::string>();
                        }
                    } else {
                        field_schema = fdef;  // Use fdef directly as schema
                        // Check for alias directly in schema
                        if (field_schema.contains("serialization_alias")) {
                            node->field_aliases[k] = field_schema["serialization_alias"].cast<std::string>();
                        } else if (field_schema.contains("alias")) {
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
                    // Schema-level field exclusion (Field(exclude=True))
                    if (fdef.contains("serialization_exclude")) {
                        py::object se = fdef["serialization_exclude"];
                        if (py::isinstance<py::bool_>(se) && se.cast<bool>()) {
                            node->field_excluded.insert(k);
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
                    // Computed field alias (from alias generator): serialization_alias takes precedence
                    if (cf.contains("serialization_alias")) {
                        node->field_aliases[prop] = cf["serialization_alias"].cast<std::string>();
                    } else if (cf.contains("alias")) {
                        node->field_aliases[prop] = cf["alias"].cast<std::string>();
                    }
                    // Computed field serialization_exclude_if (Field(exclude_if=...))
                    if (cf.contains("serialization_exclude_if")) {
                        py::object eif = cf["serialization_exclude_if"];
                        if (py::isinstance<py::function>(eif) || py::hasattr(eif, "__call__")) {
                            node->field_exclude_if[prop] = eif;
                        }
                    }
                    try {
                        node->fields[prop] = build_ser(cf["return_schema"].cast<py::dict>(), defs);
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
        // Check for root_model flag
        if (schema.contains("root_model")) {
            try { node->root_model = schema["root_model"].cast<bool>(); } catch (...) {}
        }
        // Store expected class for union discrimination
        if (schema.contains("cls")) {
            try { node->class_ = schema["cls"].cast<py::object>(); } catch (...) {}
        }
        auto c = sub();
        if (c) {
            node->children.push_back(c);
        }
    }

    // When a model/dataclass schema carries a "serialization" function
    // override, the top-level node is the function wrapper; propagate the
    // underlying model/dataclass identity so the polymorphism trampoline can
    // apply (mirrors Rust's PolymorphismTrampoline wrapping the whole model
    // serializer, function serializer included).
    if ((original_type == "model" || original_type == "dataclass") && has_ser_dict) {
        if (!node->class_.ptr() || node->class_.is_none()) {
            try {
                if (schema.contains("cls")) {
                    node->class_ = schema["cls"].cast<py::object>();
                } else if (!node->children.empty() && node->children[0]) {
                    node->class_ = node->children[0]->class_;
                }
            } catch (...) {}
        }
    }

    // Extract config options and propagate to all descendants
    if (schema.contains("config")) {
        try {
            py::dict config = schema["config"].cast<py::dict>();
            std::unordered_set<SerRef> visited;
            std::function<void(SerRef)> set_config = [&](SerRef n) {
                if (!n) return;
                if (visited.count(n)) return;  // Cycle detection
                visited.insert(n);
                if (config.contains("ser_json_inf_nan")) {
                    n->inf_nan_mode = config["ser_json_inf_nan"].cast<std::string>();
                }
                if (config.contains("ser_json_bytes")) {
                    n->ser_json_bytes = config["ser_json_bytes"].cast<std::string>();
                }
                if (config.contains("ser_json_timedelta")) {
                    n->ser_json_timedelta = config["ser_json_timedelta"].cast<std::string>();
                }
                if (config.contains("polymorphic_serialization")) {
                    try { n->polymorphic_from_config = config["polymorphic_serialization"].cast<bool>(); } catch (...) {}
                }
                for (auto& child : n->children) set_config(child);
                for (auto& [k, v] : n->fields) set_config(v);
                for (auto& [k, v] : n->tagged) set_config(v);
            };
            set_config(node);
        } catch (...) {}
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
        if (cfg.has_value()) {
            try {
                py::dict c = *cfg;
                if (c.contains("serialize_by_alias")) {
                    serialize_by_alias_ = c["serialize_by_alias"].cast<bool>();
                }
            } catch (...) { PyErr_Clear(); }
        }
    }

    py::object to_python(const py::object& value, std::optional<std::string> mode,
                         std::optional<py::object> include, std::optional<py::object> exclude,
                         std::optional<bool> by_alias, bool exclude_unset, bool exclude_defaults, bool exc_none,
                         bool exclude_computed_fields, bool round_trip, py::object warnings, std::optional<py::object> fallback,
                         bool serialize_as_any, std::optional<bool> polymorphic, py::object context) const {
        (void)fallback; (void)serialize_as_any;
        g_exclude_computed_fields = exclude_computed_fields;
        g_polymorphic_serialization = polymorphic;  // reset per call (None -> nullopt)
        if (!ser_) throw std::runtime_error("Serializer not initialized");

        // Pass include/exclude through as-is (nested dict/set filters supported)
        py::object inc = (include && !include->is_none()) ? *include : py::none();
        py::object exc = (exclude && !exclude->is_none()) ? *exclude : py::none();
        bool use_alias = by_alias.value_or(serialize_by_alias_);

        ser_warn_enter(warnings);
        try {
            py::object result = ser_->to_python(value, mode && *mode == "json", exc_none, round_trip, inc, exc, use_alias, exclude_unset, exclude_defaults, context);
            ser_warn_leave(false);  // may emit UserWarning / raise PydanticSerializationError
            return result;
        } catch (...) {
            ser_warn_leave(true);  // discard warnings collected before the error
            throw;
        }
    }

    py::bytes to_json(const py::object& value, std::optional<size_t>, std::optional<bool> ea,
                      std::optional<py::object> include, std::optional<py::object> exclude,
                      std::optional<bool> by_alias, bool exclude_unset, bool exclude_defaults, bool exc_none,
                      bool exclude_computed_fields, bool round_trip, py::object warnings, std::optional<py::object> fallback,
                      bool serialize_as_any, std::optional<bool> polymorphic, py::object context) const {
        (void)fallback; (void)serialize_as_any;
        g_exclude_computed_fields = exclude_computed_fields;
        g_polymorphic_serialization = polymorphic;  // reset per call (None -> nullopt)
        if (!ser_) throw std::runtime_error("Serializer not initialized");

        // Pass include/exclude through as-is (nested dict/set filters supported)
        py::object inc = (include && !include->is_none()) ? *include : py::none();
        py::object exc = (exclude && !exclude->is_none()) ? *exclude : py::none();
        bool use_alias = by_alias.value_or(serialize_by_alias_);

        bool e = ea.value_or(false);
        ser_warn_enter(warnings);
        try {
            std::string json = ser_->to_json(value, e, -1, round_trip, inc, exc, use_alias, exclude_unset, exclude_defaults, exc_none, context);
            ser_warn_leave(false);  // may emit UserWarning / raise PydanticSerializationError
            return py::bytes(json);
        } catch (...) {
            ser_warn_leave(true);  // discard warnings collected before the error
            throw;
        }
    }

    std::string repr() const {
        return ser_ ? "SchemaSerializer(serializer=" + ser_->type + ")" : "SchemaSerializer()";
    }

    const py::object& get_schema() const { return schema_; }

private:
    SerRef ser_;
    py::object schema_;  // Store schema for pickle support
    bool serialize_by_alias_ = false;  // config default for by_alias=None
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
    return py::bytes(any->to_json(value, ea.value_or(false), -1, round_trip, py::none(), py::none(), false, false, false, false));
}

// Convert an arbitrary Python value to its JSON-compatible Python form
// (mirrors Rust's infer_jsonable_python used by to_jsonable_python).
// Raises pydantic_core::PydanticSerializationError (registered below as a
// Python exception) for values that have no JSON-compatible form.
static py::object infer_jsonable_python(const py::object& v, const std::string& bytes_mode,
                                        const std::string& timedelta_mode,
                                        const std::string& inf_nan_mode,
                                        bool serialize_unknown) {
    if (v.is_none()) return py::none();
    if (py::isinstance<py::bool_>(v) || py::isinstance<py::int_>(v) || py::isinstance<py::str>(v)) return v;
    if (py::isinstance<py::float_>(v)) {
        double d = v.cast<double>();
        if ((std::isnan(d) || std::isinf(d)) && inf_nan_mode == "null") return py::none();
        return v;
    }
    if (py::isinstance<py::bytes>(v)) {
        std::string b = v.cast<std::string>();
        if (bytes_mode == "base64") return py::str(b64_encode_string(b));
        if (bytes_mode == "hex") {
            static const char* hex_chars = "0123456789abcdef";
            std::string enc;
            for (unsigned char c : b) {
                enc += hex_chars[c >> 4];
                enc += hex_chars[c & 0x0F];
            }
            return py::str(enc);
        }
        try {
            return py::str(v.cast<py::bytes>().operator std::string());  // utf8
        } catch (...) {
            PyErr_Clear();
            throw PydanticSerializationError("Cannot serialize bytes: invalid utf-8");
        }
    }
    // Types that serialize as their str() representation
    try {
        static py::object decimal_cls = py::module_::import("decimal").attr("Decimal");
        if (py::isinstance(v, decimal_cls)) return py::str(v);
    } catch (...) { PyErr_Clear(); }
    try {
        static py::object uuid_cls = py::module_::import("uuid").attr("UUID");
        if (py::isinstance(v, uuid_cls)) return py::str(v);
    } catch (...) { PyErr_Clear(); }
    try {
        static py::object purepath_cls = py::module_::import("pathlib").attr("PurePath");
        if (py::isinstance(v, purepath_cls)) return py::str(v);
    } catch (...) { PyErr_Clear(); }
    try {
        py::object mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
        if (py::isinstance(v, mod.attr("Url")) || py::isinstance(v, mod.attr("MultiHostUrl"))) return py::str(v);
    } catch (...) { PyErr_Clear(); }
    // datetime/date/time expose isoformat()
    if (py::hasattr(v, "isoformat")) {
        try {
            py::object iso = v.attr("isoformat")();
            std::string s = py::str(iso).cast<std::string>();
            if (s.size() >= 6 && s.substr(s.size() - 6) == "+00:00") s = s.substr(0, s.size() - 6) + "Z";
            return py::str(s);
        } catch (...) { PyErr_Clear(); }
    }
    // timedelta (duck-typed via its components)
    if (py::hasattr(v, "days") && py::hasattr(v, "seconds") && py::hasattr(v, "microseconds")
        && !py::hasattr(v, "isoformat")) {
        py::object out;
        if (json_leaf_convert("timedelta", v, "utf8", timedelta_mode, out)) return out;
    }
    // Enum members convert as their value
    if (py::hasattr(v, "_value_")) {
        return infer_jsonable_python(py::getattr(v, "_value_"), bytes_mode, timedelta_mode,
                                     inf_nan_mode, serialize_unknown);
    }
    // Sets/tuples/lists/sequences → arrays; dicts → objects (recursively)
    if (py::isinstance<py::set>(v) || py::isinstance<py::frozenset>(v)
        || py::isinstance<py::list>(v) || py::isinstance<py::tuple>(v)
        || py::isinstance<py::sequence>(v)) {
        py::list out;
        for (auto item : py::reinterpret_borrow<py::iterable>(v)) {
            out.append(infer_jsonable_python(py::reinterpret_borrow<py::object>(item), bytes_mode,
                                             timedelta_mode, inf_nan_mode, serialize_unknown));
        }
        return std::move(out);
    }
    if (py::isinstance<py::dict>(v)) {
        py::dict out;
        for (auto item : v.cast<py::dict>()) {
            auto k = infer_jsonable_python(py::reinterpret_borrow<py::object>(item.first), bytes_mode,
                                           timedelta_mode, inf_nan_mode, serialize_unknown);
            auto val = infer_jsonable_python(py::reinterpret_borrow<py::object>(item.second), bytes_mode,
                                             timedelta_mode, inf_nan_mode, serialize_unknown);
            out[k] = val;
        }
        return std::move(out);
    }
    // Model/dataclass instances: delegate to their serializer in json mode
    if (py::hasattr(v, "__pydantic_serializer__")) {
        try {
            auto ser = py::getattr(v, "__pydantic_serializer__");
            return ser.attr("to_python")(v, py::arg("mode") = "json");
        } catch (const py::error_already_set&) {
            PyErr_Clear();
        }
    }
    // Plain instances with state: mirror infer_json's __dict__ handling.
    // Note: functions/lambdas have an empty __dict__, so they fall through
    // to the error below — matching Rust's refusal to serialize callables.
    if (py::hasattr(v, "__dict__")) {
        try {
            py::dict d = py::getattr(v, "__dict__").cast<py::dict>();
            if (!d.empty()) {
                return infer_jsonable_python(py::reinterpret_borrow<py::object>(d), bytes_mode,
                                             timedelta_mode, inf_nan_mode, serialize_unknown);
            }
        } catch (...) { PyErr_Clear(); }
    }
    if (serialize_unknown) return py::str(v);
    throw PydanticSerializationError("Value is not JSON serializable");
}

static py::object to_jsonable_fn(const py::object& value, std::optional<py::object>, std::optional<py::object>,
    bool, bool, bool round_trip, std::string timedelta_mode, std::string temporal_mode, std::string bytes_mode,
    std::string inf_nan_mode, bool serialize_unknown,
    std::optional<py::object> fallback, bool serialize_as_any, std::optional<bool>, std::optional<py::object>) {
    (void)round_trip;
    (void)temporal_mode;
    (void)fallback;
    (void)serialize_as_any;
    return infer_jsonable_python(value, bytes_mode, timedelta_mode, inf_nan_mode, serialize_unknown);
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
        .def(py::init<bool, std::string, std::string, py::object>(), py::arg("round_trip"), py::arg("mode") = "python", py::arg("field_name") = "", py::arg("context") = py::none())
        .def_readonly("round_trip", &PySerializationInfo::round_trip)
        .def_readonly("mode", &PySerializationInfo::mode)
        .def_readonly("field_name", &PySerializationInfo::field_name)
        .def_readonly("context", &PySerializationInfo::context)
        .def_property_readonly("polymorphic_serialization", [](const PySerializationInfo&) -> py::object {
            if (g_polymorphic_serialization.has_value()) return py::object(py::bool_(*g_polymorphic_serialization));
            return py::none();
        });

    // Register ValidationError as a proper Python exception (inherits from ValueError like Rust)
    py::register_exception<ValidationError>(m, "ValidationError", PyExc_ValueError);

    // SchemaError as a proper Python exception
    py::register_exception<SchemaError>(m, "SchemaError", PyExc_ValueError);

    // Serialization failure for unserializable values
    py::register_exception<PydanticSerializationError>(m, "PydanticSerializationError", PyExc_ValueError);

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
                const ValidationError* ve = py::cast<const ValidationError*>(self.ptr());
                if (ve == nullptr) {
                    throw std::runtime_error("Unable to access ValidationError");
                }
                return ve->error_count();
            }, py::is_method(ve_cls))
        );
        py::setattr(ve_cls, "errors",
            py::cpp_function([](py::object self, bool include_url) -> py::list {
                const ValidationError* ve = py::cast<const ValidationError*>(self.ptr());
                if (ve == nullptr) {
                    throw std::runtime_error("Unable to access ValidationError");
                }
                // Build the error dicts manually: ErrorDetails is not a bound
                // type, so casting std::vector<ErrorDetails> to a list would be
                // undefined behavior.
                const auto& errors = ve->errors();
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
                    // input: parse the stored repr into a real Python value
#ifdef HAS_PYBIND11
                    // Use raw Python object for accurate serialization (Rust parallel:
                    // as_val_error(input) passes Py<PyAny> through). This avoids converting
                    // arbitrary objects to string repr that can't be reconstructed later.
                    if (err.has_raw_input && err.raw_input_obj.ptr()) {
                        d["input"] = err.raw_input_obj;
                    } else
#endif
                    {
                        // Fallback: parse string repr via ast.literal_eval. A value
                        // with surrounding whitespace is necessarily a string (a
                        // canonical numeric/bool/None literal has none), so keep it
                        // as a string rather than risk ast.literal_eval coercing it
                        // (e.g. " 1 " -> 1).
                        auto strip_ws = [](const std::string& s) {
                            size_t b = s.find_first_not_of(" \t\r\n");
                            if (b == std::string::npos) return std::string();
                            size_t e = s.find_last_not_of(" \t\r\n");
                            return s.substr(b, e - b + 1);
                        };
                        bool has_ws = strip_ws(err.input) != err.input;
                        if (!has_ws) {
                            try {
                                d["input"] = py::module_::import("ast").attr("literal_eval")(err.input);
                            } catch (...) {
                                d["input"] = py::str(err.input);
                            }
                        } else {
                            d["input"] = py::str(err.input);
                        }
                    }
                    if (!err.ctx.empty()) {
                        py::dict ctx;
                        for (const auto& [k, v] : err.ctx) {
                            ctx[py::str(k)] = v;
                        }
                        d["ctx"] = ctx;
                    }
                    if (include_url && !err.is_custom) {
                        d["url"] = "https://errors.pydantic.dev/2.14/v/" + err.type;
                    }
                    result.append(d);
                }
                return result;
            }, py::is_method(ve_cls), py::arg("include_url") = true)
        );
        py::setattr(ve_cls, "to_json",
            py::cpp_function([](py::object self) {
                return self.cast<const ValidationError&>().to_json_string();
            }, py::is_method(ve_cls))
        );
    }
    // Signal exceptions raised by custom serializers/schema code. These must
    // be real Python exception types so `except PydanticOmit:` works.
    py::register_exception<PydanticOmit>(m, "PydanticOmit");
    py::register_exception<PydanticUseDefault>(m, "PydanticUseDefault");

    // _LazyValidator — lazy iterator for generator/iterable validation
    struct LazyValidator {
        py::object source;
        py::function validate_fn;
        std::string schema_repr;
        size_t index = 0;
    };
    py::class_<LazyValidator>(m, "_LazyValidator")
        .def(py::init([](py::object source, py::function validate_fn, std::string schema_repr) {
            return std::make_unique<LazyValidator>(LazyValidator{source, validate_fn, schema_repr, 0});
        }))
        .def("__iter__", [](LazyValidator& self) -> py::object {
            return py::cast(self);
        })
        .def("__next__", [](LazyValidator& self) -> py::object {
            py::object item = py::module_::import("builtins").attr("next")(self.source);
            size_t idx = self.index++;
            return self.validate_fn(item, idx);
        })
        .def("__repr__", [](LazyValidator& self) -> std::string {
            return "ValidatorIterator(index=" + std::to_string(self.index) +
                   ", schema=Some(" + self.schema_repr + "))";
        });

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
                                    py::object extra, py::object from_attributes, py::object by_alias, py::object by_name,
                                    py::object allow_partial) -> py::object {
            // NEW: Use native PythonInput - no JSON round-trip!
            {
                py::module_ m = py::module_::import("__main__");
                m.attr("_last_assignment_error") = false;
            }
            std::optional<bool> fa_opt;
            if (!from_attributes.is_none()) {
                fa_opt = pyobj_to_bool(from_attributes);
            }
            std::optional<ExtraBehavior> extra_opt;
            if (!extra.is_none()) {
                std::string e = extra.cast<std::string>();
                if (e == "allow") extra_opt = ExtraBehavior::Allow;
                else if (e == "forbid") extra_opt = ExtraBehavior::Forbid;
                else extra_opt = ExtraBehavior::Ignore;
            }
            // Runtime by_alias/by_name override the config-level settings.
            std::optional<bool> by_alias_opt;
            if (!by_alias.is_none()) {
                by_alias_opt = pyobj_to_bool(by_alias);
            }
            std::optional<bool> by_name_opt;
            if (!by_name.is_none()) {
                by_name_opt = pyobj_to_bool(by_name);
            }
            // Parse allow_partial: None/False → Off, True → On,
            // "off" → Off, "on" → On, "trailing-strings" → TrailingStrings.
            PartialMode partial_mode = PartialMode::Off;
            if (!allow_partial.is_none()) {
                if (py::isinstance<py::bool_>(allow_partial)) {
                    partial_mode = allow_partial.cast<bool>() ? PartialMode::On : PartialMode::Off;
                } else if (py::isinstance<py::str>(allow_partial)) {
                    std::string s = allow_partial.cast<std::string>();
                    if (s == "on") partial_mode = PartialMode::On;
                    else if (s == "trailing-strings") partial_mode = PartialMode::TrailingStrings;
                    else partial_mode = PartialMode::Off;
                }
            }
            py::object validated = self.validate_python_object(input, pyobj_to_bool(strict), extra_opt, fa_opt, context, /*coerce_strings=*/false, self_instance, by_alias_opt, by_name_opt, partial_mode);

            // If self_instance provided, populate and return it
            if (!self_instance.is_none() && py::hasattr(self_instance, "__dict__")) {
                // object.__setattr__ == Rust force_setattr
                // (PyObject_GenericSetAttr): bypasses dataclass __setattr__
                // so frozen dataclasses can receive the dunder attributes.
                py::object force_setattr = py::module_::import("builtins").attr("object").attr("__setattr__");
                // Rust validate_init semantics: a foreign value returned by
                // an after-validator must NOT overwrite the validated fields
                // already snapshotted onto self.
                bool foreign_return = false;
                if (!self.is_root_model() && !validated.is(self_instance) &&
                    self.has_init_snapshot()) {
                    if (self.apply_init_snapshot(self_instance)) {
                        foreign_return = true;
                    }
                }
                try {
                    if (self.is_root_model()) {
                        // Root model: store the whole validated value as 'root'
                        py::dict d = self_instance.attr("__dict__");
                        d[py::str("root")] = validated;
                        if (!py::hasattr(self_instance, "__pydantic_private__")) {
                            force_setattr(self_instance, py::str("__pydantic_private__"), py::none());
                        }
                        force_setattr(self_instance, py::str("__pydantic_extra__"), py::none());
                        force_setattr(self_instance, py::str("__pydantic_fields_set__"), py::set(py::make_tuple(py::str("root"))));
                    } else if (self.is_dataclass() && !foreign_return) {
                        // Rust DataclassValidator::set_dict_call: the
                        // dataclass-args validator returns
                        // (output_dict, post_init_kwargs).  Replace __dict__
                        // with the validated flat dict (fields + extras for
                        // extra=allow); plain dataclasses do not receive the
                        // __pydantic_* attributes.
                        py::object dc_obj = validated;
                        py::object post_init_kwargs = py::none();
                        if (py::isinstance<py::tuple>(dc_obj) && py::len(dc_obj) == 2 &&
                            py::isinstance<py::dict>(dc_obj[py::int_(0)])) {
                            post_init_kwargs = dc_obj[py::int_(1)];
                            dc_obj = dc_obj[py::int_(0)];
                        }
                        if (py::isinstance<py::dict>(dc_obj)) {
                            py::dict validated_dict = dc_obj.cast<py::dict>();
                            if (validated_dict.contains("__pydantic_defaults__")) validated_dict.attr("pop")("__pydantic_defaults__");
                            if (validated_dict.contains("__pydantic_extra__")) {
                                py::object extra_val = validated_dict["__pydantic_extra__"];
                                validated_dict.attr("pop")("__pydantic_extra__");
                                if (!extra_val.is_none() && py::isinstance<py::dict>(extra_val)) {
                                    for (auto item : extra_val.cast<py::dict>()) {
                                        validated_dict[item.first] = item.second;
                                    }
                                }
                            }
                            if (validated_dict.contains("__pydantic_fields_set__")) validated_dict.attr("pop")("__pydantic_fields_set__");
                            force_setattr(self_instance, py::str("__dict__"), validated_dict);
                            // Rust: __post_init__(*post_init_kwargs)
                            if (py::hasattr(self_instance, "__post_init__")) {
                                if (!post_init_kwargs.is_none() && py::isinstance<py::tuple>(post_init_kwargs)) {
                                    self_instance.attr("__post_init__")(*post_init_kwargs.cast<py::tuple>());
                                } else {
                                    self_instance.attr("__post_init__")();
                                }
                            }
                        }
                    } else if (!foreign_return && py::isinstance<py::dict>(validated)) {
                        py::dict d = self_instance.attr("__dict__");
                        py::dict validated_dict = validated.cast<py::dict>();

                        // Extract special keys before copying to __dict__
                        py::object extra_fields = py::none();
                        py::object fields_set = py::set();

                        if (validated_dict.contains("__pydantic_extra__")) {
                            extra_fields = validated_dict["__pydantic_extra__"];
                            validated_dict.attr("pop")("__pydantic_extra__");
                        }
                        if (validated_dict.contains("__pydantic_fields_set__")) {
                            fields_set = validated_dict["__pydantic_fields_set__"];
                            validated_dict.attr("pop")("__pydantic_fields_set__");
                        }

                        // Copy only declared fields to __dict__
                        for (auto item : validated_dict) {
                            d[item.first] = item.second;
                        }

                        // Set pydantic slot attributes
                        if (!py::hasattr(self_instance, "__pydantic_private__")) {
                            force_setattr(self_instance, py::str("__pydantic_private__"), py::none());
                        }
                        force_setattr(self_instance, py::str("__pydantic_extra__"),
                            extra_fields.is_none() ? py::none() : extra_fields);
                        force_setattr(self_instance, py::str("__pydantic_fields_set__"), fields_set);
                    } else if (!foreign_return && py::hasattr(validated, "__dict__")) {
                        // validated is a model instance (e.g. from FunctionAfterValidator)
                        // Copy its __dict__ to self_instance
                        py::dict d = self_instance.attr("__dict__");
                        py::dict validated_dict = validated.attr("__dict__");
                        for (auto item : validated_dict) {
                            d[item.first] = item.second;
                        }
                        // Copy pydantic slot attributes
                        if (py::hasattr(validated, "__pydantic_extra__")) {
                            force_setattr(self_instance, py::str("__pydantic_extra__"), validated.attr("__pydantic_extra__"));
                        }
                        if (py::hasattr(validated, "__pydantic_fields_set__")) {
                            force_setattr(self_instance, py::str("__pydantic_fields_set__"), validated.attr("__pydantic_fields_set__"));
                        }
                        if (!py::hasattr(self_instance, "__pydantic_private__")) {
                            force_setattr(self_instance, py::str("__pydantic_private__"), py::none());
                        }
                    }
                    // NOTE: model_post_init is called from the Python wrapper after
                    // nested models are processed, to ensure correct call order.
                } catch (const std::exception& e) {
                    // If anything fails, just return validated as-is
                    py::print("validate_python self_instance error:", py::str(e.what()));
                }
                // Rust validate_init surfaces the after-validator's return value
                // (main.py warns when it is not self).  self keeps the validated
                // fields via the snapshot above.
                if (foreign_return) {
                    return validated;
                }
                return self_instance;
            }
            // Slots dataclasses have no __dict__ — populate via object.__setattr__
            if (!self_instance.is_none() && self.is_dataclass()) {
                // Unpack the (output_dict, post_init_kwargs) shape when present
                py::object dc_obj = validated;
                py::object post_init_kwargs = py::none();
                if (py::isinstance<py::tuple>(dc_obj) && py::len(dc_obj) == 2 &&
                    py::isinstance<py::dict>(dc_obj[py::int_(0)])) {
                    post_init_kwargs = dc_obj[py::int_(1)];
                    dc_obj = dc_obj[py::int_(0)];
                }
                if (py::isinstance<py::dict>(dc_obj)) {
                    try {
                        py::dict validated_dict = dc_obj.cast<py::dict>();
                        auto setattr = py::module_::import("builtins").attr("object").attr("__setattr__");
                        for (auto item : validated_dict) {
                            setattr(self_instance, item.first, item.second);
                        }
                        if (py::hasattr(self_instance, "__post_init__")) {
                            if (!post_init_kwargs.is_none() && py::isinstance<py::tuple>(post_init_kwargs)) {
                                self_instance.attr("__post_init__")(*post_init_kwargs.cast<py::tuple>());
                            } else {
                                self_instance.attr("__post_init__")();
                            }
                        }
                    } catch (const std::exception& e) {
                        py::print("validate_python slots dataclass error:", py::str(e.what()));
                    }
                    return self_instance;
                }
            }

            // If validated result is a simple value (like int for dict size), return original input.
            // Call validators produce real function results which may be ints — exclude them.
            // Function validators (before/after/wrap/plain) also produce real results — exclude them.
            if (py::isinstance<py::int_>(validated) && !py::isinstance<py::bool_>(input) && !self.is_call() && !self.is_function_wrapper()) {
                return input;
            }

            return validated;
        }, py::arg("object"), py::arg("strict") = py::none(), py::arg("context") = py::none(), py::arg("self_instance") = py::none(),
             py::arg("extra") = py::none(), py::arg("from_attributes") = py::none(), py::arg("by_alias") = py::none(), py::arg("by_name") = py::none(),
             py::arg("allow_partial") = py::none())
        .def("validate_json", [](SchemaValidator& self, const py::object& jd, py::object strict, py::object context, py::object extra,
                                  py::object allow_partial, py::object by_alias, py::object by_name) {
            std::string js = py::isinstance<py::bytes>(jd) ? jd.cast<std::string>() : jd.cast<std::string>();
            // Parse allow_partial to decide whether to use partial JSON parsing.
            bool partial_active = false;
            if (!allow_partial.is_none()) {
                if (py::isinstance<py::bool_>(allow_partial)) {
                    partial_active = allow_partial.cast<bool>();
                } else if (py::isinstance<py::str>(allow_partial)) {
                    std::string s = allow_partial.cast<std::string>();
                    partial_active = (s == "on" || s == "trailing-strings");
                }
            }
            // Parse JSON to Python object first, then validate as Python
            // This ensures proper type coercion (e.g., "Infinity" string -> float inf)
            py::object py_input;
            try {
                py_input = partial_active ? json_to_pyobj_partial(js) : json_to_pyobj(js);
            } catch (const py::error_already_set& e) {
                // Malformed JSON: convert JSONDecodeError to a ValidationError
                // with json_invalid type (Rust: validate_json throws ValidationError
                // for malformed JSON, not a raw JSONDecodeError).
                std::string err_msg = py::str(e.value()).cast<std::string>();
                ErrorType error_type(ErrorType::Kind::JsonInvalid, "error", err_msg);
                Location location;
                ValError val_error = ValError::line_error(error_type, location, js);
                throw ValidationError(self.title(), InputType::Json, val_error, false);
            }
            std::optional<ExtraBehavior> extra_opt;
            if (!extra.is_none()) {
                std::string e = extra.cast<std::string>();
                if (e == "allow") extra_opt = ExtraBehavior::Allow;
                else if (e == "forbid") extra_opt = ExtraBehavior::Forbid;
                else extra_opt = ExtraBehavior::Ignore;
            }
            // Parse allow_partial (same logic as validate_python).
            PartialMode partial_mode = PartialMode::Off;
            if (!allow_partial.is_none()) {
                if (py::isinstance<py::bool_>(allow_partial)) {
                    partial_mode = allow_partial.cast<bool>() ? PartialMode::On : PartialMode::Off;
                } else if (py::isinstance<py::str>(allow_partial)) {
                    std::string s = allow_partial.cast<std::string>();
                    if (s == "on") partial_mode = PartialMode::On;
                    else if (s == "trailing-strings") partial_mode = PartialMode::TrailingStrings;
                    else partial_mode = PartialMode::Off;
                }
            }
            return self.validate_python_object(py_input, pyobj_to_bool(strict), extra_opt, std::nullopt, context, /*coerce_strings=*/false, py::none(), std::nullopt, std::nullopt, partial_mode);
        }, py::arg("json_data"), py::arg("strict") = py::none(), py::arg("context") = py::none(), py::arg("extra") = py::none(),
             py::arg("allow_partial") = py::none(), py::arg("by_alias") = py::none(), py::arg("by_name") = py::none())
        .def("validate_strings", [](SchemaValidator& self, const py::object& sd, py::object strict, py::object extra,
                                     py::object allow_partial) {
            std::optional<ExtraBehavior> extra_opt;
            if (!extra.is_none()) {
                std::string e = extra.cast<std::string>();
                if (e == "allow") extra_opt = ExtraBehavior::Allow;
                else if (e == "forbid") extra_opt = ExtraBehavior::Forbid;
                else extra_opt = ExtraBehavior::Ignore;
            }
            PartialMode partial_mode = PartialMode::Off;
            if (!allow_partial.is_none()) {
                if (py::isinstance<py::bool_>(allow_partial)) {
                    partial_mode = allow_partial.cast<bool>() ? PartialMode::On : PartialMode::Off;
                } else if (py::isinstance<py::str>(allow_partial)) {
                    std::string s = allow_partial.cast<std::string>();
                    if (s == "on") partial_mode = PartialMode::On;
                    else if (s == "trailing-strings") partial_mode = PartialMode::TrailingStrings;
                    else partial_mode = PartialMode::Off;
                }
            }
            return self.validate_strings_object(sd, pyobj_to_bool(strict), extra_opt, partial_mode);
        }, py::arg("string_data"), py::arg("strict") = py::none(), py::arg("extra") = py::none(),
             py::arg("allow_partial") = py::none())
        .def("isinstance_python", [](SchemaValidator& self, const py::object& input, py::object strict) {
            // NEW: Use native PythonInput - no JSON round-trip!
            return self.isinstance_python_object(input, pyobj_to_bool(strict));
        }, py::arg("object"), py::arg("strict") = py::none())
        .def("get_default_value", [](SchemaValidator& self, py::object strict, py::object context) -> py::object {
            return self.get_default_value(pyobj_to_bool(strict), context);
        }, py::arg("strict") = py::none(), py::arg("context") = py::none())
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
        .def("unicode_string", &Url::unicode_string)
        .def("__eq__", [](const Url& u, const py::object& other) {
            if (py::isinstance<Url>(other)) return u.str() == other.cast<Url>().str();
            if (py::isinstance<py::str>(other)) return u.str() == other.cast<std::string>();
            return false;
        })
        .def("__lt__", [](const Url& u, const Url& o) { return u.str() < o.str(); })
        .def("__le__", [](const Url& u, const Url& o) { return u.str() <= o.str(); })
        .def("__gt__", [](const Url& u, const Url& o) { return u.str() > o.str(); })
        .def("__ge__", [](const Url& u, const Url& o) { return u.str() >= o.str(); })
        .def("__hash__", [](const Url& u) { return py::hash(py::str(u.str())); })
        .def_property_readonly("scheme", &Url::scheme)
        .def_property_readonly("host", [](const Url& u) -> py::object {
            auto h = u.host();
            return h.empty() ? py::none() : py::cast(h);
        })
        .def_property_readonly("port", [](const Url& u) -> py::object {
            auto port = u.port_or_default();
            return port ? py::cast(*port) : py::none();
        })
        .def_property_readonly("path", [](const Url& u) -> py::object {
            auto pth = u.path();
            return pth.empty() ? py::none() : py::cast(pth);
        })
        .def_property_readonly("query", [](const Url& u) -> py::object {
            auto q = u.query();
            return q.empty() ? py::none() : py::cast(q);
        })
        .def_property_readonly("fragment", &Url::fragment)
        .def_property_readonly("username", [](const Url& u) -> py::object {
            auto user = u.user();
            return user && !user->empty() ? py::cast(*user) : py::none();
        })
        .def_property_readonly("password", [](const Url& u) -> py::object {
            auto pw = u.password();
            return pw && !pw->empty() ? py::cast(*pw) : py::none();
        })
        .def_property_readonly("url", &Url::str)
        .def("__len__", [](const Url& u) { return u.str().size(); });

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
        .def("hosts", [](const MultiHostUrl& u) {
            py::list result;
            for (const auto& h : u.hosts()) {
                py::dict host_dict;
                host_dict["username"] = (h.username && !h.username->empty()) ? py::cast(*h.username) : py::none();
                host_dict["password"] = (h.password) ? py::cast(*h.password) : py::none();
                host_dict["host"] = h.host;
                if (h.port) host_dict["port"] = *h.port;
                else host_dict["port"] = py::none();
                result.append(host_dict);
            }
            return result;
        })
        .def_property_readonly("path", [](const MultiHostUrl& u) -> py::object {
            auto pth = u.path();
            return pth.empty() ? py::none() : py::cast(pth);
        })
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
        .def_property_readonly("url", &MultiHostUrl::str)
        .def("__len__", [](const MultiHostUrl& u) { return u.str().size(); })
        .def_static("build",
            [](const std::string& scheme,
               py::object hosts,
               py::object path,
               py::object query,
               py::object fragment,
               py::object host,
               py::object username,
               py::object password,
               py::object port) {
                auto is_none=[](const py::object& o){ return o.is_none(); };
                std::string url = scheme + "://";
                if (!is_none(hosts) && (!is_none(host) || !is_none(username) || !is_none(password) || !is_none(port))) {
                    throw py::value_error("expected one of `hosts` or singular values to be set.");
                }
                if (!is_none(hosts)) {
                    std::vector<std::string> parts;
                    for (auto item : py::cast<py::list>(hosts)) {
                        py::dict d = py::cast<py::dict>(item);
                        bool any = false;
                        std::string seg;
                        auto getu=[&](const char* k)->bool{ return d.contains(k) && !d[k].is_none(); };
                        if (getu("username") || getu("password")) {
                            if (getu("username")) { seg += py::cast<std::string>(d["username"]); }
                            if (getu("password")) { if (!seg.empty()) seg += ":"; seg += py::cast<std::string>(d["password"]); }
                            seg += "@";
                        }
                        if (getu("host")) { seg += py::cast<std::string>(d["host"]); any = true; }
                        if (getu("port")) { seg += ":" + std::to_string(py::cast<int>(d["port"])); any = true; }
                        if (!any) throw py::value_error("expected one of 'host', 'username', 'password' or 'port' to be set");
                        parts.push_back(seg);
                    }
                    for (size_t i=0;i<parts.size();++i){ url += parts[i]; if (i+1<parts.size()) url += ","; }
                } else if (!is_none(host)) {
                    if (!is_none(username)) url += py::cast<std::string>(username);
                    if (!is_none(password)) { if (!url.empty() && url.back()!='/' ) {} url += ":" + py::cast<std::string>(password); }
                    if (!is_none(username) || !is_none(password)) url += "@";
                    url += py::cast<std::string>(host);
                    if (!is_none(port)) url += ":" + std::to_string(py::cast<int>(port));
                } else {
                    throw py::value_error("expected either `host` or `hosts` to be set");
                }
                if (!is_none(path)) { url += "/"; url += py::cast<std::string>(path); }
                if (!is_none(query)) { url += "?"; url += py::cast<std::string>(query); }
                if (!is_none(fragment)) { url += "#"; url += py::cast<std::string>(fragment); }
                return MultiHostUrl(url);
            },
            py::arg("scheme"),
            py::arg("hosts") = py::none(),
            py::arg("path") = py::none(),
            py::arg("query") = py::none(),
            py::arg("fragment") = py::none(),
            py::arg("host") = py::none(),
            py::arg("username") = py::none(),
            py::arg("password") = py::none(),
            py::arg("port") = py::none());
}
