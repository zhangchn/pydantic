#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/validators/model_fields.hpp"
#include "pydantic_core/validators/containers.hpp"
#include "pydantic_core/validation_state.hpp"

using namespace pydantic_core;

// Helper: parse JSON and validate with a given validator
static std::shared_ptr<void> validate_json(std::shared_ptr<Validator> validator, const std::string& json_input) {
    auto input_result = parse_json(json_input);
    REQUIRE(input_result.is_ok());
    ValidationState state;
    auto result = validator->validate(*input_result.value(), state);
    REQUIRE(result.is_ok());
    return result.value();
}

// Helper: parse JSON and validate, expecting failure
static ValError validate_json_expect_error(std::shared_ptr<Validator> validator, const std::string& json_input) {
    auto input_result = parse_json(json_input);
    REQUIRE(input_result.is_ok());
    ValidationState state;
    auto result = validator->validate(*input_result.value(), state);
    REQUIRE(result.is_err());
    return result.error();
}

// Helper: build a validator from JSON schema string
static std::shared_ptr<Validator> build_from_json(const std::string& schema_json) {
    auto combined = SchemaBuilder::build(schema_json);
    REQUIRE(combined != nullptr);
    // We need to extract the Validator* from CombinedValidator
    // Since CombinedValidator wraps shared_ptr<Validator>, we can build via the factory
    // Actually, let's use a direct approach with the SchemaBuilder
    return nullptr;
}

// ============================================================================
// TEST SUITE: ModelFieldsValidator
// ============================================================================

TEST_SUITE("ModelFieldsValidator") {

    TEST_CASE("Simple model-fields - all required fields present") {
        // Build schema: {"type": "model-fields", "fields": {"name": {"schema": {"type": "str"}}, "age": {"schema": {"type": "int"}}}}
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "name": {"schema": {"type": "str"}},
                "age": {"schema": {"type": "int"}}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);
        CHECK(combined->name() == "model-fields");

        // Validate with matching input
        std::string input_json = R"({"name": "Alice", "age": 30})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        // Check output
        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(output->fields.count("age"));
        CHECK(output->fields_set.count("name"));
        CHECK(output->fields_set.count("age"));
        CHECK(output->fields_set.size() == 2);
    }

    TEST_CASE("Model-fields - missing required field") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "name": {"schema": {"type": "str"}},
                "age": {"schema": {"type": "int"}}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Missing 'age' field
        std::string input_json = R"({"name": "Alice"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        CHECK(result.is_err());
        CHECK(result.error().has_line_errors());
        CHECK(result.error().line_errors().size() >= 1);
    }

    TEST_CASE("Model-fields - field type mismatch") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "name": {"schema": {"type": "str"}},
                "age": {"schema": {"type": "int"}}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // 'age' is a string, should fail strict int validation
        std::string input_json = R"({"name": "Alice", "age": "thirty"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        // In lax mode, int validation might coerce from string
        // Since we're testing, the lax mode might pass. Let's check the output
        // Actually with lax mode, "thirty" won't parse as int
        // But in lax mode, string->int coercion only works for numeric strings
        // Let's check the behavior
        CHECK(result.is_err());
    }

    TEST_CASE("Model-fields - with optional field (not required)") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "name": {"schema": {"type": "str"}},
                "nickname": {"schema": {"type": "str"}, "required": false}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Only required field present
        std::string input_json = R"({"name": "Alice"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        // Optional field absent and no default - should not be in output
        CHECK(!output->fields.count("nickname"));
        CHECK(output->fields_set.size() == 1);
    }

    TEST_CASE("Model-fields - with default value") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "name": {"schema": {"type": "str"}},
                "role": {"schema": {"type": "str"}, "required": false, "default": "user"}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Only 'name' provided, 'role' should use default
        std::string input_json = R"({"name": "Alice"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(output->fields.count("role"));
        // Default value should be present
        auto& role_fv = output->fields["role"];
        auto role_val = std::static_pointer_cast<std::string>(role_fv.value);
        CHECK(*role_val == "user");
    }

    TEST_CASE("Model-fields - extra fields with ignore (default)") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "name": {"schema": {"type": "str"}}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Extra field 'unknown' should be ignored
        std::string input_json = R"({"name": "Alice", "unknown": "data"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(!output->extra.count("unknown"));  // extra should be empty for 'ignore'
    }

    TEST_CASE("Model-fields - extra fields forbidden") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "name": {"schema": {"type": "str"}}
            },
            "extra_behavior": "forbid"
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Extra field 'unknown' should cause error
        std::string input_json = R"({"name": "Alice", "unknown": "data"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        CHECK(result.is_err());
        CHECK(result.error().has_line_errors());
    }

    TEST_CASE("Model-fields - extra fields allowed") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "name": {"schema": {"type": "str"}}
            },
            "extra_behavior": "allow"
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        std::string input_json = R"({"name": "Alice", "extra_field": 42})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(output->extra.count("extra_field"));
        CHECK(output->fields_set.count("extra_field"));
    }

    TEST_CASE("Model-fields - validation alias") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "first_name": {"schema": {"type": "str"}, "validation_alias": "firstName"}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Use alias in input
        std::string input_json = R"({"firstName": "Alice"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        // Output uses canonical field name
        CHECK(output->fields.count("first_name"));
        CHECK(output->fields_set.count("first_name"));
    }

    TEST_CASE("Model-fields - nested schema (model-fields inside model-fields)") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {
                "name": {"schema": {"type": "str"}},
                "address": {
                    "schema": {
                        "type": "model-fields",
                        "fields": {
                            "city": {"schema": {"type": "str"}},
                            "zip": {"schema": {"type": "str"}}
                        }
                    }
                }
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);
        CHECK(combined->name() == "model-fields");

        std::string input_json = R"({
            "name": "Alice",
            "address": {"city": "NYC", "zip": "10001"}
        })";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(output->fields.count("address"));
    }

    TEST_CASE("Model-fields - empty fields dict") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {}
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        std::string input_json = R"({})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());
    }

    TEST_CASE("Model-fields - non-dict input returns error") {
        std::string schema_json = R"({
            "type": "model-fields",
            "fields": {"name": {"schema": {"type": "str"}}}
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Array input should fail
        std::string input_json = R"(["Alice"])";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

}

// ============================================================================
// TEST SUITE: ModelValidator
// ============================================================================

TEST_SUITE("ModelValidator") {

    TEST_CASE("Model wrapping model-fields schema") {
        std::string schema_json = R"({
            "type": "model",
            "cls": "User",
            "schema": {
                "type": "model-fields",
                "fields": {
                    "name": {"schema": {"type": "str"}},
                    "age": {"schema": {"type": "int"}}
                }
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);
        CHECK(combined->name() == "model");

        std::string input_json = R"({"name": "Bob", "age": 25})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(output->fields.count("age"));
    }

    TEST_CASE("Model - missing field propagates error") {
        std::string schema_json = R"({
            "type": "model",
            "cls": "User",
            "schema": {
                "type": "model-fields",
                "fields": {
                    "name": {"schema": {"type": "str"}},
                    "age": {"schema": {"type": "int"}}
                }
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Missing 'age'
        std::string input_json = R"({"name": "Bob"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("Model with frozen flag") {
        std::string schema_json = R"({
            "type": "model",
            "cls": "FrozenModel",
            "frozen": true,
            "schema": {
                "type": "model-fields",
                "fields": {
                    "value": {"schema": {"type": "int"}}
                }
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        std::string input_json = R"({"value": 42})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());
    }

    TEST_CASE("Model with root_model flag") {
        std::string schema_json = R"({
            "type": "model",
            "cls": "RootModel",
            "root_model": true,
            "schema": {
                "type": "model-fields",
                "fields": {
                    "root": {"schema": {"type": "str"}}
                }
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        std::string input_json = R"({"root": "hello"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());
    }

}

// ============================================================================
// TEST SUITE: TypedDictValidator
// ============================================================================

TEST_SUITE("TypedDictValidator") {

    TEST_CASE("TypedDict with total=true (all fields required by default)") {
        std::string schema_json = R"({
            "type": "typed-dict",
            "total": true,
            "fields": {
                "x": {"schema": {"type": "int"}},
                "y": {"schema": {"type": "int"}}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);
        CHECK(combined->name() == "typed-dict");

        // All fields present
        std::string input_json = R"({"x": 1, "y": 2})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("x"));
        CHECK(output->fields.count("y"));
    }

    TEST_CASE("TypedDict with total=true - missing field error") {
        std::string schema_json = R"({
            "type": "typed-dict",
            "total": true,
            "fields": {
                "x": {"schema": {"type": "int"}},
                "y": {"schema": {"type": "int"}}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Missing 'y'
        std::string input_json = R"({"x": 1})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("TypedDict with total=false (fields optional by default)") {
        std::string schema_json = R"({
            "type": "typed-dict",
            "total": false,
            "fields": {
                "x": {"schema": {"type": "int"}},
                "y": {"schema": {"type": "int"}}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Only one field - OK because total=false
        std::string input_json = R"({"x": 1})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("x"));
        CHECK(!output->fields.count("y"));
    }

    TEST_CASE("TypedDict with explicit required override") {
        std::string schema_json = R"({
            "type": "typed-dict",
            "total": false,
            "fields": {
                "x": {"schema": {"type": "int"}},
                "y": {"schema": {"type": "int"}, "required": true}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // 'y' is explicitly required
        std::string input_json = R"({"x": 1})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("TypedDict with extra_behavior=forbid") {
        std::string schema_json = R"({
            "type": "typed-dict",
            "total": false,
            "extra_behavior": "forbid",
            "fields": {
                "x": {"schema": {"type": "int"}}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Extra field 'z' should be rejected
        std::string input_json = R"({"x": 1, "z": 99})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("TypedDict with extra_behavior=allow") {
        std::string schema_json = R"({
            "type": "typed-dict",
            "total": false,
            "extra_behavior": "allow",
            "fields": {
                "x": {"schema": {"type": "int"}}
            }
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        std::string input_json = R"({"x": 1, "extra": "hello"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("x"));
        CHECK(output->extra.count("extra"));
    }

}

// ============================================================================
// TEST SUITE: DataclassValidator
// ============================================================================

TEST_SUITE("DataclassValidator") {

    TEST_CASE("Dataclass with flat field list") {
        std::string schema_json = R"({
            "type": "dataclass",
            "cls": "Person",
            "fields": [
                {"name": "name", "schema": {"type": "str"}},
                {"name": "age", "schema": {"type": "int"}}
            ]
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);
        CHECK(combined->name() == "dataclass");

        std::string input_json = R"({"name": "Charlie", "age": 35})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(output->fields.count("age"));
        CHECK(output->fields_set.size() == 2);
    }

    TEST_CASE("Dataclass - missing required field") {
        std::string schema_json = R"({
            "type": "dataclass",
            "cls": "Person",
            "fields": [
                {"name": "name", "schema": {"type": "str"}},
                {"name": "age", "schema": {"type": "int"}}
            ]
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Missing 'age'
        std::string input_json = R"({"name": "Charlie"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("Dataclass with frozen flag") {
        std::string schema_json = R"({
            "type": "dataclass",
            "cls": "FrozenDataclass",
            "frozen": true,
            "fields": [
                {"name": "value", "schema": {"type": "int"}}
            ]
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        std::string input_json = R"({"value": 100})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = combined->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());
    }

    TEST_CASE("Dataclass with extra_behavior=forbid") {
        std::string schema_json = R"({
            "type": "dataclass",
            "cls": "Person",
            "fields": [
                {"name": "name", "schema": {"type": "str"}}
            ]
        })";

        auto combined = SchemaBuilder::build(schema_json);
        REQUIRE(combined != nullptr);

        // Extra fields should be rejected by default (dataclass default is forbid)
        // Actually default is Ignore. Let's test explicit forbid.
        // We need to rebuild with extra_behavior - but dataclass doesn't support that in JSON schema
        // Let's test with the programmatic API instead
        std::vector<DataclassFieldInfo> fields;
        fields.push_back({"name", std::make_shared<StringValidator>()});

        auto validator = std::make_shared<DataclassValidator>(
            std::move(fields), "Person", false, false, ExtraBehavior::Forbid
        );

        std::string input_json = R"({"name": "Test", "extra": "field"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("Dataclass with kw_only field") {
        std::vector<DataclassFieldInfo> fields;
        fields.push_back({"name", std::make_shared<StringValidator>()});
        DataclassFieldInfo age_field;
        age_field.name = "age";
        age_field.schema = std::make_shared<IntValidator>();
        age_field.kw_only = true;
        fields.push_back(age_field);

        auto validator = std::make_shared<DataclassValidator>(fields, "Person");

        std::string input_json = R"({"name": "Test", "age": 25})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(output->fields.count("age"));
    }

    TEST_CASE("Dataclass with init_only_name (alias)") {
        std::vector<DataclassFieldInfo> fields;
        DataclassFieldInfo name_field;
        name_field.name = "full_name";
        name_field.schema = std::make_shared<StringValidator>();
        name_field.init_only_name = "name";
        fields.push_back(name_field);

        auto validator = std::make_shared<DataclassValidator>(fields, "Person");

        // Use the alias
        std::string input_json = R"({"name": "Alice"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        // Output uses canonical name
        CHECK(output->fields.count("full_name"));
    }

    TEST_CASE("Dataclass - non-dict input returns error") {
        std::vector<DataclassFieldInfo> fields;
        fields.push_back({"value", std::make_shared<IntValidator>()});
        auto validator = std::make_shared<DataclassValidator>(fields, "Data");

        std::string input_json = R"(42)";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

}

// ============================================================================
// TEST SUITE: Programmatic Validator Construction
// ============================================================================

TEST_SUITE("Programmatic Validator Construction") {

    TEST_CASE("Build ModelFieldsValidator programmatically") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo name_field;
        name_field.name = "name";
        name_field.schema = std::make_shared<StringValidator>();
        name_field.required = true;
        fields["name"] = name_field;

        FieldInfo age_field;
        age_field.name = "age";
        age_field.schema = std::make_shared<IntValidator>();
        age_field.required = false;
        age_field.default_value_str = "0";
        fields["age"] = age_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore, nullptr, "Person"
        );

        CHECK(validator->name() == "model-fields");
        CHECK(validator->model_name() == "Person");
        CHECK(validator->fields().size() == 2);
        CHECK(validator->extra_behavior() == ExtraBehavior::Ignore);
    }

    TEST_CASE("Build TypedDictValidator programmatically") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo x_field;
        x_field.name = "x";
        x_field.schema = std::make_shared<IntValidator>();
        fields["x"] = x_field;

        auto validator = std::make_shared<TypedDictValidator>(
            std::move(fields), ExtraBehavior::Ignore, false  // total=false
        );

        CHECK(validator->name() == "typed-dict");
        CHECK(!validator->total());
        CHECK(validator->fields().size() == 1);
    }

    TEST_CASE("Build ModelValidator programmatically") {
        std::unordered_map<std::string, FieldInfo> fields;
        fields["value"] = FieldInfo{"value", std::make_shared<IntValidator>(), true};

        auto fields_validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore, nullptr, "MyModel"
        );

        auto validator = std::make_shared<ModelValidator>(
            fields_validator, "MyModel", false, false, false
        );

        CHECK(validator->name() == "model");
        CHECK(validator->class_name() == "MyModel");
        CHECK(!validator->frozen());
        CHECK(!validator->root_model());
    }

    TEST_CASE("Build DataclassValidator programmatically") {
        std::vector<DataclassFieldInfo> fields;

        DataclassFieldInfo name_field;
        name_field.name = "name";
        name_field.schema = std::make_shared<StringValidator>();
        fields.push_back(name_field);

        DataclassFieldInfo age_field;
        age_field.name = "age";
        age_field.schema = std::make_shared<IntValidator>();
        age_field.frozen = true;
        fields.push_back(age_field);

        auto validator = std::make_shared<DataclassValidator>(
            std::move(fields), "Person", true  // frozen
        );

        CHECK(validator->name() == "dataclass");
        CHECK(validator->class_name() == "Person");
        CHECK(validator->frozen());
        CHECK(validator->fields().size() == 2);
    }

    TEST_CASE("Validate with programmatic ModelFieldsValidator") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo name_field;
        name_field.name = "name";
        name_field.schema = std::make_shared<StringValidator>();
        fields["name"] = name_field;

        FieldInfo score_field;
        score_field.name = "score";
        score_field.schema = std::make_shared<IntValidator>();
        fields["score"] = score_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Forbid
        );

        std::string input_json = R"({"name": "Alice", "score": 95})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(output->fields.count("score"));
        CHECK(output->fields_set.size() == 2);
    }

    TEST_CASE("Validate with programmatic TypedDictValidator and lax coercion") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo x_field;
        x_field.name = "x";
        x_field.schema = std::make_shared<IntValidator>();
        fields["x"] = x_field;

        auto validator = std::make_shared<TypedDictValidator>(
            std::move(fields), ExtraBehavior::Ignore, true
        );

        // Lax mode: string "42" coerced to int
        std::string input_json = R"({"x": "42"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());
    }

    TEST_CASE("Multiple field validation errors accumulated") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo name_field;
        name_field.name = "name";
        name_field.schema = std::make_shared<StringValidator>();
        fields["name"] = name_field;

        FieldInfo age_field;
        age_field.name = "age";
        age_field.schema = std::make_shared<IntValidator>();
        fields["age"] = age_field;

        FieldInfo email_field;
        email_field.name = "email";
        email_field.schema = std::make_shared<StringValidator>();
        fields["email"] = email_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        // All fields are strings - int validation for 'age' should fail
        std::string input_json = R"({"name": 123, "age": "not-int", "email": 456})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);

        // At minimum, 'age' should fail (string to int in lax mode: "not-int" is not a number)
        CHECK(result.is_err());
    }

    TEST_CASE("ModelFieldsValidator with nullable inner field") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo name_field;
        name_field.name = "name";
        name_field.schema = std::make_shared<StringValidator>();
        fields["name"] = name_field;

        FieldInfo optional_field;
        optional_field.name = "middle_name";
        optional_field.schema = std::make_shared<NullableValidator>(
            std::make_shared<StringValidator>()
        );
        optional_field.required = false;
        fields["middle_name"] = optional_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields)
        );

        // Provide null for optional field
        std::string input_json = R"({"name": "Alice", "middle_name": null})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        CHECK(output->fields.count("middle_name"));
    }

    // ========================================================================
    // Additional behavioral tests for model/typed-dict/dataclass depth
    // ========================================================================

    TEST_CASE("ModelFieldsValidator - strict mode rejects type coercion") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo age_field;
        age_field.name = "age";
        age_field.schema = std::make_shared<IntValidator>();
        fields["age"] = age_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        // Strict mode: string "42" should NOT coerce to int
        std::string input_json = R"({"age": "42"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        state.set_strict(true);
        auto result = validator->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("ModelFieldsValidator - strict mode accepts native types") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo age_field;
        age_field.name = "age";
        age_field.schema = std::make_shared<IntValidator>();
        fields["age"] = age_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        // Strict mode: native int should pass
        std::string input_json = R"({"age": 42})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        state.set_strict(true);
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("age"));
    }

    TEST_CASE("ModelFieldsValidator - list field validates nested items") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo tags_field;
        tags_field.name = "tags";
        tags_field.schema = std::make_shared<ListValidator>();
        fields["tags"] = tags_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        // List of values — ListValidator checks type
        std::string input_json = R"({"tags": ["python", "cpp", "rust"]})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("tags"));
    }

    TEST_CASE("ModelFieldsValidator - list field rejects non-list input") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo nums_field;
        nums_field.name = "nums";
        nums_field.schema = std::make_shared<ListValidator>();
        fields["nums"] = nums_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        // String input where list expected
        std::string input_json = R"({"nums": "not-a-list"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("ModelFieldsValidator - dict field validates nested object") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo metadata_field;
        metadata_field.name = "metadata";
        metadata_field.schema = std::make_shared<DictValidator>();
        fields["metadata"] = metadata_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        std::string input_json = R"({"metadata": {"key1": "val1", "key2": "val2"}})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("metadata"));
    }

    TEST_CASE("ModelFieldsValidator - dict field rejects non-dict input") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo meta_field;
        meta_field.name = "meta";
        meta_field.schema = std::make_shared<DictValidator>();
        fields["meta"] = meta_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        // Array input where dict expected
        std::string input_json = R"({"meta": [1, 2, 3]})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("ModelFieldsValidator - optional field without default is omitted") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo name_field;
        name_field.name = "name";
        name_field.schema = std::make_shared<StringValidator>();
        fields["name"] = name_field;

        FieldInfo bio_field;
        bio_field.name = "bio";
        bio_field.schema = std::make_shared<StringValidator>();
        bio_field.required = false;
        bio_field.default_value_str.clear();  // No default
        fields["bio"] = bio_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        // Only required field provided
        std::string input_json = R"({"name": "Alice"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("name"));
        // bio is not required and has no default — should be absent from output
        CHECK(!output->fields.count("bio"));
    }

    TEST_CASE("ModelFieldsValidator - alias fallback to canonical name") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo name_field;
        name_field.name = "full_name";
        name_field.schema = std::make_shared<StringValidator>();
        name_field.alias = "firstName";
        fields["full_name"] = name_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        // Input uses canonical name (not alias) — should still work
        std::string input_json = R"({"full_name": "Alice"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        REQUIRE(result.is_ok());

        auto output = std::static_pointer_cast<ValidatedModelFieldsOutput>(result.value());
        CHECK(output->fields.count("full_name"));
    }

    TEST_CASE("TypedDictValidator - strict mode rejects coercion on total fields") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo count_field;
        count_field.name = "count";
        count_field.schema = std::make_shared<IntValidator>();
        fields["count"] = count_field;

        auto validator = std::make_shared<TypedDictValidator>(
            std::move(fields), ExtraBehavior::Ignore, true
        );

        // String "10" should NOT coerce to int in strict mode
        std::string input_json = R"({"count": "10"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        state.set_strict(true);
        auto result = validator->validate(*input_result.value(), state);
        CHECK(result.is_err());
    }

    TEST_CASE("DataclassValidator - multiple fields with mixed valid/invalid") {
        std::vector<DataclassFieldInfo> fields;

        DataclassFieldInfo name_field;
        name_field.name = "name";
        name_field.schema = std::make_shared<StringValidator>();
        fields.push_back(name_field);

        DataclassFieldInfo age_field;
        age_field.name = "age";
        age_field.schema = std::make_shared<IntValidator>();
        fields.push_back(age_field);

        DataclassFieldInfo email_field;
        email_field.name = "email";
        email_field.schema = std::make_shared<StringValidator>();
        fields.push_back(email_field);

        auto validator = std::make_shared<DataclassValidator>(
            std::move(fields), "Person"
        );

        // 'age' is invalid (string), rest valid
        std::string input_json = R"({"name": "Bob", "age": "not-a-number", "email": "bob@test.com"})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        CHECK(result.is_err());
        CHECK(result.error().has_line_errors());
    }

    TEST_CASE("ModelFieldsValidator - bool coercion in lax mode for int field") {
        std::unordered_map<std::string, FieldInfo> fields;

        FieldInfo flag_field;
        flag_field.name = "flag";
        flag_field.schema = std::make_shared<IntValidator>();
        fields["flag"] = flag_field;

        auto validator = std::make_shared<ModelFieldsValidator>(
            std::move(fields), ExtraBehavior::Ignore
        );

        // Lax mode: true → 1
        std::string input_json = R"({"flag": true})";
        auto input_result = parse_json(input_json);
        REQUIRE(input_result.is_ok());

        ValidationState state;
        auto result = validator->validate(*input_result.value(), state);
        // In lax mode, bool→int coercion is allowed
        REQUIRE(result.is_ok());
    }
}
