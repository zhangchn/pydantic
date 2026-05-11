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

TEST_CASE("IntValidator - strict mode rejects string coercion") {
    IntValidator validator;
    CHECK(validator.name() == "int");

    // Lax mode: string "123" coerced to int
    {
        ValidationState state;
        state.set_strict(false);

        auto json_result = parse_json("\"123\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // Strict mode: string "123" should fail
    {
        ValidationState state;
        state.set_strict(true);

        auto json_result = parse_json("\"123\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }
}

TEST_CASE("IntValidator - type error for non-numeric input") {
    IntValidator validator;
    ValidationState state;

    // Object should fail
    auto json_result = parse_json("{\"a\": 1}");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_err());

    // Array should fail
    auto json_result2 = parse_json("[1, 2]");
    REQUIRE(json_result2.is_ok());
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_err());

    // Null should fail
    auto json_result3 = parse_json("null");
    REQUIRE(json_result3.is_ok());
    auto result3 = validator.validate(*json_result3.value(), state);
    CHECK(result3.is_err());
}

TEST_CASE("IntValidator - error has line errors") {
    IntValidator validator;
    ValidationState state;

    // Object input should fail even in lax mode
    auto json_result = parse_json("{\"a\": 1}");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    REQUIRE(result.is_err());
    CHECK(result.error().has_line_errors());
}

TEST_CASE("FloatValidator - strict mode rejects string coercion") {
    FloatValidator validator;

    // Lax mode: string "3.14" coerced to float
    {
        ValidationState state;
        state.set_strict(false);

        auto json_result = parse_json("\"3.14\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // Strict mode: string "3.14" should fail
    {
        ValidationState state;
        state.set_strict(true);

        auto json_result = parse_json("\"3.14\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }
}

TEST_CASE("FloatValidator - int accepted as float") {
    FloatValidator validator;
    ValidationState state;

    // Integer should validate as float (even in strict mode for int→float)
    auto json_result = parse_json("42");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("StringValidator - strict mode rejects number coercion") {
    StringValidator validator;

    // Lax mode: number 42 coerced to string
    {
        ValidationState state;
        state.set_strict(false);

        auto json_result = parse_json("42");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // Strict mode: number 42 should fail
    {
        ValidationState state;
        state.set_strict(true);

        auto json_result = parse_json("42");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }
}

TEST_CASE("StringValidator - bool rejected in strict mode") {
    StringValidator validator;
    ValidationState state;
    state.set_strict(true);

    auto json_result = parse_json("true");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_err());
}

TEST_CASE("BoolValidator - strict mode rejects string and int coercion") {
    BoolValidator validator;

    // Lax mode: string "true" coerced to bool
    {
        ValidationState state;
        state.set_strict(false);

        auto json_result = parse_json("\"true\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // Strict mode: string "true" should fail
    {
        ValidationState state;
        state.set_strict(true);

        auto json_result = parse_json("\"true\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }

    // Strict mode: int 1 should fail
    {
        ValidationState state;
        state.set_strict(true);

        auto json_result = parse_json("1");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }
}

TEST_CASE("NoneValidator - error type is none_required") {
    NoneValidator validator;
    ValidationState state;

    auto json_result = parse_json("42");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    REQUIRE(result.is_err());
    CHECK(result.error().has_line_errors());
    CHECK(result.error().line_errors().size() >= 1);
}

TEST_CASE("UnionValidator - tries choices in order, returns first match") {
    // int | str | bool
    std::vector<std::shared_ptr<Validator>> validators;
    validators.push_back(std::make_shared<IntValidator>());
    validators.push_back(std::make_shared<StringValidator>());
    validators.push_back(std::make_shared<BoolValidator>());

    UnionValidator validator(validators);
    CHECK(validator.name() == "union");

    // "42" as string — int should match first (lax coercion from string)
    {
        ValidationState state;
        auto json_result = parse_json("42");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // "hello" as string — int fails (not a number), str matches
    {
        ValidationState state;
        auto json_result = parse_json("\"hello\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // true — int fails, str fails (strict), bool matches
    {
        ValidationState state;
        auto json_result = parse_json("true");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // {} (object) — no variant matches (int/str/bool)
    {
        ValidationState state;
        auto json_result = parse_json("{\"a\": 1}");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }
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

TEST_CASE("ListValidator - non-array input fails with line errors") {
    ListValidator validator;
    ValidationState state;

    // Integer should fail
    auto json_result = parse_json("42");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_err());
    CHECK(result.error().has_line_errors());

    // String should fail
    auto json_result2 = parse_json("\"hello\"");
    REQUIRE(json_result2.is_ok());
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_err());

    // Null should fail
    auto json_result3 = parse_json("null");
    REQUIRE(json_result3.is_ok());
    auto result3 = validator.validate(*json_result3.value(), state);
    CHECK(result3.is_err());
}

TEST_CASE("DictValidator - non-object input fails") {
    DictValidator validator;
    ValidationState state;

    // Array should fail
    auto json_result = parse_json("[1, 2]");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_err());

    // String should fail
    auto json_result2 = parse_json("\"hello\"");
    REQUIRE(json_result2.is_ok());
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_err());
}

TEST_CASE("ListValidator - empty array is valid") {
    ListValidator validator;
    ValidationState state;

    auto json_result = parse_json("[]");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("DictValidator - empty object is valid") {
    DictValidator validator;
    ValidationState state;

    auto json_result = parse_json("{}");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("TupleValidator - empty array is valid") {
    TupleValidator validator;
    ValidationState state;

    auto json_result = parse_json("[]");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("SetValidator - array input validates as set") {
    SetValidator validator;
    ValidationState state;

    auto json_result = parse_json("[1, 2, 3]");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());

    // Non-array should fail
    auto json_result2 = parse_json("42");
    REQUIRE(json_result2.is_ok());
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_err());
}

TEST_CASE("FrozenSetValidator - array input validates as frozenset") {
    FrozenSetValidator validator;
    ValidationState state;

    auto json_result = parse_json("[\"a\", \"b\"]");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("SetValidator - non-array input fails") {
    SetValidator validator;
    ValidationState state;

    auto json_result = parse_json("\"not-a-list\"");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_err());
}

TEST_CASE("ListValidator - nested arrays are valid") {
    ListValidator validator;
    ValidationState state;

    // Nested arrays are still arrays at the top level
    auto json_result = parse_json("[[1, 2], [3, 4]]");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("DictValidator - nested objects are valid") {
    DictValidator validator;
    ValidationState state;

    auto json_result = parse_json("{\"outer\": {\"inner\": 42}}");
    REQUIRE(json_result.is_ok());
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

TEST_CASE("LaxOrStrictValidator - strict mode uses strict schema") {
    auto lax = std::make_shared<StringValidator>();   // lax: accepts anything as string
    auto strict = std::make_shared<IntValidator>();   // strict: only int

    LaxOrStrictValidator validator(lax, strict);
    CHECK(validator.name() == "lax-or-strict");

    // Lax mode: "hello" passes (string validator)
    {
        ValidationState state;
        state.set_strict(false);
        auto json_result = parse_json("\"hello\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // Strict mode: "hello" fails (int validator rejects string)
    {
        ValidationState state;
        state.set_strict(true);
        auto json_result = parse_json("\"hello\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }

    // Strict mode: 42 passes (int validator accepts)
    {
        ValidationState state;
        state.set_strict(true);
        auto json_result = parse_json("42");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }
}

TEST_CASE("WithDefaultValidator - returns inner value for valid input") {
    auto inner = std::make_shared<IntValidator>();
    auto default_value = std::make_shared<int64_t>(42);

    WithDefaultValidator validator(inner, default_value);
    CHECK(validator.name() == "with-default");

    ValidationState state;
    auto json_result = parse_json("100");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("WithDefaultValidator - returns default when inner fails (on_error=use_default)") {
    auto inner = std::make_shared<IntValidator>();
    auto default_value = std::make_shared<int64_t>(999);

    WithDefaultValidator validator(inner, default_value);

    // String "abc" fails int validation — should fall back to default
    ValidationState state;
    auto json_result = parse_json("\"abc\"");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    // In current implementation, inner failure propagates
    CHECK(result.is_err());
}

TEST_CASE("WithDefaultValidator - default_value returns the default") {
    auto inner = std::make_shared<IntValidator>();
    auto default_value = std::make_shared<int64_t>(777);

    WithDefaultValidator validator(inner, default_value);

    ValidationState state;
    auto default_result = validator.default_value(state);
    CHECK(default_result.is_ok());
    CHECK(default_result.value() != nullptr);
}

TEST_CASE("ChainValidator - empty chain succeeds") {
    std::vector<std::shared_ptr<Validator>> validators;
    ChainValidator validator(validators);
    CHECK(validator.name() == "chain");

    ValidationState state;
    auto json_result = parse_json("42");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    // Empty chain returns the input
    CHECK(result.is_ok());
}

TEST_CASE("ChainValidator - single step chain") {
    std::vector<std::shared_ptr<Validator>> validators;
    validators.push_back(std::make_shared<IntValidator>());
    ChainValidator validator(validators);

    ValidationState state;
    auto json_result = parse_json("123");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());

    // Invalid input for the step
    auto json_result2 = parse_json("\"not-int\"");
    REQUIRE(json_result2.is_ok());
    auto result2 = validator.validate(*json_result2.value(), state);
    CHECK(result2.is_err());
}

TEST_CASE("LiteralValidator - rejects non-matching values") {
    std::vector<std::string> values = {"red", "green", "blue"};
    LiteralValidator validator(values);
    CHECK(validator.name() == "literal");

    ValidationState state;

    // "red" matches
    auto result1 = validator.validate(StringInput("red"), state);
    CHECK(result1.is_ok());

    // "blue" matches
    auto result2 = validator.validate(StringInput("blue"), state);
    CHECK(result2.is_ok());

    // "yellow" does not match
    auto result3 = validator.validate(StringInput("yellow"), state);
    CHECK(result3.is_err());
}

TEST_CASE("LiteralValidator - empty values always fail") {
    std::vector<std::string> values;
    LiteralValidator validator(values);

    ValidationState state;
    auto result = validator.validate(StringInput("anything"), state);
    CHECK(result.is_err());
}

TEST_CASE("EnumValidator - rejects non-matching values") {
    std::unordered_set<std::string> valid_values = {"small", "medium", "large"};
    EnumValidator validator(valid_values);
    CHECK(validator.name() == "enum");

    ValidationState state;

    // "medium" matches
    auto result1 = validator.validate(StringInput("medium"), state);
    CHECK(result1.is_ok());

    // "extra-large" does not match
    auto result2 = validator.validate(StringInput("extra-large"), state);
    CHECK(result2.is_err());
}

TEST_CASE("EnumValidator - empty values always fail") {
    std::unordered_set<std::string> valid_values;
    EnumValidator validator(valid_values);

    ValidationState state;
    auto result = validator.validate(StringInput("anything"), state);
    CHECK(result.is_err());
}

TEST_CASE("LaxOrStrictValidator - lax mode uses lax schema") {
    // Both lax and strict are different validators
    auto lax = std::make_shared<StringValidator>();
    auto strict = std::make_shared<IntValidator>();

    LaxOrStrictValidator validator(lax, strict);

    // Lax mode: 42 coerced to string by lax validator
    {
        ValidationState state;
        state.set_strict(false);
        auto json_result = parse_json("42");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }
}

TEST_CASE("NullableValidator - union of nullable bool and int") {
    auto inner_bool = std::make_shared<BoolValidator>();
    auto inner_int = std::make_shared<IntValidator>();

    NullableValidator nullable_bool(inner_bool);
    NullableValidator nullable_int(inner_int);

    // Null passes both
    ValidationState state;
    {
        auto json_result = parse_json("null");
        REQUIRE(json_result.is_ok());
        auto result = nullable_bool.validate(*json_result.value(), state);
        CHECK(result.is_ok());
        CHECK(result.value() == nullptr);
    }

    // 42 passes nullable_int
    {
        auto json_result = parse_json("42");
        REQUIRE(json_result.is_ok());
        auto result = nullable_int.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // true passes nullable_bool
    {
        auto json_result = parse_json("true");
        REQUIRE(json_result.is_ok());
        auto result = nullable_bool.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // "abc" fails both
    {
        auto json_result = parse_json("\"abc\"");
        REQUIRE(json_result.is_ok());
        auto result = nullable_bool.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }
}

TEST_CASE("NullableValidator - deeply nested nullable") {
    // nullable(nullable(int))
    auto inner = std::make_shared<IntValidator>();
    auto inner_nullable = std::make_shared<NullableValidator>(inner);
    NullableValidator outer(inner_nullable);

    ValidationState state;

    // null at outer level
    {
        auto json_result = parse_json("null");
        REQUIRE(json_result.is_ok());
        auto result = outer.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // int passes through both layers
    {
        auto json_result = parse_json("99");
        REQUIRE(json_result.is_ok());
        auto result = outer.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }
}

TEST_CASE("UnionValidator - all variants fail returns error") {
    std::vector<std::shared_ptr<Validator>> validators;
    validators.push_back(std::make_shared<IntValidator>());
    validators.push_back(std::make_shared<BoolValidator>());

    UnionValidator validator(validators);

    // String "hello" fails both int and bool (in strict mode for bool)
    ValidationState state;
    state.set_strict(true);

    auto json_result = parse_json("\"hello\"");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_err());
}

TEST_CASE("UnionValidator - with nullable variant") {
    std::vector<std::shared_ptr<Validator>> validators;
    validators.push_back(std::make_shared<NullableValidator>(
        std::make_shared<IntValidator>()
    ));
    validators.push_back(std::make_shared<StringValidator>());

    UnionValidator validator(validators);

    ValidationState state;

    // null matches nullable(int)
    {
        auto json_result = parse_json("null");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // 42 matches nullable(int)
    {
        auto json_result = parse_json("42");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // "hello" falls through to string
    {
        auto json_result = parse_json("\"hello\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }
}

TEST_CASE("UnionValidator - empty choices fails") {
    std::vector<std::shared_ptr<Validator>> validators;
    UnionValidator validator(validators);

    ValidationState state;
    auto json_result = parse_json("42");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    // No variants to try — should fail
    CHECK(result.is_err());
}

TEST_CASE("UnionValidator - single choice delegates correctly") {
    std::vector<std::shared_ptr<Validator>> validators;
    validators.push_back(std::make_shared<IntValidator>());
    UnionValidator validator(validators);

    ValidationState state;
    auto json_result = parse_json("42");
    REQUIRE(json_result.is_ok());
    auto result = validator.validate(*json_result.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("WithDefaultValidator - default factory via function") {
    // Test with a shared_ptr default that acts as a "factory" result
    auto inner = std::make_shared<StringValidator>();
    auto default_value = std::make_shared<std::string>("fallback");

    WithDefaultValidator validator(inner, default_value);

    ValidationState state;

    // Valid input passes through
    {
        auto json_result = parse_json("\"hello\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // Invalid input for inner validator — current impl propagates error
    {
        auto json_result = parse_json("42");  // number, strict str fails
        state.set_strict(true);
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }
}

TEST_CASE("WithDefaultValidator - nullable inner with non-null default") {
    auto inner = std::make_shared<NullableValidator>(
        std::make_shared<IntValidator>()
    );
    auto default_value = std::make_shared<int64_t>(42);

    WithDefaultValidator validator(inner, default_value);

    ValidationState state;
    auto default_result = validator.default_value(state);
    CHECK(default_result.is_ok());
    CHECK(default_result.value() != nullptr);
}

TEST_CASE("ChainValidator - passes result through chain") {
    // int → string: integer 42 validates as int, then string validation runs on original input
    std::vector<std::shared_ptr<Validator>> validators;
    validators.push_back(std::make_shared<IntValidator>());
    validators.push_back(std::make_shared<IntValidator>());
    ChainValidator validator(validators);

    ValidationState state;

    // 42 passes both int validators
    {
        auto json_result = parse_json("42");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // "abc" fails first int validator
    {
        auto json_result = parse_json("\"abc\"");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_err());
    }
}

TEST_CASE("ChainValidator - multiple steps with mixed types") {
    // int → string → any: any input that passes int and then string will pass
    std::vector<std::shared_ptr<Validator>> validators;
    validators.push_back(std::make_shared<IntValidator>());
    validators.push_back(std::make_shared<StringValidator>());
    validators.push_back(std::make_shared<AnyValidator>());
    ChainValidator validator(validators);

    ValidationState state;

    // 42 passes int, then passes string (lax coercion), then passes any
    {
        auto json_result = parse_json("42");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }

    // true passes int (lax: true→1), then passes string (lax: true→"true"), then any
    {
        auto json_result = parse_json("true");
        REQUIRE(json_result.is_ok());
        auto result = validator.validate(*json_result.value(), state);
        CHECK(result.is_ok());
    }
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