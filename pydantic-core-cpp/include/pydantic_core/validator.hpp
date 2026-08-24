#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include "types.hpp"
#include "errors.hpp"
#include "validation_state.hpp"
#include "input.hpp"

namespace pydantic_core {

// Forward declarations for all validator types
class AnyValidator;
class NoneValidator;
class BoolValidator;
class IntValidator;
class FloatValidator;
class StringValidator;
class BytesValidator;
class ListValidator;
class DictValidator;
class SetValidator;
class FrozenSetValidator;
class TupleValidator;
class NullableValidator;
class UnionValidator;
class TaggedUnionValidator;
class ModelValidator;
class ModelFieldsValidator;
class TypedDictValidator;
class LiteralValidator;
class EnumValidator;
class DateValidator;
class TimeValidator;
class DatetimeValidator;
class TimedeltaValidator;
class UrlValidator;
class MultiHostUrlValidator;
class UuidValidator;
class FunctionBeforeValidator;
class FunctionAfterValidator;
class FunctionPlainValidator;
class FunctionWrapValidator;
class WithDefaultValidator;
class ChainValidator;
class LaxOrStrictValidator;
class JsonOrPythonValidator;
class JsonValidator;
class ArgumentsValidator;
class CallValidator;
class PyDataclassValidator;

// Validator base trait - matches Rust's Validator trait
class Validator {
public:
    virtual ~Validator() = default;
    
    // Main validation method - validates input and returns result
    virtual ValResult<std::shared_ptr<void>> validate(
        const Input& input, 
        ValidationState& state
    ) = 0;
    
    // Get default value (for WithDefaultValidator)
    virtual ValResult<std::shared_ptr<void>> default_value(
        ValidationState& state
    ) {
        return ValError::omit();
    }
    
    // Get validator name for error messages
    virtual std::string name() const = 0;

    // For root models: return the inner validator's name.
    // Default: not a root model.
    virtual std::string root_model_inner_name() const { return ""; }

    // Expected Python class for this validator (models only).  Used by unions
    // to prefer the exact-class branch for instance inputs.
    virtual const py::object& expected_class() const {
        static const py::object none = py::none();
        return none;
    }

    // For validators whose actual result type differs from name() (e.g. a
    // model whose inner is a function-after/wrap/plain validator producing a
    // py::object instead of model fields).  Returns "" when name() is accurate.
    virtual std::string result_dispatch_name() const { return ""; }

    // The type name that matches the validator's ACTUAL result value, for
    // result-to-Python dispatch.  Defaults to name(); models override it
    // (root models report their inner value type, models wrapping function
    // validators report "py_object").
    virtual std::string effective_result_name() const { return name(); }
    
    // Validate assignment (for model field assignment)
    virtual ValResult<std::shared_ptr<void>> validate_assignment(
        const Input& input,
        const std::string& field_name,
        ValidationState& state
    ) {
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "validate_assignment not supported for " + name()
        );
    }
};

// BuildValidator trait - constructs validators from schema dict
// Matches Rust's BuildValidator trait
class BuildValidator {
public:
    virtual ~BuildValidator() = default;
    
    // Build validator from schema
    // schema: dict with "type" key and other schema properties
    // config: optional CoreConfig dict
    virtual std::shared_ptr<Validator> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) = 0;
    
    // Get expected type name (e.g., "model", "int", "str")
    virtual std::string expected_type() const = 0;
};

// Schema parser - extracts typed values from schema dict
class SchemaParser {
public:
    SchemaParser(const std::unordered_map<std::string, std::string>& schema)
        : schema_(schema) {}
    
    std::string type() const {
        auto it = schema_.find("type");
        return it != schema_.end() ? it->second : "";
    }
    
    std::string get(const std::string& key, const std::string& default_val = "") const {
        auto it = schema_.find(key);
        return it != schema_.end() ? it->second : default_val;
    }
    
    std::optional<bool> get_bool(const std::string& key) const {
        auto it = schema_.find(key);
        if (it == schema_.end()) return std::nullopt;
        return it->second == "true" || it->second == "1";
    }
    
    std::optional<int64_t> get_int(const std::string& key) const {
        auto it = schema_.find(key);
        if (it == schema_.end()) return std::nullopt;
        try {
            return std::stoll(it->second);
        } catch (...) {
            return std::nullopt;
        }
    }
    
    std::optional<double> get_float(const std::string& key) const {
        auto it = schema_.find(key);
        if (it == schema_.end()) return std::nullopt;
        try {
            return std::stod(it->second);
        } catch (...) {
            return std::nullopt;
        }
    }
    
    // Get inner schema (for validators that wrap other schemas)
    std::unordered_map<std::string, std::string> get_inner_schema(const std::string& key = "schema") const {
        // For now, return empty - Phase 2 will implement proper schema parsing
        return std::unordered_map<std::string, std::string>();
    }
    
private:
    const std::unordered_map<std::string, std::string>& schema_;
};

// Validator factory - builds validators from schema type
class ValidatorFactory {
public:
    // Build validator from schema dict
    static std::shared_ptr<Validator> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    );
    
    // Register custom validator builder
    static void register_builder(const std::string& type, std::unique_ptr<BuildValidator> builder);
    
private:
    static std::unordered_map<std::string, std::unique_ptr<BuildValidator>>& builders();
};

} // namespace pydantic_core