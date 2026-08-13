#pragma once

#include <string>
#include <variant>
#include <optional>
#include <unordered_map>
#include "types.hpp"

namespace pydantic_core {

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
        FloatType,
        StringType,
        BytesType,
        DictType,
        ListType,
        TupleType,
        SetType,
        FrozenSetType,
        UnionType,
        ModelType,
        DataclassType,

        // Parsing errors - string/input could not be parsed to target type
        BoolParsing,
        IntParsing,
        FloatParsing,
        
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
        UrlScheme,
        UrlHost,
        
        // UUID errors
        UuidType,

        // Type checking errors
        IsInstanceType,
        IsSubclassType,
        CallableType,

        // Other errors
        JsonInvalid,
        CustomError,
        RecursionError,
        
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

    Kind kind() const { return kind_; }
    const std::unordered_map<std::string, std::string>& context() const { return context_; }
    std::unordered_map<std::string, std::string>& context() { return context_; }
    
    // Get type name for error
    std::string type_name() const;
    
    // Get message template
    std::string message_template() const;
    
    // Get rendered message
    std::string message() const;
    
private:
    Kind kind_;
    std::unordered_map<std::string, std::string> context_;
};

// PydanticKnownError - predefined error types (factory methods)
class PydanticKnownError {
public:
    static ErrorType none_required() { return ErrorType(ErrorType::Kind::NoneRequired); }
    static ErrorType none_type() { return ErrorType(ErrorType::Kind::NoneType); }
    static ErrorType bool_type() { return ErrorType(ErrorType::Kind::BoolType); }
    static ErrorType int_type() { return ErrorType(ErrorType::Kind::IntType); }
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