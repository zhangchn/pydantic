#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <datetime.h>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#include "pydantic_core/errors.hpp"
#include "pydantic_core/py_compat.hpp"
#include "pydantic_core/error_types.hpp"
#include "pydantic_core/types.hpp"
#include "pydantic_core/schema_validator.hpp"
#include "pydantic_core/serialization_config.hpp"
#include "pydantic_core/url_types.hpp"

namespace py = pybind11;
using namespace pydantic_core;

std::string get_version() { return "2.49.0"; }

// Runtime polymorphic-serialization flag for the current top-level
// to_python/to_json call (mirrors Rust SerializationExtra::polymorphic_serialization).
static thread_local std::optional<bool> g_polymorphic_serialization{};
// True while a serialize_as_any dump is driven by inference at the outermost
// level; nested re-entry must use the declared serializer instead.
static thread_local bool g_ser_infer_applied = false;
static thread_local bool g_exclude_computed_fields = false;

// Rust SerializationState::check (SerCheck::None/Strict/Lax). Only a union
// enables it while it tries its choices; outside a union a class mismatch is
// not an error, so the model serializer keeps serializing what it was given.
static thread_local int g_ser_check = 0;  // 0 none, 1 strict, 2 lax
struct SerCheckScope {
    int prev;
    explicit SerCheckScope(int level) : prev(g_ser_check) { g_ser_check = level; }
    ~SerCheckScope() { g_ser_check = prev; }
};

// ---------------------------------------------------------------------------
// SerializationInfo — Python-visible info object for custom serializer functions
// ---------------------------------------------------------------------------
struct PySerializationInfo {
    bool round_trip;
    std::string mode;
    std::string field_name;
    py::object context;
    py::object include;
    py::object exclude;
    py::object by_alias;
    bool exclude_unset = false;
    bool exclude_defaults = false;
    bool exclude_none = false;
    bool exclude_computed_fields = false;
    bool serialize_as_any = false;
    std::optional<bool> polymorphic_serialization;
    PySerializationInfo(bool round_trip_, std::string mode_ = "python", std::string field_name_ = "", py::object context_ = py::none())
        : round_trip(round_trip_), mode(std::move(mode_)), field_name(std::move(field_name_)), context(std::move(context_)),
          include(py::none()), exclude(py::none()), by_alias(py::none()) {}
};

// Constants of one top-level serialization call (Rust serializers::extra::Extra).
// Kept thread-local so the recursive to_python/to_json signatures stay as they are.
struct SerCallExtra {
    // Rust SerMode: "python", "json", or any other caller-supplied string.
    std::string mode = "python";
    py::object by_alias = py::none();
    bool exclude_unset = false;
    bool exclude_defaults = false;
    bool exclude_none = false;
    bool exclude_computed_fields = false;
    bool serialize_as_any = false;
    // Rust Extra::serialize_unknown: an unknown value becomes str(value) rather than
    // raising (read by the ObType::Unknown arm, infer.rs:500).
    bool serialize_unknown = false;
    py::object fallback = py::none();
    bool round_trip = false;
    // Rust Extra::context -- the caller's object, handed to every custom serializer the
    // run reaches, so it is a constant of the run and not of one field.
    py::object context = py::none();
    std::string bytes_mode = "utf8";
    std::string timedelta_mode = "iso8601";
    std::string temporal_mode = "iso8601";
    // SerializationConfig::inf_nan_mode -- what a float that is neither finite nor a
    // number becomes (config.rs:26, InfNanMode::from_args).
    std::string inf_nan_mode = "constants";
    // A thread-local is destroyed when its thread ends, which for a thread that merely
    // happened to run a serialization call is at some later, unrelated moment -- by then
    // the interpreter has released it.  Handing a reference back from there is fatal:
    // pybind11 asserts that the GIL is held, and throwing out of that destructor ends the
    // process.  The references are simply not returnable at that point, so they are
    // dropped on the floor rather than released; what is given up is the odd reference to
    // None or to a fallback callable, per thread that ends outside a call.
    ~SerCallExtra() {
        if (!Py_IsInitialized() || !PyGILState_Check()) {
            by_alias.release();
            fallback.release();
            context.release();
        }
    }
    SerCallExtra() = default;
    SerCallExtra(const SerCallExtra&) = default;
    SerCallExtra& operator=(const SerCallExtra&) = default;
};
static thread_local SerCallExtra g_ser_extra;

// The constants of a serialization call live exactly as long as that call.  Rust builds
// one Extra per top-level entry point and carries it down the whole run; the port cannot
// simply do that, because a value that brings its own serializer is delegated through the
// Python-level `to_python`/`to_json`, which rebuilds the constants from its own
// arguments.  Written over the thread local with nothing put back, that rebuild went on
// answering for the rest of the outer run after the nested call returned, and it kept
// this run's `fallback` -- and anything else the constants held -- reachable, and alive,
// until some later call replaced them.  Each entry point now saves what it found and puts
// it back on the way out.
struct SerCallExtraScope {
    SerCallExtra saved;
    SerCallExtraScope() : saved(g_ser_extra) { g_ser_extra = SerCallExtra{}; }
    SerCallExtraScope(const SerCallExtraScope&) = delete;
    SerCallExtraScope& operator=(const SerCallExtraScope&) = delete;
    ~SerCallExtraScope() { g_ser_extra = saved; }
};

// The constants a value that brings its own serializer has to take with it.  Rust does
// not start a new serialization process there: `call_pydantic_serializer` re-scopes only
// the config (infer.rs:667) and hands the same Extra down, so every one of these is the
// outer run's value inside the nested model.  The port leaves the run for a Python-level
// `to_python`, which rebuilds the constants from its arguments, so they are passed on as
// kwargs -- otherwise an `Any` field holding a model emitted fields the outer run had
// been asked to leave out.  `by_alias` and `fallback` are only forwarded when set: Rust
// keeps them optional and the nested serializer resolves each against its own config.
// `include`/`exclude` are absent on purpose -- those are scoped to a point in the tree,
// not to the run, and cannot be re-expressed as the root of a fresh call.
// `serialize_unknown` belongs to the run as much as the exclude flags do -- Rust hands the
// very same Extra down (infer.rs:668), so a model reached by inference still knows what the
// caller asked for -- but neither binding's SchemaSerializer.to_json/to_python accepts it as
// an argument (Rust's has no such keyword at all, and the port must refuse it too), so it
// cannot ride along with the kwargs.  It goes beside the call instead, and whichever entry
// point the call runs picks it up.
static thread_local bool g_ser_delegate_pending = false;
static thread_local bool g_ser_delegate_serialize_unknown = false;

static void ser_apply_delegated_extra() {
    if (g_ser_delegate_pending) g_ser_extra.serialize_unknown = g_ser_delegate_serialize_unknown;
}

static py::dict ser_extra_forwarded() {
    py::dict kw;
    if (!g_ser_extra.by_alias.is_none()) kw["by_alias"] = g_ser_extra.by_alias;
    kw["exclude_unset"] = g_ser_extra.exclude_unset;
    kw["exclude_defaults"] = g_ser_extra.exclude_defaults;
    kw["exclude_none"] = g_ser_extra.exclude_none;
    kw["exclude_computed_fields"] = g_ser_extra.exclude_computed_fields;
    kw["serialize_as_any"] = g_ser_extra.serialize_as_any;
    if (!g_ser_extra.fallback.is_none()) kw["fallback"] = g_ser_extra.fallback;
    kw["round_trip"] = g_ser_extra.round_trip;
    // The caller's context and an explicit polymorphic flag belong to the run as much as
    // the exclude flags do (polymorphism_trampoline.rs reads
    // `state.extra.polymorphic_serialization` and only falls back to the node's config
    // when it is None), so a custom serializer below a nested model saw `context=None`
    // where Rust hands it the object the outer call was given.
    if (!g_ser_extra.context.is_none()) kw["context"] = g_ser_extra.context;
    if (g_polymorphic_serialization.has_value()) {
        kw["polymorphic_serialization"] = *g_polymorphic_serialization;
    }
    g_ser_delegate_pending = true;
    g_ser_delegate_serialize_unknown = g_ser_extra.serialize_unknown;
    return kw;
}

// Rust tools::safe_repr (tools.rs:121): repr(v), and when repr itself raises the
// message says the type could not be printed instead of leaking that failure.
static std::string ser_safe_repr(const py::object& v) {
    try {
        return py::repr(v).cast<std::string>();
    } catch (const py::error_already_set&) {
        PyErr_Clear();
    }
    try {
        return "<unprintable " + py::getattr(py::type::of(v), "__qualname__").cast<std::string>() + " object>";
    } catch (const py::error_already_set&) {
        PyErr_Clear();
    }
    return "<unprintable object>";
}

// The pairs Rust asks a dataclass for (infer.rs:681-706, with get_field_marker at :709-713):
// `__dataclass_fields__` in declaration order, keeping only the entries whose `_field_type` is
// dataclasses._FIELD -- a ClassVar or an InitVar is passed over *before* its name is read, and an
// InitVar has no attribute to read -- with each value taken back by getattr, so a field the
// instance never set still answers from the class and a slot-backed one is found at all.  An
// object's __dict__ answers none of that: it has nothing for a slots class, nothing for an
// init=False field the instance never set, and everything for an attribute no field names.
static py::dict ser_dataclass_pairs(const py::object& v) {
    static const py::object& field_marker =
        held_python_object([] { return py::module_::import("dataclasses").attr("_FIELD"); });
    py::dict out;
    for (auto item : py::getattr(v, "__dataclass_fields__").cast<py::dict>()) {
        py::object name = py::reinterpret_borrow<py::object>(item.first);
        if (!py::getattr(item.second, "_field_type").is(field_marker)) continue;
        out[name] = py::getattr(v, name);
    }
    return out;
}

// Rust infer::serialize_unknown (infer.rs:520): str(value), or a placeholder when
// str() itself raises.
static std::string ser_serialize_unknown(const py::object& v) {
    try {
        return py::str(v).cast<std::string>();
    } catch (const py::error_already_set&) {
        PyErr_Clear();
    }
    try {
        return "<Unserializable " + py::getattr(py::type::of(v), "__qualname__").cast<std::string>() + " object>";
    } catch (const py::error_already_set&) {
        PyErr_Clear();
    }
    return "<Unserializable object>";
}

// `include`/`exclude` are the filters in effect at this point in the tree, not
// the top-level ones (Rust reads them from SerializationState, which descends).
static PySerializationInfo make_ser_info(bool round_trip, const std::string& field_name,
                                         const py::object& context,
                                         const py::object& include, const py::object& exclude) {
    PySerializationInfo info(round_trip, g_ser_extra.mode, field_name, context);
    if (include.ptr()) info.include = include;
    if (exclude.ptr()) info.exclude = exclude;
    info.by_alias = g_ser_extra.by_alias;
    info.exclude_unset = g_ser_extra.exclude_unset;
    info.exclude_defaults = g_ser_extra.exclude_defaults;
    info.exclude_none = g_ser_extra.exclude_none;
    info.exclude_computed_fields = g_ser_extra.exclude_computed_fields;
    info.serialize_as_any = g_ser_extra.serialize_as_any;
    info.polymorphic_serialization = g_polymorphic_serialization;
    return info;
}

// Recursion guard for the polymorphism trampoline.
static thread_local int g_trampoline_depth = 0;

// Serialization that leaves the C++ stack to re-enter the module as a Python call: the
// trampoline handing a subclass to its own serializer, or an inferred value handed to
// its `__pydantic_serializer__`.  Rust stays inside one serde run for both -- the
// trampoline is a serializer node (polymorphism_trampoline.rs) and infer.rs:662 only
// swaps the config -- so exactly one boundary names the failure.  Nothing inside such a
// call may add a JSON wrapper or a class name; that is the outermost run's job.

// How deep a map key's own nesting may be walked before the run is refused rather than
// continued.  Not a rule copied from Rust -- Rust's key path carries no RecursionGuard, it just
// runs until the process does: `to_json` over a nested-tuple key answers `{"leaf":1}` 20000
// tuples deep on the reference build and segfaults at 50000.  The heaviest key path this module
// walks is the one that invents a fresh key at every step -- a fallback handed back another
// unknown, which test_json_key_fallback_termination exercises -- and here it reaches 49750 steps
// before the stack gives out, on a main thread and on a default threading.Thread alike.  The
// bound sits well under that cliff, so such a run ends with an error rather than with the
// interpreter, and far above the nesting of any key a serializer is asked about.  The bound it
// replaced was the value walk's 255, borrowed from Rust's RecursionGuard -- a rule about values
// -- so a key nested past 253 was refused as a circular reference while the reference build
// printed the answer.
static constexpr int SER_INFER_KEY_DEPTH_LIMIT = 10000;

static thread_local int g_ser_json_nested = 0;

struct SerNestedCall {
    const bool saved_pending = g_ser_delegate_pending;
    const bool saved_serialize_unknown = g_ser_delegate_serialize_unknown;
    SerNestedCall() { ++g_ser_json_nested; }
    ~SerNestedCall() {
        --g_ser_json_nested;
        g_ser_delegate_pending = saved_pending;
        g_ser_delegate_serialize_unknown = saved_serialize_unknown;
    }
    SerNestedCall(const SerNestedCall&) = delete;
    SerNestedCall& operator=(const SerNestedCall&) = delete;
};

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
        if (py::type::of(value).equal(cls) || !py_hasattr(value, "__pydantic_serializer__")) {
            return false;
        }
        ++g_trampoline_depth;
        struct DepthGuard { ~DepthGuard() { --g_trampoline_depth; } } guard;
        py::object sub_ser = py::getattr(value, "__pydantic_serializer__");
        py::dict kw = ser_extra_forwarded();
        kw["include"] = include;
        kw["exclude"] = exclude;
        kw["by_alias"] = by_alias;
        kw["exclude_unset"] = exclude_unset;
        kw["exclude_defaults"] = exclude_defaults;
        kw["exclude_none"] = exc_none;
        kw["round_trip"] = round_trip;
        SerNestedCall nested;
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
        if (py_hasattr(o, "__dict__")) {
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
static py::object json_to_pyobj_partial(const std::string& json_str,
                                        PartialMode mode = PartialMode::On) {
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
    // Offset of the currently open string's quote, and whether a truncated
    // escape sequence was left behind when the input ran out.
    size_t open_string_start = std::string::npos;
    bool dangling_escape = false;

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
                dangling_escape = true;
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
                open_string_start = repaired.size();
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

    if (in_string && mode == PartialMode::TrailingStrings) {
        // Rust (jiter allow_partial='trailing-strings') keeps a string value
        // that the input cut short, so the caller sees the partial text. Only
        // a value counts: a trailing key without its ':' is still dropped.
        bool is_value = false;
        for (size_t j = open_string_start; j > 0; j--) {
            char prev = repaired[j - 1];
            if (prev == ' ' || prev == '\n' || prev == '\t' || prev == '\r') continue;
            is_value = (prev == ':');
            break;
        }
        if (is_value) {
            if (dangling_escape) repaired.pop_back();
            repaired += '"';
            in_string = false;
        }
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

// Re-indents an already-built JSON document the way serde_json's pretty printer
// does; defined next to the top-level to_json entry points that use it.
static std::string json_pretty_print(const std::string& compact, int indent);
using SerRef = std::shared_ptr<SerNode>;

// ---------------------------------------------------------------------------
// Shared JSON-mode conversion helpers
// ---------------------------------------------------------------------------

// The base64 Rust writes is URL_SAFE (config.rs:316 and :329) with canonical padding:
// '-' and '_' in place of '+' and '/'.  A '+' in the table here is invisible until a
// value happens to hit those two indexes -- b'\xfb\xff' is "-_8=" in Rust and was
// "+/8=" here -- and base64 read back by a url_unsafe_b64n.pad decoder differs.
static std::string b64_encode_string(const std::string& b) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
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

static std::string bytes_hex_encode(const std::string& b) {
    static const char* hx = "0123456789abcdef";
    std::string out;
    for (unsigned char c : b) {
        out += hx[c >> 4];
        out += hx[c & 0x0F];
    }
    return out;
}

struct Utf8Bad {
    size_t start;      // Rust's valid_up_to: where the byte string stops being valid
    size_t end;        // one past the bytes the failure is reported over
    bool incomplete;   // the buffer ran out mid-sequence, so there is no error_len
};

// Both of the utf8 failures Rust reports come off one walk, and CPython's decoder words
// neither of them the same way, so the walk is done here: serialize_bytes shows std's
// Display for a from_utf8 error (config.rs:323), "invalid utf-8 sequence of N bytes from
// index I" -- N counting the bytes of the sequence that were still well formed -- or
// "incomplete utf-8 byte sequence from index I" when the buffer stops halfway through
// one; bytes_to_string hands the same pair to pyo3 (config.rs:314), which reports it as
// a UnicodeDecodeError spanning [valid_up_to, valid_up_to+error_len), or the rest of the
// buffer when the sequence was only truncated.  Before this the JSON writer emitted the
// undecodable bytes as they stood, and JSON containing them is not JSON at all.
static std::optional<Utf8Bad> utf8_bad(const std::string& b) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(b.data());
    size_t n = b.size();
    for (size_t i = 0; i < n; ) {
        unsigned char c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        // 0xC0/0xC1 can only be overlong and 0xF5.. cannot start anything, and a bare
        // continuation byte is a one-byte error by itself.
        if (c < 0xC2 || c > 0xF4) return Utf8Bad{i, i + 1, false};
        size_t want = (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        // 0xE0/0xED/0xF0/0xF4 narrow the range of the first continuation byte: overlong,
        // surrogates, past U+10FFFF.
        unsigned char lo = 0x80, hi = 0xBF;
        if (c == 0xE0) lo = 0xA0;
        else if (c == 0xED) hi = 0x9F;
        else if (c == 0xF0) lo = 0x90;
        else if (c == 0xF4) hi = 0x8F;
        for (size_t k = 1; k < want; k++) {
            if (i + k >= n) return Utf8Bad{i, n, true};
            unsigned char d = p[i + k];
            bool ok = (k == 1) ? (d >= lo && d <= hi) : (d >= 0x80 && d <= 0xBF);
            if (!ok) return Utf8Bad{i, i + k, false};
        }
        i += want;
    }
    return std::nullopt;
}

static std::string rust_utf8_reason(const Utf8Bad& bad) {
    if (bad.incomplete)
        return "incomplete utf-8 byte sequence from index " + std::to_string(bad.start);
    return "invalid utf-8 sequence of " + std::to_string(bad.end - bad.start) +
           " bytes from index " + std::to_string(bad.start);
}

// pyo3's PyUnicodeDecodeError::new_utf8 (config.rs:336) always blames the fixed reason
// "invalid utf-8"; CPython's own decoder names the case ("invalid start byte", "invalid
// continuation byte", "unexpected end of data"), so letting its error escape would give
// every one of these messages a different tail than Rust's.
static void raise_rust_decode_error(const std::string& b, const Utf8Bad& bad) {
    PyObject* err = PyUnicodeDecodeError_Create("utf-8", b.data(), (Py_ssize_t)b.size(),
                                                (Py_ssize_t)bad.start, (Py_ssize_t)bad.end,
                                                "invalid utf-8");
    if (err) {
        PyErr_SetObject(PyExc_UnicodeDecodeError, err);
        Py_DECREF(err);
    } else {
        PyErr_Clear();
        PyErr_SetString(PyExc_UnicodeDecodeError, "invalid utf-8");
    }
    throw py::error_already_set();
}

// BytesMode::from_str, called by every entry point that takes the kwarg.  The trailing
// "or " is in Rust's message verbatim.
static void check_bytes_mode(const std::string& mode) {
    if (mode != "utf8" && mode != "base64" && mode != "hex")
        throw SchemaError("Invalid BytesMode serialization mode: `" + mode +
                          "`, expected utf8 or base64 or hex or ");
}

// The other three modes an entry point takes, same macro and same trailing "or "
// (config.rs:118-147).
static void check_temporal_mode(const std::string& mode) {
    if (mode != "iso8601" && mode != "seconds" && mode != "milliseconds")
        throw SchemaError("Invalid TemporalMode serialization mode: `" + mode +
                          "`, expected iso8601 or seconds or milliseconds or ");
}

static void check_timedelta_mode(const std::string& mode) {
    if (mode != "iso8601" && mode != "float")
        throw SchemaError("Invalid TimedeltaMode serialization mode: `" + mode +
                          "`, expected iso8601 or float or ");
}

static void check_inf_nan_mode(const std::string& mode) {
    if (mode != "null" && mode != "constants" && mode != "strings")
        throw SchemaError("Invalid InfNanMode serialization mode: `" + mode +
                          "`, expected null or constants or strings or ");
}

// SerializationConfig::from_args (config.rs:58-74).  One mode answers for every temporal
// kind, so timedelta_mode="float" -- which has no milliseconds of its own to ask for --
// becomes seconds for a datetime too, and is never even read when temporal_mode says
// something else: with temporal_mode="seconds" a timedelta_mode="bogus" goes unanswered.
static std::string resolve_temporal_mode(const std::string& timedelta_mode,
                                         const std::string& temporal_mode) {
    if (temporal_mode != "iso8601") {
        check_temporal_mode(temporal_mode);
        return temporal_mode;
    }
    check_timedelta_mode(timedelta_mode);
    return timedelta_mode == "float" ? "seconds" : "iso8601";
}

// What a resolved temporal mode means for the one kind that has a second name for its
// shape: seconds is "float" and milliseconds is "milliseconds" to the timedelta leaf.
static std::string timedelta_shape_for(const std::string& temporal_mode) {
    if (temporal_mode == "seconds") return "float";
    if (temporal_mode == "milliseconds") return "milliseconds";
    return "iso8601";
}

// Convert one typed leaf value for JSON-mode serialization (SchemaSerializer
// mode="json" and to_jsonable_python). Returns true and sets `out` when the
// (type, value) pair has a JSON-compatible form; false otherwise.
// Convert a datetime/date/time to a float timestamp (seconds or milliseconds).
// Naive datetimes are treated as UTC (matches Rust ser_json_temporal behavior).
// Returns false if the type is not a temporal type.
static bool temporal_to_float(const std::string& type, const py::object& value,
                              const std::string& ser_json_temporal, double& out) {
    if (type != "datetime" && type != "date" && type != "time") return false;
    py::object datetime_mod = py::module_::import("datetime");
    long long whole_seconds = 0;
    long long microseconds = 0;
    if (type == "datetime") {
        py::object dt = value;
        if (value.attr("tzinfo").is_none()) {
            dt = value.attr("replace")(py::arg("tzinfo") = datetime_mod.attr("timezone").attr("utc"));
        }
        whole_seconds = static_cast<long long>(dt.attr("timestamp")().cast<double>());
        microseconds = dt.attr("microsecond").cast<long long>();
    } else if (type == "date") {
        py::object dt = datetime_mod.attr("datetime")(
            value.attr("year"), value.attr("month"), value.attr("day"),
            py::arg("tzinfo") = datetime_mod.attr("timezone").attr("utc"));
        whole_seconds = static_cast<long long>(dt.attr("timestamp")().cast<double>());
        microseconds = 0;
    } else { // time
        whole_seconds = value.attr("hour").cast<long long>() * 3600 +
                        value.attr("minute").cast<long long>() * 60 +
                        value.attr("second").cast<long long>();
        microseconds = value.attr("microsecond").cast<long long>();
    }
    if (ser_json_temporal == "milliseconds") {
        out = static_cast<double>(whole_seconds) * 1000.0 + static_cast<double>(microseconds) / 1000.0;
    } else {
        out = static_cast<double>(whole_seconds) + static_cast<double>(microseconds) / 1000000.0;
    }
    return true;
}

static void ensure_datetime_api();
static bool json_leaf_convert(const std::string& type, const py::object& value,
                              const std::string& ser_json_bytes,
                              const std::string& ser_json_timedelta,
                              const std::string& ser_json_temporal,
                              py::object& out);

// Rust infer_to_python picks the JSON leaf form from the *value*, not from a
// schema type, so the infer path needs its own dispatch.
// Rust's Display for f64: the shortest digits that round-trip, written in plain
// decimal notation (never with an exponent) and with inf/NaN spelled out.
static std::string rust_f64_display(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    std::string s = format_double(value);
    // The sign comes off before the exponent is looked for: "%g" writes the '-' ahead of
    // the mantissa, so on a negative number the 'e' sits one place further along than the
    // renormalising below assumes, and the 'e' itself ends up inside the digit string
    // (-86400500 came out as "-864005e0", -1.5e-7 as "-15e00000").
    bool neg = !s.empty() && s[0] == '-';
    if (neg) s.erase(s.begin());
    size_t e = s.find('e');
    if (e == std::string::npos) return neg ? "-" + s : s;
    std::string mantissa = s.substr(0, e);
    int exp = std::stoi(s.substr(e + 1));
    std::string digits;
    int point = 0;
    for (size_t i = 0; i < mantissa.size(); ++i) {
        char c = mantissa[i];
        if (c == '.') {
            point = static_cast<int>(digits.size());
            continue;
        }
        digits += c;
    }
    if (point == 0) point = static_cast<int>(digits.size());
    int dec = point + exp;
    std::string out;
    if (dec <= 0) {
        out = "0." + std::string(static_cast<size_t>(-dec), '0') + digits;
    } else if (static_cast<size_t>(dec) >= digits.size()) {
        out = digits + std::string(static_cast<size_t>(dec) - digits.size(), '0');
    } else {
        out = digits.substr(0, static_cast<size_t>(dec)) + "." + digits.substr(static_cast<size_t>(dec));
    }
    if (out.find('.') != std::string::npos) {
        size_t last = out.find_last_not_of('0');
        if (out[last] == '.') --last;
        out.erase(last + 1);
    }
    return neg ? "-" + out : out;
}

// serde_json writes a float through ryu's "pretty" form, and that is what Rust's JSON
// output carries: the shortest digits that round-trip, written as plain decimal while the
// leading digit sits between 1e-5 and 1e16 and as d[.ddd]e+NN outside it, with an unpadded
// exponent and a ".0" on a value with no fraction.  Python's repr shares the digits but not
// the thresholds or the padding ("1e-05" where the JSON is 0.00001, "1e-07" where it is
// 1e-7), and Rust's own Display is plain-only ("10000000000000000" for 1e16), so neither is
// reusable here.  std::to_string is worse than both: six decimals, so a typed float node
// turned 3.14159265358979 into 3.141593 and 1e-7 into 0.0.
static std::string ser_json_f64(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    const bool neg = std::signbit(value) != 0;
    std::string sign = neg ? "-" : "";
    if (value == 0.0) return sign + "0.0";

    // %e always writes one leading digit and an exponent, so the digits and their place
    // can be read off without guessing which notation %g settled on.
    const double magnitude = neg ? -value : value;
    char buf[64];
    for (int precision = 0; precision <= 16; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*e", precision, magnitude);
        if (std::strtod(buf, nullptr) == magnitude) break;
    }
    std::string text(buf);
    size_t epos = text.find('e');
    std::string mantissa = text.substr(0, epos);
    const int e10 = std::stoi(text.substr(epos + 1));
    std::string digits;
    for (char ch : mantissa) {
        if (ch != '.') digits += ch;
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    if (e10 >= -5 && e10 <= 15) {
        std::string out;
        if (e10 >= 0) {
            size_t whole = static_cast<size_t>(e10) + 1;
            out = digits.substr(0, std::min(whole, digits.size()));
            if (whole > digits.size()) out += std::string(whole - digits.size(), '0');
            if (whole < digits.size()) out += "." + digits.substr(whole);
            else out += ".0";
        } else {
            out = "0." + std::string(static_cast<size_t>(-e10) - 1, '0') + digits;
        }
        return sign + out;
    }
    std::string out = digits.substr(0, 1);
    if (digits.size() > 1) out += "." + digits.substr(1);
    out += 'e';
    out += e10 >= 0 ? "+" : "-";
    out += std::to_string(e10 >= 0 ? e10 : -e10);
    return sign + out;
}

// Rust's numeric leaves write the node's number, not the value's: an int node serializes an i64
// and a float node a double, so True, an int subclass or an IntEnum come out as 1 or 1.0 instead
// of true or the subclass.  A float node takes everything C's float converter accepts -- Decimal,
// Fraction, any __float__ -- which is the set the wheel accepts and no wider: str and bytes are
// refused although float("1.5") works, because they are not numbers at all, and an int past the
// range of a double is refused too (float.rs:128-140).
static bool ser_extract_f64(PyObject* v, double* out) {
    if (!PyNumber_Check(v)) return false;
    double d = PyFloat_AsDouble(v);
    if (d == -1.0 && PyErr_Occurred()) {
        PyErr_Clear();
        return false;
    }
    *out = d;
    return true;
}

// The same numbers as the exact type a json-mode run answers with.  Rust rebuilds them from what
// the value holds rather than from the converter its class might have written: crate::input::Int is
// an i64 or a BigInt, so an int subclass that defines __int__ still answers its digits, and
// PyFloat_AsDouble reads the double a float subclass stores rather than asking its __float__.
static py::object ser_exact_int(const py::object& v) {
    if (PyLong_CheckExact(v.ptr())) return v;
    int overflow = 0;
    long long x = PyLong_AsLongLongAndOverflow(v.ptr(), &overflow);
    if (overflow == 0 && !(x == -1 && PyErr_Occurred()))
        return py::reinterpret_steal<py::object>(PyLong_FromLongLong(x));
    // Too big for a machine word, so ask int() for it: a plain subclass answers an exact int with
    // every digit.  One that both defines __int__ and does not fit a machine word is the case this
    // reads the subclass's answer rather than its digits, which no public digit read reaches.
    PyErr_Clear();
    PyObject* big = PyNumber_Long(v.ptr());
    if (!big) throw py::error_already_set();
    return py::reinterpret_steal<py::object>(big);
}

static py::object ser_exact_float(const py::object& v) {
    if (PyFloat_CheckExact(v.ptr())) return v;
    double d = PyFloat_AsDouble(v.ptr());
    if (d == -1.0 && PyErr_Occurred()) throw py::error_already_set();
    return py::float_(d);
}

// Text is copied by concatenating, which builds a new str from the buffer instead of asking the
// subclass for __str__, which it is free to override.
static py::object ser_exact_str(const py::object& v) {
    if (PyUnicode_CheckExact(v.ptr())) return v;
    static const py::object& empty = held_python_object(
        [] { return py::reinterpret_steal<py::object>(PyUnicode_FromString("")); });
    PyObject* copied = PyUnicode_Concat(empty.ptr(), v.ptr());
    if (!copied) throw py::error_already_set();
    return py::reinterpret_steal<py::object>(copied);
}

// What a float node answers once it has a double, the inf_nan_mode included -- Rust's
// serialize_f64 (float.rs:59-73), which the JSON writer reaches for every float value.
static std::string ser_json_f64_modes(double d, const std::string& inf_nan_mode) {
    if (std::isnan(d)) {
        if (inf_nan_mode == "null") return "null";
        if (inf_nan_mode == "strings") return "\"NaN\"";
        return "NaN";
    }
    if (std::isinf(d)) {
        if (inf_nan_mode == "null") return "null";
        if (inf_nan_mode == "strings") return d > 0 ? "\"Infinity\"" : "\"-Infinity\"";
        return d > 0 ? "Infinity" : "-Infinity";
    }
    return ser_json_f64(d);
}

// Rust serializers::type_serializers::complex::complex_to_str: the imaginary
// part comes first, and the real part is prefixed only when it is non-zero.
static std::string complex_to_str_rust(double re, double im) {
    std::string s = rust_f64_display(im) + "j";
    if (re != 0.0) {
        std::string sign = (std::isnan(im) || !std::signbit(im)) ? "+" : "";
        s = rust_f64_display(re) + sign + s;
    }
    return s;
}

static bool json_infer_leaf(const py::object& value, py::object& out) {
    ensure_datetime_api();
    std::string t;
    if (py::isinstance<py::bytes>(value) || py::isinstance<py::bytearray>(value)) t = "bytes";
    else if (PyDelta_Check(value.ptr())) t = "timedelta";
    else if (PyDateTime_Check(value.ptr())) t = "datetime";
    else if (PyDate_Check(value.ptr())) t = "date";
    else if (PyTime_Check(value.ptr())) t = "time";
    else if (PyComplex_Check(value.ptr())) t = "complex";
    else return false;
    return json_leaf_convert(t, value, g_ser_extra.bytes_mode, g_ser_extra.timedelta_mode,
                             g_ser_extra.temporal_mode, out);
}

static bool json_leaf_convert(const std::string& type, const py::object& value,
                              const std::string& ser_json_bytes,
                              const std::string& ser_json_timedelta,
                              const std::string& ser_json_temporal,
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
        if (type == "complex") {
            if (!PyComplex_Check(value.ptr())) return false;
            out = py::str(complex_to_str_rust(PyComplex_RealAsDouble(value.ptr()),
                                              PyComplex_ImagAsDouble(value.ptr())));
            return true;
        }
        if (type == "datetime" || type == "date" || type == "time") {
            if (ser_json_temporal == "seconds" || ser_json_temporal == "milliseconds") {
                double ts;
                if (temporal_to_float(type, value, ser_json_temporal, ts)) {
                    out = py::float_(ts);
                    return true;
                }
            }
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
            if (ser_json_timedelta == "milliseconds") {
                long long micros = (long long)(days * 86400 + seconds) * 1000000LL + microseconds;
                out = py::float_(micros / 1000.0);
                return true;
            }
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

// A lazy SerializationIterator outlives the call that built it, so it cannot share that call's
// frame.  Rust's __next__ rebuilds a state from the Extra the view kept (generator.rs:181) and
// runs final_check after serializing the item (:190), which has the effect of a fresh frame per
// pull that starts out holding whatever the run had already registered when the view was built:
// those warnings are reported again on *every* pull, next to whatever the item just registered.
static void ser_warn_enter_snapshot(bool enabled, bool as_error,
                                    const std::vector<std::string>& seed) {
    ser_warn_stack().push_back(SerWarnFrame{enabled, as_error, seed});
}

struct SerWarnViewScope {
    bool closed = false;
    SerWarnViewScope(bool enabled, bool as_error, const std::vector<std::string>& seed) {
        ser_warn_enter_snapshot(enabled, as_error, seed);
    }
    void emit() {  // pops the frame and reports what it collected
        if (closed) return;
        closed = true;
        ser_warn_leave(false);
    }
    ~SerWarnViewScope() {
        if (!closed) ser_warn_leave(true);
    }
};

// Depth of "a union is trying its candidates" regions. A serializer function
// that rejects its value while a candidate is merely being tried is not a
// warning: Rust keeps those errors for itself and only reports them once every
// choice failed (union.rs register_union_serialization_warnings).
static thread_local int g_ser_attempt_depth = 0;
struct SerAttemptScope {
    SerAttemptScope() { ++g_ser_attempt_depth; }
    ~SerAttemptScope() { --g_ser_attempt_depth; }
};

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

// Tag keys and discriminator values are compared by value: a str-Enum member
// must match the literal tag it carries, not its repr ("SomeEnum.DOG").
static std::string ser_tag_string(py::handle value) {
    if (PyUnicode_Check(value.ptr())) return value.cast<std::string>();
    try {
        return py::str(value).cast<std::string>();
    } catch (const py::error_already_set&) {
        PyErr_Clear();
        return std::string();
    }
}

// Rust registers a bare PydanticSerializationUnexpectedValue(message) for
// serializer decisions that are not a field type mismatch.
static void ser_warn_message(const std::string& message, const py::object& value) {
    auto& stack = ser_warn_stack();
    if (stack.empty() || !stack.back().enabled) return;
    std::string msg = message + " [input_value=" + ser_warn_input_repr(value) +
                      ", input_type=" + ser_warn_input_type(value) + "]";
    ser_warn_register("PydanticSerializationUnexpectedValue(" + msg + ")");
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

// filter.rs:29-32, the refusal for a negative index over an iterable with no length.  It is a
// Python error rather than pybind's builtin_exception because the latter is a std::runtime_error
// that only becomes a ValueError in pybind's translator, past the JSON boundary where the wheel's
// `Error serializing to JSON: ValueError: ...` rename is decided.
static void ser_unsized_index_refusal() {
    PyErr_SetString(PyExc_ValueError,
                    "Negative indices cannot be used to exclude items on unsized iterables");
    throw py::error_already_set();
}

// Fold an index key against the length.  filter.rs:21-36 (`map_negative_index`) asks Python for
// `key % len` of *every* key, not just negative ones, so include={7: True} over a three-item list
// is a request for index 1; anything whose modulo fails -- a string key, a length of zero -- is
// left as it is, exactly like Rust's `unwrap_or_else(|_| value.clone())`.  A null `len` is Rust's
// `None`: an iterable whose length is never taken (a generator, `None` at generator.rs:68) has
// nothing to fold a key by, so the key is used exactly as written and a negative one is refused
// outright (:27-33) rather than quietly kept.
static py::object map_negative_index(const py::object& key, const py::object* len) {
    if (len) {
        try {
            return key.attr("__mod__")(*len);
        } catch (const py::error_already_set&) {
            PyErr_Clear();
            return key;
        }
    }
    static const py::object& zero = held_python_object([] { return py::int_(0); });
    int negative = PyObject_RichCompareBool(key.ptr(), zero.ptr(), Py_LT);
    if (negative < 0)
        PyErr_Clear();   // :28 `unwrap_or(false)` -- a key that will not answer is not a negative one
    else if (negative == 1)
        ser_unsized_index_refusal();
    return key;
}

// Map every key or member of an include/exclude object (:38-58).  Only a dict's keys and a set's
// members name positions, so anything else is left alone for the filter to refuse or to ask
// `__contains__` about.
static py::object map_negative_indices(const py::object& obj, const py::object* len) {
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

// The form for a walk that always has the length in hand -- a tuple and a named tuple are both
// measured before their items are paired with serializers.
static py::object map_negative_indices(const py::object& obj, py::ssize_t len) {
    py::object n = py::int_(len);
    return map_negative_indices(obj, &n);
}

// The include/exclude a container node asks about each of its positions.  A sized iterable has a
// length to fold an index key against, so the fold is done once, before the walk starts: with a
// length in hand nothing can be refused and the answer cannot change from one element to the next.
// An unsized one has no length, and Rust maps the keys *inside* every filter consult of its own
// (filter.rs:100-105, `index_filter` -> `map_negative_indices`), which is where a negative key is
// refused.  Asking per element is what lets an iterable that yields nothing pass without ever
// getting as far as complaining.
struct SerIndexFilter {
    py::object include = py::none();          // as the call handed them
    py::object exclude = py::none();
    py::object folded_include = py::none();   // a sized iterable's, folded once
    py::object folded_exclude = py::none();
    bool unsized = false;

    // `len < 0` is how these walks spell "no length"; a filter that was not passed stays None.
    void bind(const py::object& inc, const py::object& exc, py::ssize_t len) {
        include = inc;
        exclude = exc;
        unsized = (len < 0);
        if (unsized) return;
        py::object n = py::int_(len);
        if (!inc.is_none()) folded_include = map_negative_indices(inc, &n);
        if (!exc.is_none()) folded_exclude = map_negative_indices(exc, &n);
    }

    // The decision for one position, in the shape apply_ser_filter answers with: whether to keep
    // the element and which sub-filter its contents are then asked about.
    SerFilterResult ask(py::ssize_t index) const;
};

// Rust's GeneratorSerializer keeps a Python-mode iterator lazy: model_dump()
// hands the caller a SerializationIterator that serializes each item as it is
// pulled, so the items are never materialized and never measured.
struct SerializationIterator {
    py::object items;  // the wrapped iterator, also what its repr shows
    SerRef child;
    // An iterator the *infer* walk was handed has no item serializer to keep -- the item
    // is whatever the walk makes of it (infer.rs:266-271 hands it to AnySerializer).
    bool infer_items = false;
    // generator.rs:68 never takes a length, so the view is unsized whatever was handed it.
    SerIndexFilter filter;
    py::object context = py::none();
    bool exc_none = false;
    bool round_trip = false;
    bool by_alias = false;
    bool exclude_unset = false;
    bool exclude_defaults = false;
    // What the run had already registered when this view was built, re-emitted on every pull.
    bool warn_enabled = false;
    bool warn_as_error = false;
    std::vector<std::string> warn_seed;
    size_t index = 0;
};

// The view is built while the run's own frame is the innermost one, which is where the warnings
// it will report on each pull have to be picked up from.
static void ser_warn_snapshot_into(SerializationIterator& it) {
    if (ser_warn_stack().empty()) return;
    const SerWarnFrame& frame = ser_warn_stack().back();
    it.warn_enabled = frame.enabled;
    it.warn_as_error = frame.as_error;
    it.warn_seed = frame.items;
}

static py::object make_serialization_iterator(const py::object& value, const SerRef& child,
                                              bool exc_none, bool round_trip,
                                              const py::object& include, const py::object& exclude,
                                              bool by_alias, bool exclude_unset, bool exclude_defaults,
                                              const py::object& context) {
    auto out = std::make_shared<SerializationIterator>();
    out->items = py::iter(value);
    out->child = child;
    out->filter.bind(include, exclude, -1);
    out->context = context;
    out->exc_none = exc_none;
    out->round_trip = round_trip;
    out->by_alias = by_alias;
    out->exclude_unset = exclude_unset;
    out->exclude_defaults = exclude_defaults;
    ser_warn_snapshot_into(*out);
    return py::cast(out);
}

// The infer walk's own answer for an iterator (infer.rs:264-271): the same lazy view, with
// each item left to the walk rather than to a serializer node, and the run's pair asked about
// each position with no length to fold against -- index_filter over value.len()? (:204-213).
static py::object make_inferred_iterator(const py::object& value, bool exc_none, bool round_trip,
                                         const py::object& include, const py::object& exclude) {
    auto out = std::make_shared<SerializationIterator>();
    out->items = py::iter(value);
    out->infer_items = true;
    // infer.rs:204-213 asks index_filter with no length for every item it pulls, so an inferred
    // iterator is filtered exactly like the generator node's view -- the pair travels with the
    // view, which is why it outlives the call that built it, and an item that is itself a
    // container is inferred under the pair that position named.
    out->filter.bind(include, exclude, -1);
    out->exc_none = exc_none;
    out->round_trip = round_trip;
    ser_warn_snapshot_into(*out);
    return py::cast(out);
}

// Apply call-time include/exclude to a key (or index).  Mirrors the Rust
// FilterLogic::filter with default_filter = true (no schema-level filter).
// Rust SerializationCallable::__call__ takes an index_key only as an int or a
// str and refuses anything else with a TypeError before the include/exclude
// filter is consulted.  Letting a nonsense key reach the filter made it read as
// an omission, so a bare PydanticOmit came out of the serializer instead of the
// type error the caller asked about.
static void ser_check_index_key(const py::object& index_key) {
    if (py::isinstance<py::str>(index_key) || py::isinstance<py::int_>(index_key)) return;
    std::string repr;
    try {
        repr = py::repr(index_key).cast<std::string>();
    } catch (const py::error_already_set&) {
        PyErr_Clear();
        repr = "<unknown>";
    }
    PyErr_SetString(PyExc_TypeError,
                    ("'index_key' is expected to be an integer or a string, got '" + repr + "'").c_str());
    throw py::error_already_set();
}

// filter.rs:290 -- the ask for an object that is neither a dict nor a set.  It has three answers,
// not two: yes, no, and "this object cannot answer" -- no `__contains__` at all, or a call with the
// key that raises -- and the third is what lands on the refusal below rather than on a plain no.
// Defined with the jsonable walk's copy of the filter, which asks the same question; both walks need
// the same three answers.
static bool ser_check_contains(const py::object& o, const py::object& key, bool* found);

// A filter that is neither a set nor a dict is refused.  This is a Python error rather than
// pybind's builtin_exception -- which is a std::runtime_error that only becomes a TypeError in
// pybind's translator, past the JSON boundary -- because Rust raises it as a PyErr from wherever a
// filter is consulted, so a json run renames it at errors.rs:71 and reports
// `Error serializing to JSON: TypeError: ...` while a python run reports the bare TypeError.
static void ser_filter_refusal(const char* argument) {
    std::string message = std::string("`") + argument + "` argument must be a set or dict.";
    PyErr_SetString(PyExc_TypeError, message.c_str());
    throw py::error_already_set();
}

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
        } else {
            bool holds = false;
            if (!ser_check_contains(exclude, key, &holds))
                ser_filter_refusal("exclude");
            if (holds) {
                out.omit = true;
                return out;
            }
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
        bool holds = false;
        if (!ser_check_contains(include, key, &holds))
            ser_filter_refusal("include");
        if (holds) {
            out.include = py::none();
            out.exclude = next_exclude;
            return out;
        }
        out.omit = true;  // key not in include
        return out;
    }

    // No include filter: keep the item, propagate the exclude sub-filter
    if (!next_exclude.is_none()) {
        out.exclude = next_exclude;
    }
    return out;
}

SerFilterResult SerIndexFilter::ask(py::ssize_t index) const {
    py::object key = py::int_(static_cast<long long>(index));
    if (!unsized) return apply_ser_filter(key, folded_include, folded_exclude);
    // No length to fold by: every key is asked about as it was written, and a negative one is
    // refused here -- once per element, only once an element exists to ask about.
    py::object inc = include.is_none() ? include : map_negative_indices(include, nullptr);
    py::object exc = exclude.is_none() ? exclude : map_negative_indices(exclude, nullptr);
    return apply_ser_filter(key, inc, exc);
}

// Thread-local recursion guard mirroring Rust's RecursionState
// (recursion_guard.rs): re-entering the same (object, definition node)
// pair is a cycle; depth over the limit is too deep.  Both surface as
// ValueError in Rust (serializers/extra.rs).

// Rust decides what an escaping serialization failure is called in one place:
// se_err_py_err (serializers/errors.rs:63) waits at the end of the JSON-string run
// (to_json_bytes, shared.rs:602) and every failure that reached it through the serde
// boundary is renamed `Error serializing to JSON: <what it said>`.  A failure that
// came from Python says which it was -- `Error serializing to JSON: ValueError: xxx`
// -- because the serde boundary stringifies a PyErr with its type name in front
// (errors.rs:21).  The python-output run has no such wrapper, so what a site has to
// know is whether it is inside a JSON run, not what its error looks like.
static thread_local int g_ser_json_depth = 0;

struct SerJsonRun {
    SerJsonRun() { ++g_ser_json_depth; }
    ~SerJsonRun() { --g_ser_json_depth; }
    SerJsonRun(const SerJsonRun&) = delete;
    SerJsonRun& operator=(const SerJsonRun&) = delete;
};

// The naming described above, for the failures that came from Python.  Returns false
// when the error keeps its own identity instead: a run nested inside another one, or
// anything inside a call that re-entered the module (both re-enter the entry point as
// Python calls, so only the run that started the serialization gets to name), or an
// unexpected value, which Rust carries across the boundary as a marker because it asks
// for another try rather than reporting.
static bool ser_json_name_python_error(py::error_already_set& e, std::string* out) {
    if (g_ser_json_depth != 1 || g_ser_json_nested != 0) return false;
    try {
        py::object unexpected = py::module_::import("pydantic_core_cpp").attr("PydanticSerializationUnexpectedValue");
        int is = PyObject_IsInstance(e.value().ptr(), unexpected.ptr());
        PyErr_Clear();
        if (is == 1) return false;
        // Nor is a failure that already names itself one renamed: a delegated model's run
        // throws `Unable to serialize unknown type: ...` and reaches this boundary through a
        // Python call, where serde would hand the very same PyErr on untouched
        // (errors.rs:63 renames only serde's own SerializationError).  The unexpected-value
        // subclass keeps the exemption above, which asks for another try rather than
        // reporting.
        py::object ser_err = py::module_::import("pydantic_core_cpp").attr("PydanticSerializationError");
        int is_ser = PyObject_IsInstance(e.value().ptr(), ser_err.ptr());
        PyErr_Clear();
        if (is_ser == 1) return false;
    } catch (...) { PyErr_Clear(); }
    std::string type_name, detail;
    try { type_name = py::str(e.type().attr("__name__")).cast<std::string>(); } catch (...) { PyErr_Clear(); }
    try { detail = py::str(e.value()).cast<std::string>(); } catch (...) { PyErr_Clear(); }
    PyErr_Clear();
    *out = type_name + ": " + detail;
    return true;
}

// Rust serializers::type_serializers::function::on_error. Returns true when the
// error was a PydanticSerializationUnexpectedValue (the caller should fall back
// to the inner schema); otherwise throws the wrapped PydanticSerializationError.
static bool handle_ser_call_error(const py::error_already_set& e, const std::string& function_name) {
    PyObject* exc = e.value().ptr();
    try {
        py::object unexpected = py::module_::import("pydantic_core_cpp").attr("PydanticSerializationUnexpectedValue");
        if (PyObject_IsInstance(exc, unexpected.ptr()) == 1) {
            if (g_ser_check != 0 || g_ser_attempt_depth != 0) {
                // Rust on_error: while a union checks its candidates the error
                // propagates (Err(err)) so the next choice is tried; falling
                // back to inference here would let the wrong branch succeed.
                PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(exc)), exc);
                throw py::error_already_set();
            }
            std::string msg;
            try { msg = py::str(e.value()).cast<std::string>(); } catch (...) { PyErr_Clear(); }
            PyErr_Clear();
            ser_warn_register("PydanticSerializationUnexpectedValue(" + msg + ")");
            return true;
        }
    } catch (...) { PyErr_Clear(); }
    std::string type_name;
    try { type_name = py::str(e.type().attr("__name__")).cast<std::string>(); } catch (...) { PyErr_Clear(); }
    std::string detail;
    try { detail = py::str(e.value()).cast<std::string>(); } catch (...) { PyErr_Clear(); }
    std::string inner = "Error calling function `" + function_name + "`: " + type_name + ": " + detail;
    // This wording is pydantic's own, so a JSON run reports it the way the serde
    // boundary reports any Python error: class name in front, behind the prefix.  The
    // boundary itself no longer renames what already names itself a serialization failure
    // (see ser_json_name_python_error), so a call that re-entered the module and failed
    // here gets its prefix from this site too -- deferring it used to leave the outer run
    // to add it, and `nested polym dump_json` came out without one.
    if (g_ser_json_depth > 0)
        throw PydanticSerializationError("Error serializing to JSON: PydanticSerializationError: " + inner);
    throw PydanticSerializationError(inner);
}

// Rust serializers::type_serializers::format: when_used gating.
static bool ser_when_used_skips(const std::string& when_used, bool json_mode, const py::object& value) {
    if ((when_used == "json" || when_used == "json-unless-none") && !json_mode) return true;
    if ((when_used == "unless-none" || when_used == "json-unless-none") && value.is_none()) return true;
    return false;
}

static void ensure_datetime_api() {
    static bool loaded = false;
    if (!loaded) { PyDateTime_IMPORT; loaded = true; }
}

static bool py_is_datetime_like(PyObject* p) {
    ensure_datetime_api();
    return PyDateTime_Check(p) || PyDate_Check(p) || PyTime_Check(p) || PyDelta_Check(p);
}

// Types Rust's infer layer recognizes on its own; anything else is ObType::Unknown
// and therefore eligible for the caller-supplied `fallback`.
static bool py_infer_known_type(const py::object& v) {
    if (v.is_none()) return true;
    if (py::isinstance<py::bool_>(v) || py::isinstance<py::int_>(v) || py::isinstance<py::float_>(v) ||
        py::isinstance<py::str>(v) || py::isinstance<py::bytes>(v) || py::isinstance<py::bytearray>(v) ||
        py_is_datetime_like(v.ptr()) || py::isinstance<py::dict>(v) || py::isinstance<py::list>(v) ||
        py::isinstance<py::tuple>(v) || py::isinstance<py::set>(v) || py::isinstance<py::frozenset>(v) ||
        py::isinstance(v, py_fraction_type())) {
        return true;
    }
    return py_hasattr(v, "__pydantic_serializer__");
}

// Rust's infer layer consults its own type table before it calls a value unknown
// (ob_type.rs:215-407): every exact type in that table, everything that reaches one of them
// by walking tp_base, and then the isinstance list at :336-407 -- plus the two ducks tested
// on the value itself, a `__pydantic_serializer__` (:421) and `__dataclass_fields__` (:410),
// neither of which a type object is allowed to answer.  What this says unknown about has no
// arm of infer's own to fall into, which is what a json run's refusal is keyed on
// (infer.rs:221-231).  PurePosixPath is unknown on purpose: the table names pathlib.Path
// (:296) and a PurePath's bases never reach it, and `range`/`array.array` are not in it
// either -- both are refused by a json run however iterable they look.
static bool ser_infer_known_ob_type(const py::object& v) {
    PyObject* p = v.ptr();
    if (v.is_none() || PyBool_Check(p) || PyLong_Check(p) || PyFloat_Check(p) ||
        PyUnicode_Check(p) || PyBytes_Check(p) || PyByteArray_Check(p) || PyComplex_Check(p) ||
        PyList_Check(p) || PyTuple_Check(p) || PyDict_Check(p) || PySet_Check(p) ||
        PyFrozenSet_Check(p) || py_is_datetime_like(p))
        return true;
    // ob_type.rs:294 counts a value its own iterator as ObType::Generator, and pyo3 asks
    // that as PyIter_Check -- the Py_TPFLAGS_HAVE_ITER flag, not tp_iternext, which every
    // heap type carries as slot_tp_iternext whether or not the class defines __next__.
    // Asking the pointer instead called a plain object an iterator and never refused it.
    if (PyIter_Check(p)) return true;
    if (py::isinstance(v, py_fraction_type())) return true;
    try {
        static const py::object& deque_cls =
            held_python_object([] { return py::module_::import("collections").attr("deque"); });
        if (py::isinstance(v, deque_cls)) return true;
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    try {
        static const py::object& enum_cls =
            held_python_object([] { return py::module_::import("enum").attr("Enum"); });
        if (py::isinstance(v, enum_cls)) return true;
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    if (!PyType_Check(p)) {
        if (py_hasattr(v, "__pydantic_serializer__")) return true;
        if (py_hasattr(v, "__dataclass_fields__")) return true;
    }
    // The rest of the isinstance list, in ob_type.rs's own order.
    try {
        static const py::object& ip_cls = held_python_object([] {
            py::object m = py::module_::import("ipaddress");
            return py::make_tuple(m.attr("IPv4Address"), m.attr("IPv6Address"),
                                  m.attr("IPv4Network"), m.attr("IPv6Network"));
        });
        if (py::isinstance(v, ip_cls)) return true;
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    try {
        static const py::object& pattern_cls =
            held_python_object([] { return py::module_::import("re").attr("Pattern"); });
        if (py::isinstance(v, pattern_cls)) return true;
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    try {
        static const py::object& decimal_cls =
            held_python_object([] { return py::module_::import("decimal").attr("Decimal"); });
        if (py::isinstance(v, decimal_cls)) return true;
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    try {
        static const py::object& uuid_cls =
            held_python_object([] { return py::module_::import("uuid").attr("UUID"); });
        if (py::isinstance(v, uuid_cls)) return true;
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    try {
        static const py::object& path_cls =
            held_python_object([] { return py::module_::import("pathlib").attr("Path"); });
        if (py::isinstance(v, path_cls)) return true;
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    try {
        static const py::object& url_mod = held_python_object(
            [] { return py::module_::import("pydantic_core_cpp._pydantic_core_cpp"); });
        if (py::isinstance(v, url_mod.attr("Url")) || py::isinstance(v, url_mod.attr("MultiHostUrl")))
            return true;
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    return false;
}

// A text form is asked of Python, not of the object's C type.  pybind's py::str is a checked cast:
// shown a str subclass it hands back the object's content and never runs __str__, while Python's
// str() -- and pyo3's .str(), which is what Rust calls for these kinds (infer.rs:188, :221, :613,
// :627) -- do.  So a str subclass that rewrites its own text prints its own text, and one whose
// __str__ refuses propagates the refusal instead of quietly printing what it was built from.
static py::object ser_text_form(const py::object& v) {
    PyObject* s = PyObject_Str(v.ptr());
    if (!s) throw py::error_already_set();
    return py::reinterpret_steal<py::object>(s);
}

// pyo3's Display for a Python object is str() with this placeholder when str() raises, and
// infer.rs:122 takes Decimal and Fraction through it -- so a leaf whose __str__ explodes is
// still serialized, as text, in a run that was never given a fallback.  The name is the
// class's own, not its qualified name.
static py::object ser_display(const py::object& v) {
    try {
        // PyObject_Str rather than py::str, for the str-subclass reason above; the refusal still
        // has to reach the catch below, which is where the placeholder lives
        PyObject* s = PyObject_Str(v.ptr());
        if (!s) throw py::error_already_set();
        return py::reinterpret_steal<py::object>(s);
    } catch (const py::error_already_set&) {
        PyErr_Clear();
        std::string name = "?";
        try { name = py::getattr(py::type::of(v), "__name__").cast<std::string>(); }
        catch (const py::error_already_set&) { PyErr_Clear(); }
        return py::str("<unprintable " + name + " object>");
    }
}

// infer.rs:191-194 hands a UUID to uuid_to_string (type_serializers/uuid.rs:16-21), which
// reads the value's own `int` and prints it as sixteen big-endian bytes: a URN- or hex-built
// UUID comes back in the hyphenated form, and a subclass whose __str__ lies is not believed.
// The three refusals of that extraction are CPython's own words -- "'X' object cannot be
// interpreted as an integer", "int too big to convert", "can't convert negative int to
// unsigned" -- so the extraction is asked of CPython and its errors are left to propagate
// rather than composed here.  They have to arrive as Python's own: an error the port raises
// itself keeps its identity across a JSON run, while one that came from a call is renamed by
// the serde boundary to `Error serializing to JSON: TypeError: ...`.
static py::object ser_uuid_to_string(const py::object& v) {
    py::object i = v.attr("int");
    PyObject* indexed = PyNumber_Index(i.ptr());
    if (!indexed) throw py::error_already_set();
    py::object count = py::reinterpret_steal<py::object>(indexed);
    static const py::object& to_bytes = held_python_object(
        [] { return py::getattr(py::module_::import("builtins").attr("int"), "to_bytes"); });
    std::string bytes = to_bytes(count, 16, "big").cast<py::bytes>().cast<std::string>();
    static const char* hexd = "0123456789abcdef";
    std::string hex;
    hex.reserve(36);
    for (size_t n = 0; n < bytes.size(); ++n) {
        unsigned char b = static_cast<unsigned char>(bytes[n]);
        hex += hexd[b >> 4];
        hex += hexd[b & 0xf];
        if (n == 3 || n == 5 || n == 7 || n == 9) hex += '-';
    }
    return py::str(hex);
}

// The ObTypes whose json form is a string, shared by every json-running walk: Url,
// MultiHostUrl, Path and the four named ipaddress types go through serialize_via_str
// (infer.rs:184-190), which is str() with the error left standing -- unlike Decimal and
// Fraction, a Path whose __str__ raises does not become a placeholder.  A pattern gives its
// `pattern` attribute, not its str() (infer.rs:220, :675-678), because str() of a compiled
// pattern is its repr.  IPv4Interface and IPv6Interface arrive through their Address base,
// which is why they print with the prefix length.
struct SerStrClasses {
    py::object decimal, uuid, path, ip, url, multihost_url, pattern;
};

static SerStrClasses make_ser_str_classes() {
    SerStrClasses classes;
    // Filled a class at a time: one class that cannot be imported costs its own ObType, where
    // a single try around the whole table would leave a walk with no str forms at all.
    auto fill = [](py::object& slot, const char* module, const char* name) {
        try {
            slot = py::module_::import(module).attr(name);
        } catch (const py::error_already_set&) {
            PyErr_Clear();
        }
    };
    fill(classes.decimal, "decimal", "Decimal");
    fill(classes.uuid, "uuid", "UUID");
    fill(classes.path, "pathlib", "Path");
    fill(classes.pattern, "re", "Pattern");
    try {
        py::object m = py::module_::import("ipaddress");
        classes.ip = py::make_tuple(m.attr("IPv4Address"), m.attr("IPv6Address"),
                                    m.attr("IPv4Network"), m.attr("IPv6Network"));
    } catch (const py::error_already_set&) {
        PyErr_Clear();
    }
    try {
        py::object m = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
        classes.url = m.attr("Url");
        classes.multihost_url = m.attr("MultiHostUrl");
    } catch (const py::error_already_set&) {
        PyErr_Clear();
    }
    return classes;
}

// Given up to the heap and never deleted, for the reason held_python_object gives: a
// py::object whose destructor runs after Py_Finalize takes the interpreter down with it.
static const SerStrClasses& ser_str_classes() {
    static const SerStrClasses* classes = new SerStrClasses(make_ser_str_classes());
    return *classes;
}

static bool ser_infer_json_str(const py::object& v, py::object& out) {
    const SerStrClasses& c = ser_str_classes();
    // A class that is not in the table asks nothing, so its ObType is left to the walk's own
    // answer rather than the whole table being skipped.
    auto is_a = [&](const py::object& cls) { return cls.ptr() != nullptr && py::isinstance(v, cls); };
    // infer.rs:122: Display, which is str() with a placeholder and so cannot fail.
    if (is_a(c.decimal) || is_a(py_fraction_type())) {
        out = ser_display(v);
        return true;
    }
    if (is_a(c.uuid)) {
        out = ser_uuid_to_string(v);
        return true;
    }
    if (is_a(c.path) || is_a(c.ip) || is_a(c.url) || is_a(c.multihost_url)) {
        out = ser_text_form(v);
        return true;
    }
    if (is_a(c.pattern)) {
        out = ser_text_form(py::getattr(v, "pattern"));
        return true;
    }
    return false;
}

// Rust CombinedSerializer enum variant names, as they appear in SchemaSerializer.__repr__.
static std::string ser_variant_name(const std::string& t) {
    static const std::unordered_map<std::string, std::string> names = {
        {"any", "Any"}, {"none", "None"}, {"bool", "Bool"}, {"int", "Int"}, {"float", "Float"},
        {"str", "Str"}, {"bytes", "Bytes"}, {"date", "Date"}, {"time", "Time"},
        {"datetime", "Datetime"}, {"timedelta", "Timedelta"}, {"list", "List"}, {"set", "Set"},
        {"frozenset", "FrozenSet"}, {"deque", "Deque"}, {"tuple", "Tuple"}, {"named-tuple", "NamedTuple"},
        {"generator", "Generator"}, {"dict", "Dict"},
        {"nullable", "Nullable"}, {"nullable-union", "Union"}, {"union", "Union"},
        {"tagged-union", "TaggedUnion"}, {"default", "WithDefault"}, {"with-default", "WithDefault"},
        {"model", "Model"}, {"model-fields", "Fields"}, {"typed-dict", "TypedDict"},
        {"dataclass", "Dataclass"}, {"dataclass-args", "DataclassArgs"}, {"format", "Format"},
        {"to-string", "ToString"}, {"enum", "Enum"}, {"literal", "Literal"}, {"uuid", "Uuid"},
        {"url", "Url"}, {"json", "Json"}, {"definitions", "Definitions"},
        {"definition-ref", "DefinitionRef"}, {"is-instance", "Any"}, {"is-subclass", "Any"},
        {"lax-or-strict", "Any"}, {"json-or-python", "JsonOrPython"}, {"complex", "Complex"},
        {"decimal", "Decimal"}, {"fraction", "Fraction"}, {"function-plain", "Function"},
        {"function-after", "Function"},
        {"function-before", "Function"}, {"function-wrap", "Function"},
    };
    auto it = names.find(t);
    return it != names.end() ? it->second : "Any";
}

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
    // The same two questions asked of this node itself, by the mismatch rule in
    // the walks below.
    std::string type_name_for_warning() const;
    bool value_matches_type(const py::object& v) const;

    // Rust leaf serializers warn once per container item whose runtime type
    // disagrees with the declared item serializer, then fall back to inference.  The
    // item's own node does both (see the rule in to_python/to_json); what is left here
    // is the union's round, where a mismatch is the answer that ends this choice.
    static const py::object& check_item_type(const SerRef& child, const py::object& item) {
        if (child && !value_matches_type(child, item)) {
            // Rust CollectWarnings::on_fallback_py: while a union checks its
            // choices (SerCheck::Strict/Lax) the mismatch is an error, not a
            // warning, so the union bails out of this choice and tries the next.
            if (g_ser_check != 0) {
                throw std::runtime_error("Unexpected value for serializer " + type_name_for_warning(child));
            }
        }
        return item;
    }
    // Rust locates a discriminator on a dict key, an object attribute, or
    // __pydantic_extra__: a validated value is a model instance, not a dict.
    static bool lookup_discriminator(const py::object& value, const std::vector<std::string>& path, py::object* out) {
        py::object cur = value;
        for (const auto& key : path) {
            py::object next;
            if (py::isinstance<py::dict>(cur)) {
                auto d = cur.cast<py::dict>();
                if (!d.contains(key)) return false;
                next = d[py::str(key)];
            } else {
                bool found = false;
                try {
                    if (py_hasattr(cur, key.c_str())) { next = py::getattr(cur, key.c_str()); found = true; }
                } catch (const py::error_already_set&) {
                    PyErr_Clear();
                }
                if (!found) {
                    try {
                        py::object extra = py::getattr(cur, "__pydantic_extra__");
                        if (!extra.is_none() && py::isinstance<py::dict>(extra) &&
                            extra.cast<py::dict>().contains(key)) {
                            next = extra[py::str(key)];
                            found = true;
                        }
                    } catch (const py::error_already_set&) {
                        PyErr_Clear();
                    }
                }
                if (!found) return false;
            }
            cur = std::move(next);
        }
        if (cur.is_none()) return false;
        *out = std::move(cur);
        return true;
    }

    std::vector<SerRef> children;
    // A tuple schema's variadic_item_index: >=0 means one serializer answers every
    // item, which is also why a variadic tuple never warns about the item count.
    int tuple_variadic_index = -1;
    // For tagged-union: map from tag -> serializer
    std::unordered_map<std::string, SerRef> tagged;
    // For tagged-union: the discriminator lookup paths, and the choices in
    // declaration order for the left-to-right fallback.
    std::vector<std::vector<std::string>> tagged_discriminator;
    // Discriminator(callable): Rust calls it on the value being serialized.
    py::object tagged_discriminator_callable = py::none();
    std::vector<SerRef> tagged_left_to_right;
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
    // Rust keeps the serializer function's name to wrap its exceptions.
    std::string func_name;
    // For default
    py::object default_val;
    bool has_default_val = false;
    // For default: default_factory (Rust DefaultType::DefaultFactory)
    py::object default_factory;
    bool default_factory_takes_data = false;
    // For format
    std::string format_str;
    // Rust Function*Serializer::return_serializer: serializes what a custom
    // serializer function returned ("return_schema" in the ser schema).
    SerRef return_ser;
    // For root models
    bool root_model = false;
    // For model/dataclass serializers: expected Python class (union discrimination)
    py::object class_;
    // Rust NamedTupleSerializer::name - the class named in a wrong-type warning.
    std::string class_name;
    // Polymorphic serialization enabled via schema config (Rust enabled_from_config)
    bool polymorphic_from_config = false;
    // Rust ModelSerializer::has_extra - only a model whose own config sets
    // extra_fields_behavior="allow" serializes its __pydantic_extra__ entries.
    bool extra_allowed = false;
    // Rust TypedDictSerializer with extra_behavior=allow (FieldsMode::TypedDictAllow):
    // a typed-dict keeps its undeclared keys in its own dict, and an optional
    // extras_schema says how to serialize them.
    bool typed_dict_allow_extra = false;
    SerRef extra_ser;
    // Rust's FloatSerializer bakes this at build time (float.rs:45-56) from the config
    // pydantic handed the *serializer*, and a config that does not name it falls back to
    // InfNanMode::default() -- the enum's first variant, Null (config.rs:141-147), not
    // SerializationConfig::default()'s Constants.  Only the module entry points print
    // constants by default, because their default is the string their binding reads.
    std::string inf_nan_mode = "null";
    // Whether the model (or dataclass) this node was built under has settled the mode for
    // it.  Only that nearest enclosing model is asked (model.rs:117, dataclass.rs:103), so
    // neither an ancestor's config nor the one the serializer was handed answers afterwards.
    bool inf_nan_claimed = false;
    // For bytes serialization: "utf8" (default), "base64", or "hex"
    std::string ser_json_bytes = "utf8";
    // For timedelta serialization: "iso8601" (default) or "float"
    std::string ser_json_timedelta = "iso8601";
    // For datetime/date/time serialization: "iso8601" (default), "seconds", or "milliseconds"
    std::string ser_json_temporal = "iso8601";

    // Report the Python objects this node and its subtree hold so the cyclic
    // collector can see them (see Validator::visit_refs): a serializer names the
    // class it serializes and the class keeps its serializer, so neither the
    // pair nor anything it holds is reachable from a traversal the collector
    // can do on its own.
    void visit_refs(RefVisitor visit, void* arg) const {
        if (!gc_detail::enter_node(this)) return;
        for (const auto& child : children) {
            if (child) child->visit_refs(visit, arg);
        }
        for (const auto& choice : tagged) {
            if (choice.second) choice.second->visit_refs(visit, arg);
        }
        for (const auto& choice : tagged_left_to_right) {
            if (choice) choice->visit_refs(visit, arg);
        }
        for (const auto& field : fields) {
            if (field.second) field.second->visit_refs(visit, arg);
        }
        if (return_ser) return_ser->visit_refs(visit, arg);
        if (extra_ser) extra_ser->visit_refs(visit, arg);
        visit_ref(visit, arg, tagged_discriminator_callable);
        for (const auto& excluded : field_exclude_if) {
            visit_ref(visit, arg, excluded.second);
        }
        visit_ref(visit, arg, py_func);
        visit_ref(visit, arg, default_val);
        visit_ref(visit, arg, default_factory);
        visit_ref(visit, arg, class_);
    }

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
        func_name = other.func_name;
        info_arg = other.info_arg;
        is_field_serializer = other.is_field_serializer;
        when_used = other.when_used;
        default_val = other.default_val;
        has_default_val = other.has_default_val;
        default_factory = other.default_factory;
        default_factory_takes_data = other.default_factory_takes_data;
        format_str = other.format_str;
        return_ser = other.return_ser;
        root_model = other.root_model;
        class_ = other.class_;
        class_name = other.class_name;
        polymorphic_from_config = other.polymorphic_from_config;
        extra_allowed = other.extra_allowed;
        typed_dict_allow_extra = other.typed_dict_allow_extra;
        extra_ser = other.extra_ser;
        inf_nan_mode = other.inf_nan_mode;
        ser_json_bytes = other.ser_json_bytes;
        ser_json_timedelta = other.ser_json_timedelta;
        ser_json_temporal = other.ser_json_temporal;
    }

    // Rust's derived Debug on the serializer tree prints each nested field by
    // name; pydantic's tests read "return_serializer: <Variant>" out of repr().
    void append_repr(std::string& out, int indent,
                     std::unordered_set<const SerNode*>* visited = nullptr) const {
        out += std::string(indent * 4, ' ');
        out += "serializer: " + ser_variant_name(type) + "\n";
        // Recursive models point back at their own definition node.
        std::unordered_set<const SerNode*> local_visited;
        if (!visited) visited = &local_visited;
        if (!visited->insert(this).second) return;
        if (return_ser) {
            out += std::string(indent * 4, ' ');
            out += "return_serializer: " + ser_variant_name(return_ser->type) + "\n";
            return_ser->append_repr(out, indent + 1, visited);
        }
        for (auto& c : children) if (c) c->append_repr(out, indent + 1, visited);
        for (const auto& k : field_order) {
            auto it = fields.find(k);
            if (it != fields.end() && it->second) it->second->append_repr(out, indent + 1, visited);
        }
        for (auto& [tag, t] : tagged) if (t) t->append_repr(out, indent + 1, visited);
    }

    // Rust leaf serializers report an unexpected value while a union is checking
    // its choices (SerCheck::Strict/Lax); without it a union would accept the
    // first choice that performs no type check at all.
    // Rust ObTypeLookup::is_type compares type pointers: the exact type is
    // IsType::Exact, a matching ancestor (bool for an int node, a str subclass
    // for a str node) is IsType::Subclass, anything else is IsType::False.
    // Returns 1/0/-1 for those.
    static int ser_type_match(const std::string& node_type, const py::object& value) {
        PyObject* v = value.ptr();
        if (node_type == "str" || node_type == "string" || node_type == "str-constrained") {
            if (Py_TYPE(v) == &PyUnicode_Type) return 1;
            return PyUnicode_Check(v) ? 0 : -1;
        }
        if (node_type == "int" || node_type == "int-constrained") {
            if (Py_TYPE(v) == &PyLong_Type) return 1;
            // bool is a Python int subclass, so it reaches here as a subclass.
            return PyLong_Check(v) ? 0 : -1;
        }
        if (node_type == "float" || node_type == "float-constrained") {
            if (Py_TYPE(v) == &PyFloat_Type) return 1;
            // Rust special-cases an int as subclass input to the float serializer.
            if (PyFloat_Check(v) || PyLong_Check(v)) return 0;
            return -1;
        }
        if (node_type == "bool") return PyBool_Check(v) ? 1 : -1;
        if (node_type == "bytes") {
            if (Py_TYPE(v) == &PyBytes_Type) return 1;
            return PyBytes_Check(v) ? 0 : -1;
        }
        if (node_type == "list" || node_type == "generator") {
            if (Py_TYPE(v) == &PyList_Type) return 1;
            return PyList_Check(v) ? 0 : -1;
        }
        if (node_type == "deque") {
            if (reinterpret_cast<PyObject*>(Py_TYPE(v)) == py_deque_type().ptr()) return 1;
            return py::isinstance(v, py_deque_type()) ? 0 : -1;
        }
        if (node_type == "fraction") {
            if (reinterpret_cast<PyObject*>(Py_TYPE(v)) == py_fraction_type().ptr()) return 1;
            return py::isinstance(v, py_fraction_type()) ? 0 : -1;
        }
        if (node_type == "set") {
            if (Py_TYPE(v) == &PySet_Type) return 1;
            return PySet_Check(v) ? 0 : -1;
        }
        if (node_type == "frozenset") {
            if (Py_TYPE(v) == &PyFrozenSet_Type) return 1;
            return PyFrozenSet_Check(v) ? 0 : -1;
        }
        if (node_type == "tuple") {
            if (Py_TYPE(v) == &PyTuple_Type) return 1;
            if (PyTuple_Check(v) || PyList_Check(v)) return 0;
            return -1;
        }
        if (node_type == "dict") {
            if (Py_TYPE(v) == &PyDict_Type) return 1;
            return PyDict_Check(v) ? 0 : -1;
        }
        return 1;
    }

    bool ser_check_accepts(const py::object& value) const {
        if (g_ser_check == 0) return true;
        if (type == "named-tuple") {
            // Rust tuple_value: while a union checks its choices the value has to
            // be an instance of the named tuple class itself; the lax round only
            // requires that it can be cast to a tuple at all.
            if (g_ser_check == 1) return class_ && py::isinstance(value, class_);
            return py::isinstance<py::tuple>(value) || py::isinstance<py::list>(value);
        }
        int match = ser_type_match(type, value);
        if (match != 0) return match > 0;
        // IsType::Subclass: SerCheck::Strict refuses it, SerCheck::Lax allows it.
        return g_ser_check == 2;
    }

    // Feed a custom serializer function's result through its declared return
    // serializer (Rust FunctionPlain/WrapSerializer::to_python).  Rust hands the
    // result an empty include/exclude pair -- function.rs:220 "Filtering was done
    // by the function, so drop include/exclude for the return serializer" -- so
    // every caller below passes py::none() for both.
    py::object apply_return_ser(const py::object& result, bool json_mode, bool exc_none, bool round_trip,
                                const py::object& include, const py::object& exclude, bool by_alias,
                                bool exclude_unset, bool exclude_defaults, const py::object& context) const {
        if (!return_ser) return result;
        if (!value_matches_type(return_ser, result)) {
            ser_warn_unexpected_value("", type_name_for_warning(return_ser), result);
        }
        return return_ser->to_python(result, json_mode, exc_none, round_trip, include, exclude, by_alias,
                                     exclude_unset, exclude_defaults, context);
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
        // None is not a wrong-typed value, it is the null leaf every node answers to,
        // at any depth and without a warning: int/list/tuple/model all answer None, and
        // only the key path stringifies it.  Answering here rather than in each leaf
        // also keeps the leaf forms (b'None', b'false', b'"None"', a cast RuntimeError,
        // py::len on a NoneType) from being reached at all.
        if (value.is_none()) {
            return py::none();
        }
        // Rust OnErr::Warn (serializers/mod.rs): a typed serializer whose input type
        // refuses the value leaves "Expected `X` ..." behind and the value goes
        // through inference, so it is never written in the node's own form.  While a
        // union checks its choices the mismatch stays an error instead, so that arm
        // keeps falling through to ser_check_accepts below.
        if (g_ser_check == 0 && !value_matches_type(value)) {
            ser_warn_unexpected_value("", type_name_for_warning(), value);
            return serialize_any_value(value, exc_none, round_trip, json_mode, include, exclude);
        }
        // Rust's IsType::Subclass arm (simple.rs:126-132, float.rs:103-110): a JSON run extracts
        // the value into the node's own number -- a bool at an int node becomes the int 1, an int
        // or bool at a float node becomes a float -- while a python run takes the other arm and
        // hands back the object it was given (string.rs:47-49 is the same rule for text).  The
        // extraction reads what the value holds and not what its class converts to, so an int
        // subclass that defines __int__ arrives as its digits, a float subclass that defines
        // __float__ as the double it stores and a str subclass as the text it holds; an int past
        // the range of a double raises through the float arm here, which is what the wheel does too.
        if (g_ser_check == 0 && json_mode &&
            (type == "int" || type == "int-constrained" || type == "float" || type == "float-constrained" ||
             type == "str" || type == "string" || type == "str-constrained") &&
            ser_type_match(type, value) == 0) {
            if (type == "int" || type == "int-constrained") return ser_exact_int(value);
            if (type == "float" || type == "float-constrained") return ser_exact_float(value);
            return ser_exact_str(value);
        }
        if (!ser_check_accepts(value)) {
            throw std::runtime_error("Unexpected value for serializer " + type);
        }
        if (type == "lax-or-strict") {
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
        }
        if (type == "is-instance" || type == "is-subclass") {
            return value;
        }
        if (type == "missing-sentinel") {
            py::object missing = missing_sentinel_obj();
            if (value.is(missing)) return value;
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
            py::object exc_type = py::module_::import("pydantic_core_cpp").attr("PydanticSerializationUnexpectedValue");
            PyErr_SetString(exc_type.ptr(), "Expected 'MISSING' sentinel");
            throw py::error_already_set();
        }
        if (type == "ellipsis") {
            if (value.is(py::ellipsis())) return value;
            py::object exc_type = py::module_::import("pydantic_core_cpp").attr("PydanticSerializationUnexpectedValue");
            PyErr_SetString(exc_type.ptr(), "Expected 'Ellipsis' object");
            throw py::error_already_set();
        }
        if (type == "nullable" || type == "nullable-union") {
            if (value.is_none()) return py::none();
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
        }
        if (type == "union") {
            SerAttemptScope attempt;
            // Rust UnionChoices::serialize: an inner union does not run its own
            // strict/lax rounds while an outer union is checking choices; it gets
            // one pass at the current check level and the failure propagates so
            // the outer union can move on to its next choice. Running every
            // round per nesting level re-ran each choice's serializers.
            if (g_ser_check != 0) {
                std::exception_ptr last;
                for (auto& c : children) {
                    try { return c->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context); }
                    catch (...) { last = std::current_exception(); }
                }
                if (last) std::rethrow_exception(last);
                throw std::runtime_error("Unexpected value for serializer union");
            }
            // Top level: try the choices left to right with strict class checks,
            // then again with lax (isinstance) checks.
            for (int level = 1; level <= 2; level++) {
                SerCheckScope scope(level);
                for (auto& c : children) {
                    try { return c->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context); } catch (...) {}
                }
            }
            for (auto& c : children) {
                try { return c->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context); } catch (...) {}
            }
        }
        if (type == "tagged-union" && !tagged_left_to_right.empty()) {
            SerAttemptScope attempt;
            // Rust TaggedUnionSerializer resolves the discriminator and
            // serializes with the matching choice. Reading it only from a dict
            // key (and guessing the key name) meant a validated model instance
            // never matched, so the fallback below emitted a different
            // variant's fields.
            if (!tagged_discriminator_callable.is_none()) {
                // Rust: Discriminator::Function(func) => func.call1((value,)).ok(),
                // so a callable that raises simply leaves no tag and the
                // left-to-right fallback below takes over.
                try {
                    py::object tag_obj = tagged_discriminator_callable(value);
                    std::string tag = ser_tag_string(tag_obj);
                    auto cit = tagged.find(tag);
                    if (cit != tagged.end()) {
                        try {
                            return cit->second->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
                        } catch (const py::error_already_set&) {
                            PyErr_Clear();
                            ser_warn_message("Pydantic serialization failed for tagged union variant '" + tag + "'", value);
                        }
                    }
                } catch (const py::error_already_set&) {
                    PyErr_Clear();
                }
            }
            for (const auto& path : tagged_discriminator) {
                py::object tag_obj;
                if (!lookup_discriminator(value, path, &tag_obj)) continue;
                std::string tag = ser_tag_string(tag_obj);
                if (tag.empty()) continue;
                auto it = tagged.find(tag);
                if (it == tagged.end()) continue;
                try {
                    return it->second->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
                } catch (const py::error_already_set&) {
                    PyErr_Clear();
                    // Rust warns for the matched variant and falls through to
                    // inference rather than trying unrelated variants silently.
                    ser_warn_message("Pydantic serialization failed for tagged union variant '" + tag + "'", value);
                }
            }
            // No discriminator value: Rust registers a warning and tries the
            // choices left to right, in declaration order.
            // Rust UnionChoices::serialize: a strict pass first, then a lax
            // pass, so a choice whose class does not match the value cannot
            // "succeed" by emitting the wrong variant's fields.
            ser_warn_message("Defaulting to left to right union serialization - failed to get discriminator value for tagged union serialization", value);
            for (int level = 1; level <= 2; level++) {
                SerCheckScope scope(level);
                for (auto& c : tagged_left_to_right) {
                    try { return c->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context); } catch (...) {}
                }
            }
        }
        if (type == "default" || type == "with-default") {
            // Rust WithDefaultSerializer::to_python forwards the value untouched:
            // a None field with a non-None default serializes as null, not as the
            // default.  Whether the field is emitted at all is decided by the
            // field loop (exclude_unset/exclude_defaults), not here.
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
        }
        if (type == "json") {
            if (round_trip) {
                // Serialize value to JSON string, return as Python string
                std::string json_str;
                if (!children.empty()) {
                    json_str = children[0]->to_json(value, false, -1, round_trip, py::none(), py::none(), false, false, false, exc_none, context);
                } else {
                    json_str = infer_json(value, false, -1, py::none(), py::none());
                }
                return py::cast(json_str);
            }
            // Non-round-trip: delegate to inner serializer
            if (!children.empty()) {
                return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
            }
        }
        if (type == "json-or-python") {
            if (json_mode && !children.empty()) return children[0]->to_python(value, true, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
            if (children.size() > 1) return children[1]->to_python(value, false, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
        }
        if (type == "enum") {
            if (!json_mode) {
                // Python mode: return the Enum member as-is
                return value;
            }
            if (py_hasattr(value, "value")) {
                auto ev = py::getattr(value, "value");
                if (!children.empty()) return children[0]->to_python(ev, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
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
            // Union discrimination (Rust ModelSerializer::allow_value): while a
            // union is trying its choices, reject a value that is not of (or not
            // even an instance of) the expected class so the next choice is tried.
            if (class_.ptr() && !class_.is_none()) {
                bool ok;
                if (g_ser_check == 1) {
                    ok = (static_cast<void*>(value.ptr()->ob_type) == static_cast<void*>(class_.ptr()));
                } else if (g_ser_check == 2) {
                    ok = py::isinstance(value, class_);
                } else {
                    // Rust allow_value_root_model checks isinstance even with no
                    // union active; a non-root model only needs an instance dict.
                    // An unmatched value falls back to inference with a warning
                    // instead of silently emitting an empty dict.
                    // A model reached through this pipeline may still be the
                    // dict representation that _dict_to_model turns into an
                    // instance later, so a dict is accepted too.
                    ok = root_model
                        ? py::isinstance(value, class_)
                        : (py_hasattr(value, "__dict__") || py::isinstance<py::dict>(value));
                }
                if (!ok) {
                    if (g_ser_check != 0) {
                        throw std::runtime_error("Value is not an instance of the expected model class");
                    }
                    std::string model_name = "model";
                    try { model_name = py::getattr(class_, "__name__").cast<std::string>(); }
                    catch (const py::error_already_set&) { PyErr_Clear(); }
                    ser_warn_unexpected_value("", model_name, value);
                    return serialize_any_value(value, exc_none, round_trip, json_mode, include, exclude);
                }
            }
            // For root models, extract the 'root' attribute before delegating
            if (root_model && py_hasattr(value, "root")) {
                auto root_val = py::getattr(value, "root");
                // If the child is a field serializer, pass the model instance
                if (!children.empty() && children[0]->is_field_serializer && children[0]->py_func.ptr() && !children[0]->py_func.is_none()) {
                    auto child = children[0];
                    // Create handler for wrap mode
                    if (child->type == "function-wrap") {
                        py::object handler = py::cpp_function([child, root_val, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context](const py::object& v, py::object index_key) -> py::object {
                            py::object inc = include, exc = exclude;
                            if (!index_key.is_none()) {
                                ser_check_index_key(index_key);
                                auto f = apply_ser_filter(index_key, include, exclude);
                                if (f.omit) throw PydanticOmit();
                                inc = f.include;
                                exc = f.exclude;
                            }
                            if (!child->children.empty()) {
                                return child->children[0]->to_python(v, json_mode, exc_none, round_trip, inc, exc, by_alias, exclude_unset, exclude_defaults, context);
                            }
                            return v;
                        }, py::arg("value"), py::arg("index_key") = py::none());
                        if (child->info_arg) {
                            auto info = make_ser_info(round_trip, "root", context, include, exclude);
                            return child->apply_return_ser(child->py_func(value, root_val, handler, py::cast(info)),
                                json_mode, exc_none, round_trip, py::none(), py::none(), by_alias, exclude_unset, exclude_defaults, context);
                        } else {
                            return child->apply_return_ser(child->py_func(value, root_val, handler),
                                json_mode, exc_none, round_trip, py::none(), py::none(), by_alias, exclude_unset, exclude_defaults, context);
                        }
                    } else {
                        // function-plain
                        if (child->info_arg) {
                            auto info = make_ser_info(round_trip, "root", context, include, exclude);
                            return child->apply_return_ser(child->py_func(value, root_val, py::cast(info)),
                                json_mode, exc_none, round_trip, py::none(), py::none(), by_alias, exclude_unset, exclude_defaults, context);
                        } else {
                            return child->apply_return_ser(child->py_func(value, root_val),
                                json_mode, exc_none, round_trip, py::none(), py::none(), by_alias, exclude_unset, exclude_defaults, context);
                        }
                    }
                }
                // Rust ModelSerializer::serialize_root_model swaps in
                // AnySerializer when serialize_as_any is set: the root value is
                // serialized from its runtime type, so a subclass's extra
                // fields are visible even though the schema declares the base.
                if (g_ser_extra.serialize_as_any) {
                    return SerNode::serialize_any_value(root_val, exc_none, round_trip, json_mode, include, exclude);
                }
                return children[0]->to_python(root_val, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
            }
            return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
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
                if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
                return value;
            }
            if (type == "function-plain") {
                try {
                    if (info_arg) {
                        auto info = make_ser_info(round_trip, "", context, include, exclude);
                        return apply_return_ser(py_func(value, py::cast(info)), json_mode, exc_none, round_trip,
                                                py::none(), py::none(), by_alias, exclude_unset, exclude_defaults, context);
                    }
                    return apply_return_ser(py_func(value), json_mode, exc_none, round_trip,
                                            py::none(), py::none(), by_alias, exclude_unset, exclude_defaults, context);
                } catch (const py::error_already_set& e) {
                    // Rust (function.rs on_error + infer_to_python): a serializer
                    // function that rejects its own value leaves a warning behind
                    // and the value is serialized by inference instead.
                    if (!handle_ser_call_error(e, func_name)) throw;
                    return SerNode::serialize_any_value(value, exc_none, round_trip, json_mode, include, exclude);
                }
            }
            if (type == "function-after" || type == "function-before" || type == "function-wrap") {
                py::object handler = py::cpp_function([this, value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context](const py::object& v, py::object index_key) -> py::object {
                    py::object inc = include, exc = exclude;
                    if (!index_key.is_none()) {
                        ser_check_index_key(index_key);
                        auto f = apply_ser_filter(index_key, include, exclude);
                        if (f.omit) throw PydanticOmit();
                        inc = f.include;
                        exc = f.exclude;
                    }
                    if (!children.empty()) return children[0]->to_python(v, json_mode, exc_none, round_trip, inc, exc, by_alias, exclude_unset, exclude_defaults, context);
                    return v;
                }, py::arg("value"), py::arg("index_key") = py::none());
                if (type == "function-wrap") {
                    py::object wrapped;
                    try {
                        if (info_arg) {
                            auto info = make_ser_info(round_trip, "", context, include, exclude);
                            wrapped = py_func(value, handler, py::cast(info));
                        } else {
                            wrapped = py_func(value, handler);
                        }
                    } catch (const py::error_already_set& e) {
                        // Rust sends every wrap error through on_error, so
                        // anything that is not an "unexpected value" -- including
                        // the PydanticOmit the handler itself may raise -- comes
                        // back as a PydanticSerializationError naming the
                        // function rather than raw.
                        if (!handle_ser_call_error(e, func_name)) throw;
                        return SerNode::serialize_any_value(value, exc_none, round_trip, json_mode, include, exclude);
                    }
                    return apply_return_ser(wrapped, json_mode, exc_none, round_trip, py::none(), py::none(),
                                            by_alias, exclude_unset, exclude_defaults, context);
                }
                try {
                    if (info_arg) {
                        auto info = make_ser_info(round_trip, "", context, include, exclude);
                        return py_func(value, handler, py::cast(info));
                    } else {
                        return py_func(value, handler);
                    }
                } catch (...) {
                    PyErr_Clear();
                    try {
                        if (info_arg) {
                            auto info = make_ser_info(round_trip, "", context, include, exclude);
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
        // A deque is only accepted as-is: anything else warns and serializes
        // by inference (Rust DequeSerializer::to_python). The output keeps the
        // input's `maxlen`, and in JSON mode the items become an array.
        if (type == "deque") {
            if (!py::isinstance(value, py_deque_type())) {
                std::string deque_name =
                    "deque[" + (children.empty() ? std::string("any") : type_name_for_warning(children[0])) + "]";
                ser_warn_unexpected_value("", deque_name, value);
                return serialize_any_value(value, exc_none, round_trip, json_mode, include, exclude);
            }
            std::optional<size_t> maxlen = py_deque_maxlen(value);

            py::iterable seq = py::reinterpret_borrow<py::iterable>(value);
            py::ssize_t len = -1;
            if (!include.is_none() || !exclude.is_none()) {
                try {
                    len = py::len(seq);
                } catch (...) {
                    PyErr_Clear();
                }
            }
            SerIndexFilter filter;
            filter.bind(include, exclude, len);
            py::list items;
            py::ssize_t idx = 0;
            for (auto item : seq) {
                auto next = filter.ask(idx);
                if (!next.omit) {
                    py::object element = py::reinterpret_borrow<py::object>(item);
                    items.append(children.empty()
                        ? element
                        : children[0]->to_python(check_item_type(children[0], element), json_mode, exc_none,
                                                 round_trip, next.include, next.exclude, by_alias,
                                                 exclude_unset, exclude_defaults, context));
                }
                idx++;
            }
            if (json_mode) return std::move(items);
            return py_deque_new(items, maxlen);
        }

        // For list/dict/tuple/containers, serialize children.  A list with no items_schema is
        // built with the any serializer as its item serializer (list.rs:37-41: `None =>
        // AnySerializer::build`), so its elements still go through the walk -- there is simply no
        // child node to ask, and the walk is asked directly below.  The other containers here are
        // answered as they always were when they have no child.
        if (type == "list" ||
            ((type == "set" || type == "frozenset" || type == "generator") && !children.empty())) {
            py::iterable seq = py::reinterpret_borrow<py::iterable>(value);
            // A length is only needed to resolve negative include/exclude keys,
            // and an arbitrary iterable (a custom Iterable, a lazy validator
            // iterator) has none.
            py::ssize_t len = -1;
            if (!include.is_none() || !exclude.is_none()) {
                try {
                    len = py::len(seq);
                } catch (...) {
                    PyErr_Clear();
                }
            }
            SerIndexFilter filter;
            filter.bind(include, exclude, len);
            // A set node has no position to ask about (see below), and set_frozenset.rs hands its
            // items `state` untouched -- no filter of its own, and no key to have folded.  So the
            // items are built with the pair this node was handed exactly as written, folded by
            // whichever node they reach against *that* node's length: a set of three 5-tuples with
            // include={5: True} hands each tuple {5: True}, which the tuple folds to {0: True},
            // where folding here would hand them {2: True} and answer with the third element.
            py::object inc = include;
            py::object exc = exclude;
            // In JSON the items have to become an array here, but Python mode
            // keeps an iterator lazy for the caller to drain.  The view is handed the call's own
            // filter, unsized: generator.rs:68 never takes a length, so it is the view's job to
            // ask about every key as it was written.
            if (type == "generator" && !json_mode && PyIter_Check(value.ptr())) {
                return make_serialization_iterator(value, children[0], exc_none, round_trip, include, exclude,
                                                   by_alias, exclude_unset, exclude_defaults, context);
            }
            py::ssize_t idx = 0;
            // A set node never consults include/exclude: set_frozenset.rs has no filter at all,
            // so its items are serialized with the filter the set itself was handed, untouched
            // (`item_serializer.to_python(&element, state)`).  There is no index to ask about.
            if (type == "set") {
                if (json_mode) {
                    // JSON has no set type: serialize as an array
                    py::list jresult;
                    for (auto item : seq) {
                        jresult.append(children[0]->to_python(check_item_type(children[0], py::reinterpret_borrow<py::object>(item)), json_mode, exc_none, round_trip, inc, exc, by_alias, exclude_unset, exclude_defaults, context));
                    }
                    return std::move(jresult);
                }
                py::set result;
                for (auto item : seq) {
                    result.add(children[0]->to_python(check_item_type(children[0], py::reinterpret_borrow<py::object>(item)), json_mode, exc_none, round_trip, inc, exc, by_alias, exclude_unset, exclude_defaults, context));
                }
                return std::move(result);
            } else if (type == "frozenset") {
                if (json_mode) {
                    // JSON has no frozenset type: serialize as an array
                    py::list jtemp;
                    for (auto item : seq) {
                        jtemp.append(children[0]->to_python(check_item_type(children[0], py::reinterpret_borrow<py::object>(item)), json_mode, exc_none, round_trip, inc, exc, by_alias, exclude_unset, exclude_defaults, context));
                    }
                    return std::move(jtemp);
                }
                py::set temp;
                for (auto item : seq) {
                    temp.add(children[0]->to_python(check_item_type(children[0], py::reinterpret_borrow<py::object>(item)), json_mode, exc_none, round_trip, inc, exc, by_alias, exclude_unset, exclude_defaults, context));
                }
                return py::frozenset(temp);
            } else {
                py::list result;
                for (auto item : seq) {
                    auto next = filter.ask(idx);
                    if (!next.omit) {
                        py::object element = py::reinterpret_borrow<py::object>(item);
                        result.append(children.empty()
                            ? serialize_any_value(element, exc_none, round_trip, json_mode, next.include, next.exclude)
                            : children[0]->to_python(check_item_type(children[0], element), json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context));
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
                // In json mode the key is asked for its text, not its serialized value
                // (dict.rs:90-93), which is why a float key leaves the run as "1.5".
                auto out_k = json_mode
                    ? children[0]->to_json_key(check_item_type(children[0], k), exc_none, round_trip, by_alias, context)
                    : children[0]->to_python(check_item_type(children[0], k), json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context);
                auto out_v = children.size() > 1 ? children[1]->to_python(check_item_type(children[1], v), json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context) : v;
                result[out_k] = out_v;
            }
            return std::move(result);
        }
        // named-tuple: Rust for_each_item_and_serializer pairs every item with the
        // field serializer at the same index; the result keeps its tuple identity
        // in Python mode and becomes an array in JSON mode.
        if (type == "named-tuple") {
            std::string nt_name = class_name.empty() ? std::string("named-tuple") : class_name;
            if (!py::isinstance<py::tuple>(value) && !py::isinstance<py::list>(value)) {
                // Rust tuple_value's cast::<PyTuple> fails: the value goes through
                // inference, as an error while a union checks its choices.
                if (g_ser_check != 0) {
                    throw std::runtime_error("Unexpected value for serializer " + nt_name);
                }
                ser_warn_unexpected_value("", nt_name, value);
                return serialize_any_value(value, exc_none, round_trip, json_mode, include, exclude);
            }
            auto seq = value.cast<py::sequence>();
            size_t n_items = static_cast<size_t>(py::len(seq));
            if (g_ser_check != 0 && n_items != children.size()) {
                throw std::runtime_error("Expected " + std::to_string(children.size()) +
                                         " items, but got " + std::to_string(n_items));
            }
            if (n_items < children.size()) {
                ser_warn_register("PydanticSerializationUnexpectedValue(Unexpected too few items present in named tuple)");
            }
            py::object inc = include.is_none() ? py::none()
                                               : map_negative_indices(include, static_cast<py::ssize_t>(n_items));
            py::object exc = exclude.is_none() ? py::none()
                                               : map_negative_indices(exclude, static_cast<py::ssize_t>(n_items));
            py::list temp;
            size_t i = 0;
            for (auto item : seq) {
                if (i >= children.size()) break;  // Rust drops the extras with a warning
                auto next = apply_ser_filter(py::int_(static_cast<py::ssize_t>(i)), inc, exc);
                if (!next.omit) {
                    py::object v = py::reinterpret_borrow<py::object>(item);
                    temp.append(children[i]->to_python(check_item_type(children[i], v), json_mode, exc_none,
                                                       round_trip, next.include, next.exclude, by_alias,
                                                       exclude_unset, exclude_defaults, context));
                }
                i++;
            }
            if (n_items > children.size()) {
                ser_warn_register("PydanticSerializationUnexpectedValue(Unexpected extra items present in named tuple)");
            }
            // JSON has no tuple type: Rust emits an array.
            if (json_mode) {
                py::list out;
                for (auto item : temp) out.append(item);
                return std::move(out);
            }
            return py::tuple(temp);
        }
        if (type == "tuple" && !children.empty()) {
            auto seq = py::reinterpret_borrow<py::sequence>(value);
            py::ssize_t n_items = py::len(seq);
            // A variadic tuple answers every item with its one serializer, so its length
            // is never a surprise; Rust takes that branch before the count checks
            // (for_each_tuple_item_and_serializer, type_serializers/tuple.rs:198-235).
            bool variadic = tuple_variadic_index >= 0;
            if (!variadic && g_ser_check != 0 && static_cast<size_t>(n_items) != children.size()) {
                throw std::runtime_error("Expected " + std::to_string(children.size()) +
                                         " items, but got " + std::to_string(n_items));
            }
            if (!variadic && static_cast<size_t>(n_items) < children.size()) {
                ser_warn_register("PydanticSerializationUnexpectedValue(Unexpected too few items present in tuple)");
            }
            py::object inc = include.is_none() ? py::none()
                                               : map_negative_indices(include, n_items);
            py::object exc = exclude.is_none() ? py::none()
                                               : map_negative_indices(exclude, n_items);
            py::list temp;
            size_t i = 0;
            bool extra_warned = false;
            for (auto item : seq) {
                if (!variadic && i >= children.size() && !extra_warned) {
                    // Rust warns once for the leftovers, and warns whether or not the
                    // filter keeps any of them.
                    ser_warn_register("PydanticSerializationUnexpectedValue(Unexpected extra items present in tuple)");
                    extra_warned = true;
                }
                auto next = apply_ser_filter(py::int_(static_cast<py::ssize_t>(i)), inc, exc);
                if (!next.omit) {
                    auto v = py::reinterpret_borrow<py::object>(item);
                    const SerRef* child = variadic
                        ? &children[i < static_cast<size_t>(tuple_variadic_index) ? i
                                                                                  : tuple_variadic_index]
                        : (i < children.size() ? &children[i] : nullptr);
                    if (child) {
                        temp.append((*child)->to_python(check_item_type(*child, v), json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context));
                    } else {
                        // Past the declared items the leftover belongs to Any, not to the
                        // last declared serializer: an extra str at a tuple[int] owes no
                        // Expected `int` of its own.
                        temp.append(serialize_any_value(v, exc_none, round_trip, json_mode, next.include, next.exclude));
                    }
                }
                i++;
            }
            // JSON has no tuple type: Rust emits an array.
            if (json_mode) {
                py::list out;
                for (auto item : temp) out.append(item);
                return std::move(out);
            }
            return py::tuple(temp);
        }
        if (type == "any" || type == "call") {
            // Rust builds no dedicated serializer for a call schema, so the
            // value is inferred (a namedtuple thus becomes a plain tuple).
            return serialize_any_value(value, exc_none, round_trip, json_mode, include, exclude);
        }
        // Rust ToStringSerializer is a leaf: str(value), gated by when_used.
        if (type == "to-string") {
            if (!ser_when_used_skips(when_used, json_mode, value)) return py::str(value);
            return value;
        }
        // Rust FormatSerializer: builtin format(value, formatting_string),
        // gated by when_used; falls back to the inner schema when skipped.
        if (type == "format" && !format_str.empty()) {
            if (!ser_when_used_skips(when_used, json_mode, value)) {
                py::object spec = py::str(format_str);
                if (PyObject* r = PyObject_Format(value.ptr(), spec.ptr())) {
                    return py::reinterpret_steal<py::object>(r);
                }
                PyErr_Clear();  // Rust surfaces this as a serialization error; fall through
            }
            if (!children.empty()) return children[0]->to_python(value, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
        }
        // Rust FractionSerializer::to_python: a Fraction is rendered with its
        // display form ("1/3") in Python mode too, anything else warns and is
        // serialized by inference.
        if (type == "fraction") {
            if (!py::isinstance(value, py_fraction_type())) {
                ser_warn_unexpected_value("", type, value);
                return serialize_any_value(value, exc_none, round_trip, json_mode, include, exclude);
            }
            return py::str(value);
        }
        // In json mode, convert leaf values to their JSON-compatible form
        // (bytes→str, decimal/uuid→str, datetime→ISO string, timedelta→ISO
        // duration or float, etc.), mirroring Rust's mode="json" behavior.
        if (json_mode) {
            py::object converted;
            if (json_leaf_convert(type, value, ser_json_bytes, ser_json_timedelta, ser_json_temporal, converted)) {
                return converted;
            }
            // Enum members serialize as their value
            if (type == "enum" && py_hasattr(value, "value")) {
                auto ev = py::getattr(value, "value");
                if (!children.empty()) {
                    return children[0]->to_python(ev, json_mode, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
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
        if (value.is_none()) {
            return "null";
        }
        // Rust's FloatSerializer::serde_serialize (float.rs:128-140) asks extract::<f64> and
        // nothing else, so the float node has to be asked before the mismatch rule below: a Decimal
        // or a Fraction becomes its double here with no warning, in a json run only -- the same node
        // in a jsonable run warns and infers -- while an int too large for a double is the one value
        // the node's own type check accepts and this writer must not, so it warns and infers.
        if (g_ser_check == 0 && (type == "float" || type == "float-constrained")) {
            double fd = 0.0;
            if (!ser_extract_f64(value.ptr(), &fd)) {
                ser_warn_unexpected_value("", type_name_for_warning(), value);
                return infer_json(value, ensure_ascii, indent, include, exclude);
            }
            return ser_json_f64_modes(fd, inf_nan_mode);
        }
        // See the same rule in to_python: refuse the value, warn, write what
        // inference makes of it.  Handing a str to the int writer used to print it
        // unquoted, and a str to the float/bool/bytes writers raised pybind's cast
        // error instead of answering at all.
        if (g_ser_check == 0 && !value_matches_type(value)) {
            ser_warn_unexpected_value("", type_name_for_warning(), value);
            return infer_json(value, ensure_ascii, indent, include, exclude);
        }
        if (!ser_check_accepts(value)) {
            throw std::runtime_error("Unexpected value for serializer " + type);
        }
        if (type == "lax-or-strict") {
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
        }
        if (type == "union") {
            SerAttemptScope attempt;
            // Rust UnionChoices::serialize: one pass at the current level and
            // propagate when an outer union is already checking choices.
            if (g_ser_check != 0) {
                std::exception_ptr last;
                for (auto& c : children) {
                    try { return c->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context); }
                    catch (...) { last = std::current_exception(); }
                }
                if (last) std::rethrow_exception(last);
                throw std::runtime_error("Unexpected value for serializer union");
            }
            // Top level: strict pass, lax pass, then uncheck.
            for (int level = 1; level <= 2; level++) {
                SerCheckScope scope(level);
                for (auto& c : children) {
                    try { return c->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context); } catch (...) {}
                }
            }
            for (auto& c : children) {
                try { return c->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context); } catch (...) {}
            }
        }
        if (type == "dict" && !children.empty()) {
            std::string out = "{";
            bool first = true;
            auto d = value.cast<py::dict>();
            for (auto item : d) {
                auto k = py::reinterpret_borrow<py::object>(item.first);
                auto v = py::reinterpret_borrow<py::object>(item.second);
                auto next = apply_ser_filter(k, include, exclude);
                if (next.omit) continue;
                py::object out_k = children[0]->to_json_key(k, exc_none, round_trip, by_alias, context);
                std::string val_json = children.size() > 1
                    ? children[1]->to_json(check_item_type(children[1], v), ensure_ascii, -1, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context)
                    : infer_json(v, ensure_ascii, -1, next.include, next.exclude);
                if (!first) out += ",";
                first = false;
                std::string key_str = out_k.cast<std::string>();
                out += json_escape(key_str, ensure_ascii) + ":" + val_json;
            }
            out += "}";
            return out;
        }
        if (type == "is-instance" || type == "is-subclass") {
            return infer_json(value, ensure_ascii, indent, include, exclude);
        }
        if (type == "none" || type == "is-none") return "null";
        // Above the python-shape bool arm, which would otherwise answer b'true' here: Rust's int
        // arm extracts an i64, and a bool is one to it.  PyLong gives the digits either way, so the
        // bool case needs no branch of its own.
        if (type == "int" || type == "int-constrained") {
            int overflow = 0;
            long long iv = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
            if (overflow != 0 || (iv == -1 && PyErr_Occurred())) {
                // Rust prints its BigInt arm from the digits; do the same here.
                PyErr_Clear();
                return py::str(value).cast<std::string>();
            }
            return std::to_string(iv);
        }
        // Only a union round reaches this arm now: outside one the float node was answered above.
        // The round has already accepted the value's type, so the cast has nothing left to refuse.
        if (type == "float" || type == "float-constrained") {
            return ser_json_f64_modes(value.cast<double>(), inf_nan_mode);
        }
        if (type == "bool" || py::isinstance<py::bool_>(value)) {
            return value.cast<bool>() ? "true" : "false";
        }
        if (type == "str" || type == "string" || type == "str-constrained") {
            return json_escape(value.cast<std::string>(), ensure_ascii);
        }
        if (type == "bytes") {
            std::string b = value.cast<std::string>();
            if (ser_json_bytes == "base64") return "\"" + b64_encode_string(b) + "\"";
            if (ser_json_bytes == "hex") return "\"" + bytes_hex_encode(b) + "\"";
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
                    inner_json = children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
                } else {
                    inner_json = infer_json(value, ensure_ascii, indent, include, exclude);
                }
                return json_escape(inner_json, ensure_ascii);
            }
            if (!children.empty()) {
                return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
            }
        }
        if (type == "nullable" || type == "nullable-union") {
            if (value.is_none()) return "null";
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
        }
        if (type == "missing-sentinel") {
            // Rust MissingSentinelSerializer::serde_serialize: only the inner
            // serializer produces JSON, the sentinel has no JSON form of its own.
            if (!value.is(missing_sentinel_obj()) && !children.empty()) {
                return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
            }
            throw std::runtime_error("'MISSING' can\'t be serialized to JSON");
        }
        if (type == "ellipsis") {
            // Rust EllipsisSerializer::serde_serialize (ellipsis.rs:63-68): the
            // literal has no JSON spelling to fall back on.
            throw PydanticSerializationError("Error serializing to JSON: 'Ellipsis' can't be serialized to JSON");
        }
        if (type == "default" || type == "with-default") {
            if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
        }
        if (type == "to-string") {
            py::object inner = !children.empty() ? children[0]->to_python(value, false, false, round_trip) : value;
            return json_escape(py::str(inner).cast<std::string>(), ensure_ascii);
        }
        if (type == "enum") {
            if (py_hasattr(value, "value")) {
                auto ev = py::getattr(value, "value");
                if (!children.empty()) return children[0]->to_json(ev, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
                return infer_json(ev, ensure_ascii, indent, include, exclude);
            }
            return infer_json(value, ensure_ascii, indent, include, exclude);
        }
        if (type == "complex" && PyComplex_Check(value.ptr())) {
            return json_escape(complex_to_str_rust(PyComplex_RealAsDouble(value.ptr()),
                                                   PyComplex_ImagAsDouble(value.ptr())),
                                ensure_ascii);
        }
        // Rust FractionSerializer::serde_serialize: collect_str(str(value)).
        if (type == "fraction") {
            if (!py::isinstance(value, py_fraction_type())) {
                ser_warn_unexpected_value("", type, value);
                return infer_json(value, ensure_ascii, indent, include, exclude);
            }
            return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
        }
        // Types that serialize as their str() representation
        if (type == "uuid" || type == "decimal" || type == "ipaddress" ||
            type == "ipv4address" || type == "ipv6address" ||
            type == "ipv4interface" || type == "ipv6interface" ||
            type == "ipv4network" || type == "ipv6network") {
            return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
        }
        // datetime/date/time: call .isoformat() or convert to a float timestamp
        if (type == "datetime" || type == "date" || type == "time") {
            if (ser_json_temporal == "seconds" || ser_json_temporal == "milliseconds") {
                double ts;
                if (temporal_to_float(type, value, ser_json_temporal, ts)) {
                    return py::str(py::repr(py::float_(ts))).cast<std::string>();
                }
            }
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
                if (ser_json_timedelta == "float" || ser_json_timedelta == "milliseconds") {
                    long days = value.attr("days").cast<long>();
                    long seconds = value.attr("seconds").cast<long>();
                    long microseconds = value.attr("microseconds").cast<long>();
                    double total = days * 86400.0 + seconds + microseconds / 1000000.0;
                    if (ser_json_timedelta == "milliseconds") {
                        total = ((long long)(days * 86400 + seconds) * 1000000LL + microseconds) / 1000.0;
                    }
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
        // list: serialize items through the declared item serializer. Without
        // this the node fell through to infer_json, which reads __dict__ and so
        // emitted a subclass's extra fields (and unserialized leaf values) in
        // place of the declared item type.
        if (type == "named-tuple") {
            std::string nt_name = class_name.empty() ? std::string("named-tuple") : class_name;
            if (!py::isinstance<py::tuple>(value) && !py::isinstance<py::list>(value)) {
                if (g_ser_check != 0) {
                    throw std::runtime_error("Unexpected value for serializer " + nt_name);
                }
                ser_warn_unexpected_value("", nt_name, value);
                return infer_json(value, ensure_ascii, indent, include, exclude);
            }
            auto seq = value.cast<py::sequence>();
            size_t n_items = static_cast<size_t>(py::len(seq));
            if (g_ser_check != 0 && n_items != children.size()) {
                throw std::runtime_error("Expected " + std::to_string(children.size()) +
                                         " items, but got " + std::to_string(n_items));
            }
            if (n_items < children.size()) {
                ser_warn_register("PydanticSerializationUnexpectedValue(Unexpected too few items present in named tuple)");
            }
            py::object inc = include.is_none() ? py::none()
                                               : map_negative_indices(include, static_cast<py::ssize_t>(n_items));
            py::object exc = exclude.is_none() ? py::none()
                                               : map_negative_indices(exclude, static_cast<py::ssize_t>(n_items));
            std::string out = "[";
            bool first = true;
            size_t i = 0;
            for (auto item : seq) {
                if (i >= children.size()) break;  // Rust drops the extras with a warning
                auto next = apply_ser_filter(py::int_(static_cast<py::ssize_t>(i)), inc, exc);
                if (!next.omit) {
                    py::object v = py::reinterpret_borrow<py::object>(item);
                    if (!first) out += ",";
                    first = false;
                    out += children[i]->to_json(check_item_type(children[i], v), ensure_ascii, -1, round_trip,
                                                next.include, next.exclude, by_alias, exclude_unset,
                                                exclude_defaults, exc_none, context);
                }
                i++;
            }
            if (n_items > children.size()) {
                ser_warn_register("PydanticSerializationUnexpectedValue(Unexpected extra items present in named tuple)");
            }
            out += "]";
            return out;
        }
        // tuple: every item goes through the serializer the schema declared for its
        // position (the last declared one for the items past that), as in the python
        // walk.  Rust pairs items and serializers in for_each_tuple_item_and_serializer
        // (type_serializers/tuple.rs:168) once, and both output modes walk that pairing, so
        // serializer function has to run here too: tuple[fn(int)] <- (1,) printed [1]
        // through infer_json where the wheel prints the function's answer, and a
        // wrong-typed item owed its warning in a python run and none in a json run.
        // The index filter belongs to the same pairing (tuple.rs's index_filter), so
        // include/exclude reach a json run as they reach a python one.
        if (type == "tuple" && !children.empty()) {
            std::string tn = type_name_for_warning();
            if (!py::isinstance<py::tuple>(value)) {
                // Rust's cast::<PyTuple> fails: while a union tries its choices the
                // choice is refused; otherwise the node warns and inference answers.
                if (g_ser_check != 0) {
                    throw std::runtime_error("Unexpected value for serializer " + tn);
                }
                ser_warn_unexpected_value("", tn, value);
                return infer_json(value, ensure_ascii, indent, include, exclude);
            }
            auto seq = py::reinterpret_borrow<py::sequence>(value);
            py::ssize_t n_items = py::len(seq);
            py::object inc = include.is_none() ? py::none()
                                               : map_negative_indices(include, n_items);
            py::object exc = exclude.is_none() ? py::none()
                                               : map_negative_indices(exclude, n_items);
            bool variadic = tuple_variadic_index >= 0;
            if (!variadic && g_ser_check != 0 && static_cast<size_t>(n_items) != children.size()) {
                throw std::runtime_error("Expected " + std::to_string(children.size()) +
                                         " items, but got " + std::to_string(n_items));
            }
            if (!variadic && static_cast<size_t>(n_items) < children.size()) {
                ser_warn_register("PydanticSerializationUnexpectedValue(Unexpected too few items present in tuple)");
            }
            std::string out = "[";
            bool first = true;
            bool extra_warned = false;
            size_t i = 0;
            for (auto item : seq) {
                if (!variadic && i >= children.size() && !extra_warned) {
                    ser_warn_register("PydanticSerializationUnexpectedValue(Unexpected extra items present in tuple)");
                    extra_warned = true;
                }
                auto next = apply_ser_filter(py::int_(static_cast<py::ssize_t>(i)), inc, exc);
                if (!next.omit) {
                    py::object v = py::reinterpret_borrow<py::object>(item);
                    const SerRef* child = variadic
                        ? &children[i < static_cast<size_t>(tuple_variadic_index) ? i
                                                                                  : tuple_variadic_index]
                        : (i < children.size() ? &children[i] : nullptr);
                    if (!first) out += ",";
                    first = false;
                    out += child
                               ? (*child)->to_json(check_item_type(*child, v), ensure_ascii, -1,
                                                   round_trip, next.include, next.exclude, by_alias,
                                                   exclude_unset, exclude_defaults, exc_none, context)
                               : infer_json(v, ensure_ascii, -1, next.include, next.exclude);
                }
                i++;
            }
            out += "]";
            return out;
        }
        // list/deque/generator: serialize as a JSON array, asking the call's filter about every
        // position -- list.rs:64, deque.rs:75 and generator.rs:68 all call index_filter.  A list
        // and a deque have a length to fold an index key by; a generator's length is never taken
        // (`None` at generator.rs:68), which in Rust means "use the key exactly as written, and
        // refuse a negative one", not "skip the filter".
        if ((type == "list" && !children.empty()) || type == "deque" || type == "generator") {
            py::iterable seq = py::reinterpret_borrow<py::iterable>(value);
            py::ssize_t len = -1;
            if (type != "generator" && (!include.is_none() || !exclude.is_none())) {
                try {
                    len = py::len(seq);
                } catch (...) {
                    PyErr_Clear();
                }
            }
            SerIndexFilter filter;
            filter.bind(include, exclude, len);
            std::string out = "[";
            bool first = true;
            py::ssize_t idx = 0;
            for (auto item : seq) {
                auto next = filter.ask(idx);
                if (!next.omit) {
                    if (!first) out += ",";
                    first = false;
                    py::object obj = py::reinterpret_borrow<py::object>(item);
                    if (!children.empty()) {
                        // state.scoped_include_exclude (list.rs:104) swaps only the filter pair on
                        // the way down: exclude_unset and exclude_defaults travel with the item.
                        out += children[0]->to_json(check_item_type(children[0], obj), ensure_ascii, -1, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
                    } else {
                        out += infer_json(obj, ensure_ascii, -1, next.include, next.exclude);
                    }
                }
                idx++;
            }
            out += "]";
            return out;
        }
        // A set has no position to ask about, so it never consults the filter and hands its items
        // the state it was given (set_frozenset.rs).  JSON has no set type: it becomes an array.
        if (type == "set" || type == "frozenset") {
            std::string out = "[";
            bool first = true;
            for (auto item : py::reinterpret_borrow<py::iterable>(value)) {
                if (!first) out += ",";
                first = false;
                py::object obj = py::reinterpret_borrow<py::object>(item);
                if (!children.empty()) {
                    out += children[0]->to_json(check_item_type(children[0], obj), ensure_ascii, -1, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
                } else {
                    out += infer_json(obj, ensure_ascii, -1, include, exclude);
                }
            }
            out += "]";
            return out;
        }
        if (!fields.empty() || type == "model-fields" || type == "typed-dict" || type == "dataclass-args") {
            // A model/dataclass/typed-dict always serializes to a dict, even
            // when it declares no fields (empty model -> {}).
            return serialize_fields_json(value, ensure_ascii, indent, exc_none, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, context);
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
            // Union discrimination (Rust ModelSerializer::allow_value): while a
            // union is trying its choices, reject a value that is not of (or not
            // even an instance of) the expected class so the next choice is tried.
            if (class_.ptr() && !class_.is_none()) {
                bool ok;
                if (g_ser_check == 1) {
                    ok = (static_cast<void*>(value.ptr()->ob_type) == static_cast<void*>(class_.ptr()));
                } else if (g_ser_check == 2) {
                    ok = py::isinstance(value, class_);
                } else {
                    // A model reached through this pipeline may still be the
                    // dict representation that _dict_to_model turns into an
                    // instance later, so a dict is accepted too.
                    ok = root_model
                        ? py::isinstance(value, class_)
                        : (py_hasattr(value, "__dict__") || py::isinstance<py::dict>(value));
                }
                if (!ok) {
                    if (g_ser_check != 0) {
                        throw std::runtime_error("Value is not an instance of the expected model class");
                    }
                    std::string model_name = "model";
                    try { model_name = py::getattr(class_, "__name__").cast<std::string>(); }
                    catch (const py::error_already_set&) { PyErr_Clear(); }
                    ser_warn_unexpected_value("", model_name, value);
                    return infer_json(value, ensure_ascii, indent, include, exclude);
                }
            }
            // For root models, extract the 'root' attribute before delegating
            if (root_model && py_hasattr(value, "root")) {
                auto root_val = py::getattr(value, "root");
                // If the child is a field serializer, pass the model instance
                if (!children.empty() && children[0]->is_field_serializer && children[0]->py_func.ptr() && !children[0]->py_func.is_none()) {
                    auto child = children[0];
                    // For field serializers, call the function directly
                    if (child->type == "function-wrap") {
                        // Create handler that serializes to JSON
                        py::object handler = py::cpp_function([child, root_val, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context](const py::object& v, py::object index_key) -> py::object {
                            py::object inc = include, exc = exclude;
                            if (!index_key.is_none()) {
                                ser_check_index_key(index_key);
                                auto f = apply_ser_filter(index_key, include, exclude);
                                if (f.omit) throw PydanticOmit();
                                inc = f.include;
                                exc = f.exclude;
                            }
                            if (!child->children.empty()) {
                                return py::str(child->children[0]->to_json(v, ensure_ascii, indent, round_trip, inc, exc, by_alias, exclude_unset, exclude_defaults, exc_none, context));
                            }
                            return py::str(v);
                        }, py::arg("value"), py::arg("index_key") = py::none());
                        if (child->info_arg) {
                            auto info = make_ser_info(round_trip, "root", context, include, exclude);
                            return py::str(child->py_func(value, root_val, handler, py::cast(info)));
                        } else {
                            return py::str(child->py_func(value, root_val, handler));
                        }
                    } else {
                        // function-plain
                        py::object result;
                        if (child->info_arg) {
                            auto info = make_ser_info(round_trip, "root", context, include, exclude);
                            result = child->py_func(value, root_val, py::cast(info));
                        } else {
                            result = child->py_func(value, root_val);
                        }
                        return infer_json(result, ensure_ascii, indent, py::none(), py::none());
                    }
                }
                // Rust ModelSerializer::serialize_root_model swaps in
                // AnySerializer when serialize_as_any is set (see to_python).
                if (g_ser_extra.serialize_as_any) {
                    return infer_json(root_val, ensure_ascii, indent, include, exclude);
                }
                return children[0]->to_json(root_val, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
            }
            return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
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
                if (!children.empty()) return children[0]->to_json(value, ensure_ascii, indent, round_trip, include, exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
                return infer_json(value, ensure_ascii, indent, include, exclude);
            }
            py::object result;
            if (type == "function-plain") {
                try {
                    if (info_arg) {
                        auto info = make_ser_info(round_trip, "", context, include, exclude);
                        result = py_func(value, py::cast(info));
                    } else {
                        result = py_func(value);
                    }
                } catch (const py::error_already_set& e) {
                    if (!handle_ser_call_error(e, func_name)) throw;
                    return infer_json(value, ensure_ascii, indent, include, exclude);
                }
            } else {
                result = to_python(value, true, exc_none, round_trip, include, exclude, by_alias,
                                   exclude_unset, exclude_defaults, context);
            }
            result = apply_return_ser(result, true, exc_none, round_trip, py::none(), py::none(), by_alias,
                                      exclude_unset, exclude_defaults, context);
            return infer_json(result, ensure_ascii, indent, py::none(), py::none());
        }
        if (type == "format" && !format_str.empty() && !ser_when_used_skips(when_used, true, value)) {
            py::object spec = py::str(format_str);
            if (PyObject* r = PyObject_Format(value.ptr(), spec.ptr())) {
                py::object formatted = py::reinterpret_steal<py::object>(r);
                return infer_json(formatted, ensure_ascii, indent, include, exclude);
            }
            PyErr_Clear();
        }
        return infer_json(value, ensure_ascii, indent, include, exclude);
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

  public:
    // Thread-local stack of object addresses currently being serialized,
    // mirroring Rust's RecursionGuard: re-entering an ancestor object means
    // a reference cycle (raise), excessive depth is a safety net (raise).
    static std::vector<const void*>& json_rec_stack() {
        static thread_local std::vector<const void*> stack;
        return stack;
    }

    // A map key is asked what it is written as, and Rust's key path carries no RecursionGuard
    // at all: neither a repeat nor a depth is refused there, and `to_json` over a 20000-deep
    // nested tuple key answers `{{"leaf":1}}` right up to the depth where the process runs out
    // of C stack and dies.  This bound is therefore not a rule copied from Rust but the one
    // thing that keeps a deep key from taking the interpreter down with it -- the bound is
    // SER_INFER_KEY_DEPTH_LIMIT, measured rather than derived.
    static int& json_key_depth() {
        static thread_local int depth = 0;
        return depth;
    }

    static bool is_enum_instance(const py::object& v) {
        static const py::object& enum_cls = held_python_object([] { return py::module_::import("enum").attr("Enum"); });
        return py::isinstance(v, enum_cls);
    }
    // The key-side list: a map key is written as text whatever its value form is, and
    // these kinds have no text but their own (infer.rs:554, :576-593).  Values go to
    // ser_infer_json_str, which answers the same kinds with their own serializers.
    static const std::vector<py::object>& str_known_classes() {
        static std::vector<py::object> classes;
        if (!classes.empty()) return classes;
        try {
            classes.push_back(py::module_::import("decimal").attr("Decimal"));
            classes.push_back(py::module_::import("uuid").attr("UUID"));
            classes.push_back(py::module_::import("pathlib").attr("Path"));
            py::object ip = py::module_::import("ipaddress");
            for (const char* name : {"IPv4Address", "IPv6Address", "IPv4Network", "IPv6Network",
                                     "IPv4Interface", "IPv6Interface"}) {
                classes.push_back(ip.attr(name));
            }
        } catch (...) {
            PyErr_Clear();
            classes.clear();
        }
        return classes;
    }

    // A leaf converter yields text, except for the temporal modes that ask for
    // a number of (milli)seconds.
    static std::string json_escape_converted(const py::object& converted, bool ensure_ascii) {
        if (py::isinstance<py::float_>(converted)) return py::str(py::repr(converted)).cast<std::string>();
        return json_escape(converted.cast<std::string>(), ensure_ascii);
    }

    // Rust's infer_serialize classifies a fixed set of object kinds and has no
    // generic attribute walk: a plain object is ObType::Unknown and JSON
    // serialization fails with "Unable to serialize unknown type". Only the
    // structured kinds Rust recognises keep the attribute walk here.
    static bool dict_inferable_object(const py::object& value) {
        if (py_hasattr(value, "__pydantic_fields__") || py_hasattr(value, "__dataclass_fields__")) return true;
        try {
            py::object dc = py::module_::import("dataclasses");
            if (py::cast<bool>(dc.attr("is_dataclass")(value))) return true;
        } catch (...) { PyErr_Clear(); }
        return false;
    }

    // The ObType::Unknown arm of a key (infer.rs:630-639).  The run's `fallback` is asked
    // first and what it hands back is asked the same question again, because a fallback may
    // return another unknown; `serialize_unknown` prints the value instead; with neither the
    // failure names the key's type, since the key is by definition unprintable here.
    static py::object unknown_json_key(const py::object& key, bool json_text) {
        if (g_ser_extra.fallback.ptr() && !g_ser_extra.fallback.is_none())
            return infer_json_key(g_ser_extra.fallback(key), json_text);
        if (g_ser_extra.serialize_unknown) return py::str(ser_serialize_unknown(key));
        // A key is always inside a map, so in JSON text mode the serde boundary names this
        // run's failure before the message reaches the caller -- one wrap however deep the
        // key sits, because the boundary is the entry point and not each collection.  The
        // jsonable walk has no such boundary and reports the error as it stands.
        std::string msg = "Unable to serialize unknown type: " + ser_safe_repr(py::type::of(key));
        if (json_text) throw PydanticSerializationError("Error serializing to JSON: PydanticSerializationError: " + msg);
        throw PydanticSerializationError(msg);
    }

    // Rust infer_json_key (infer.rs:530-641) asks a dict key what kind of thing it is rather
    // than printing it.  Both walks ask it: the JSON text through the serde boundary, and the
    // jsonable walk because it is to_python with SerMode::Json too (:275), where a map entry's
    // key is turned with json_key (shared.rs:737-740) -- which is why a key leaves
    // to_jsonable_python a str and `{1: "x"}` and `{'1': 'x'}` are one and the same run.
    // Printing every key instead cost more than the shape of the output: an unknown key was
    // rendered where Rust refuses it, the run's fallback was never asked about a key at all, a
    // bool was named in Python's rather than JSON's words, and a collection was printed instead
    // of refused.
    static py::object infer_json_key(const py::object& key, bool json_text) {
        // Rust bounds neither the fallback nor a self-referential key here, and the reference
        // build dies at the bottom of that stack -- to_json({FH(): 1}, fallback=lambda v: FH())
        // takes the wheel down with it.  The key walk's own bound is what this leans on, reported
        // with the error the value walk already uses, so the run ends before the stack does.  A
        // count rather than a set of open ids answers for a key because a key is hashed, and a
        // container that could name itself never reaches here as one.
        int& kd = json_key_depth();
        if (kd >= SER_INFER_KEY_DEPTH_LIMIT) {
            if (json_text)
                throw PydanticSerializationError("Error serializing to JSON: ValueError: Circular reference detected (depth exceeded)");
            throw py::value_error("Circular reference detected (depth exceeded)");
        }
        ++kd;
        struct KeyDepthPop {
            int& d;
            ~KeyDepthPop() { --d; }
        } popper{kd};
        // ObType::Enum is asked before the mixin types a member also satisfies, and asks its
        // value the same question again (infer.rs:616-619).
        if (is_enum_instance(key)) return infer_json_key(py::getattr(key, "value"), json_text);
        if (key.is_none()) return py::str("None");
        // ObType::Bool comes before Int, whose Python spelling would be "True".
        if (py::isinstance<py::bool_>(key)) return py::str(key.ptr() == Py_True ? "true" : "false");
        if (py::isinstance<py::int_>(key)) return py::str(key);
        // A float key is str(key) -- Python's "nan"/"inf", not Rust's "NaN" -- except under
        // inf_nan_mode="null", where it takes the same "None" the None arm writes
        // (infer.rs:546-553).  "strings" is not here: it changes what a *value* is written
        // as, and a key is already written as a string.
        if (py::isinstance<py::float_>(key)) {
            double kd = key.cast<double>();
            if ((std::isnan(kd) || std::isinf(kd)) && g_ser_extra.inf_nan_mode == "null")
                return py::str("None");
            return py::str(key);
        }
        if (py::isinstance<py::str>(key)) return key;
        if (py::isinstance<py::bytes>(key) || py::isinstance<py::bytearray>(key)) {
            // A key follows the run's bytes mode like a value does, and its utf8 failure is
            // reported through bytes_to_string, so it arrives as a UnicodeDecodeError rather
            // than the unknown-type wording -- str() of the key turned b'ab' into "b'ab'".
            std::string kb;
            if (py::isinstance<py::bytes>(key)) {
                kb = key.cast<std::string>();
            } else {
                PyObject* b = PyBytes_FromObject(key.ptr());
                if (!b) { PyErr_Clear(); return unknown_json_key(key, json_text); }
                kb = py::reinterpret_steal<py::bytes>(b).cast<std::string>();
            }
            if (g_ser_extra.bytes_mode == "hex") kb = bytes_hex_encode(kb);
            else if (g_ser_extra.bytes_mode == "base64") kb = b64_encode_string(kb);
            else if (auto bad = utf8_bad(kb)) raise_rust_decode_error(kb, *bad);
            return py::str(kb);
        }
        {
            // The temporal kinds and a complex number each have a form of their own, and the
            // run's temporal_mode picks which of them a datetime key gets -- the
            // space-separated str(datetime) is not one of them.
            py::object conv;
            if (json_infer_leaf(key, conv)) {
                // A temporal key under seconds/milliseconds is Rust's own Display of the
                // number (config.rs:205-247), which writes an integral one as
                // "1704164645" -- Python's str() of the same float adds ".0".
                if (py::isinstance<py::float_>(conv))
                    return py::str(rust_f64_display(conv.cast<double>()));
                return py::str(conv);
            }
        }
        // ObType::Tuple asks every element and joins the answers with "," (tuple.rs:248);
        // Python's own repr of the tuple would read "(1, 'a')".
        if (py::isinstance<py::tuple>(key)) {
            std::string joined;
            bool first = true;
            for (auto item : py::reinterpret_borrow<py::tuple>(key)) {
                if (!first) joined += ",";
                first = false;
                joined += infer_json_key(py::reinterpret_borrow<py::object>(item), json_text).cast<std::string>();
            }
            return py::str(joined);
        }
        {
            // A collection is refused outright (infer.rs:601-608), and the name in the message
            // is Rust's snake_case ObType.  An unhashable one never reaches here -- Python
            // keeps it out of a dict to begin with -- so this answers a hashable subclass.
            const char* refused = nullptr;
            if (py::isinstance<py::list>(key)) refused = "list";
            else if (py::isinstance<py::set>(key)) refused = "set";
            else if (py::isinstance<py::frozenset>(key)) refused = "frozenset";
            else if (py::isinstance<py::dict>(key)) refused = "dict";
            else if (PyIter_Check(key.ptr())) refused = "generator";
            else {
                try {
                    static const py::object& deque_cls = held_python_object([] { return py::module_::import("collections").attr("deque"); });
                    if (py::isinstance(key, deque_cls)) refused = "deque";
                } catch (const py::error_already_set&) { PyErr_Clear(); }
            }
            if (refused) {
                std::string msg = std::string("`") + refused + "` not valid as object key";
                if (json_text) throw PydanticSerializationError("Error serializing to JSON: TypeError: " + msg);
                throw py::type_error(msg);
            }
        }
        // ObType::Pattern is its pattern source, because str() of a compiled pattern is its
        // repr on this Python (infer.rs:624-629).  Only the import is guarded: asking the source
        // for its text can refuse (infer.rs:627 propagates that), and swallowing it here would
        // leave the pattern to be refused as an unknown type instead.
        bool is_pattern = false;
        try {
            static const py::object& pattern_cls = held_python_object([] { return py::module_::import("re").attr("Pattern"); });
            is_pattern = py::isinstance(key, pattern_cls);
        } catch (const py::error_already_set&) { PyErr_Clear(); }
        if (is_pattern) return ser_text_form(py::getattr(key, "pattern"));
        // A dataclass or a pydantic model is named by str() (infer.rs:610-615), which Rust
        // precedes by checking the key is hashable -- which it already is, having come out of a
        // dict.  A type object answers the same probes and is refused (ob_type.rs:421).
        if ((dict_inferable_object(key) || (py_hasattr(key, "__pydantic_serializer__") && !PyType_Check(key.ptr())))
            && !PyType_Check(key.ptr()))
            return ser_text_form(key);
        // Decimal, UUID, Path and the ipaddress types have no form but their own text
        // (infer.rs:554, :576-593), so does a Fraction, and so does a URL.
        for (const py::object& known : str_known_classes()) {
            if (py::isinstance(key, known)) return py::str(key);
        }
        try {
            static const py::object& fraction_cls = held_python_object([] { return py::module_::import("fractions").attr("Fraction"); });
            if (py::isinstance(key, fraction_cls)) return py::str(key);
        } catch (const py::error_already_set&) { PyErr_Clear(); }
        try {
            static const py::object& url_mod_holder = held_python_object([] { return py::module_::import("pydantic_core_cpp._pydantic_core_cpp"); });
            py::object url_cls = url_mod_holder.attr("Url");
            py::object murl_cls = url_mod_holder.attr("MultiHostUrl");
            if (py::isinstance(key, url_cls) || py::isinstance(key, murl_cls)) return py::str(key);
            if (py_hasattr(key, "_url")) {
                auto inner = py::getattr(key, "_url");
                if (py::isinstance(inner, url_cls) || py::isinstance(inner, murl_cls)) return py::str(key);
            }
        } catch (const py::error_already_set&) { PyErr_Clear(); }
        return unknown_json_key(key, json_text);
    }

    // Rust TypeSerializer::json_key (shared.rs:416-435): in a json-mode run a dict key is
    // asked for the *text* it is written as, not for its serialized value, and it is asked
    // with the filters emptied (dict.rs:87-94).  A typed key answers with the form this node
    // would print for a value of its own type -- from the config it was built with, the same
    // one its values get -- and a key that is not that type is handed to the infer walk,
    // whose mismatch the caller already reported.  A wrapper forwards, a tuple joins its
    // items' keys with ",", and a collection rules out only None by name: every other value
    // is inferred and refused there, in the infer walk's own words.
    py::object to_json_key(const py::object& key, bool exc_none, bool round_trip, bool by_alias,
                           const py::object& context) const {
        const std::string& t = type;
        // Inside a JSON-text run the infer walk reports through the serde boundary, exactly
        // as it does when the same key is asked for its JSON text directly.
        const bool json_text = g_ser_json_depth > 0;
        auto inferred = [&] { return infer_json_key(key, json_text); };
        // A wrapper has no key form of its own, so the inner serializer is asked
        // (with_default.rs:46-50, definitions.rs:87-91, json_or_python.rs:52-58).
        if (t == "with-default" || t == "default" || t == "lax-or-strict" || t == "definitions" ||
            t == "definition-ref" || t == "json-or-python") {
            if (children.empty()) return inferred();
            return children[0]->to_json_key(key, exc_none, round_trip, by_alias, context);
        }
        // A nullable key that is None keeps the "None" the infer walk writes for it;
        // anything else is the inner type's question again (nullable.rs:47-53).
        if (t == "nullable" || t == "nullable-union") {
            if (key.is_none()) return py::str("None");
            if (children.empty()) return inferred();
            return children[0]->to_json_key(key, exc_none, round_trip, by_alias, context);
        }
        // A union asks each choice until one answers, and infers when none of them will
        // (union.rs:78-84, whose choices.serialize at :358-400 turns every mismatch into a
        // warning before letting the key move on).
        if (t == "union") {
            for (const SerRef& c : children) {
                if (!c) continue;
                try {
                    return c->to_json_key(key, exc_none, round_trip, by_alias, context);
                } catch (...) {}
            }
            return inferred();
        }
        // A key the node's own type will not take is refused before it is asked for text:
        // the refusal is what gets reported and the text is the infer walk's, exactly as
        // warn_fallback_py then infer_json_key does (string.rs:58-70, simple.rs:140-152) and
        // as the value walks do at their top.  While a union checks its choices the refusal
        // stays an error instead, so that the next choice is tried.
        if (!value_matches_type(key)) {
            if (g_ser_check != 0)
                throw std::runtime_error("Unexpected value for serializer " + type_name_for_warning());
            ser_warn_unexpected_value("", type_name_for_warning(), key);
            return inferred();
        }
        PyObject* p = key.ptr();
        // A str key is the text already (string.rs:58-70).
        if (t == "str" || t == "string" || t == "str-constrained")
            return PyUnicode_Check(p) ? key : inferred();
        // An int key is str(int), and a bool is one of those: the type lookup calls a bool a
        // subclass of int, so it is spelled "True" here rather than the "true" a bool node
        // writes for itself (simple.rs:145-151, :180-184, and :186-192 for a bool's own).
        if (t == "int" || t == "int-constrained")
            return PyLong_Check(p) ? py::str(key) : inferred();
        if (t == "bool")
            return PyBool_Check(p) ? py::str(p == Py_True ? "true" : "false") : inferred();
        // A float key is str(float) -- "nan" and "inf" in Python's spelling, not the run's
        // inf_nan form, because a key is text either way and that mode has nothing left to
        // rewrite (float.rs:110-122).  An int answers str(int) as a subclass.
        if (t == "float" || t == "float-constrained")
            return (PyFloat_Check(p) || PyLong_Check(p)) ? py::str(key) : inferred();
        if (t == "none")
            return key.is_none() ? py::str("None") : inferred();
        // An enum key is asked again by its value, through the enum's own value serializer
        // (enum_.rs:74-88).
        if (t == "enum") {
            if (class_ && py::isinstance(key, class_)) {
                try {
                    py::object v = py::getattr(key, "value");
                    if (children.empty()) return infer_json_key(v, json_text);
                    return children[0]->to_json_key(v, exc_none, round_trip, by_alias, context);
                } catch (const py::error_already_set&) { PyErr_Clear(); }
            }
            return inferred();
        }
        // A tuple key is its items' keys joined with "," -- Python's own repr of the tuple
        // would read "(1, 'a')" -- and a named tuple does the same with its field
        // serializers (tuple.rs:93-117, named_tuple.rs:99-118).  Past the last declared
        // position the variadic serializer answers, as the value walk does.
        if (t == "tuple" || t == "named-tuple") {
            if (!PyTuple_Check(p) || children.empty()) return inferred();
            std::string joined;
            size_t i = 0;
            for (auto item : py::reinterpret_borrow<py::sequence>(key)) {
                py::object v = py::reinterpret_borrow<py::object>(item);
                const SerRef& c = i < children.size() ? children[i] : children.back();
                std::string part = (!children.empty() && c)
                    ? c->to_json_key(check_item_type(c, v), exc_none, round_trip, by_alias, context).cast<std::string>()
                    : infer_json_key(v, json_text).cast<std::string>();
                if (i) joined += ",";
                joined += part;
                i++;
            }
            return py::str(joined);
        }
        // The typed leaves print a key in the form their values get.  A temporal key under
        // seconds or milliseconds is Rust's own Display of the number, which writes an
        // integral one without ".0".
        py::object converted;
        if (json_leaf_convert(t, key, ser_json_bytes, ser_json_timedelta, ser_json_temporal, converted)) {
            if (py::isinstance<py::float_>(converted))
                return py::str(rust_f64_display(converted.cast<double>()));
            return converted;
        }
        // A to-string or format key is formatted like a value, and when its when_used skips
        // this run it is written as "None" rather than passed to the inner schema
        // (format.rs:123-133, :193-203).
        if (t == "to-string" || (t == "format" && !format_str.empty())) {
            if (!ser_when_used_skips(when_used, true, key)) {
                if (t == "to-string") return py::str(key);
                if (PyObject* r = PyObject_Format(p, py::str(format_str).ptr()))
                    return py::reinterpret_steal<py::object>(r);
                PyErr_Clear();
            }
            return py::str("None");
        }
        // A collection cannot be a key at all, and None is the only value the type itself
        // rules out (shared.rs:422-435); anything else is inferred and refused there.
        if (key.is_none() && (t == "list" || t == "set" || t == "frozenset" || t == "dict" ||
                              t == "typed-dict" || t == "generator" || t == "deque" ||
                              t == "complex" || t == "ellipsis")) {
            std::string msg = "`" + t + "` not valid as object key";
            if (json_text) throw PydanticSerializationError("Error serializing to JSON: TypeError: " + msg);
            throw py::type_error(msg);
        }
        return inferred();
    }

    static std::string infer_json(const py::object& value, bool ensure_ascii, int indent,
                                  const py::object& include = py::none(),
                                  const py::object& exclude = py::none()) {
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
        return infer_json_body(value, ensure_ascii, indent, include, exclude);
    }

    static std::string infer_json_body(const py::object& value, bool ensure_ascii, int indent,
                                       const py::object& include, const py::object& exclude) {
        if (value.is_none()) return "null";
        // The dataclass duck test heads the walk rather than sit among the arms, because Rust
        // matches the value's own type pointer first and only reaches `is_dataclass` once that
        // table has named nothing (ob_type.rs:219-305).  A dataclass built over a base type is
        // therefore asked its fields rather than answered by what it inherits: `@dataclass class
        // D(int)` and `@dataclass class D(set)` are both `b'{"a":1}'` there, not the number nor
        // the sequence.  Only `is_pydantic_serializable` (ob_type.rs:287) is asked earlier, one
        // test ahead, so a value that carries a serializer of its own is left to the arm below --
        // which is what keeps a pydantic dataclass's aliases and fields intact.
        if (!PyType_Check(value.ptr()) && py_hasattr(value, "__dataclass_fields__")
            && !py_hasattr(value, "__pydantic_serializer__"))
            return infer_json(ser_dataclass_pairs(value), ensure_ascii, indent, include, exclude);
        // Rust infer_serialize ObType::Enum: serialize the member's value. This
        // must precede the bool/int/str leaves, because a mixin member (IntEnum,
        // str Enum) also satisfies those checks, and the __dict__ fallback at the
        // bottom, because a member's __dict__ holds _value_/_name_.
        if (is_enum_instance(value)) {
            return infer_json(py::getattr(value, "value"), ensure_ascii, indent, include, exclude);
        }
        if (py::isinstance<py::bool_>(value)) return value.cast<bool>() ? "true" : "false";
        if (py::isinstance<py::int_>(value)) return py::str(py::repr(value)).cast<std::string>();
        if (py::isinstance<py::float_>(value)) {
            // serialize_f64 (float.rs:59-75): only the modes that name a non-finite float
            // differently change anything, and a finite float answers for itself either way.
            double d = value.cast<double>();
            if (std::isnan(d) || std::isinf(d)) {
                if (g_ser_extra.inf_nan_mode == "null") return "null";
                if (g_ser_extra.inf_nan_mode == "strings") {
                    if (std::isnan(d)) return "\"NaN\"";
                    return d > 0 ? "\"Infinity\"" : "\"-Infinity\"";
                }
            }
            return ser_json_f64(d);
        }
        if (py::isinstance<py::str>(value)) return json_escape(value.cast<std::string>(), ensure_ascii);
        if (PyComplex_Check(value.ptr())) {
            return json_escape(complex_to_str_rust(PyComplex_RealAsDouble(value.ptr()),
                                                   PyComplex_ImagAsDouble(value.ptr())),
                                ensure_ascii);
        }
        if (py::isinstance<py::bytes>(value)) {
            std::string b = value.cast<std::string>();
            if (g_ser_extra.bytes_mode == "hex") return json_escape(bytes_hex_encode(b), ensure_ascii);
            if (g_ser_extra.bytes_mode == "base64") return json_escape(b64_encode_string(b), ensure_ascii);
            // utf8 is a strict decode, not a best effort: bytes_to_string fails rather
            // than let a byte string through that no reader could decode back.
            if (auto bad = utf8_bad(b))
                throw PydanticSerializationError("Error serializing to JSON: " + rust_utf8_reason(*bad));
            return json_escape(b, ensure_ascii);
        }
        // Rust infer_serialize_known classifies every ObType it recognises, so a
        // datetime or UUID reaches its own serializer; without these branches an
        // Any-typed value fell through to repr() and JSON got
        // "datetime.datetime(2024, 1, 1)" instead of "2024-01-01T00:00:00".
        if (py::isinstance<py::bytearray>(value)) {
            // ObType::Bytearray goes through the same bytes mode as bytes.
            if (PyObject* b = PyBytes_FromObject(value.ptr())) {
                py::object conv;
                bool ok = json_infer_leaf(py::reinterpret_steal<py::object>(b), conv);
                if (ok) return json_escape_converted(conv, ensure_ascii);
            } else {
                PyErr_Clear();
            }
        }
        {
            py::object conv;
            if (json_infer_leaf(value, conv)) return json_escape_converted(conv, ensure_ascii);
        }
        if (py::isinstance<py::set>(value) || py::isinstance<py::frozenset>(value) ||
            py::isinstance(value, py_deque_type()) ||  // ObType::Deque, a sequence unlike the set arms
            PyIter_Check(value.ptr())) {  // ObType::Set/FrozenSet/Generator
            // Two answers, not one: serialize_seq! (infer.rs:340-357) hands a set's or frozenset's
            // items an empty pair -- a set has no position to ask about -- while a deque is
            // measured and filtered by position and an iterator is filtered with no length at all
            // (:482-490), which is where a negative key is refused instead of folded.
            std::string out = "[";
            bool first = true;
            bool no_position = py::isinstance<py::set>(value) || py::isinstance<py::frozenset>(value);
            SerIndexFilter filter;
            if (!no_position) {
                py::ssize_t dlen = -1;
                if (!PyIter_Check(value.ptr())) {
                    try { dlen = py::len(value); } catch (...) { PyErr_Clear(); }
                }
                filter.bind(include, exclude, dlen);
            }
            py::ssize_t idx = 0;
            for (auto item : py::reinterpret_borrow<py::iterable>(value)) {
                SerFilterResult next;  // a set's items keep an empty pair
                if (!no_position) {
                    next = filter.ask(idx);
                    idx++;
                    if (next.omit) continue;
                }
                if (!first) out += ",";
                first = false;
                out += infer_json(py::reinterpret_borrow<py::object>(item), ensure_ascii, -1,
                                  next.include, next.exclude);
            }
            out += "]";
            return out;
        }
        // The same table the json arm of the object walk consults, so that a value cannot be
        // text in one json run and a refusal in another: Display for a Decimal or Fraction,
        // the bytes of a UUID's own int for a UUID, str() with the error left standing for
        // Url/Path/ipaddress, the pattern source for a compiled pattern.
        {
            py::object str_form;
            if (ser_infer_json_str(value, str_form))
                return json_escape(str_form.cast<std::string>(), ensure_ascii);
        }
        if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
            std::string out = "[";
            bool first = true;
            SerIndexFilter filter;
            filter.bind(include, exclude, py::len(value));
            py::ssize_t idx = 0;
            for (auto item : py::reinterpret_borrow<py::sequence>(value)) {
                auto next = filter.ask(idx);
                idx++;
                if (next.omit) continue;
                if (!first) out += ",";
                first = false;
                out += infer_json(py::reinterpret_borrow<py::object>(item), ensure_ascii, -1,
                                  next.include, next.exclude);
            }
            out += "]";
            return out;
        }
        if (py::isinstance<py::dict>(value)) {
            std::string out = "{";
            bool first = true;
            auto d = value.cast<py::dict>();
            for (auto item : d) {
                // The key is asked exactly as written, before it is turned into its json form.
                auto next = apply_ser_filter(py::reinterpret_borrow<py::object>(item.first), include, exclude);
                if (next.omit) continue;
                if (!first) out += ",";
                first = false;
                out += json_escape(infer_json_key(py::reinterpret_borrow<py::object>(item.first), true).cast<std::string>(), ensure_ascii);
                out += ":";
                out += infer_json(py::reinterpret_borrow<py::object>(item.second), ensure_ascii, -1,
                                  next.include, next.exclude);
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
            if (py_hasattr(value, "_url")) {
                auto inner = py::getattr(value, "_url");
                if (py::isinstance(inner, url_cls) || py::isinstance(inner, murl_cls)) {
                    return json_escape(py::str(value).cast<std::string>(), ensure_ascii);
                }
            }
        } catch (...) {}

        // Rust infer_to_python serializes a pydantic model through its own
        // __pydantic_serializer__ (infer.rs:649) and lets a failure from it escape, so
        // this does too: catching it used to fall through to the __dict__ read below,
        // which quietly emitted the very fields the annotated serializer had refused to
        // produce, and would emit a subclass's extra fields besides.  A type object is
        // not such a value even though it answers the attribute too (ob_type.rs:421).
        if (py_hasattr(value, "__pydantic_serializer__") && !PyType_Check(value.ptr())) {
            SerNestedCall nested;
            auto ser = py::getattr(value, "__pydantic_serializer__");
            py::dict kw = ser_extra_forwarded();
            // call_pydantic_serializer (infer.rs:662-673) swaps state.config for the delegated
            // serializer's own and writes the value into *this* run's sink with it, so what the
            // delegated value is worth -- the form a nan takes, bytes as utf8 or base64, a
            // datetime as ISO or as seconds -- is settled by that serializer and never by the
            // call that happened to reach it.  Taking to_python's objects back and printing them
            // here gets every one of those that is still a Python object wrong, which is today
            // only a non-finite float: `to_json(M())` printed NaN where the wheel prints null,
            // and `to_json(M(), inf_nan_mode='null')` null where the wheel keeps a model that
            // asked for constants at NaN.  Splicing the delegated serializer's JSON is the same
            // boundary; the indent this run was asked for stays outside, since it is applied to
            // the whole compact string at the end by json_pretty_print.
            kw["ensure_ascii"] = ensure_ascii;
            // Only the config is swapped (infer.rs:662-673): the state the walk carries --
            // and its include/exclude -- is handed straight on, so the filters that got this
            // far are the filters the delegated value's fields are asked about too.  Dropping
            // them made `to_json(M(), include={"a": True})` print every field.
            kw["include"] = include;
            kw["exclude"] = exclude;
            py::bytes as_json = ser.attr("to_json")(value, **kw);
            return as_json.cast<std::string>();
        }

        // A class answers the field/dataclass probes too, and its __dict__ is the
        // class's own namespace, so Rust refuses type objects here as well
        // (ob_type.rs:421); walking it used to report the failure as a mappingproxy.
        if (py_hasattr(value, "__dict__") && !PyType_Check(value.ptr()) && dict_inferable_object(value))
            return infer_json(value.attr("__dict__"), ensure_ascii, indent, include, exclude);

        // Rust infer_serialize ObType::Unknown (infer.rs:495-508): the run's `fallback`
        // is asked first and what it returns is re-inferred, because a fallback may hand
        // back a model or another unknown of its own.  `serialize_unknown` stringifies.
        // With neither, the failure names the value's *type* -- the value is by
        // definition something this run cannot print usefully -- and a fallback that
        // raises keeps its own error, which the serde boundary then names.
        if (g_ser_extra.fallback.ptr() && !g_ser_extra.fallback.is_none())
            return infer_json(g_ser_extra.fallback(value), ensure_ascii, indent, include, exclude);
        if (g_ser_extra.serialize_unknown) return json_escape(ser_serialize_unknown(value), ensure_ascii);
        throw PydanticSerializationError("Unable to serialize unknown type: " + ser_safe_repr(py::type::of(value)));
    }

    // Recursively serialize an arbitrary Python value for use in extra fields
    // (mirrors Rust's infer_to_python: models → to_python, dicts → recurse, lists → recurse).
    // Cyclic values return the innermost occurrence as-is (Rust infer_to_python
    // semantics) instead of recursing forever.
    static std::vector<const void*>& py_rec_stack() {
        static thread_local std::vector<const void*> stack;
        return stack;
    }

    // Rust takes a recursion guard for every value the infer walks, not only for containers, and
    // the guard answers two questions at once (infer.rs:52-67, recursion_guard.rs:32-42): an id
    // already on the way down is a reference cycle, and a walk that only gets absurdly deep is
    // refused the same way it is stopped -- depth > 255, so 255 values may be open at once.  A
    // python run swallows either answer and hands back the value it was given, while a json run --
    // and mode='json' is one -- lets the ValueError out (extra.rs:93-94).  Counting scalars is not
    // decoration: a fallback that returns its own argument repeats an id at the second step and a
    // fallback that builds a new object every call never repeats one, so neither is a container and
    // the walk between them and a segfault is this guard.
    static py::object serialize_any_value(const py::object& v, bool exc_none, bool round_trip, bool json_mode = false,
                                          const py::object& include = py::none(),
                                          const py::object& exclude = py::none()) {
        std::vector<const void*>& st = py_rec_stack();
        const void* p = v.ptr();
        for (const void* q : st) {
            if (q == p) {
                if (!json_mode) return v;
                throw py::value_error("Circular reference detected (id repeated)");
            }
        }
        if (st.size() >= 255) {
            if (!json_mode) return v;
            throw py::value_error("Circular reference detected (depth exceeded)");
        }
        st.push_back(p);
        struct StackPop {
            std::vector<const void*>& s;
            ~StackPop() { s.pop_back(); }
        } popper{st};
        return serialize_any_value_inner(v, exc_none, round_trip, json_mode, include, exclude);
    }

    static py::object serialize_any_value_inner(const py::object& v, bool exc_none, bool round_trip, bool json_mode,
                                                const py::object& include, const py::object& exclude) {
        if (v.is_none()) return py::none();
        // Model / dataclass instances: use __pydantic_serializer__ if available
        if (py_hasattr(v, "__pydantic_serializer__") && !PyType_Check(v.ptr())) {
            auto ser = py::getattr(v, "__pydantic_serializer__");
            // Rust call_pydantic_serializer keeps the current state, so a
            // serialize_as_any dump stays inferred all the way down.  Returning `v`
            // when the call raised handed back a model instance that the caller had
            // asked for in serialized form; Rust lets the error out, so it goes out here.
            SerNestedCall nested;
            py::dict kw = ser_extra_forwarded();
            kw["exclude_none"] = exc_none;
            kw["round_trip"] = round_trip;
            // The same state is kept through the delegation (infer.rs:662-673), so the pair
            // this position was reached with is the pair its fields are asked about.
            kw["include"] = include;
            kw["exclude"] = exclude;
            return ser.attr("to_python")(v, py::arg("mode") = (json_mode ? "json" : "python"), **kw);
        }
        // A dataclass that carries no serializer of its own has its fields inferred one by one
        // (ob_type.rs:290-291 puts the `__dataclass_fields__` duck test right after the
        // `__pydantic_serializer__` one and refuses it for a class).  What comes out is pairs
        // through serialize_pairs, so a field's name is what the filter is asked about and its
        // value is inferred under the pair that answer names, exactly as a mapping's entries are.
        if (py_hasattr(v, "__dataclass_fields__") && !PyType_Check(v.ptr())) {
            py::dict fields = ser_dataclass_pairs(v);
            py::dict out;
            for (auto item : fields) {
                py::object name = py::reinterpret_borrow<py::object>(item.first);
                auto next = apply_ser_filter(name, include, exclude);
                if (next.omit) continue;
                out[name] = serialize_any_value(py::reinterpret_borrow<py::object>(item.second),
                                                exc_none, round_trip, json_mode,
                                                next.include, next.exclude);
            }
            return std::move(out);
        }
        if (py::isinstance<py::dict>(v)) {
            py::dict out;
            auto d = v.cast<py::dict>();
            for (auto item : d) {
                auto k = py::reinterpret_borrow<py::object>(item.first);
                auto val = py::reinterpret_borrow<py::object>(item.second);
                // serialize_pairs (infer.rs:729-741) asks AnyFilter::key_filter about every key of
                // an inferred mapping and hands the value the pair that answer names, with the key
                // asked exactly as written -- it is the entry's *key* that is turned afterwards.
                auto next = apply_ser_filter(k, include, exclude);
                if (next.omit) continue;
                // serialize_entry (infer.rs:734-741) hands the key and the value to the *same*
                // any serializer, so a key is inferred too rather than passed through: in json
                // mode that is the json_key question -- an int key leaves as "1" and a tuple key
                // as "1,2" while the values keep their Python forms -- and in python mode it is
                // the very same walk the value takes, which is what turns a Fraction key into
                // '3/2', a namedtuple key into a plain tuple, and a frozenset key into a rebuilt
                // frozenset, and rebuilds a frozen-dataclass key until the dict refuses it as
                // unhashable.  A key the python walk would hand back unchanged -- a str, an int,
                // a Decimal (the python arm of infer.rs has no Decimal case to stringify) --
                // answers for itself either way, so only those forms change here.
                if (json_mode)
                    k = infer_json_key(k, false);
                else
                    k = serialize_any_value(k, exc_none, round_trip, json_mode,
                                            next.include, next.exclude);
                out[k] = serialize_any_value(val, exc_none, round_trip, json_mode, next.include, next.exclude);
            }
            return std::move(out);
        }
        if (py::isinstance<py::list>(v)) {
            py::list out;
            auto lst = v.cast<py::list>();
            SerIndexFilter filter;
            filter.bind(include, exclude, py::len(lst));
            py::ssize_t idx = 0;
            for (auto item : lst) {
                auto next = filter.ask(idx);
                idx++;
                if (next.omit) continue;
                out.append(serialize_any_value(py::reinterpret_borrow<py::object>(item), exc_none, round_trip,
                                               json_mode, next.include, next.exclude));
            }
            return std::move(out);
        }
        if (py::isinstance<py::tuple>(v)) {
            py::list temp;
            auto t = v.cast<py::tuple>();
            SerIndexFilter filter;
            filter.bind(include, exclude, py::len(t));
            py::ssize_t idx = 0;
            for (auto item : t) {
                auto next = filter.ask(idx);
                idx++;
                if (next.omit) continue;
                temp.append(serialize_any_value(py::reinterpret_borrow<py::object>(item), exc_none, round_trip,
                                                json_mode, next.include, next.exclude));
            }
            if (json_mode) return std::move(temp);  // JSON has no tuple type
            return py::tuple(temp);
        }
        if (py::isinstance<py::set>(v) || py::isinstance<py::frozenset>(v)) {
            py::list temp;
            // The set and frozenset arms are the one place the infer walk *drops* a filter on the
            // way down (serialize_seq!, infer.rs:68-77): a set has no position to ask about, and
            // its items are inferred under an empty pair, so a run that asked for index 0 keeps a
            // set's whole tuple item rather than that tuple's first element.
            for (auto item : py::reinterpret_borrow<py::iterable>(v)) {
                temp.append(serialize_any_value(py::reinterpret_borrow<py::object>(item), exc_none, round_trip,
                                                json_mode));
            }
            if (json_mode) return std::move(temp);  // JSON has no set type
            return py::isinstance<py::frozenset>(v)
                ? py::object(py::frozenset(temp))
                : py::object(py::set(temp));
        }
        // Sequence[Model] serializes through pydantic's serialize_sequence_via_list,
        // which hands back the container type it was given; the items still need
        // serializing, as Rust's own sequence serializer does.
        if (py::isinstance(v, py_deque_type())) {
            py::list temp;
            // A deque is a sequence to the infer walk (serialize_seq_filter! over value.len()?,
            // infer.rs:156-158 / 250-252), so its positions are filtered -- a filter that is
            // neither a set nor a dict is refused from inside that ask -- and new_deque puts the
            // source's maxlen back on the value that comes out.
            SerIndexFilter filter;
            py::ssize_t dlen = -1;
            try { dlen = py::len(v); } catch (...) { PyErr_Clear(); }
            filter.bind(include, exclude, dlen);
            py::ssize_t idx = 0;
            for (auto item : v) {
                auto next = filter.ask(idx);
                idx++;
                if (next.omit) continue;
                temp.append(serialize_any_value(py::reinterpret_borrow<py::object>(item), exc_none, round_trip,
                                                json_mode, next.include, next.exclude));
            }
            if (json_mode) return std::move(temp);  // JSON has no deque type
            return py_deque_new(temp, py_deque_maxlen(v));
        }
        // Rust infer_to_python ObType::Generator (infer.rs:264-271): a python run does not
        // consume the iterator it was handed, it hands the caller a lazy SerializationIterator
        // that serializes each item as that item is pulled.  This is the answer for a value that
        // is *itself* an iterator -- a range is not one, and a node of another type gets here too
        // once it has warned about the value and fallen through to inference.  A json run has
        // nowhere to put laziness, so its arm drains the iterator into a list like the other
        // sequence arms above.  Being a known type, it is answered before the fallback is asked.
        if (PyIter_Check(v.ptr())) {
            if (!json_mode) return make_inferred_iterator(v, exc_none, round_trip, include, exclude);
            // The json arm has nowhere to put laziness but keeps the filtering: infer.rs:202-213
            // (python) and :482-490 (serde) both ask index_filter with no length, which is where a
            // negative key is refused rather than folded.
            py::list temp;
            SerIndexFilter filter;
            filter.bind(include, exclude, -1);
            py::ssize_t idx = 0;
            for (auto item : py::reinterpret_borrow<py::iterable>(v)) {
                auto next = filter.ask(idx);
                idx++;
                if (next.omit) continue;
                temp.append(serialize_any_value(py::reinterpret_borrow<py::object>(item), exc_none, round_trip,
                                                json_mode, next.include, next.exclude));
            }
            return std::move(temp);
        }
        // Rust infer_to_python ObType::Fraction: the display string in both
        // Python and JSON mode, and a display that pyo3 makes safe to fail.
        if (py::isinstance(v, py_fraction_type())) return ser_display(v);
        // Rust infer_to_python ObType::Unknown: the caller's `fallback` callable
        // gets a turn (its result is re-inferred) before the value is passed
        // through untouched.
        if (!py_infer_known_type(v) && g_ser_extra.fallback.ptr() && !g_ser_extra.fallback.is_none()) {
            py::object next = g_ser_extra.fallback(v);
            return serialize_any_value(next, exc_none, round_trip, json_mode, include, exclude);
        }
        if (json_mode) {
            // Rust infer_to_python ObType::Enum serializes the member's value.
            if (is_enum_instance(v) && py_hasattr(v, "value")) {
                return serialize_any_value(py::getattr(v, "value"), exc_none, round_trip, json_mode, include, exclude);
            }
            // infer_to_python's json arm (infer.rs:115-121): a float that is neither finite
            // nor a number is taken away by the mode that asks for it to be gone -- the mode
            // of the serializer this run belongs to, which is no longer the entry point's
            // once a value brought its own serializer.  A typed float leaf is not asked:
            // `SchemaSerializer(float_schema()).to_python(nan, mode="json")` keeps nan.
            if (PyFloat_Check(v.ptr())) {
                double d = PyFloat_AsDouble(v.ptr());
                if ((std::isnan(d) || std::isinf(d)) && g_ser_extra.inf_nan_mode == "null")
                    return py::none();
            }
            // infer.rs:106-124 rebuilds a scalar subclass as the exact type it subclasses -- "have
            // to do this to make sure subclasses of for example str are upcast to str" -- because
            // what the json run holds is a number or a text, not an object that behaves like one.
            // An exact value keeps its incref path, and the python run has no such arm at all,
            // which is why model_dump() can hold a SubInt that model_dump(mode='json') answers as a
            // plain int.  A bool is an exact type of its own and is not upcast to an int.
            if (PyLong_Check(v.ptr()) && !PyBool_Check(v.ptr())) return ser_exact_int(v);
            if (PyFloat_Check(v.ptr())) return ser_exact_float(v);
            if (PyUnicode_Check(v.ptr())) return ser_exact_str(v);
            py::object converted;
            if (json_infer_leaf(v, converted)) return converted;
            if (ser_infer_json_str(v, converted)) return converted;
            // The json arm ends at ObType::Unknown (infer.rs:221-231): after the `fallback`
            // had its turn above, `serialize_unknown` takes str() of the value -- a
            // placeholder when str() itself raises -- and with neither the run refuses by
            // naming the value's type.  A Python-mode run asks none of this and hands the
            // object straight back (:279-286), which is why model_dump() can hold a value
            // that model_dump(mode='json') refuses.
            if (!ser_infer_known_ob_type(v)) {
                if (g_ser_extra.serialize_unknown) return py::str(ser_serialize_unknown(v));
                throw PydanticSerializationError(
                    "Unable to serialize unknown type: " + ser_safe_repr(py::type::of(v)));
            }
        }
        return v;
    }

    // Rust's leaf serializers type-check each value they serialize and warn on
    // a mismatch ("Expected `X` - serialized value may not be as expected").
    // Wrapper nodes ("default"/"with-default"/"lax-or-strict"/"nullable"/
    // "json-or-python"/definitions) forward non-None values to an inner node,
    // so resolve the node that actually receives the value before applying the
    // declared-type check.  Returns nullptr when a wrapper intercepts the value
    // (e.g. None with a default returns the default without reaching a leaf).
    static SerRef ser_leaf_for_value(const SerRef& n, const py::object& v, bool json_mode) {
        SerRef cur = n;
        while (cur) {
            const std::string& t = cur->type;
            if (t == "default" || t == "with-default") {
                cur = cur->children.empty() ? nullptr : cur->children[0];
            } else if (t == "lax-or-strict" || t == "definitions" || t == "definition-ref") {
                cur = cur->children.empty() ? nullptr : cur->children[0];
            } else if (t == "json-or-python") {
                // Python mode uses children[1], JSON mode children[0].
                cur = cur->children.empty() ? nullptr
                    : (json_mode ? cur->children[0]
                       : (cur->children.size() > 1 ? cur->children[1] : cur->children[0]));
            } else if (t == "nullable" || t == "nullable-union") {
                if (v.is_none()) return nullptr;
                cur = cur->children.empty() ? nullptr : cur->children[0];
            } else {
                break;
            }
        }
        return cur;
    }

    // Rust SerFields::serialize turns a key the schema never declared into an
    // "Unexpected field" mismatch while a union picks a member under strict
    // checks, so the union moves on instead of quietly dropping the key.  Only a
    // typed-dict that takes extras keeps unknown keys (Rust
    // FieldsMode::TypedDictAllow), and the __pydantic_* bookkeeping the port
    // carries in its own dict representation is not a field either.
    void ser_reject_unexpected_fields(const py::dict& main) const {
        if (g_ser_check != 1 || extra_allowed) return;
        for (const auto& item : main) {
            std::string key;
            try { key = py::str(item.first).cast<std::string>(); } catch (...) { continue; }
            if (fields.count(key) || computed_fields_.count(key)) continue;
            if (key == "__pydantic_fields_set__" || key == "__pydantic_defaults__" ||
                key == "__pydantic_extra__")
                continue;
            py::object exc_type =
                py::module_::import("pydantic_core_cpp").attr("PydanticSerializationUnexpectedValue");
            PyErr_SetString(exc_type.ptr(), ("Unexpected field `" + key + "`").c_str());
            throw py::error_already_set();
        }
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
        else if (py_hasattr(value, "__dict__")) main = py::getattr(value, "__dict__").cast<py::dict>();
        py::object missing_obj = missing_sentinel_obj();
        ser_reject_unexpected_fields(main);

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
                } else if (py_hasattr(value, "__pydantic_fields_set__")) {
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
            } else {
                // Rust iterates the instance's own __dict__: a declared field
                // that is absent from it (e.g. removed by copy(exclude=...))
                // is NOT emitted, even when the schema declares a default.
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

            // Field values are type-checked against the node that actually
            // receives them (wrappers forward the value inward).  Applies to
            // regular fields as well as computed fields, mirroring the Rust
            // leaf-serializer checks.
            // Rust SerFields::prepare_value: with serialize_as_any every field
            // serializer is replaced by inference, except a custom
            // @field_serializer, which must still run.
            const bool ser_as_any = g_ser_extra.serialize_as_any &&
                !((ser->type == "function-plain" || ser->type == "function-wrap") && ser->is_field_serializer);

            SerRef type_leaf = (ser_as_any || !ser || fv.is_none())
                ? nullptr : ser_leaf_for_value(ser, fv, json_mode);
            bool ser_type_mismatch = false;
            if (type_leaf &&
                type_leaf->type != "function-plain" && type_leaf->type != "function-wrap" &&
                type_leaf->type != "function-after" && type_leaf->type != "function-before" &&
                !SerNode::value_matches_type(type_leaf, fv)) {
                ser_type_mismatch = true;
                ser_warn_unexpected_value(k, SerNode::type_name_for_warning(type_leaf), fv);
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

            if (ser_as_any) {
                serialized = serialize_any_value(fv, exc_none, round_trip, json_mode, next.include, next.exclude);
            } else if (ser_type_mismatch) {
                // Field value does not match its declared type: warn and fall
                // back to infer serialization (Rust behavior).
                serialized = serialize_any_value(fv, exc_none, round_trip, json_mode, next.include, next.exclude);
            } else if (use_field_serializer) {
                // Try field serializer call with model instance first
                auto info = make_ser_info(round_trip, k, context, next.include, next.exclude);
                bool tried = false;
                try {
                    if (ser->type == "function-wrap") {
                        // For wrap mode, create a handler function
                        py::object handler = py::cpp_function([ser, fv, json_mode, exc_none, round_trip, next, by_alias, exclude_unset, exclude_defaults, context](const py::object& v, py::object index_key) -> py::object {
                            py::object inc = next.include, exc = next.exclude;
                            if (!index_key.is_none()) {
                                ser_check_index_key(index_key);
                                auto f = apply_ser_filter(index_key, next.include, next.exclude);
                                if (f.omit) throw PydanticOmit();
                                inc = f.include;
                                exc = f.exclude;
                            }
                            // The handler Rust gives a wrap function carries the
                            // call's own state (SerializationCallable, function.rs:386)
                            // and serializes with it (:494), so in a json call the
                            // function is handed the JSON form -- an ISO string for a
                            // datetime -- and can branch on info.mode.
                            if (!ser->children.empty()) {
                                return ser->children[0]->to_python(v, json_mode, exc_none, round_trip, inc, exc, by_alias, exclude_unset, exclude_defaults, context);
                            }
                            return v;
                        }, py::arg("value"), py::arg("index_key") = py::none());
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
                } catch (const py::error_already_set& e) {
                    tried = handle_ser_call_error(e, ser->func_name) ? false : true;
                    if (tried) throw;  // unreachable; keeps the compiler happy
                }
                if (!tried) {
                    // Fallback to inner schema if available, otherwise use default serialization
                    if (!ser->children.empty()) {
                        serialized = ser->children[0]->to_python(fv, json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context);
                    } else {
                        serialized = ser->to_python(fv, json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context);
                    }
                } else {
                    serialized = ser->apply_return_ser(serialized, json_mode, exc_none, round_trip, py::none(),
                        py::none(), by_alias, exclude_unset, exclude_defaults, context);
                }
            } else {
                // Use the field serializer directly (it handles list/dict iteration, etc.)
                serialized = ser->to_python(fv, json_mode, exc_none, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, context);
            }
            result[py::str(output_key)] = serialized;
        }
        // Rust FieldsMode::TypedDictAllow: the extras of a typed-dict live in
        // the dict itself rather than in __pydantic_extra__, so every key the
        // schema does not declare is serialized too.
        if (typed_dict_allow_extra) {
            for (auto item : main) {
                std::string k = py::str(item.first).cast<std::string>();
                if (fields.find(k) != fields.end()) continue;
                if (k == "__pydantic_extra__" || k == "__pydantic_fields_set__" ||
                    k == "__pydantic_defaults__") continue;
                auto next = apply_ser_filter(py::str(k), include, exclude);
                if (next.omit) continue;
                py::object v = py::reinterpret_borrow<py::object>(item.second);
                if (exc_none && v.is_none()) continue;
                if (v.is(missing_obj)) continue;
                py::object serialized = extra_ser
                    ? extra_ser->to_python(v, json_mode, exc_none, round_trip, next.include, next.exclude,
                                           by_alias, exclude_unset, exclude_defaults, context)
                    : serialize_any_value(v, exc_none, round_trip, json_mode, next.include, next.exclude);
                result[py::str(k)] = serialized;
            }
        }
        // Extra fields - also apply include/exclude if they match by name
        if (extra_allowed && py_hasattr(value, "__pydantic_extra__")) {
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
                    result[py::str(k)] = serialize_any_value(v, exc_none, round_trip, json_mode, next.include, next.exclude);
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
                                       bool exclude_defaults = false,
                                       const py::object& context = py::none()) const {
        std::string out = "{";
        bool first = true;
        py::dict main;
        if (py::isinstance<py::dict>(value)) main = value.cast<py::dict>();
        else if (py_hasattr(value, "__dict__")) main = py::getattr(value, "__dict__").cast<py::dict>();
        py::object missing_obj = missing_sentinel_obj();
        ser_reject_unexpected_fields(main);

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
                } else if (py_hasattr(value, "__pydantic_fields_set__")) {
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
            } else {
                // Rust iterates the instance's own __dict__: a declared field
                // that is absent from it (e.g. removed by copy(exclude=...))
                // is NOT emitted, even when the schema declares a default.
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

            // Field values are type-checked against the node that actually
            // receives them (wrappers forward the value inward).  Applies to
            // regular fields as well as computed fields, mirroring the Rust
            // leaf-serializer checks.
            // Rust SerFields::prepare_value, json mode (see the python branch).
            const bool ser_as_any = g_ser_extra.serialize_as_any &&
                !((ser->type == "function-plain" || ser->type == "function-wrap") && ser->is_field_serializer);

            SerRef type_leaf = (ser_as_any || !ser || fv.is_none())
                ? nullptr : ser_leaf_for_value(ser, fv, true);
            bool ser_type_mismatch = false;
            if (type_leaf &&
                type_leaf->type != "function-plain" && type_leaf->type != "function-wrap" &&
                type_leaf->type != "function-after" && type_leaf->type != "function-before" &&
                !SerNode::value_matches_type(type_leaf, fv)) {
                ser_type_mismatch = true;
                ser_warn_unexpected_value(k, SerNode::type_name_for_warning(type_leaf), fv);
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

            if (ser_as_any) {
                field_json = infer_json(fv, ensure_ascii, -1, next.include, next.exclude);
            } else if (use_field_serializer) {
                // Try field serializer call with model instance first
                auto info = make_ser_info(round_trip, k, context, next.include, next.exclude);
                bool tried = false;
                try {
                    py::object result;
                    if (ser->type == "function-wrap") {
                        // For wrap mode, create a handler function
                        py::object handler = py::cpp_function([ser, fv, ensure_ascii, round_trip, next, by_alias, exclude_unset, exclude_defaults, exc_none, context](const py::object& v, py::object index_key) -> py::object {
                            py::object inc = next.include, exc = next.exclude;
                            if (!index_key.is_none()) {
                                ser_check_index_key(index_key);
                                auto f = apply_ser_filter(index_key, next.include, next.exclude);
                                if (f.omit) throw PydanticOmit();
                                inc = f.include;
                                exc = f.exclude;
                            }
                            if (!ser->children.empty()) {
                                // Call to_python and let infer_json handle the conversion
                                auto py_result = ser->children[0]->to_python(v, true, exc_none, round_trip, inc, exc, by_alias, exclude_unset, exclude_defaults, context);
                                return py_result;
                            }
                            return v;
                        }, py::arg("value"), py::arg("index_key") = py::none());
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
                    field_json = infer_json(result, ensure_ascii, -1, py::none(), py::none());
                    tried = true;
                } catch (const py::error_already_set& e) {
                    tried = handle_ser_call_error(e, ser->func_name) ? false : true;
                    if (tried) throw;  // unreachable; keeps the compiler happy
                }
                if (!tried) {
                    // Fallback to field serializer (handles list/dict iteration, etc.)
                    field_json = ser->to_json(fv, ensure_ascii, -1, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
                }
            } else if (ser_type_mismatch) {
                // Field value does not match its declared type: warn and fall
                // back to infer serialization (Rust behavior).
                field_json = infer_json(fv, ensure_ascii, -1, next.include, next.exclude);
            } else {
                // Use the field serializer directly (handles list/dict iteration, etc.)
                field_json = ser->to_json(fv, ensure_ascii, -1, round_trip, next.include, next.exclude, by_alias, exclude_unset, exclude_defaults, exc_none, context);
            }

            if (!first) out += ",";
            first = false;
            out += json_escape(output_key, ensure_ascii) + ":" + field_json;
        }
        // Rust FieldsMode::TypedDictAllow: undeclared keys of a typed-dict are
        // part of the dict itself and are serialized with extras_schema.
        if (typed_dict_allow_extra) {
            for (auto item : main) {
                std::string k = py::str(item.first).cast<std::string>();
                if (fields.find(k) != fields.end()) continue;
                if (k == "__pydantic_extra__" || k == "__pydantic_fields_set__" ||
                    k == "__pydantic_defaults__") continue;
                auto next = apply_ser_filter(py::str(k), include, exclude);
                if (next.omit) continue;
                py::object v = py::reinterpret_borrow<py::object>(item.second);
                if (exc_none && v.is_none()) continue;
                if (v.is(missing_obj)) continue;
                std::string field_json = extra_ser
                    ? extra_ser->to_json(v, ensure_ascii, -1, round_trip, next.include, next.exclude,
                                         by_alias, exclude_unset, exclude_defaults, exc_none, context)
                    : infer_json(v, ensure_ascii, -1, next.include, next.exclude);
                if (!first) out += ",";
                first = false;
                out += json_escape(k, ensure_ascii) + ":" + field_json;
            }
        }
        if (extra_allowed && py_hasattr(value, "__pydantic_extra__")) {
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
                    out += json_escape(k, ensure_ascii) + ":" + infer_json(v, ensure_ascii, -1, next.include, next.exclude);
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
    return n->type_name_for_warning();
}

std::string SerNode::type_name_for_warning() const {
    const std::string& t = type;
    auto child0 = [&]() -> std::string {
        return children.empty() ? "any" : type_name_for_warning(children[0]);
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
    if (t == "deque") return "deque[" + child0() + "]";
    if (t == "frozenset") return "frozenset[" + child0() + "]";
    if (t == "tuple") {
        // Rust composes the name at build time from every item serializer and inserts "..."
        // after the variadic one (tuple.rs:46-52), so a positional tuple keeps all of its
        // items in the name, a variadic one says tuple[int, ...], and an empty one tuple[].
        std::vector<std::string> names;
        for (const auto& c : children) names.push_back(type_name_for_warning(c));
        if (tuple_variadic_index >= 0) {
            size_t at = static_cast<size_t>(tuple_variadic_index) + 1;
            if (at > names.size()) at = names.size();
            names.insert(names.begin() + at, "...");
        }
        std::string out = "tuple[";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) out += ", ";
            out += names[i];
        }
        return out + "]";
    }
    if (t == "named-tuple") return class_name.empty() ? std::string("named-tuple") : class_name;
    if (t == "dict") {
        std::string keyn = children.size() > 0 ? type_name_for_warning(children[0]) : "any";
        std::string valn = children.size() > 1 ? type_name_for_warning(children[1]) : "any";
        return "dict[" + keyn + ", " + valn + "]";
    }
    // simple.rs:25-39 keeps NoneSerializer's name as its EXPECTED_TYPE, so a bare node says
    // Expected `none` and a container composes list[none], dict[none, any], tuple[none].
    if (t == "none" || t == "is-none") return "none";
    if (t == "any") return "any";
    return t;
}

// Whether a Python value is compatible with the node's declared Python type.
// Mirrors Rust's ObType::is_type / IsType::False -> warn fallback.  Unknown or
// permissive node types always return true (no warning).
bool SerNode::value_matches_type(const SerRef& n, const py::object& v) {
    if (!n) return true;
    return n->value_matches_type(v);
}

// A class kept for the lifetime of the interpreter and asked whether a value is one of
// it.  A class that could not be imported asks nothing: the node stays permissive, which is
// what it was before it had a predicate at all, rather than refusing every value.
static bool ser_matches_class(const py::object& cls, const py::object& v) {
    if (!cls.ptr()) return true;
    try {
        return py::isinstance(v, cls);
    } catch (const py::error_already_set&) {
        PyErr_Clear();
        return true;
    }
}

bool SerNode::value_matches_type(const py::object& v) const {
    if (v.is_none()) return true;
    const std::string& t = type;
    if (t == "any" || t == "is-instance" || t == "is-subclass") return true;
    // NoneSerializer answers its own type exactly (simple.rs:44-54, :56-68): a subclass of
    // None cannot exist, so anything else is a mismatch it warns about and then infers.
    if (t == "none" || t == "is-none") return false;
    if (t == "int" || t == "int-constrained") return py::isinstance<py::int_>(v);   // bool is an int subclass
    if (t == "float" || t == "float-constrained") return py::isinstance<py::float_>(v) || py::isinstance<py::int_>(v);
    if (t == "bool") return py::isinstance<py::bool_>(v);
    if (t == "str" || t == "string" || t == "str-constrained") return py::isinstance<py::str>(v);
    if (t == "bytes") return py::isinstance<py::bytes>(v);
    if (t == "list") return py::isinstance<py::list>(v);
    if (t == "set") return py::isinstance<py::set>(v);
    if (t == "deque") return py::isinstance(v, py_deque_type());
    if (t == "frozenset") return py::isinstance<py::frozenset>(v);
    if (t == "tuple") return py::isinstance<py::tuple>(v);
    // Rust ObTypeLookup::is_type for a named tuple is a plain PyTuple check: the
    // class itself is only compared while a union checks its choices.
    if (t == "named-tuple") return py::isinstance<py::tuple>(v);
    if (t == "dict") return py::isinstance<py::dict>(v);
    // The rest of Rust's leaf serializers refuse a value that is not of their own type --
    // or of a subclass of it, which ob_type.rs's ancestor walk grants -- warn in that
    // type's name, and print what inference makes of the value instead.
    if (t == "date" || t == "datetime" || t == "time" || t == "timedelta") {
        try {
            static const py::object& date_cls =
                held_python_object([] { return py::module_::import("datetime").attr("date"); });
            static const py::object& datetime_cls =
                held_python_object([] { return py::module_::import("datetime").attr("datetime"); });
            static const py::object& time_cls =
                held_python_object([] { return py::module_::import("datetime").attr("time"); });
            static const py::object& timedelta_cls =
                held_python_object([] { return py::module_::import("datetime").attr("timedelta"); });
            // A datetime is date's own subclass, yet the date serializer says E:date for it,
            // so the date node asks for a date that is not a datetime rather than trusting
            // the ancestor walk.
            if (t == "date" && py::isinstance(v, datetime_cls)) return false;
            const py::object& want = t == "date" ? date_cls
                                         : t == "datetime" ? datetime_cls
                                         : t == "time" ? time_cls : timedelta_cls;
            return py::isinstance(v, want);
        } catch (const py::error_already_set&) {
            PyErr_Clear();
            return true;
        }
    }
    const SerStrClasses& str_classes = ser_str_classes();
    if (t == "decimal") return ser_matches_class(str_classes.decimal, v);
    if (t == "uuid") return ser_matches_class(str_classes.uuid, v);
    // Url and MultiHostUrl are each their own type: a Url at a multi-host node warns.
    if (t == "url") return ser_matches_class(str_classes.url, v);
    if (t == "multi-host-url") return ser_matches_class(str_classes.multihost_url, v);
    if (t == "complex") return PyComplex_Check(v.ptr());
    if (t == "fraction") return ser_matches_class(py_fraction_type(), v);
    // Rust's generator check is the iterator protocol -- a pyo3 downcast to PyIterator, which
    // is tp_iternext on the value's type -- so a generator and a list_iterator both pass while
    // a str, a list or an IPv4Network is foreign. Walking the foreign ones is both the wrong
    // answer and, for a /8 network, unbounded.
    if (t == "generator") return PyIter_Check(v.ptr());
    return true;
}

// ---------------------------------------------------------------------------
// Build serializer from schema
// ---------------------------------------------------------------------------
using SerMemo = std::unordered_map<const void*, SerRef>;

static thread_local int _build_ser_depth = 0;
static thread_local int _build_ser_truncated = 0;

// Rust resolves a single temporal mode for every temporal type: an explicit
// ser_json_temporal decides a timedelta's shape too -- even when it says
// iso8601 -- and only when that key is absent does ser_json_timedelta get a
// say.  The answer is given back in ser_json_timedelta's own vocabulary.
static std::string timedelta_mode_from_config(const py::dict& config) {
    if (config.contains("ser_json_temporal")) {
        std::string temporal = config["ser_json_temporal"].cast<std::string>();
        if (temporal == "seconds") return "float";
        if (temporal == "milliseconds") return "milliseconds";
        return "iso8601";
    }
    if (config.contains("ser_json_timedelta") &&
        config["ser_json_timedelta"].cast<std::string>() == "float") {
        return "float";
    }
    return "iso8601";
}

static SerRef build_ser_impl(const py::dict& schema,
                        std::unordered_map<std::string, SerRef>& defs,
                        SerMemo& memo);

static SerRef build_ser(const py::dict& schema,
                        std::unordered_map<std::string, SerRef>& defs,
                        SerMemo& memo) {
    // Pydantic emits a model schema once and points at it from every field
    // that uses it, so a core schema is a DAG: building a serializer per
    // reference costs the number of paths through the schema rather than its
    // size. A serializer depends on nothing but the node it was built from, so
    // shared nodes share one node.
    auto cached = memo.find(schema.ptr());
    if (cached != memo.end()) return cached->second;

    int truncated = _build_ser_truncated;
    auto ser = build_ser_impl(schema, defs, memo);
    // A subtree the recursion guard cut short is not the node this schema
    // describes, so it must not become the answer for that schema.
    if (ser && _build_ser_truncated == truncated) memo[schema.ptr()] = ser;
    return ser;
}

static SerRef build_ser_impl(const py::dict& schema,
                        std::unordered_map<std::string, SerRef>& defs,
                        SerMemo& memo) {
    _build_ser_depth++;
    if (_build_ser_depth > 200) {
        std::string t = "unknown";
        try { t = schema["type"].cast<std::string>(); } catch (...) {}
        fprintf(stderr, "ERROR: build_ser_impl recursion depth exceeded 200, type=%s\n", t.c_str());
        _build_ser_truncated++;
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

    // Rust's LiteralSerializer has no leaf of its own: with the optional
    // validate-while-serializing switch off (the default) it hands the value to
    // inference, which is what turns a str-Enum member into a plain string when
    // the target is JSON.
    if (type == "literal") {
        node->type = "any";
        return node;
    }

    // Rust FormatSerializer/ToStringSerializer default `when_used` to `json-unless-none`.
    if (type == "format" || type == "to-string") {
        node->when_used = "json-unless-none";
        auto read_when_used = [&](const py::dict& d) -> bool {
            try {
                if (d.contains("when_used")) {
                    node->when_used = d["when_used"].cast<std::string>();
                    return true;
                }
            } catch (...) {}
            return false;
        };
        if (!read_when_used(ser_dict)) read_when_used(schema);
    }

    // Handle model-field wrapper: unwrap to inner schema
    if (type == "model-field") {
        try {
            auto inner = build_ser_impl(schema["schema"].cast<py::dict>(), defs, memo);
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
        try { return build_ser(schema[key].cast<py::dict>(), defs, memo); } catch (...) { return nullptr; }
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
                // A definition may carry its own "serialization" override (a
                // model_serializer on a model, for instance). Unwrapping to
                // d["schema"] would drop it, so in that case build from the
                // definition itself and let the type-specific code unwrap.
                bool def_has_ser = false;
                try { def_has_ser = d.contains("serialization") && !d["serialization"].is_none(); } catch (...) {}
                // Model-like definitions keep their own node: unwrapping to
                // d["schema"] would lose class_, root_model and extra behaviour,
                // so a definition-ref could no longer reject a foreign value.
                std::string def_type;
                try { def_type = d["type"].cast<std::string>(); } catch (...) {}
                bool keep_model_wrapper = def_type == "model" || def_type == "dataclass" ||
                                          def_type == "typed-dict";
                if (d.contains("schema") && !def_has_ser && !keep_model_wrapper) {
                    actual = build_ser_impl(d["schema"].cast<py::dict>(), defs, memo);
                } else {
                    // Definitions without inner schema (e.g. enum), model-like
                    // definitions and those with their own serializer are built
                    // from the definition itself.
                    actual = build_ser_impl(d, defs, memo);
                }
                // Copy actual content into the stub (preserves shared_ptr identity)
                defs[ref]->copy_from(*actual);
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "Error building definitions: %s\n", e.what());
        }
        try { return build_ser_impl(schema["schema"].cast<py::dict>(), defs, memo); } catch (...) {}
        return node;
    }
    if (original_type == "definition-ref") {
        try {
            std::string ref = schema["schema_ref"].cast<std::string>();
            auto it = defs.find(ref);
            if (it != defs.end()) {
                // Rust CombinedSerializer::_build resolves a "serialization"
                // override with find_serializer(ser_type, ser_schema) before it
                // ever dispatches on the outer type, so the definition-ref is
                // dropped entirely. SerializeAsAny[X] arrives as {"type": "any"}.
                if (has_ser_dict) {
                    try {
                        std::string st = ser_dict["type"].cast<std::string>();
                        if (st != "include-exclude-sequence" && st != "include-exclude-dict" &&
                            st != "base64" && st != "function-plain" && st != "function-wrap") {
                            return build_ser_impl(ser_dict, defs, memo);
                        }
                    } catch (...) {}
                }
                // If this definition-ref has a serialization override, apply it
                if (has_ser_dict && ser_dict.contains("function")) {
                    auto wrapped = std::make_shared<SerNode>();
                    wrapped->type = ser_dict["type"].cast<std::string>();
                    wrapped->py_func = ser_dict["function"];
                    wrapped->info_arg = ser_info_arg;
                    wrapped->is_field_serializer = ser_is_field_serializer;
                    wrapped->when_used = ser_when_used;
                    try { wrapped->func_name = py::str(ser_dict["function"].attr("__name__")).cast<std::string>(); }
                    catch (...) { PyErr_Clear(); try { wrapped->func_name = py::str(ser_dict["function"].attr("__qualname__")).cast<std::string>(); } catch (...) { PyErr_Clear(); } }
                    try {
                        if (ser_dict.contains("return_schema")) {
                            wrapped->return_ser = build_ser(ser_dict["return_schema"].cast<py::dict>(), defs, memo);
                        } else {
                            auto any = std::make_shared<SerNode>();
                            any->type = "any";
                            wrapped->return_ser = any;
                        }
                    } catch (...) {}
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
        try { node->children.push_back(build_ser(schema["lax_schema"].cast<py::dict>(), defs, memo)); } catch (...) {}
        try { node->children.push_back(build_ser(schema["strict_schema"].cast<py::dict>(), defs, memo)); } catch (...) {}
    }

    // is-instance: no-op for serialization (passthrough)
    // No children to build — type stays as-is

    // Types with inner schema (schema key)
    if (type == "nullable" || type == "nullable-union" || type == "default" || type == "with-default" ||
        type == "json" || type == "format" || type == "to-string" || type == "enum" ||
        type == "missing-sentinel") {
        auto c = sub();
        if (c) node->children.push_back(c);
    }

    // Types with items_schema key
    if (type == "list" || type == "set" || type == "frozenset" || type == "deque" || type == "generator") {
        try {
            auto c = build_ser_impl(schema["items_schema"].cast<py::dict>(), defs, memo);
            if (c) node->children.push_back(c);
        } catch (...) {}
    }

    if (type == "dict") {
        auto ks = [&](const char* k) -> SerRef { try { return build_ser(schema[k].cast<py::dict>(), defs, memo); } catch (...) { return nullptr; } };
        auto key_ser = ks("keys_schema");
        auto val_ser = ks("values_schema");
        // A dict with no keys_schema asks its keys of the any serializer, not of a str one
        // (dict.rs:42-44 builds AnySerializer for the missing arm): the node is named
        // dict[any, any] after it (dict.rs:57-62, any.rs get_name), a non-str key costs no
        // warning, and a key of any type takes the infer walk's form -- which is what the
        // "str" node was already falling back to, only after refusing first.
        if (!key_ser) { key_ser = std::make_shared<SerNode>(); key_ser->type = "any"; }
        if (!val_ser) { val_ser = std::make_shared<SerNode>(); val_ser->type = "any"; }
        node->children.push_back(key_ser);
        node->children.push_back(val_ser);
    }

    if (type == "named-tuple") {
        // Rust NamedTupleSerializer::new keeps the class (union discrimination and
        // the wrong-type warning) and one serializer per field, by position.
        try { node->class_ = py::object(schema["cls"]); } catch (...) { PyErr_Clear(); }
        try {
            node->class_name = schema["cls_name"].cast<std::string>();
        } catch (...) {
            PyErr_Clear();
            try { node->class_name = node->class_.attr("__name__").cast<std::string>(); }
            catch (...) { PyErr_Clear(); }
        }
        try {
            std::vector<SerRef> kids;
            for (auto it : schema["fields"].cast<py::list>()) {
                py::dict f = it.cast<py::dict>();
                kids.push_back(build_ser(f["schema"].cast<py::dict>(), defs, memo));
            }
            node->children = std::move(kids);
        } catch (...) { PyErr_Clear(); }
    }
    if (type == "tuple") {
        try {
            auto items = schema["items_schema"];
            if (py::isinstance<py::list>(items)) {
                for (auto it : items.cast<py::list>())
                    node->children.push_back(build_ser(it.cast<py::dict>(), defs, memo));
            } else node->children.push_back(build_ser(items.cast<py::dict>(), defs, memo));
            if (schema.contains("variadic_item_index")) {
                node->tuple_variadic_index = schema["variadic_item_index"].cast<int>();
            }
        } catch (...) {}
    }

    if (type == "union") {
        try {
            for (auto ch : schema["choices"].cast<py::list>()) {
                if (py::isinstance<py::tuple>(ch)) {
                    auto t = ch.cast<py::tuple>();
                    node->children.push_back(build_ser(t[0].cast<py::dict>(), defs, memo));
                } else node->children.push_back(build_ser(ch.cast<py::dict>(), defs, memo));
            }
        } catch (...) {}
    }

    if (type == "tagged-union") {
        // The discriminator is a field name, or a list of field paths when the
        // union was split with a one_of discriminator.
        try {
            py::object disc = schema["discriminator"];
            if (py::isinstance<py::str>(disc)) {
                node->tagged_discriminator.push_back({disc.cast<std::string>()});
            } else if (PyCallable_Check(disc.ptr())) {
                node->tagged_discriminator_callable = disc;
            } else if (py::isinstance<py::list>(disc)) {
                for (auto item : disc.cast<py::list>()) {
                    if (py::isinstance<py::str>(item)) {
                        node->tagged_discriminator.push_back({item.cast<std::string>()});
                    } else if (py::isinstance<py::list>(item)) {
                        std::vector<std::string> path;
                        for (auto step : item.cast<py::list>()) {
                            path.push_back(py::str(step).cast<std::string>());
                        }
                        if (!path.empty()) node->tagged_discriminator.push_back(std::move(path));
                    }
                }
            }
        } catch (...) {}
        try {
            for (auto item : schema["choices"].cast<py::dict>()) {
                std::string tag = ser_tag_string(item.first);
                auto choice = build_ser(item.second.cast<py::dict>(), defs, memo);
                node->tagged[tag] = choice;
                node->tagged_left_to_right.push_back(std::move(choice));
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
        // core_schema.format_ser_schema emits "formatting_string". When the format
        // schema is a serialization override it sits in ser_dict, not in `schema`.
        try { node->format_str = ser_dict["formatting_string"].cast<std::string>(); } catch (...) {}
        if (node->format_str.empty()) { try { node->format_str = schema["formatting_string"].cast<std::string>(); } catch (...) {} }
        if (node->format_str.empty()) { try { node->format_str = schema["formatting"].cast<std::string>(); } catch (...) {} }
        try { node->when_used = ser_dict["when_used"].cast<std::string>(); } catch (...) {}
    }

    if (type == "function-plain" || type == "function-after" || type == "function-before" || type == "function-wrap") {
        // A "function-plain" that arrives as schema.type rather than
        // schema.serialization.type names no serializer at all: Rust's
        // FunctionPlainSerializerBuilder::build is AnySerializer::build and nothing else
        // (function.rs:65-76), because the plain-function meaning belongs to the
        // serialization slot -- shared.rs:181-191 is where such a node is really built.
        // So inference answers this value, under the run's pair, whatever the function or
        // its when_used says; the port kept an inert function node here and its python arm
        // handed the value back exactly as it came in, filtered or not.
        if (type == "function-plain" && !has_ser_dict) {
            node->type = "any";
            return node;
        }
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
                try { node->func_name = py::str(func.attr("__name__")).cast<std::string>(); }
                catch (...) { PyErr_Clear(); try { node->func_name = py::str(func.attr("__qualname__")).cast<std::string>(); } catch (...) { PyErr_Clear(); } }
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
        if (type != "function-plain" || has_ser_dict) {
            // Rust Function*Serializer::build takes the serializer underneath a
            // function wrapper from the serialization override whenever that
            // names a schema of its own, and falls back to the outer schema only
            // then.  A ValidateAs field says "serialize the annotated type",
            // which is not the collection the validator ran over: keeping the
            // validator's inner schema handed the validated int to a list
            // serializer.
            SerRef c;
            if (has_ser_dict) {
                try {
                    if (ser_dict.contains("schema") && !ser_dict["schema"].is_none())
                        c = build_ser(ser_dict["schema"].cast<py::dict>(), defs, memo);
                } catch (...) { PyErr_Clear(); }
            }
            if (!c && has_ser_dict) {
                // Rust builds a serializer from the *serialization* schema, so a
                // wrap serializer on a leaf that has no inner schema of its own
                // (is-instance) still knows what to serialize underneath.
                try { c = build_ser(ser_dict["schema"].cast<py::dict>(), defs, memo); } catch (...) {}
            }
            bool wants_outer = type != "function-plain" || node->when_used != "always";
            if (!c && has_ser_dict && wants_outer) {
                // An override that names no schema of its own sits ON a schema that
                // does, and Rust serializes that one underneath it: the core schema
                // copied without `serialization` (so the copy does not build this
                // same override again) and without `ref`, which the definitions
                // already know (function.rs:357, copy_outer_schema at :293).  Without
                // it a wrap serializer on a datetime has nothing to hand its handler.
                // A plain serializer only wants it when when_used can skip the call
                // entirely -- that is Rust's fallback_serializer (function.rs:124).
                // The copy is thrown away as soon as it is built, and the memo keys a
                // cached serializer by the schema dict's address, so this goes to
                // build_ser_impl: caching the copy would let the next field's copy
                // land on the same address and be handed this field's serializer.
                try {
                    py::dict outer = schema.attr("copy")().cast<py::dict>();
                    outer.attr("pop")("serialization", py::none());
                    outer.attr("pop")("ref", py::none());
                    c = build_ser_impl(outer, defs, memo);
                } catch (...) { PyErr_Clear(); }
            }
            if (!c) c = sub();
            // Rust builds the fallback serializer from the outer schema, so a
            // nullable schema whose serializer is overridden must still map
            // None to null rather than hand None to the inner serializer.
            if (c && (original_type == "nullable" || original_type == "nullable-union")) {
                auto wrap = std::make_shared<SerNode>();
                wrap->type = "nullable";
                wrap->children.push_back(c);
                c = wrap;
            }
            if (c) node->children.push_back(c);
        }
        try {
            if (has_ser_dict && ser_dict.contains("return_schema")) {
                node->return_ser = build_ser(ser_dict["return_schema"].cast<py::dict>(), defs, memo);
            } else if (has_ser_dict) {
                auto any = std::make_shared<SerNode>();
                any->type = "any";
                node->return_ser = any;
            }
        } catch (...) {}
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
                        if (py::isinstance<py::function>(eif) || py_hasattr(eif, "__call__")) {
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
                    node->fields[k] = build_ser(field_schema, defs, memo);
                    node->field_order.push_back(k);
                }
            }
        } catch (...) {}
        // A typed-dict with extra_behavior=allow keeps undeclared keys in its
        // own dict, so they are serialized as well; extras_schema (only legal
        // with extra_behavior=allow, as Rust enforces) names their serializer.
        if (type == "typed-dict") {
            std::string eb;
            try { if (schema.contains("extra_behavior")) eb = schema["extra_behavior"].cast<std::string>(); } catch (...) { PyErr_Clear(); }
            if (eb.empty() && schema.contains("config") && py::isinstance<py::dict>(schema["config"])) {
                try {
                    py::dict cfg = schema["config"].cast<py::dict>();
                    if (cfg.contains("extra_fields_behavior")) eb = cfg["extra_fields_behavior"].cast<std::string>();
                } catch (...) { PyErr_Clear(); }
            }
            if (eb == "allow") {
                node->typed_dict_allow_extra = true;
                if (schema.contains("extras_schema") && !schema["extras_schema"].is_none()) {
                    try { node->extra_ser = build_ser(schema["extras_schema"].cast<py::dict>(), defs, memo); } catch (...) { PyErr_Clear(); }
                }
            }
        }
        try {
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
                        if (py::isinstance<py::function>(eif) || py_hasattr(eif, "__call__")) {
                            node->field_exclude_if[prop] = eif;
                        }
                    }
                    try {
                        node->fields[prop] = build_ser(cf["return_schema"].cast<py::dict>(), defs, memo);
                    } catch (...) {
                        // If no return_schema, fall back to any
                        py::dict any_schema;
                        any_schema["type"] = "any";
                        node->fields[prop] = build_ser(any_schema, defs, memo);
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

    // Rust ModelSerializer::has_extra - the model's own config decides whether
    // __pydantic_extra__ is serialized. The flag lives on the model node and is
    // mirrored onto its fields node, which is what actually emits the extras.
    if (original_type == "model" || original_type == "model-fields" ||
        original_type == "typed-dict" || original_type == "dataclass") {
        bool extra_allowed = false;
        try {
            py::object cfg = schema["config"];
            if (py::isinstance<py::dict>(cfg)) {
                extra_allowed = cfg.cast<py::dict>()["extra_fields_behavior"].cast<std::string>() == "allow";
            }
        } catch (...) { PyErr_Clear(); }
        node->extra_allowed = extra_allowed;
        if (!node->children.empty() && node->children[0])
            node->children[0]->extra_allowed = extra_allowed;
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
            // "models ignore the parent config and always use the config from this model"
            // (model.rs:111-121, and dataclass.rs:103 likewise): a model rebuilds its whole
            // subtree with the config its own schema carries, so the mode below a model is
            // the one *that* model's config names -- and InfNanMode::default(), Null, when it
            // names none.  Every other serializer is built with whatever its parent was
            // handed (shared.rs:39, :62-73, :171-236) and is asked nothing of its own
            // `config` but polymorphic_serialization (shared.rs:252), which is why
            // `SchemaSerializer({'type':'float','config':{'ser_json_inf_nan':'constants'}}
            // .to_json(nan)` is b'null' on the wheel: a float node's config is never read.
            std::string inf_nan = "null";
            if (original_type == "model" || original_type == "dataclass") {
                try {
                    if (config.contains("ser_json_inf_nan"))
                        inf_nan = config["ser_json_inf_nan"].cast<std::string>();
                } catch (...) { PyErr_Clear(); }
            }
            std::unordered_set<SerRef> visited;
            std::function<void(SerRef)> set_config = [&](SerRef n) {
                if (!n) return;
                if (visited.count(n)) return;  // Cycle detection
                visited.insert(n);
                // Settled here or by a nearer model, never by a frame further up -- whose
                // walk runs later, this subtree having been built inside the recursion.
                if (original_type == "model" || original_type == "dataclass") {
                    if (!n->inf_nan_claimed) {
                        n->inf_nan_mode = inf_nan;
                        n->inf_nan_claimed = true;
                    }
                }
                if (config.contains("ser_json_bytes")) {
                    n->ser_json_bytes = config["ser_json_bytes"].cast<std::string>();
                }
                if (config.contains("ser_json_temporal") || config.contains("ser_json_timedelta")) {
                    n->ser_json_timedelta = timedelta_mode_from_config(config);
                }
                if (config.contains("ser_json_temporal")) {
                    n->ser_json_temporal = config["ser_json_temporal"].cast<std::string>();
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

    explicit PySchemaSerializer(const py::dict& schema, const std::optional<py::dict>& cfg = std::nullopt)
        : schema_(schema) {
        std::unordered_map<std::string, SerRef> defs;
        SerMemo memo;
        ser_ = build_ser(schema, defs, memo);
        if (cfg.has_value()) {
            try {
                py::dict c = *cfg;
                if (c.contains("serialize_by_alias")) {
                    serialize_by_alias_ = c["serialize_by_alias"].cast<bool>();
                }
                if (c.contains("ser_json_inf_nan")) {
                    inf_nan_mode_ = c["ser_json_inf_nan"].cast<std::string>();
                }
                // Propagate ser_json_* config to all serializer nodes (the config
                // is not embedded in the schema when a TypeAdapter is created with
                // an explicit config).
                std::unordered_set<SerRef> visited;
                std::function<void(SerRef)> set_config = [&](SerRef n) {
                    if (!n) return;
                    if (visited.count(n)) return;
                    visited.insert(n);
                    if (c.contains("ser_json_inf_nan") && !n->inf_nan_claimed) {
                        n->inf_nan_mode = c["ser_json_inf_nan"].cast<std::string>();
                    }
                    if (c.contains("ser_json_bytes")) {
                        n->ser_json_bytes = c["ser_json_bytes"].cast<std::string>();
                        ser_json_bytes_ = n->ser_json_bytes;
                    }
                    if (c.contains("ser_json_temporal") || c.contains("ser_json_timedelta")) {
                        n->ser_json_timedelta = timedelta_mode_from_config(c);
                        ser_json_timedelta_ = n->ser_json_timedelta;
                    }
                    if (c.contains("ser_json_temporal")) {
                        n->ser_json_temporal = c["ser_json_temporal"].cast<std::string>();
                        ser_json_temporal_ = n->ser_json_temporal;
                    }
                    for (auto& child : n->children) set_config(child);
                    for (auto& [k, v] : n->fields) set_config(v);
                    for (auto& [k, v] : n->tagged) set_config(v);
                };
                set_config(ser_);
            } catch (...) { PyErr_Clear(); }
        }
    }

    // Overload that accepts _use_prebuilt (unused but needed for pydantic API)
    explicit PySchemaSerializer(const py::dict& schema, const std::optional<py::dict>& cfg, bool)
        : schema_(schema) {
        std::unordered_map<std::string, SerRef> defs;
        SerMemo memo;
        ser_ = build_ser(schema, defs, memo);
        if (cfg.has_value()) {
            try {
                py::dict c = *cfg;
                if (c.contains("serialize_by_alias")) {
                    serialize_by_alias_ = c["serialize_by_alias"].cast<bool>();
                }
                if (c.contains("ser_json_inf_nan")) {
                    inf_nan_mode_ = c["ser_json_inf_nan"].cast<std::string>();
                }
                // Propagate ser_json_* config to all serializer nodes. The config
                // is not embedded in the schema when a TypeAdapter is created with
                // an explicit config, so build_ser's schema["config"] path misses it.
                std::unordered_set<SerRef> visited;
                std::function<void(SerRef)> set_config = [&](SerRef n) {
                    if (!n) return;
                    if (visited.count(n)) return;
                    visited.insert(n);
                    if (c.contains("ser_json_inf_nan") && !n->inf_nan_claimed) {
                        n->inf_nan_mode = c["ser_json_inf_nan"].cast<std::string>();
                    }
                    if (c.contains("ser_json_bytes")) {
                        n->ser_json_bytes = c["ser_json_bytes"].cast<std::string>();
                        ser_json_bytes_ = n->ser_json_bytes;
                    }
                    if (c.contains("ser_json_temporal") || c.contains("ser_json_timedelta")) {
                        n->ser_json_timedelta = timedelta_mode_from_config(c);
                        ser_json_timedelta_ = n->ser_json_timedelta;
                    }
                    if (c.contains("ser_json_temporal")) {
                        n->ser_json_temporal = c["ser_json_temporal"].cast<std::string>();
                        ser_json_temporal_ = n->ser_json_temporal;
                    }
                    for (auto& child : n->children) set_config(child);
                    for (auto& [k, v] : n->fields) set_config(v);
                    for (auto& [k, v] : n->tagged) set_config(v);
                };
                set_config(ser_);
            } catch (...) { PyErr_Clear(); }
        }
    }

    py::object to_python(const py::object& value, std::optional<std::string> mode,
                         std::optional<py::object> include, std::optional<py::object> exclude,
                         std::optional<bool> by_alias, bool exclude_unset, bool exclude_defaults, bool exc_none,
                         bool exclude_computed_fields, bool round_trip, py::object warnings, std::optional<py::object> fallback,
                         bool serialize_as_any, std::optional<bool> polymorphic, py::object context) const {
        (void)serialize_as_any;
        g_exclude_computed_fields = exclude_computed_fields;
        g_polymorphic_serialization = polymorphic;  // reset per call (None -> nullopt)
        SerCallExtraScope ser_scope;
        ser_apply_delegated_extra();
        g_ser_extra.mode = mode.has_value() ? *mode : std::string("python");
        g_ser_extra.round_trip = round_trip;
        g_ser_extra.context = context.is_none() ? py::none() : context;
        g_ser_extra.by_alias = by_alias ? py::cast(*by_alias) : py::none();
        g_ser_extra.exclude_unset = exclude_unset;
        g_ser_extra.exclude_defaults = exclude_defaults;
        g_ser_extra.exclude_none = exc_none;
        g_ser_extra.exclude_computed_fields = exclude_computed_fields;
        g_ser_extra.serialize_as_any = serialize_as_any;
        g_ser_extra.fallback = fallback && !fallback->is_none() ? *fallback : py::none();
        g_ser_extra.bytes_mode = ser_json_bytes_;
        g_ser_extra.timedelta_mode = ser_json_timedelta_;
        g_ser_extra.temporal_mode = ser_json_temporal_;
        g_ser_extra.inf_nan_mode = inf_nan_mode_;
        if (!ser_) throw std::runtime_error("Serializer not initialized");

        // Pass include/exclude through as-is (nested dict/set filters supported)
        py::object inc = (include && !include->is_none()) ? *include : py::none();
        py::object exc = (exclude && !exclude->is_none()) ? *exclude : py::none();
        bool use_alias = by_alias.value_or(serialize_by_alias_);

        // Rust resolves serialize_as_any at the outermost serializer call: the
        // value is serialized from its runtime type, not its declared type. The
        // nested model delegation in serialize_any_value_inner re-enters here
        // with the flag still set and must then use its own declared serializer
        // (Rust's serialize_no_infer), so the substitution happens once only.
        if (serialize_as_any && !g_ser_infer_applied) {
            g_ser_infer_applied = true;
            struct InferGuard { ~InferGuard() { g_ser_infer_applied = false; } } infer_guard;
            ser_warn_enter(warnings);
            try {
                py::object result = SerNode::serialize_any_value(
                    value, exc_none, round_trip, mode && *mode == "json", inc, exc);
                ser_warn_leave(false);
                return result;
            } catch (...) {
                ser_warn_leave(true);
                throw;
            }
        }

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

    // The JSON-string run: a failure that escapes it is renamed by Rust's
    // se_err_py_err, so this is where one is turned into
    // `Error serializing to JSON: ValueError: xxx`.  Errors the port raised itself
    // already carry the wording Rust gives them, including the prefix where the
    // serde boundary would have added one.
    py::bytes to_json(const py::object& value, std::optional<size_t> indent, std::optional<bool> ea,
                      std::optional<py::object> include, std::optional<py::object> exclude,
                      std::optional<bool> by_alias, bool exclude_unset, bool exclude_defaults, bool exc_none,
                      bool exclude_computed_fields, bool round_trip, py::object warnings, std::optional<py::object> fallback,
                      bool serialize_as_any, std::optional<bool> polymorphic, py::object context) const {
        SerJsonRun run;
        py::bytes encoded;
        try {
            // to_json_inner leaves the warning frame open when the encoding succeeded, so
            // the flush below is the only thing that closes it and it happens past this
            // mapping.
            encoded = to_json_inner(value, indent, ea, include, exclude, by_alias, exclude_unset, exclude_defaults,
                                    exc_none, exclude_computed_fields, round_trip, warnings, fallback,
                                    serialize_as_any, polymorphic, context);
        } catch (py::error_already_set& e) {
            std::string named;
            if (!ser_json_name_python_error(e, &named)) throw;
            throw PydanticSerializationError("Error serializing to JSON: " + named);
        }
        // Rust asks the warnings for their say after the encoded bytes have already come
        // back through the serde mapping (mod.rs:189 runs after to_json_bytes), so what a
        // warning filter turns into an error escapes a json run as itself -- a UserWarning,
        // not the serialization error the encoding step would have named it.  Flushing here
        // rather than inside the encoding is what keeps that difference; a run that failed
        // never reaches it, and its warnings are dropped the way Rust drops them.
        ser_warn_leave(false);  // may emit UserWarning / raise PydanticSerializationError
        return encoded;
    }

    py::bytes to_json_inner(const py::object& value, std::optional<size_t> indent, std::optional<bool> ea,
                      std::optional<py::object> include, std::optional<py::object> exclude,
                      std::optional<bool> by_alias, bool exclude_unset, bool exclude_defaults, bool exc_none,
                      bool exclude_computed_fields, bool round_trip, py::object warnings, std::optional<py::object> fallback,
                      bool serialize_as_any, std::optional<bool> polymorphic, py::object context) const {
        
        g_exclude_computed_fields = exclude_computed_fields;
        g_polymorphic_serialization = polymorphic;  // reset per call (None -> nullopt)
        SerCallExtraScope ser_scope;
        ser_apply_delegated_extra();
        g_ser_extra.mode = "json";
        g_ser_extra.round_trip = round_trip;
        g_ser_extra.context = context.is_none() ? py::none() : context;
        g_ser_extra.by_alias = by_alias ? py::cast(*by_alias) : py::none();
        g_ser_extra.exclude_unset = exclude_unset;
        g_ser_extra.exclude_defaults = exclude_defaults;
        g_ser_extra.exclude_none = exc_none;
        g_ser_extra.exclude_computed_fields = exclude_computed_fields;
        g_ser_extra.serialize_as_any = serialize_as_any;
        g_ser_extra.fallback = fallback && !fallback->is_none() ? *fallback : py::none();
        g_ser_extra.bytes_mode = ser_json_bytes_;
        g_ser_extra.timedelta_mode = ser_json_timedelta_;
        g_ser_extra.temporal_mode = ser_json_temporal_;
        g_ser_extra.inf_nan_mode = inf_nan_mode_;
        if (!ser_) throw std::runtime_error("Serializer not initialized");

        // Pass include/exclude through as-is (nested dict/set filters supported)
        py::object inc = (include && !include->is_none()) ? *include : py::none();
        py::object exc = (exclude && !exclude->is_none()) ? *exclude : py::none();
        bool use_alias = by_alias.value_or(serialize_by_alias_);

        bool e = ea.value_or(false);

        // Rust resolves serialize_as_any at the outermost serializer call (see
        // the to_python branch above for why this happens only once).
        if (serialize_as_any && !g_ser_infer_applied) {
            g_ser_infer_applied = true;
            struct InferGuard { ~InferGuard() { g_ser_infer_applied = false; } } infer_guard;
            ser_warn_enter(warnings);
            try {
                std::string json = SerNode::infer_json(value, e, -1, inc, exc);
                if (indent.has_value()) json = json_pretty_print(json, static_cast<int>(*indent));
                return py::bytes(std::move(json));  // the warning frame is left for the caller
            } catch (...) {
                ser_warn_leave(true);
                throw;
            }
        }
        ser_warn_enter(warnings);
        try {
            std::string json = ser_->to_json(value, e, -1, round_trip, inc, exc, use_alias, exclude_unset, exclude_defaults, exc_none, context);
            if (indent.has_value()) json = json_pretty_print(json, static_cast<int>(*indent));
            return py::bytes(std::move(json));  // the warning frame is left for the caller
        } catch (...) {
            ser_warn_leave(true);  // discard warnings collected before the error
            throw;
        }
    }

    std::string repr() const {
        if (!ser_) return "SchemaSerializer()";
        std::string out = "SchemaSerializer(serializer=" + ser_->type + ")";
        std::string body;
        ser_->append_repr(body, 1);
        if (!body.empty()) out += "\n" + body;
        return out;
    }

    const py::object& get_schema() const { return schema_; }

    // See SerNode::visit_refs.  The schema is kept only to pickle the
    // serializer, but it names every class and callable in the tree, so the
    // collector has to be told about it as well.
    void visit_refs(RefVisitor visit, void* arg) const {
        visit_ref(visit, arg, schema_);
        if (ser_) ser_->visit_refs(visit, arg);
    }

private:
    SerRef ser_;
    py::object schema_;  // Store schema for pickle support
    bool serialize_by_alias_ = false;  // config default for by_alias=None
    std::string ser_json_bytes_ = "utf8";
    std::string ser_json_timedelta_ = "iso8601";
    std::string ser_json_temporal_ = "iso8601";
    // SerializationConfig::from_config(config) for the config *this* serializer was built
    // with (mod.rs:76) -- the unmerged constructor argument, so a schema that embeds the
    // mode without the constructor naming it leaves the enum default, Null.
    std::string inf_nan_mode_ = "null";
};

// ---------------------------------------------------------------------------
// Standalone to_json
// ---------------------------------------------------------------------------
// Rust serializes with serde_json's pretty printer when an indent is given:
// newline plus indentation after every opening bracket, before the separator
// and before every closing bracket, ": " between a key and its value, and
// empty containers left on one line.
static std::string json_pretty_print(const std::string& compact, int indent) {
    std::string out;
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    auto newline = [&](int d) {
        out += '\n';
        if (d > 0) out.append(static_cast<size_t>(indent) * static_cast<size_t>(d), ' ');
    };
    for (size_t i = 0; i < compact.size(); ++i) {
        char c = compact[i];
        if (in_string) {
            out += c;
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out += c;
        } else if (c == '{' || c == '[') {
            char closing = (c == '{') ? '}' : ']';
            out += c;
            if (i + 1 < compact.size() && compact[i + 1] == closing) {
                out += compact[i + 1];
                ++i;
                continue;
            }
            newline(++depth);
        } else if (c == '}' || c == ']') {
            if (depth > 0) --depth;
            newline(depth);
            out += c;
        } else if (c == ',') {
            out += c;
            newline(depth);
        } else if (c == ':') {
            out += ": ";
        } else {
            out += c;
        }
    }
    return out;
}

static py::bytes to_json_fn(const py::object& value, std::optional<size_t> indent, std::optional<bool> ea,
    std::optional<py::object> include, std::optional<py::object> exclude, bool by_alias, bool, bool round_trip,
    std::string timedelta_mode, std::string temporal_mode, std::string bytes_mode,
    std::string inf_nan_mode, bool serialize_unknown,
    std::optional<py::object> fallback, bool, std::optional<bool> polymorphic, std::optional<py::object> context) {
    SerRef any = std::make_shared<SerNode>();
    any->type = "any";
    // Same naming as SchemaSerializer::to_json: this is a JSON-string run too.
    SerJsonRun run;
    // Rust builds an Extra for every entry point (shared.rs), so this does too: the
    // values inferred below have to be able to see this call's `fallback` and
    // `serialize_unknown`, and leaving the previous call's extra in place would have
    // handed them someone else's.
    SerCallExtraScope ser_scope;
    ser_apply_delegated_extra();
    // SerializationConfig::from_args, asked in Rust's own order: the temporal mode (which
    // may be the timedelta mode wearing its hat), then bytes, then inf_nan.  Whoever is
    // wrong first is the only one named, so the order is part of what is reproduced.
    std::string temporal = resolve_temporal_mode(timedelta_mode, temporal_mode);
    check_bytes_mode(bytes_mode);
    check_inf_nan_mode(inf_nan_mode);
    g_ser_extra.mode = "json";
    g_ser_extra.temporal_mode = temporal;
    g_ser_extra.timedelta_mode = timedelta_shape_for(temporal);
    g_ser_extra.inf_nan_mode = inf_nan_mode;
    // ...which now includes the caller's bytes mode: it was dropped, so every byte
    // string in the value was written as utf8 text however the caller asked for it.
    g_ser_extra.bytes_mode = bytes_mode;
    // mod.rs:244 hands this entry's own `by_alias` to the Extra, so a value below the
    // walk that brings its own serializer is asked for its aliases too -- naming the
    // keys of the value itself is nothing this run has to say about.
    g_ser_extra.by_alias = py::cast(by_alias);
    g_ser_extra.serialize_unknown = serialize_unknown;
    g_ser_extra.fallback = fallback && !fallback->is_none() ? *fallback : py::none();
    g_ser_extra.round_trip = round_trip;
    g_ser_extra.context = context && !context->is_none() ? *context : py::none();
    // Reset, not inherited: this entry's Extra carries its own polymorphic flag, and the
    // thread local otherwise still answers for whoever ran last.
    g_polymorphic_serialization = polymorphic;
    try {
        // mod.rs:256 puts this entry's own include/exclude on the state and :257-263 walks the
        // value through AnySerializer::get(), so the pair rides on to the infer walk -- the only
        // thing that can answer for a value that brings no serializer of its own.
        std::string json = any->to_json(value, ea.value_or(false), -1, round_trip,
            include && !include->is_none() ? *include : py::none(),
            exclude && !exclude->is_none() ? *exclude : py::none(), by_alias, false, false, false);
        if (indent.has_value()) json = json_pretty_print(json, static_cast<int>(*indent));
        return py::bytes(std::move(json));
    } catch (py::error_already_set& e) {
        std::string named;
        if (!ser_json_name_python_error(e, &named)) throw;
        throw PydanticSerializationError("Error serializing to JSON: " + named);
    }
}

// The `include` and `exclude` arguments of the jsonable entry points decide, one element
// at a time, whether the walk is shown that element at all -- Rust's AnyFilter
// (serializers/filter.rs:153-234 as the walk asks it at :267-285).  A filter is either a
// set of keys or a dict keyed by key whose values are filters again for what is below;
// `...` -- or `True`, kept for pydantic V1's sake (filter.rs:320) -- means "this one, with
// nothing below it filtered".  `__all__` is merged into whichever key is being asked about
// (filter.rs:329), which is why a filter is consulted per element instead of once per
// collection, and why an element that survives can still be filtered from underneath.
struct JsonableRun {
    std::string bytes_mode;
    std::string timedelta_mode;
    // The entry point's one resolved temporal mode, in its own vocabulary
    // (iso8601|seconds|milliseconds); timedelta_mode above is what it means for a delta.
    std::string temporal_mode;
    std::string inf_nan_mode;
    bool serialize_unknown = false;
    bool by_alias = true;
};

// filter.rs:320 -- both spellings of "no filter below here"
static bool ser_is_ellipsis_like(PyObject* v) {
    return v == Py_Ellipsis || (PyBool_Check(v) && v == Py_True);
}

static const py::object& ser_all_key() {
    static const py::object& all = held_python_object([] { return py::str("__all__"); });
    return all;
}

static const py::object& ser_ellipsis_obj() {
    static const py::object& e = held_python_object([] { return py::ellipsis(); });
    return e;
}

// filter.rs:352 -- a set is a dict that filters every one of its members; anything else is
// refused by name
static py::dict ser_as_dict(const py::object& v) {
    if (PyDict_Check(v.ptr())) return py::reinterpret_steal<py::dict>(PyDict_Copy(v.ptr()));
    if (PySet_Check(v.ptr())) {
        py::dict out;
        for (auto item : v.cast<py::set>())
            out[py::reinterpret_borrow<py::object>(item)] = ser_ellipsis_obj();
        return out;
    }
    throw py::type_error(
        "`include` and `exclude` must be of type `dict[str | int, <recursive> | ...] | set[str | int | ...]`");
}

// filter.rs:369 -- fold an `__all__` value into the entry kept for one key.  A key the
// entry already names wins where it says `...`; elsewhere the two are merged by recursing
// into the entry's own filter, which is what lets `{'a': {'b': ...}}` and `{'b': ...}` in
// `__all__` agree on dropping `b` from `a` and nothing else.
static py::dict ser_merge_dicts(const py::dict& item_dict, const py::object& all_value) {
    py::dict out = py::reinterpret_steal<py::dict>(PyDict_Copy(item_dict.ptr()));
    if (PyDict_Check(all_value.ptr())) {
        for (auto item : all_value.cast<py::dict>()) {
            py::object key = py::reinterpret_borrow<py::object>(item.first);
            py::object all = py::reinterpret_borrow<py::object>(item.second);
            PyObject* found = PyDict_GetItemWithError(out.ptr(), key.ptr());
            if (!found && PyErr_Occurred()) throw py::error_already_set();
            if (!found) {
                out[key] = all;
                continue;
            }
            py::object kept = py::reinterpret_borrow<py::object>(found);
            if (ser_is_ellipsis_like(kept.ptr())) continue;
            // :377 asks what the entry is before it asks what `__all__` says, so an entry
            // that is neither dict nor set is refused even when `__all__` is `...`
            py::dict kept_dict = ser_as_dict(kept);
            if (!ser_is_ellipsis_like(all.ptr())) out[key] = ser_merge_dicts(kept_dict, all);
        }
        return out;
    }
    if (PySet_Check(all_value.ptr())) {
        for (auto item : all_value.cast<py::set>()) {
            py::object key = py::reinterpret_borrow<py::object>(item);
            if (PyDict_Contains(out.ptr(), key.ptr()) != 1) out[key] = ser_ellipsis_obj();
        }
        return out;
    }
    throw py::type_error(
        "'__all__' key of `include` and `exclude` must be of type `dict[str | int, <recursive> | ...] | set[str | int | ...]`");
}

// filter.rs:329 -- the entry for one key, with the `__all__` entry folded in.  False is
// only for a key neither entry names: a key whose entry says `None` is present, and what
// it filters with is `None`, which is the difference between `{1: ...}` dropping element 1
// and `{1: None}` keeping it.
static bool ser_merge_all_value(const py::dict& d, const py::object& key, py::object* out) {
    py::object item, all;
    if (PyObject* found = PyDict_GetItemWithError(d.ptr(), key.ptr()))
        item = py::reinterpret_borrow<py::object>(found);
    else if (PyErr_Occurred())
        throw py::error_already_set();
    if (PyObject* found = PyDict_GetItemWithError(d.ptr(), ser_all_key().ptr()))
        all = py::reinterpret_borrow<py::object>(found);
    else if (PyErr_Occurred())
        throw py::error_already_set();
    if (item && all) {
        if (ser_is_ellipsis_like(item.ptr()) || ser_is_ellipsis_like(all.ptr()))
            *out = item;   // :338 either side spelling "everything" settles it as the entry
        else
            *out = ser_merge_dicts(ser_as_dict(item), all);
        return true;
    }
    if (item) {
        *out = item;
        return true;
    }
    if (all) {
        *out = all;
        return true;
    }
    return false;
}

// filter.rs:290 -- ask anything that is neither dict nor set whether it holds the key, the
// way a list or a str does.  False means there was no `__contains__` to ask, or asking
// raised, which is how `exclude='abc'` lands on "`exclude` argument must be a set or
// dict." rather than on the string's own TypeError.
static bool ser_check_contains(const py::object& o, const py::object& key, bool* found) {
    py::object contains;
    try {
        contains = py::getattr(o, "__contains__");
    } catch (const py::error_already_set&) {
        PyErr_Clear();
        return false;
    }
    int holds = -1;
    try {
        holds = PyObject_IsTrue(contains(key).ptr());
    } catch (const py::error_already_set&) {
        PyErr_Clear();
        return false;
    }
    if (holds < 0) throw py::error_already_set();
    if (holds == 1) {
        *found = true;
        return true;
    }
    // :296 the second question is not forgiven for failing; the first one is
    *found = PyObject_IsTrue(contains(ser_all_key()).ptr()) == 1;
    return true;
}

// filter.rs:20 -- a negative index is the same place counted from the end, which only
// means anything once the length is known.  Over an unsized iterable it is an error
// instead, and only once the walk reaches an element to ask about (:282 maps per element),
// so `to_jsonable_python(iter([]), exclude={-1})` never gets as far as complaining.
static py::object ser_map_negative_index(const py::object& v, const py::object* len) {
    if (len) {
        try {
            return v.attr("__mod__")(*len);
        } catch (const py::error_already_set&) {
            PyErr_Clear();   // :25 a key with no __mod__ is its own key
        }
        return v;
    }
    static const py::object& zero = held_python_object([] { return py::int_(0); });
    int negative = PyObject_RichCompareBool(v.ptr(), zero.ptr(), Py_LT);
    if (negative < 0)
        PyErr_Clear();   // :28 a key that will not answer is not a negative index
    else if (negative == 1)
        throw py::value_error("Negative indices cannot be used to exclude items on unsized iterables");
    return v;
}

// filter.rs:39 -- only a dict's keys or a set's members name positions, so only those are
// remapped.  A frozenset is left alone too: PySet_Check does not cover it, and it reaches
// the filter through `__contains__` instead, where a negative index simply matches nothing.
static py::object ser_map_negative_indices(const py::object& v, const py::object* len) {
    if (PyDict_Check(v.ptr())) {
        py::dict out;
        for (auto item : v.cast<py::dict>())
            out[ser_map_negative_index(py::reinterpret_borrow<py::object>(item.first), len)] =
                py::reinterpret_borrow<py::object>(item.second);
        return std::move(out);
    }
    if (PySet_Check(v.ptr())) {
        py::set out;
        for (auto item : v.cast<py::set>())
            out.add(ser_map_negative_index(py::reinterpret_borrow<py::object>(item), len));
        return std::move(out);
    }
    return v;   // :57 left as it is, for the filter to refuse or to ask `__contains__`
}

// filter.rs:153 -- the decision for one element, as this walk asks it (:310 and :314: the
// walk has no schema-level filter, so nothing is included or dropped by default and every
// answer comes from the two arguments).  False drops the element; otherwise `next_*` carry
// the filters its own contents are asked about, which are None unless a filter named
// something below it.  An argument that is present but None is a no-op either way (:162
// and :190), which is why "not passed" and "passed as None" need no distinction here.
static bool ser_any_filter(const py::object& key, const py::object& include, const py::object& exclude,
                           py::object* next_include, py::object* next_exclude) {
    py::object child_exclude;
    if (!exclude.is_none()) {
        if (PyDict_Check(exclude.ptr())) {
            py::object value;
            if (ser_merge_all_value(exclude.cast<py::dict>(), key, &value)) {
                if (ser_is_ellipsis_like(value.ptr())) return false;
                child_exclude = std::move(value);
            }
        } else if (PySet_Check(exclude.ptr())) {
            int holds = PySet_Contains(exclude.ptr(), key.ptr());
            if (holds < 0) throw py::error_already_set();
            if (holds == 1 || PySet_Contains(exclude.ptr(), ser_all_key().ptr()) == 1) return false;
        } else {
            bool holds = false;
            if (!ser_check_contains(exclude, key, &holds))
                throw py::type_error("`exclude` argument must be a set or dict.");
            if (holds) return false;
        }
    }

    py::object child_include;
    if (!include.is_none()) {
        if (PyDict_Check(include.ptr())) {
            py::object value;
            if (!ser_merge_all_value(include.cast<py::dict>(), key, &value))
                return false;   // :202 an include that exists and does not name this key drops it
            if (!ser_is_ellipsis_like(value.ptr())) child_include = std::move(value);
        } else if (PySet_Check(include.ptr())) {
            int holds = PySet_Contains(include.ptr(), key.ptr());
            if (holds < 0) throw py::error_already_set();
            if (holds != 1 && PySet_Contains(include.ptr(), ser_all_key().ptr()) != 1) return false;
        } else {
            bool holds = false;
            if (!ser_check_contains(include, key, &holds))
                throw py::type_error("`include` argument must be a set or dict.");
            if (!holds) return false;
        }
    }

    *next_include = child_include ? std::move(child_include) : py::none();
    *next_exclude = child_exclude ? std::move(child_exclude) : py::none();
    return true;
}

// Convert an arbitrary Python value to its JSON-compatible Python form
// (mirrors Rust's infer_jsonable_python used by to_jsonable_python).
// Raises pydantic_core::PydanticSerializationError (registered below as a
// Python exception) for values that have no JSON-compatible form.
// Rust's infer_to_python_known puts a recursion guard on every value it infers, not
// just on containers (infer.rs:57, recursion_guard.rs:32-42): meeting a value that is
// already on the way down is a reference cycle, and a walk that only gets absurdly deep
// is stopped the same way.  Rust raises ValueError for both (extra.rs:93-94).  The
// jsonable walk had neither, so it is also the only thing between a `fallback` that
// hands its own argument back -- which pydantic's own tests do -- and unbounded
// recursion that ends in a segfault rather than an error.
static std::vector<const void*>& jsonable_rec_stack() {
    static thread_local std::vector<const void*> stack;
    return stack;
}

static py::object infer_jsonable_python(const py::object& v, const JsonableRun& run,
                                        const py::object& include, const py::object& exclude) {
    std::vector<const void*>& st = jsonable_rec_stack();
    const void* p = v.ptr();
    for (const void* q : st) {
        if (q == p) throw py::value_error("Circular reference detected (id repeated)");
    }
    if (st.size() >= 255) throw py::value_error("Circular reference detected (depth exceeded)");
    st.push_back(p);
    struct StackPop {
        std::vector<const void*>& s;
        ~StackPop() { s.pop_back(); }
    } popper{st};
    if (v.is_none()) return py::none();
    // Head of the walk for the reason `infer_json` gives: Rust's exact-type table is matched
    // first and `is_dataclass` after it, so a dataclass is asked its fields before any arm can
    // answer it by the type it inherits -- `@dataclass class D(int)` becomes `{"a": 1}`, not the
    // int.  A value with a serializer of its own (a pydantic dataclass, whose aliases only that
    // serializer knows) is the one thing asked earlier still, and the delegation arm below has it.
    if (!PyType_Check(v.ptr()) && py_hasattr(v, "__dataclass_fields__")
        && !py_hasattr(v, "__pydantic_serializer__"))
        return infer_jsonable_python(ser_dataclass_pairs(v), run, include, exclude);
    // ObType::Enum is recognised by the metaclass of the value's own type being exactly
    // type(enum.Enum) (ob_type.rs:283 and :315), and Rust tests it ahead of the numbers
    // because its lookup matches exact type pointers, which an IntEnum member never
    // satisfies.  Reaching the isinstance tests below instead hands back the member
    // itself rather than the value it was built from, and a `_value_` attribute alone is
    // not an enum: Rust has no such duck and reports that object as unknown.
    bool enum_member = false;
    try {
        static const py::object& enum_metaclass = held_python_object(
            [] { return py::module_::import("enum").attr("Enum").attr("__class__"); });
        enum_member = Py_TYPE(Py_TYPE(v.ptr())) ==
                        reinterpret_cast<PyTypeObject*>(enum_metaclass.ptr());
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    if (enum_member)
        return infer_jsonable_python(py::getattr(v, "value"), run, include, exclude);
    if (PyBool_Check(v.ptr())) return v;
    if (PyLong_Check(v.ptr())) {
        // infer.rs:108 upcasts int subclasses -- "make sure subclasses of for example str
        // are upcast" -- so an IntFlag member or a hand-rolled int subclass leaves here
        // as the number it behaves like, not as an object that only serializes because
        // json.dumps happens to follow the int protocol.
        return ser_exact_int(v);
    }
    if (PyUnicode_Check(v.ptr())) {
        // Same for str (infer.rs:124), by the same buffer copy.
        return ser_exact_str(v);
    }
    if (PyFloat_Check(v.ptr())) {
        double d = v.cast<double>();
        if ((std::isnan(d) || std::isinf(d)) && run.inf_nan_mode == "null") return py::none();
        if (PyFloat_CheckExact(v.ptr())) return v;
        return py::float_(d);
    }
    if (py::isinstance<py::bytes>(v)) {
        std::string b = v.cast<std::string>();
        if (run.bytes_mode == "base64") return py::str(b64_encode_string(b));
        if (run.bytes_mode == "hex") {
            static const char* hex_chars = "0123456789abcdef";
            std::string enc;
            for (unsigned char c : b) {
                enc += hex_chars[c >> 4];
                enc += hex_chars[c & 0x0F];
            }
            return py::str(enc);
        }
        // utf8 is a strict decode, and the UnicodeDecodeError that escapes this entry
        // point is pyo3's, built from from_utf8's error (config.rs: bytes_to_string); the
        // port wrapped it in a PydanticSerializationError of its own making before.
        if (auto bad = utf8_bad(b)) raise_rust_decode_error(b, *bad);
        return py::reinterpret_steal<py::object>(
            PyUnicode_DecodeUTF8(b.data(), (Py_ssize_t)b.size(), nullptr));
    }
    // ObType::Bytes and ObType::Bytearray share one conversion (infer.rs:126-140): the
    // buffer is a byte string and follows the run's bytes mode.
    if (py::isinstance<py::bytearray>(v)) {
        py::object as_bytes = py::reinterpret_steal<py::object>(
            PyBytes_FromStringAndSize(PyByteArray_AS_STRING(v.ptr()), PyByteArray_GET_SIZE(v.ptr())));
        return infer_jsonable_python(as_bytes, run, include, exclude);
    }
    // ObType::Complex is Rust's own spelling of the number, not Python's repr.
    if (PyComplex_Check(v.ptr()))
        return py::str(complex_to_str_rust(PyComplex_RealAsDouble(v.ptr()),
                                           PyComplex_ImagAsDouble(v.ptr())));
    // The same table the two other json walks consult (infer.rs:122, :184-190, :191-194,
    // :220), and asked before the Unknown arm at the bottom of this function, which is where
    // Rust leaves it: a Decimal goes through pyo3's display rather than plain str(), a UUID is
    // rebuilt from its own int, and a Path or address whose str raises ends the run with the
    // value's own error instead of having it cleared here and being passed on as unknown.
    {
        py::object str_form;
        if (ser_infer_json_str(v, str_form)) return str_form;
    }
    // datetime/date/time expose isoformat() -- unless the run asked for timestamps, in
    // which case the same mode that moves a datetime moves a timedelta (infer.rs:169-182,
    // config.rs:161-186).
    if (py_hasattr(v, "isoformat")) {
        try {
            // The three checks below read through the datetime module's imported API
            // pointer, which is null until ensure_datetime_api has asked for it -- on the
            // first jsonable datetime a process ever serializes that is still this call.
            ensure_datetime_api();
            const char* t = PyDateTime_Check(v.ptr()) ? "datetime"
                          : PyDate_Check(v.ptr()) ? "date"
                          : PyTime_Check(v.ptr()) ? "time" : nullptr;
            if (t && run.temporal_mode != "iso8601") {
                py::object stamped;
                if (json_leaf_convert(t, v, run.bytes_mode, run.timedelta_mode, run.temporal_mode,
                                      stamped))
                    return stamped;
            }
            py::object iso = v.attr("isoformat")();
            std::string s = py::str(iso).cast<std::string>();
            if (s.size() >= 6 && s.substr(s.size() - 6) == "+00:00") s = s.substr(0, s.size() - 6) + "Z";
            return py::str(s);
        } catch (...) { PyErr_Clear(); }
    }
    // timedelta (duck-typed via its components)
    if (py_hasattr(v, "days") && py_hasattr(v, "seconds") && py_hasattr(v, "microseconds")
        && !py_hasattr(v, "isoformat")) {
        py::object out;
        if (json_leaf_convert("timedelta", v, "utf8", run.timedelta_mode, "iso8601", out)) return out;
    }
    // The exact-type walk above is only Rust's fast path: when it comes back empty,
    // lookup_ob_type runs fallback_isinstance (ob_type.rs:338), which asks the real
    // isinstance -- including isinstance(v, enum.Enum) at :390.  That is the pass that
    // picks up an enum whose metaclass is a *subclass* of EnumMeta, whose member the
    // metaclass-identity test above deliberately does not match, and it comes after the
    // numbers, so an IntFlag member is still the int Rust reads it as.
    try {
        static const py::object& enum_cls =
            held_python_object([] { return py::module_::import("enum").attr("Enum"); });
        if (py::isinstance(v, enum_cls))
            return infer_jsonable_python(py::getattr(v, "value"), run, include, exclude);
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    // Sets/tuples/lists/deques/iterators → arrays; dicts → objects (recursively).
    // Rust's table names its iterables (ObType::List/Tuple/Set/Frozenset/Deque, and
    // ObType::Generator for a value that is itself an iterator, ob_type.rs:294) and walks
    // tp_base to find them; it never asks an ABC.  Asking collections.abc.Sequence
    // instead handed the walk to any value that only declares itself a sequence: range
    // and array.array became arrays Rust reports as unknown, a memoryview became the ints
    // of its bytes, and a network object iterated its own address space --
    // to_jsonable_python(ipaddress.IPv6Network("::/64"), fallback=f) walks 2**64 hosts and
    // never comes back, where Rust turns the value into "::/64".
    bool deque_member = false;
    try {
        static const py::object& deque_cls =
            held_python_object([] { return py::module_::import("collections").attr("deque"); });
        deque_member = py::isinstance(v, deque_cls);
    } catch (const py::error_already_set&) { PyErr_Clear(); }
    // A set is walked by Rust's serialize_seq! (infer.rs:148-155), which hands every member
    // an empty filter: include/exclude do not reach inside a set, and its members are not
    // numbered, where a dict keys by them and a list counts them.
    if (py::isinstance<py::set>(v) || py::isinstance<py::frozenset>(v)) {
        py::list out;
        for (auto item : py::reinterpret_borrow<py::iterable>(v)) {
            out.append(infer_jsonable_python(py::reinterpret_borrow<py::object>(item), run, py::none(), py::none()));
        }
        return std::move(out);
    }
    if (py::isinstance<py::list>(v) || py::isinstance<py::tuple>(v) || deque_member || PyIter_Check(v.ptr())) {
        // A list, tuple or deque knows its length and an iterator does not (infer.rs:140-158
        // against :201-213), and that is the difference between a negative index in a filter
        // meaning the last element and meaning an error.
        const bool sized = py::isinstance<py::list>(v) || py::isinstance<py::tuple>(v) || deque_member;
        py::object length;
        if (sized) {
            Py_ssize_t n = PyObject_Length(v.ptr());
            if (n < 0) throw py::error_already_set();
            length = py::cast(n);
        }
        py::list out;
        Py_ssize_t index = 0;
        for (auto item : py::reinterpret_borrow<py::iterable>(v)) {
            py::object next_include = py::none(), next_exclude = py::none();
            if (!include.is_none() || !exclude.is_none()) {
                const py::object* len = sized ? &length : nullptr;
                py::object asked_include = include.is_none() ? py::none() : ser_map_negative_indices(include, len);
                py::object asked_exclude = exclude.is_none() ? py::none() : ser_map_negative_indices(exclude, len);
                if (!ser_any_filter(py::cast(index), asked_include, asked_exclude, &next_include, &next_exclude)) {
                    ++index;
                    continue;
                }
            }
            out.append(infer_jsonable_python(py::reinterpret_borrow<py::object>(item), run, next_include, next_exclude));
            ++index;
        }
        return std::move(out);
    }
    if (py::isinstance<py::dict>(v)) {
        py::dict out;
        for (auto item : v.cast<py::dict>()) {
            py::object key = py::reinterpret_borrow<py::object>(item.first);
            py::object next_include, next_exclude;
            // infer.rs:734 -- a dict is filtered by the key it holds, before that key is
            // converted into something JSON allows.
            if (!ser_any_filter(key, include, exclude, &next_include, &next_exclude)) continue;
            // The key is asked the JSON question even though the values are asked the python
            // one, because this walk runs in json mode (shared.rs:737-740): an int key leaves
            // as "1", not as 1.
            auto k = SerNode::infer_json_key(key, false);
            auto val = infer_jsonable_python(py::reinterpret_borrow<py::object>(item.second), run,
                                             next_include, next_exclude);
            out[k] = val;
        }
        return std::move(out);
    }
    // Model/dataclass instances: delegate to their serializer in json mode
    if (py_hasattr(v, "__pydantic_serializer__") && !PyType_Check(v.ptr())) {
        SerNestedCall nested;
        auto ser = py::getattr(v, "__pydantic_serializer__");
        py::dict kw = ser_extra_forwarded();
        kw["by_alias"] = run.by_alias;  // this entry point's own argument, not the thread local's
        // infer.rs:658-672 hands the model the state the walk is carrying, so the filters
        // that got this far are the filters its fields are asked about too.
        kw["include"] = include;
        kw["exclude"] = exclude;
        return ser.attr("to_python")(v, py::arg("mode") = "json", **kw);
    }
    // A dataclass was already asked its fields above, with the containers.  What is left here is
    // an object whose __dict__ Rust does not walk at all: refusing it is what keeps a
    // SimpleNamespace from becoming its fields and a plain instance from becoming its attributes,
    // and keeps to_jsonable_python(sys) from recursing through sys.modules -- every module in the
    // process -- until the stack gave out and took the interpreter with it.  A type object answers
    // the same probes and is refused too (ob_type.rs:421).
    if (py_hasattr(v, "__dict__") && !PyType_Check(v.ptr()) && SerNode::dict_inferable_object(v)) {
        py::object fields;
        try {
            fields = py::getattr(v, "__dict__");
            if (!py::isinstance<py::dict>(fields)) fields = py::object();
        } catch (...) { PyErr_Clear(); }
        if (fields.ptr())
            return infer_jsonable_python(fields, run, include, exclude);
    }
    // Rust infer_to_python ObType::Unknown (infer.rs:221-230), which is what
    // to_jsonable_python runs -- it is to_python with SerMode::Json (mod.rs:275).  The
    // run's fallback is asked first and its result re-inferred; serialize_unknown takes
    // str(), or a placeholder when str() raises; otherwise the failure names the value's
    // type.  The message here was this module's own invention: "Value is not JSON
    // serializable" appears nowhere in pydantic-core, so a caller matching on Rust's
    // wording got nothing.  A fallback that raises keeps its own error, unwrapped.
    if (g_ser_extra.fallback.ptr() && !g_ser_extra.fallback.is_none())
        return infer_jsonable_python(g_ser_extra.fallback(v), run, include, exclude);
    if (run.serialize_unknown) return py::str(ser_serialize_unknown(v));
    throw PydanticSerializationError("Unable to serialize unknown type: " + ser_safe_repr(py::type::of(v)));
}

static py::object to_jsonable_fn(const py::object& value, std::optional<py::object> include,
    std::optional<py::object> exclude, bool by_alias, bool exclude_none, bool round_trip,
    std::string timedelta_mode, std::string temporal_mode, std::string bytes_mode,
    std::string inf_nan_mode, bool serialize_unknown,
    std::optional<py::object> fallback, bool serialize_as_any, std::optional<bool> polymorphic, std::optional<py::object> context) {
    // The same from_args, in the same order, as the JSON entry point: nothing below is
    // asked before the modes have been read, so a refused call mutates no run state.
    std::string temporal = resolve_temporal_mode(timedelta_mode, temporal_mode);
    check_bytes_mode(bytes_mode);
    check_inf_nan_mode(inf_nan_mode);
    // Rust builds the same Extra for this entry that to_python(mode="json") gets, and
    // `fallback` rides on it (mod.rs:293-307), so the values inferred below read it
    // from there rather than from whatever the previous call left behind.
    SerCallExtraScope ser_scope;
    ser_apply_delegated_extra();
    g_ser_extra.mode = "json";
    g_ser_extra.by_alias = py::cast(by_alias);
    // :300 and :305 put both on the Extra, so they are this run's options even though only
    // a model below the walk is the one that reads them.
    g_ser_extra.exclude_none = exclude_none;
    g_ser_extra.serialize_as_any = serialize_as_any;
    g_ser_extra.serialize_unknown = serialize_unknown;
    g_ser_extra.fallback = fallback && !fallback->is_none() ? *fallback : py::none();
    g_ser_extra.bytes_mode = bytes_mode;
    g_ser_extra.temporal_mode = temporal;
    g_ser_extra.timedelta_mode = timedelta_shape_for(temporal);
    g_ser_extra.inf_nan_mode = inf_nan_mode;
    g_ser_extra.round_trip = round_trip;  // mod.rs:302 hands it to the Extra
    g_ser_extra.context = context && !context->is_none() ? *context : py::none();
    g_polymorphic_serialization = polymorphic;
    JsonableRun run{bytes_mode, timedelta_shape_for(temporal), temporal, inf_nan_mode, serialize_unknown, by_alias};
    return infer_jsonable_python(value, run, include ? *include : py::none(), exclude ? *exclude : py::none());
}

// pybind11 binds a class without tp_traverse, so everything its C++ members
// hold stays outside the cyclic collector's view.  Extract the bound object to
// walk it: for a single-inheritance binding pybind11 keeps the value pointer in
// the instance's simple layout.
template <typename T>
T* bound_cpp_object(PyObject* self) {
    auto* inst = reinterpret_cast<py::detail::instance*>(self);
    if (inst->simple_layout) return static_cast<T*>(inst->simple_value_holder[0]);
    // A holder too large for the inline slot keeps [val*][holder] behind a pointer
    // instead, which is where a py::cast(std::shared_ptr<T>) value ends up.
    return static_cast<T*>(inst->nonsimple.values_and_holders[0]);
}

extern "C" int visit_schema_validator(PyObject* self, visitproc traverse_fn, void* arg) {
    const gc_detail::TraversalRoot root;
    if (auto* validator = bound_cpp_object<SchemaValidator>(self)) {
        validator->visit_refs(traverse_fn, arg);
    }
    return 0;
}

// The lazy Python-mode view of an iterator field outlives the model_dump() call
// that made it, and it owns the iterator it walks: a caller that hangs on to it
// closes a cycle the collector could not see.
extern "C" int visit_serialization_iterator(PyObject* self, visitproc traverse_fn, void* arg) {
    const gc_detail::TraversalRoot root;
    if (auto* iter_obj = bound_cpp_object<SerializationIterator>(self)) {
        if (iter_obj->child) iter_obj->child->visit_refs(traverse_fn, arg);
        visit_ref(traverse_fn, arg, iter_obj->items);
        visit_ref(traverse_fn, arg, iter_obj->filter.include);
        visit_ref(traverse_fn, arg, iter_obj->filter.exclude);
        visit_ref(traverse_fn, arg, iter_obj->filter.folded_include);
        visit_ref(traverse_fn, arg, iter_obj->filter.folded_exclude);
        visit_ref(traverse_fn, arg, iter_obj->context);
    }
    return 0;
}

extern "C" int visit_schema_serializer(PyObject* self, visitproc traverse_fn, void* arg) {
    const gc_detail::TraversalRoot root;
    if (auto* serializer = bound_cpp_object<PySchemaSerializer>(self)) {
        serializer->visit_refs(traverse_fn, arg);
    }
    return 0;
}

// Opt a pybind11 type into the collector.  Call before any instance or Python
// subclass exists: PyType_GenericAlloc pairs the GC header it then adds with
// PyObject_GC_Del, and tp_free was inherited from object.
void enable_gc_traversal(py::handle type, traverseproc traverse) {
    PyTypeObject* ready = reinterpret_cast<PyTypeObject*>(type.ptr());
    ready->tp_flags |= Py_TPFLAGS_HAVE_GC;
    ready->tp_traverse = traverse;
    ready->tp_free = &PyObject_GC_Del;
}

// The length limits are measured on the whole iteration, so the error names the
// iterable that was handed in, not the item that closed it (Rust generator.rs:128-152).
static ValError iterator_length_error(ErrorType&& err, const py::object& input_obj) {
    return ValError::line_error(std::move(err), Location(), py::repr(input_obj).cast<std::string>());
}

// An iterator error is raised from __next__, long after validate_python returned, so
// nothing has seeded the module-level channel the wrapper reads ctx objects back
// from -- the hand-off validate_python does before it throws has to happen here too.
static void publish_error_ctx(const ValError& val_error) {
    try {
        py::module_ m = py::module_::import("__main__");
        py::list err_ctx_objs;
        if (val_error.has_line_errors()) {
            for (const auto& le : val_error.line_errors()) {
                py::dict ctx_d;
                for (const auto& [k, v] : le->error_type.context_objects()) {
                    ctx_d[py::str(k)] = v;
                }
                err_ctx_objs.append(std::move(ctx_d));
            }
        }
        m.attr("_last_error_ctx_objs") = std::move(err_ctx_objs);
    } catch (...) {}
}

// The json entry point throws from its own lambda, so the index-aligned input
// objects validate_python publishes before throwing have to be seeded here too
// -- a payload with no literal form (a bytearray) is otherwise lost.
static void publish_error_inputs(const ValError& val_error) {
    try {
        py::module_ m = py::module_::import("__main__");
        py::list err_input_objs;
        if (val_error.has_line_errors()) {
            for (const auto& le : val_error.line_errors()) {
                err_input_objs.append(le->raw_input_obj.ptr() ? py::object(le->raw_input_obj) : py::none());
            }
        }
        m.attr("_last_error_input_objs") = std::move(err_input_objs);
    } catch (...) {}
}

// validation_exception.rs:237 keys the documentation link on the pydantic that asked,
// major and minor only, and falls back to "latest" when pydantic cannot be asked.  Rust
// settles that in a OnceLock the first time an error is rendered; settling it once here
// too keeps errors() and str(ValidationError) on one prefix for the life of the process.
static const std::string& error_url_prefix() {
    static const std::string prefix = [] {
        std::string version;
        try {
            version = py::module_::import("pydantic").attr("VERSION").cast<std::string>();
        } catch (...) {
            version.clear();
        }
        if (version.empty()) {
            return std::string("https://errors.pydantic.dev/latest/v/");
        }
        const size_t first = version.find('.');
        const size_t second = first == std::string::npos
                                  ? std::string::npos
                                  : version.find('.', first + 1);
        return "https://errors.pydantic.dev/" + version.substr(0, second) + "/v/";
    }();
    return prefix;
}

// validation_exception.rs:208 answers the environment question once and keeps the
// answer, together with the deprecation warning that comes from the legacy variable, in
// a process-wide OnceLock.  The renderer asks instead of reading the environment
// itself:  re-importing pydantic re-executes the Python module that renders the text,
// and a gate kept there would be handed a fresh answer -- and a fresh warning -- long
// after Rust's had settled.  The answer is settled the first time it is asked, which in
// practice is the first error rendered, so the deprecation warning lands on that error's
// own render rather than on loading the extension.
static bool include_url_env_value() {
    static const bool settled = [] {
        // var_os rather than var: only whether the variable exists, and whether its value
        // is empty, are asked.  A warning the caller turned into an error must not fail the
        // rendering, so the raised state is dropped the way Rust drops the warning result.
        if (const char* omitted = getenv("PYDANTIC_ERRORS_OMIT_URL")) {
            if (PyErr_WarnEx(PyExc_DeprecationWarning,
                             "PYDANTIC_ERRORS_OMIT_URL is deprecated, use "
                             "PYDANTIC_ERRORS_INCLUDE_URL instead", 1) == -1) {
                PyErr_Clear();
            }
            return omitted[0] == '\0';
        }
        const char* included = getenv("PYDANTIC_ERRORS_INCLUDE_URL");
        if (included == nullptr) {
            return true;
        }
        std::string value(included);
        for (char& c : value) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        return value == "1" || value == "true";
    }();
    return settled;
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
        .def_readonly("include", &PySerializationInfo::include)
        .def_readonly("exclude", &PySerializationInfo::exclude)
        .def_readonly("by_alias", &PySerializationInfo::by_alias)
        .def_readonly("exclude_unset", &PySerializationInfo::exclude_unset)
        .def_readonly("exclude_defaults", &PySerializationInfo::exclude_defaults)
        .def_readonly("exclude_none", &PySerializationInfo::exclude_none)
        .def_readonly("exclude_computed_fields", &PySerializationInfo::exclude_computed_fields)
        .def_readonly("serialize_as_any", &PySerializationInfo::serialize_as_any)
        .def("mode_is_json", [](const PySerializationInfo& self) { return self.mode == "json"; })
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
                        d["url"] = error_url_prefix() + err.type;
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
    m.def("_include_url_env", []() { return include_url_env_value(); },
        "Whether str(ValidationError) carries the documentation link");
    m.def("_error_url_prefix", []() -> std::string { return error_url_prefix(); },
        "Prefix of the documentation page for an error type");

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
        std::optional<size_t> min_length;
        std::optional<size_t> max_length;
        py::object input_obj;
    };
    py::class_<LazyValidator>(m, "_LazyValidator")
        .def(py::init([](py::object source, py::function validate_fn, std::string schema_repr,
                         std::optional<size_t> min_length, std::optional<size_t> max_length,
                         py::object input) {
            auto self = std::make_unique<LazyValidator>();
            self->source = std::move(source);
            self->validate_fn = std::move(validate_fn);
            self->schema_repr = std::move(schema_repr);
            self->min_length = min_length;
            self->max_length = max_length;
            self->input_obj = std::move(input);
            return self;
        }),
             py::arg("source"), py::arg("validate_fn"), py::arg("schema_repr"),
             py::arg("min_length") = std::nullopt, py::arg("max_length") = std::nullopt,
             py::arg("input") = py::none())
        .def("__iter__", [](LazyValidator& self) -> py::object {
            return py::cast(self);
        })
        .def("__next__", [](LazyValidator& self) -> py::object {
            py::object item;
            try {
                item = py::module_::import("builtins").attr("next")(self.source);
            } catch (py::error_already_set& e) {
                if (!e.matches(PyExc_StopIteration))
                    throw;
                // The source ran out: too few items to satisfy the lower bound.
                if (self.min_length && self.index < *self.min_length) {
                    ErrorType err(ErrorType::Kind::TooShort);
                    err.context()["field_type"] = "Generator";
                    err.set_ctx_object("min_length", std::to_string(*self.min_length),
                                       py::int_(static_cast<int>(*self.min_length)));
                    err.set_ctx_object("actual_length", std::to_string(self.index),
                                       py::int_(static_cast<int>(self.index)));
                    ValError val_error = iterator_length_error(std::move(err), self.input_obj);
                    publish_error_ctx(val_error);
                    throw ValidationError("ValidatorIterator", InputType::Python, val_error, self.input_obj);
                }
                throw;
            }
            size_t idx = self.index++;
            // Too many: the item was pulled but never validated, so the count that
            // overshoots is reported as "more" rather than a number.
            if (self.max_length && idx >= *self.max_length) {
                ErrorType err(ErrorType::Kind::TooLong);
                err.context()["field_type"] = "Generator";
                err.set_ctx_object("max_length", std::to_string(*self.max_length),
                                   py::int_(static_cast<int>(*self.max_length)));
                err.set_ctx_object("actual_length", "more", py::none());
                ValError val_error = iterator_length_error(std::move(err), self.input_obj);
                publish_error_ctx(val_error);
                throw ValidationError("ValidatorIterator", InputType::Python, val_error, self.input_obj);
            }
            return self.validate_fn(item, idx);
        })
        .def("__repr__", [](LazyValidator& self) -> std::string {
            return "ValidatorIterator(index=" + std::to_string(self.index) +
                   ", schema=Some(" + self.schema_repr + "))";
        });
    // The validation-side twin holds the source iterator it pulls from, so it
    // takes part in the same cycle.  (No visit_ function: the struct is local
    // to this scope, so the callback has to be here too.)
    enable_gc_traversal(m.attr("_LazyValidator"), +[](PyObject* obj, visitproc traverse_fn, void* arg) -> int {
        if (auto* lazy = bound_cpp_object<LazyValidator>(obj)) {
            visit_ref(traverse_fn, arg, lazy->source);
            visit_ref(traverse_fn, arg, lazy->validate_fn);
            visit_ref(traverse_fn, arg, lazy->input_obj);
        }
        return 0;
    });

    // SerializationIterator — the lazy Python-mode view of an iterator field
    py::class_<SerializationIterator, std::shared_ptr<SerializationIterator>>(m, "SerializationIterator")
        .def_property_readonly("index", [](const SerializationIterator& self) { return self.index; })
        .def("__iter__", [](SerializationIterator& self) -> py::object {
            return py::cast(self);
        })
        .def("__next__", [](SerializationIterator& self) -> py::object {
            for (;;) {
                py::object item;
                try {
                    item = py::module_::import("builtins").attr("next")(self.items);
                } catch (py::stop_iteration&) {
                    throw;
                }
                // generator.rs:184-185 asks the filter with the position and only then advances
                // it, so a consult that refuses leaves the position unconsumed.
                auto next = self.filter.ask(static_cast<py::ssize_t>(self.index));
                self.index += 1;
                if (next.omit) continue;
                SerWarnViewScope scope(self.warn_enabled, self.warn_as_error, self.warn_seed);
                py::object value = self.infer_items
                    ? SerNode::serialize_any_value(
                          py::reinterpret_borrow<py::object>(item), self.exc_none, self.round_trip,
                          false, next.include, next.exclude)
                    : self.child->to_python(
                          SerNode::check_item_type(self.child, py::reinterpret_borrow<py::object>(item)),
                          false, self.exc_none, self.round_trip, next.include, next.exclude,
                          self.by_alias, self.exclude_unset, self.exclude_defaults, self.context);
                scope.emit();
                return value;
            }
        })
        .def("__repr__", [](SerializationIterator& self) {
            return "SerializationIterator(index=" + std::to_string(self.index) +
                   ", iterator=" + py::repr(self.items).cast<std::string>() + ")";
        });

    enable_gc_traversal(m.attr("SerializationIterator"), &visit_serialization_iterator);

    // SchemaValidator
    py::class_<SchemaValidator>(m, "SchemaValidator")
        // Primary constructor: takes Python dict directly (Rust-style)
        .def(py::init([](const py::dict& schema, const py::dict& config) {
            return std::make_unique<SchemaValidator>(schema, config);
        }), py::arg("schema"), py::arg("config") = py::none())
        // Legacy constructor with bool flag for backwards compat
        .def(py::init([](const py::dict& schema, const py::dict& config, bool use_prebuilt) {
            return std::make_unique<SchemaValidator>(schema, config, use_prebuilt);
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
            if (!self_instance.is_none() && py_hasattr(self_instance, "__dict__")) {
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
                        if (!py_hasattr(self_instance, "__pydantic_private__")) {
                            force_setattr(self_instance, py::str("__pydantic_private__"), py::none());
                        }
                        force_setattr(self_instance, py::str("__pydantic_extra__"), py::none());
                        // Rust ModelValidator: a PydanticUndefined input means the
                        // schema default filled the root, so no field is set.
                        py::object root_undefined = pydantic_undefined_obj();
                        bool root_from_default =
                            root_undefined.ptr() && input.ptr() == root_undefined.ptr();
                        force_setattr(self_instance, py::str("__pydantic_fields_set__"),
                                      root_from_default
                                          ? py::set()
                                          : py::set(py::make_tuple(py::str("root"))));
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
                            if (py_hasattr(self_instance, "__post_init__")) {
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
                        if (!py_hasattr(self_instance, "__pydantic_private__")) {
                            force_setattr(self_instance, py::str("__pydantic_private__"), py::none());
                        }
                        force_setattr(self_instance, py::str("__pydantic_extra__"),
                            extra_fields.is_none() ? py::none() : extra_fields);
                        force_setattr(self_instance, py::str("__pydantic_fields_set__"), fields_set);
                    } else if (!foreign_return && py_hasattr(validated, "__dict__")) {
                        // validated is a model instance (e.g. from FunctionAfterValidator)
                        // Copy its __dict__ to self_instance
                        py::dict d = self_instance.attr("__dict__");
                        py::dict validated_dict = validated.attr("__dict__");
                        for (auto item : validated_dict) {
                            d[item.first] = item.second;
                        }
                        // Copy pydantic slot attributes
                        if (py_hasattr(validated, "__pydantic_extra__")) {
                            force_setattr(self_instance, py::str("__pydantic_extra__"), validated.attr("__pydantic_extra__"));
                        }
                        if (py_hasattr(validated, "__pydantic_fields_set__")) {
                            force_setattr(self_instance, py::str("__pydantic_fields_set__"), validated.attr("__pydantic_fields_set__"));
                        }
                        if (!py_hasattr(self_instance, "__pydantic_private__")) {
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
                        if (py_hasattr(self_instance, "__post_init__")) {
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

            return validated;
        }, py::arg("object"), py::arg("strict") = py::none(), py::arg("context") = py::none(), py::arg("self_instance") = py::none(),
             py::arg("extra") = py::none(), py::arg("from_attributes") = py::none(), py::arg("by_alias") = py::none(), py::arg("by_name") = py::none(),
             py::arg("allow_partial") = py::none())
        .def("validate_json", [](SchemaValidator& self, const py::object& jd, py::object strict, py::object context, py::object extra,
                                  py::object allow_partial, py::object by_alias, py::object by_name) {
            // Rust reads the payload through validate_bytes, so anything that is not a
            // string/bytes/bytearray becomes a json_type error carrying the original
            // object rather than a binding cast failure.
            if (!py::isinstance<py::str>(jd) && !py::isinstance<py::bytes>(jd) &&
                !py::isinstance<py::bytearray>(jd)) {
                ErrorType error_type(ErrorType::Kind::JsonType);
                Location location;
                ValError val_error = ValError::line_error(error_type, location, std::string());
                throw ValidationError(self.title(), InputType::Json, val_error, jd, false);
            }
            std::string js = py::isinstance<py::bytes>(jd) ? jd.cast<std::string>() : jd.cast<std::string>();
            // Parse allow_partial to decide whether to use partial JSON parsing.
            bool partial_active = false;
            bool partial_trailing_strings = false;
            if (!allow_partial.is_none()) {
                if (py::isinstance<py::bool_>(allow_partial)) {
                    partial_active = allow_partial.cast<bool>();
                } else if (py::isinstance<py::str>(allow_partial)) {
                    std::string s = allow_partial.cast<std::string>();
                    partial_active = (s == "on" || s == "trailing-strings");
                    partial_trailing_strings = (s == "trailing-strings");
                }
            }
            // Parse JSON to Python object first, then validate as Python
            // This ensures proper type coercion (e.g., "Infinity" string -> float inf)
            py::object py_input;
            try {
                py_input = partial_active
                                 ? json_to_pyobj_partial(js, partial_trailing_strings
                                                            ? PartialMode::TrailingStrings
                                                            : PartialMode::On)
                                 : json_to_pyobj(js);
            } catch (const py::error_already_set& e) {
                // Malformed JSON: convert JSONDecodeError to a ValidationError
                // with json_invalid type (Rust: validate_json throws ValidationError
                // for malformed JSON, not a raw JSONDecodeError).  The wording has
                // to be jiter's, not the json module's.
                auto diagnosis = json_diagnose_parse_error(js);
                std::string err_msg = diagnosis.value_or(py::str(e.value()).cast<std::string>());
                ErrorType error_type(ErrorType::Kind::JsonInvalid, "error", err_msg);
                Location location;
                // Rust hands the payload object itself to the error (map_json_err takes
                // `input`), so what gets rendered has to be its repr: the undecoded text
                // '[1,\n2,\n3,]' is read back through literal_eval as the list [1, 2, 3].
                ValError val_error =
                    ValError::line_error(error_type, location, py::repr(jd).cast<std::string>(), jd);
                publish_error_inputs(val_error);
                throw ValidationError(self.title(), InputType::Json, val_error, jd);
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
            return self.validate_python_object(py_input, pyobj_to_bool(strict), extra_opt, std::nullopt, context, /*coerce_strings=*/false, py::none(), std::nullopt, std::nullopt, partial_mode, InputType::Json);
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
        .def_property_readonly("validator_display_name", &SchemaValidator::validator_display_name)
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

    enable_gc_traversal(m.attr("SchemaValidator"), &visit_schema_validator);

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

    enable_gc_traversal(m.attr("SchemaSerializer"), &visit_schema_serializer);

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

    // Rust's from_json: parse JSON into Python objects, optionally repairing
    // truncated input (jiter's allow_partial).  A parse failure is a ValueError
    // carrying jiter's own wording, not the json module's.
    m.def("from_json",
          [](py::object data, bool allow_inf_nan, py::object cache_strings,
             py::object allow_partial) -> py::object {
              (void)allow_inf_nan;
              (void)cache_strings;
              std::string text;
              if (PyUnicode_Check(data.ptr())) {
                  text = py::cast<std::string>(data);
              } else if (PyBytes_Check(data.ptr())) {
                  text.assign(PyBytes_AS_STRING(data.ptr()), PyBytes_GET_SIZE(data.ptr()));
              } else if (PyByteArray_Check(data.ptr())) {
                  text.assign(PyByteArray_AS_STRING(data.ptr()), PyByteArray_GET_SIZE(data.ptr()));
              } else {
                  throw py::type_error("Expected bytes, bytearray or str");
              }
              bool partial = false;
              bool partial_trailing = false;
              if (!allow_partial.is_none()) {
                  if (py::isinstance<py::bool_>(allow_partial)) {
                      partial = allow_partial.cast<bool>();
                  } else if (py::isinstance<py::str>(allow_partial)) {
                      std::string mode = allow_partial.cast<std::string>();
                      partial = (mode == "on" || mode == "trailing-strings");
                      partial_trailing = (mode == "trailing-strings");
                  }
              }
              try {
                  return partial
                           ? json_to_pyobj_partial(text, partial_trailing
                                                        ? PartialMode::TrailingStrings
                                                        : PartialMode::On)
                           : json_to_pyobj(text);
              } catch (const py::error_already_set& e) {
                  std::string python_message = py::str(e.value()).cast<std::string>();
                  auto diagnosis = json_diagnose_parse_error(text);
                  throw py::value_error(diagnosis.value_or(python_message));
              }
          },
          py::arg("data"), py::kw_only(), py::arg("allow_inf_nan") = true,
          py::arg("cache_strings") = "all", py::arg("allow_partial") = false);

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
