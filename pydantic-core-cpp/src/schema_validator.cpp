#include "pydantic_core/schema_validator.hpp"
#include "pydantic_core/errors.hpp"
#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/validators/model_fields.hpp"

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
                        if (!fval) {
                            out += "null";
                        } else {
                            auto* val_str = static_cast<std::string*>(fval.get());
                            if (fval.get() == val_str) {
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

std::string SchemaValidator::repr() const {
    return "SchemaValidator(title='" + title_ + "')";
}

ValidationError SchemaValidator::prepare_error(const ValError& err, InputType input_type) {
    return ValidationError(title_, input_type, err);
}

} // namespace pydantic_core
