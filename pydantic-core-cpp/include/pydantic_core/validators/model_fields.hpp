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

// Forward declaration — defined in schema_validator.cpp
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name);

// Forward declaration — defined below in this file
class ModelValidator;

// Base class for type-safe identification of result types
struct TypedResult {
    virtual ~TypedResult() = default;
    virtual const char* result_type() const = 0;
};

// Wrapper for py::object results that can be distinguished from ValidatedModelFieldsOutput
// Used when ModelValidator returns an existing instance (revalidate_instances='never')
// or when function validators return py::object results for model fields.
struct PyObjectWrapper : public TypedResult {
    // Magic number for safe identification without dynamic_cast on void*
    static constexpr uint64_t MAGIC = 0xC0FEE1234BAE1234ULL;
    uint64_t magic = MAGIC;
    py::object obj;
    explicit PyObjectWrapper(py::object o) : obj(std::move(o)) {}
    const char* result_type() const override { return "py_object"; }
};

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
    py::object default_factory = py::none();   // Python callable for default_factory
    py::object default_py_obj = py::none();    // Complex Python object default (callables, etc.)
    bool default_factory_takes_data = false;   // Whether factory receives validated data dict

    std::string display_name() const {
        return alias.empty() ? name : alias;
    }
};

// ============================================================================
// ValidatedModelFieldsOutput - the return value from ModelFieldsValidator
// ============================================================================
struct ValidatedModelFieldsOutput : public TypedResult {
    struct FieldValue {
        std::shared_ptr<void> value;
        std::string type_name;  // "str", "int", "float", "bool", "bytes", "dict", "list", etc.
    };
    std::unordered_map<std::string, FieldValue> fields;  // Validated field values
    std::vector<std::string> field_order;                // Fields in declaration order
    std::vector<std::pair<std::string, FieldValue>> extra; // Extra fields (if allow), insertion order
    std::set<std::string> fields_set;                    // Names of fields that were in input
    std::unordered_map<std::string, py::object> defaults; // Default values for non-required fields

    const char* result_type() const override { return "model_fields"; }
};

// ============================================================================
// ModelFieldsValidator - validates dict input against a set of typed fields
// ============================================================================
class ModelFieldsValidator : public Validator {
public:
    ModelFieldsValidator() = default;

    // For a field schema that is a root model, use the inner validator's name
    // so result conversion dispatches on the actual value type (e.g. "int"
    // instead of "model").  Otherwise use the validator's effective result
    // name — function-wrapper validators produce their inner validator's
    // result type, so the stored type name must match the actual value.
    static std::string field_type_name(const std::shared_ptr<Validator>& schema) {
        if (schema) {
            auto inner = schema->root_model_inner_name();
            if (!inner.empty()) {
                return inner;
            }
            return schema->effective_result_name();
        }
        return "null_schema";
    }

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
        // from_attributes: use the parameter if explicitly set, otherwise use the schema default
        bool use_from_attributes = from_attributes_;
        if (state.from_attributes().has_value()) {
            use_from_attributes = state.from_attributes().value();
        }
        
        // Get dict - if from_attributes is true, extract only known field attributes
        // (only for non-dict inputs; dicts are already in the right format)
        if (use_from_attributes) {
            auto* py_input = dynamic_cast<const PythonInput*>(&input);
            if (py_input && !py::isinstance<py::dict>(py_input->py_object())) {
                const py::object& obj = py_input->py_object();
                py::dict filtered;
                for (const auto& name : field_order_) {
                    const auto& field = fields_.at(name);
                    std::string lookup_key = field.alias.empty() ? name : field.alias;
                    try {
                        py::object value = obj.attr(lookup_key.c_str());
                        filtered[py::str(lookup_key)] = value;
                    } catch (...) {}
                    if (!field.alias.empty() && lookup_key != name) {
                        try {
                            py::object value = obj.attr(name.c_str());
                            filtered[py::str(name)] = value;
                        } catch (...) {}
                    }
                }
                auto dict = std::make_unique<PythonValidatedDict>(filtered);
                return validate_dict(std::move(dict), input, state);
            }
        }
        
        // Regular dict validation — recursion guard using the actual object address
        // to detect cyclic references (same PyObject seen again at a deeper level)
        const void* rec_key = dynamic_cast<const PythonInput*>(&input)
            ? dynamic_cast<const PythonInput*>(const_cast<Input*>(&input))->py_object().ptr()
            : static_cast<const void*>(&input);
        auto rec_entry = state.enter_recursion(rec_key);
        if (!rec_entry.allowed()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::RecursionLoop),
                state.location(),
                "Recursion error - cyclic reference detected"
            );
        }

        auto dict_result = input.validate_dict(state.strict_or(false));
        if (dict_result.is_err()) {
            ErrorType err(ErrorType::Kind::ModelType);
            err.context()["class_name"] = model_name_.empty() ? "Model" : model_name_;
            auto line_err = std::make_shared<ValLineError>(ValLineError{err, state.location(), input.as_error_value().repr});
            // Pass the actual Python object as input for proper serialization
            if (auto* py_input = dynamic_cast<const PythonInput*>(&input)) {
                line_err->raw_input_obj = py_input->py_object();
            }
            return ValError::line_errors({std::move(line_err)});
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
                    // Determine type name from field validator.
                    // The validator's effective_result_name() already includes "maybe_wrapper:"
                    // prefix if it might return a PyObjectWrapper (from revalidate_instances).
                    fv.type_name = field_type_name(field.schema);

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
                } else if (!field.default_factory.is_none()) {
                    // Call default_factory to get the default value
                    ValidatedModelFieldsOutput::FieldValue fv;
                    try {
                        py::object raw;
                        if (field.default_factory_takes_data) {
                            // Build dict of already-validated fields for the factory
                            py::dict data_dict;
                            for (const auto& fname : output.field_order) {
                                auto& fval = output.fields.at(fname);
                                data_dict[py::str(fname)] = value_to_python_with_type(fval.value, fval.type_name);
                            }
                            raw = field.default_factory(data_dict);
                        } else {
                            raw = field.default_factory();
                        }
                        if (field.schema) {
                            PythonInput py_in(raw);
                            auto default_result = field.schema->validate(py_in, state);
                            if (default_result.is_ok()) {
                                fv.value = default_result.value();
                                fv.type_name = field_type_name(field.schema);
                            } else {
                                fv.value = std::make_shared<py::object>(std::move(raw));
                                fv.type_name = "py_object";
                            }
                        } else {
                            fv.value = std::make_shared<py::object>(std::move(raw));
                            fv.type_name = "py_object";
                        }
                    } catch (py::error_already_set& e) {
                        if (field.default_factory_takes_data) {
                            // Let exceptions from data-aware factories propagate (e.g. KeyError)
                            throw;
                        }
                        auto err = ValError::line_error(
                            ErrorType(ErrorType::Kind::CustomError),
                            state.location(),
                            std::string("default_factory failed: ") + e.what()
                        );
                        combined_errors.merge(std::move(err));
                    }
                    output.fields[name] = std::move(fv);
                    output.field_order.push_back(name);
                } else if (!field.default_py_obj.is_none()) {
                    // Complex Python object default (callable, etc.)
                    ValidatedModelFieldsOutput::FieldValue fv;
                    py::object raw = field.default_py_obj;
                    if (field.schema) {
                        PythonInput py_in(raw);
                        auto default_result = field.schema->validate(py_in, state);
                        if (default_result.is_ok()) {
                            fv.value = default_result.value();
                            fv.type_name = field_type_name(field.schema);
                        } else {
                            fv.value = std::make_shared<py::object>(std::move(raw));
                            fv.type_name = "py_object";
                        }
                    } else {
                        fv.value = std::make_shared<py::object>(std::move(raw));
                        fv.type_name = "py_object";
                    }
                    output.fields[name] = std::move(fv);
                    output.field_order.push_back(name);
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
                            fv.type_name = field_type_name(field.schema);
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

        // Populate defaults map for exclude_defaults support
        // Include ALL non-required fields with their default values
        for (const auto& name : field_order_) {
            const auto& field = fields_.at(name);
            if (!field.required) {
                py::object def_val = py::none();
                if (!field.default_py_obj.is_none()) {
                    def_val = field.default_py_obj;
                } else if (!field.default_value_str.empty()) {
                    // Parse via json.loads (handles strings, numbers, bools, null)
                    try {
                        py::object json_mod = py::module_::import("json");
                        def_val = json_mod.attr("loads")(field.default_value_str);
                    } catch (...) {
                        def_val = py::str(field.default_value_str);
                    }
                }
                output.defaults[name] = std::move(def_val);
            }
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
    void set_extras_keys_validator(std::shared_ptr<Validator> v) { extras_keys_validator_ = std::move(v); }
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
                // In strings mode (validate_strings), string values always coerce
                // regardless of strict — match Rust's StringInput semantics.
                if (state.coerce_strings() && py::isinstance<py::str>(*py_obj_opt)) {
                    StringInput str_input(py::str(*py_obj_opt).cast<std::string>());
                    str_input.set_current_location(state.location());
                    auto result = field.schema->validate(str_input, state);
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
        // Call-level extra setting (e.g. validate_strings(extra='allow')) overrides
        // the validator's build-time behavior.
        ExtraBehavior behavior = state.extra_behavior_or(extra_behavior_);
        if (behavior == ExtraBehavior::Ignore) {
            return;
        }

        // Try JsonValidatedDict path
        auto* json_dict = dynamic_cast<const JsonValidatedDict*>(&dict);
        if (json_dict) {
            for (const auto& key : json_dict->keys()) {
                if (used_keys.count(key)) {
                    continue;
                }

                if (behavior == ExtraBehavior::Forbid) {
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

                if (extras_keys_validator_) {
                    // Validate the extra key itself (e.g. max_length on str keys)
                    StringInput key_input(key);
                    state.push_loc(key);
                    key_input.set_current_location(state.location());
                    auto key_result = extras_keys_validator_->validate(key_input, state);
                    state.pop_loc();
                    if (key_result.is_err()) {
                        combined_errors.merge(std::move(key_result.error()));
                        continue;
                    }
                }

                if (extras_validator_) {
                    state.push_loc(key);
                    field_input->set_current_location(state.location());
                    auto result = extras_validator_->validate(*field_input, state);
                    state.pop_loc();
                    if (result.is_ok()) {
                        ValidatedModelFieldsOutput::FieldValue fv;
                        fv.value = result.value();
                        fv.type_name = extras_validator_->name();
                        output.extra.emplace_back(key, std::move(fv));
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
                    output.extra.emplace_back(key, std::move(fv));
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

                if (behavior == ExtraBehavior::Forbid) {
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

                // In strings mode (validate_strings), string values always coerce
                std::unique_ptr<Input> field_input;
                if (state.coerce_strings() && py::isinstance<py::str>(*py_obj_opt)) {
                    field_input = std::make_unique<StringInput>(py::str(*py_obj_opt).cast<std::string>());
                } else {
                    field_input = std::make_unique<PythonInput>(*py_obj_opt);
                }

                if (extras_keys_validator_) {
                    // Validate the extra key itself (e.g. max_length on str keys)
                    PythonInput key_input{py::str(key)};
                    state.push_loc(py::str(key));
                    key_input.set_current_location(state.location());
                    auto key_result = extras_keys_validator_->validate(key_input, state);
                    state.pop_loc();
                    if (key_result.is_err()) {
                        combined_errors.merge(std::move(key_result.error()));
                        continue;
                    }
                }

                if (extras_validator_) {
                    state.push_loc(key);
                    field_input->set_current_location(state.location());
                    auto result = extras_validator_->validate(*field_input, state);
                    state.pop_loc();
                    if (result.is_ok()) {
                        ValidatedModelFieldsOutput::FieldValue fv;
                        fv.value = result.value();
                        fv.type_name = extras_validator_->name();
                        output.extra.emplace_back(key, std::move(fv));
                        output.fields_set.insert(key);
                    } else {
                        combined_errors.merge(std::move(result.error()));
                    }
                } else {
                    // Store the raw Python object wrapped in shared_ptr
                    ValidatedModelFieldsOutput::FieldValue fv;
                    fv.value = std::make_shared<py::object>(*py_obj_opt);
                    fv.type_name = "py_object";
                    output.extra.emplace_back(key, std::move(fv));
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
    std::shared_ptr<Validator> extras_keys_validator_;
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
enum class RevalidateInstances {
    Always,
    Never,
    SubclassInstances
};

class ModelValidator : public Validator {
public:
    ModelValidator() = default;

    ModelValidator(
        std::shared_ptr<Validator> fields_validator,
        std::string class_name = "Model",
        bool frozen = false,
        bool custom_init = false,
        bool root_model = false,
        py::object class_ = py::none(),
        RevalidateInstances revalidate = RevalidateInstances::Never
    )
        : fields_validator_(std::move(fields_validator))
        , class_name_(std::move(class_name))
        , frozen_(frozen)
        , custom_init_(custom_init)
        , root_model_(root_model)
        , class_(std::move(class_))
        , revalidate_(revalidate)
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

        // Check if input is already an instance of the expected class
        if (!class_.is_none()) {
            auto* py_input = dynamic_cast<const PythonInput*>(&input);
            if (py_input) {
                const py::object& obj = py_input->py_object();
                bool is_instance = false;
                try {
                    is_instance = py::isinstance(obj, class_);
                } catch (...) {}

                if (is_instance) {
                    bool should_revalidate = false;
                    switch (revalidate_) {
                        case RevalidateInstances::Always:
                            should_revalidate = true;
                            break;
                        case RevalidateInstances::Never:
                            should_revalidate = false;
                            break;
                        case RevalidateInstances::SubclassInstances:
                            // Revalidate if it's a subclass instance (not exact class)
                            try {
                                py::object obj_type = py::type::of(obj);
                                should_revalidate = !obj_type.is(class_);
                            } catch (...) {
                                should_revalidate = true;
                            }
                            break;
                    }

                    if (!should_revalidate) {
                        // Return the instance as-is, wrapped in PyObjectWrapper
                        return ValResult<std::shared_ptr<void>>(std::make_shared<PyObjectWrapper>(obj));
                    }
                }
            }
        }

        return fields_validator_->validate(input, state);
    }

    // Expected Python class for this model (used by unions to prefer the
    // exact-class branch when the input is already a model instance).
    const py::object& expected_class() const { return class_; }

    // The type name matching this model's actual result value:
    // - models wrapping a function-after/wrap/plain validator produce a
    //   py::object (the Python callable's output)
    // - root models produce their inner validator's value type
    // - regular models produce ValidatedModelFieldsOutput ("model")
    std::string result_dispatch_name() const override {
        if (fields_validator_) {
            std::string n = fields_validator_->name();
            if (n == "function-after" || n == "function-wrap" || n == "function-plain") {
                return "py_object";
            }
        }
        return "";
    }

    std::string effective_result_name() const override {
        std::string base_name;
        if (fields_validator_) {
            std::string n = fields_validator_->name();
            if (n == "function-after" || n == "function-wrap" || n == "function-plain") {
                base_name = "py_object";
            } else if (n == "function-before") {
                // function-before transforms the input, then the inner validator
                // produces the result — so the stored value has the inner
                // validator's type (e.g. ValidatedModelFieldsOutput), not a
                // py::object. Without this, result conversion would misread the
                // value as a py::object and crash.
                base_name = fields_validator_->effective_result_name();
            } else if (root_model_) {
                // Root model result is the inner validator's value type
                // (recursively resolved for nested root models)
                auto inner = fields_validator_->root_model_inner_name();
                if (!inner.empty()) {
                    base_name = inner;
                } else {
                    base_name = n;
                }
            } else {
                base_name = n;
            }
        } else {
            base_name = name();
        }

        // If this model has a class and might return PyObjectWrapper (revalidate != 'always'),
        // prefix with "maybe_wrapper:" so the conversion code checks for it at runtime.
        if (!class_.is_none() && revalidate_ != RevalidateInstances::Always) {
            return "maybe_wrapper:" + base_name;
        }
        return base_name;
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

    std::string root_model_inner_name() const override {
        if (root_model_ && fields_validator_) {
            // Resolve recursively through nested root models
            auto inner = fields_validator_->root_model_inner_name();
            if (!inner.empty()) {
                return inner;
            }
            return fields_validator_->name();
        }
        return "";
    }

    void set_fields_validator(std::shared_ptr<Validator> v) { fields_validator_ = std::move(v); }
    void set_class_name(const std::string& name) { class_name_ = name; }
    void set_frozen(bool f) { frozen_ = f; }
    void set_revalidate(RevalidateInstances r) { revalidate_ = r; }

private:
    std::shared_ptr<Validator> fields_validator_;
    std::string class_name_ = "Model";
    bool frozen_ = false;
    bool custom_init_ = false;
    bool root_model_ = false;
    py::object class_ = py::none();
    RevalidateInstances revalidate_ = RevalidateInstances::Never;
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
                    auto inner = field.schema->root_model_inner_name();
                    fv.type_name = inner.empty() ? field.schema->name() : inner;
                }
                output.fields[field.name] = std::move(fv);
                fields_set.insert(field.name);
            }

            state.pop_loc();
        }

        // Handle extra fields
        ExtraBehavior behavior = state.extra_behavior_or(extra_behavior_);
        if (json_dict && behavior == ExtraBehavior::Forbid) {
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
