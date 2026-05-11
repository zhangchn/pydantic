#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/validators/model_fields.hpp"
#include <simdjson.h>

namespace pydantic_core {

// Helper: convert string to ExtraBehavior
static ExtraBehavior extra_behavior_from_string(const std::string& s) {
    if (s == "allow") return ExtraBehavior::Allow;
    if (s == "forbid") return ExtraBehavior::Forbid;
    return ExtraBehavior::Ignore;
}

// Forward declaration for internal recursive schema parsing
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
    if (type == "int") return std::make_shared<IntValidator>();
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
    if (type == "uuid") return std::make_shared<UuidValidator>();

    // Complex validators need recursive parsing - handled by build_from_element
    throw SchemaError("Validator type requires recursive parsing: " + type);
}

// Internal recursive schema parser using simdjson elements
static std::shared_ptr<Validator> build_from_element(
    const simdjson::dom::element& elem,
    const std::unordered_map<std::string, std::string>& config
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
    if (type == "int") return std::make_shared<IntValidator>();
    if (type == "float") return std::make_shared<FloatValidator>();
    if (type == "str") return std::make_shared<StringValidator>();
    if (type == "bytes") return std::make_shared<BytesValidator>();

    // List with item validator
    if (type == "list") {
        auto list_validator = std::make_shared<ListValidator>();
        auto items_schema = elem["items_schema"];
        if (!items_schema.error() && !items_schema.value().is_null()) {
            auto inner = build_from_element(items_schema.value(), config);
            // ListValidator needs to be updated to support inner validators
            // For now, build succeeds but item validation is not performed
        }
        return list_validator;
    }

    if (type == "dict") return std::make_shared<DictValidator>();
    if (type == "set") return std::make_shared<SetValidator>();
    if (type == "frozenset") return std::make_shared<FrozenSetValidator>();
    if (type == "tuple") return std::make_shared<TupleValidator>();

    // Nullable
    if (type == "nullable") {
        auto schema_elem = elem["schema"];
        if (!schema_elem.error()) {
            auto inner = build_from_element(schema_elem.value(), config);
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
                validators.push_back(build_from_element(choice, config));
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
            inner = build_from_element(schema_elem.value(), config);
        }

        auto default_elem = elem["default"];
        std::shared_ptr<void> default_value;
        if (!default_elem.error()) {
            // Parse default value based on JSON type
            if (default_elem.value().is_string()) {
                default_value = std::make_shared<std::string>(
                    std::string(default_elem.value().get_string().value()));
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
            }
        }

        if (inner) {
            return std::make_shared<WithDefaultValidator>(inner, default_value);
        }
        return std::make_shared<WithDefaultValidator>(
            std::make_shared<AnyValidator>(), default_value);
    }

    // Chain
    if (type == "chain") {
        auto steps = elem["steps"];
        if (!steps.error() && steps.value().is_array()) {
            std::vector<std::shared_ptr<Validator>> validators;
            for (auto step : steps.value().get_array().value()) {
                validators.push_back(build_from_element(step, config));
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
        if (!lax_elem.error()) lax = build_from_element(lax_elem.value(), config);
        if (!strict_elem.error()) strict = build_from_element(strict_elem.value(), config);
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

    if (type == "enum") return std::make_shared<EnumValidator>();
    if (type == "date") return std::make_shared<DateValidator>();
    if (type == "time") return std::make_shared<TimeValidator>();
    if (type == "datetime") return std::make_shared<DatetimeValidator>();
    if (type == "timedelta") return std::make_shared<TimedeltaValidator>();
    if (type == "url") return std::make_shared<UrlValidator>();
    if (type == "uuid") return std::make_shared<UuidValidator>();

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

        auto validator = std::make_shared<ModelFieldsValidator>();
        validator->set_extra_behavior(extra_behavior);
        validator->set_model_name(model_name);

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

            // Parse schema
            auto schema_elem = field_def_elem["schema"];
            if (!schema_elem.error()) {
                info.schema = build_from_element(schema_elem.value(), config);
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

            // Parse default value (from with_default_schema nested inside)
            auto default_elem = field_def_elem["default"];
            if (!default_elem.error()) {
                info.required = false;
                if (default_elem.value().is_string()) {
                    info.default_value = std::make_shared<std::string>(
                        std::string(default_elem.value().get_string().value()));
                } else if (default_elem.value().is_int64()) {
                    info.default_value = std::make_shared<int64_t>(
                        default_elem.value().get_int64());
                } else if (default_elem.value().is_uint64()) {
                    info.default_value = std::make_shared<uint64_t>(
                        default_elem.value().get_uint64());
                } else if (default_elem.value().is_double()) {
                    info.default_value = std::make_shared<double>(
                        default_elem.value().get_double());
                } else if (default_elem.value().is_bool()) {
                    info.default_value = std::make_shared<bool>(
                        default_elem.value().get_bool());
                } else if (default_elem.value().is_null()) {
                    info.default_value = nullptr;
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
                info.schema = build_from_element(schema_elem.value(), config);
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
            fields_validator = build_from_element(schema_elem.value(), config);
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
                    info.schema = build_from_element(schema_val.value(), config);
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
            inner = build_from_element(schema_val.value(), config);
        } else {
            inner = std::make_shared<AnyValidator>();
        }
        return std::make_shared<JsonValidator>(inner);
    }

    // ========================================================================
    // Definitions - for recursive schemas
    // ========================================================================
    if (type == "definitions") {
        auto schema_val = elem["schema"];
        if (!schema_val.error()) {
            return build_from_element(schema_val.value(), config);
        }
        return std::make_shared<AnyValidator>();
    }

    if (type == "definition-ref") {
        // Recursive reference - for now treat as any
        return std::make_shared<AnyValidator>();
    }

    throw SchemaError("Unknown validator type: " + type);
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

} // namespace pydantic_core
