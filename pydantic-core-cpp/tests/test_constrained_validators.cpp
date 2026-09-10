#include <doctest/doctest.h>

#include <pybind11/pybind11.h>
#include <pybind11/embed.h>

#include "pydantic_core/validators/basic.hpp"
#include "pydantic_core/validators/containers.hpp"
#include "pydantic_core/validators/special.hpp"
#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/validation_state.hpp"

// Python interpreter for the lifetime of the process (validators under test
// convert values to/from Python objects; one global guard since a second
// scoped_interpreter fails while one is running).
static pybind11::scoped_interpreter g_py_interpreter_guard_{};

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

// ========================================================================
// DefinitionRef / recursive schema tests
// ========================================================================
TEST_CASE("SchemaBuilder - handles definitions wrapper") {
    std::string schema = R"({
        "type": "definitions",
        "schema": {"type": "definition-ref", "schema_ref": "MyNode"},
        "definitions": [{
            "type": "model",
            "ref": "MyNode",
            "cls": "NodeClass",
            "schema": {
                "type": "model-fields",
                "fields": {
                    "value": {
                        "type": "model-field",
                        "schema": {"type": "int"}
                    }
                }
            }
        }]
    })";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
    // The definition-ref is resolved to its target definition ("model").
    CHECK(v->name() == "model");
}

TEST_CASE("SchemaBuilder - definition-ref validates against definition") {
    std::string schema = R"({
        "type": "definitions",
        "schema": {"type": "definition-ref", "schema_ref": "MyNode"},
        "definitions": [{
            "type": "model",
            "ref": "MyNode",
            "cls": "NodeClass",
            "schema": {
                "type": "model-fields",
                "fields": {
                    "value": {
                        "type": "model-field",
                        "schema": {"type": "int"}
                    }
                }
            }
        }]
    })";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
    
    ValidationState state;
    auto input = parse_json(R"({"value": 42})");
    REQUIRE(input.is_ok());
    auto result = v->validate(*input.value(), state);
    CHECK(result.is_ok());
}

TEST_CASE("SchemaBuilder - handles nullable definition-ref") {
    std::string schema = R"({
        "type": "definitions",
        "schema": {"type": "definition-ref", "schema_ref": "MyNode"},
        "definitions": [{
            "type": "model",
            "ref": "MyNode",
            "cls": "NodeClass",
            "schema": {
                "type": "model-fields",
                "fields": {
                    "value": {
                        "type": "model-field",
                        "schema": {"type": "int"}
                    }
                }
            }
        }]
    })";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

} // TEST_SUITE

TEST_SUITE("Decimal Validator") {

static py::object decimal_type_obj() {
    return py::module_::import("decimal").attr("Decimal");
}

static py::object dec(const std::string& value) {
    return decimal_type_obj()(value);
}

// Returns the error type name, or an empty string when validation succeeds.
static std::string decimal_error_type(DecimalValidator& validator, const py::object& value) {
    ValidationState state;
    PythonInput input{value};
    auto result = validator.validate(input, state);
    if (result.is_ok()) return {};
    return result.error().line_errors()[0]->error_type.type_name();
}

static py::object decimal_value(DecimalValidator& validator, const py::object& value) {
    ValidationState state;
    PythonInput input{value};
    auto result = validator.validate(input, state);
    REQUIRE(result.is_ok());
    return *std::static_pointer_cast<py::object>(result.value());
}

TEST_CASE("DecimalValidator - coerces str, int and float inputs") {
    DecimalValidator validator;

    CHECK(py::repr(decimal_value(validator, py::str("42.24"))).cast<std::string>() == "Decimal('42.24')");
    CHECK(py::repr(decimal_value(validator, py::int_(42))).cast<std::string>() == "Decimal('42')");
    // str(float) first, so the binary expansion of 0.1 must not leak through.
    CHECK(py::repr(decimal_value(validator, py::float_(0.1))).cast<std::string>() == "Decimal('0.1')");
}

TEST_CASE("DecimalValidator - passes Decimal instances through unchanged") {
    DecimalValidator validator;
    CHECK(py::repr(decimal_value(validator, dec("42.0"))).cast<std::string>() == "Decimal('42.0')");
}

TEST_CASE("DecimalValidator - decimal_type error for unsupported inputs") {
    DecimalValidator validator;
    CHECK(decimal_error_type(validator, py::list()) == "decimal_type");
    CHECK(decimal_error_type(validator, py::none()) == "decimal_type");
    // bool is an int subclass but is not decimal-coercible (Rust excludes it too)
    CHECK(decimal_error_type(validator, py::bool_(true)) == "decimal_type");
}

TEST_CASE("DecimalValidator - decimal_parsing error for unparseable strings") {
    DecimalValidator validator;
    CHECK(decimal_error_type(validator, py::str("not-a-number")) == "decimal_parsing");
}

TEST_CASE("DecimalValidator - strict requires a Decimal instance") {
    DecimalValidator validator;
    validator.strict = true;
    CHECK(decimal_error_type(validator, py::str("42")) == "is_instance_of");
    CHECK(decimal_error_type(validator, dec("42")) == "");
}

TEST_CASE("DecimalValidator - gt/lt/ge/le constraints") {
    ValidationState state;

    DecimalValidator gt;
    gt.gt = dec("42.24");
    CHECK(gt.validate(PythonInput{dec("43")}, state).is_ok());
    CHECK(decimal_error_type(gt, dec("42")) == "greater_than");

    DecimalValidator lt;
    lt.lt = dec("42.24");
    CHECK(lt.validate(PythonInput{dec("42")}, state).is_ok());
    CHECK(decimal_error_type(lt, dec("43")) == "less_than");

    DecimalValidator ge;
    ge.ge = dec("42.24");
    CHECK(ge.validate(PythonInput{dec("42.24")}, state).is_ok());
    CHECK(decimal_error_type(ge, dec("42")) == "greater_than_equal");

    DecimalValidator le;
    le.le = dec("42.24");
    CHECK(le.validate(PythonInput{dec("42.24")}, state).is_ok());
    CHECK(decimal_error_type(le, dec("43")) == "less_than_equal");
}

TEST_CASE("DecimalValidator - constraint ctx carries the real Decimal") {
    DecimalValidator validator;
    validator.gt = dec("42.24");

    ValidationState state;
    PythonInput input{dec("42")};
    auto result = validator.validate(input, state);
    REQUIRE(result.is_err());

    const auto& line_error = result.error().line_errors()[0];
    // The message renders the display form, errors() reports the object.
    CHECK(line_error->error_type.message() == "Input should be greater than 42.24");
    const auto& ctx_objs = line_error->error_type.context_objects();
    auto it = ctx_objs.find("gt");
    REQUIRE(it != ctx_objs.end());
    CHECK(py::isinstance(it->second, decimal_type_obj()));
    CHECK(py::repr(it->second).cast<std::string>() == "Decimal('42.24')");
}

TEST_CASE("DecimalValidator - max_digits and decimal_places") {
    DecimalValidator too_many_places;
    too_many_places.max_digits = 2;
    too_many_places.decimal_places = 1;
    CHECK(decimal_error_type(too_many_places, dec("0.99")) == "decimal_max_places");

    DecimalValidator too_many_whole;
    too_many_whole.max_digits = 3;
    too_many_whole.decimal_places = 1;
    CHECK(decimal_error_type(too_many_whole, dec("999")) == "decimal_whole_digits");

    DecimalValidator too_many_digits;
    too_many_digits.max_digits = 20;
    too_many_digits.decimal_places = 2;
    CHECK(decimal_error_type(too_many_digits, dec("7424742403889818000000")) == "decimal_max_digits");

    // Leading zeros must not count against the limits.
    DecimalValidator within_limits;
    within_limits.max_digits = 6;
    within_limits.decimal_places = 2;
    CHECK(decimal_error_type(within_limits, dec("000000000001111.700000")) == "");
}

TEST_CASE("DecimalValidator - digit limits pluralize correctly") {
    DecimalValidator places;
    places.decimal_places = 1;
    ValidationState state;
    {
        PythonInput input{dec("0.99")};
        auto result = places.validate(input, state);
        REQUIRE(result.is_err());
        CHECK(result.error().line_errors()[0]->error_type.message() ==
              "Decimal input should have no more than 1 decimal place");
    }

    DecimalValidator digits;
    digits.max_digits = 1;
    {
        PythonInput input{dec("99")};
        auto result = digits.validate(input, state);
        REQUIRE(result.is_err());
        CHECK(result.error().line_errors()[0]->error_type.message() ==
              "Decimal input should have no more than 1 digit in total");
    }
}

TEST_CASE("DecimalValidator - allow_inf_nan gates the finite check") {
    DecimalValidator validator;
    CHECK(decimal_error_type(validator, dec("NaN")) == "finite_number");
    CHECK(decimal_error_type(validator, dec("Infinity")) == "finite_number");

    DecimalValidator allow_inf;
    allow_inf.allow_inf_nan = true;
    CHECK(decimal_error_type(allow_inf, dec("NaN")) == "");
    CHECK(decimal_error_type(allow_inf, dec("Infinity")) == "");
}

TEST_CASE("DecimalValidator - multiple_of") {
    DecimalValidator validator;
    validator.multiple_of = dec("5");
    CHECK(decimal_error_type(validator, dec("45")) == "");

    ValidationState state;
    PythonInput input{dec("42")};
    auto result = validator.validate(input, state);
    REQUIRE(result.is_err());

    const auto& line_error = result.error().line_errors()[0];
    CHECK(line_error->error_type.type_name() == "multiple_of");
    CHECK(line_error->error_type.message() == "Input should be a multiple of 5");
    const auto& ctx_objs = line_error->error_type.context_objects();
    auto it = ctx_objs.find("multiple_of");
    REQUIRE(it != ctx_objs.end());
    CHECK(py::isinstance(it->second, decimal_type_obj()));
}

} // TEST_SUITE("Decimal Validator")

