#include "pydantic_core/schema_validator.hpp"
#include "pydantic_core/errors.hpp"
#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/validators/model_fields.hpp"
#include <pybind11/stl.h>

namespace py = pybind11;
namespace pydantic_core {

SchemaValidator::SchemaValidator(const std::string& schema_json, 
                                const std::string& config_json)
    : schema_json_(schema_json), config_json_(config_json) {
    // Parse title from schema (placeholder)
    title_ = "Schema";
    
    // Build validator from schema
    build_validator();
}

void SchemaValidator::build_validator() {
    try {
        validator_ = SchemaBuilder::build(schema_json_, config_json_);
    } catch (const std::exception& e) {
        throw SchemaError(std::string("Failed to build validator: ") + e.what());
    }
    
    // Set up config defaults
    config_.strict = std::nullopt;
    config_.extra_behavior = std::nullopt;
    config_.from_attributes = std::nullopt;
    config_.cache_strings = StringCacheMode::All;
}

std::string SchemaValidator::validate_python(const std::string& input_json,
                                             std::optional<bool> strict,
                                             std::optional<ExtraBehavior> extra) {
    (void)strict;
    (void)extra;
    
    if (!validator_) {
        throw std::runtime_error("Validator not initialized");
    }
    
    // Parse input JSON
    auto json_input_result = parse_json(input_json);
    if (json_input_result.is_err()) {
        throw std::runtime_error("Invalid input JSON");
    }
    
    auto json_input = std::move(json_input_result.value());
    
    // Create validation state
    ValidationState state(config_);
    
    // Validate
    auto result = validator_->validate(*json_input, state);
    
    if (result.is_ok()) {
        // For model-fields validation, merge default values into the output JSON.
        // Only do this if the validator chain contains a ModelFieldsValidator.
        // We detect this by checking if the top-level validator is a ModelValidator
        // (which wraps a ModelFieldsValidator) or directly a ModelFieldsValidator.
        const ModelFieldsValidator* mf_validator = nullptr;

        // Check if validator_ is directly a ModelFieldsValidator
        // CombinedValidator wraps validators in a variant, so we need to check each variant element
        // For simplicity, we try a heuristic: attempt to access model-fields specific output
        auto& validated = result.value();

        // Check if validator_ has a fields() method (ModelFieldsValidator)
        // Since we can't dynamic_cast on CombinedValidator, we use a simple approach:
        // Check if the result value pointer could be a ValidatedModelFieldsOutput.
        // ValidatedModelFieldsOutput has specific fields (fields, fields_set, extra).
        // We'll check by looking at the validator name.
        if (validator_) {
            auto vname = validator_->name();
            if (vname == "model" || vname == "model-fields" || vname == "typed-dict" || vname == "dataclass") {
                // This is a model-like validator, try to apply defaults
                auto* mfo = static_cast<ValidatedModelFieldsOutput*>(validated.get());
                if (mfo && (!mfo->fields.empty() || !mfo->fields_set.empty() || !mfo->extra.empty())) {
                // Parse input JSON to get existing values
                auto input_doc = simdjson::padded_string(input_json);
                simdjson::dom::parser parser;
                auto input_obj = parser.parse(input_doc);
                if (!input_obj.error() && input_obj.value().is_object()) {
                    auto obj = input_obj.value().get_object();
                    std::string out = "{";
                    bool first = true;

                    // Write all existing fields from input
                    for (auto& [key, val] : obj.value()) {
                        if (!first) out += ",";
                        first = false;
                        out += "\"" + std::string(key) + "\":" + simdjson::minify(val);
                    }

                    // Add validated fields that weren't in input (defaults)
                    for (const auto& [fname, fval] : mfo->fields) {
                        // Skip if already in input
                        bool in_input = false;
                        for (auto& [k, v] : obj.value()) {
                            if (std::string(k) == fname) { in_input = true; break; }
                        }
                        if (in_input) continue;

                        if (!first) out += ",";
                        first = false;
                        out += "\"" + fname + "\":";
                        if (!fval.value) {
                            out += "null";
                        } else {
                            auto* val_str = static_cast<std::string*>(fval.value.get());
                            if (fval.value.get() == val_str) {
                                const std::string& s = *val_str;
                                // Heuristic: detect JSON literals and numbers
                                if (s == "null" || s == "true" || s == "false") {
                                    out += s;
                                } else {
                                    // Check if it looks like a number
                                    bool is_number = !s.empty();
                                    size_t start = 0;
                                    if (s[0] == '-') { start = 1; if (s.size() == 1) is_number = false; }
                                    for (size_t i = start; i < s.size() && is_number; ++i) {
                                        if (s[i] != '.' && s[i] != 'e' && s[i] != 'E' && s[i] != '+' && !std::isdigit(s[i])) {
                                            is_number = false;
                                        }
                                    }
                                    if (is_number) {
                                        out += s;
                                    } else {
                                        // JSON-escape and wrap in quotes
                                        out += "\"";
                                        for (char c : s) {
                                            if (c == '"') out += "\\\"";
                                            else if (c == '\\') out += "\\\\";
                                            else if (c == '\n') out += "\\n";
                                            else if (c == '\r') out += "\\r";
                                            else if (c == '\t') out += "\\t";
                                            else out += c;
                                        }
                                        out += "\"";
                                    }
                                }
                            } else {
                                out += "null";
                            }
                        }
                    }
                    out += "}";
                    return out;
                }
                }
            }
        }
        // For non-model validators, just return the input
        return input_json;
    } else {
        // Prepare and throw validation error
        auto err = prepare_error(result.error(), InputType::Python);
        throw err;
    }
}

std::string SchemaValidator::validate_json(const std::string& json_data,
                                          std::optional<bool> strict) {
    (void)strict;
    
    if (!validator_) {
        throw std::runtime_error("Validator not initialized");
    }
    
    // Parse input JSON
    auto json_input_result = parse_json(json_data);
    if (json_input_result.is_err()) {
        throw std::runtime_error("Invalid input JSON");
    }
    
    auto json_input = std::move(json_input_result.value());
    
    // Create validation state
    ValidationState state(config_);
    
    // Validate
    auto result = validator_->validate(*json_input, state);
    
    if (result.is_ok()) {
        return json_data;
    } else {
        auto err = prepare_error(result.error(), InputType::Json);
        throw err;
    }
}

std::string SchemaValidator::validate_strings(const std::string& string_data,
                                              std::optional<bool> strict) {
    (void)strict;
    return string_data;
}

// NEW: Native Python object validate_strings (no JSON round-trip)
// Converts string values to their Python types (int, float, bool, etc.) before validation
py::object SchemaValidator::validate_strings_object(const py::object& input,
                                                    std::optional<bool> strict) {
    if (!validator_) {
        throw std::runtime_error("Validator not initialized");
    }

    // Convert string values in the input to their Python types
    py::dict typed_dict;

    if (py::isinstance<py::dict>(input)) {
        for (auto item : input.cast<py::dict>()) {
            py::str key = py::reinterpret_borrow<py::str>(item.first);
            py::object val = py::reinterpret_borrow<py::object>(item.second);

            if (py::isinstance<py::str>(val)) {
                std::string s = val.cast<std::string>();
                // Try to parse as bool
                if (s == "true") {
                    typed_dict[key] = py::bool_(true);
                } else if (s == "false") {
                    typed_dict[key] = py::bool_(false);
                } else if (s == "null" || s == "None") {
                    typed_dict[key] = py::none();
                } else {
                    // Try int
                    bool parsed = false;
                    try {
                        typed_dict[key] = py::int_(py::str(s));
                        parsed = true;
                    } catch (...) {}

                    // Try float
                    if (!parsed) {
                        try {
                            typed_dict[key] = py::float_(py::str(s));
                            parsed = true;
                        } catch (...) {}
                    }

                    // Keep as string
                    if (!parsed) {
                        typed_dict[key] = val;
                    }
                }
            } else {
                // Non-string value, keep as-is
                typed_dict[key] = val;
            }
        }
    } else {
        // Not a dict, validate as-is
        return validate_python_object(input, strict);
    }

    // Validate the typed dict
    return validate_python_object(typed_dict, strict);
}

bool SchemaValidator::isinstance_python(const std::string& input_json,
                                        std::optional<bool> strict) {
    (void)strict;

    try {
        validate_python(input_json);
        return true;
    } catch (...) {
        return false;
    }
}

// NEW: Native Python object isinstance check (no JSON round-trip)
bool SchemaValidator::isinstance_python_object(const py::object& input,
                                               std::optional<bool> strict) {
    if (!validator_) {
        return false;
    }

    try {
        // Create PythonInput wrapping the PyObject
        PythonInput py_input(input);

        // Create validation state
        ValidationState state(config_);
        if (strict.has_value()) {
            state.set_strict(*strict);
        }

        // Validate using the unified Input interface
        auto result = validator_->validate(py_input, state);
        return result.is_ok();
    } catch (...) {
        return false;
    }
}

std::optional<std::string> SchemaValidator::get_default_value(std::optional<bool> strict) {
    (void)strict;
    
    if (!validator_) {
        return std::nullopt;
    }
    
    ValidationState state(config_);
    auto result = validator_->default_value(state);
    
    if (result.is_ok()) {
        return std::nullopt; // Placeholder
    }
    return std::nullopt;
}

std::string SchemaValidator::validate_assignment(const std::string& obj_json,
                                                 const std::string& field_name,
                                                 const std::string& field_value) {
    (void)obj_json;
    (void)field_name;
    (void)field_value;
    // Placeholder
    return field_value;
}

// NEW: Native Python object validate_assignment (no JSON round-trip)
py::object SchemaValidator::validate_assignment_object(const py::object& obj,
                                                       const std::string& field_name,
                                                       const py::object& field_value) {
    if (!validator_) {
        throw std::runtime_error("Validator not initialized");
    }

    // The obj should be a dict or model-like object
    // We need to validate the field_value against the field's schema

    // For now, we use a simplified approach:
    // 1. Extract the field validator from the schema (if it's a model-fields validator)
    // 2. Validate field_value against it
    // 3. Return the updated object

    // Since we don't have direct access to field-level validators yet,
    // we'll use a heuristic: validate the entire object with the new field value merged in

    py::dict input_dict;
    if (py::isinstance<py::dict>(obj)) {
        input_dict = obj.cast<py::dict>();
    } else if (py::hasattr(obj, "__dict__")) {
        input_dict = obj.attr("__dict__").cast<py::dict>();
    } else {
        throw std::runtime_error("validate_assignment: object is not a dict or model");
    }

    // Create a new dict with the updated field
    py::dict updated_dict;
    for (auto item : input_dict) {
        updated_dict[item.first] = item.second;
    }
    updated_dict[py::str(field_name.c_str())] = field_value;

    // Validate the entire object (this will validate all fields, not just the one)
    // This is a simplification; the proper approach would be to extract the field validator
    return validate_python_object(updated_dict);
}

std::string SchemaValidator::repr() const {
    return "SchemaValidator(title='" + title_ + "')";
}

ValidationError SchemaValidator::prepare_error(const ValError& err, InputType input_type) {
    return ValidationError(title_, input_type, err);
}

// ============================================================================
// NEW: Native Python object validation (no JSON round-trip)
// ============================================================================

py::object SchemaValidator::validate_python_object(const py::object& input,
                                                   std::optional<bool> strict,
                                                   std::optional<ExtraBehavior> extra) {
    if (!validator_) {
        throw std::runtime_error("Validator not initialized");
    }

    // Create PythonInput wrapping the PyObject
    PythonInput py_input(input);

    // Create validation state
    ValidationState state(config_);
    if (strict.has_value()) {
        state.set_strict(*strict);
    }
    if (extra.has_value()) {
        state.set_extra_behavior(*extra);
    }

    // Validate using the unified Input interface
    auto result = validator_->validate(py_input, state);

    if (result.is_ok()) {
        return result_to_python(result.value());
    } else {
        auto err = prepare_error(result.error(), InputType::Python);
        throw err;
    }
}

// Helper: convert shared_ptr<void> to Python object using type name
py::object SchemaValidator::result_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name) {
    if (!value) {
        return py::none();
    }

    // Use type_name to determine how to cast
    // For "nullable", try all types since we don't know the inner type
    bool try_all = (type_name == "nullable");

    if (try_all || type_name == "str" || type_name == "string") {
        auto* s = static_cast<std::string*>(value.get());
        if (s) {
            try {
                std::string str_val = *s;
                // Handle JSON literals
                if (str_val == "null") return py::none();
                if (str_val == "true") return py::bool_(true);
                if (str_val == "false") return py::bool_(false);
                if (!str_val.empty() && str_val.front() == '"' && str_val.back() == '"') {
                    py::object json_mod = py::module_::import("json");
                    return json_mod.attr("loads")(str_val);
                }
                return py::str(str_val);
            } catch (...) {
                // Invalid string, fall through
            }
        }
    }

    if (try_all || type_name == "int" || type_name == "int64") {
        try {
            auto* i = static_cast<int64_t*>(value.get());
            if (i) return py::int_(*i);
        } catch (...) {}
        try {
            auto* i = static_cast<int*>(value.get());
            if (i) return py::int_(*i);
        } catch (...) {}
    }
    if (try_all || type_name == "float") {
        try {
            auto* d = static_cast<double*>(value.get());
            if (d) return py::float_(*d);
        } catch (...) {}
    }
    if (try_all || type_name == "bool") {
        try {
            auto* b = static_cast<bool*>(value.get());
            if (b) return py::bool_(*b);
        } catch (...) {}
    }
    if (try_all || type_name == "bytes") {
        try {
            auto* v = static_cast<std::vector<uint8_t>*>(value.get());
            if (v) return py::bytes(reinterpret_cast<const char*>(v->data()), v->size());
        } catch (...) {}
    }
    if (try_all || type_name == "model" || type_name == "model-fields" || type_name == "typed-dict" || type_name == "dataclass") {
        // Directly convert ValidatedModelFieldsOutput to dict
        try {
            auto* mfo = static_cast<ValidatedModelFieldsOutput*>(value.get());
            if (mfo) {
                py::dict out;
                for (const auto& [key, fv] : mfo->fields) {
                    out[py::str(key)] = result_to_python_with_type(fv.value, fv.type_name);
                }
                for (const auto& [key, fv] : mfo->extra) {
                    out[py::str(key)] = result_to_python_with_type(fv.value, fv.type_name);
                }
                return std::move(out);
            }
        } catch (...) {}
    }

    // Fallback
    return py::none();
}

py::object SchemaValidator::result_to_python(const std::shared_ptr<void>& result, bool check_model) {
    if (!result) {
        return py::none();
    }

    // For model-like validators, try ValidatedModelFieldsOutput
    // Always try this, not just when check_model is true
    if (validator_) {
        auto vname = validator_->name();
        if (vname == "model" || vname == "model-fields" || vname == "typed-dict" || vname == "dataclass") {
            try {
                auto* mfo = static_cast<ValidatedModelFieldsOutput*>(result.get());
                if (mfo) {
                    py::dict out;
                    for (const auto& [key, fv] : mfo->fields) {
                        // Use type_name to convert value
                        py::object py_val = result_to_python_with_type(fv.value, fv.type_name);
                        out[py::str(key)] = py_val;
                    }
                    for (const auto& [key, fv] : mfo->extra) {
                        out[py::str(key)] = result_to_python_with_type(fv.value, fv.type_name);
                    }
                    return std::move(out);
                }
            } catch (...) {}
        }
    }

    // Try to determine type using typeid
    // Note: this requires RTTI and works with shared_ptr<void> only if
    // the shared_ptr was created with the correct type

    // Try string - use a wrapper approach
    // Since we can't safely cast shared_ptr<void> to shared_ptr<string>,
    // we'll try to detect the type by checking the memory layout

    // For now, return a placeholder for string values
    // TODO: Implement proper type-safe result storage

    // Try int
    try {
        auto* i = static_cast<int*>(result.get());
        if (i) return py::int_(*i);
    } catch (...) {}

    // Try int64_t
    try {
        auto* i = static_cast<int64_t*>(result.get());
        if (i) return py::int_(*i);
    } catch (...) {}

    // Try uint64_t
    try {
        auto* u = static_cast<uint64_t*>(result.get());
        if (u) return py::int_(*u);
    } catch (...) {}

    // Try double
    try {
        auto* d = static_cast<double*>(result.get());
        if (d) return py::float_(*d);
    } catch (...) {}

    // Try bool
    try {
        auto* b = static_cast<bool*>(result.get());
        if (b) return py::bool_(*b);
    } catch (...) {}

    // Try vector<uint8_t> (bytes)
    try {
        auto* v = static_cast<std::vector<uint8_t>*>(result.get());
        if (v) return py::bytes(reinterpret_cast<const char*>(v->data()), v->size());
    } catch (...) {}

    // Fallback
    return py::none();
}

} // namespace pydantic_core
