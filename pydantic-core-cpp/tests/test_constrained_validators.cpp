#include <doctest/doctest.h>
#include "pydantic_core/validators/basic.hpp"
#include "pydantic_core/validators/containers.hpp"
#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/validation_state.hpp"

using namespace pydantic_core;

TEST_SUITE("Constrained Validators") {

// ========================================================================
// ConstrainedIntValidator tests
// ========================================================================
TEST_CASE("ConstrainedIntValidator - gt constraint") {
    ConstrainedIntValidator validator;
    validator.gt = 10;
    
    ValidationState state;
    
    auto r1 = parse_json("15");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("10");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
    
    auto r3 = parse_json("5");
    REQUIRE(r3.is_ok());
    CHECK(validator.validate(*r3.value(), state).is_err());
}

TEST_CASE("ConstrainedIntValidator - ge constraint") {
    ConstrainedIntValidator validator;
    validator.ge = 10;
    
    ValidationState state;
    
    auto r1 = parse_json("10");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("15");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_ok());
    
    auto r3 = parse_json("9");
    REQUIRE(r3.is_ok());
    CHECK(validator.validate(*r3.value(), state).is_err());
}

TEST_CASE("ConstrainedIntValidator - lt constraint") {
    ConstrainedIntValidator validator;
    validator.lt = 100;
    
    ValidationState state;
    
    auto r1 = parse_json("99");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("100");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
}

TEST_CASE("ConstrainedIntValidator - le constraint") {
    ConstrainedIntValidator validator;
    validator.le = 100;
    
    ValidationState state;
    
    auto r1 = parse_json("100");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("99");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_ok());
    
    auto r3 = parse_json("101");
    REQUIRE(r3.is_ok());
    CHECK(validator.validate(*r3.value(), state).is_err());
}

TEST_CASE("ConstrainedIntValidator - multiple_of constraint") {
    ConstrainedIntValidator validator;
    validator.multiple_of = 5;
    
    ValidationState state;
    
    auto r1 = parse_json("10");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("15");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_ok());
    
    auto r3 = parse_json("7");
    REQUIRE(r3.is_ok());
    CHECK(validator.validate(*r3.value(), state).is_err());
}

TEST_CASE("ConstrainedIntValidator - combined constraints (gt and lt)") {
    ConstrainedIntValidator validator;
    validator.gt = 0;
    validator.lt = 100;
    
    ValidationState state;
    
    auto r1 = parse_json("50");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("0");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
    
    auto r3 = parse_json("100");
    REQUIRE(r3.is_ok());
    CHECK(validator.validate(*r3.value(), state).is_err());
}

// ========================================================================
// ConstrainedFloatValidator tests
// ========================================================================
TEST_CASE("ConstrainedFloatValidator - gt constraint") {
    ConstrainedFloatValidator validator;
    validator.gt = 0.0;
    
    ValidationState state;
    
    auto r1 = parse_json("1.5");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("0.0");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
    
    auto r3 = parse_json("-1.0");
    REQUIRE(r3.is_ok());
    CHECK(validator.validate(*r3.value(), state).is_err());
}

TEST_CASE("ConstrainedFloatValidator - le constraint") {
    ConstrainedFloatValidator validator;
    validator.le = 10.0;
    
    ValidationState state;
    
    auto r1 = parse_json("10.0");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("9.99");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_ok());
    
    auto r3 = parse_json("10.01");
    REQUIRE(r3.is_ok());
    CHECK(validator.validate(*r3.value(), state).is_err());
}

TEST_CASE("ConstrainedFloatValidator - multiple_of constraint") {
    ConstrainedFloatValidator validator;
    validator.multiple_of = 0.5;
    
    ValidationState state;
    
    auto r1 = parse_json("1.5");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("2.0");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_ok());
    
    auto r3 = parse_json("1.7");
    REQUIRE(r3.is_ok());
    CHECK(validator.validate(*r3.value(), state).is_err());
}

// ========================================================================
// StrConstrainedValidator tests
// ========================================================================
TEST_CASE("StrConstrainedValidator - min_length constraint") {
    StrConstrainedValidator validator;
    validator.min_length = 3;
    
    ValidationState state;
    
    auto r1 = parse_json("\"hello\"");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("\"ab\"");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
    
    auto r3 = parse_json("\"abc\"");
    REQUIRE(r3.is_ok());
    CHECK(validator.validate(*r3.value(), state).is_ok());
}

TEST_CASE("StrConstrainedValidator - max_length constraint") {
    StrConstrainedValidator validator;
    validator.max_length = 5;
    
    ValidationState state;
    
    auto r1 = parse_json("\"hello\"");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("\"toolong\"");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
}

TEST_CASE("StrConstrainedValidator - strip_whitespace") {
    StrConstrainedValidator validator;
    validator.strip_whitespace = true;
    
    ValidationState state;
    
    auto r = parse_json("\"  hello  \"");
    REQUIRE(r.is_ok());
    auto result = validator.validate(*r.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("StrConstrainedValidator - to_lower") {
    StrConstrainedValidator validator;
    validator.to_lower = true;
    
    ValidationState state;
    
    auto r = parse_json("\"HELLO\"");
    REQUIRE(r.is_ok());
    auto result = validator.validate(*r.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("StrConstrainedValidator - pattern matching") {
    StrConstrainedValidator validator;
    validator.pattern = "^[a-z]+$";
    
    ValidationState state;
    
    auto r1 = parse_json("\"hello\"");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("\"hello123\"");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
}

// ========================================================================
// Container validators with constraints
// ========================================================================
TEST_CASE("ListValidator - min_length constraint") {
    ListValidator validator;
    validator.min_length = 2;
    
    ValidationState state;
    
    auto r1 = parse_json("[1, 2, 3]");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("[1]");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
}

TEST_CASE("ListValidator - max_length constraint") {
    ListValidator validator;
    validator.max_length = 3;
    
    ValidationState state;
    
    auto r1 = parse_json("[1, 2, 3]");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("[1, 2, 3, 4]");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
}

TEST_CASE("DictValidator - min_length constraint") {
    DictValidator validator;
    validator.min_length = 2;
    
    ValidationState state;
    
    auto r1 = parse_json("{\"a\": 1, \"b\": 2}");
    REQUIRE(r1.is_ok());
    CHECK(validator.validate(*r1.value(), state).is_ok());
    
    auto r2 = parse_json("{\"a\": 1}");
    REQUIRE(r2.is_ok());
    CHECK(validator.validate(*r2.value(), state).is_err());
}

// ========================================================================
// SchemaBuilder tests for constrained validators
// ========================================================================
TEST_CASE("SchemaBuilder - builds ConstrainedIntValidator with gt") {
    std::string schema = R"({"type": "int", "gt": 0})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
    CHECK(v->name() == "constrained-int");
    
    ValidationState state;
    auto input = parse_json("1");
    REQUIRE(input.is_ok());
    CHECK(v->validate(*input.value(), state).is_ok());
    
    auto input2 = parse_json("0");
    REQUIRE(input2.is_ok());
    CHECK(v->validate(*input2.value(), state).is_err());
}

TEST_CASE("SchemaBuilder - builds ConstrainedIntValidator with multiple_of") {
    std::string schema = R"({"type": "int", "multiple_of": 5})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
    CHECK(v->name() == "constrained-int");
}

TEST_CASE("SchemaBuilder - builds StrConstrainedValidator with min_length") {
    std::string schema = R"({"type": "str", "min_length": 3})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
    CHECK(v->name() == "constrained-str");
}

TEST_CASE("SchemaBuilder - builds StrConstrainedValidator with pattern") {
    std::string schema = R"({"type": "str", "pattern": "^[a-z]+$"})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
    CHECK(v->name() == "constrained-str");
}

TEST_CASE("SchemaBuilder - builds ConstrainedFloatValidator") {
    std::string schema = R"({"type": "float", "gt": 0.0})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
    CHECK(v->name() == "constrained-float");
}

TEST_CASE("SchemaBuilder - builds plain IntValidator without constraints") {
    std::string schema = R"({"type": "int"})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
    CHECK(v->name() == "int");
}

TEST_CASE("SchemaBuilder - builds plain StringValidator without constraints") {
    std::string schema = R"({"type": "str"})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
    CHECK(v->name() == "str");
}

} // TEST_SUITE
