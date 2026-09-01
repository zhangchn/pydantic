#pragma once

#include <memory>
#include <optional>
#include <string>
#include "errors.hpp"
#include "validation_state.hpp"
#include "result.hpp"
#include "types.hpp"
#include "combined_validator.hpp"

// Forward declare pybind11 types
namespace pybind11 {
class object;
class dict;
}
namespace py = pybind11;

namespace pydantic_core {

// SchemaValidator - the main validation class
// Matches Rust's SchemaValidator pyclass
class SchemaValidator {
public:
    // Constructor from JSON strings (legacy)
    SchemaValidator(const std::string& schema_json,
                   const std::string& config_json = "");

    // Constructor from Python dicts directly (like Rust — no JSON round-trip)
    SchemaValidator(const py::dict& schema,
                   const py::dict& config);

    // Validate Python object (JSON string input) - legacy
    std::string validate_python(const std::string& input_json,
                               std::optional<bool> strict = std::nullopt,
                               std::optional<ExtraBehavior> extra = std::nullopt);

    // Validate Python object directly (NEW - no JSON round-trip)
    py::object validate_python_object(const py::object& input,
                                      std::optional<bool> strict = std::nullopt,
                                      std::optional<ExtraBehavior> extra = std::nullopt,
                                      std::optional<bool> from_attributes = std::nullopt,
                                      py::object context = py::none(),
                                      bool coerce_strings = false,
                                      py::object self_instance = py::none(),
                                      std::optional<bool> by_alias = std::nullopt,
                                      std::optional<bool> by_name = std::nullopt);

    // Populate self_instance from the fields snapshot taken by the outermost
    // fields-position function-after (BaseModel.__init__ path).  Returns
    // false when no snapshot exists (and clears it).
    bool apply_init_snapshot(const py::object& self_instance);
    bool has_init_snapshot() const { return !init_snapshot_.is_none(); }

    // isinstance check on Python object directly (NEW - no JSON round-trip)
    bool isinstance_python_object(const py::object& input,
                                  std::optional<bool> strict = std::nullopt);

    // Validate JSON data directly
    std::string validate_json(const std::string& json_data,
                             std::optional<bool> strict = std::nullopt);

    // Validate strings (string mapping) - legacy JSON path
    std::string validate_strings(const std::string& string_data,
                                std::optional<bool> strict = std::nullopt);

    // Validate strings on Python object directly (NEW - no JSON round-trip)
    py::object validate_strings_object(const py::object& input,
                                       std::optional<bool> strict = std::nullopt,
                                       std::optional<ExtraBehavior> extra = std::nullopt);

    // isinstance check - returns bool instead of raising
    bool isinstance_python(const std::string& input_json,
                          std::optional<bool> strict = std::nullopt);

    // Get default value
    std::optional<std::string> get_default_value(std::optional<bool> strict = std::nullopt);

    // Validate assignment to field - legacy JSON path
    std::string validate_assignment(const std::string& obj_json,
                                   const std::string& field_name,
                                   const std::string& field_value);

    // Validate assignment on Python objects directly (NEW - no JSON round-trip)
    py::object validate_assignment_object(const py::object& obj,
                                          const std::string& field_name,
                                          const py::object& field_value);

    // Properties
    const std::string& title() const { return title_; }

    // Whether the top-level schema is a root model (RootModel[T])
    bool is_root_model() const;

    // Whether the top-level schema is a "call" validator (validate_call).
    // Call validators produce real function results which may be plain ints,
    // so the int-result input passthrough heuristic must not apply to them.
    bool is_call() const { return validator_ && validator_->name() == "call"; }

    // Whether the top-level schema is a function validator (before/after/wrap/plain).
    // Function validators produce real results which may be plain ints,
    // so the int-result input passthrough heuristic must not apply to them.
    bool is_function_wrapper() const {
        return validator_ && validator_->name().rfind("function-", 0) == 0;
    }

    // Whether the top-level schema is a dataclass validator
    bool is_dataclass() const { return validator_ && validator_->name() == "dataclass"; }

    // The post_init method name from the schema (e.g. "model_post_init"), or empty
    const std::string& post_init() const { return post_init_; }

    // Representation
    std::string repr() const;

    // Get schema JSON (for pickle)
    const std::string& schema_json() const { return schema_json_; }

    // Get config JSON (for pickle)
    const std::string& config_json() const { return config_json_; }

private:
    std::shared_ptr<CombinedValidator> validator_;
    std::string title_;
    std::string schema_json_;
    std::string config_json_;
    std::string post_init_;

    ValidationState::Config config_;

    // Pre-func validated-fields snapshot from the outermost fields-position
    // function-after during the most recent validate_python_object call.
    py::object init_snapshot_ = py::none();

    // config hide_input_in_errors: omit input_value/input_type from display
    bool hide_input_in_errors_ = false;

    // Build validator from schema
    void build_validator();

    // Prepare validation error from ValError
    ValidationError prepare_error(const ValError& err, InputType input_type);

    // Convert validated result to Python object
    py::object result_to_python(const std::shared_ptr<void>& result, bool check_model = true);
};

// Convert a validated result (shared_ptr<void>) to a Python object by type
// name.  Free function so validators (e.g. function-after needs the validated
// inner result as a Python object for the Python callable) can use it too.
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name);

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
