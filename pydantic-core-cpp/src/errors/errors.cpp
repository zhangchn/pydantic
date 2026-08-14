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
                                const ValError& val_error)
    : title_(title), input_type_(input_type) {
    build_errors_from_val_error(val_error);

    std::ostringstream oss;
    oss << error_count() << " validation error(s) for " << title_ << "\n";
    for (const auto& err : errors_) {
        if (!err.loc.empty()) {
            oss << err.loc << "\n  ";
        }
        oss << err.msg
            << " [type=" << err.type
            << ", input_value=" << err.input
            << "]\n";
    }
    // Structured error details (with ctx and typed loc items) — the Python
    // wrapper uses this to build errors() since register_exception instances
    // cannot be cast back to the C++ type.
    oss << "__PYDANTIC_ERRORS__:" << errors_to_json() << "\n";
    what_message_ = oss.str();
}

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
        for (const auto& [k, v] : err.ctx) {
            if (!cfirst) oss << ",";
            cfirst = false;
            oss << json_quote(k) << ":" << json_quote(v);
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
            if (!line_err->raw_input_obj.is_none()) {
                details.has_raw_input = true;
                details.raw_input_obj = line_err->raw_input_obj;
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