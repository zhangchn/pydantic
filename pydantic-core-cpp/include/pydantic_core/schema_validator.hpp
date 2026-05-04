#pragma once

#include <string>
#include <memory>
#include "errors.hpp"
#include "validation_state.hpp"
#include "result.hpp"
#include "types.hpp"
#include "combined_validator.hpp"

namespace pydantic_core {

// SchemaValidator - the main validation class
// Matches Rust's SchemaValidator pyclass
class SchemaValidator {
public:
    // Constructor from schema dict and optional config
    SchemaValidator(const std::string& schema_json, 
                   const std::string& config_json = "");
    
    // Validate Python object (JSON string input)
    std::string validate_python(const std::string& input_json,
                               std::optional<bool> strict = std::nullopt,
                               std::optional<ExtraBehavior> extra = std::nullopt);
    
    // Validate JSON data directly
    std::string validate_json(const std::string& json_data,
                             std::optional<bool> strict = std::nullopt);
    
    // Validate strings (string mapping)
    std::string validate_strings(const std::string& string_data,
                                std::optional<bool> strict = std::nullopt);
    
    // isinstance check - returns bool instead of raising
    bool isinstance_python(const std::string& input_json,
                          std::optional<bool> strict = std::nullopt);
    
    // Get default value
    std::optional<std::string> get_default_value(std::optional<bool> strict = std::nullopt);
    
    // Validate assignment to field
    std::string validate_assignment(const std::string& obj_json,
                                   const std::string& field_name,
                                   const std::string& field_value);
    
    // Properties
    const std::string& title() const { return title_; }
    
    // Representation
    std::string repr() const;
    
private:
    std::shared_ptr<CombinedValidator> validator_;
    std::string title_;
    std::string schema_json_;
    std::string config_json_;
    
    ValidationState::Config config_;
    
    // Build validator from schema
    void build_validator();
    
    // Prepare validation error from ValError
    ValidationError prepare_error(const ValError& err, InputType input_type);
};

// Some type - wrapper for optional values
// Matches Rust's PySome pyclass
template<typename T>
class Some {
public:
    explicit Some(T value) : value_(std::move(value)) {}
    
    const T& value() const { return value_; }
    T& value() { return value_; }
    
    std::string repr() const {
        return "Some(" + value_.repr() + ")";
    }
    
private:
    T value_;
};

} // namespace pydantic_core
