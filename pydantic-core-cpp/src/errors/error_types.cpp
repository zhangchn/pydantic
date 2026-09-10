#include "pydantic_core/error_types.hpp"
#include <unordered_map>

namespace pydantic_core {

std::string ErrorType::type_name() const {
    if (!custom_type_name_.empty()) return custom_type_name_;
    static const std::unordered_map<Kind, std::string> names = {
        // Type errors
        {Kind::NoneRequired, "none_required"},
        {Kind::NoneType, "none_type"},
        {Kind::BoolType, "bool_type"},
        {Kind::IntType, "int_type"},
        {Kind::IntFromFloat, "int_from_float"},
        {Kind::FloatType, "float_type"},
        {Kind::StringType, "string_type"},
        {Kind::BytesType, "bytes_type"},
        {Kind::DictType, "dict_type"},
        {Kind::ListType, "list_type"},
        {Kind::TupleType, "tuple_type"},
        {Kind::SetType, "set_type"},
        {Kind::FrozenSetType, "frozen_set_type"},
        {Kind::FrozenField, "frozen_field"},
        {Kind::FrozenInstance, "frozen_instance"},
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
        
        // List/Set constraint errors - use generic too_short/too_long
        {Kind::ListTooShort, "too_short"},
        {Kind::ListTooLong, "too_long"},
        {Kind::SetTooShort, "too_short"},
        {Kind::SetTooLong, "too_long"},

        // Dict constraint errors - use generic too_short/too_long
        {Kind::DictTooShort, "too_short"},
        {Kind::DictTooLong, "too_long"},
        
        // Tuple errors
        {Kind::TupleLengthMismatch, "tuple_length_mismatch"},
        
        // Literal errors
        {Kind::LiteralMismatch, "literal_mismatch"},
        {Kind::LiteralError, "literal_error"},
        {Kind::EnumError, "enum"},
        
        // Collection / string / bytes errors (Rust names)
        {Kind::IterableType, "iterable_type"},
        {Kind::IterationError, "iteration_error"},
        {Kind::MappingType, "mapping_type"},
        {Kind::StringSubType, "string_sub_type"},
        {Kind::StringUnicode, "string_unicode"},
        {Kind::BytesInvalidEncoding, "bytes_invalid_encoding"},
        {Kind::SetItemNotHashable, "set_item_not_hashable"},
        {Kind::MissingSentinelError, "missing_sentinel_error"},
        {Kind::JsonType, "json_type"},
        {Kind::IntParsingSize, "int_parsing_size"},
        {Kind::ComplexType, "complex_type"},
        {Kind::ComplexStrParsing, "complex_str_parsing"},

        // Field errors
        {Kind::DictKeysMissing, "dict_keys_missing"},
        {Kind::DictKeysUnexpected, "dict_keys_unexpected"},
        {Kind::FieldRequired, "field_required"},
        {Kind::Missing, "missing"},
        {Kind::ExtraForbidden, "extra_forbidden"},
        {Kind::InvalidKey, "invalid_key"},
        {Kind::NoSuchAttribute, "no_such_attribute"},
        {Kind::ModelAttributesType, "model_attributes_type"},
        {Kind::GetAttributeError, "get_attribute_error"},
        {Kind::NeedsPythonObject, "needs_python_object"},
        {Kind::DataclassExactType, "dataclass_exact_type"},
        {Kind::DefaultFactoryNotCalled, "default_factory_not_called"},
        {Kind::UnionTagInvalid, "union_tag_invalid"},
        {Kind::UnionTagNotFound, "union_tag_not_found"},

        // Arguments errors
        {Kind::ArgumentsType, "arguments_type"},
        {Kind::MissingArgument, "missing_argument"},
        {Kind::MissingKeywordOnlyArgument, "missing_keyword_only_argument"},
        {Kind::MissingPositionalOnlyArgument, "missing_positional_only_argument"},
        {Kind::UnexpectedPositionalArgument, "unexpected_positional_argument"},
        {Kind::UnexpectedKeywordArgument, "unexpected_keyword_argument"},
        {Kind::MultipleArgumentValues, "multiple_argument_values"},

        // Date/Time errors
        {Kind::DateType, "date_type"},
        {Kind::DateParsing, "date_parsing"},
        {Kind::DateFromDatetimeParsing, "date_from_datetime_parsing"},
        {Kind::DateFromDatetimeInexact, "date_from_datetime_inexact"},
        {Kind::DatePast, "date_past"},
        {Kind::DateFuture, "date_future"},
        {Kind::TimeType, "time_type"},
        {Kind::TimeParsing, "time_parsing"},
        {Kind::DateTimeType, "datetime_type"},
        {Kind::DateTimeParsing, "datetime_parsing"},
        {Kind::DatetimeFromDateParsing, "datetime_from_date_parsing"},
        {Kind::DatetimeObjectInvalid, "datetime_object_invalid"},
        {Kind::DatetimePast, "datetime_past"},
        {Kind::DatetimeFuture, "datetime_future"},
        {Kind::TimezoneAware, "timezone_aware"},
        {Kind::TimezoneNaive, "timezone_naive"},
        {Kind::TimezoneOffset, "timezone_offset"},
        {Kind::TimedeltaType, "time_delta_type"},
        {Kind::TimedeltaParsing, "time_delta_parsing"},

        // URL errors
        {Kind::UrlType, "url_type"},
        {Kind::UrlParsing, "url_parsing"},
        {Kind::UrlSyntaxViolation, "url_syntax_violation"},
        {Kind::UrlTooLong, "url_too_long"},
        {Kind::UrlScheme, "url_scheme"},
        {Kind::UrlHost, "url_host"},

        // UUID errors
        {Kind::UuidType, "uuid_type"},
        {Kind::UuidParsing, "uuid_parsing"},
        {Kind::UuidVersion, "uuid_version"},

        // Decimal errors
        {Kind::DecimalType, "decimal_type"},
        {Kind::DecimalParsing, "decimal_parsing"},
        {Kind::DecimalMaxDigits, "decimal_max_digits"},
        {Kind::DecimalMaxPlaces, "decimal_max_places"},
        {Kind::DecimalWholeDigits, "decimal_whole_digits"},

        // Type checking errors
        {Kind::IsInstanceType, "is_instance_of"},
        {Kind::IsSubclassType, "is_subclass_of"},
        {Kind::CallableType, "callable_type"},
        {Kind::ModelType, "model_type"},
        {Kind::DataclassType, "dataclass_type"},

        // Other errors
        {Kind::JsonInvalid, "json_invalid"},
        {Kind::InvalidJsonValue, "invalid-json-value"},
        {Kind::CustomError, "custom_error"},
        {Kind::RecursionError, "recursion_error"},
        {Kind::RecursionLoop, "recursion_loop"},
        {Kind::ValueError, "value_error"},
        {Kind::AssertionError, "assertion_error"},
        
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
        {Kind::IntFromFloat, "Input should be a valid integer, got a number with a fractional part"},
        {Kind::FloatType, "Input should be a valid number"},
        {Kind::StringType, "Input should be a valid string"},
        {Kind::BytesType, "Input should be a valid bytes"},
        {Kind::DictType, "Input should be a valid dictionary or mapping"},
        {Kind::ListType, "Input should be a valid list or array"},
        {Kind::TupleType, "Input should be a valid tuple"},
        {Kind::SetType, "Input should be a valid set"},
        {Kind::FrozenSetType, "Input should be a valid frozenset"},
        {Kind::FrozenField, "Field is frozen"},
        {Kind::FrozenInstance, "Instance is frozen"},
        {Kind::UnionType, "Input should match one of the expected types"},
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
        {Kind::StringTooShort, "String should have at least {min_length} character{s}"},
        {Kind::StringTooLong, "String should have at most {max_length} character{s}"},
        {Kind::StringPatternMismatch, "String should match pattern '{pattern}'"},
        
        // Bytes constraint errors
        {Kind::BytesTooShort, "Data should have at least {min_length} bytes"},
        {Kind::BytesTooLong, "Data should have at most {max_length} bytes"},
        
        // List/Set constraint errors - use generic too_short/too_long messages
        {Kind::ListTooShort, "{field_type} should have at least {min_length} items after validation, not {actual_length}"},
        {Kind::ListTooLong, "{field_type} should have at most {max_length} items after validation, not {actual_length}"},
        {Kind::SetTooShort, "{field_type} should have at least {min_length} items after validation, not {actual_length}"},
        {Kind::SetTooLong, "{field_type} should have at most {max_length} items after validation, not {actual_length}"},

        // Dict constraint errors - use generic too_short/too_long messages
        {Kind::DictTooShort, "{field_type} should have at least {min_length} items after validation, not {actual_length}"},
        {Kind::DictTooLong, "{field_type} should have at most {max_length} items after validation, not {actual_length}"},
        
        // Tuple errors
        {Kind::TupleLengthMismatch, "Tuple should have {expected} items, got {actual}"},
        
        // Literal errors
        {Kind::LiteralMismatch, "Input should match one of the allowed values"},
        {Kind::LiteralError, "Input should be {expected}"},
        {Kind::EnumError, "Input should be {expected}"},

        // Collection / string / bytes errors (Rust messages)
        {Kind::IterableType, "Input should be iterable"},
        {Kind::IterationError, "Error iterating over object, error: {error}"},
        {Kind::MappingType, "Input should be a valid mapping, error: {error}"},
        {Kind::StringSubType, "Input should be a string, not an instance of a subclass of str"},
        {Kind::StringUnicode, "Input should be a valid string, unable to parse raw data as a unicode string"},
        {Kind::BytesInvalidEncoding, "Data should be valid {encoding}: {encoding_error}"},
        {Kind::SetItemNotHashable, "Set items should be hashable"},
        {Kind::MissingSentinelError, "Input should be the 'MISSING' sentinel"},
        {Kind::JsonType, "JSON input should be string, bytes or bytearray"},
        {Kind::IntParsingSize, "Unable to parse input string as an integer, exceeded maximum size"},
        {Kind::ComplexType, "Input should be a valid python complex object, a number, or a valid complex string following the rules at https://docs.python.org/3/library/functions.html#complex"},
        {Kind::ComplexStrParsing, "Input should be a valid complex string following the rules at https://docs.python.org/3/library/functions.html#complex"},

        // Field errors
        {Kind::DictKeysMissing, "Missing required keys"},
        {Kind::DictKeysUnexpected, "Unexpected keys provided"},
        {Kind::FieldRequired, "Field required"},
        {Kind::Missing, "Missing field"},
        {Kind::ExtraForbidden, "Extra inputs are not permitted"},
        {Kind::InvalidKey, "Keys should be strings"},
        {Kind::NoSuchAttribute, "Object has no attribute '{attribute}'"},
        {Kind::ModelAttributesType, "Input should be a valid dictionary or object to extract fields from"},
        {Kind::GetAttributeError, "Error extracting attribute: {error}"},
        {Kind::NeedsPythonObject, "Cannot check `{method_name}` when validating from json, use a JsonOrPython validator instead"},
        {Kind::DataclassExactType, "Input should be an instance of {class_name}"},
        {Kind::DefaultFactoryNotCalled, "The default factory uses validated data, but at least one validation error occurred"},
        {Kind::UnionTagInvalid, "Input tag '{tag}' found using {discriminator} does not match any of the expected tags: {expected_tags}"},
        {Kind::UnionTagNotFound, "Unable to extract tag using discriminator {discriminator}"},

        // Arguments errors
        {Kind::ArgumentsType, "Arguments must be a tuple, list or a dictionary"},
        {Kind::MissingArgument, "Missing required argument"},
        {Kind::MissingKeywordOnlyArgument, "Missing required keyword only argument"},
        {Kind::MissingPositionalOnlyArgument, "Missing required positional only argument"},
        {Kind::UnexpectedPositionalArgument, "Unexpected positional argument"},
        {Kind::UnexpectedKeywordArgument, "Unexpected keyword argument"},
        {Kind::MultipleArgumentValues, "Got multiple values for argument"},
        
        // Other errors
        {Kind::JsonInvalid, "Invalid JSON: {error}"},
        {Kind::InvalidJsonValue, "input was not a valid JSON value"},
        {Kind::CustomError, "{message}"},
        {Kind::RecursionError, "Recursion depth exceeded"},
        {Kind::RecursionLoop, "Recursion error - cyclic reference detected"},
        {Kind::ValueError, "Value error, {error}"},
        {Kind::AssertionError, "Assertion failed, {error}"},
        
        // Generic constraint errors
        {Kind::GreaterThan, "Input should be greater than {gt}"},
        {Kind::LessThan, "Input should be less than {lt}"},
        {Kind::GreaterThanEqual, "Input should be greater than or equal to {ge}"},
        {Kind::LessThanEqual, "Input should be less than or equal to {le}"},
        {Kind::MultipleOf, "Input should be a multiple of {multiple_of}"},
        {Kind::FiniteNumber, "Input should be a finite number"},
        {Kind::TooShort, "{field_type} should have at least {min_length} items after validation, not {actual_length}"},
        {Kind::TooLong, "{field_type} should have at most {max_length} items after validation, not {actual_length}"},
        {Kind::StringNotAscii, "Input should be ASCII"},

        // Date/Time errors
        {Kind::DateType, "Input should be a valid date"},
        {Kind::DateParsing, "Input should be a valid date in the format YYYY-MM-DD, {error}"},
        {Kind::DateFromDatetimeParsing, "Input should be a valid date or datetime, {error}"},
        {Kind::DateFromDatetimeInexact, "Datetimes provided to dates should have zero time - e.g. be exact dates"},
        {Kind::DatePast, "Date should be in the past"},
        {Kind::DateFuture, "Date should be in the future"},
        {Kind::TimeType, "Input should be a valid time"},
        {Kind::TimeParsing, "Input should be in a valid time format, {error}"},
        {Kind::DateTimeType, "Input should be a valid datetime"},
        {Kind::DateTimeParsing, "Input should be a valid datetime, {error}"},
        {Kind::DatetimeFromDateParsing, "Input should be a valid datetime or date, {error}"},
        {Kind::DatetimeObjectInvalid, "Invalid datetime object"},
        {Kind::DatetimePast, "Input should be in the past"},
        {Kind::DatetimeFuture, "Input should be in the future"},
        {Kind::TimezoneAware, "Input should have timezone info"},
        {Kind::TimezoneNaive, "Input should not have timezone info"},
        {Kind::TimezoneOffset, "Datetime should have timezone offset {tz_expected}, got {tz_actual}"},
        {Kind::TimedeltaType, "Input should be a valid timedelta"},
        {Kind::TimedeltaParsing, "Input should be a valid timedelta, {error}"},

        // URL errors (Rust messages)
        {Kind::UrlType, "URL input should be a string or URL"},
        {Kind::UrlParsing, "Input should be a valid URL, {error}"},
        {Kind::UrlSyntaxViolation, "Input violated strict URL syntax rules, {error}"},
        {Kind::UrlTooLong, "URL should have at most {max_length} character{s}"},
        {Kind::UrlScheme, "URL scheme should be {expected_schemes}"},
        {Kind::UrlHost, "URL host should be valid, {error}"},

        // UUID errors (Rust messages)
        {Kind::UuidType, "UUID input should be a string, bytes or UUID object"},
        {Kind::UuidParsing, "Input should be a valid UUID, {error}"},
        {Kind::UuidVersion, "UUID version {expected_version} expected"},

        // Decimal errors (Rust messages)
        {Kind::DecimalType, "Decimal input should be an integer, float, string or Decimal object"},
        {Kind::DecimalParsing, "Input should be a valid decimal"},
        {Kind::DecimalMaxDigits, "Decimal input should have no more than {max_digits} digit{s} in total"},
        {Kind::DecimalMaxPlaces, "Decimal input should have no more than {decimal_places} decimal place{s}"},
        {Kind::DecimalWholeDigits, "Decimal input should have no more than {whole_digits} digit{s} before the decimal point"},

        // Type checking errors
        {Kind::IsInstanceType, "Input should be an instance of {class}"},
        {Kind::IsSubclassType, "Input should be a subclass of {class}"},
        {Kind::CallableType, "Input should be callable"},
        {Kind::ModelType, "Input should be a valid dictionary or instance of {class_name}"},
        {Kind::DataclassType, "Input should be a dictionary or an instance of {class_name}"},
    };
    auto it = templates.find(kind_);
    return it != templates.end() ? it->second : "Validation error";
}

std::string ErrorType::message() const {
    if (!custom_message_.empty()) return custom_message_;
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

ErrorType ErrorType::build_known_type(const std::string& type_str) {
    // First, try direct match against known kind names
    static const std::unordered_map<std::string, Kind> name_to_kind = {
        {"none_required", Kind::NoneRequired},      {"none_type", Kind::NoneType},
        {"bool_type", Kind::BoolType},              {"int_type", Kind::IntType},
        {"int_from_float", Kind::IntFromFloat},
        {"float_type", Kind::FloatType},            {"string_type", Kind::StringType},
        {"bytes_type", Kind::BytesType},            {"dict_type", Kind::DictType},
        {"list_type", Kind::ListType},              {"tuple_type", Kind::TupleType},
        {"set_type", Kind::SetType},                {"frozen_set_type", Kind::FrozenSetType},
        {"frozen_field", Kind::FrozenField},        {"frozen_instance", Kind::FrozenInstance},
        {"union_type", Kind::UnionType},            {"date_type", Kind::DateType},
        {"time_type", Kind::TimeType},              {"datetime_type", Kind::DateTimeType},
        {"bool_parsing", Kind::BoolParsing},        {"int_parsing", Kind::IntParsing},
        {"float_parsing", Kind::FloatParsing},      {"int_multiple_of", Kind::IntMultipleOf},
        {"int_greater_than", Kind::IntGreaterThan}, {"int_less_than", Kind::IntLessThan},
        {"int_greater_than_equal", Kind::IntGreaterThanEqual},
        {"int_less_than_equal", Kind::IntLessThanEqual},
        {"float_multiple_of", Kind::FloatMultipleOf},{"float_greater_than", Kind::FloatGreaterThan},
        {"float_less_than", Kind::FloatLessThan},   {"float_greater_than_equal", Kind::FloatGreaterThanEqual},
        {"float_less_than_equal", Kind::FloatLessThanEqual},
        {"string_too_short", Kind::StringTooShort}, {"string_too_long", Kind::StringTooLong},
        {"string_pattern_mismatch", Kind::StringPatternMismatch},
        {"bytes_too_short", Kind::BytesTooShort},   {"bytes_too_long", Kind::BytesTooLong},
        {"too_short", Kind::TooShort},              {"too_long", Kind::TooLong},
        {"tuple_length_mismatch", Kind::TupleLengthMismatch},
        {"literal_mismatch", Kind::LiteralMismatch},{"literal_error", Kind::LiteralError},
        {"dict_keys_missing", Kind::DictKeysMissing},{"dict_keys_unexpected", Kind::DictKeysUnexpected},
        {"field_required", Kind::FieldRequired},    {"missing", Kind::Missing},
        {"extra_forbidden", Kind::ExtraForbidden},  {"no_such_attribute", Kind::NoSuchAttribute},
        {"arguments_type", Kind::ArgumentsType},    {"missing_argument", Kind::MissingArgument},
        {"missing_keyword_only_argument", Kind::MissingKeywordOnlyArgument},
        {"missing_positional_only_argument", Kind::MissingPositionalOnlyArgument},
        {"unexpected_positional_argument", Kind::UnexpectedPositionalArgument},
        {"unexpected_keyword_argument", Kind::UnexpectedKeywordArgument},
        {"multiple_argument_values", Kind::MultipleArgumentValues},
        {"date_parsing", Kind::DateParsing},        {"date_from_datetime_parsing", Kind::DateFromDatetimeParsing},
        {"date_from_datetime_inexact", Kind::DateFromDatetimeInexact},
        {"date_past", Kind::DatePast},              {"date_future", Kind::DateFuture},
        {"time_parsing", Kind::TimeParsing},        {"datetime_parsing", Kind::DateTimeParsing},
        {"datetime_from_date_parsing", Kind::DatetimeFromDateParsing},
        {"datetime_object_invalid", Kind::DatetimeObjectInvalid},
        {"datetime_past", Kind::DatetimePast},      {"datetime_future", Kind::DatetimeFuture},
        {"timezone_aware", Kind::TimezoneAware},    {"timezone_naive", Kind::TimezoneNaive},
        {"timezone_offset", Kind::TimezoneOffset},  {"timedelta_type", Kind::TimedeltaType},
        {"timedelta_parsing", Kind::TimedeltaParsing},
        {"time_delta_type", Kind::TimedeltaType},   {"time_delta_parsing", Kind::TimedeltaParsing},
        {"url_type", Kind::UrlType},                {"url_parsing", Kind::UrlParsing},
        {"url_syntax_violation", Kind::UrlSyntaxViolation}, {"url_too_long", Kind::UrlTooLong},
        {"url_scheme", Kind::UrlScheme},            {"url_host", Kind::UrlHost},
        {"uuid_type", Kind::UuidType},              {"uuid_parsing", Kind::UuidParsing},
        {"uuid_version", Kind::UuidVersion},
        {"decimal_type", Kind::DecimalType},        {"decimal_parsing", Kind::DecimalParsing},
        {"decimal_max_digits", Kind::DecimalMaxDigits}, {"decimal_max_places", Kind::DecimalMaxPlaces},
        {"decimal_whole_digits", Kind::DecimalWholeDigits},
        {"is_instance_of", Kind::IsInstanceType},   {"is_subclass_of", Kind::IsSubclassType},
        {"callable_type", Kind::CallableType},      {"json_invalid", Kind::JsonInvalid},
        {"enum", Kind::EnumError},                  {"invalid_key", Kind::InvalidKey},
        {"iterable_type", Kind::IterableType},      {"iteration_error", Kind::IterationError},
        {"mapping_type", Kind::MappingType},        {"string_sub_type", Kind::StringSubType},
        {"string_unicode", Kind::StringUnicode},    {"bytes_invalid_encoding", Kind::BytesInvalidEncoding},
        {"set_item_not_hashable", Kind::SetItemNotHashable}, {"missing_sentinel_error", Kind::MissingSentinelError},
        {"json_type", Kind::JsonType},              {"int_parsing_size", Kind::IntParsingSize},
        {"complex_type", Kind::ComplexType},        {"complex_str_parsing", Kind::ComplexStrParsing},
        {"model_attributes_type", Kind::ModelAttributesType}, {"get_attribute_error", Kind::GetAttributeError},
        {"needs_python_object", Kind::NeedsPythonObject}, {"dataclass_exact_type", Kind::DataclassExactType},
        {"default_factory_not_called", Kind::DefaultFactoryNotCalled},
        {"union_tag_invalid", Kind::UnionTagInvalid}, {"union_tag_not_found", Kind::UnionTagNotFound},
        {"invalid_json_value", Kind::InvalidJsonValue},
        {"recursion_error", Kind::RecursionError},  {"recursion_loop", Kind::RecursionLoop},
        {"greater_than", Kind::GreaterThan},
        {"less_than", Kind::LessThan},              {"greater_than_equal", Kind::GreaterThanEqual},
        {"less_than_equal", Kind::LessThanEqual},   {"multiple_of", Kind::MultipleOf},
        {"finite_number", Kind::FiniteNumber},      {"string_not_ascii", Kind::StringNotAscii},
        {"value_error", Kind::ValueError},          {"assertion_error", Kind::AssertionError},
        {"model_type", Kind::ModelType},           {"dataclass_type", Kind::DataclassType},
        {"enum_error", Kind::EnumError},
    };

    auto it = name_to_kind.find(type_str);
    if (it != name_to_kind.end()) {
        return ErrorType(it->second);
    }

    // Unknown custom error type — build a pre-rendered ErrorType that produces
    // exactly the requested type_name. The message will be set via ctx["message"]
    // when CustomErrorValidator constructs the error (passing msg_ as context).
    return ErrorType(type_str, "");
}

} // namespace pydantic_core