#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
#include <optional>
#include <unordered_map>
#ifdef HAS_PYBIND11
#include <pybind11/pybind11.h>
namespace py = pybind11;
#endif
#include "types.hpp"

namespace pydantic_core {

// Constraint values appear verbatim in error messages, so they need the same
// shortest round-trip rendering Rust's formatter gives ("0.1", "1"), not the
// fixed six-decimal text std::to_string produces.
inline std::string format_double(double value) {
    char buf[64];
    for (int precision = 1; precision <= 17; ++precision) {
        int written = std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
        if (written < 0 || written >= static_cast<int>(sizeof(buf))) {
            continue;
        }
        if (std::strtod(buf, nullptr) == value) {
            return std::string(buf, static_cast<size_t>(written));
        }
    }
    return std::to_string(value);
}


// ErrorType is standalone - no circular dependencies

// Error type enumeration - matches Rust's ErrorType enum (~100 types)
class ErrorType {
public:
    enum class Kind {
        // Type errors - wrong type provided
        NoneRequired,
        NoneType,
        BoolType,
        IntType,
        IntFromFloat,
        FloatType,
        StringType,
        BytesType,
        DictType,
        ListType,
        TupleType,
        SetType,
        FrozenSetType,
        FrozenField,
        FrozenInstance,
        UnionType,
        ModelType,
        DataclassType,

        // Parsing errors - string/input could not be parsed to target type
        BoolParsing,
        IntParsing,
        IntParsingSize,
        FloatParsing,
        ComplexType,
        ComplexStrParsing,

        // String/bytes encoding errors
        StringSubType,
        StringUnicode,
        BytesInvalidEncoding,
        SetItemNotHashable,
        MissingSentinelError,
        JsonType,
        
        // Integer constraint errors
        IntMultipleOf,
        IntGreaterThan,
        IntLessThan,
        IntGreaterThanEqual,
        IntLessThanEqual,
        
        // Float constraint errors
        FloatMultipleOf,
        FloatGreaterThan,
        FloatLessThan,
        FloatGreaterThanEqual,
        FloatLessThanEqual,
        
        // String constraint errors
        StringTooShort,
        StringTooLong,
        StringPatternMismatch,
        
        // Bytes constraint errors
        BytesTooShort,
        BytesTooLong,
        
        // List/Set constraint errors
        ListTooShort,
        ListTooLong,
        SetTooShort,
        SetTooLong,
        
        // Dict constraint errors
        DictTooShort,
        DictTooLong,
        
        // Tuple errors
        TupleLengthMismatch,
        
        // Literal errors
        LiteralMismatch,
        LiteralError,
        EnumError,
        
        // Dict field errors
        DictKeysMissing,
        DictKeysUnexpected,
        
        // Field errors
        FieldRequired,
        Missing,
        ExtraForbidden,
        InvalidKey,
        NoSuchAttribute,

        // Arguments errors (function parameter validation)
        ArgumentsType,
        MissingArgument,
        MissingKeywordOnlyArgument,
        MissingPositionalOnlyArgument,
        UnexpectedPositionalArgument,
        UnexpectedKeywordArgument,
        MultipleArgumentValues,
        
        // Date/Time errors
        DateType,
        DateParsing,
        DateFromDatetimeParsing,
        DateFromDatetimeInexact,
        DatePast,
        DateFuture,
        TimeType,
        TimeParsing,
        DateTimeType,
        DateTimeParsing,
        DatetimeFromDateParsing,
        DatetimeObjectInvalid,
        DatetimePast,
        DatetimeFuture,
        TimezoneAware,
        TimezoneNaive,
        TimezoneOffset,
        TimedeltaType,
        TimedeltaParsing,
        
        // URL errors
        UrlType,
        UrlParsing,
        UrlSyntaxViolation,
        UrlTooLong,
        UrlScheme,
        UrlHost,
        
        // UUID errors
        UuidType,
        UuidParsing,
        UuidVersion,

        // Decimal errors
        DecimalType,
        DecimalParsing,
        DecimalMaxDigits,
        DecimalMaxPlaces,
        DecimalWholeDigits,

        // Type checking errors
        IsInstanceType,
        IsSubclassType,
        CallableType,

        // Collection type errors (Rust-named)
        IterableType,
        IterationError,
        MappingType,

        // Model / attribute-extraction errors
        ModelAttributesType,
        GetAttributeError,
        NeedsPythonObject,
        DataclassExactType,
        DefaultFactoryNotCalled,

        // Union discriminator errors
        UnionTagInvalid,
        UnionTagNotFound,

        // Other errors
        JsonInvalid,
        InvalidJsonValue,
        CustomError,
        RecursionError,
        RecursionLoop,
        ValueError,
        AssertionError,
        
        // Generic constraint errors (used by constrained int/float validators)
        GreaterThan,
        LessThan,
        GreaterThanEqual,
        LessThanEqual,
        MultipleOf,
        FiniteNumber,
        
        // Container length errors (generic)
        TooShort,
        TooLong,
        
        // String encoding errors
        StringNotAscii
    };
    
    // Constructor for simple error types
    explicit ErrorType(Kind kind) : kind_(kind) {}
    
    // Constructor with numeric context (for constraints)
    ErrorType(Kind kind, int64_t numeric_value) : kind_(kind) {
        context_["value"] = std::to_string(numeric_value);
    }
    
    // Constructor with two numeric values (for tuple mismatch)
    ErrorType(Kind kind, int64_t val1, int64_t val2) : kind_(kind) {
        context_["expected"] = std::to_string(val1);
        context_["actual"] = std::to_string(val2);
    }
    
    // Constructor with double context (for float constraints)
    ErrorType(Kind kind, double numeric_value) : kind_(kind) {
        context_["value"] = std::to_string(numeric_value);
    }

    // Constructor with string context (for datetime/timezone errors)
    ErrorType(Kind kind, std::string key, std::string value) : kind_(kind) {
        context_[std::move(key)] = std::move(value);
    }

    // Constructor with two string context values
    ErrorType(Kind kind, std::string key1, std::string val1, std::string key2, std::string val2) : kind_(kind) {
        context_[std::move(key1)] = std::move(val1);
        context_[std::move(key2)] = std::move(val2);
    }

    // Construct from raw type_name and message (pre-rendered, no template processing)
    ErrorType(std::string custom_type_name, std::string custom_message)
        : kind_(Kind::CustomError), custom_type_name_(std::move(custom_type_name)), custom_message_(std::move(custom_message)) {}

    Kind kind() const { return kind_; }
    const std::unordered_map<std::string, std::string>& context() const { return context_; }
    std::unordered_map<std::string, std::string>& context() { return context_; }
    
    // Get type name for error
    std::string type_name() const;
    bool is_custom() const { return !custom_type_name_.empty(); }
    
    // Get message template
    std::string message_template() const;
    
    // Get rendered message
    std::string message() const;

    // Context values that must reach errors() as real Python objects (a Decimal
    // constraint, for example). `display` renders the message and the JSON ctx
    // placeholder; the object itself is carried alongside it because a Decimal
    // cannot survive the JSON channel without losing its type.
#ifdef HAS_PYBIND11
    void set_ctx_object(const std::string& key, const std::string& display, py::object value) {
        context_[key] = display;
        context_objects_[key] = std::move(value);
    }

    const std::unordered_map<std::string, py::object>& context_objects() const {
        return context_objects_;
    }
#endif

    // Build a known ErrorType from a custom error type string.
    // Tries to match against all registered known kinds first.
    // Falls back to {Kind::CustomError} if no match is found.
    static ErrorType build_known_type(const std::string& type_str);
    
private:
    Kind kind_;
    std::unordered_map<std::string, std::string> context_;
#ifdef HAS_PYBIND11
    std::unordered_map<std::string, py::object> context_objects_;
#endif
    // For pre-rendered custom errors (type_name + message stored as-is)
    std::string custom_type_name_;
    std::string custom_message_;
};

// PydanticKnownError - predefined error types (factory methods)
class PydanticKnownError {
public:
    static ErrorType none_required() { return ErrorType(ErrorType::Kind::NoneRequired); }
    static ErrorType none_type() { return ErrorType(ErrorType::Kind::NoneType); }
    static ErrorType bool_type() { return ErrorType(ErrorType::Kind::BoolType); }
    static ErrorType int_type() { return ErrorType(ErrorType::Kind::IntType); }
    static ErrorType int_from_float() { return ErrorType(ErrorType::Kind::IntFromFloat); }
    static ErrorType float_type() { return ErrorType(ErrorType::Kind::FloatType); }
    static ErrorType string_type() { return ErrorType(ErrorType::Kind::StringType); }
    static ErrorType bytes_type() { return ErrorType(ErrorType::Kind::BytesType); }
    static ErrorType dict_type() { return ErrorType(ErrorType::Kind::DictType); }
    static ErrorType list_type() { return ErrorType(ErrorType::Kind::ListType); }
    static ErrorType tuple_type() { return ErrorType(ErrorType::Kind::TupleType); }
    static ErrorType set_type() { return ErrorType(ErrorType::Kind::SetType); }
    static ErrorType frozenset_type() { return ErrorType(ErrorType::Kind::FrozenSetType); }
    static ErrorType union_type() { return ErrorType(ErrorType::Kind::UnionType); }
    static ErrorType model_type() { return ErrorType(ErrorType::Kind::ModelType); }
    static ErrorType dataclass_type() { return ErrorType(ErrorType::Kind::DataclassType); }
    static ErrorType missing() { return ErrorType(ErrorType::Kind::Missing); }
    static ErrorType extra_forbidden() { return ErrorType(ErrorType::Kind::ExtraForbidden); }
    static ErrorType literal_mismatch() { return ErrorType(ErrorType::Kind::LiteralMismatch); }
    static ErrorType enum_error() { return ErrorType(ErrorType::Kind::EnumError); }
    static ErrorType custom_error(const std::string& msg = "") { return ErrorType(ErrorType::Kind::CustomError); }
};

// PydanticOmit - signal to omit field from output
class PydanticOmit : public std::exception {
public:
    const char* what() const noexcept override { return "PydanticOmit"; }
};

// PydanticUseDefault - signal to use default value  
class PydanticUseDefault : public std::exception {
public:
    const char* what() const noexcept override { return "PydanticUseDefault"; }
};

} // namespace pydantic_core