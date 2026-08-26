#include "pydantic_core/errors.hpp"
#include <sstream>

#ifdef HAS_PYBIND11
namespace py = pybind11;
#endif

namespace pydantic_core {

namespace {

std::string json_quote(const std::string& s) {
    std::ostringstream oss;
    oss << '"';
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default: oss << c;
        }
    }
    oss << '"';
    return oss.str();
}

} // namespace

std::string ValLineError::message() const {
    std::string loc = location.to_string();
    if (!loc.empty()) {
        return "[" + loc + "] " + error_type.message() + " [input=" + input_value + "]";
    }
    return error_type.message() + " [input=" + input_value + "]";
}

ValidationError::ValidationError(const std::string& title, InputType input_type,
                                const ValError& val_error, bool hide_input)
    : title_(title), input_type_(input_type), hide_input_(hide_input) {
    build_errors_from_val_error(val_error);

    std::ostringstream oss;
    oss << error_count() << " validation error(s) for " << title_ << "\n";
    for (const auto& err : errors_) {
        if (!err.loc.empty()) {
            oss << err.loc << "\n  ";
        }
        oss << err.msg
            << " [type=" << err.type;
        // Rust: when hide_input is set, omit input_value/input_type entirely
        if (!hide_input_) {
            oss << ", input_value=" << err.input;
        }
        oss << "]\n";
    }
    // Structured error details (with ctx and typed loc items) — the Python
    // wrapper uses this to build errors() since register_exception instances
    // cannot be cast back to the C++ type.
    oss << "__PYDANTIC_ERRORS__:" << errors_to_json() << "\n";
    what_message_ = oss.str();
}

#ifdef HAS_PYBIND11
ValidationError::ValidationError(const std::string& title, InputType input_type,
                                const ValError& val_error, py::object raw_input,
                                bool hide_input)
    : ValidationError(title, input_type, val_error, hide_input) {
    // Rust parallel: as_val_error(input) — attach the raw Python input to
    // line errors that don't carry their own input object, so errors()
    // can report the actual Python value instead of its string repr.
    for (auto& details : errors_) {
        if (!details.has_raw_input && raw_input.ptr()) {
            details.has_raw_input = true;
            details.raw_input_obj = raw_input;
        }
    }
}
#endif

std::string ValidationError::errors_to_json() const {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < errors_.size(); ++i) {
        if (i > 0) oss << ",";
        const auto& err = errors_[i];
        oss << "{\"type\":" << json_quote(err.type)
            << ",\"loc\":[";
        if (!err.loc_items.empty()) {
            bool first = true;
            for (const auto& item : err.loc_items) {
                if (!first) oss << ",";
                first = false;
                if (auto* idx = std::get_if<int64_t>(&item)) {
                    oss << *idx;
                } else {
                    oss << json_quote(std::get<std::string>(item));
                }
            }
        }
        oss << "]"
            << ",\"msg\":" << json_quote(err.msg)
            << ",\"input\":" << json_quote(err.input)
            << ",\"ctx\":{";
        bool cfirst = true;
        // Exceptions cannot be JSON-serialized, so emit a marker that the
        // Python wrapper resolves against the stored error objects. When the
        // marker is emitted it replaces the message-string "error" entry.
#ifdef HAS_PYBIND11
        bool emit_error_ref = err.has_raw_error && err.raw_error_obj.ptr();
#else
        bool emit_error_ref = false;
#endif
        for (const auto& [k, v] : err.ctx) {
            if (emit_error_ref && k == "error") continue;
            if (!cfirst) oss << ",";
            cfirst = false;
            oss << json_quote(k) << ":" << json_quote(v);
        }
        if (emit_error_ref) {
            if (!cfirst) oss << ",";
            cfirst = false;
            oss << json_quote("error") << ":" << json_quote("__PYDANTIC_EXC_REF__");
        }
        oss << "}}";
    }
    oss << "]";
    return oss.str();
}
void ValidationError::build_errors_from_val_error(const ValError& val_error) {
    if (val_error.has_line_errors()) {
        for (const auto& line_err : val_error.line_errors()) {
            ErrorDetails details;
            details.type = line_err->error_type.type_name();
            details.loc = line_err->location.to_string();
            details.loc_items = line_err->location.items;
            details.msg = line_err->error_type.message();
            details.input = line_err->input_value;
#ifdef HAS_PYBIND11
            // Preserve original Python object for accurate serialization (Rust parallel)
            // Note: default-constructed py::object has a null handle, so check ptr()
            // rather than is_none() (which is false for a null handle).
            if (line_err->raw_input_obj.ptr()) {
                details.has_raw_input = true;
                details.raw_input_obj = line_err->raw_input_obj;
            }
            // Preserve the Python exception for ctx['error'] (value_error/assertion_error).
            // Note: default-constructed py::object has a null handle, so check ptr()
            // rather than is_none() (which is false for a null handle).
            if (line_err->raw_error_obj.ptr()) {
                details.has_raw_error = true;
                details.raw_error_obj = line_err->raw_error_obj;
            }
#endif
            // Filter out internal pluralization keys from context
            auto ctx = line_err->error_type.context();
            ctx.erase("s");
            details.ctx = ctx;
            errors_.push_back(details);
        }
    }
}

std::string ValidationError::error_count_message() const {
    return std::to_string(error_count()) + " validation error(s) for " + title_;
}

std::string ValidationError::to_json_string() const {
    std::ostringstream oss;
    oss << "{\"title\": \"" << title_ << "\", \"error_count\": " << error_count() << "}";
    return oss.str();
}

} // namespace pydantic_core