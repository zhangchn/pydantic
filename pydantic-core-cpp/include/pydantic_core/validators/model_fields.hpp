#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/string_input.hpp"
// Note: python_input.hpp is included in the .cpp file that uses it
// to avoid pybind11 dependency in test targets
#ifdef HAS_PYBIND11
#include "pydantic_core/python_input.hpp"
#endif
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <set>
#include <algorithm>

namespace pydantic_core {

// ExtraBehavior is already defined in types.hpp

// ============================================================================
// FieldInfo - describes a single field in a model/typed-dict/dataclass
// ============================================================================
struct FieldInfo {
    std::string name;                          // Field name (e.g., "field_a")
    std::shared_ptr<Validator> schema;         // Inner validator for this field
    bool required = true;                      // Whether the field must be present
    std::string default_value_str;           // Default value as string representation.
                                                // Empty = no default (field is required).
                                                // "null" = explicit null default.
                                                // Strings stored raw (no JSON quotes).
    bool frozen = false;                       // Whether field can be reassigned
    std::string alias;                         // Alternative name for lookup

    std::string display_name() const {
        return alias.empty() ? name : alias;
    }
};

// ============================================================================
// ValidatedModelFieldsOutput - the return value from ModelFieldsValidator
// ============================================================================
struct ValidatedModelFieldsOutput {
    struct FieldValue {
        std::shared_ptr<void> value;
        std::string type_name;  // "str", "int", "float", "bool", "bytes", "dict", "list", etc.
    };
    std::unordered_map<std::string, FieldValue> fields;  // Validated field values
    std::vector<std::string> field_order;                // Fields in declaration order
    std::unordered_map<std::string, FieldValue> extra;   // Extra fields (if allow)
    std::set<std::string> fields_set;                    // Names of fields that were in input
};

// ============================================================================
// ModelFieldsValidator - validates dict input against a set of typed fields
// ============================================================================
class ModelFieldsValidator : public Validator {
public:
    ModelFieldsValidator() = default;

    ModelFieldsValidator(
        std::unordered_map<std::string, FieldInfo> fields,
        ExtraBehavior extra_behavior = ExtraBehavior::Ignore,
        std::shared_ptr<Validator> extras_validator = nullptr,
        std::string model_name = "Model"
    )
        : fields_(std::move(fields))
        , extra_behavior_(extra_behavior)
        , extras_validator_(std::move(extras_validator))
        , model_name_(std::move(model_name))
    {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Check from_attributes setting from state or schema
        bool use_from_attributes = state.from_attributes_or(from_attributes_);
        
        // Get dict - if from_attributes is true, try to get attributes from object
        if (use_from_attributes) {
            // For Python input with from_attributes, use validate_dict_from_attributes
            auto* py_input = dynamic_cast<const PythonInput*>(&input);
            if (py_input) {
                auto dict_result = py_input->validate_dict_from_attributes(state.strict_or(false));
                if (dict_result.is_ok()) {
                    auto dict = std::move(dict_result.value());
                    return validate_dict(std::move(dict), input, state);
                }
                // If validate_dict_from_attributes failed, fall back to regular validate_dict
            }
        }
        
        // Regular dict validation — recursion guard using unique depth tokens
        char depth_unique_marker = 0;
        auto rec_entry = state.enter_recursion(&depth_unique_marker);
        if (!rec_entry.allowed()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::RecursionError),
                state.location(),
                "Recursion error - cyclic reference detected"
            );
        }

        auto dict_result = input.validate_dict(state.strict_or(false));
        if (dict_result.is_err()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::ModelType),
                state.location(),
                input.as_error_value().repr
            );
        }

        auto dict = std::move(dict_result.value());
        return validate_dict(std::move(dict), input, state);
    }

    ValResult<std::shared_ptr<void>> validate_dict(
        std::unique_ptr<ValidatedDict> dict,
        const Input& original_input,
        ValidationState& state
    ) {
        ValidatedModelFieldsOutput output;
        ValError combined_errors(ValError::Kind::LineErrors);
        std::set<std::string> used_keys;

        for (const auto& name : field_order_) {
            const auto& field = fields_.at(name);
            state.push_loc(name);

            std::string lookup_key = field.alias.empty() ? name : field.alias;
            bool has_entry = false;

            if (dict->has_key(lookup_key)) {
                has_entry = true;
                used_keys.insert(lookup_key);
            } else if (!field.alias.empty() && dict->has_key(name)) {
                has_entry = true;
                used_keys.insert(name);
            }

            if (has_entry) {
                // Use whichever key was actually found (alias or canonical)
                std::string actual_key = dict->has_key(lookup_key) ? lookup_key : name;
                auto validate_result = validate_field_value_result(*dict, actual_key, field, state, combined_errors);

                if (validate_result.has_value()) {
                    // Validation succeeded (value may be nullptr for nullable)
                    auto& val = validate_result.value();
                    ValidatedModelFieldsOutput::FieldValue fv;
                    fv.value = val;
                    // Determine type name from field validator
                    if (field.schema) {
                        fv.type_name = field.schema->name();
                    } else {
                        fv.type_name = "null_schema";
                    }
                    output.fields[name] = std::move(fv);
                    output.fields_set.insert(name);
                    output.field_order.push_back(name);
                }
            } else {
                // Field not found
                if (field.required) {
                    auto err = ValError::line_error(
                        PydanticKnownError::missing(),
                        state.location(),
                        original_input.as_error_value().repr
                    );
                    combined_errors.merge(std::move(err));
                } else if (!field.default_value_str.empty()) {
                    ValidatedModelFieldsOutput::FieldValue fv;
                    // Parse the default value through the field's validator
                    // to get a properly typed result (e.g. double* for float fields,
                    // not a raw string like "0.700000")
                    auto parse_result = parse_json(field.default_value_str);
                    if (parse_result.is_ok() && field.schema) {
                        auto json_input = std::move(parse_result.value());
                        auto default_result = field.schema->validate(*json_input, state);
                        if (default_result.is_ok()) {
                            fv.value = default_result.value();
                            fv.type_name = field.schema->name();
                        } else {
                            fv.value = std::make_shared<std::string>(field.default_value_str);
                            fv.type_name = "str";
                        }
                    } else {
                        fv.value = std::make_shared<std::string>(field.default_value_str);
                        fv.type_name = "str";
                    }
                    output.fields[name] = std::move(fv);
                    output.field_order.push_back(name);
                }
            }

            state.pop_loc();
        }

        // Handle extra fields
        handle_extra_fields(*dict, used_keys, output, state, combined_errors);

        // Check if we have any line errors
        if (combined_errors.has_line_errors() && !combined_errors.line_errors().empty()) {
            return combined_errors;
        }

        return ValResult<std::shared_ptr<void>>(
            std::make_shared<ValidatedModelFieldsOutput>(std::move(output))
        );
    }

    std::string name() const override { return "model-fields"; }

    // Accessors for testing and schema building
    const std::unordered_map<std::string, FieldInfo>& fields() const { return fields_; }
    ExtraBehavior extra_behavior() const { return extra_behavior_; }
    const std::string& model_name() const { return model_name_; }

    void add_field(const std::string& name, FieldInfo info) {
        fields_[name] = std::move(info);
        field_order_.push_back(name);
    }

    void set_extra_behavior(ExtraBehavior eb) { extra_behavior_ = eb; }
    void set_model_name(const std::string& name) { model_name_ = name; }
    void set_extras_validator(std::shared_ptr<Validator> v) { extras_validator_ = std::move(v); }
    void set_from_attributes(bool value) { from_attributes_ = value; }
    bool from_attributes() const { return from_attributes_; }

protected:
    std::optional<std::shared_ptr<void>> validate_field_value_result(
        const ValidatedDict& dict,
        const std::string& key,
        const FieldInfo& field,
        ValidationState& state,
        ValError& combined_errors
    ) {
        // Try PythonValidatedDict path first
        // Note: HAS_PYBIND11 is defined in CMakeLists.txt for targets that use pybind11
#ifdef HAS_PYBIND11
        auto* py_dict = dynamic_cast<const PythonValidatedDict*>(&dict);
        if (py_dict) {
            auto py_obj_opt = py_dict->get_object(key);
            if (py_obj_opt) {
                PythonInput field_input(*py_obj_opt);
                field_input.set_current_location(state.location());
                auto result = field.schema->validate(field_input, state);
                if (result.is_ok()) {
                    return result.value();
                }
                auto& err = result.error();
                if (err.is_omit()) {
                    return std::nullopt;
                } else if (err.has_line_errors()) {
                    auto mutable_err = const_cast<ValError*>(&err);
                    combined_errors.merge(std::move(*mutable_err));
                } else if (err.is_internal()) {
                    auto new_err = ValError::line_error(
                        ErrorType(ErrorType::Kind::CustomError),
                        state.location(),
                        err.internal_message()
                    );
                    combined_errors.merge(std::move(new_err));
                }
                return std::nullopt;
            }
        }
        // Fallback for Python path: try to get entry and use StringInput
#endif

        // Try JsonValidatedDict path
        auto* json_dict = dynamic_cast<const JsonValidatedDict*>(&dict);
        if (json_dict) {
            auto element_opt = json_dict->get_element(key);
            if (element_opt) {
                auto field_input = JsonInput::create_from_element(*element_opt);
                auto result = field.schema->validate(*field_input, state);
                if (result.is_ok()) {
                    return result.value();
                }
                auto& err = result.error();
                if (err.is_omit()) {
                    return std::nullopt;
                } else if (err.has_line_errors()) {
                    auto mutable_err = const_cast<ValError*>(&err);
                    combined_errors.merge(std::move(*mutable_err));
                } else if (err.is_internal()) {
                    auto new_err = ValError::line_error(
                        ErrorType(ErrorType::Kind::CustomError),
                        state.location(),
                        err.internal_message()
                    );
                    combined_errors.merge(std::move(new_err));
                }
                return std::nullopt;
            }
        }

        // Fallback: use the dict entry's string representation
        auto entry_opt = dict.get(key);
        if (entry_opt) {
            StringInput str_input(entry_opt->value_repr);
            auto result = field.schema->validate(str_input, state);
            if (result.is_ok()) {
                return result.value();
            }
            auto& err = result.error();
            if (err.has_line_errors()) {
                auto mutable_err = const_cast<ValError*>(&err);
                combined_errors.merge(std::move(*mutable_err));
            } else if (err.is_internal()) {
                auto new_err = ValError::line_error(
                    ErrorType(ErrorType::Kind::CustomError),
                    state.location(),
                    entry_opt->value_repr
                );
                combined_errors.merge(std::move(new_err));
            }
        }
        return std::nullopt;
    }

    // Old method kept for compatibility
    std::shared_ptr<void> validate_field_value(
        const ValidatedDict& dict,
        const std::string& key,
        const FieldInfo& field,
        ValidationState& state,
        ValError& combined_errors
    ) {
        auto* json_dict = dynamic_cast<const JsonValidatedDict*>(&dict);
        if (json_dict) {
            auto element_opt = json_dict->get_element(key);
            if (element_opt) {
                auto field_input = JsonInput::create_from_element(*element_opt);
                auto result = field.schema->validate(*field_input, state);
                if (result.is_ok()) {
                    return result.value();
                }
                auto& err = result.error();
                if (err.is_omit()) {
                    return nullptr;
                } else if (err.has_line_errors()) {
                    // Merge the validation error
                    auto mutable_err = const_cast<ValError*>(&err);
                    combined_errors.merge(std::move(*mutable_err));
                } else if (err.is_internal()) {
                    auto new_err = ValError::line_error(
                        ErrorType(ErrorType::Kind::CustomError),
                        state.location(),
                        err.internal_message()
                    );
                    combined_errors.merge(std::move(new_err));
                }
                return nullptr;
            }
        }

        // Fallback: use the dict entry's string representation
        auto entry_opt = dict.get(key);
        if (entry_opt) {
            StringInput str_input(entry_opt->value_repr);
            auto result = field.schema->validate(str_input, state);
            if (result.is_ok()) {
                return result.value();
            }
            auto& err = result.error();
            if (err.has_line_errors()) {
                auto mutable_err = const_cast<ValError*>(&err);
                combined_errors.merge(std::move(*mutable_err));
            } else if (err.is_internal()) {
                auto new_err = ValError::line_error(
                    ErrorType(ErrorType::Kind::CustomError),
                    state.location(),
                    entry_opt->value_repr
                );
                combined_errors.merge(std::move(new_err));
            }
        }
        return nullptr;
    }

    void handle_extra_fields(
        const ValidatedDict& dict,
        const std::set<std::string>& used_keys,
        ValidatedModelFieldsOutput& output,
        ValidationState& state,
        ValError& combined_errors
    ) {
        if (extra_behavior_ == ExtraBehavior::Ignore) {
            return;
        }

        // Try JsonValidatedDict path
        auto* json_dict = dynamic_cast<const JsonValidatedDict*>(&dict);
        if (json_dict) {
            for (const auto& key : json_dict->keys()) {
                if (used_keys.count(key)) {
                    continue;
                }

                if (extra_behavior_ == ExtraBehavior::Forbid) {
                    auto element_opt = json_dict->get_element(key);
                    std::string input_repr = "...";
                    if (element_opt) {
                        auto tmp_input = JsonInput::create_from_element(*element_opt);
                        input_repr = tmp_input->as_error_value().repr;
                    }
                    state.push_loc(key);
                    auto err = ValError::line_error(
                        ErrorType(ErrorType::Kind::ExtraForbidden),
                        state.location(),
                        input_repr
                    );
                    state.pop_loc();
                    combined_errors.merge(std::move(err));
                    continue;
                }

                // ExtraBehavior::Allow
                auto element_opt = json_dict->get_element(key);
                if (!element_opt) continue;

                auto field_input = JsonInput::create_from_element(*element_opt);

                if (extras_validator_) {
                    state.push_loc(key);
                    field_input->set_current_location(state.location());
                    auto result = extras_validator_->validate(*field_input, state);
                    state.pop_loc();
                    if (result.is_ok()) {
                        ValidatedModelFieldsOutput::FieldValue fv;
                        fv.value = result.value();
                        fv.type_name = extras_validator_->name();
                        output.extra[key] = std::move(fv);
                        output.fields_set.insert(key);
                    } else {
                        combined_errors.merge(std::move(result.error()));
                    }
                } else {
                    ValidatedModelFieldsOutput::FieldValue fv;
                    fv.value = std::make_shared<std::string>(
                        field_input->as_error_value().repr
                    );
                    fv.type_name = "str";
                    output.extra[key] = std::move(fv);
                    output.fields_set.insert(key);
                }
            }
            return;
        }

#ifdef HAS_PYBIND11
        // Try PythonValidatedDict path
        auto* py_dict = dynamic_cast<const PythonValidatedDict*>(&dict);
        if (py_dict) {
            for (const auto& key : py_dict->keys()) {
                if (used_keys.count(key)) {
                    continue;
                }

                if (extra_behavior_ == ExtraBehavior::Forbid) {
                    auto py_obj_opt = py_dict->get_object(key);
                    std::string input_repr = "...";
                    if (py_obj_opt) {
                        PythonInput tmp_input(*py_obj_opt);
                        input_repr = tmp_input.as_error_value().repr;
                    }
                    state.push_loc(key);
                    auto err = ValError::line_error(
                        ErrorType(ErrorType::Kind::ExtraForbidden),
                        state.location(),
                        input_repr
                    );
                    state.pop_loc();
                    combined_errors.merge(std::move(err));
                    continue;
                }

                // ExtraBehavior::Allow
                auto py_obj_opt = py_dict->get_object(key);
                if (!py_obj_opt) continue;

                PythonInput field_input(*py_obj_opt);

                if (extras_validator_) {
                    state.push_loc(key);
                    field_input.set_current_location(state.location());
                    auto result = extras_validator_->validate(field_input, state);
                    state.pop_loc();
                    if (result.is_ok()) {
                        ValidatedModelFieldsOutput::FieldValue fv;
                        fv.value = result.value();
                        fv.type_name = extras_validator_->name();
                        output.extra[key] = std::move(fv);
                        output.fields_set.insert(key);
                    } else {
                        combined_errors.merge(std::move(result.error()));
                    }
                } else {
                    // Store the raw Python object wrapped in shared_ptr
                    ValidatedModelFieldsOutput::FieldValue fv;
                    fv.value = std::make_shared<py::object>(*py_obj_opt);
                    fv.type_name = "py_object";
                    output.extra[key] = std::move(fv);
                    output.fields_set.insert(key);
                }
            }
            return;
        }
#endif
    }

    std::unordered_map<std::string, FieldInfo> fields_;
    std::vector<std::string> field_order_;  // Fields in declaration order
    ExtraBehavior extra_behavior_ = ExtraBehavior::Ignore;
    std::shared_ptr<Validator> extras_validator_;
    std::string model_name_;
    bool from_attributes_ = false;  // from_attributes setting from schema
};

// ============================================================================
// TypedDictValidator - validates typed dict structures
// ============================================================================
class TypedDictValidator : public ModelFieldsValidator {
public:
    TypedDictValidator() : total_(true) {}

    TypedDictValidator(
        std::unordered_map<std::string, FieldInfo> fields,
        ExtraBehavior extra_behavior = ExtraBehavior::Ignore,
        bool total = true
    )
        : ModelFieldsValidator(std::move(fields), extra_behavior)
        , total_(total)
    {
        name_ = "typed-dict";
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto dict_result = input.validate_dict(state.strict_or(false));
        if (dict_result.is_err()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DictType),
                state.location(),
                input.as_error_value().repr
            );
        }

        auto dict = std::move(dict_result.value());
        return validate_dict(std::move(dict), input, state);
    }

    std::string name() const override { return name_; }

    bool total() const { return total_; }
    void set_total(bool t) { total_ = t; }

private:
    std::string name_ = "typed-dict";
    bool total_ = true;
};

// ============================================================================
// ModelValidator - validates model instances
// ============================================================================
class ModelValidator : public Validator {
public:
    ModelValidator() = default;

    ModelValidator(
        std::shared_ptr<Validator> fields_validator,
        std::string class_name = "Model",
        bool frozen = false,
        bool custom_init = false,
        bool root_model = false
    )
        : fields_validator_(std::move(fields_validator))
        , class_name_(std::move(class_name))
        , frozen_(frozen)
        , custom_init_(custom_init)
        , root_model_(root_model)
    {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (!fields_validator_) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "ModelValidator: no fields validator set"
            );
        }
        return fields_validator_->validate(input, state);
    }

    ValResult<std::shared_ptr<void>> validate_assignment(
        const Input& input,
        const std::string& field_name,
        ValidationState& state
    ) override {
        if (!fields_validator_) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "ModelValidator: no fields validator set"
            );
        }
        return fields_validator_->validate_assignment(input, field_name, state);
    }

    std::string name() const override { return "model"; }

    const std::shared_ptr<Validator>& fields_validator() const { return fields_validator_; }
    const std::string& class_name() const { return class_name_; }
    bool frozen() const { return frozen_; }
    bool root_model() const { return root_model_; }

    void set_fields_validator(std::shared_ptr<Validator> v) { fields_validator_ = std::move(v); }
    void set_class_name(const std::string& name) { class_name_ = name; }
    void set_frozen(bool f) { frozen_ = f; }

private:
    std::shared_ptr<Validator> fields_validator_;
    std::string class_name_ = "Model";
    bool frozen_ = false;
    bool custom_init_ = false;
    bool root_model_ = false;
};

// ============================================================================
// DataclassValidator - validates dataclass instances
// ============================================================================
struct DataclassFieldInfo {
    std::string name;
    std::shared_ptr<Validator> schema;
    bool kw_only = false;
    bool frozen = false;
    std::string init_only_name;  // alias for init
};

class DataclassValidator : public Validator {
public:
    DataclassValidator() = default;

    DataclassValidator(
        std::vector<DataclassFieldInfo> fields,
        std::string class_name = "Dataclass",
        bool frozen = false,
        bool post_init = false,
        ExtraBehavior extra_behavior = ExtraBehavior::Ignore
    )
        : fields_(std::move(fields))
        , class_name_(std::move(class_name))
        , frozen_(frozen)
        , post_init_(post_init)
        , extra_behavior_(extra_behavior)
    {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto dict_result = input.validate_dict(state.strict_or(false));
        if (dict_result.is_err()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DataclassType),
                state.location(),
                input.as_error_value().repr
            );
        }

        auto dict = std::move(dict_result.value());
        ValidatedModelFieldsOutput output;
        std::set<std::string> fields_set;
        ValError combined_errors(ValError::Kind::LineErrors);

        auto* json_dict = dynamic_cast<const JsonValidatedDict*>(dict.get());
        std::set<std::string> used_keys;

        for (const auto& field : fields_) {
            state.push_loc(field.name);

            std::string lookup_key = field.init_only_name.empty() ? field.name : field.init_only_name;
            bool found = false;
            std::shared_ptr<void> validated_value;

            if (json_dict) {
                auto element_opt = json_dict->get_element(lookup_key);
                if (element_opt) {
                    found = true;
                    used_keys.insert(lookup_key);
                    auto field_input = JsonInput::create_from_element(*element_opt);
                    auto result = field.schema->validate(*field_input, state);
                    if (result.is_ok()) {
                        validated_value = result.value();
                    } else {
                        auto& err = result.error();
                        if (err.has_line_errors()) {
                            auto mutable_err = const_cast<ValError*>(&err);
                            combined_errors.merge(std::move(*mutable_err));
                        } else if (err.is_internal()) {
                            auto new_err = ValError::line_error(
                                ErrorType(ErrorType::Kind::CustomError),
                                state.location(),
                                field_input->as_error_value().repr
                            );
                            combined_errors.merge(std::move(new_err));
                        }
                    }
                }
            }

            if (!found && json_dict && lookup_key != field.name) {
                if (json_dict->has_key(field.name)) {
                    found = true;
                    used_keys.insert(field.name);
                    auto element_opt = json_dict->get_element(field.name);
                    if (element_opt) {
                        auto field_input = JsonInput::create_from_element(*element_opt);
                        auto result = field.schema->validate(*field_input, state);
                        if (result.is_ok()) {
                            validated_value = result.value();
                        }
                    }
                }
            }

            if (!found) {
                auto err = ValError::line_error(
                    PydanticKnownError::missing(),
                    state.location(),
                    input.as_error_value().repr
                );
                combined_errors.merge(std::move(err));
            } else if (validated_value) {
                ValidatedModelFieldsOutput::FieldValue fv;
                fv.value = validated_value;
                if (field.schema) {
                    fv.type_name = field.schema->name();
                }
                output.fields[field.name] = std::move(fv);
                fields_set.insert(field.name);
            }

            state.pop_loc();
        }

        // Handle extra fields
        if (json_dict && extra_behavior_ == ExtraBehavior::Forbid) {
            for (const auto& key : json_dict->keys()) {
                if (used_keys.count(key)) continue;
                auto element_opt = json_dict->get_element(key);
                std::string repr = "...";
                if (element_opt) {
                    auto tmp = JsonInput::create_from_element(*element_opt);
                    repr = tmp->as_error_value().repr;
                }
                auto err = ValError::line_error(
                    ErrorType(ErrorType::Kind::ExtraForbidden),
                    state.location(),
                    repr
                );
                combined_errors.merge(std::move(err));
            }
        }

        if (combined_errors.has_line_errors() && !combined_errors.line_errors().empty()) {
            return combined_errors;
        }

        auto result = std::make_shared<ValidatedModelFieldsOutput>();
        result->fields = std::move(output.fields);
        result->extra = std::move(output.extra);
        result->fields_set = std::move(fields_set);

        return ValResult<std::shared_ptr<void>>(result);
    }

    std::string name() const override { return "dataclass"; }

    const std::vector<DataclassFieldInfo>& fields() const { return fields_; }
    const std::string& class_name() const { return class_name_; }
    bool frozen() const { return frozen_; }

    void set_fields(std::vector<DataclassFieldInfo> f) { fields_ = std::move(f); }
    void set_class_name(const std::string& name) { class_name_ = name; }
    void set_frozen(bool f) { frozen_ = f; }
    void set_extra_behavior(ExtraBehavior eb) { extra_behavior_ = eb; }

private:
    std::vector<DataclassFieldInfo> fields_;
    std::string class_name_ = "Dataclass";
    bool frozen_ = false;
    bool post_init_ = false;
    ExtraBehavior extra_behavior_ = ExtraBehavior::Ignore;
};

} // namespace pydantic_core
