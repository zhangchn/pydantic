#include "pydantic_core/errors.hpp"
#include <sstream>

namespace pydantic_core {

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
        oss << err.loc << "\n  " << err.msg << "\n";
    }
    what_message_ = oss.str();
}

void ValidationError::build_errors_from_val_error(const ValError& val_error) {
    if (val_error.has_line_errors()) {
        for (const auto& line_err : val_error.line_errors()) {
            ErrorDetails details;
            details.type = line_err->error_type.type_name();
            details.loc = line_err->location.to_string();
            details.msg = line_err->error_type.message();
            details.input = line_err->input_value;
            details.ctx = line_err->error_type.context();
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