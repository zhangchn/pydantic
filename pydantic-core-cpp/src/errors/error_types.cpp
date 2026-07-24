#include "pydantic_core/error_types.hpp"
#include <unordered_map>

namespace pydantic_core {

std::string ErrorType::type_name() const {
    static const std::unordered_map<Kind, std::string> names = {
        // Type errors
        {Kind::NoneRequired, "none_required"},
        {Kind::NoneType, "none_type"},
        {Kind::BoolType, "bool_type"},
        {Kind::IntType, "int_type"},
        {Kind::FloatType, "float_type"},
        {Kind::StringType, "string_type"},
        {Kind::BytesType, "bytes_type"},
        {Kind::DictType, "dict_type"},
        {Kind::ListType, "list_type"},
        {Kind::TupleType, "tuple_type"},
        {Kind::SetType, "set_type"},
        {Kind::FrozenSetType, "frozenset_type"},
        {Kind::UnionType, "union_type"},
        {Kind::DateType, "date_type"},
        {Kind::TimeType, "time_type"},
        {Kind::DateTimeType, "datetime_type"},

        // Parsing errors
        {Kind::BoolParsing, "bool_parsing"},
        {Kind::IntParsing, "int_parsing"},
        {Kind::FloatParsing, "float_parsing"},
        
        // Integer constraint errors
        {Kind::IntMultipleOf, "int_multiple_of"},
        {Kind::IntGreaterThan, "int_greater_than"},
        {Kind::IntLessThan, "int_less_than"},
        {Kind::IntGreaterThanEqual, "int_greater_than_equal"},
        {Kind::IntLessThanEqual, "int_less_than_equal"},
        
        // Float constraint errors
        {Kind::FloatMultipleOf, "float_multiple_of"},
        {Kind::FloatGreaterThan, "float_greater_than"},
        {Kind::FloatLessThan, "float_less_than"},
        {Kind::FloatGreaterThanEqual, "float_greater_than_equal"},
        {Kind::FloatLessThanEqual, "float_less_than_equal"},
        
        // String constraint errors
        {Kind::StringTooShort, "string_too_short"},
        {Kind::StringTooLong, "string_too_long"},
        {Kind::StringPatternMismatch, "string_pattern_mismatch"},
        
        // Bytes constraint errors
        {Kind::BytesTooShort, "bytes_too_short"},
        {Kind::BytesTooLong, "bytes_too_long"},
        
        // List/Set constraint errors
        {Kind::ListTooShort, "list_too_short"},
        {Kind::ListTooLong, "list_too_long"},
        {Kind::SetTooShort, "set_too_short"},
        {Kind::SetTooLong, "set_too_long"},
        
        // Dict constraint errors
        {Kind::DictTooShort, "dict_too_short"},
        {Kind::DictTooLong, "dict_too_long"},
        
        // Tuple errors
        {Kind::TupleLengthMismatch, "tuple_length_mismatch"},
        
        // Literal errors
        {Kind::LiteralMismatch, "literal_mismatch"},
        
        // Field errors
        {Kind::DictKeysMissing, "dict_keys_missing"},
        {Kind::DictKeysUnexpected, "dict_keys_unexpected"},
        {Kind::FieldRequired, "field_required"},
        {Kind::Missing, "missing"},
        {Kind::ExtraForbidden, "extra_forbidden"},
        
        // Other errors
        {Kind::JsonInvalid, "json_invalid"},
        {Kind::CustomError, "custom_error"},
        {Kind::RecursionError, "recursion_error"},
        
        // Generic constraint errors
        {Kind::GreaterThan, "greater_than"},
        {Kind::LessThan, "less_than"},
        {Kind::GreaterThanEqual, "greater_than_equal"},
        {Kind::LessThanEqual, "less_than_equal"},
        {Kind::MultipleOf, "multiple_of"},
        {Kind::FiniteNumber, "finite_number"},
        {Kind::TooShort, "too_short"},
        {Kind::TooLong, "too_long"},
        {Kind::StringNotAscii, "string_not_ascii"}
    };
    auto it = names.find(kind_);
    return it != names.end() ? it->second : "unknown_error";
}

std::string ErrorType::message_template() const {
    static const std::unordered_map<Kind, std::string> templates = {
        // Type errors
        {Kind::NoneRequired, "Input should be None"},
        {Kind::NoneType, "Input should be None"},
        {Kind::BoolType, "Input should be a valid boolean"},
        {Kind::IntType, "Input should be a valid integer"},
        {Kind::FloatType, "Input should be a valid number"},
        {Kind::StringType, "Input should be a valid string"},
        {Kind::BytesType, "Input should be a valid bytes"},
        {Kind::DictType, "Input should be a valid dictionary or mapping"},
        {Kind::ListType, "Input should be a valid list or array"},
        {Kind::TupleType, "Input should be a valid tuple"},
        {Kind::SetType, "Input should be a valid set"},
        {Kind::FrozenSetType, "Input should be a valid frozenset"},
        {Kind::UnionType, "Input should match one of the expected types"},
        {Kind::DateType, "Input should be a valid date in YYYY-MM-DD format"},
        {Kind::TimeType, "Input should be a valid time in HH:MM:SS format"},
        {Kind::DateTimeType, "Input should be a valid datetime"},

        // Parsing errors
        {Kind::BoolParsing, "Input should be a valid boolean, unable to interpret input"},
        {Kind::IntParsing, "Input should be a valid integer, unable to parse string as an integer"},
        {Kind::FloatParsing, "Input should be a valid number, unable to parse string as a number"},
        
        // Integer constraint errors
        {Kind::IntMultipleOf, "Input should be a multiple of {value}"},
        {Kind::IntGreaterThan, "Input should be greater than {value}"},
        {Kind::IntLessThan, "Input should be less than {value}"},
        {Kind::IntGreaterThanEqual, "Input should be greater than or equal to {value}"},
        {Kind::IntLessThanEqual, "Input should be less than or equal to {value}"},
        
        // Float constraint errors
        {Kind::FloatMultipleOf, "Input should be a multiple of {value}"},
        {Kind::FloatGreaterThan, "Input should be greater than {value}"},
        {Kind::FloatLessThan, "Input should be less than {value}"},
        {Kind::FloatGreaterThanEqual, "Input should be greater than or equal to {value}"},
        {Kind::FloatLessThanEqual, "Input should be less than or equal to {value}"},
        
        // String constraint errors
        {Kind::StringTooShort, "String should have at least {value} characters"},
        {Kind::StringTooLong, "String should have at most {value} characters"},
        {Kind::StringPatternMismatch, "String should match pattern"},
        
        // Bytes constraint errors
        {Kind::BytesTooShort, "Bytes should have at least {value} bytes"},
        {Kind::BytesTooLong, "Bytes should have at most {value} bytes"},
        
        // List/Set constraint errors
        {Kind::ListTooShort, "List should have at least {value} items"},
        {Kind::ListTooLong, "List should have at most {value} items"},
        {Kind::SetTooShort, "Set should have at least {value} items"},
        {Kind::SetTooLong, "Set should have at most {value} items"},
        
        // Dict constraint errors
        {Kind::DictTooShort, "Dict should have at least {value} items"},
        {Kind::DictTooLong, "Dict should have at most {value} items"},
        
        // Tuple errors
        {Kind::TupleLengthMismatch, "Tuple should have {expected} items, got {actual}"},
        
        // Literal errors
        {Kind::LiteralMismatch, "Input should match one of the allowed values"},
        
        // Field errors
        {Kind::DictKeysMissing, "Missing required keys"},
        {Kind::DictKeysUnexpected, "Unexpected keys provided"},
        {Kind::FieldRequired, "Field required"},
        {Kind::Missing, "Missing field"},
        {Kind::ExtraForbidden, "Extra inputs are not permitted"},
        
        // Other errors
        {Kind::JsonInvalid, "Invalid JSON"},
        {Kind::CustomError, "{message}"},
        {Kind::RecursionError, "Recursion depth exceeded"},
        
        // Generic constraint errors
        {Kind::GreaterThan, "Input should be greater than {value}"},
        {Kind::LessThan, "Input should be less than {value}"},
        {Kind::GreaterThanEqual, "Input should be greater than or equal to {value}"},
        {Kind::LessThanEqual, "Input should be less than or equal to {value}"},
        {Kind::MultipleOf, "Input should be a multiple of {value}"},
        {Kind::FiniteNumber, "Input should be a finite number"},
        {Kind::TooShort, "Input should have at least {value} items"},
        {Kind::TooLong, "Input should have at most {value} items"},
        {Kind::StringNotAscii, "Input should be ASCII"}
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