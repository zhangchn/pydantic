#include "pydantic_core/schema_validator.hpp"
#include "pydantic_core/py_compat.hpp"
#include "pydantic_core/errors.hpp"
#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/url_types.hpp"
#include "pydantic_core/validators/model_fields.hpp"
#include "pydantic_core/validators/complex.hpp"
#include "pydantic_core/py_time.hpp"
#include <memory>
#include <pybind11/stl.h>
#include <stdexcept>

namespace py = pybind11;
namespace pydantic_core {

static void reraise_if_internal(const ValError& err);

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

    // Extract post_init method name from schema
    if (schema.contains("post_init")) {
        post_init_ = py::str(schema["post_init"]).cast<std::string>();
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
    if (config.contains("strict") && !config["strict"].is_none()) {
        config_.strict = config["strict"].cast<bool>();
    }
    config_.extra_behavior = std::nullopt;
    config_.from_attributes = std::nullopt;
    if (config.contains("from_attributes") && !config["from_attributes"].is_none()) {
        config_.from_attributes = config["from_attributes"].cast<bool>();
    }
    if (config.contains("extra_fields_behavior") && !config["extra_fields_behavior"].is_none()) {
        std::string eb = config["extra_fields_behavior"].cast<std::string>();
        if (eb == "allow") config_.extra_behavior = ExtraBehavior::Allow;
        else if (eb == "forbid") config_.extra_behavior = ExtraBehavior::Forbid;
        else config_.extra_behavior = ExtraBehavior::Ignore;
    }
    config_.cache_strings = StringCacheMode::All;
    if (config.contains("val_temporal_unit") && !config["val_temporal_unit"].is_none()) {
        bool ok = false;
        config_.val_temporal_unit = timestamp_unit_from_string(
            config["val_temporal_unit"].cast<std::string>(), &ok);
    }

    // Parse hide_input_in_errors from the Python config dict
    if (config.contains("hide_input_in_errors") && !config["hide_input_in_errors"].is_none()) {
        hide_input_in_errors_ = config["hide_input_in_errors"].cast<bool>();
    }
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

    // Parse from_attributes from JSON config if available
    if (!config_json_.empty()) {
        simdjson::dom::parser parser;
        auto doc = parser.parse(config_json_);
        if (!doc.error()) {
            auto fa = doc["from_attributes"];
            if (!fa.error() && fa.value().is_bool()) {
                config_.from_attributes = fa.value().get_bool();
            }
        }
    }
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
            if (vname == "model" || vname == "model-fields" || vname == "typed-dict") {
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
// Validates in "strings mode": string values always coerce regardless of
// strict (matching Rust's StringInput semantics) and extras keep raw strings.
py::object SchemaValidator::validate_strings_object(const py::object& input,
                                                    std::optional<bool> strict,
                                                    std::optional<ExtraBehavior> extra,
                                                    PartialMode allow_partial) {
    if (!validator_) {
        throw std::runtime_error("Validator not initialized");
    }

    if (py::isinstance<py::str>(input)) {
        // Single string value: validate via StringInput so string values
        // always coerce regardless of strict (Rust StringInput semantics)
        StringInput str_input(input.cast<std::string>());
        ValidationState state(config_);
        if (strict.has_value()) {
            state.set_strict(*strict);
        }
        if (extra.has_value()) {
            state.set_extra_behavior(*extra);
        }
        state.set_coerce_strings(true);
        state.set_allow_partial(allow_partial);
        auto result = validator_->validate(str_input, state);
        if (result.is_ok()) {
            return result_to_python(result.value());
        }
        auto err = prepare_error(result.error(), InputType::String);
        throw err;
    }

    // Dict (or any other) input: validate as-is in strings mode.  Declared
    // fields coerce string values via StringInput; extra fields keep the raw
    // string value.
    return validate_python_object(input, strict, extra, std::nullopt, py::none(), /*coerce_strings=*/true, py::none(), std::nullopt, std::nullopt, allow_partial);
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

// Forward declaration (defined below)
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name);

py::object SchemaValidator::get_default_value(std::optional<bool> strict, py::object context) {
    (void)strict;
    (void)context;

    if (!validator_) {
        return py::none();
    }

    ValidationState state(config_);
    auto result = validator_->default_value(state);

    if (!result.is_ok()) {
        // Omit / no default
        return py::none();
    }

    auto value = result.value();
    if (!value) {
        return py::none();
    }

    // The default is stored as a live Python object by WithDefaultValidator.
    try {
        auto* py_obj = static_cast<py::object*>(value.get());
        if (py_obj) return *py_obj;
    } catch (...) {}

    // Fallback: convert by the validator's effective result type.
    return value_to_python_with_type(value, validator_->effective_result_name());
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
// Gather an instance's data into a dict, whether it lives in __dict__
// (regular models/dataclasses) or in __slots__ (slots dataclasses).
static py::dict collect_instance_data(const py::object& obj) {
    py::dict result;
    if (py::isinstance<py::dict>(obj)) {
        return obj.cast<py::dict>();
    }
    if (py_hasattr(obj, "__dict__")) {
        py::object d = obj.attr("__dict__");
        if (py::isinstance<py::dict>(d)) {
            for (auto item : d.cast<py::dict>()) {
                result[item.first] = item.second;
            }
            return result;
        }
    }
    // Slots dataclass: no __dict__; collect data slots (skip dunders).
    if (py_hasattr(obj, "__slots__")) {
        py::object slots_obj = obj.attr("__slots__");
        if (py::isinstance<py::tuple>(slots_obj)) {
            for (auto s : slots_obj.cast<py::tuple>()) {
                std::string name = s.cast<std::string>();
                if (name.rfind("__", 0) == 0) continue;
                if (py_hasattr(obj, name.c_str())) {
                    result[py::str(name)] = obj.attr(name.c_str());
                }
            }
        }
    }
    return result;
}

py::object SchemaValidator::validate_assignment_object(const py::object& obj,
                                                       const std::string& field_name,
                                                       const py::object& field_value) {
    if (!validator_) {
        throw std::runtime_error("Validator not initialized");
    }

    // Modern path: model schemas implement Rust-style per-field assignment
    // validation through the validate_assignment virtual (function wrappers
    // forward to the model; unknown schemas report "not supported" and we
    // fall back to the legacy whole-model re-validation below).
    if (auto base = validator_->base_validator()) {
        ValidationState state(config_);
        auto r = base->validate_assignment(obj, field_name, field_value, state);
        if (!r.is_ok()) {
            bool unsupported = false;
            if (r.error().has_line_errors()) {
                auto les = r.error().line_errors();
                if (les.size() == 1 &&
                    les[0]->error_type.type_name() == "custom_error" &&
                    les[0]->input_value.find("not supported for this schema") != std::string::npos) {
                    unsupported = true;
                }
            }
            if (!unsupported) {
#ifdef HAS_PYBIND11
                try {
                    py::module_ m = py::module_::import("__main__");
                    // Assignment errors carry their own correct inputs;
                    // skip the Python-side raw-input override entirely.
                    m.attr("_last_raw_input") = obj;
                    m.attr("_last_assignment_error") = py::bool_(true);
                    py::list err_objs;
                    py::list err_ctx_objs;
                    if (r.error().has_line_errors()) {
                        for (const auto& le : r.error().line_errors()) {
                            err_objs.append(le->raw_error_obj.ptr() ? py::object(le->raw_error_obj) : py::none());
                            py::dict ctx_d;
                            for (const auto& [k, v] : le->error_type.context_objects()) {
                                ctx_d[py::str(k)] = v;
                            }
                            err_ctx_objs.append(std::move(ctx_d));
                        }
                    }
                    m.attr("_last_error_objs") = err_objs;
                    m.attr("_last_error_ctx_objs") = err_ctx_objs;
                } catch (...) {}
#endif
                reraise_if_internal(r.error());
                auto err = prepare_error(r.error(), InputType::Python);
                throw err;
            }
        } else {
            try {
                return *std::static_pointer_cast<py::object>(r.value());
            } catch (...) {
                // fall through to legacy path on conversion failure
            }
        }
    }

    // Check if the field exists on the object
    bool field_exists = false;
    if (py_hasattr(obj, field_name.c_str())) {
        field_exists = true;
    } else if (py_hasattr(obj, "__pydantic_extra__")) {
        py::object extra = obj.attr("__pydantic_extra__");
        if (!extra.is_none() && py::isinstance<py::dict>(extra)) {
            py::dict extra_dict = extra.cast<py::dict>();
            if (extra_dict.contains(py::str(field_name.c_str()))) {
                field_exists = true;
            }
        }
    }

    // If field doesn't exist, check if extra fields are allowed
    if (!field_exists) {
        // Check if extra fields are allowed by checking config or __pydantic_extra__
        bool extra_allowed = false;

        // First check __pydantic_extra__ - if it's a dict (even empty), extra='allow'
        if (py_hasattr(obj, "__pydantic_extra__")) {
            py::object extra = obj.attr("__pydantic_extra__");
            if (py::isinstance<py::dict>(extra)) {
                extra_allowed = true;
            }
        }

        // If not determined yet, check the config
        if (!extra_allowed && py_hasattr(obj, "__pydantic_config__")) {
            py::object config = obj.attr("__pydantic_config__");
            if (py::isinstance<py::dict>(config)) {
                py::dict config_dict = config.cast<py::dict>();
                // pydantic v2 uses extra_fields_behavior; accept legacy "extra"
                for (const char* key : {"extra_fields_behavior", "extra"}) {
                    if (extra_allowed) break;
                    if (config_dict.contains(key)) {
                        py::object extra_val = config_dict[key];
                        if (py::isinstance<py::str>(extra_val)) {
                            std::string extra_str = extra_val.cast<std::string>();
                            if (extra_str == "allow") {
                                extra_allowed = true;
                            }
                        }
                    }
                }
            }
        }

        // This validator's own config also carries extra_fields_behavior
        if (!extra_allowed && config_.extra_behavior.has_value() &&
            *config_.extra_behavior == ExtraBehavior::Allow) {
            extra_allowed = true;
        }

        if (!extra_allowed) {
            // Raise no_such_attribute error
            ErrorType err_type(ErrorType::Kind::NoSuchAttribute, "attribute", field_name);
            Location loc;
            loc.push(field_name);
            auto val_err = ValError::line_error(err_type, loc, py::repr(field_value).cast<std::string>());
            throw ValidationError(title_, InputType::Python, val_err, hide_input_in_errors_);
        }

        // Extra fields are allowed - validate and set
        py::dict input_dict = collect_instance_data(obj);

        py::dict updated_dict;
        for (auto item : input_dict) {
            updated_dict[item.first] = item.second;
        }
        updated_dict[py::str(field_name.c_str())] = field_value;

        py::object validated_result = validate_python_object(updated_dict);

        // Extract the validated value
        py::object validated_value = field_value;
        if (py::isinstance<py::dict>(validated_result)) {
            py::dict result_dict = validated_result.cast<py::dict>();
            if (result_dict.contains(py::str(field_name.c_str()))) {
                validated_value = result_dict[py::str(field_name.c_str())];
            }
        } else if (py_hasattr(validated_result, "__pydantic_extra__")) {
            py::object extra = validated_result.attr("__pydantic_extra__");
            if (!extra.is_none() && py::isinstance<py::dict>(extra)) {
                py::dict extra_dict = extra.cast<py::dict>();
                if (extra_dict.contains(py::str(field_name.c_str()))) {
                    validated_value = extra_dict[py::str(field_name.c_str())];
                }
            }
        }

        // Set the value: models keep extras in __pydantic_extra__
        py::str fname(field_name.c_str());
        if (!py::isinstance<py::dict>(obj) && py_hasattr(obj, "__pydantic_extra__")) {
            py::object ex = obj.attr("__pydantic_extra__");
            if (ex.is_none()) {
                ex = py::dict();
                py::setattr(obj, "__pydantic_extra__", ex);
            }
            ex.cast<py::dict>()[fname] = validated_value;
            py::object fs = obj.attr("__pydantic_fields_set__");
            if (!fs.is_none() && py_hasattr(fs, "add")) {
                fs.attr("add")(fname);
            }
            return validated_value;
        }
        if (py::isinstance<py::dict>(obj)) {
            obj.cast<py::dict>()[fname] = validated_value;
        } else {
            py::module_::import("builtins")
                .attr("object").attr("__setattr__")(obj, fname, validated_value);
        }
        return validated_value;
    }

    // Field exists - validate and set it
    py::dict input_dict = collect_instance_data(obj);

    py::dict updated_dict;
    for (auto item : input_dict) {
        updated_dict[item.first] = item.second;
    }
    // Merge existing extras so re-validation doesn't drop them
    if (py_hasattr(obj, "__pydantic_extra__")) {
        py::object ex = obj.attr("__pydantic_extra__");
        if (!ex.is_none() && py::isinstance<py::dict>(ex)) {
            py::dict exd = ex.cast<py::dict>();
            for (auto item : exd) {
                if (!updated_dict.contains(item.first)) {
                    updated_dict[item.first] = item.second;
                }
            }
        }
    }
    updated_dict[py::str(field_name.c_str())] = field_value;

    py::object validated_result = validate_python_object(updated_dict);

    py::object validated_value = field_value;  // Default to original value
    if (py::isinstance<py::dict>(validated_result)) {
        py::dict result_dict = validated_result.cast<py::dict>();
        if (result_dict.contains(py::str(field_name.c_str()))) {
            validated_value = result_dict[py::str(field_name.c_str())];
        }
    } else if (py_hasattr(validated_result, field_name.c_str())) {
        validated_value = validated_result.attr(field_name.c_str());
    }

    // Extras live in __pydantic_extra__; declared fields in __dict__
    py::str fname(field_name.c_str());
    bool is_extra_field = false;
    if (!py::isinstance<py::dict>(obj) && py_hasattr(obj, "__pydantic_extra__")) {
        py::object d = obj.attr("__dict__");
        if (!d.contains(fname)) {
            is_extra_field = true;
        }
    }
    if (is_extra_field) {
        py::object ex = obj.attr("__pydantic_extra__");
        if (ex.is_none()) {
            ex = py::dict();
            py::setattr(obj, "__pydantic_extra__", ex);
        }
        ex.cast<py::dict>()[fname] = validated_value;
        return validated_value;
    }
    if (py::isinstance<py::dict>(obj)) {
        obj.cast<py::dict>()[fname] = validated_value;
    } else {
        py::module_::import("builtins")
            .attr("object").attr("__setattr__")(obj, py::str(field_name.c_str()), validated_value);
    }

    return validated_value;
}

std::string SchemaValidator::validator_display_name() const {
    if (!validator_) {
        return title_;
    }
    try {
        std::string name = validator_->display_name();
        return name.empty() ? title_ : name;
    } catch (...) {
        PyErr_Clear();
        return title_;
    }
}

std::string SchemaValidator::repr() const {
    return "SchemaValidator(title='" + title_ + "')";
}

bool SchemaValidator::is_root_model() const {
    if (validator_) {
        auto inner_name = validator_->root_model_inner_name();
        return inner_name.has_value();
    }
    return false;
}

ValidationError SchemaValidator::prepare_error(const ValError& err, InputType input_type) {
    return ValidationError(title_, input_type, err, hide_input_in_errors_);
}

// Populate self_instance from the snapshot taken by the outermost
// function-after (Rust validate_init semantics): the snapshot is the inner
// constructed model instance; copy its state onto self_instance.  A validator
// returning a foreign instance does NOT overwrite these fields.
bool SchemaValidator::apply_init_snapshot(const py::object& self_instance) {
    py::object snap = std::move(init_snapshot_);
    init_snapshot_ = py::none();
    if (snap.is_none() || !py_hasattr(snap, "__dict__")) return false;
    try {
        py::dict d = self_instance.attr("__dict__");
        py::dict snap_dict = snap.attr("__dict__");
        for (auto item : snap_dict) {
            d[item.first] = item.second;
        }
        if (py_hasattr(snap, "__pydantic_extra__")) {
            py::setattr(self_instance, "__pydantic_extra__", snap.attr("__pydantic_extra__"));
        }
        if (py_hasattr(snap, "__pydantic_fields_set__")) {
            py::setattr(self_instance, "__pydantic_fields_set__", snap.attr("__pydantic_fields_set__"));
        }
        if (!py_hasattr(self_instance, "__pydantic_private__")) {
            py::setattr(self_instance, "__pydantic_private__", py::none());
        }
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// NEW: Native Python object validation (no JSON round-trip)
// ============================================================================

py::object SchemaValidator::validate_python_object(const py::object& input,
                                                   std::optional<bool> strict,
                                                   std::optional<ExtraBehavior> extra,
                                                   std::optional<bool> from_attributes,
                                                   py::object context,
                                                   bool coerce_strings,
                                                   py::object self_instance,
                                                   std::optional<bool> by_alias,
                                                   std::optional<bool> by_name,
                                                   PartialMode allow_partial,
                                                   InputType input_type) {
    if (!validator_) {
        throw std::runtime_error("Validator not initialized");
    }

    // Create PythonInput wrapping the PyObject
    PythonInput py_input(input);

    // Create validation state
    ValidationState state(config_);
    state.set_input_type(input_type);
    if (strict.has_value()) {
        state.set_strict(*strict);
    }
    if (extra.has_value()) {
        state.set_extra_behavior(*extra);
    }
    if (from_attributes.has_value()) {
        state.set_from_attributes(*from_attributes);
    }
    if (by_alias.has_value()) {
        state.set_by_alias(*by_alias);
    }
    if (by_name.has_value()) {
        state.set_by_name(*by_name);
    }
    if (!context.is_none()) {
        state.set_context_py(context);
    }
    state.set_coerce_strings(coerce_strings);
    state.set_allow_partial(allow_partial);
    // Seed the top-level input identity so the outermost fields-position
    // function-after can snapshot pre-func fields (BaseModel.__init__ path).
    state.set_top_input_ptr(static_cast<const void*>(input.ptr()));
    if (!self_instance.is_none()) {
        state.set_init_self_py(self_instance);
    }
    init_snapshot_ = py::none();

    // Validate using the unified Input interface
    auto result = validator_->validate(py_input, state);

    if (result.is_ok()) {
        init_snapshot_ = state.init_fields_snapshot();
        return result_to_python(result.value());
    } else {
#ifdef HAS_PYBIND11
        // Store raw Python input on module for error dict reconstruction.
        // This mirrors Rust's approach where as_val_error(input) keeps Py<PyAny>.
        // register_exception creates a new Python exception from C++ object, so
        // the C++ ValidationError errors() lambda can't run — but module-level
        // storage survives the throw/catch cycle.
        try {
            py::module_ m = py::module_::import("__main__");
            m.attr("_last_raw_input") = input;
            // Collect Python exception objects from line errors, index-aligned
            // with the error details, so the Python wrapper can attach them to
            // ctx['error'] for value_error/assertion_error entries.
            py::list err_objs;
            py::list err_ctx_objs;
            if (result.error().has_line_errors()) {
                for (const auto& le : result.error().line_errors()) {
                    err_objs.append(le->raw_error_obj.ptr() ? py::object(le->raw_error_obj) : py::none());
                    py::dict ctx_d;
                    for (const auto& [k, v] : le->error_type.context_objects()) {
                        ctx_d[py::str(k)] = v;
                    }
                    err_ctx_objs.append(std::move(ctx_d));
                }
            }
            m.attr("_last_error_objs") = err_objs;
            m.attr("_last_error_ctx_objs") = err_ctx_objs;
        } catch (...) {
            // Silently ignore if module state setting fails
        }
#endif
        reraise_if_internal(result.error());
        // Rust turns an Omit that escapes the whole schema into a SchemaError
        // (ValidationError::omit_error) at the binding boundary.
        if (result.error().is_omit()) {
            throw SchemaError(
                "Uncaught Omit error, please check your usage of `default` validators.");
        }
        auto err = prepare_error(result.error(), InputType::Python);
        throw err;
    }
}

// Re-raise an InternalErr carrying the original Python exception (Rust
// propagates RuntimeError etc. unchanged instead of wrapping them).
static void reraise_if_internal(const ValError& err) {
#ifdef HAS_PYBIND11
    if (err.kind() == ValError::Kind::InternalErr && err.has_internal_py_err()) {
        py::object exc = err.internal_py_err();
        PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(exc.ptr())), exc.ptr());
        throw py::error_already_set();
    }
#else
    (void)err;
#endif
}

// Helper: convert shared_ptr<void> to Python object using type name
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name) {
    if (!value) {
        return py::none();
    }

    // Handle "maybe_wrapper:" prefix - check if it's a PyObjectWrapper via TypedResult.
    // The value might be:
    // 1. PyObjectWrapper* (from ModelValidator returning existing instance, revalidate='never')
    // 2. py::object* (from function-after/wrap/plain validators)
    // 3. ValidatedModelFieldsOutput* (from normal model validation)
    // For model effective types (1 and 3), both inherit from TypedResult, so we can safely
    // use static_cast and check result_type(). For "py_object" effective type (2), we can't
    // safely check because py::object doesn't inherit from TypedResult.
    std::string effective_type = type_name;
    if (type_name.rfind("maybe_wrapper:", 0) == 0) {
        effective_type = type_name.substr(14);  // strlen("maybe_wrapper:") == 14
        if (effective_type == "model" || effective_type == "model-fields" || effective_type == "typed-dict") {
            // Both PyObjectWrapper and ValidatedModelFieldsOutput inherit from TypedResult.
            try {
                auto* typed = static_cast<TypedResult*>(value.get());
                if (typed && std::string(typed->result_type()) == "py_object") {
                    auto* wrapper = static_cast<PyObjectWrapper*>(typed);
                    return wrapper->obj;
                }
            } catch (...) {}
        }
        // For "py_object" effective type, we can't safely distinguish PyObjectWrapper from
        // raw py::object. This shouldn't happen in practice because ModelValidator with
        // function-after fields_validator returns "maybe_wrapper:py_object", and the result
        // is either PyObjectWrapper (revalidate='never') or py::object (from function).
        // We handle this by falling through to the py_object handling below.
    }

    // Handle raw Python object (used for extra fields, is-instance, is-subclass)
    if (effective_type == "py_object") {
        try {
            auto* py_obj = static_cast<py::object*>(value.get());
            if (py_obj) return *py_obj;
        } catch (...) {}
        return py::none();
    }

    // Handle py_object_wrapper (PyObjectWrapper from ModelValidator returning existing instance)
    if (effective_type == "py_object_wrapper") {
        try {
            auto* wrapper = static_cast<PyObjectWrapper*>(value.get());
            if (wrapper) return wrapper->obj;
        } catch (...) {}
        return py::none();
    }

    // py_raw_object: PyObject* stored by is-instance/is-subclass validators
    if (effective_type == "py_raw_object") {
        try {
            auto* raw = static_cast<PyObject*>(value.get());
            if (raw) return py::reinterpret_borrow<py::object>(raw);
        } catch (...) {}
        return py::none();
    }

    // Handle date/time types
    if (effective_type == "date") {
        try {
            auto* ed = static_cast<EitherDate*>(value.get());
            if (ed) {
                auto& d = ed->value;
                return py_date_object(d);
            }
        } catch (...) {}
        return py::none();
    }
    if (effective_type == "time") {
        try {
            auto* et = static_cast<EitherTime*>(value.get());
            if (et) {
                auto& t = et->value;
                return py_time_object(t);
            }
        } catch (...) {}
        return py::none();
    }
    if (effective_type == "datetime") {
        try {
            auto* edt = static_cast<EitherDateTime*>(value.get());
            if (edt) {
                // Preserve the original Python datetime object (keeps named
                // tzinfo like America/Los_Angeles instead of a fixed offset).
                if (!edt->original_obj.is_none()) {
                    return edt->original_obj;
                }
                auto& dt = edt->value;
                return py_datetime_object(dt);
            }
        } catch (...) {}
        return py::none();
    }
    if (effective_type == "timedelta") {
        try {
            auto* etd = static_cast<EitherTimedelta*>(value.get());
            if (etd) {
                auto& td = etd->value;
                py::object datetime_mod = py::module_::import("datetime");
                return datetime_mod.attr("timedelta")(
                    py::arg("days") = td.days,
                    py::arg("seconds") = td.seconds,
                    py::arg("microseconds") = td.microseconds);
            }
        } catch (...) {}
        return py::none();
    }

    // Use type_name to determine how to cast
    // For wrapper types, try all scalar types since we don't know the inner type
    // (NOT for model/typed-dict/dataclass — those have their own handler below)
    bool try_all = (effective_type == "nullable"
                    || effective_type == "lax-or-strict" || effective_type == "json-or-python"
                    || effective_type == "tagged-union" || effective_type == "union"
                    || effective_type == "any-of");

    // For "any", try specific type casts based on actual value content
    bool is_any = (effective_type == "any");

    // Helper: match base type name, including constrained- variants
    auto matches_type = [&](const std::string& base) -> bool {
        return effective_type == base || effective_type == ("constrained-" + base) ||
               effective_type == (base + "-constrained") || effective_type == ("constr-" + base) ||
               effective_type == (base + "-constr");
    };

    // For wrapper types with try_all, check list/container types first to avoid
    // UB from static_cast<std::string*> on a py::list object
    if (try_all && matches_type("list")) {
        try {
            auto* lst = static_cast<py::list*>(value.get());
            if (lst) return *lst;
        } catch (...) {}
    }

    if (try_all || matches_type("str") || effective_type == "string") {
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

    if (try_all || matches_type("int") || effective_type == "int64") {
        // EitherInt wrapper (produced by the int validators) supports both the
        // int64 and uint64 range, so check it first to preserve values larger
        // than 2^63-1.
        try {
            auto* ei = static_cast<EitherInt*>(value.get());
            if (ei) {
                if (auto i64 = ei->as_i64()) {
                    return py::int_(*i64);
                }
                if (auto u64 = ei->as_u64()) {
                    return py::reinterpret_steal<py::object>(
                        PyLong_FromUnsignedLongLong(*u64));
                }
            }
        } catch (...) {}
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
    if (!effective_type.empty() && (effective_type == "model" || effective_type == "model-fields" || effective_type == "typed-dict")) {
        // Directly convert ValidatedModelFieldsOutput to dict.
        // Note: "dataclass" is NOT included — the dataclass validator returns
        // a py::object (fields dict or constructed instance), not fields output.
        try {
            auto* mfo = static_cast<ValidatedModelFieldsOutput*>(value.get());
            if (mfo) {
                py::dict out;
                for (const auto& key : mfo->field_order) {
                    const auto& fv = mfo->fields.at(key);
                    out[py::str(key)] = value_to_python_with_type(fv.value, fv.type_name);
                }
                if (effective_type == "typed-dict") {
                    // TypedDict: Rust returns a plain dict — no
                    // __pydantic_fields_set__, and extra fields are merged
                    // directly into the output dict (not a separate
                    // __pydantic_extra__ dict).
                    for (const auto& [key, fv] : mfo->extra) {
                        out[py::str(key)] = value_to_python_with_type(fv.value, fv.type_name);
                    }
                } else {
                    // Model/model-fields: attach __pydantic_fields_set__ for
                    // exclude_unset support and keep extras in a separate
                    // __pydantic_extra__ dict.
                    py::set fields_set;
                    for (const auto& fname : mfo->fields_set) {
                        fields_set.add(py::str(fname));
                    }
                    out[py::str("__pydantic_fields_set__")] = std::move(fields_set);
                    if (!mfo->extra.empty()) {
                        py::dict extra_dict;
                        for (const auto& [key, fv] : mfo->extra) {
                            extra_dict[py::str(key)] = value_to_python_with_type(fv.value, fv.type_name);
                        }
                        out[py::str("__pydantic_extra__")] = std::move(extra_dict);
                    }
                }
                return std::move(out);
            }
        } catch (...) {}
    }

    // Url type - convert to Python Url object using pybind11 cast
    if (effective_type == "url") {
        try {
            auto* url_ptr = static_cast<Url*>(value.get());
            if (url_ptr) {
                return py::cast(*url_ptr);
            }
        } catch (...) {}
    }

    // MultiHostUrl type
    if (effective_type == "multi-host-url") {
        try {
            auto* url_ptr = static_cast<MultiHostUrl*>(value.get());
            if (url_ptr) {
                return py::cast(*url_ptr);
            }
        } catch (...) {}
    }

    // UUID type — stored as std::string by UuidValidator
    if (effective_type == "uuid") {
        try {
            auto* s = static_cast<std::string*>(value.get());
            if (s) {
                py::object uuid_mod = py::module_::import("uuid");
                return uuid_mod.attr("UUID")(*s);
            }
        } catch (...) {}
    }

    // Literal type — the validator now returns the stored expected Python
    // object (e.g. an enum member); fall back to plain-string results from
    // the legacy JSON path.
    if (effective_type == "literal") {
        try {
            auto* obj = static_cast<py::object*>(value.get());
            if (obj) return *obj;
        } catch (...) {}
        try {
            auto* s = static_cast<std::string*>(value.get());
            if (s) return py::str(*s);
        } catch (...) {}
    }

    // "any" type — try all casts (numbers before strings)
    if (is_any) {
        // AnyValidator now stores values as py::object to preserve identity and type
        try {
            auto* obj = static_cast<py::object*>(value.get());
            if (obj) {
                return *obj;
            }
        } catch (...) {}
        // Fallback for legacy string storage
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
    if (effective_type == "enum" || effective_type == "enum-constrained") {
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

    // "dict" type — return the py::dict directly
    if (matches_type("dict")) {
        try {
            auto* dct = static_cast<py::dict*>(value.get());
            if (dct) {
                return *dct;
            }
        } catch (...) {}
    }

    // For function-after/before/wrap/plain validators, check py::object*
    bool is_function_type = (effective_type == "function-after" || effective_type == "function-before" ||
                             effective_type == "function-wrap" || effective_type == "function-plain" ||
                             effective_type == "call" || effective_type == "arguments" || effective_type == "dataclass" ||
                             effective_type == "callable" || effective_type == "set" || effective_type == "frozenset" ||
                             effective_type == "tuple");
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

    // Determine the type from the validator name.  The effective result name
    // reflects the ACTUAL stored value type: models may wrap function
    // validators (producing py::object), and call validators with a return
    // validator produce that validator's result type rather than a py::object.
    std::string vname;
    if (validator_) {
        vname = validator_->effective_result_name();
    }

    // Strip "maybe_wrapper:" prefix for the model check below
    std::string effective_vname = vname;
    if (vname.rfind("maybe_wrapper:", 0) == 0) {
        effective_vname = vname.substr(14);
    }

    // For model-like validators, try ValidatedModelFieldsOutput.
    // Note: "dataclass" is NOT included — the dataclass validator returns a
    // py::object (fields dict or constructed instance), not fields output.
    if (!effective_vname.empty() && (effective_vname == "model" || effective_vname == "model-fields" || effective_vname == "typed-dict")) {
        // Check if it's a PyObjectWrapper first (from revalidate_instances='never' with existing instance)
        try {
            auto* typed = static_cast<TypedResult*>(result.get());
            if (typed && std::string(typed->result_type()) == "py_object") {
                auto* wrapper = static_cast<PyObjectWrapper*>(typed);
                return wrapper->obj;
            }
        } catch (...) {}

        // Otherwise, expect ValidatedModelFieldsOutput
        try {
            auto* mfo = static_cast<ValidatedModelFieldsOutput*>(result.get());
            if (mfo) {
                py::dict out;
                for (const auto& key : mfo->field_order) {
                    const auto& fv = mfo->fields.at(key);
                    py::object py_val = value_to_python_with_type(fv.value, fv.type_name);
                    out[py::str(key)] = py_val;
                }
                if (effective_vname == "typed-dict") {
                    // TypedDict: Rust returns a plain dict — no
                    // __pydantic_fields_set__, and extra fields are merged
                    // directly into the output dict (not a separate
                    // __pydantic_extra__ dict).
                    for (const auto& [key, fv] : mfo->extra) {
                        out[py::str(key)] = value_to_python_with_type(fv.value, fv.type_name);
                    }
                } else {
                    // Model/model-fields: attach __pydantic_fields_set__ for
                    // exclude_unset support and keep extras in a separate
                    // __pydantic_extra__ dict.
                    py::set fields_set;
                    for (const auto& fname : mfo->fields_set) {
                        fields_set.add(py::str(fname));
                    }
                    out[py::str("__pydantic_fields_set__")] = std::move(fields_set);
                    if (!mfo->extra.empty()) {
                        py::dict extra_dict;
                        for (const auto& [key, fv] : mfo->extra) {
                            extra_dict[py::str(key)] = value_to_python_with_type(fv.value, fv.type_name);
                        }
                        out[py::str("__pydantic_extra__")] = std::move(extra_dict);
                    }
                }

                return std::move(out);
            }
        } catch (...) {}
    }

    // Use result_to_python_with_type with the validator's name for proper type dispatch
    if (!vname.empty()) {
        return value_to_python_with_type(result, vname);
    }

    // Fallback
    return py::none();
}

} // namespace pydantic_core
