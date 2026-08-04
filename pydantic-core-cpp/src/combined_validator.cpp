#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/validators/model_fields.hpp"
#include "pydantic_core/validators/special.hpp"
#include "pydantic_core/validators/functions.hpp"
#include <pybind11/pybind11.h>
#include <simdjson.h>

namespace py = pybind11;

namespace pydantic_core {

// Helper: serialize a schema default value to a JSON string, handling
// values that json.dumps cannot serialize directly (bytes, Enum members).
static std::string py_default_to_json_str(const py::object& py_default) {
    auto default_fn = py::cpp_function([](py::handle o) -> py::object {
        if (py::isinstance<py::bytes>(o)) {
            // Decode UTF-8 (matches ser_json_bytes='utf8' default)
            return py::object(py::reinterpret_borrow<py::object>(o)).attr("decode")("utf-8");
        }
        if (py::hasattr(o, "_value_") && py::hasattr(o, "_name_")) {
            // Enum member — use its name so the enum validator can rebuild it
            return py::getattr(o, "name");
        }
        if (py::hasattr(o, "__dict__")) {
            return py::getattr(o, "__dict__");
        }
        return py::str(py::repr(o));
    });
    return py::module_::import("json").attr("dumps")(py_default, py::arg("default") = default_fn).cast<std::string>();
}

// Helper: convert string to ExtraBehavior
static ExtraBehavior extra_behavior_from_string(const std::string& s) {
    if (s == "allow") return ExtraBehavior::Allow;
    if (s == "forbid") return ExtraBehavior::Forbid;
    return ExtraBehavior::Ignore;
}

// Forward declarations for internal recursive schema parsing
class DefinitionsRegistry;
static std::shared_ptr<Validator> build_from_element(
    const simdjson::dom::element& elem,
    const std::unordered_map<std::string, std::string>& config,
    std::shared_ptr<DefinitionsRegistry> definitions
);

// Overload without definitions (for backwards compatibility)
static std::shared_ptr<Validator> build_from_element(
    const simdjson::dom::element& elem,
    const std::unordered_map<std::string, std::string>& config
);

// Convert a simdjson element to our flat schema dict
static std::unordered_map<std::string, std::string> element_to_dict(const simdjson::dom::element& elem) {
    std::unordered_map<std::string, std::string> result;
    auto obj = elem.get_object();
    if (obj.error()) return result;
    for (auto field : obj.value()) {
        std::string key = std::string(field.key);
        std::string value;
        if (field.value.is_string()) {
            auto sv = field.value.get_string();
            value = std::string(sv.value());
        } else if (field.value.is_bool()) {
            value = field.value.get_bool() ? "true" : "false";
        } else if (field.value.is_int64()) {
            value = std::to_string(field.value.get_int64());
        } else if (field.value.is_uint64()) {
            value = std::to_string(field.value.get_uint64());
        } else if (field.value.is_double()) {
            value = std::to_string(field.value.get_double());
        } else if (field.value.is_null()) {
            value = "null";
        } else {
            // Complex type - serialize to JSON string
            value = simdjson::minify(field.value);
        }
        result[key] = value;
    }
    return result;
}

// Build a validator from a flat schema dict (existing interface)
static std::shared_ptr<Validator> build_from_flat_dict(
    const std::unordered_map<std::string, std::string>& schema,
    const std::unordered_map<std::string, std::string>& config
) {
    SchemaParser parser(schema);
    std::string type = parser.type();

    if (type.empty()) {
        throw SchemaError("Schema missing 'type' field");
    }

    // Basic validators
    if (type == "any") return std::make_shared<AnyValidator>();
    if (type == "none") return std::make_shared<NoneValidator>();
    if (type == "bool") return std::make_shared<BoolValidator>();
    if (type == "int") {
        auto v = std::make_shared<IntValidator>();
        return v;
    }
    if (type == "float") return std::make_shared<FloatValidator>();
    if (type == "str") return std::make_shared<StringValidator>();
    if (type == "bytes") return std::make_shared<BytesValidator>();
    if (type == "list") return std::make_shared<ListValidator>();
    if (type == "dict") return std::make_shared<DictValidator>();
    if (type == "set") return std::make_shared<SetValidator>();
    if (type == "frozenset") return std::make_shared<FrozenSetValidator>();
    if (type == "tuple") return std::make_shared<TupleValidator>();
    if (type == "literal") return std::make_shared<LiteralValidator>();
    if (type == "enum") return std::make_shared<EnumValidator>();
    if (type == "date") return std::make_shared<DateValidator>();
    if (type == "time") return std::make_shared<TimeValidator>();
    if (type == "datetime") return std::make_shared<DatetimeValidator>();
    if (type == "timedelta") return std::make_shared<TimedeltaValidator>();
    if (type == "url") return std::make_shared<UrlValidator>();
    if (type == "multi-host-url") return std::make_shared<MultiHostUrlValidator>();
    if (type == "uuid") return std::make_shared<UuidValidator>();
    if (type == "is-instance") return std::make_shared<IsInstanceValidator>();
    if (type == "is-subclass") return std::make_shared<IsSubclassValidator>();
    if (type == "callable") return std::make_shared<CallableValidator>();
    if (type == "generator") return std::make_shared<AnyValidator>();

    // Complex validators need recursive parsing - handled by build_from_element
    throw SchemaError("Validator type requires recursive parsing: " + type);
}

// Internal recursive schema parser using simdjson elements
static std::shared_ptr<Validator> build_from_element(
    const simdjson::dom::element& elem,
    const std::unordered_map<std::string, std::string>& config,
    std::shared_ptr<DefinitionsRegistry> definitions
) {
    auto obj = elem.get_object();
    if (obj.error()) {
        throw SchemaError("Schema must be a JSON object");
    }

    // Extract type field
    std::string type;
    auto type_val = elem["type"];
    if (type_val.error()) {
        throw SchemaError("Schema missing 'type' field");
    }
    auto type_str = type_val.value().get_string();
    if (type_str.error()) {
        throw SchemaError("'type' must be a string");
    }
    type = std::string(type_str.value());

    // Basic validators - can be built from flat dict
    auto flat_schema = element_to_dict(elem);

    if (type == "any") return std::make_shared<AnyValidator>();
    if (type == "none") return std::make_shared<NoneValidator>();
    if (type == "bool") return std::make_shared<BoolValidator>();

    // Int validator — check for constraints
    if (type == "int") {
        auto ci = [&](const char* key) { return !elem[key].error(); };
        bool has_c = ci("gt") || ci("ge") || ci("lt") || ci("le") || ci("multiple_of");
        if (has_c) {
            auto cv = std::make_shared<ConstrainedIntValidator>();
            auto cfg_s = config.find("strict");
            if (cfg_s != config.end()) cv->strict = (cfg_s->second == "true");
            auto pi = [&](const char* k) -> std::optional<int64_t> {
                auto v = elem[k];
                if (!v.error()) {
                    if (v.value().is_int64()) return v.value().get_int64();
                    if (v.value().is_uint64()) return static_cast<int64_t>(v.value().get_uint64());
                }
                return std::nullopt;
            };
            if (auto v = pi("gt")) cv->gt = *v;
            if (auto v = pi("ge")) cv->ge = *v;
            if (auto v = pi("lt")) cv->lt = *v;
            if (auto v = pi("le")) cv->le = *v;
            if (auto v = pi("multiple_of")) cv->multiple_of = *v;
            return cv;
        }
        return std::make_shared<IntValidator>();
    }

    // Float validator — check for constraints
    if (type == "float") {
        auto ci = [&](const char* key) { return !elem[key].error(); };
        bool has_c = ci("gt") || ci("ge") || ci("lt") || ci("le") || ci("multiple_of");
        if (has_c) {
            auto cv = std::make_shared<ConstrainedFloatValidator>();
            auto cfg_s = config.find("strict");
            if (cfg_s != config.end()) cv->strict = (cfg_s->second == "true");
            auto pf = [&](const char* k) -> std::optional<double> {
                auto v = elem[k];
                if (!v.error()) {
                    if (v.value().is_double()) return v.value().get_double();
                    if (v.value().is_int64()) return static_cast<double>(v.value().get_int64());
                }
                return std::nullopt;
            };
            if (auto v = pf("gt")) cv->gt = *v;
            if (auto v = pf("ge")) cv->ge = *v;
            if (auto v = pf("lt")) cv->lt = *v;
            if (auto v = pf("le")) cv->le = *v;
            if (auto v = pf("multiple_of")) cv->multiple_of = *v;
            return cv;
        }
        return std::make_shared<FloatValidator>();
    }

    // Str validator — check for constraints
    if (type == "str") {
        auto ci = [&](const char* key) { return !elem[key].error(); };
        bool has_c = ci("min_length") || ci("max_length") || ci("pattern") ||
                     ci("strip_whitespace") || ci("to_lower") || ci("to_upper");
        if (has_c) {
            auto cv = std::make_shared<StrConstrainedValidator>();
            auto cfg_s = config.find("strict");
            if (cfg_s != config.end()) cv->strict = (cfg_s->second == "true");
            auto ps = [&](const char* k) -> std::optional<size_t> {
                auto v = elem[k];
                if (!v.error() && v.value().is_uint64()) return static_cast<size_t>(v.value().get_uint64());
                if (!v.error() && v.value().is_int64() && v.value().get_int64() >= 0)
                    return static_cast<size_t>(v.value().get_int64());
                return std::nullopt;
            };
            if (auto v = ps("min_length")) cv->min_length = *v;
            if (auto v = ps("max_length")) cv->max_length = *v;
            auto pat = elem["pattern"];
            if (!pat.error() && pat.value().is_string()) cv->pattern = std::string(pat.value().get_string().value());
            auto sw = elem["strip_whitespace"];
            if (!sw.error() && sw.value().is_bool()) cv->strip_whitespace = sw.value().get_bool();
            auto tl = elem["to_lower"];
            if (!tl.error() && tl.value().is_bool()) cv->to_lower = tl.value().get_bool();
            auto tu = elem["to_upper"];
            if (!tu.error() && tu.value().is_bool()) cv->to_upper = tu.value().get_bool();
            return cv;
        }
        return std::make_shared<StringValidator>();
    }

    // Bytes validator — check for constraints
    if (type == "bytes") {
        auto ci = [&](const char* key) { return !elem[key].error(); };
        bool has_c = ci("min_length") || ci("max_length");
        if (has_c) {
            auto cv = std::make_shared<BytesConstrainedValidator>();
            auto cfg_s = config.find("strict");
            if (cfg_s != config.end()) cv->strict = (cfg_s->second == "true");
            auto ps = [&](const char* k) -> std::optional<size_t> {
                auto v = elem[k];
                if (!v.error() && v.value().is_uint64()) return static_cast<size_t>(v.value().get_uint64());
                if (!v.error() && v.value().is_int64() && v.value().get_int64() >= 0)
                    return static_cast<size_t>(v.value().get_int64());
                return std::nullopt;
            };
            if (auto v = ps("min_length")) cv->min_length = *v;
            if (auto v = ps("max_length")) cv->max_length = *v;
            return cv;
        }
        return std::make_shared<BytesValidator>();
    }

    // List with item validator
    if (type == "list") {
        auto lv = std::make_shared<ListValidator>();
        auto items_schema = elem["items_schema"];
        if (!items_schema.error() && !items_schema.value().is_null()) {
            lv->items_schema = build_from_element(items_schema.value(), config, definitions);
        }
        // Parse min_length/max_length
        auto parse_sz = [&](const char* k) -> std::optional<size_t> {
            auto v = elem[k];
            if (!v.error() && v.value().is_uint64()) return static_cast<size_t>(v.value().get_uint64());
            return std::nullopt;
        };
        if (auto v = parse_sz("min_length")) lv->min_length = *v;
        if (auto v = parse_sz("max_length")) lv->max_length = *v;
        auto cfg_s = config.find("strict");
        if (cfg_s != config.end()) lv->strict = (cfg_s->second == "true");
        return lv;
    }

    // Dict with key/value validators
    if (type == "dict") {
        auto dv = std::make_shared<DictValidator>();
        auto keys_schema = elem["keys_schema"];
        if (!keys_schema.error() && !keys_schema.value().is_null()) {
            dv->keys_schema = build_from_element(keys_schema.value(), config, definitions);
        }
        auto values_schema = elem["values_schema"];
        if (!values_schema.error() && !values_schema.value().is_null()) {
            dv->values_schema = build_from_element(values_schema.value(), config, definitions);
        }
        return dv;
    }
    if (type == "set") return std::make_shared<SetValidator>();
    if (type == "frozenset") return std::make_shared<FrozenSetValidator>();

    // Tuple with positional items
    if (type == "tuple") {
        auto tv = std::make_shared<TupleValidator>();
        auto items_arr = elem["items_schema"];
        if (!items_arr.error() && items_arr.value().is_array()) {
            for (auto item : items_arr.value().get_array().value()) {
                tv->items.push_back(build_from_element(item, config, definitions));
            }
        }
        auto variadic = elem["variadic_item_index"];
        if (!variadic.error() && variadic.value().is_uint64()) {
            tv->variadic = true;
        }
        return tv;
    }

    // Nullable
    if (type == "nullable") {
        auto schema_elem = elem["schema"];
        if (!schema_elem.error()) {
            auto inner = build_from_element(schema_elem.value(), config, definitions);
            return std::make_shared<NullableValidator>(inner);
        }
        return std::make_shared<NullableValidator>();
    }

    // Union
    if (type == "union") {
        auto choices = elem["choices"];
        if (!choices.error() && choices.value().is_array()) {
            std::vector<std::shared_ptr<Validator>> validators;
            for (auto choice : choices.value().get_array().value()) {
                validators.push_back(build_from_element(choice, config, definitions));
            }
            return std::make_shared<UnionValidator>(validators);
        }
        return std::make_shared<UnionValidator>();
    }

    if (type == "tagged-union") return std::make_shared<TaggedUnionValidator>();

    // With default
    if (type == "default") {
        auto schema_elem = elem["schema"];
        std::shared_ptr<Validator> inner;
        if (!schema_elem.error()) {
            inner = build_from_element(schema_elem.value(), config, definitions);
        }

        auto default_elem = elem["default"];
        std::shared_ptr<void> default_value;
        std::string default_value_str;
        if (!default_elem.error()) {
            // Parse default value based on JSON type
            if (default_elem.value().is_string()) {
                default_value = std::make_shared<std::string>(
                    std::string(default_elem.value().get_string().value()));
                default_value_str = std::string(default_elem.value().get_string().value());
            } else if (default_elem.value().is_int64()) {
                default_value = std::make_shared<int64_t>(default_elem.value().get_int64());
            } else if (default_elem.value().is_uint64()) {
                default_value = std::make_shared<uint64_t>(default_elem.value().get_uint64());
            } else if (default_elem.value().is_double()) {
                default_value = std::make_shared<double>(default_elem.value().get_double());
            } else if (default_elem.value().is_bool()) {
                default_value = std::make_shared<bool>(default_elem.value().get_bool());
            } else if (default_elem.value().is_null()) {
                default_value = nullptr;
            } else if (default_elem.value().is_array() || default_elem.value().is_object()) {
                // For complex defaults (arrays, objects), store as JSON string
                // and parse at validation time
                auto raw = simdjson::to_string(default_elem.value());
                default_value_str = std::string(raw);
            }
        }

        if (inner) {
            return std::make_shared<WithDefaultValidator>(inner, default_value, default_value_str);
        }
        return std::make_shared<WithDefaultValidator>(nullptr, default_value, default_value_str);
    }

    // Chain
    if (type == "chain") {
        auto steps = elem["steps"];
        if (!steps.error() && steps.value().is_array()) {
            std::vector<std::shared_ptr<Validator>> validators;
            for (auto step : steps.value().get_array().value()) {
                validators.push_back(build_from_element(step, config, definitions));
            }
            return std::make_shared<ChainValidator>(validators);
        }
        return std::make_shared<ChainValidator>();
    }

    // Lax or strict
    if (type == "lax-or-strict") {
        auto lax_elem = elem["lax_schema"];
        auto strict_elem = elem["strict_schema"];
        std::shared_ptr<Validator> lax, strict;
        if (!lax_elem.error()) lax = build_from_element(lax_elem.value(), config, definitions);
        if (!strict_elem.error()) strict = build_from_element(strict_elem.value(), config, definitions);
        if (!lax) lax = std::make_shared<AnyValidator>();
        if (!strict) strict = std::make_shared<AnyValidator>();
        return std::make_shared<LaxOrStrictValidator>(lax, strict);
    }

    // Literal
    if (type == "literal") {
        auto expected = elem["expected"];
        if (!expected.error() && expected.value().is_array()) {
            std::vector<std::string> values;
            for (auto v : expected.value().get_array().value()) {
                if (v.is_string()) {
                    values.push_back(std::string(v.get_string().value()));
                } else if (v.is_int64()) {
                    values.push_back(std::to_string(v.get_int64()));
                } else if (v.is_bool()) {
                    values.push_back(v.get_bool() ? "true" : "false");
                }
            }
            return std::make_shared<LiteralValidator>(values);
        }
        return std::make_shared<LiteralValidator>();
    }

    if (type == "enum") {
        std::unordered_set<std::string> members;
        auto members_elem = elem["members"];
        if (!members_elem.error() && members_elem.value().is_array()) {
            for (auto m : members_elem.value().get_array().value()) {
                if (m.is_string()) {
                    members.insert(std::string(m.get_string().value()));
                }
            }
        }
        return std::make_shared<EnumValidator>(std::move(members));
    }
    if (type == "date") return std::make_shared<DateValidator>();
    if (type == "time") return std::make_shared<TimeValidator>();
    if (type == "datetime") return std::make_shared<DatetimeValidator>();
    if (type == "timedelta") return std::make_shared<TimedeltaValidator>();
    if (type == "url") return std::make_shared<UrlValidator>();
    if (type == "multi-host-url") return std::make_shared<MultiHostUrlValidator>();
    if (type == "uuid") return std::make_shared<UuidValidator>();

    // is-instance validator
    if (type == "is-instance") {
        auto v = std::make_shared<IsInstanceValidator>();
        auto cls_val = elem["cls"];
        if (!cls_val.error() && cls_val.value().is_string()) {
            v->set_class_name(std::string(cls_val.value().get_string().value()));
        }
        return v;
    }

    // is-subclass validator
    if (type == "is-subclass") {
        auto v = std::make_shared<IsSubclassValidator>();
        auto cls_val = elem["cls"];
        if (!cls_val.error() && cls_val.value().is_string()) {
            v->set_class_name(std::string(cls_val.value().get_string().value()));
        }
        return v;
    }

    // callable validator
    if (type == "callable") {
        return std::make_shared<CallableValidator>();
    }

    // generator validator (treated as any for now)
    if (type == "generator") {
        return std::make_shared<AnyValidator>();
    }

    // ========================================================================
    // Function validators - call Python functions with ValidationInfo
    // ========================================================================
    
    // function-before: func(input, info) -> transformed, then validate
    if (type == "function-before") {
        auto inner_schema = elem["schema"];
        std::shared_ptr<Validator> inner_v;
        if (!inner_schema.error()) {
            inner_v = build_from_element(inner_schema.value(), config, definitions);
        }
        auto validator = std::make_shared<FunctionBeforeValidator>(inner_v, py::none());
        // Note: Python function is set later by SchemaValidator when it receives the schema dict
        return validator;
    }
    
    // function-after: validate first, then func(validated, info) -> output
    if (type == "function-after") {
        auto inner_schema = elem["schema"];
        std::shared_ptr<Validator> inner_v;
        if (!inner_schema.error()) {
            inner_v = build_from_element(inner_schema.value(), config, definitions);
        }
        auto validator = std::make_shared<FunctionAfterValidator>(inner_v, py::none());
        return validator;
    }
    
    // function-plain: func(input, info) -> output (no inner validation)
    if (type == "function-plain") {
        auto validator = std::make_shared<FunctionPlainValidator>(py::none());
        return validator;
    }
    
    // function-wrap: func(input, handler, info) -> output
    // handler calls inner validator
    if (type == "function-wrap") {
        auto inner_schema = elem["schema"];
        std::shared_ptr<Validator> inner_v;
        if (!inner_schema.error()) {
            inner_v = build_from_element(inner_schema.value(), config, definitions);
        }
        auto validator = std::make_shared<FunctionWrapValidator>(inner_v, py::none());
        return validator;
    }

    // json-or-python validator
    if (type == "json-or-python") {
        auto json_schema = elem["json_schema"];
        auto python_schema = elem["python_schema"];
        std::shared_ptr<Validator> json_v, python_v;
        if (!json_schema.error()) {
            json_v = build_from_element(json_schema.value(), config, definitions);
        }
        if (!python_schema.error()) {
            python_v = build_from_element(python_schema.value(), config, definitions);
        }
        if (!json_v) json_v = std::make_shared<AnyValidator>();
        if (!python_v) python_v = std::make_shared<AnyValidator>();
        return std::make_shared<JsonOrPythonValidator>(json_v, python_v);
    }

    // ========================================================================
    // ModelFields - the core nested validator
    // ========================================================================
    if (type == "model-fields") {
        auto fields_elem = elem["fields"];
        ExtraBehavior extra_behavior = ExtraBehavior::Ignore;

        // Parse extra_behavior from config or schema
        auto eb_val = elem["extra_behavior"];
        if (!eb_val.error() && eb_val.value().is_string()) {
            extra_behavior = extra_behavior_from_string(
                std::string(eb_val.value().get_string().value()));
        } else {
            auto it = config.find("extra_fields_behavior");
            if (it != config.end()) {
                extra_behavior = extra_behavior_from_string(it->second);
            }
        }

        std::string model_name = "Model";
        auto mn_val = elem["model_name"];
        if (!mn_val.error() && mn_val.value().is_string()) {
            model_name = std::string(mn_val.value().get_string().value());
        }

        // Parse from_attributes from schema or config
        bool from_attributes = false;
        auto fa_val = elem["from_attributes"];
        if (!fa_val.error() && fa_val.value().is_bool()) {
            from_attributes = fa_val.value().get_bool();
        } else {
            auto it = config.find("from_attributes");
            if (it != config.end()) {
                from_attributes = (it->second == "true");
            }
        }

        auto validator = std::make_shared<ModelFieldsValidator>();
        validator->set_extra_behavior(extra_behavior);
        validator->set_model_name(model_name);
        validator->set_from_attributes(from_attributes);

        // Parse extras_schema (value validator for extra fields)
        auto es_val = elem["extras_schema"];
        if (!es_val.error()) {
            auto es_validator = build_from_element(es_val.value(), config, definitions);
            validator->set_extras_validator(es_validator);
        }

        // Parse extras_keys_schema (key validator for extra fields)
        auto eks_val = elem["extras_keys_schema"];
        if (!eks_val.error()) {
            auto eks_validator = build_from_element(eks_val.value(), config, definitions);
            validator->set_extras_keys_validator(eks_validator);
        }

        if (fields_elem.error() || !fields_elem.value().is_object()) {
            // Empty fields
            return validator;
        }

        auto fields_obj = fields_elem.value().get_object();
        for (auto& [field_name, field_def_elem] : fields_obj.value()) {
            std::string fname = std::string(field_name);
            auto field_def = field_def_elem.get_object();
            if (field_def.error()) continue;

            FieldInfo info;
            info.name = fname;

            // Parse schema — may be wrapped in a "default" schema type
            auto schema_elem = field_def_elem["schema"];
            if (!schema_elem.error()) {
                // Check if this is a "default" wrapper schema
                auto schema_type_elem = schema_elem.value()["type"];
                bool is_default_schema = false;
                if (!schema_type_elem.error() && schema_type_elem.value().is_string()) {
                    std::string st = std::string(schema_type_elem.value().get_string().value());
                    if (st == "default" || st == "with-default") {
                        is_default_schema = true;
                    }
                }

                if (is_default_schema) {
                    // The actual validator is nested inside schema.schema
                    auto inner_schema = schema_elem.value()["schema"];
                    if (!inner_schema.error()) {
                        info.schema = build_from_element(inner_schema.value(), config, definitions);
                    } else {
                        info.schema = std::make_shared<AnyValidator>();
                    }
                    // The default value will be parsed below from schema.default
                    // We don't set info.required = false yet; that happens in default parsing
                } else {
                    info.schema = build_from_element(schema_elem.value(), config, definitions);
                }
            } else {
                info.schema = std::make_shared<AnyValidator>();
            }

            // Parse required (default true)
            auto req_val = field_def_elem["required"];
            if (!req_val.error() && req_val.value().is_bool()) {
                info.required = req_val.value().get_bool();
            }

            // Parse frozen
            auto frozen_val = field_def_elem["frozen"];
            if (!frozen_val.error() && frozen_val.value().is_bool()) {
                info.frozen = frozen_val.value().get_bool();
            }

            // Parse validation_alias
            auto alias_val = field_def_elem["validation_alias"];
            if (!alias_val.error() && alias_val.value().is_string()) {
                info.alias = std::string(alias_val.value().get_string().value());
            }

            // Parse default value — check both field_def_elem["default"] and schema_elem["default"]
            // When schema type is "default", the default value is inside the schema wrapper
            auto default_elem = field_def_elem["default"];
            if (default_elem.error() && !schema_elem.error()) {
                // Try schema_elem["default"] for "default" wrapper schema type
                default_elem = schema_elem.value()["default"];
            }
            if (!default_elem.error()) {
                info.required = false;
                if (default_elem.value().is_string()) {
                    // Store raw string (no JSON quotes)
                    info.default_value_str = std::string(default_elem.value().get_string().value());
                } else if (default_elem.value().is_int64()) {
                    info.default_value_str = std::to_string(default_elem.value().get_int64());
                } else if (default_elem.value().is_uint64()) {
                    info.default_value_str = std::to_string(default_elem.value().get_uint64());
                } else if (default_elem.value().is_double()) {
                    info.default_value_str = std::to_string(default_elem.value().get_double());
                } else if (default_elem.value().is_bool()) {
                    info.default_value_str = default_elem.value().get_bool() ? "true" : "false";
                } else if (default_elem.value().is_null()) {
                    info.default_value_str = "null";
                    info.required = false;
                } else if (default_elem.value().is_array() || default_elem.value().is_object()) {
                    // For complex defaults, store as JSON string
                    info.default_value_str = std::string(simdjson::to_string(default_elem.value()));
                    info.required = false;
                }
            }

            validator->add_field(fname, std::move(info));
        }

        return validator;
    }

    // ========================================================================
    // TypedDict - similar to model-fields with "total" option
    // ========================================================================
    if (type == "typed-dict") {
        auto fields_elem = elem["fields"];
        bool total = true;

        auto total_val = elem["total"];
        if (!total_val.error() && total_val.value().is_bool()) {
            total = total_val.value().get_bool();
        }

        ExtraBehavior extra_behavior = ExtraBehavior::Ignore;
        auto eb_val = elem["extra_behavior"];
        if (!eb_val.error() && eb_val.value().is_string()) {
            extra_behavior = extra_behavior_from_string(
                std::string(eb_val.value().get_string().value()));
        } else {
            auto it = config.find("extra_fields_behavior");
            if (it != config.end()) {
                extra_behavior = extra_behavior_from_string(it->second);
            }
        }

        auto validator = std::make_shared<TypedDictValidator>();
        validator->set_total(total);

        // Set extra behavior via base class
        // (TypedDictValidator inherits from ModelFieldsValidator)
        // We need to set it on the parent - but the parent's set_extra_behavior
        // isn't accessible from here directly. Let's create a ModelFieldsValidator
        // approach instead.

        // Build fields the same way as model-fields
        if (fields_elem.error() || !fields_elem.value().is_object()) {
            // Need to manually create fields map and pass to TypedDictValidator
            return validator;
        }

        auto fields_obj = fields_elem.value().get_object();
        std::unordered_map<std::string, FieldInfo> fields_map;

        for (auto& [field_name, field_def_elem] : fields_obj.value()) {
            std::string fname = std::string(field_name);
            auto field_def = field_def_elem.get_object();
            if (field_def.error()) continue;

            FieldInfo info;
            info.name = fname;

            auto schema_elem = field_def_elem["schema"];
            if (!schema_elem.error()) {
                info.schema = build_from_element(schema_elem.value(), config, definitions);
            } else {
                info.schema = std::make_shared<AnyValidator>();
            }

            auto req_val = field_def_elem["required"];
            if (!req_val.error() && req_val.value().is_bool()) {
                info.required = req_val.value().get_bool();
            } else {
                // If not specified, use total setting
                info.required = total;
            }

            auto frozen_val = field_def_elem["frozen"];
            if (!frozen_val.error() && frozen_val.value().is_bool()) {
                info.frozen = frozen_val.value().get_bool();
            }

            auto alias_val = field_def_elem["validation_alias"];
            if (!alias_val.error() && alias_val.value().is_string()) {
                info.alias = std::string(alias_val.value().get_string().value());
            }

            fields_map[fname] = std::move(info);
        }

        return std::make_shared<TypedDictValidator>(
            std::move(fields_map), extra_behavior, total);
    }

    // ========================================================================
    // Model - wraps a model-fields schema
    // ========================================================================
    if (type == "model") {
        std::string class_name = "Model";
        auto cls_val = elem["cls"];
        if (!cls_val.error() && cls_val.value().is_string()) {
            class_name = std::string(cls_val.value().get_string().value());
        }

        bool frozen = false;
        auto frozen_val = elem["frozen"];
        if (!frozen_val.error() && frozen_val.value().is_bool()) {
            frozen = frozen_val.value().get_bool();
        }

        bool root_model = false;
        auto root_val = elem["root_model"];
        if (!root_val.error() && root_val.value().is_bool()) {
            root_model = root_val.value().get_bool();
        }

        std::shared_ptr<Validator> fields_validator;
        auto schema_elem = elem["schema"];
        if (!schema_elem.error()) {
            fields_validator = build_from_element(schema_elem.value(), config, definitions);
        } else {
            fields_validator = std::make_shared<ModelFieldsValidator>();
        }

        auto validator = std::make_shared<ModelValidator>(
            fields_validator, class_name, frozen, false, root_model);
        return validator;
    }

    // ========================================================================
    // Dataclass - flat field list
    // ========================================================================
    if (type == "dataclass") {
        std::string class_name = "Dataclass";
        auto cls_val = elem["cls"];
        if (!cls_val.error() && cls_val.value().is_string()) {
            class_name = std::string(cls_val.value().get_string().value());
        }

        bool frozen = false;
        auto frozen_val = elem["frozen"];
        if (!frozen_val.error() && frozen_val.value().is_bool()) {
            frozen = frozen_val.value().get_bool();
        }

        std::vector<DataclassFieldInfo> dc_fields;
        auto fields_arr = elem["fields"];
        if (!fields_arr.error() && fields_arr.value().is_array()) {
            for (auto field_elem : fields_arr.value().get_array().value()) {
                DataclassFieldInfo info;

                auto name_val = field_elem["name"];
                if (!name_val.error() && name_val.value().is_string()) {
                    info.name = std::string(name_val.value().get_string().value());
                }

                auto schema_val = field_elem["schema"];
                if (!schema_val.error()) {
                    info.schema = build_from_element(schema_val.value(), config, definitions);
                } else {
                    info.schema = std::make_shared<AnyValidator>();
                }

                auto kw_val = field_elem["kw_only"];
                if (!kw_val.error() && kw_val.value().is_bool()) {
                    info.kw_only = kw_val.value().get_bool();
                }

                auto frz_val = field_elem["frozen"];
                if (!frz_val.error() && frz_val.value().is_bool()) {
                    info.frozen = frz_val.value().get_bool();
                }

                auto init_val = field_elem["init_only_name"];
                if (!init_val.error() && init_val.value().is_string()) {
                    info.init_only_name = std::string(init_val.value().get_string().value());
                }

                dc_fields.push_back(std::move(info));
            }
        }

        return std::make_shared<DataclassValidator>(
            dc_fields, class_name, frozen);
    }

    // ========================================================================
    // JSON validator - validates JSON string input against inner schema
    // ========================================================================
    if (type == "json") {
        auto schema_val = elem["schema"];
        std::shared_ptr<Validator> inner;
        if (!schema_val.error()) {
            inner = build_from_element(schema_val.value(), config, definitions);
        } else {
            inner = std::make_shared<AnyValidator>();
        }
        return std::make_shared<JsonValidator>(inner);
    }
    // ========================================================================
    // Definitions - for recursive schemas
    // ========================================================================
    if (type == "definitions") {
        auto defs_arr = elem["definitions"];
        auto schema_val = elem["schema"];
        
        auto registry = std::make_shared<DefinitionsRegistry>();
        
        // First pass: extract all refs and register stubs so recursive refs can resolve
        if (!defs_arr.error() && defs_arr.value().is_array()) {
            for (auto def_elem : defs_arr.value().get_array().value()) {
                auto ref_val = def_elem["ref"];
                if (!ref_val.error() && ref_val.value().is_string()) {
                    std::string ref = std::string(ref_val.value().get_string().value());
                    registry->add_definition(ref, std::make_shared<AnyValidator>());
                }
            }
        }
        
        // Second pass: build all definitions (recursive refs resolve to stubs, then get replaced)
        if (!defs_arr.error() && defs_arr.value().is_array()) {
            for (auto def_elem : defs_arr.value().get_array().value()) {
                auto ref_val = def_elem["ref"];
                std::string ref;
                if (!ref_val.error() && ref_val.value().is_string()) {
                    ref = std::string(ref_val.value().get_string().value());
                }
                if (ref.empty()) continue;
                
                auto def_validator = build_from_element(def_elem, config, registry);
                registry->add_definition(ref, def_validator);
            }
        }
        
        // Third pass: build the main schema with fully populated registry
        if (!schema_val.error()) {
            auto main_validator = build_from_element(schema_val.value(), config, registry);
            return main_validator;
        }
        return std::make_shared<AnyValidator>();
    }

    if (type == "definition-ref") {
        auto ref_val = elem["schema_ref"];
        std::string ref;
        if (!ref_val.error() && ref_val.value().is_string()) {
            ref = std::string(ref_val.value().get_string().value());
        }
        
        auto def_ref = std::make_shared<DefinitionRefValidator>();
        def_ref->set_ref(ref);
        def_ref->set_definitions(definitions);
        return def_ref;
    }

    throw SchemaError("Unknown validator type: " + type);
}

// Backwards-compatible overload without definitions
static std::shared_ptr<Validator> build_from_element(
    const simdjson::dom::element& elem,
    const std::unordered_map<std::string, std::string>& config
) {
    return build_from_element(elem, config, nullptr);
}

// ============================================================================
// Public API
// ============================================================================

std::shared_ptr<CombinedValidator> SchemaBuilder::build(
    const std::string& schema_json,
    const std::string& config_json
) {
    // Parse config JSON FIRST with a separate parser
    // (simdjson::dom::parser reuses its internal buffer, so parsing config
    // after schema would invalidate the schema document)
    std::unordered_map<std::string, std::string> config;
    if (!config_json.empty()) {
        simdjson::dom::parser config_parser;
        auto config_doc = config_parser.parse(config_json);
        if (!config_doc.error()) {
            auto config_obj = config_doc.get_object();
            for (auto field : config_obj.value()) {
                std::string key = std::string(field.key);
                std::string value;
                if (field.value.is_string()) {
                    auto sv = field.value.get_string();
                    value = std::string(sv.value());
                } else if (field.value.is_bool()) {
                    value = field.value.get_bool() ? "true" : "false";
                } else {
                    value = simdjson::minify(field.value);
                }
                config[key] = value;
            }
        }
    }

    // Parse schema JSON using a separate parser
    simdjson::dom::parser schema_parser;
    auto doc = schema_parser.parse(schema_json);
    if (doc.error()) {
        throw std::runtime_error("Invalid schema JSON: " + std::string(simdjson::error_message(doc.error())));
    }

    // Build validator from element (recursive)
    auto validator = build_from_element(doc.value(), config);
    return std::make_shared<CombinedValidator>(validator);
}

std::shared_ptr<CombinedValidator> SchemaBuilder::build_from_dict(
    const std::unordered_map<std::string, std::string>& schema,
    const std::unordered_map<std::string, std::string>& config
) {
    // Parse the schema JSON strings back into elements for recursive building
    // This is needed because the dict interface flattens nested JSON
    auto it = schema.find("type");
    std::string type = (it != schema.end()) ? it->second : "";

    if (type.empty()) {
        throw SchemaError("Schema missing 'type' field");
    }

    // For complex types, rebuild JSON and use recursive parser
    if (type == "model-fields" || type == "typed-dict" || type == "model" ||
        type == "dataclass" || type == "nullable" || type == "union" ||
        type == "chain" || type == "default" || type == "json" ||
        type == "lax-or-strict") {

        // Convert dict back to JSON string and parse
        std::string json = "{";
        bool first = true;
        for (const auto& [key, value] : schema) {
            if (!first) json += ",";
            first = false;
            json += "\"" + key + "\":" + value;
        }
        json += "}";

        simdjson::dom::parser parser;
        auto doc = parser.parse(json);
        if (doc.error()) {
            throw SchemaError("Invalid schema JSON: " + std::string(simdjson::error_message(doc.error())));
        }

        auto validator = build_from_element(doc.value(), config);
        return std::make_shared<CombinedValidator>(validator);
    }

    // Simple types can be built directly
    auto validator = build_from_flat_dict(schema, config);
    return std::make_shared<CombinedValidator>(validator);
}

// ============================================================================
// Python dict-based builder (like Rust — no JSON round-trip)
// ============================================================================

// Helper: extract string from py::dict with optional default
static std::string py_str(const py::dict& d, const char* key, const std::string& fallback = "") {
    if (!d.contains(key)) return fallback;
    auto val = d[key];
    if (val.is_none()) return fallback;
    return py::str(val).cast<std::string>();
}

// Helper: extract int from py::dict
static int64_t py_int(const py::dict& d, const char* key, int64_t fallback = 0) {
    if (!d.contains(key)) return fallback;
    return d[key].cast<int64_t>();
}

// Helper: extract bool from py::dict
static bool py_bool(const py::dict& d, const char* key, bool fallback = false) {
    if (!d.contains(key)) return fallback;
    auto val = d[key];
    if (py::isinstance<py::bool_>(val)) return val.cast<bool>();
    return false;
}

// Forward declaration for recursive building
static std::shared_ptr<Validator> build_from_py_dict(
    const py::dict& schema,
    const py::dict& config,
    std::shared_ptr<DefinitionsRegistry> definitions
);

// Build a DefinitionsRegistry from a Python definitions list
static std::shared_ptr<DefinitionsRegistry> build_definitions_from_py(
    const py::list& defs_list,
    const py::dict& config,
    std::shared_ptr<DefinitionsRegistry> registry
) {
    for (auto item : defs_list) {
        auto def_dict = item.cast<py::dict>();
        std::string ref = py_str(def_dict, "ref");
        if (ref.empty()) continue;
        auto inner = def_dict["schema"].cast<py::dict>();
        auto validator = build_from_py_dict(inner, config, registry);
        registry->add_definition(ref, validator);
    }
    return registry;
}

static std::shared_ptr<Validator> build_from_py_dict(
    const py::dict& schema,
    const py::dict& config,
    std::shared_ptr<DefinitionsRegistry> definitions
) {
    std::string type = py_str(schema, "type");
    if (type.empty()) {
        throw SchemaError("Schema missing 'type' field");
    }

    // --- Scalar validators ---
    if (type == "any") return std::make_shared<AnyValidator>();
    if (type == "none") return std::make_shared<NoneValidator>();
    if (type == "bool") return std::make_shared<BoolValidator>();

    if (type == "int" || type == "int-constrained" || type == "constr-int") {
        if (schema.contains("multiple_of") || schema.contains("le") || schema.contains("ge") ||
            schema.contains("lt") || schema.contains("gt")) {
            auto v = std::make_shared<ConstrainedIntValidator>();
            return v;
        }
        return std::make_shared<IntValidator>();
    }

    if (type == "float" || type == "float-constrained" || type == "constr-float") {
        if (schema.contains("multiple_of") || schema.contains("le") || schema.contains("ge") ||
            schema.contains("lt") || schema.contains("gt")) {
            auto v = std::make_shared<ConstrainedFloatValidator>();
            return v;
        }
        return std::make_shared<FloatValidator>();
    }

    if (type == "str" || type == "string" || type == "str-constrained" || type == "constr-str") {
        if (schema.contains("max_length") || schema.contains("min_length") ||
            schema.contains("pattern") || schema.contains("strip_whitespace")) {
            auto v = std::make_shared<StrConstrainedValidator>();
            auto ps = [&](const char* k) -> std::optional<size_t> {
                if (!schema.contains(k)) return std::nullopt;
                py::object val = schema[k];
                if (py::isinstance<py::int_>(val)) {
                    long long i = val.cast<long long>();
                    if (i >= 0) return static_cast<size_t>(i);
                }
                return std::nullopt;
            };
            if (auto mv = ps("min_length")) v->min_length = *mv;
            if (auto mv = ps("max_length")) v->max_length = *mv;
            if (schema.contains("pattern") && py::isinstance<py::str>(schema["pattern"])) {
                v->pattern = schema["pattern"].cast<std::string>();
            }
            if (schema.contains("strip_whitespace") && py::isinstance<py::bool_>(schema["strip_whitespace"])) {
                v->strip_whitespace = schema["strip_whitespace"].cast<bool>();
            }
            if (schema.contains("to_lower") && py::isinstance<py::bool_>(schema["to_lower"])) {
                v->to_lower = schema["to_lower"].cast<bool>();
            }
            if (schema.contains("to_upper") && py::isinstance<py::bool_>(schema["to_upper"])) {
                v->to_upper = schema["to_upper"].cast<bool>();
            }
            return v;
        }
        return std::make_shared<StringValidator>();
    }

    if (type == "bytes" || type == "bytes-constrained" || type == "constr-bytes") {
        if (schema.contains("max_length") || schema.contains("min_length")) {
            auto v = std::make_shared<BytesConstrainedValidator>();
            auto ps = [&](const char* k) -> std::optional<size_t> {
                if (!schema.contains(k)) return std::nullopt;
                py::object val = schema[k];
                if (py::isinstance<py::int_>(val)) {
                    long long i = val.cast<long long>();
                    if (i >= 0) return static_cast<size_t>(i);
                }
                return std::nullopt;
            };
            if (auto mv = ps("min_length")) v->min_length = *mv;
            if (auto mv = ps("max_length")) v->max_length = *mv;
            return v;
        }
        return std::make_shared<BytesValidator>();
    }

    // --- Date/time validators ---
    if (type == "date") return std::make_shared<DateValidator>();
    if (type == "time") return std::make_shared<TimeValidator>();
    if (type == "datetime") return std::make_shared<DatetimeValidator>();
    if (type == "timedelta") return std::make_shared<TimedeltaValidator>();

    // --- URL validators ---
    if (type == "url") return std::make_shared<UrlValidator>();
    if (type == "multi-host-url") return std::make_shared<MultiHostUrlValidator>();

    // --- UUID ---
    if (type == "uuid") return std::make_shared<UuidValidator>();

    // --- Decimal ---
    if (type == "decimal" || type == "decimal-constrained") {
        // Decimal not yet implemented — use AnyValidator as stub
        return std::make_shared<AnyValidator>();
    }

    // --- Literal ---
    if (type == "literal") {
        std::vector<std::string> expected;
        if (schema.contains("expected")) {
            auto lst = schema["expected"].cast<py::list>();
            for (auto item : lst) {
                expected.push_back(py::str(item).cast<std::string>());
            }
        }
        return std::make_shared<LiteralValidator>(std::move(expected));
    }

    // --- Enum ---
    if (type == "enum") {
        std::vector<std::string> members;
        if (schema.contains("members")) {
            auto lst = schema["members"].cast<py::list>();
            for (auto item : lst) {
                // For Enum members, use .value instead of str() (which gives 'ClassName.MEMBER');
                // plain strings (e.g. manually built schemas) are used as-is.
                std::string member_str;
                if (py::hasattr(item, "value")) {
                    member_str = py::str(py::getattr(item, "value")).cast<std::string>();
                } else {
                    member_str = py::str(item).cast<std::string>();
                }
                members.push_back(member_str);
            }
        }
        std::unordered_set<std::string> member_set(members.begin(), members.end());
        return std::make_shared<EnumValidator>(std::move(member_set));
    }

    // --- IsInstance / IsSubclass / Callable ---
    if (type == "is-instance") {
        auto v = std::make_shared<IsInstanceValidator>();
        if (schema.contains("cls")) {
            py::object cls = schema["cls"];
            // Type is stored as string or as Python class
            if (py::isinstance<py::type>(cls)) {
                v->set_py_class(cls);
            } else if (py::isinstance<py::str>(cls)) {
                v->set_class_name(cls.cast<std::string>());
            }
        }
        return v;
    }

    if (type == "is-subclass") {
        auto v = std::make_shared<IsSubclassValidator>();
        if (schema.contains("cls")) {
            auto cls = schema["cls"];
            if (py::isinstance<py::str>(cls)) {
                v->set_class_name(cls.cast<std::string>());
            }
        }
        return v;
    }

    if (type == "callable") return std::make_shared<CallableValidator>();

    // --- Nullable ---
    if (type == "nullable") {
        std::shared_ptr<Validator> inner;
        if (schema.contains("schema")) {
            inner = build_from_py_dict(schema["schema"].cast<py::dict>(), config, definitions);
        }
        auto v = std::make_shared<NullableValidator>(inner);
        return v;
    }

    // --- WithDefault ---
    if (type == "default") {
        std::shared_ptr<Validator> inner;
        if (schema.contains("schema")) {
            inner = build_from_py_dict(schema["schema"].cast<py::dict>(), config, definitions);
        }
        // Default value is stored directly as Python object
        std::shared_ptr<void> default_val;
        std::string default_val_str;
        if (schema.contains("default")) {
            auto py_default = schema["default"];
            if (py::isinstance<py::str>(py_default)) {
                default_val = std::make_shared<std::string>(py_default.cast<std::string>());
            } else if (py::isinstance<py::int_>(py_default)) {
                default_val = std::make_shared<int64_t>(py_default.cast<int64_t>());
            } else if (py::isinstance<py::float_>(py_default)) {
                default_val = std::make_shared<double>(py_default.cast<double>());
            } else if (py::isinstance<py::bool_>(py_default)) {
                default_val = std::make_shared<bool>(py_default.cast<bool>());
            } else if (!py_default.is_none()) {
                // Complex default (list, dict) — serialize to JSON string for later parsing
                default_val_str = py_default_to_json_str(py_default);
            }
        }
        return std::make_shared<WithDefaultValidator>(inner, default_val, default_val_str);
    }

    // --- Function validators (Before / After / Wrap / Plain) ---
    if (type == "function-before") {
        std::shared_ptr<Validator> inner;
        if (schema.contains("schema")) {
            inner = build_from_py_dict(schema["schema"].cast<py::dict>(), config, definitions);
        }
        py::object func = py::none();
        if (schema.contains("function")) func = schema["function"];
        return std::make_shared<FunctionBeforeValidator>(inner, func);
    }

    if (type == "function-after") {
        std::shared_ptr<Validator> inner;
        if (schema.contains("schema")) {
            inner = build_from_py_dict(schema["schema"].cast<py::dict>(), config, definitions);
        }
        py::object func = py::none();
        if (schema.contains("function")) {
            func = schema["function"];
            // function may be a dict like {'function': actual_callable, 'type': 'no-info'}
            if (py::isinstance<py::dict>(func)) {
                py::dict func_dict = func.cast<py::dict>();
                if (func_dict.contains("function")) {
                    func = func_dict["function"];
                }
            }
        }
        return std::make_shared<FunctionAfterValidator>(inner, func);
    }

    if (type == "function-plain") {
        py::object func = py::none();
        if (schema.contains("function")) {
            func = schema["function"];
            if (py::isinstance<py::dict>(func)) {
                py::dict func_dict = func.cast<py::dict>();
                if (func_dict.contains("function")) {
                    func = func_dict["function"];
                }
            }
        }
        return std::make_shared<FunctionPlainValidator>(func);
    }

    if (type == "function-wrap") {
        std::shared_ptr<Validator> inner;
        if (schema.contains("schema")) {
            inner = build_from_py_dict(schema["schema"].cast<py::dict>(), config, definitions);
        }
        py::object func = py::none();
        if (schema.contains("function")) func = schema["function"];
        return std::make_shared<FunctionWrapValidator>(inner, func);
    }

    // --- LaxOrStrict ---
    if (type == "lax-or-strict") {
        std::shared_ptr<Validator> lax, strict;
        if (schema.contains("lax_schema")) {
            lax = build_from_py_dict(schema["lax_schema"].cast<py::dict>(), config, definitions);
        }
        if (schema.contains("strict_schema")) {
            strict = build_from_py_dict(schema["strict_schema"].cast<py::dict>(), config, definitions);
        }
        return std::make_shared<LaxOrStrictValidator>(lax, strict);
    }

    // --- JsonOrPython ---
    if (type == "json-or-python") {
        std::shared_ptr<Validator> json_v, python_v;
        if (schema.contains("json_schema")) {
            json_v = build_from_py_dict(schema["json_schema"].cast<py::dict>(), config, definitions);
        }
        if (schema.contains("python_schema")) {
            python_v = build_from_py_dict(schema["python_schema"].cast<py::dict>(), config, definitions);
        }
        return std::make_shared<JsonOrPythonValidator>(json_v, python_v);
    }

    // --- Union ---
    if (type == "union") {
        std::vector<std::shared_ptr<Validator>> choices;
        if (schema.contains("choices")) {
            auto choices_list = schema["choices"].cast<py::list>();
            for (auto item : choices_list) {
                auto choice = build_from_py_dict(item.cast<py::dict>(), config, definitions);
                choices.push_back(choice);
            }
        }
        return std::make_shared<UnionValidator>(std::move(choices));
    }

    // --- TaggedUnion ---
    if (type == "tagged-union") {
        std::string discriminator = py_str(schema, "discriminator");
        std::vector<std::shared_ptr<Validator>> choices;
        if (schema.contains("choices")) {
            auto choices_list = schema["choices"].cast<py::list>();
            for (auto item : choices_list) {
                auto choice = build_from_py_dict(item.cast<py::dict>(), config, definitions);
                choices.push_back(choice);
            }
        }
        return std::make_shared<TaggedUnionValidator>(discriminator, std::move(choices));
    }

    // --- List ---
    if (type == "list" || type == "list-constrained" || type == "constr-list") {
        auto v = std::make_shared<ListValidator>();
        if (schema.contains("items_schema") || schema.contains("items")) {
            auto items_key = schema.contains("items_schema") ? "items_schema" : "items";
            v->items_schema = build_from_py_dict(schema[items_key].cast<py::dict>(), config, definitions);
        }
        return v;
    }

    // --- Tuple ---
    if (type == "tuple" || type == "tuple-constrained" || type == "constr-tuple" || type == "tuple-variable") {
        // Simple stub
        return std::make_shared<TupleValidator>();
    }

    // --- Dict ---
    if (type == "dict" || type == "dict-constrained" || type == "constr-dict") {
        auto v = std::make_shared<DictValidator>();
        if (schema.contains("keys_schema")) {
            v->keys_schema = build_from_py_dict(schema["keys_schema"].cast<py::dict>(), config, definitions);
        }
        if (schema.contains("values_schema")) {
            v->values_schema = build_from_py_dict(schema["values_schema"].cast<py::dict>(), config, definitions);
        }
        return v;
    }

    // --- Set ---
    if (type == "set" || type == "set-constrained" || type == "constr-set" || type == "frozenset" || type == "frozenset-constrained") {
        // Simple stub
        return std::make_shared<SetValidator>();
    }

    // --- Model ---
    if (type == "model") {
        std::shared_ptr<Validator> inner;
        if (schema.contains("schema")) {
            inner = build_from_py_dict(schema["schema"].cast<py::dict>(), config, definitions);
        }
        std::string model_name = py_str(schema, "title", py_str(schema, "model_name", py_str(schema, "cls", "")));
        auto v = std::make_shared<ModelValidator>(inner, model_name);
        return v;
    }

    // --- ModelFields / TypedDict / Dataclass ---
    if (type == "model-fields" || type == "typed-dict" || type == "dataclass-args") {
        auto v = std::make_shared<ModelFieldsValidator>();
        if (schema.contains("fields")) {
            auto fields_dict = schema["fields"].cast<py::dict>();
            for (auto item : fields_dict) {
                std::string field_name = py::str(item.first).cast<std::string>();
                auto field_def = item.second.cast<py::dict>();
                
                // Extract field schema
                std::shared_ptr<Validator> field_validator;
                py::dict field_schema_dict;
                std::string default_val_str;
                bool required = true;
                
                if (field_def.contains("schema")) {
                    field_schema_dict = field_def["schema"].cast<py::dict>();

                    // Extract default from inner "default" type schema
                    if (field_schema_dict.contains("type") &&
                        py::str(field_schema_dict["type"]).cast<std::string>() == "default") {
                        // Unwrap default schema: use inner validator and extract default value string
                        if (field_schema_dict.contains("default")) {
                            auto py_default = field_schema_dict["default"];
                            if (py_default.is_none()) {
                                default_val_str = "null";
                            } else {
                                default_val_str = py_default_to_json_str(py_default);
                            }
                            required = false;
                        }
                        if (field_schema_dict.contains("schema")) {
                            // Use the inner validator directly (skip the WithDefault wrapper)
                            field_validator = build_from_py_dict(
                                field_schema_dict["schema"].cast<py::dict>(), config, definitions);
                        }
                    } else {
                        field_validator = build_from_py_dict(field_schema_dict, config, definitions);
                    }
                }
                
                v->add_field(field_name, FieldInfo{
                    "",          // name (set via add_field's first param)
                    field_validator,
                    required,    // required
                    default_val_str,  // default_value_str
                    false,       // frozen
                    ""           // alias
                });
            }
        }
        
        // Extract extras behavior from config
        std::string extra_str = py_str(config, "extra_fields_behavior",
            py_str(config, "extra_behavior", py_str(config, "extra", "ignore")));
        auto extra = extra_behavior_from_string(extra_str);
        v->set_extra_behavior(extra);

        // Extract from_attributes from schema or config
        bool from_attributes = false;
        if (schema.contains("from_attributes")) {
            from_attributes = schema["from_attributes"].cast<bool>();
        } else if (config.contains("from_attributes")) {
            from_attributes = config["from_attributes"].cast<bool>();
        }
        v->set_from_attributes(from_attributes);

        // Extract extras_schema (value validator applied to extra fields)
        if (schema.contains("extras_schema")) {
            auto extras_schema = schema["extras_schema"].cast<py::dict>();
            auto extras_validator = build_from_py_dict(extras_schema, config, definitions);
            v->set_extras_validator(extras_validator);
        }

        // Extract extras_keys_schema (validator applied to extra field keys)
        if (schema.contains("extras_keys_schema")) {
            auto extras_keys_schema = schema["extras_keys_schema"].cast<py::dict>();
            auto extras_keys_validator = build_from_py_dict(extras_keys_schema, config, definitions);
            v->set_extras_keys_validator(extras_keys_validator);
        }

        return v;
    }

    // --- Definitions wrapper ---
    if (type == "definitions") {
        auto registry = std::make_shared<DefinitionsRegistry>();
        
        // Build all definitions first
        if (schema.contains("definitions")) {
            auto defs = schema["definitions"];
            if (py::isinstance<py::list>(defs)) {
                build_definitions_from_py(defs.cast<py::list>(), config, registry);
            } else if (py::isinstance<py::dict>(defs)) {
                // Handle lazy-loading definitions builder
                py::dict defs_dict = defs.cast<py::dict>();
                if (defs_dict.contains("definitions")) {
                    auto inner_defs = defs_dict["definitions"];
                    if (py::isinstance<py::dict>(inner_defs)) {
                        auto inner_dict = inner_defs.cast<py::dict>();
                        for (auto item : inner_dict) {
                            std::string ref = py::str(item.first).cast<std::string>();
                            auto def_schema = item.second.cast<py::dict>();
                            auto validator = build_from_py_dict(def_schema, config, registry);
                            registry->add_definition(ref, validator);
                        }
                    }
                }
            }
        }
        
        // Build main schema
        if (schema.contains("schema")) {
            auto main_schema = schema["schema"].cast<py::dict>();
            auto main_validator = build_from_py_dict(main_schema, config, registry);
            // Simple: return the main validator directly (definitions are resolved on demand)
            return main_validator;
        }
        
        throw SchemaError("definitions schema missing 'schema'");
    }

    // --- Definition-ref ---
    if (type == "definition-ref") {
        std::string ref = py_str(schema, "schema_ref");
        if (!ref.empty() && definitions) {
            auto validator = definitions->get_definition(ref);
            if (validator) {
                auto def_ref = std::make_shared<DefinitionRefValidator>();
                def_ref->set_ref(ref);
                def_ref->set_definitions(definitions);
                return def_ref;
            }
        }
        // Fallback: try to resolve later
        if (!ref.empty()) {
            auto def_ref = std::make_shared<DefinitionRefValidator>();
            def_ref->set_ref(ref);
            def_ref->set_definitions(definitions);
            return def_ref;
        }
        throw SchemaError("definition-ref missing 'schema_ref'");
    }

    // --- JSON ---
    if (type == "json") {
        std::shared_ptr<Validator> inner;
        if (schema.contains("schema")) {
            inner = build_from_py_dict(schema["schema"].cast<py::dict>(), config, definitions);
        }
        return std::make_shared<JsonValidator>(inner);
    }

    throw SchemaError("Unknown schema type: " + type);
}

// SchemaBuilder::build_from_py — public entry point
std::shared_ptr<CombinedValidator> SchemaBuilder::build_from_py(
    const py::dict& schema,
    const py::dict& config
) {
    auto validator = build_from_py_dict(schema, config, nullptr);
    return std::make_shared<CombinedValidator>(validator);
}

} // namespace pydantic_core
