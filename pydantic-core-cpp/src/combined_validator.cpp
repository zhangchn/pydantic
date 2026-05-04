#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include <simdjson.h>

namespace pydantic_core {

std::shared_ptr<CombinedValidator> SchemaBuilder::build(
    const std::string& schema_json,
    const std::string& config_json
) {
    // Parse schema JSON using simdjson
    simdjson::dom::parser parser;
    auto doc = parser.parse(schema_json);
    if (doc.error()) {
        throw std::runtime_error("Invalid schema JSON: " + std::string(simdjson::error_message(doc.error())));
    }

    // Convert parsed JSON to schema dict
    std::unordered_map<std::string, std::string> schema;
    auto obj = doc.get_object();
    for (auto field : obj) {
        std::string key = std::string(field.key);
        std::string value;
        
        if (field.value.is_string()) {
            auto str = field.value.get_string();
            value = std::string(str.value());
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
            // For complex types, serialize to JSON string
            simdjson::simdjson_result<std::string> serialized = simdjson::minify(field.value);
            value = serialized.value();
        }
        
        schema[key] = value;
    }

    // Parse config JSON if provided
    std::unordered_map<std::string, std::string> config;
    if (!config_json.empty()) {
        auto config_doc = parser.parse(config_json);
        if (!config_doc.error()) {
            auto config_obj = config_doc.get_object();
            for (auto field : config_obj) {
                std::string key = std::string(field.key);
                std::string value;
                
                if (field.value.is_string()) {
                    auto str = field.value.get_string();
                    value = std::string(str.value());
                } else if (field.value.is_bool()) {
                    value = field.value.get_bool() ? "true" : "false";
                } else {
                    simdjson::simdjson_result<std::string> serialized = simdjson::minify(field.value);
                    value = serialized.value();
                }
                
                config[key] = value;
            }
        }
    }

    return build_from_dict(schema, config);
}

std::shared_ptr<CombinedValidator> SchemaBuilder::build_from_dict(
    const std::unordered_map<std::string, std::string>& schema,
    const std::unordered_map<std::string, std::string>& config
) {
    SchemaParser parser(schema);
    std::string type = parser.type();

    if (type == "any") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<AnyValidator>()
        );
    } else if (type == "none") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<NoneValidator>()
        );
    } else if (type == "bool") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<BoolValidator>()
        );
    } else if (type == "int") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<IntValidator>()
        );
    } else if (type == "float") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<FloatValidator>()
        );
    } else if (type == "str") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<StringValidator>()
        );
    } else if (type == "bytes") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<BytesValidator>()
        );
    } else if (type == "list") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<ListValidator>()
        );
    } else if (type == "dict") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<DictValidator>()
        );
    } else if (type == "set") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<SetValidator>()
        );
    } else if (type == "frozenset") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<FrozenSetValidator>()
        );
    } else if (type == "tuple") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<TupleValidator>()
        );
    } else if (type == "nullable") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<NullableValidator>()
        );
    } else if (type == "union") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<UnionValidator>()
        );
    } else if (type == "tagged-union") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<TaggedUnionValidator>()
        );
    } else if (type == "model") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<ModelValidator>()
        );
    } else if (type == "model-fields") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<ModelFieldsValidator>()
        );
    } else if (type == "typed-dict") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<TypedDictValidator>()
        );
    } else if (type == "literal") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<LiteralValidator>()
        );
    } else if (type == "enum") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<EnumValidator>()
        );
    } else if (type == "date") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<DateValidator>()
        );
    } else if (type == "time") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<TimeValidator>()
        );
    } else if (type == "datetime") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<DatetimeValidator>()
        );
    } else if (type == "timedelta") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<TimedeltaValidator>()
        );
    } else if (type == "url") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<UrlValidator>()
        );
    } else if (type == "uuid") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<UuidValidator>()
        );
    } else if (type == "function-before") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<FunctionBeforeValidator>()
        );
    } else if (type == "function-after") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<FunctionAfterValidator>()
        );
    } else if (type == "function-plain") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<FunctionPlainValidator>()
        );
    } else if (type == "function-wrap") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<FunctionWrapValidator>()
        );
    } else if (type == "default") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<WithDefaultValidator>()
        );
    } else if (type == "chain") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<ChainValidator>()
        );
    } else if (type == "lax-or-strict") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<LaxOrStrictValidator>()
        );
    } else if (type == "json-or-python") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<JsonOrPythonValidator>()
        );
    } else if (type == "json") {
        return std::make_shared<CombinedValidator>(
            std::make_shared<JsonValidator>()
        );
    } else {
        throw std::runtime_error("Unknown validator type: " + type);
    }
}

} // namespace pydantic_core
