#include "pydantic_core/error_types.hpp"
#include <unordered_map>

namespace pydantic_core {

std::string ErrorType::type_name() const {
    static const std::unordered_map<Kind, std::string> names = {
        {Kind::NoneRequired, "none_required"},
        {Kind::BoolType, "bool_type"},
        {Kind::IntType, "int_type"},
        {Kind::FloatType, "float_type"},
        {Kind::StringType, "string_type"},
        {Kind::BytesType, "bytes_type"},
        {Kind::DictType, "dict_type"},
        {Kind::ListType, "list_type"},
        {Kind::StringTooShort, "string_too_short"},
        {Kind::StringTooLong, "string_too_long"},
        {Kind::IntGreaterThan, "int_greater_than"},
        {Kind::IntLessThan, "int_less_than"},
        {Kind::FieldRequired, "field_required"},
        {Kind::Missing, "missing"},
        {Kind::CustomError, "custom_error"}
    };
    auto it = names.find(kind_);
    return it != names.end() ? it->second : "unknown_error";
}

std::string ErrorType::message_template() const {
    static const std::unordered_map<Kind, std::string> templates = {
        {Kind::NoneRequired, "Input should be None"},
        {Kind::BoolType, "Input should be a valid boolean"},
        {Kind::IntType, "Input should be a valid integer"},
        {Kind::FloatType, "Input should be a valid number"},
        {Kind::StringType, "Input should be a valid string"},
        {Kind::BytesType, "Input should be a valid bytes"},
        {Kind::DictType, "Input should be a valid dictionary"},
        {Kind::ListType, "Input should be a valid list"},
        {Kind::StringTooShort, "String should have at least {min_length} characters"},
        {Kind::StringTooLong, "String should have at most {max_length} characters"},
        {Kind::IntGreaterThan, "Input should be greater than {gt}"},
        {Kind::IntLessThan, "Input should be less than {lt}"},
        {Kind::FieldRequired, "Field required"},
        {Kind::Missing, "Missing field: {field_name}"},
        {Kind::CustomError, "{message}"}
    };
    auto it = templates.find(kind_);
    return it != templates.end() ? it->second : "Validation error";
}

std::string ErrorType::message() const {
    std::string result = message_template();
    for (const auto& [key, value] : context_) {
        std::string placeholder = "{" + key + "}";
        size_t pos = result.find(placeholder);
        while (pos != std::string::npos) {
            result.replace(pos, placeholder.length(), value);
            pos = result.find(placeholder);
        }
    }
    return result;
}

} // namespace pydantic_core