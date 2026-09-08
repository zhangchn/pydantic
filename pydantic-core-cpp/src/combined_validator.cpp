#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/py_compat.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/validators/model_fields.hpp"
#include "pydantic_core/validators/special.hpp"
#include "pydantic_core/validators/functions.hpp"
#include <memory>
#include <pybind11/pybind11.h>
#include <simdjson.h>
#include <stdexcept>

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
        if (py_hasattr(o, "_value_") && py_hasattr(o, "_name_")) {
            // Enum member — use its name so the enum validator can rebuild it
            return py::getattr(o, "name");
        }
        if (py_hasattr(o, "__dict__")) {
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
    if (type == "missing-sentinel") return std::make_shared<MissingSentinelValidator>();
    if (type == "bool") {
        auto v = std::make_shared<BoolValidator>();
        auto it = schema.find("strict");
        if (it != schema.end() && (it->second == "true" || it->second == "1")) {
            v->strict = true;
        }
        return v;
    }
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
    if (type == "url") {
        auto v = std::make_shared<UrlValidator>();
        auto ml_it = schema.find("max_length");
        if (ml_it != schema.end()) {
            try {
                long long ml = std::stoll(ml_it->second);
                if (ml >= 0) v->max_length = static_cast<size_t>(ml);
            } catch (...) {}
        }
        auto as_it = schema.find("allowed_schemes");
        if (as_it != schema.end()) {
            // Stored as a JSON array string by element_to_dict, e.g. ["http","https"]
            auto as_doc = simdjson::padded_string(as_it->second);
            simdjson::dom::parser as_parser;
            auto as_res = as_parser.parse(as_doc);
            if (!as_res.error() && as_res.value().is_array()) {
                for (auto s : as_res.value().get_array().value()) {
                    if (s.is_string()) {
                        v->allowed_schemes.push_back(std::string(s.get_string().value()));
                    }
                }
            }
        }
        auto hr_it = schema.find("host_required");
        if (hr_it != schema.end()) v->host_required = (hr_it->second == "true" || hr_it->second == "1");
        auto pep_it = schema.find("preserve_empty_path");
        if (pep_it != schema.end()) {
            v->preserve_empty_path = (pep_it->second == "true" || pep_it->second == "1");
        } else {
            auto cfg_it = config.find("url_preserve_empty_path");
            if (cfg_it != config.end()) v->preserve_empty_path = (cfg_it->second == "true" || cfg_it->second == "1");
        }
        return v;
    }
    if (type == "multi-host-url") return std::make_shared<MultiHostUrlValidator>();
    if (type == "uuid") {
        auto v = std::make_shared<UuidValidator>();
        auto it = schema.find("strict");
        if (it != schema.end() && (it->second == "true" || it->second == "1")) {
            v->strict = true;
        }
        return v;
    }
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
    if (type == "missing-sentinel") return std::make_shared<MissingSentinelValidator>();
    if (type == "bool") {
        auto v = std::make_shared<BoolValidator>();
        auto it = flat_schema.find("strict");
        if (it != flat_schema.end() && (it->second == "true" || it->second == "1")) {
            v->strict = true;
        }
        return v;
    }

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
            std::vector<std::string> reprs;
            for (auto v : expected.value().get_array().value()) {
                if (v.is_string()) {
                    std::string s = std::string(v.get_string().value());
                    values.push_back(s);
                    reprs.push_back("'" + s + "'");
                } else if (v.is_int64()) {
                    std::string s = std::to_string(v.get_int64());
                    values.push_back(s);
                    reprs.push_back(s);
                } else if (v.is_bool()) {
                    std::string s = v.get_bool() ? "true" : "false";
                    values.push_back(s);
                    reprs.push_back(s);
                }
            }
            std::string expected_repr;
            if (!reprs.empty()) {
                expected_repr = reprs[0];
                for (size_t i = 1; i < reprs.size(); ++i) {
                    expected_repr += " or ";
                    expected_repr += reprs[i];
                }
            }
            return std::make_shared<LiteralValidator>(std::move(values), std::move(expected_repr));
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
    if (type == "url") {
        auto v = std::make_shared<UrlValidator>();
        auto it = flat_schema.find("max_length");
        if (it != flat_schema.end()) {
            try {
                long long ml = std::stoll(it->second);
                if (ml >= 0) v->max_length = static_cast<size_t>(ml);
            } catch (...) {}
        }
        return v;
    }
    if (type == "multi-host-url") return std::make_shared<MultiHostUrlValidator>();
    if (type == "uuid") {
        auto v = std::make_shared<UuidValidator>();
        auto it = flat_schema.find("strict");
        if (it != flat_schema.end() && (it->second == "true" || it->second == "1")) {
            v->strict = true;
        }
        return v;
    }

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

    // generator validator
    if (type == "generator") {
        return std::make_shared<GeneratorValidator>();
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

            // Parse validation_alias (string, AliasPath = flat list, or AliasChoices = list of lists)
            auto alias_val = field_def_elem["validation_alias"];
            if (!alias_val.error()) {
                if (alias_val.value().is_string()) {
                    info.alias = std::string(alias_val.value().get_string().value());
                    info.validation_paths.push_back({info.alias});
                    info.has_alias = true;
                } else if (alias_val.value().is_array()) {
                    // Determine if it's an AliasPath (flat list of scalars) or
                    // AliasChoices (list of lists).
                    bool is_choices = false;
                    for (auto a : alias_val.value().get_array().value()) {
                        if (a.is_array()) { is_choices = true; break; }
                    }
                    if (is_choices) {
                        // AliasChoices: each element is a path
                        for (auto a : alias_val.value().get_array().value()) {
                            std::vector<std::string> path;
                            if (a.is_array()) {
                                for (auto inner : a.get_array().value()) {
                                    if (inner.is_string()) path.push_back(std::string(inner.get_string().value()));
                                    else if (inner.is_int64()) path.push_back(std::to_string(inner.get_int64()));
                                    else if (inner.is_uint64()) path.push_back(std::to_string(inner.get_uint64()));
                                }
                            } else if (a.is_string()) {
                                path.push_back(std::string(a.get_string().value()));
                            }
                            if (!path.empty()) {
                                info.validation_paths.push_back(path);
                                if (info.alias.empty()) info.alias = path[0];
                            }
                        }
                    } else {
                        // AliasPath: flat list of scalars
                        std::vector<std::string> path;
                        for (auto a : alias_val.value().get_array().value()) {
                            if (a.is_string()) path.push_back(std::string(a.get_string().value()));
                            else if (a.is_int64()) path.push_back(std::to_string(a.get_int64()));
                            else if (a.is_uint64()) path.push_back(std::to_string(a.get_uint64()));
                        }
                        if (!path.empty()) {
                            info.validation_paths.push_back(path);
                            info.alias = path[0];
                        }
                    }
                    info.has_alias = !info.validation_paths.empty();
                }
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
            if (!alias_val.error()) {
                if (alias_val.value().is_string()) {
                    info.alias = std::string(alias_val.value().get_string().value());
                    info.validation_paths.push_back({info.alias});
                    info.has_alias = true;
                } else if (alias_val.value().is_array()) {
                    bool is_choices = false;
                    for (auto a : alias_val.value().get_array().value()) {
                        if (a.is_array()) { is_choices = true; break; }
                    }
                    if (is_choices) {
                        for (auto a : alias_val.value().get_array().value()) {
                            std::vector<std::string> path;
                            if (a.is_array()) {
                                for (auto inner : a.get_array().value()) {
                                    if (inner.is_string()) path.push_back(std::string(inner.get_string().value()));
                                    else if (inner.is_int64()) path.push_back(std::to_string(inner.get_int64()));
                                    else if (inner.is_uint64()) path.push_back(std::to_string(inner.get_uint64()));
                                }
                            } else if (a.is_string()) {
                                path.push_back(std::string(a.get_string().value()));
                            }
                            if (!path.empty()) {
                                info.validation_paths.push_back(path);
                                if (info.alias.empty()) info.alias = path[0];
                            }
                        }
                    } else {
                        std::vector<std::string> path;
                        for (auto a : alias_val.value().get_array().value()) {
                            if (a.is_string()) path.push_back(std::string(a.get_string().value()));
                            else if (a.is_int64()) path.push_back(std::to_string(a.get_int64()));
                            else if (a.is_uint64()) path.push_back(std::to_string(a.get_uint64()));
                        }
                        if (!path.empty()) {
                            info.validation_paths.push_back(path);
                            info.alias = path[0];
                        }
                    }
                    info.has_alias = !info.validation_paths.empty();
                }
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

        // Parse revalidate_instances from schema or config
        RevalidateInstances revalidate = RevalidateInstances::Never;
        std::string revalidate_str;
        auto revalidate_val = elem["revalidate_instances"];
        if (!revalidate_val.error() && revalidate_val.value().is_string()) {
            revalidate_str = revalidate_val.value().get_string().value();
        } else {
            // Try config
            auto config_revalidate = config.find("revalidate_instances");
            if (config_revalidate != config.end()) {
                revalidate_str = config_revalidate->second;
            }
        }
        if (revalidate_str == "always") {
            revalidate = RevalidateInstances::Always;
        } else if (revalidate_str == "subclass-instances") {
            revalidate = RevalidateInstances::SubclassInstances;
        }

        auto validator = std::make_shared<ModelValidator>(
            fields_validator, class_name, frozen, false, root_model, py::none(), revalidate);
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
    if (type == "model-fields" || type == "typed-dict" ||
        type == "dataclass" || type == "nullable" ||
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
        // Pydantic emits FLAT definitions ({ref, type: ...}) where a
        // "schema" key belongs to the validator itself (e.g. function-after).
        // Only unwrap when there is no "type" (hand-written wrapper form).
        py::dict inner;
        if (!def_dict.contains("type") && def_dict.contains("schema")) {
            inner = def_dict["schema"].cast<py::dict>();
        } else {
            inner = def_dict;
        }
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
    if (type == "missing-sentinel") return std::make_shared<MissingSentinelValidator>();
    if (type == "bool") {
        auto v = std::make_shared<BoolValidator>();
        if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
            v->strict = schema["strict"].cast<bool>();
        }
        return v;
    }

    if (type == "int" || type == "int-constrained" || type == "constr-int") {
        if (schema.contains("multiple_of") || schema.contains("le") || schema.contains("ge") ||
            schema.contains("lt") || schema.contains("gt")) {
            auto v = std::make_shared<ConstrainedIntValidator>();
            if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
                v->strict = schema["strict"].cast<bool>();
            }
            auto get_i64 = [&](const char* k) -> std::optional<int64_t> {
                if (!schema.contains(k)) return std::nullopt;
                py::object val = schema[k];
                if (py::isinstance<py::int_>(val)) return val.cast<int64_t>();
                return std::nullopt;
            };
            v->gt = get_i64("gt");
            v->ge = get_i64("ge");
            v->lt = get_i64("lt");
            v->le = get_i64("le");
            v->multiple_of = get_i64("multiple_of");
            return v;
        }
        auto v = std::make_shared<IntValidator>();
        if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
            v->strict = schema["strict"].cast<bool>();
        }
        return v;
    }

    if (type == "float" || type == "float-constrained" || type == "constr-float") {
        if (schema.contains("multiple_of") || schema.contains("le") || schema.contains("ge") ||
            schema.contains("lt") || schema.contains("gt")) {
            auto v = std::make_shared<ConstrainedFloatValidator>();
            if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
                v->strict = schema["strict"].cast<bool>();
            }
            auto get_f = [&](const char* k) -> std::optional<double> {
                if (!schema.contains(k)) return std::nullopt;
                py::object val = schema[k];
                if (py::isinstance<py::int_>(val)) return val.cast<double>();
                if (py::isinstance<py::float_>(val)) return val.cast<double>();
                return std::nullopt;
            };
            v->gt = get_f("gt");
            v->ge = get_f("ge");
            v->lt = get_f("lt");
            v->le = get_f("le");
            v->multiple_of = get_f("multiple_of");
            return v;
        }
        auto v = std::make_shared<FloatValidator>();
        if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
            v->strict = schema["strict"].cast<bool>();
        }
        return v;
    }

    if (type == "complex") {
        auto v = std::make_shared<ComplexValidator>();
        if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
            v->strict = schema["strict"].cast<bool>();
        }
        return v;
    }

    if (type == "str" || type == "string" || type == "str-constrained" || type == "constr-str") {
        // Mirror Rust's schema_or_config: read constraint from the schema key,
        // falling back to the config key (str_min_length, str_max_length,
        // str_strip_whitespace, str_to_lower, str_to_upper).
        auto cfg_int = [&](const char* cfg_key) -> std::optional<size_t> {
            if (!config.contains(cfg_key)) return std::nullopt;
            py::object val = config[cfg_key];
            if (py::isinstance<py::int_>(val)) {
                long long i = val.cast<long long>();
                if (i >= 0) return static_cast<size_t>(i);
            }
            return std::nullopt;
        };
        auto cfg_bool = [&](const char* cfg_key) -> std::optional<bool> {
            if (!config.contains(cfg_key)) return std::nullopt;
            py::object val = config[cfg_key];
            if (py::isinstance<py::bool_>(val)) return val.cast<bool>();
            return std::nullopt;
        };
        auto sch_int = [&](const char* k) -> std::optional<size_t> {
            if (!schema.contains(k)) return std::nullopt;
            py::object val = schema[k];
            if (py::isinstance<py::int_>(val)) {
                long long i = val.cast<long long>();
                if (i >= 0) return static_cast<size_t>(i);
            }
            return std::nullopt;
        };
        auto sch_bool = [&](const char* k) -> std::optional<bool> {
            if (!schema.contains(k)) return std::nullopt;
            py::object val = schema[k];
            if (py::isinstance<py::bool_>(val)) return val.cast<bool>();
            return std::nullopt;
        };
        std::optional<size_t> min_len = sch_int("min_length");
        if (!min_len) min_len = cfg_int("str_min_length");
        std::optional<size_t> max_len = sch_int("max_length");
        if (!max_len) max_len = cfg_int("str_max_length");
        std::optional<bool> strip_ws = sch_bool("strip_whitespace");
        if (!strip_ws) strip_ws = cfg_bool("str_strip_whitespace");
        std::optional<bool> to_lower = sch_bool("to_lower");
        if (!to_lower) to_lower = cfg_bool("str_to_lower");
        std::optional<bool> to_upper = sch_bool("to_upper");
        if (!to_upper) to_upper = cfg_bool("str_to_upper");
        // Rust: schema_or_config_same — an explicit schema value (even False)
        // overrides the config value.
        std::optional<bool> coerce_numbers = sch_bool("coerce_numbers_to_str");
        if (!coerce_numbers) coerce_numbers = cfg_bool("coerce_numbers_to_str");
        bool has_pattern = schema.contains("pattern") && py::isinstance<py::str>(schema["pattern"]);
        if (min_len || max_len || has_pattern || strip_ws || to_lower || to_upper) {
            auto v = std::make_shared<StrConstrainedValidator>();
            if (min_len) v->min_length = min_len;
            if (max_len) v->max_length = max_len;
            if (has_pattern) v->pattern = schema["pattern"].cast<std::string>();
            if (strip_ws) v->strip_whitespace = *strip_ws;
            if (to_lower) v->to_lower = *to_lower;
            if (to_upper) v->to_upper = *to_upper;
            if (coerce_numbers) v->coerce_numbers_to_str = *coerce_numbers;
            return v;
        }
        auto v = std::make_shared<StringValidator>();
        if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
            v->strict = schema["strict"].cast<bool>();
        }
        if (coerce_numbers) v->coerce_numbers_to_str = *coerce_numbers;
        return v;
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
        auto bv = std::make_shared<BytesValidator>();
        if (config.contains("val_json_bytes")) {
            bv->val_json_bytes = config["val_json_bytes"].cast<std::string>();
        }
        return bv;
    }

    // --- Date/time validators ---
    if (type == "date") return std::make_shared<DateValidator>();
    if (type == "time") return std::make_shared<TimeValidator>();
    if (type == "datetime") return std::make_shared<DatetimeValidator>();
    if (type == "timedelta") return std::make_shared<TimedeltaValidator>();

    // --- URL validators ---
    if (type == "url") {
        auto v = std::make_shared<UrlValidator>();
        if (schema.contains("max_length") && py::isinstance<py::int_>(schema["max_length"])) {
            long long ml = schema["max_length"].cast<long long>();
            if (ml >= 0) v->max_length = static_cast<size_t>(ml);
        }
        if (schema.contains("allowed_schemes") &&
            (py::isinstance<py::list>(schema["allowed_schemes"]) ||
             py::isinstance<py::tuple>(schema["allowed_schemes"]))) {
            for (auto item : py::cast<py::sequence>(schema["allowed_schemes"])) {
                if (py::isinstance<py::str>(item)) {
                    v->allowed_schemes.push_back(item.cast<std::string>());
                }
            }
        }
        if (schema.contains("host_required") && py::isinstance<py::bool_>(schema["host_required"])) {
            v->host_required = schema["host_required"].cast<bool>();
        }
        if (schema.contains("preserve_empty_path") && py::isinstance<py::bool_>(schema["preserve_empty_path"])) {
            v->preserve_empty_path = schema["preserve_empty_path"].cast<bool>();
        } else if (config.contains("url_preserve_empty_path") && py::isinstance<py::bool_>(config["url_preserve_empty_path"])) {
            v->preserve_empty_path = config["url_preserve_empty_path"].cast<bool>();
        }
        if (schema.contains("default_host") && !schema["default_host"].is_none()) {
            v->default_host = schema["default_host"].cast<std::string>();
        }
        if (schema.contains("default_port") && py::isinstance<py::int_>(schema["default_port"])) {
            v->default_port = schema["default_port"].cast<int>();
        }
        if (schema.contains("default_path") && !schema["default_path"].is_none()) {
            v->default_path = schema["default_path"].cast<std::string>();
        }
        if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
            v->strict = schema["strict"].cast<bool>();
        }
        return v;
    }
    if (type == "multi-host-url") {
        auto v = std::make_shared<MultiHostUrlValidator>();
        if (schema.contains("max_length") && py::isinstance<py::int_>(schema["max_length"])) {
            long long ml = schema["max_length"].cast<long long>();
            if (ml >= 0) v->max_length = static_cast<size_t>(ml);
        }
        if (schema.contains("allowed_schemes") &&
            (py::isinstance<py::list>(schema["allowed_schemes"]) ||
             py::isinstance<py::tuple>(schema["allowed_schemes"]))) {
            for (auto item : py::cast<py::sequence>(schema["allowed_schemes"])) {
                if (py::isinstance<py::str>(item)) {
                    v->allowed_schemes.push_back(item.cast<std::string>());
                }
            }
        }
        if (schema.contains("host_required") && py::isinstance<py::bool_>(schema["host_required"])) {
            v->host_required = schema["host_required"].cast<bool>();
        }
        if (schema.contains("preserve_empty_path") && py::isinstance<py::bool_>(schema["preserve_empty_path"])) {
            v->preserve_empty_path = schema["preserve_empty_path"].cast<bool>();
        } else if (config.contains("url_preserve_empty_path") && py::isinstance<py::bool_>(config["url_preserve_empty_path"])) {
            v->preserve_empty_path = config["url_preserve_empty_path"].cast<bool>();
        }
        if (schema.contains("default_host") && !schema["default_host"].is_none()) {
            v->default_host = schema["default_host"].cast<std::string>();
        }
        if (schema.contains("default_port") && py::isinstance<py::int_>(schema["default_port"])) {
            v->default_port = schema["default_port"].cast<int>();
        }
        if (schema.contains("default_path") && !schema["default_path"].is_none()) {
            v->default_path = schema["default_path"].cast<std::string>();
        }
        if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
            v->strict = schema["strict"].cast<bool>();
        }
        return v;
    }

    // --- UUID ---
    if (type == "uuid") {
        auto v = std::make_shared<UuidValidator>();
        if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
            v->strict = schema["strict"].cast<bool>();
        }
        return v;
    }

    // --- Decimal ---
    if (type == "decimal" || type == "decimal-constrained") {
        auto v = std::make_shared<DecimalValidator>();
        if (schema.contains("strict") && py::isinstance<py::bool_>(schema["strict"])) {
            v->strict = schema["strict"].cast<bool>();
        }
        if (schema.contains("gt") && !schema["gt"].is_none()) v->gt = schema["gt"];
        if (schema.contains("lt") && !schema["lt"].is_none()) v->lt = schema["lt"];
        if (schema.contains("ge") && !schema["ge"].is_none()) v->ge = schema["ge"];
        if (schema.contains("le") && !schema["le"].is_none()) v->le = schema["le"];
        return v;
    }

    // --- Literal ---
    if (type == "literal") {
        std::string expected_repr;
        py::object expected_obj;
        if (schema.contains("expected")) {
            expected_obj = schema["expected"];
            auto lst = expected_obj.cast<py::sequence>();
            std::vector<std::string> reprs;
            for (auto item : lst) {
                reprs.push_back(py::repr(item).cast<std::string>());
            }
            // Build expected_repr like "repr1 or repr2"
            if (!reprs.empty()) {
                expected_repr = reprs[0];
                for (size_t i = 1; i < reprs.size(); ++i) {
                    expected_repr += " or ";
                    expected_repr += reprs[i];
                }
            }
        } else {
            expected_obj = py::list();
        }
        return std::make_shared<LiteralValidator>(expected_obj, std::move(expected_repr));
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
                if (py_hasattr(item, "value")) {
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
            // Accept anything usable as the second isinstance argument:
            // builtin/metaclass types, ABCMeta subclasses (collections.abc)
            // and typing special forms like Sequence all pass here.
            bool class_like = false;
            try {
                class_like = py::isinstance<py::type>(cls) || py_hasattr(cls, "__mro__");
            } catch (...) {}
            if (class_like) {
                v->set_py_class(cls);
                // Rust uses the class qualname (no module prefix) for the error ctx
                try {
                    v->set_class_name(py::str(py::getattr(cls, "__qualname__")).cast<std::string>());
                } catch (...) {
                    v->set_class_name(py::str(cls).cast<std::string>());
                }
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
            bool sub_like = false;
            try {
                sub_like = py::isinstance<py::type>(cls) || py_hasattr(cls, "__mro__");
            } catch (...) {}
            if (py::isinstance<py::str>(cls)) {
                v->set_class_name(cls.cast<std::string>());
            } else if (sub_like) {
                v->set_py_class(cls);
                // Rust uses the class qualname (no module prefix) for the error ctx
                try {
                    v->set_class_name(py::str(py::getattr(cls, "__qualname__")).cast<std::string>());
                } catch (...) {
                    v->set_class_name(py::str(cls).cast<std::string>());
                }
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
        std::string default_type;
        py::object callable_default;
        if (schema.contains("default")) {
            auto py_default = schema["default"];
            if (py::isinstance<py::str>(py_default)) {
                default_val = std::make_shared<std::string>(py_default.cast<std::string>());
                default_type = "str";
            } else if (py::isinstance<py::int_>(py_default)) {
                default_val = std::make_shared<int64_t>(py_default.cast<int64_t>());
                default_type = "int";
            } else if (py::isinstance<py::float_>(py_default)) {
                default_val = std::make_shared<double>(py_default.cast<double>());
                default_type = "float";
            } else if (py::isinstance<py::bool_>(py_default)) {
                default_val = std::make_shared<bool>(py_default.cast<bool>());
                default_type = "bool";
            } else if (!py_default.is_none()) {
                // Non-primitive default (list, dict, callable, custom object) —
                // keep as a live Python object. JSON-serializing fails for dicts
                // with non-serializable keys (e.g. Path, function) and loses the
                // original types; a live object matches Rust's behavior.
                callable_default = py_default;
            }
        }
        auto wd = std::make_shared<WithDefaultValidator>(inner, default_val, default_val_str);
        if (callable_default.ptr()) {
            wd->set_default_py_obj(std::move(callable_default));
        }
        if (!default_type.empty()) {
            wd->set_default_type(default_type);
        }
        if (schema.contains("default") && schema["default"].is_none()) {
            // Explicit None default — distinguishable from "no default" so
            // default_value() returns None instead of omitting the field
            wd->set_default_is_none(true);
        }
        if (schema.contains("default_factory") && !schema["default_factory"].is_none()) {
            wd->set_default_factory(schema["default_factory"]);
        }
        // validate_default comes from the schema or the top-level config
        bool validate_default = false;
        if (schema.contains("validate_default") && py::isinstance<py::bool_>(schema["validate_default"])) {
            validate_default = schema["validate_default"].cast<bool>();
        }
        if (config.contains("validate_default") && py::isinstance<py::bool_>(config["validate_default"])) {
            validate_default = config["validate_default"].cast<bool>();
        }
        if (validate_default) {
            wd->set_validate_default(true);
        }
        return wd;
    }

    // --- Function validators (Before / After / Wrap / Plain) ---
    if (type == "function-before") {
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
        if (schema.contains("function")) {
            func = schema["function"];
            if (py::isinstance<py::dict>(func)) {
                py::dict func_dict = func.cast<py::dict>();
                if (func_dict.contains("function")) {
                    func = func_dict["function"];
                }
            }
        }
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
        auto uv = std::make_shared<UnionValidator>(std::move(choices));
        if (schema.contains("custom_error_type") && !schema["custom_error_type"].is_none()) {
            std::string cmsg = schema.contains("custom_error_message") && !schema["custom_error_message"].is_none()
                ? schema["custom_error_message"].cast<std::string>() : std::string();
            uv->set_custom_error(schema["custom_error_type"].cast<std::string>(), cmsg);
        }
        return uv;
    }

    // --- TaggedUnion ---
    if (type == "tagged-union") {
        auto disc_obj = schema["discriminator"];
        // Callable discriminator: build a tag->validator map
        if (py::isinstance<py::function>(disc_obj) || py_hasattr(disc_obj, "__call__")) {
            std::unordered_map<std::string, std::shared_ptr<Validator>> choice_map;
            if (schema.contains("choices")) {
                auto choices_obj = schema["choices"];
                if (py::isinstance<py::dict>(choices_obj)) {
                    auto choices_dict = choices_obj.cast<py::dict>();
                    for (auto item : choices_dict) {
                        std::string tag = py::str(item.first).cast<std::string>();
                        auto choice = build_from_py_dict(item.second.cast<py::dict>(), config, definitions);
                        choice_map[tag] = std::move(choice);
                    }
                }
            }
            return std::make_shared<TaggedUnionValidator>(disc_obj.cast<py::object>(), std::move(choice_map));
        }
        // String discriminator: try all validators in order
        std::string discriminator = py_str(schema, "discriminator");
        std::vector<std::shared_ptr<Validator>> choices;
        if (schema.contains("choices")) {
            auto choices_obj = schema["choices"];
            if (py::isinstance<py::list>(choices_obj)) {
                auto choices_list = choices_obj.cast<py::list>();
                for (auto item : choices_list) {
                    auto choice = build_from_py_dict(item.cast<py::dict>(), config, definitions);
                    choices.push_back(choice);
                }
            } else if (py::isinstance<py::dict>(choices_obj)) {
                // Tagged union: choices is a dict mapping tag -> schema
                auto choices_dict = choices_obj.cast<py::dict>();
                for (auto item : choices_dict) {
                    auto choice = build_from_py_dict(item.second.cast<py::dict>(), config, definitions);
                    choices.push_back(choice);
                }
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
        if (schema.contains("min_length")) {
            v->min_length = schema["min_length"].cast<size_t>();
        }
        if (schema.contains("max_length")) {
            v->max_length = schema["max_length"].cast<size_t>();
        }
        return v;
    }

    // --- Generator ---
    if (type == "generator") {
        auto v = std::make_shared<GeneratorValidator>();
        if (schema.contains("items_schema")) {
            v->items_schema = build_from_py_dict(schema["items_schema"].cast<py::dict>(), config, definitions);
        }
        return v;
    }

    // --- Custom Error ---
    if (type == "custom-error") {
        std::shared_ptr<Validator> inner;
        if (schema.contains("schema")) {
            inner = build_from_py_dict(schema["schema"].cast<py::dict>(), config, definitions);
        }
        std::string msg;
        if (schema.contains("custom_error_message")) {
            msg = py::str(schema["custom_error_message"]).cast<std::string>();
        }
        std::string error_type;
        if (schema.contains("custom_error_type")) {
            error_type = py::str(schema["custom_error_type"]).cast<std::string>();
        }
        return std::make_shared<CustomErrorValidator>(std::move(inner), std::move(msg), std::move(error_type));
    }

    // --- Tuple ---
    if (type == "tuple" || type == "tuple-constrained" || type == "constr-tuple" || type == "tuple-variable") {
        auto tv = std::make_shared<TupleValidator>();
        if (schema.contains("items_schema")) {
            auto items = schema["items_schema"].cast<py::list>();
            for (auto item : items) {
                tv->items.push_back(build_from_py_dict(item.cast<py::dict>(), config, definitions));
            }
        }
        if (schema.contains("variadic_item_index")) {
            tv->variadic = true;
        }
        return tv;
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
        if (schema.contains("min_length")) {
            v->min_length = schema["min_length"].cast<size_t>();
        }
        if (schema.contains("max_length")) {
            v->max_length = schema["max_length"].cast<size_t>();
        }
        return v;
    }

    // --- Set ---
    if (type == "set" || type == "set-constrained" || type == "constr-set") {
        auto v = std::make_shared<SetValidator>();
        if (schema.contains("items_schema")) {
            v->items_schema = build_from_py_dict(schema["items_schema"].cast<py::dict>(), config, definitions);
        }
        if (schema.contains("min_length")) {
            v->min_length = schema["min_length"].cast<size_t>();
        }
        if (schema.contains("max_length")) {
            v->max_length = schema["max_length"].cast<size_t>();
        }
        return v;
    }
    if (type == "frozenset" || type == "frozenset-constrained") {
        auto v = std::make_shared<FrozenSetValidator>();
        if (schema.contains("items_schema")) {
            v->items_schema = build_from_py_dict(schema["items_schema"].cast<py::dict>(), config, definitions);
        }
        if (schema.contains("min_length")) {
            v->min_length = schema["min_length"].cast<size_t>();
        }
        if (schema.contains("max_length")) {
            v->max_length = schema["max_length"].cast<size_t>();
        }
        return v;
    }

    // --- Model ---
    if (type == "model") {
        // Merge model's own config into the config dict for inner validators
        py::dict inner_config = config;
        if (schema.contains("config") && py::isinstance<py::dict>(schema["config"])) {
            py::dict model_config = schema["config"].cast<py::dict>();
            for (auto item : model_config) {
                inner_config[item.first] = item.second;
            }
        }
        std::shared_ptr<Validator> inner;
        if (schema.contains("schema")) {
            inner = build_from_py_dict(schema["schema"].cast<py::dict>(), inner_config, definitions);
        }
        std::string model_name;
        if (schema.contains("title") && !schema["title"].is_none()) {
            model_name = py::str(schema["title"]).cast<std::string>();
        } else if (schema.contains("model_name") && !schema["model_name"].is_none()) {
            model_name = py::str(schema["model_name"]).cast<std::string>();
        } else if (schema.contains("cls") && !schema["cls"].is_none()) {
            auto cls = schema["cls"];
            if (py_hasattr(cls, "__name__")) {
                model_name = cls.attr("__name__").cast<std::string>();
            } else {
                model_name = py::str(cls).cast<std::string>();
            }
        }
        // Propagate model name to inner ModelFieldsValidator (JSON path defaults to "Model")
        if (!model_name.empty()) {
            if (auto mfv = std::dynamic_pointer_cast<ModelFieldsValidator>(inner)) {
                mfv->set_model_name(model_name);
            }
        }
        // Propagate from_attributes to inner ModelFieldsValidator
        {
            bool fa = false;
            if (schema.contains("from_attributes") && !schema["from_attributes"].is_none()) {
                fa = schema["from_attributes"].cast<bool>();
            } else if (config.contains("from_attributes") && py::isinstance<py::bool_>(config["from_attributes"])) {
                fa = config["from_attributes"].cast<bool>();
            }
            if (fa) {
                if (auto mfv = std::dynamic_pointer_cast<ModelFieldsValidator>(inner)) {
                    mfv->set_from_attributes(true);
                }
            }
        }
        // Propagate alias lookup mode (validate_by_name/validate_by_alias/
        // loc_by_alias) from the model config to the inner ModelFieldsValidator.
        // Read from inner_config (parent + this model's own config merged).
        {
            if (auto mfv = std::dynamic_pointer_cast<ModelFieldsValidator>(inner)) {
                if (inner_config.contains("validate_by_name") && py::isinstance<py::bool_>(inner_config["validate_by_name"])) {
                    mfv->set_validate_by_name(inner_config["validate_by_name"].cast<bool>());
                }
                if (inner_config.contains("validate_by_alias") && py::isinstance<py::bool_>(inner_config["validate_by_alias"])) {
                    mfv->set_validate_by_alias(inner_config["validate_by_alias"].cast<bool>());
                }
                if (inner_config.contains("loc_by_alias") && py::isinstance<py::bool_>(inner_config["loc_by_alias"])) {
                    mfv->set_loc_by_alias(inner_config["loc_by_alias"].cast<bool>());
                }
            }
        }
        bool root_model = false;
        if (schema.contains("root_model")) {
            root_model = schema["root_model"].cast<bool>();
        }
        py::object model_cls = py::none();
        if (schema.contains("cls")) {
            try { model_cls = schema["cls"].cast<py::object>(); } catch (...) {}
        }

        // Parse revalidate_instances from schema or the model's own config
        // (models ignore the parent config and always use the config from this model)
        RevalidateInstances revalidate = RevalidateInstances::Never;
        std::string revalidate_str;
        if (schema.contains("revalidate_instances")) {
            try { revalidate_str = schema["revalidate_instances"].cast<std::string>(); } catch (...) {}
        } else if (schema.contains("config")) {
            try {
                auto model_config = schema["config"].cast<py::dict>();
                if (model_config.contains("revalidate_instances")) {
                    revalidate_str = model_config["revalidate_instances"].cast<std::string>();
                }
            } catch (...) {}
        }
        if (revalidate_str == "always") {
            revalidate = RevalidateInstances::Always;
        } else if (revalidate_str == "subclass-instances") {
            revalidate = RevalidateInstances::SubclassInstances;
        }
        // Default is "never" which maps to RevalidateInstances::Never

        auto v = std::make_shared<ModelValidator>(inner, model_name, /*frozen=*/false, /*custom_init=*/false, root_model, model_cls, revalidate);
        return v;
    }

    // --- Chain ---
    if (type == "chain") {
        std::vector<std::shared_ptr<Validator>> validators;
        if (schema.contains("steps")) {
            auto steps = schema["steps"].cast<py::list>();
            for (auto step : steps) {
                validators.push_back(build_from_py_dict(step.cast<py::dict>(), config, definitions));
            }
        }
        return std::make_shared<ChainValidator>(std::move(validators));
    }

    // --- ModelFields / TypedDict ---
    // (dataclass-args is handled separately below — its fields are a list,
    // not a dict, so the model-fields path must not intercept it)
    if (type == "model-fields" || type == "typed-dict") {
        // TypedDict must use TypedDictValidator (name "typed-dict") so result
        // conversion produces a plain dict (no __pydantic_fields_set__, extras
        // merged in) matching Rust. ModelFieldsValidator names itself
        // "model-fields" and would leak __pydantic_fields_set__ into the output.
        std::shared_ptr<ModelFieldsValidator> v;
        if (type == "typed-dict") {
            v = std::make_shared<TypedDictValidator>();
        } else {
            v = std::make_shared<ModelFieldsValidator>();
        }
        if (schema.contains("fields")) {
            auto fields_dict = schema["fields"].cast<py::dict>();
            for (auto item : fields_dict) {
                std::string field_name = py::str(item.first).cast<std::string>();
                auto field_def = item.second.cast<py::dict>();
                
                // Extract field schema
                std::shared_ptr<Validator> field_validator;
                py::dict field_schema_dict;
                std::string default_val_str;
                py::object field_default_py_obj = py::none();
                bool required = true;

                // typed-dict fields declare requiredness explicitly
                if (field_def.contains("required") && py::isinstance<py::bool_>(field_def["required"])) {
                    required = field_def["required"].cast<bool>();
                }

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
                            } else if (py_hasattr(py_default, "__call__")) {
                                // Callable default — store as Python object, not JSON string
                                // (will be handled by default_py_obj in missing-field path)
                            } else if (py::isinstance<py::str>(py_default) || py::isinstance<py::int_>(py_default) ||
                                       py::isinstance<py::float_>(py_default) || py::isinstance<py::bool_>(py_default) ||
                                       py::isinstance<py::list>(py_default) || py::isinstance<py::dict>(py_default)) {
                                // JSON-native types — try to convert to JSON string.
                                // Dicts with non-serializable keys (e.g. Path, function)
                                // fail; fall back to a live Python object.
                                try {
                                    default_val_str = py_default_to_json_str(py_default);
                                } catch (py::error_already_set& e) {
                                    e.restore();
                                    PyErr_Clear();
                                    default_val_str.clear();
                                    field_default_py_obj = py_default;
                                }
                            } else {
                                // Non-JSON types (timedelta, date, datetime, Decimal, etc.)
                                // Store as Python object for proper validation later
                                // Will be handled by default_py_obj in missing-field path
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

                FieldInfo info;
                info.name = field_name;
                info.schema = field_validator;
                info.required = required;
                info.default_value_str = default_val_str;
                info.frozen = false;
                // validate_default: per-field flag on the with-default schema
                // or the top-level config (Rust reads both).
                if (field_def.contains("schema")) {
                    auto fsd_vd = field_def["schema"];
                    if (fsd_vd.contains("validate_default") && py::isinstance<py::bool_>(fsd_vd["validate_default"])) {
                        info.validate_default = fsd_vd["validate_default"].cast<bool>();
                    }
                }
                if (!info.validate_default && config.contains("validate_default") &&
                    py::isinstance<py::bool_>(config["validate_default"])) {
                    info.validate_default = config["validate_default"].cast<bool>();
                }
                // Extract default_factory and callable defaults from the field schema
                if (field_def.contains("schema")) {
                    auto fsd = field_def["schema"].cast<py::dict>();
                    if (fsd.contains("default_factory") && !fsd["default_factory"].is_none()) {
                        info.default_factory = fsd["default_factory"];
                        info.required = false;
                        if (fsd.contains("default_factory_takes_data") &&
                            py::isinstance<py::bool_>(fsd["default_factory_takes_data"])) {
                            info.default_factory_takes_data = fsd["default_factory_takes_data"].cast<bool>();
                        }
                    } else if (fsd.contains("default")) {
                        auto py_default = fsd["default"];
                        if (!py_default.is_none()) {
                            if (!field_default_py_obj.is_none()) {
                                // JSON serialization failed (e.g. dict with
                                // non-serializable keys) — use the live object.
                                info.default_py_obj = field_default_py_obj;
                                info.required = false;
                            } else if (py_hasattr(py_default, "__call__")) {
                                info.default_py_obj = py_default;
                                info.required = false;
                            } else if (!py::isinstance<py::str>(py_default) && !py::isinstance<py::int_>(py_default) &&
                                       !py::isinstance<py::float_>(py_default) && !py::isinstance<py::bool_>(py_default) &&
                                       !py::isinstance<py::list>(py_default) && !py::isinstance<py::dict>(py_default)) {
                                // Non-JSON types (timedelta, date, datetime, Decimal, etc.)
                                // Store as Python object for proper validation
                                info.default_py_obj = py_default;
                                info.required = false;
                            }
                        }
                    }
                }
                // Parse validation_alias (string, AliasPath = flat list, or AliasChoices = list of lists)
                if (field_def.contains("validation_alias")) {
                    auto alias_val = field_def["validation_alias"];
                    if (py::isinstance<py::str>(alias_val)) {
                        info.alias = alias_val.cast<std::string>();
                        info.validation_paths.push_back({info.alias});
                        info.has_alias = true;
                    } else if (py::isinstance<py::list>(alias_val) ||
                               py::isinstance<py::tuple>(alias_val)) {
                        py::sequence seq = py::cast<py::sequence>(alias_val);
                        bool is_choices = false;
                        for (auto item : seq) {
                            if (py::isinstance<py::list>(item) || py::isinstance<py::tuple>(item)) {
                                is_choices = true;
                                break;
                            }
                        }
                        if (is_choices) {
                            for (auto item : seq) {
                                std::vector<std::string> path;
                                if (py::isinstance<py::list>(item) || py::isinstance<py::tuple>(item)) {
                                    for (auto inner : py::cast<py::sequence>(item)) {
                                        if (py::isinstance<py::str>(inner)) path.push_back(inner.cast<std::string>());
                                        else if (py::isinstance<py::int_>(inner)) path.push_back(std::to_string(inner.cast<long>()));
                                    }
                                } else if (py::isinstance<py::str>(item)) {
                                    path.push_back(item.cast<std::string>());
                                }
                                if (!path.empty()) {
                                    info.validation_paths.push_back(path);
                                    if (info.alias.empty()) info.alias = path[0];
                                }
                            }
                        } else {
                            std::vector<std::string> path;
                            for (auto item : seq) {
                                if (py::isinstance<py::str>(item)) path.push_back(item.cast<std::string>());
                                else if (py::isinstance<py::int_>(item)) path.push_back(std::to_string(item.cast<long>()));
                            }
                            if (!path.empty()) {
                                info.validation_paths.push_back(path);
                                info.alias = path[0];
                            }
                        }
                        info.has_alias = !info.validation_paths.empty();
                    }
                }
                v->add_field(field_name, std::move(info));
            }
        }
        
        // Extract extras behavior from the schema (typed-dicts embed their own
        // extra_behavior) or the top-level config
        std::string extra_str = py_str(schema, "extra_behavior",
            py_str(config, "extra_fields_behavior",
                py_str(config, "extra_behavior", py_str(config, "extra", "ignore"))));
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

    // --- Arguments (function parameter validation) ---
    if (type == "arguments") {
        auto v = std::make_shared<ArgumentsValidator>();
        if (schema.contains("arguments_schema")) {
            auto args_list = schema["arguments_schema"].cast<py::list>();
            for (auto item : args_list) {
                auto arg = item.cast<py::dict>();
                ArgumentsValidator::Parameter p;
                p.name = py_str(arg, "name");
                std::string mode = py_str(arg, "mode", "positional_or_keyword");
                p.positional = (mode == "positional_only" || mode == "positional_or_keyword");
                p.positional_only = (mode == "positional_only");
                if (arg.contains("alias") && !arg["alias"].is_none()) {
                    // alias: plain string, or AliasChoices as a nested list of
                    // strings ([["d"], ["e"]]); look up each candidate in order
                    py::object alias = arg["alias"];
                    if (py::isinstance<py::str>(alias)) {
                        p.validation_aliases.push_back(alias.cast<std::string>());
                    } else if (py::isinstance<py::list>(alias)) {
                        for (auto choice : alias.cast<py::list>()) {
                            if (py::isinstance<py::str>(choice)) {
                                p.validation_aliases.push_back(choice.cast<std::string>());
                            } else if (py::isinstance<py::list>(choice)) {
                                for (auto inner : choice.cast<py::list>()) {
                                    p.validation_aliases.push_back(py::str(inner).cast<std::string>());
                                }
                            }
                        }
                    }
                }
                if (arg.contains("schema")) {
                    p.validator = build_from_py_dict(arg["schema"].cast<py::dict>(), config, definitions);
                }
                v->parameters.push_back(std::move(p));
            }
        }
        for (size_t i = 0; i < v->parameters.size(); ++i) {
            if (v->parameters[i].positional) v->positional_params_count = i + 1;
        }
        if (schema.contains("var_args_schema")) {
            v->var_args_validator = build_from_py_dict(schema["var_args_schema"].cast<py::dict>(), config, definitions);
        }
        v->var_kwargs_mode = py_str(schema, "var_kwargs_mode", "uniform");
        if (schema.contains("var_kwargs_schema")) {
            v->var_kwargs_validator = build_from_py_dict(schema["var_kwargs_schema"].cast<py::dict>(), config, definitions);
        }
        // Keyword lookup mode (default: by alias only)
        if (schema.contains("validate_by_alias") && py::isinstance<py::bool_>(schema["validate_by_alias"])) {
            v->validate_by_alias = schema["validate_by_alias"].cast<bool>();
        }
        if (schema.contains("validate_by_name") && py::isinstance<py::bool_>(schema["validate_by_name"])) {
            v->validate_by_name = schema["validate_by_name"].cast<bool>();
        }
        std::string extra_str = py_str(schema, "extra",
            py_str(config, "extra_fields_behavior",
                py_str(config, "extra_behavior", py_str(config, "extra", "forbid"))));
        v->extra = extra_behavior_from_string(extra_str);
        return v;
    }

    // --- Call (validate function call: arguments + return value) ---
    if (type == "call") {
        std::shared_ptr<Validator> arguments_validator;
        if (schema.contains("arguments_schema")) {
            arguments_validator = build_from_py_dict(schema["arguments_schema"].cast<py::dict>(), config, definitions);
        }
        py::object function = py::none();
        if (schema.contains("function")) {
            function = schema["function"];
        }
        std::shared_ptr<Validator> return_validator;
        if (schema.contains("return_schema")) {
            return_validator = build_from_py_dict(schema["return_schema"].cast<py::dict>(), config, definitions);
        }
        return std::make_shared<CallValidator>(arguments_validator, function, return_validator);
    }

    // --- Dataclass args (validates dataclass fields from ArgsKwargs/dict) ---
    if (type == "dataclass-args") {
        auto v = std::make_shared<ArgumentsValidator>();
        if (schema.contains("fields")) {
            auto fields_list = schema["fields"].cast<py::list>();
            size_t positional_count = 0;
            for (auto item : fields_list) {
                auto f = item.cast<py::dict>();
                ArgumentsValidator::Parameter p;
                p.name = py_str(f, "name");
                if (f.contains("init") && py::isinstance<py::bool_>(f["init"])) {
                    p.init = f["init"].cast<bool>();
                }
                if (f.contains("init_only") && py::isinstance<py::bool_>(f["init_only"])) {
                    p.init_only = f["init_only"].cast<bool>();
                }
                // validation_alias: plain string, or AliasChoices as a nested
                // list of strings. Lookup uses the alias first, then the name.
                if (f.contains("validation_alias") && !f["validation_alias"].is_none()) {
                    py::object alias = f["validation_alias"];
                    if (py::isinstance<py::str>(alias)) {
                        p.validation_aliases.push_back(alias.cast<std::string>());
                    } else if (py::isinstance<py::list>(alias)) {
                        for (auto choice : alias.cast<py::list>()) {
                            if (py::isinstance<py::str>(choice)) {
                                p.validation_aliases.push_back(choice.cast<std::string>());
                            } else if (py::isinstance<py::list>(choice)) {
                                for (auto inner : choice.cast<py::list>()) {
                                    p.validation_aliases.push_back(py::str(inner).cast<std::string>());
                                }
                            }
                        }
                    }
                }
                // Rust: `kw_only.unwrap_or(true)` — missing/None means
                // kw_only; only an explicit false is positional.
                bool kw_only = true;
                if (f.contains("kw_only") && !f["kw_only"].is_none() &&
                    py::isinstance<py::bool_>(f["kw_only"])) {
                    kw_only = f["kw_only"].cast<bool>();
                }
                p.positional = !kw_only;
                p.positional_only = false;
                if (f.contains("schema")) {
                    p.validator = build_from_py_dict(f["schema"].cast<py::dict>(), config, definitions);
                }
                v->parameters.push_back(std::move(p));
                if (!kw_only) positional_count++;
            }
            v->positional_params_count = positional_count;
        }
        // Dataclasses default to extra=ignore
        std::string extra_str = py_str(schema, "extra_behavior",
            py_str(config, "extra_fields_behavior",
                py_str(config, "extra_behavior", py_str(config, "extra", "ignore"))));
        v->extra = extra_behavior_from_string(extra_str);
        v->dataclass_mode = true;
        v->dataclass_name_ = py_str(schema, "dataclass_name", "");
        if (schema.contains("collect_init_only") && py::isinstance<py::bool_>(schema["collect_init_only"])) {
            v->collect_init_only_ = schema["collect_init_only"].cast<bool>();
        }
        return v;
    }

    // --- Dataclass (validate + construct a pydantic dataclass instance) ---
    if (type == "dataclass") {
        auto v = std::make_shared<PyDataclassValidator>();
        // Dataclasses ignore the parent config and use their own embedded config
        py::dict inner_config = config;
        if (schema.contains("config") && py::isinstance<py::dict>(schema["config"])) {
            inner_config = schema["config"].cast<py::dict>();
        }
        if (schema.contains("schema")) {
            v->set_args_validator(build_from_py_dict(schema["schema"].cast<py::dict>(), inner_config, definitions));
        }
        if (schema.contains("cls") && !schema["cls"].is_none()) {
            v->set_class(schema["cls"]);
        }
        if (schema.contains("post_init")) {
            try { v->set_post_init(schema["post_init"].cast<bool>()); } catch (...) {}
        }
        if (schema.contains("fields")) {
            std::vector<std::string> names;
            for (auto f : schema["fields"].cast<py::list>()) {
                names.push_back(py::str(f).cast<std::string>());
            }
            v->set_field_names(std::move(names));
        }
        // revalidate_instances (Rust `schema_or_config_same`): "always",
        // "subclass-instances", or "never" (default).
        {
            std::string rv;
            if (schema.contains("revalidate_instances") && py::isinstance<py::str>(schema["revalidate_instances"])) {
                rv = schema["revalidate_instances"].cast<std::string>();
            } else if (inner_config.contains("revalidate_instances") &&
                       py::isinstance<py::str>(inner_config["revalidate_instances"])) {
                rv = inner_config["revalidate_instances"].cast<std::string>();
            }
            if (rv == "always") v->set_revalidate(RevalidateInstances::Always);
            else if (rv == "subclass-instances") v->set_revalidate(RevalidateInstances::SubclassInstances);
        }
        return v;
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
