#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/py_compat.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/string_input.hpp"
// Note: python_input.hpp is included in the .cpp file that uses it
// to avoid pybind11 dependency in test targets
#ifdef HAS_PYBIND11
#include "pydantic_core/python_input.hpp"
#endif
#include <memory>
#include <functional>
#include <string>
#include <utility>
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
    std::string alias;                         // Primary/display alias (first key of first path)
    // Validation lookup paths. Each path is a sequence of keys (strings; ints
    // stored as their string form). A single-key path is a flat alias; a
    // multi-key path is an AliasPath (nested lookup). Multiple paths are
    // AliasChoices (first match wins).
    std::vector<std::vector<std::string>> validation_paths;
    bool has_alias = false;                     // Whether an alias is set (true even if "")
    py::object default_factory = py::none();   // Python callable for default_factory
    py::object default_py_obj = py::none();    // Complex Python object default (callables, etc.)
    bool default_factory_takes_data = false;   // Whether factory receives validated data dict
    // Rust: defaults are used raw unless validate_default is set (field or
    // config); when set, the default is validated and errors are reported.
    bool validate_default = false;

    std::string display_name() const {
        return has_alias ? alias : name;
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

    // Resolve a lookup path (sequence of keys) against the input dict.
    // The first key is looked up in the dict; subsequent keys navigate into
    // the nested value (dict key or list/tuple index). Returns the resolved
    // value, or std::nullopt if any step fails.
    static std::optional<py::object> resolve_path(ValidatedDict& dict, const std::vector<std::string>& path) {
        if (path.empty()) return std::nullopt;
        auto first = dict.get_value(path[0]);
        if (!first) return std::nullopt;
        py::object current = *first;
        for (size_t i = 1; i < path.size(); i++) {
            const std::string& key = path[i];
            if (py::isinstance<py::dict>(current)) {
                py::dict d = current.cast<py::dict>();
                if (!d.contains(py::str(key))) return std::nullopt;
                current = d[py::str(key)];
            } else if (py::isinstance<py::list>(current)) {
                try {
                    long idx = std::stol(key);
                    py::list l = current.cast<py::list>();
                    if (idx < 0 || idx >= (long)l.size()) return std::nullopt;
                    current = l[(size_t)idx];
                } catch (...) { return std::nullopt; }
            } else if (py::isinstance<py::tuple>(current)) {
                try {
                    long idx = std::stol(key);
                    py::tuple t = current.cast<py::tuple>();
                    if (idx < 0 || idx >= (long)t.size()) return std::nullopt;
                    current = t[(size_t)idx];
                } catch (...) { return std::nullopt; }
            } else {
                return std::nullopt;
            }
        }
        return current;
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
    {
        // Record every field so validation iterates over all of them.  (The
        // add_field() path records fields as they are added; this constructor
        // receives a complete map up front.)
        for (auto& [name, info] : fields_) {
            field_order_.push_back(name);
        }
    }

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
                    // Same lookup-key set as validate_dict (Rust semantics).
                    // For from_attributes, use the first key of each path
                    // (flat attribute access).
                    std::vector<std::string> lookup_keys;
                    if (validate_by_alias_) {
                        for (const auto& p : field.validation_paths) {
                            if (!p.empty()) lookup_keys.push_back(p[0]);
                        }
                    }
                    if (!field.has_alias || validate_by_name_) {
                        lookup_keys.push_back(name);
                    }
                    std::unordered_set<std::string> seen_keys;
                    for (const auto& key : lookup_keys) {
                        if (!seen_keys.insert(key).second) continue;
                        try {
                            py::object value = obj.attr(key.c_str());
                            filtered[py::str(key)] = value;
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

        // Partial mode: compute the last key of the input dict (Rust:
        // typed_dict.rs partial_last_key).  Only the field whose first lookup
        // key matches this key keeps partial mode active; its line errors are
        // suppressed when the field is optional.
        std::optional<std::string> partial_last_key;
        if (state.is_partial()) {
            auto keys = dict->keys();
            if (!keys.empty()) {
                partial_last_key = keys.back();
            }
        }

        // Scope a dict of validated fields as state.data while fields are
        // validated (Rust: scoped_set_data(model_dict)).  Python callable
        // validators read it back as ValidationInfo.data so V1-style
        // validators see previously-validated fields.
        py::dict model_data;
        ScopedValidationData data_scope(state, py::object(model_data));
        const std::optional<std::string> outer_field_name = state.field_name();

        auto add_to_data = [&model_data](const std::string& n,
                                         const ValidatedModelFieldsOutput::FieldValue& fv) {
            if (!fv.value) return;
            try {
                model_data[py::str(n)] = value_to_python_with_type(fv.value, fv.type_name);
            } catch (...) {}
        };

        for (const auto& name : field_order_) {
            const auto& field = fields_.at(name);
            // Error location uses the alias when loc_by_alias (default true),
            // else the field name (Rust: loc_by_alias config).
            std::string loc_name = (loc_by_alias_ && field.has_alias) ? field.alias : name;
            state.push_loc(loc_name);
            // Expose the current field name to validators via ValidationInfo
            // (Rust: scoped_set_field_name).  Restored after the loop.
            state.set_field_name(name);

            // Runtime by_alias/by_name (from model_validate) override the
            // config-level settings.
            bool use_by_alias = validate_by_alias_;
            bool use_by_name = validate_by_name_;
            if (state.by_alias().has_value()) use_by_alias = state.by_alias().value();
            if (state.by_name().has_value()) use_by_name = state.by_name().value();
            // Build lookup paths (Rust LookupPathCollection semantics):
            // - each validation path is a lookup path when validate_by_alias
            //   (default true);
            // - the field name is a lookup path only when there is no alias
            //   or validate_by_name is set.
            std::vector<std::vector<std::string>> lookup_paths;
            if (use_by_alias) {
                for (const auto& p : field.validation_paths) {
                    lookup_paths.push_back(p);
                }
            }
            if (!field.has_alias || use_by_name) {
                lookup_paths.push_back({name});
            }

            // Try each path in order; first match wins.
            bool has_entry = false;
            std::optional<py::object> resolved_value;
            for (const auto& path : lookup_paths) {
                auto resolved = resolve_path(*dict, path);
                if (resolved) {
                    has_entry = true;
                    resolved_value = *resolved;
                    used_keys.insert(path[0]);
                    break;
                }
            }

            if (has_entry) {
                // Partial mode: this field is the last partial key if its
                // first lookup key matches the input dict's last key (Rust:
                // typed_dict.rs is_last_partial).
                bool is_last_partial = false;
                if (partial_last_key.has_value()) {
                    // The first lookup path's first key is the primary lookup key.
                    if (!lookup_paths.empty() && !lookup_paths[0].empty()) {
                        is_last_partial = (lookup_paths[0][0] == *partial_last_key);
                    }
                }
                auto validate_result = validate_field_value_from_object(*resolved_value, field, state, combined_errors, is_last_partial);

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
                    add_to_data(name, output.fields.at(name));
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
                        apply_field_default(raw, field, state, fv, combined_errors);
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
                    add_to_data(name, output.fields.at(name));
                } else if (!field.default_py_obj.is_none()) {
                    // Complex Python object default (callable result, date,
                    // timedelta, etc.) — used raw unless validate_default.
                    ValidatedModelFieldsOutput::FieldValue fv;
                    apply_field_default(field.default_py_obj, field, state, fv, combined_errors);
                    output.fields[name] = std::move(fv);
                    output.field_order.push_back(name);
                    add_to_data(name, output.fields.at(name));
                } else if (!field.default_value_str.empty()) {
                    ValidatedModelFieldsOutput::FieldValue fv;
                    // Reconstruct the Python default object from the stored
                    // JSON text (primitives round-trip; complex types use the
                    // default_py_obj path instead).
                    py::object raw = py::none();
                    auto parse_result = parse_json(field.default_value_str);
                    if (parse_result.is_ok()) {
                        try {
                            raw = parse_result.value()->as_python_object();
                        } catch (...) {}
                    }
                    apply_field_default(raw, field, state, fv, combined_errors);
                    output.fields[name] = std::move(fv);
                    output.field_order.push_back(name);
                    add_to_data(name, output.fields.at(name));
                }
            }

            state.pop_loc();
        }

        // Restore the outer field name so enclosing validators (e.g. an
        // after-function reading ValidationInfo.field_name) don't observe a
        // stale per-field name.
        state.set_field_name_opt(outer_field_name);

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

    // Apply a field default (Rust semantics): used raw unless
    // FieldInfo::validate_default is set, in which case the default is run
    // through the field schema and failures are reported as line errors at
    // the field's location (input = the default value itself).
    void apply_field_default(const py::object& raw, const FieldInfo& field,
                             ValidationState& state,
                             ValidatedModelFieldsOutput::FieldValue& fv,
                             ValError& combined_errors) {
        if (!field.validate_default || !field.schema) {
            fv.value = std::make_shared<py::object>(raw);
            fv.type_name = "py_object";
            return;
        }
        try {
            PythonInput py_in(raw);
            auto r = field.schema->validate(py_in, state);
            if (r.is_ok()) {
                fv.value = r.value();
                fv.type_name = field_type_name(field.schema);
                return;
            }
            combined_errors.merge(std::move(r.error()));
        } catch (...) {
        }
        fv.value = std::make_shared<py::object>(raw);
        fv.type_name = "py_object";
    }

    // Rust ModelFieldsValidator::validate_assignment: validate ONLY the
    // assigned field against its schema, with state.data scoped to the model
    // dict minus that field.  input_dict already contains the new value.
    // Returns the updated full dict (py::object).
    ValResult<std::shared_ptr<void>> validate_assignment(
        const py::object& obj, const std::string& field_name,
        const py::object& field_value, ValidationState& state) override {
        return validate_assignment_impl(obj.cast<py::dict>(), field_name, field_value, state);
    }

    ValResult<std::shared_ptr<void>> validate_assignment_impl(
        const py::dict& input_dict, const std::string& field_name,
        const py::object& field_value, ValidationState& state) {
        auto it = fields_.find(field_name);
        if (it == fields_.end()) {
            // Unknown field: extras behavior decides
            if (extra_behavior_ == ExtraBehavior::Allow) {
                py::dict updated = input_dict;
                if (extras_validator_) {
                    PythonInput py_in(field_value);
                    py_in.set_current_location(state.location());
                    auto r = extras_validator_->validate(py_in, state);
                    if (r.is_err()) return r.error();
                    updated[py::str(field_name)] =
                        value_to_python_with_type(r.value(), field_type_name(extras_validator_));
                } else {
                    updated[py::str(field_name)] = field_value;
                }
                return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(updated));
            }
            ErrorType err(ErrorType::Kind::NoSuchAttribute, "attribute", field_name);
            Location loc;
            loc.push(field_name);
            return ValError::line_error(err, loc, py::repr(field_value).cast<std::string>());
        }

        const FieldInfo& field = it->second;
        if (field.frozen) {
            ErrorType err(ErrorType::Kind::FrozenField);
            Location loc;
            loc.push(field.name);
            return ValError::line_error(err, loc, py::repr(field_value).cast<std::string>());
        }
        if (!field.schema) {
            py::dict updated = input_dict;
            updated[py::str(field_name)] = field_value;
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(updated));
        }

        // data scope: everything except the assigned field (V1 behaviour)
        py::dict data_dict;
        for (auto item : input_dict) {
            if (py::str(item.first).cast<std::string>() != field_name) {
                data_dict[item.first] = item.second;
            }
        }
        ScopedValidationData data_scope(state, py::object(data_dict));
        state.set_field_name(field.name);
        PythonInput py_in(field_value);
        py_in.set_current_location(state.location());
        auto result = field.schema->validate(py_in, state);
        state.set_field_name_opt(std::nullopt);

        if (result.is_err()) {
            // attach outer location (the field name)
            ValError err = result.error();
            if (err.has_line_errors()) {
                for (auto& le : err.line_errors()) {
                    le->location.prepend(field.name);
                }
            }
            return err;
        }

        py::dict updated = input_dict;
        updated[py::str(field_name)] =
            value_to_python_with_type(result.value(), field_type_name(field.schema));
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(updated));
    }

    void set_extra_behavior(ExtraBehavior eb) { extra_behavior_ = eb; }
    void set_model_name(const std::string& name) { model_name_ = name; }
    void set_extras_validator(std::shared_ptr<Validator> v) { extras_validator_ = std::move(v); }
    void set_extras_keys_validator(std::shared_ptr<Validator> v) { extras_keys_validator_ = std::move(v); }
    void set_from_attributes(bool value) { from_attributes_ = value; }
    bool from_attributes() const { return from_attributes_; }
    void set_validate_by_name(bool value) { validate_by_name_ = value; }
    void set_validate_by_alias(bool value) { validate_by_alias_ = value; }
    void set_loc_by_alias(bool value) { loc_by_alias_ = value; }

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

    // Validate a pre-resolved py::object value (used for AliasPath/AliasChoices
    // where the value was navigated out of the input dict).
    // When is_last_partial && !field.required, line errors are suppressed
    // (Rust: partial mode drops errors for the last partial key on optional
    // fields, omitting the field from the output).
    std::optional<std::shared_ptr<void>> validate_field_value_from_object(
        const py::object& value,
        const FieldInfo& field,
        ValidationState& state,
        ValError& combined_errors,
        bool is_last_partial = false
    ) {
        // Partial mode: suppress line errors for the last partial key on
        // optional fields (Rust: typed_dict.rs error suppression).
        bool suppress_errors = is_last_partial && !field.required;
        if (state.coerce_strings() && py::isinstance<py::str>(value)) {
            StringInput str_input(py::str(value).cast<std::string>());
            str_input.set_current_location(state.location());
            auto result = field.schema->validate(str_input, state);
            if (result.is_ok()) return result.value();
            auto& err = result.error();
            if (err.is_omit()) return std::nullopt;
            else if (err.has_line_errors()) {
                if (!suppress_errors) {
                    auto mutable_err = const_cast<ValError*>(&err);
                    combined_errors.merge(std::move(*mutable_err));
                }
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
        PythonInput field_input(value);
        field_input.set_current_location(state.location());
        auto result = field.schema->validate(field_input, state);
        if (result.is_ok()) return result.value();
        auto& err = result.error();
        if (err.is_omit()) return std::nullopt;
        else if (err.has_line_errors()) {
            if (!suppress_errors) {
                auto mutable_err = const_cast<ValError*>(&err);
                combined_errors.merge(std::move(*mutable_err));
            }
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
    // Alias lookup mode (Rust LookupPathCollection semantics):
    // - validate_by_alias (default true): the alias(es) are lookup keys.
    // - validate_by_name (default false): the field name is a lookup key
    //   only when there is no alias or validate_by_name is set.
    // - loc_by_alias (default true): error locations use the alias, else name.
    bool validate_by_alias_ = true;
    bool validate_by_name_ = false;
    bool loc_by_alias_ = true;
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
        // Result conversion asks this validator for the stored value's type name
        // after validate() returns, and the instance-reuse path below stores a
        // PyObjectWrapper rather than the model's usual output, so record it.
        last_result_name_.clear();

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
                        last_result_name_ = "py_object_wrapper";
                        return ValResult<std::shared_ptr<void>>(std::make_shared<PyObjectWrapper>(obj));
                    }
                } else {
                    // Input is a model instance of a different class — reject with model_type error
                    // (unless it's a dict, which is valid model input). A root model is
                    // different: its input is the root value, validated against the inner
                    // schema, so a non-RootModel value (e.g. the root type's own instance)
                    // falls through to the fields validator rather than being rejected.
                    if (!root_model_ && !py::isinstance<py::dict>(obj) && py_hasattr(obj, "__pydantic_validator__")) {
                        ErrorType err(ErrorType::Kind::ModelType);
                        err.context()["class_name"] = class_name_.empty() ? "Model" : class_name_;
                        auto line_err = std::make_shared<ValLineError>(ValLineError{err, state.location(), input.as_error_value().repr});
                        line_err->raw_input_obj = obj;
                        return ValError::line_errors({std::move(line_err)});
                    }
                }
            }
        }

        return fields_validator_->validate(input, state);
    }

    // Rust ModelValidator::validate_assignment: re-validate the whole model
    // data with the assigned value substituted (single-field validation
    // happens in the fields validator), then write attributes back.
    ValResult<std::shared_ptr<void>> validate_assignment(
        const py::object& obj, const std::string& field_name,
        const py::object& field_value, ValidationState& state) override {
        state.in_assignment = true;
        if (frozen_) {
            return ValError::line_error(ErrorType(ErrorType::Kind::FrozenInstance),
                                        state.location(), py::repr(field_value).cast<std::string>());
        }
        if (root_model_) {
            if (field_name != "root") {
                ErrorType err(ErrorType::Kind::NoSuchAttribute, "attribute", field_name);
                Location loc;
                loc.push(field_name);
                return ValError::line_error(err, loc, py::repr(field_value).cast<std::string>());
            }
            state.set_field_name("root");
            PythonInput py_in(field_value);
            auto r = fields_validator_->validate(py_in, state);
            state.set_field_name_opt(std::nullopt);
            if (r.is_err()) return r.error();
            py::object out = value_to_python_with_type(r.value(), effective_result_name());
            py::module_::import("builtins").attr("object").attr("__setattr__")(
                obj, py::str("root"), out);
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(obj));
        }

        // input_dict = __dict__ + __pydantic_extra__ + new value
        // input_dict = copy of __dict__ + __pydantic_extra__ + new value
        py::dict input_dict;
        {
            py::object d = obj.attr("__dict__");
            if (!d.is_none() && py::isinstance<py::dict>(d)) {
                py::dict src = d.cast<py::dict>();
                for (auto kv : src) {
                    input_dict[kv.first] = kv.second;
                }
            }
        }
        py::object existing_extra = py::none();
        if (py_hasattr(obj, "__pydantic_extra__")) {
            existing_extra = obj.attr("__pydantic_extra__");
            if (!existing_extra.is_none() && py::isinstance<py::dict>(existing_extra)) {
                py::dict exd = existing_extra.cast<py::dict>();
                for (auto kv : exd) {
                    input_dict[kv.first] = kv.second;
                }
            }
        }
        input_dict[py::str(field_name)] = field_value;

        // Route through the virtual chain so nested function-before/wrap
        // validators around the fields schema run first (Rust semantics).
        std::function<ModelFieldsValidator*(Validator*)> resolve =
            [&](Validator* v) -> ModelFieldsValidator* {
            if (!v) return nullptr;
            if (auto* mf = dynamic_cast<ModelFieldsValidator*>(v)) return mf;
            return resolve(v->inner_validator().get());
        };
        ModelFieldsValidator* fields_impl = resolve(fields_validator_.get());
        if (!fields_impl) {
            return ValError::line_error(ErrorType(ErrorType::Kind::CustomError),
                                        state.location(), "model fields validator missing");
        }
        auto res = fields_validator_->validate_assignment(
            py::object(input_dict), field_name, field_value, state);
        if (res.is_err()) return res.error();

        std::shared_ptr<py::object> updated_ptr =
            std::static_pointer_cast<py::object>(res.value());
        if (!updated_ptr || !py::isinstance<py::dict>(*updated_ptr)) {
            return ValError::line_error(ErrorType(ErrorType::Kind::CustomError),
                                        state.location(), "assignment validation returned non-dict");
        }
        py::dict updated = updated_ptr->cast<py::dict>();

        // Split dunders / extras out of the updated dict
        py::object new_extra = py::none();
        bool is_declared = fields_impl->fields().find(field_name) != fields_impl->fields().end();
        if (updated.contains("__pydantic_extra__")) {
            new_extra = updated[py::str("__pydantic_extra__")];
            updated.attr("pop")(py::str("__pydantic_extra__"), py::none());
        }
        if (!is_declared) {
            // assigned extra: route into __pydantic_extra__
            if (new_extra.is_none()) new_extra = py::dict();
            if (!py::isinstance<py::dict>(new_extra)) new_extra = py::dict(new_extra);
            new_extra.cast<py::dict>()[py::str(field_name)] = updated[py::str(field_name)];
            updated.attr("pop")(py::str(field_name), py::none());
        }
        updated.attr("pop")(py::str("__pydantic_fields_set__"), py::none());
        updated.attr("pop")(py::str("__pydantic_defaults__"), py::none());

        // Replace model __dict__ contents
        py::object d2 = obj.attr("__dict__");
        if (!d2.is_none() && py::isinstance<py::dict>(d2)) {
            py::dict dd = d2.cast<py::dict>();
            dd.clear();
            for (auto kv : updated) {
                dd[kv.first] = kv.second;
            }
        }
        auto setattr_fn = py::module_::import("builtins").attr("object").attr("__setattr__");
        if (!py_hasattr(obj, "__pydantic_private__")) {
            setattr_fn(obj, py::str("__pydantic_private__"), py::none());
        }
        py::object final_extra = new_extra.is_none()
            ? (existing_extra.is_none() ? py::object(py::none()) : existing_extra)
            : new_extra;
        setattr_fn(obj, py::str("__pydantic_extra__"), final_extra);
        py::object fs = py_hasattr(obj, "__pydantic_fields_set__")
            ? py::object(obj.attr("__pydantic_fields_set__")) : py::object(py::none());
        if (!fs.is_none() && py_hasattr(fs, "add")) {
            fs.attr("add")(py::str(field_name));
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(obj));
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

    // Result conversion adds exactly one "maybe_wrapper:" marker; a name that
    // already carries one would survive substr() and match no handler.
    static std::string strip_wrapper_marker(const std::string& name) {
        constexpr std::string_view prefix = "maybe_wrapper:";
        if (name.rfind(prefix, 0) == 0) return name.substr(prefix.size());
        return name;
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
                // (recursively resolved for nested root models). Must go through
                // root_model_inner_name(): the inner validator's name() can
                // disagree with what it stores (a tagged union stores a
                // py::object but reports "tagged-union").
                std::string inner = root_model_inner_name();
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

    // The wrapped fields validator (for reaches_fields_result traversal).
    std::shared_ptr<Validator> inner_validator() const override { return fields_validator_; }

    const std::shared_ptr<Validator>& fields_validator() const { return fields_validator_; }
    const std::string& class_name() const { return class_name_; }
    bool frozen() const { return frozen_; }
    bool root_model() const { return root_model_; }

    std::string root_model_inner_name() const override {
        // A root model reports its root value's type (e.g. "int"), which would
        // make result conversion cast a PyObjectWrapper to that type; the
        // instance-reuse path has to override it.
        if (root_model_ && !last_result_name_.empty()) return last_result_name_;
        if (root_model_ && fields_validator_) {
            // Resolve recursively through nested root models
            auto inner = fields_validator_->root_model_inner_name();
            if (!inner.empty()) {
                return strip_wrapper_marker(inner);
            }
            // effective_result_name(), not name(): a tagged union stores a
            // py::object while name() reports "tagged-union", so using name()
            // made conversion cast a py::object to the union's nominal type.
            // The wrapper marker stays out: callers add exactly one.
            return strip_wrapper_marker(fields_validator_->effective_result_name());
        }
        return "";
    }

    void set_fields_validator(std::shared_ptr<Validator> v) { fields_validator_ = std::move(v); }
    void set_class_name(const std::string& name) { class_name_ = name; }
    void set_frozen(bool f) { frozen_ = f; }
    void set_revalidate(RevalidateInstances r) { revalidate_ = r; }

private:
    std::string last_result_name_;
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
