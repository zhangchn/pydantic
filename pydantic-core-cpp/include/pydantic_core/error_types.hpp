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
        
        // Value errors - value out of bounds
        StringTooShort,
        StringTooLong,
        
        IntGreaterThan,
        IntLessThan,
        
        FloatGreaterThan,
        FloatLessThan,
        
        DictKeysMissing,
        DictKeysUnexpected,
        
        // Field errors
        FieldRequired,
        Missing,
        
        // Other errors
        JsonInvalid,
        CustomError
    };
    
    explicit ErrorType(Kind kind) : kind_(kind) {}
    
    // Error types with context
    static ErrorType string_too_short(size_t min_length) {
        ErrorType err(Kind::StringTooShort);
        err.context_["min_length"] = std::to_string(min_length);
        return err;
    }
    
    static ErrorType string_too_long(size_t max_length) {
        ErrorType err(Kind::StringTooLong);
        err.context_["max_length"] = std::to_string(max_length);
        return err;
    }
    
    static ErrorType int_greater_than(int64_t gt) {
        ErrorType err(Kind::IntGreaterThan);
        err.context_["gt"] = std::to_string(gt);
        return err;
    }
    
    static ErrorType int_less_than(int64_t lt) {
        ErrorType err(Kind::IntLessThan);
        err.context_["lt"] = std::to_string(lt);
        return err;
    }
    
    static ErrorType missing_field(const std::string& field_name) {
        ErrorType err(Kind::Missing);
        err.context_["field_name"] = field_name;
        return err;
    }
    
    static ErrorType field_required() {
        return ErrorType(Kind::FieldRequired);
    }
    
    static ErrorType custom(const std::string& message, const std::string& error_type) {
        ErrorType err(Kind::CustomError);
        err.context_["message"] = message;
        err.context_["error_type"] = error_type;
        return err;
    }
    
    Kind kind() const { return kind_; }
    const std::unordered_map<std::string, std::string>& context() const { return context_; }
    
    std::string type_name() const;
    std::string message_template() const;
    std::string message() const;
    
private:
    Kind kind_;
    std::unordered_map<std::string, std::string> context_;
};

// PydanticKnownError - predefined error types
class PydanticKnownError {
public:
    static ErrorType none_required() { return ErrorType(ErrorType::Kind::NoneRequired); }
    static ErrorType bool_type() { return ErrorType(ErrorType::Kind::BoolType); }
    static ErrorType int_type() { return ErrorType(ErrorType::Kind::IntType); }
    static ErrorType float_type() { return ErrorType(ErrorType::Kind::FloatType); }
    static ErrorType string_type() { return ErrorType(ErrorType::Kind::StringType); }
    static ErrorType bytes_type() { return ErrorType(ErrorType::Kind::BytesType); }
    static ErrorType dict_type() { return ErrorType(ErrorType::Kind::DictType); }
    static ErrorType list_type() { return ErrorType(ErrorType::Kind::ListType); }
};

// PydanticCustomError - user-defined error
class PydanticCustomError {
public:
    PydanticCustomError(const std::string& error_type, const std::string& message_template)
        : error_type_(error_type), message_template_(message_template) {}
    
private:
    std::string error_type_;
    std::string message_template_;
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