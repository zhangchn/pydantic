#include "pydantic_core/validator.hpp"
#include "pydantic_core/validators/basic.hpp"
#include "pydantic_core/validators/containers.hpp"
#include "pydantic_core/validators/complex.hpp"
#include "pydantic_core/validators/functions.hpp"
#include "pydantic_core/validators/special.hpp"
#include "pydantic_core/validators/model_fields.hpp"
#include <unordered_map>
#include <memory>

namespace pydantic_core {

// Forward declarations
class AnyValidator;
class NoneValidator;
class BoolValidator;
class IntValidator;
class FloatValidator;
class StringValidator;
class BytesValidator;
class ListValidator;
class DictValidator;
class SetValidator;
class FrozenSetValidator;
class TupleValidator;
class NullableValidator;
class UnionValidator;
class TaggedUnionValidator;
class ModelValidator;
class ModelFieldsValidator;
class TypedDictValidator;
class LiteralValidator;
class EnumValidator;
class DateValidator;
class TimeValidator;
class DatetimeValidator;
class TimedeltaValidator;
class UrlValidator;
class UuidValidator;
class FunctionBeforeValidator;
class FunctionAfterValidator;
class FunctionPlainValidator;
class FunctionWrapValidator;
class WithDefaultValidator;
class ChainValidator;
class LaxOrStrictValidator;
class JsonOrPythonValidator;
class JsonValidator;

std::unordered_map<std::string, std::unique_ptr<BuildValidator>>& ValidatorFactory::builders() {
    static std::unordered_map<std::string, std::unique_ptr<BuildValidator>> instance;
    return instance;
}

void ValidatorFactory::register_builder(const std::string& type, std::unique_ptr<BuildValidator> builder) {
    builders()[type] = std::move(builder);
}

std::shared_ptr<Validator> ValidatorFactory::build(
    const std::unordered_map<std::string, std::string>& schema,
    const std::unordered_map<std::string, std::string>& config
) {
    SchemaParser parser(schema);
    std::string type = parser.type();
    
    if (type.empty()) {
        throw SchemaError("Schema missing 'type' field");
    }
    
    // Check registered builders first
    auto& all_builders = builders();
    auto it = all_builders.find(type);
    if (it != all_builders.end()) {
        return it->second->build(schema, config);
    }
    
    // Built-in validators
    if (type == "any") {
        return std::make_shared<AnyValidator>();
    }
    if (type == "none") {
        return std::make_shared<NoneValidator>();
    }
    if (type == "bool") {
        return std::make_shared<BoolValidator>();
    }
    if (type == "int") {
        return std::make_shared<IntValidator>();
    }
    if (type == "float") {
        return std::make_shared<FloatValidator>();
    }
    if (type == "str") {
        return std::make_shared<StringValidator>();
    }
    if (type == "bytes") {
        return std::make_shared<BytesValidator>();
    }
    if (type == "list") {
        return std::make_shared<ListValidator>();
    }
    if (type == "dict") {
        return std::make_shared<DictValidator>();
    }
    if (type == "set") {
        return std::make_shared<SetValidator>();
    }
    if (type == "frozenset") {
        return std::make_shared<FrozenSetValidator>();
    }
    if (type == "tuple") {
        return std::make_shared<TupleValidator>();
    }
    if (type == "nullable") {
        return std::make_shared<NullableValidator>();
    }
    if (type == "union") {
        return std::make_shared<UnionValidator>();
    }
    if (type == "tagged-union") {
        return std::make_shared<TaggedUnionValidator>();
    }
    if (type == "model") {
        return std::make_shared<ModelValidator>();
    }
    if (type == "model-fields") {
        return std::make_shared<ModelFieldsValidator>();
    }
    if (type == "typed-dict") {
        return std::make_shared<TypedDictValidator>();
    }
    if (type == "literal") {
        return std::make_shared<LiteralValidator>();
    }
    if (type == "enum") {
        return std::make_shared<EnumValidator>();
    }
    if (type == "date") {
        return std::make_shared<DateValidator>();
    }
    if (type == "time") {
        return std::make_shared<TimeValidator>();
    }
    if (type == "datetime") {
        return std::make_shared<DatetimeValidator>();
    }
    if (type == "timedelta") {
        return std::make_shared<TimedeltaValidator>();
    }
    if (type == "url") {
        return std::make_shared<UrlValidator>();
    }
    if (type == "uuid") {
        return std::make_shared<UuidValidator>();
    }
    if (type == "function-before") {
        return std::make_shared<FunctionBeforeValidator>();
    }
    if (type == "function-after") {
        return std::make_shared<FunctionAfterValidator>();
    }
    if (type == "function-plain") {
        return std::make_shared<FunctionPlainValidator>();
    }
    if (type == "function-wrap") {
        return std::make_shared<FunctionWrapValidator>();
    }
    if (type == "with-default") {
        return std::make_shared<WithDefaultValidator>();
    }
    if (type == "chain") {
        return std::make_shared<ChainValidator>();
    }
    if (type == "lax-or-strict") {
        return std::make_shared<LaxOrStrictValidator>();
    }
    if (type == "json-or-python") {
        return std::make_shared<JsonOrPythonValidator>();
    }
    if (type == "json") {
        return std::make_shared<JsonValidator>();
    }
    
    throw SchemaError("Unknown validator type: " + type);
}

} // namespace pydantic_core