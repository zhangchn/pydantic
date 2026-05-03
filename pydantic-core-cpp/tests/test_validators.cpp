#include <doctest/doctest.h>
#include "pydantic_core/validator.hpp"
#include "pydantic_core/validators/basic.hpp"
#include "pydantic_core/validators/containers.hpp"
#include "pydantic_core/validators/complex.hpp"
#include "pydantic_core/validators/functions.hpp"
#include "pydantic_core/validators/special.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/string_input.hpp"

using namespace pydantic_core;

TEST_SUITE("Basic Validators") {

TEST_CASE("AnyValidator") {
    AnyValidator validator;
    CHECK(validator.name() == "any");
    
    // Create a simple JSON input
    auto json_result = parse_json("42");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("NoneValidator") {
    NoneValidator validator;
    CHECK(validator.name() == "none");
    
    // Test with null
    auto json_result = parse_json("null");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    CHECK(result.value() == nullptr);
    
    // Test with non-null (should fail)
    auto json_result2 = parse_json("42");
    CHECK(json_result2.is_ok());
    
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_err());
}

TEST_CASE("BoolValidator") {
    BoolValidator validator;
    CHECK(validator.name() == "bool");
    
    // Test with true
    auto json_result = parse_json("true");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    
    // Test with false
    auto json_result2 = parse_json("false");
    CHECK(json_result2.is_ok());
    
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_ok());
    
    // Test with string (lax mode)
    auto json_result3 = parse_json("\"true\"");
    CHECK(json_result3.is_ok());
    
    auto result3 = validator.validate(*json_result3.value(), state);
    CHECK(result3.is_ok());
}

TEST_CASE("IntValidator") {
    IntValidator validator;
    CHECK(validator.name() == "int");
    
    // Test with integer
    auto json_result = parse_json("42");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    
    // Test with string (lax mode)
    auto json_result2 = parse_json("\"123\"");
    CHECK(json_result2.is_ok());
    
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_ok());
}

TEST_CASE("FloatValidator") {
    FloatValidator validator;
    CHECK(validator.name() == "float");
    
    // Test with float
    auto json_result = parse_json("3.14");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    
    // Test with integer (should work as float)
    auto json_result2 = parse_json("42");
    CHECK(json_result2.is_ok());
    
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_ok());
}

TEST_CASE("StringValidator") {
    StringValidator validator;
    CHECK(validator.name() == "str");
    
    // Test with string
    auto json_result = parse_json("\"hello\"");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    
    // Test with number (lax mode)
    auto json_result2 = parse_json("42");
    CHECK(json_result2.is_ok());
    
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_ok());
}

} // TEST_SUITE

TEST_SUITE("Container Validators") {

TEST_CASE("ListValidator") {
    ListValidator validator;
    CHECK(validator.name() == "list");
    
    // Test with array
    auto json_result = parse_json("[1, 2, 3]");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    
    // Test with non-array (should fail)
    auto json_result2 = parse_json("42");
    CHECK(json_result2.is_ok());
    
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_err());
}

TEST_CASE("DictValidator") {
    DictValidator validator;
    CHECK(validator.name() == "dict");
    
    // Test with object
    auto json_result = parse_json("{\"key\": \"value\"}");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    
    // Test with non-object (should fail)
    auto json_result2 = parse_json("42");
    CHECK(json_result2.is_ok());
    
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_err());
}

TEST_CASE("TupleValidator") {
    TupleValidator validator;
    CHECK(validator.name() == "tuple");
    
    // Test with array (tuples are arrays in JSON)
    auto json_result = parse_json("[1, 2, 3]");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

} // TEST_SUITE

TEST_SUITE("Complex Validators") {

TEST_CASE("NullableValidator") {
    auto inner = std::make_shared<IntValidator>();
    NullableValidator validator(inner);
    CHECK(validator.name() == "nullable");
    
    // Test with null
    auto json_result = parse_json("null");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    CHECK(result.value() == nullptr);
    
    // Test with integer
    auto json_result2 = parse_json("42");
    CHECK(json_result2.is_ok());
    
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_ok());
    
    // Test with string (should fail)
    auto json_result3 = parse_json("\"abc\"");
    CHECK(json_result3.is_ok());
    
    auto result3 = validator.validate(*json_result3.value(), state);
    CHECK(result3.is_err());
}

TEST_CASE("UnionValidator") {
    std::vector<std::shared_ptr<Validator>> validators;
    validators.push_back(std::make_shared<IntValidator>());
    validators.push_back(std::make_shared<StringValidator>());
    
    UnionValidator validator(validators);
    CHECK(validator.name() == "union");
    
    // Test with integer (matches first variant)
    auto json_result = parse_json("42");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    
    // Test with string (matches second variant)
    auto json_result2 = parse_json("\"hello\"");
    CHECK(json_result2.is_ok());
    
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_ok());
}

TEST_CASE("LaxOrStrictValidator") {
    auto lax = std::make_shared<StringValidator>();
    auto strict = std::make_shared<IntValidator>();
    
    LaxOrStrictValidator validator(lax, strict);
    CHECK(validator.name() == "lax-or-strict");
    
    // Test in lax mode (uses lax validator)
    ValidationState state;
    state.set_input_type(InputType::Python);
    
    auto json_result = parse_json("\"hello\"");
    CHECK(json_result.is_ok());
    
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("WithDefaultValidator") {
    auto inner = std::make_shared<IntValidator>();
    auto default_value = std::make_shared<int>(0);
    
    WithDefaultValidator validator(inner, default_value);
    CHECK(validator.name() == "with-default");
    
    // Test with integer
    auto json_result = parse_json("42");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
    
    // Test default value
    auto default_result = validator.default_value(state);
    CHECK(default_result.is_ok());
}

TEST_CASE("ChainValidator") {
    std::vector<std::shared_ptr<Validator>> validators;
    validators.push_back(std::make_shared<IntValidator>());
    validators.push_back(std::make_shared<StringValidator>());
    
    ChainValidator validator(validators);
    CHECK(validator.name() == "chain");
    
    // Test with integer (matches first)
    auto json_result = parse_json("42");
    CHECK(json_result.is_ok());
    
    ValidationState state;
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

} // TEST_SUITE

TEST_SUITE("Special Validators") {

TEST_CASE("DateValidator") {
    DateValidator validator;
    CHECK(validator.name() == "date");
    
    // Placeholder test - Phase 2 will implement actual date parsing
    ValidationState state;
    auto result = validator.validate(StringInput("2024-01-01"), state);
    CHECK(result.is_ok());
}

TEST_CASE("DatetimeValidator") {
    DatetimeValidator validator;
    CHECK(validator.name() == "datetime");
    
    ValidationState state;
    auto result = validator.validate(StringInput("2024-01-01T12:00:00"), state);
    CHECK(result.is_ok());
}

TEST_CASE("UrlValidator") {
    UrlValidator validator;
    CHECK(validator.name() == "url");
    
    ValidationState state;
    auto result = validator.validate(StringInput("https://example.com"), state);
    CHECK(result.is_ok());
}

TEST_CASE("UuidValidator") {
    UuidValidator validator;
    CHECK(validator.name() == "uuid");
    
    ValidationState state;
    auto result = validator.validate(StringInput("550e8400-e29b-41d4-a716-446655440000"), state);
    CHECK(result.is_ok());
}

TEST_CASE("LiteralValidator") {
    std::vector<std::string> values = {"a", "b", "c"};
    LiteralValidator validator(values);
    CHECK(validator.name() == "literal");
    
    ValidationState state;
    auto result = validator.validate(StringInput("a"), state);
    CHECK(result.is_ok());
}

TEST_CASE("EnumValidator") {
    std::unordered_set<std::string> valid_values = {"red", "green", "blue"};
    EnumValidator validator(valid_values);
    CHECK(validator.name() == "enum");
    
    ValidationState state;
    auto result = validator.validate(StringInput("red"), state);
    CHECK(result.is_ok());
}

} // TEST_SUITE

TEST_SUITE("ValidatorFactory") {

TEST_CASE("Build basic validators") {
    // Test building AnyValidator
    std::unordered_map<std::string, std::string> schema;
    schema["type"] = "any";
    
    auto validator = ValidatorFactory::build(schema, {});
    CHECK(validator->name() == "any");
    
    // Test building IntValidator
    schema["type"] = "int";
    auto int_validator = ValidatorFactory::build(schema, {});
    CHECK(int_validator->name() == "int");
    
    // Test building StringValidator
    schema["type"] = "str";
    auto str_validator = ValidatorFactory::build(schema, {});
    CHECK(str_validator->name() == "str");
    
    // Test building BoolValidator
    schema["type"] = "bool";
    auto bool_validator = ValidatorFactory::build(schema, {});
    CHECK(bool_validator->name() == "bool");
    
    // Test building NoneValidator
    schema["type"] = "none";
    auto none_validator = ValidatorFactory::build(schema, {});
    CHECK(none_validator->name() == "none");
}

TEST_CASE("Build container validators") {
    std::unordered_map<std::string, std::string> schema;
    
    // List
    schema["type"] = "list";
    auto list_validator = ValidatorFactory::build(schema, {});
    CHECK(list_validator->name() == "list");
    
    // Dict
    schema["type"] = "dict";
    auto dict_validator = ValidatorFactory::build(schema, {});
    CHECK(dict_validator->name() == "dict");
    
    // Tuple
    schema["type"] = "tuple";
    auto tuple_validator = ValidatorFactory::build(schema, {});
    CHECK(tuple_validator->name() == "tuple");
}

TEST_CASE("Build complex validators") {
    std::unordered_map<std::string, std::string> schema;
    
    // Nullable
    schema["type"] = "nullable";
    auto nullable_validator = ValidatorFactory::build(schema, {});
    CHECK(nullable_validator->name() == "nullable");
    
    // Union
    schema["type"] = "union";
    auto union_validator = ValidatorFactory::build(schema, {});
    CHECK(union_validator->name() == "union");
    
    // Model
    schema["type"] = "model";
    auto model_validator = ValidatorFactory::build(schema, {});
    CHECK(model_validator->name() == "model");
    
    // TypedDict
    schema["type"] = "typed-dict";
    auto typed_dict_validator = ValidatorFactory::build(schema, {});
    CHECK(typed_dict_validator->name() == "typed-dict");
}

TEST_CASE("Build special validators") {
    std::unordered_map<std::string, std::string> schema;
    
    // Date
    schema["type"] = "date";
    auto date_validator = ValidatorFactory::build(schema, {});
    CHECK(date_validator->name() == "date");
    
    // Datetime
    schema["type"] = "datetime";
    auto datetime_validator = ValidatorFactory::build(schema, {});
    CHECK(datetime_validator->name() == "datetime");
    
    // URL
    schema["type"] = "url";
    auto url_validator = ValidatorFactory::build(schema, {});
    CHECK(url_validator->name() == "url");
    
    // UUID
    schema["type"] = "uuid";
    auto uuid_validator = ValidatorFactory::build(schema, {});
    CHECK(uuid_validator->name() == "uuid");
    
    // Literal
    schema["type"] = "literal";
    auto literal_validator = ValidatorFactory::build(schema, {});
    CHECK(literal_validator->name() == "literal");
    
    // Enum
    schema["type"] = "enum";
    auto enum_validator = ValidatorFactory::build(schema, {});
    CHECK(enum_validator->name() == "enum");
}

TEST_CASE("Unknown validator type throws") {
    std::unordered_map<std::string, std::string> schema;
    schema["type"] = "unknown-type";
    
    CHECK_THROWS_AS(ValidatorFactory::build(schema, {}), SchemaError);
}

} // TEST_SUITE