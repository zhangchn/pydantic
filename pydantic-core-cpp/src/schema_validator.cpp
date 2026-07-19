#include "pydantic_core/schema_validator.hpp"
#include "pydantic_core/errors.hpp"
#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/url_types.hpp"
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

// NEW: Constructor from Python dict directly (like Rust — no JSON serialization)
SchemaValidator::SchemaValidator(const py::dict& schema,
                                const py::dict& config)
    : schema_json_(""), config_json_("") {
    // Extract title from schema
    if (schema.contains("title")) {
        title_ = py::str(schema["title"]).cast<std::string>();
    } else {
        title_ = "Schema";
    }

    try {
        validator_ = SchemaBuilder::build_from_py(schema, config);
    } catch (const std::exception& e) {
        throw SchemaError(std::string("Error building \"") + 
                          (schema.contains("type") ? py::str(schema["type"]).cast<std::string>() : "?") + 
                          "\" validator:\n  " + e.what());
    }

    // Set up config defaults
    config_.strict = std::nullopt;
    config_.extra_behavior = std::nullopt;
    config_.from_attributes = std::nullopt;
    config_.cache_strings = StringCacheMode::All;
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
                                                   std::optional<ExtraBehavior> extra,
                                                   std::optional<bool> from_attributes) {
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
    if (from_attributes.has_value()) {
        state.set_from_attributes(*from_attributes);
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
    // For wrapper types, try all scalar types since we don't know the inner type
    // (NOT for model/typed-dict/dataclass — those have their own handler below)
    bool try_all = (type_name == "nullable" || type_name == "function-after"
                    || type_name == "function-before" || type_name == "function-wrap"
                    || type_name == "function-plain"
                    || type_name == "lax-or-strict" || type_name == "json-or-python");

    // For "any", try specific type casts based on actual value content
    bool is_any = (type_name == "any");

    // Helper: match base type name, including constrained- variants
    auto matches_type = [&](const std::string& base) -> bool {
        return type_name == base || type_name == ("constrained-" + base) ||
               type_name == (base + "-constrained") || type_name == ("constr-" + base) ||
               type_name == (base + "-constr");
    };

    // For wrapper types with try_all, check list/container types first to avoid
    // UB from static_cast<std::string*> on a py::list object
    if (try_all && matches_type("list")) {
        try {
            auto* lst = static_cast<py::list*>(value.get());
            if (lst) return *lst;
        } catch (...) {}
    }

    if (try_all || matches_type("str") || type_name == "string") {
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

    if (try_all || matches_type("int") || type_name == "int64") {
        try {
            auto* i = static_cast<int64_t*>(value.get());
            if (i) return py::int_(*i);
        } catch (...) {}
        try {
            auto* i = static_cast<int*>(value.get());
            if (i) return py::int_(*i);
        } catch (...) {}
    }
    if (try_all || matches_type("float")) {
        try {
            auto* d = static_cast<double*>(value.get());
            if (d) return py::float_(*d);
        } catch (...) {}
    }
    if (try_all || matches_type("bool")) {
        try {
            auto* b = static_cast<bool*>(value.get());
            if (b) return py::bool_(*b);
        } catch (...) {}
    }
    if (try_all || matches_type("bytes")) {
        try {
            auto* v = static_cast<std::vector<uint8_t>*>(value.get());
            if (v) return py::bytes(reinterpret_cast<const char*>(v->data()), v->size());
        } catch (...) {}
    }
    if (!type_name.empty() && (type_name == "model" || type_name == "model-fields" || type_name == "typed-dict" || type_name == "dataclass")) {
        // Directly convert ValidatedModelFieldsOutput to dict
        try {
            auto* mfo = static_cast<ValidatedModelFieldsOutput*>(value.get());
            if (mfo) {
                py::dict out;
                for (const auto& key : mfo->field_order) {
                    const auto& fv = mfo->fields.at(key);
                    out[py::str(key)] = result_to_python_with_type(fv.value, fv.type_name);
                }
                // Attach __pydantic_fields_set__ for exclude_unset support
                py::set fields_set;
                for (const auto& fname : mfo->fields_set) {
                    fields_set.add(py::str(fname));
                }
                out[py::str("__pydantic_fields_set__")] = std::move(fields_set);

                // Attach __pydantic_defaults__ for exclude_defaults support
                py::dict defaults_dict;
                for (const auto& key : mfo->field_order) {
                    const auto& fv = mfo->fields.at(key);
                    if (mfo->fields_set.find(key) == mfo->fields_set.end()) {
                        defaults_dict[py::str(key)] = result_to_python_with_type(fv.value, fv.type_name);
                    }
                }
                out[py::str("__pydantic_defaults__")] = std::move(defaults_dict);

                for (const auto& [key, fv] : mfo->extra) {
                    out[py::str(key)] = result_to_python_with_type(fv.value, fv.type_name);
                }
                return std::move(out);
            }
        } catch (...) {}
    }

    // Url type - convert to Python Url object using pybind11 cast
    if (type_name == "url") {
        try {
            auto* url_ptr = static_cast<Url*>(value.get());
            if (url_ptr) {
                return py::cast(*url_ptr);
            }
        } catch (...) {}
    }

    // MultiHostUrl type
    if (type_name == "multi-host-url") {
        try {
            auto* url_ptr = static_cast<MultiHostUrl*>(value.get());
            if (url_ptr) {
                return py::cast(*url_ptr);
            }
        } catch (...) {}
    }

    // "any" type — try all casts (numbers before strings)
    if (is_any) {
        // AnyValidator stores values as string*, parse the string
        // to determine the correct Python type
        try {
            auto* s = static_cast<std::string*>(value.get());
            if (s) {
                std::string str_val = *s;
                if (str_val == "null") return py::none();
                if (str_val == "true") return py::bool_(true);
                if (str_val == "false") return py::bool_(false);
                try { size_t p = 0; int iv = std::stoi(str_val, &p); if (p == str_val.length()) return py::int_(iv); } catch (...) {}
                try { double dv = std::stod(str_val); return py::float_(dv); } catch (...) {}
                return py::str(str_val);
            }
        } catch (...) {}
        return py::none();
    }

    // "enum" type — return the matched string value
    if (type_name == "enum" || type_name == "enum-constrained") {
        try {
            auto* s = static_cast<std::string*>(value.get());
            if (s) {
                return py::str(*s);
            }
        } catch (...) {}
        return py::none();
    }

    // "list" type — return the py::list directly (for non-wrapper type_name)
    if (matches_type("list")) {
        try {
            auto* lst = static_cast<py::list*>(value.get());
            if (lst) {
                return *lst;
            }
        } catch (...) {}
    }

    // For function-after/before/wrap/plain validators, check py::object*
    bool is_function_type = (type_name == "function-after" || type_name == "function-before" ||
                             type_name == "function-wrap" || type_name == "function-plain");
    if (is_function_type) {
        try {
            auto* obj = static_cast<py::object*>(value.get());
            if (obj) {
                return *obj;
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

    // Determine the type from the validator name
    std::string vname;
    if (validator_) {
        vname = validator_->name();
    }

    // For model-like validators, try ValidatedModelFieldsOutput
    if (!vname.empty() && (vname == "model" || vname == "model-fields" || vname == "typed-dict" || vname == "dataclass")) {
        try {
            auto* mfo = static_cast<ValidatedModelFieldsOutput*>(result.get());
            if (mfo) {
                py::dict out;
                for (const auto& key : mfo->field_order) {
                    const auto& fv = mfo->fields.at(key);
                    py::object py_val = result_to_python_with_type(fv.value, fv.type_name);
                    out[py::str(key)] = py_val;
                }
                // Attach __pydantic_fields_set__ for exclude_unset support
                py::set fields_set;
                for (const auto& fname : mfo->fields_set) {
                    fields_set.add(py::str(fname));
                }
                out[py::str("__pydantic_fields_set__")] = std::move(fields_set);

                // Attach __pydantic_defaults__ for exclude_defaults support
                // (fields NOT in fields_set that have defaults)
                py::dict defaults_dict;
                for (const auto& key : mfo->field_order) {
                    const auto& fv = mfo->fields.at(key);
                    if (mfo->fields_set.find(key) == mfo->fields_set.end()) {
                        // This field was NOT in the input — it came from a default
                        defaults_dict[py::str(key)] = result_to_python_with_type(fv.value, fv.type_name);
                    }
                }
                out[py::str("__pydantic_defaults__")] = std::move(defaults_dict);

                // Include extra fields
                for (const auto& [key, fv] : mfo->extra) {
                    out[py::str(key)] = result_to_python_with_type(fv.value, fv.type_name);
                }

                return std::move(out);
            }
        } catch (...) {}
    }

    // Use result_to_python_with_type with the validator's name for proper type dispatch
    if (!vname.empty()) {
        return result_to_python_with_type(result, vname);
    }

    // Fallback
    return py::none();
}

} // namespace pydantic_core
