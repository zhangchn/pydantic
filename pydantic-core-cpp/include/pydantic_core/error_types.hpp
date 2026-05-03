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
        
        // Dict field errors
        DictKeysMissing,
        DictKeysUnexpected,
        
        // Field errors
        FieldRequired,
        Missing,
        
        // Other errors
        JsonInvalid,
        CustomError,
        RecursionError
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
    
    Kind kind() const { return kind_; }
    const std::unordered_map<std::string, std::string>& context() const { return context_; }
    
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