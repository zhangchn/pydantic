#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <unordered_map>

#include <pybind11/pybind11.h>
#include <pybind11/embed.h>

#include "pydantic_core/schema_validator.hpp"
#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/json_input.hpp"

namespace py = pybind11;

// Python interpreter for the lifetime of the process (validate_python/
// isinstance_python convert values to Python objects; one global guard since
// a second scoped_interpreter fails while one is running).
static pybind11::scoped_interpreter g_py_interpreter_guard_{};

using namespace pydantic_core;

TEST_SUITE("SchemaValidator Integration") {

    TEST_CASE("SchemaValidator - build any validator") {
        std::string schema_json = R"({"type": "any"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build int validator") {
        std::string schema_json = R"({"type": "int"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build str validator") {
        std::string schema_json = R"({"type": "str"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build bool validator") {
        std::string schema_json = R"({"type": "bool"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build float validator") {
        std::string schema_json = R"({"type": "float"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build nullable validator") {
        std::string schema_json = R"({"type": "nullable"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build union validator") {
        std::string schema_json = R"({"type": "union"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build list validator") {
        std::string schema_json = R"({"type": "list"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build dict validator") {
        std::string schema_json = R"({"type": "dict"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build set validator") {
        std::string schema_json = R"({"type": "set"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build frozenset validator") {
        std::string schema_json = R"({"type": "frozenset"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build tuple validator") {
        std::string schema_json = R"({"type": "tuple"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build date validator") {
        std::string schema_json = R"({"type": "date"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build time validator") {
        std::string schema_json = R"({"type": "time"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build datetime validator") {
        std::string schema_json = R"({"type": "datetime"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build timedelta validator") {
        std::string schema_json = R"({"type": "timedelta"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build url validator") {
        std::string schema_json = R"({"type": "url"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build uuid validator") {
        std::string schema_json = R"({"type": "uuid"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build literal validator") {
        std::string schema_json = R"({"type": "literal"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build enum validator") {
        std::string schema_json = R"({"type": "enum"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json));
    }

    TEST_CASE("SchemaValidator - build with config") {
        std::string schema_json = R"({"type": "any"})";
        std::string config_json = R"({"title": "TestSchema"})";
        
        REQUIRE_NOTHROW(SchemaValidator validator(schema_json, config_json));
    }

    TEST_CASE("SchemaValidator - invalid schema throws") {
        std::string schema_json = R"({"type": "unknown_type"})";
        
        REQUIRE_THROWS_AS(SchemaValidator validator(schema_json), SchemaError&);
    }

    TEST_CASE("SchemaValidator - invalid JSON throws") {
        std::string schema_json = "not valid json";
        
        REQUIRE_THROWS_AS(SchemaValidator validator(schema_json), SchemaError&);
    }

    TEST_CASE("SchemaValidator - validate_python with any schema") {
        std::string schema_json = R"({"type": "any"})";
        SchemaValidator validator(schema_json);
        
        std::string input_json = R"({"key": "value"})";
        REQUIRE_NOTHROW(validator.validate_python(input_json));
    }

    TEST_CASE("SchemaValidator - validate_python with int schema") {
        std::string schema_json = R"({"type": "int"})";
        SchemaValidator validator(schema_json);
        
        std::string input_json = "42";
        REQUIRE_NOTHROW(validator.validate_python(input_json));
    }

    TEST_CASE("SchemaValidator - validate_python with str schema") {
        std::string schema_json = R"({"type": "str"})";
        SchemaValidator validator(schema_json);
        
        std::string input_json = R"("hello")";
        REQUIRE_NOTHROW(validator.validate_python(input_json));
    }

    TEST_CASE("SchemaValidator - validate_python with bool schema") {
        std::string schema_json = R"({"type": "bool"})";
        SchemaValidator validator(schema_json);
        
        std::string input_json = "true";
        REQUIRE_NOTHROW(validator.validate_python(input_json));
    }

    TEST_CASE("SchemaValidator - validate_python with float schema") {
        std::string schema_json = R"({"type": "float"})";
        SchemaValidator validator(schema_json);
        
        std::string input_json = "3.14";
        REQUIRE_NOTHROW(validator.validate_python(input_json));
    }

    TEST_CASE("SchemaValidator - validate_json with any schema") {
        std::string schema_json = R"({"type": "any"})";
        SchemaValidator validator(schema_json);
        
        std::string json_data = R"({"key": "value"})";
        REQUIRE_NOTHROW(validator.validate_json(json_data));
    }

    TEST_CASE("SchemaValidator - isinstance_python returns true for valid input") {
        std::string schema_json = R"({"type": "any"})";
        SchemaValidator validator(schema_json);
        
        std::string input_json = R"({"key": "value"})";
        REQUIRE(validator.isinstance_python(input_json) == true);
    }

    TEST_CASE("SchemaValidator - title property") {
        std::string schema_json = R"({"type": "any"})";
        SchemaValidator validator(schema_json);
        
        CHECK(validator.title() == "Schema");
    }

    TEST_CASE("SchemaValidator - repr property") {
        std::string schema_json = R"({"type": "any"})";
        SchemaValidator validator(schema_json);
        
        std::string repr = validator.repr();
        CHECK(repr.find("SchemaValidator") != std::string::npos);
        CHECK(repr.find("Schema") != std::string::npos);
    }

    TEST_CASE("SchemaBuilder - build any validator") {
        std::string schema_json = R"({"type": "any"})";
        
        auto validator = SchemaBuilder::build(schema_json);
        CHECK(validator != nullptr);
        CHECK(validator->name() == "any");
    }

    TEST_CASE("SchemaBuilder - build int validator") {
        std::string schema_json = R"({"type": "int"})";
        
        auto validator = SchemaBuilder::build(schema_json);
        CHECK(validator != nullptr);
        CHECK(validator->name() == "int");
    }

    TEST_CASE("SchemaBuilder - build str validator") {
        std::string schema_json = R"({"type": "str"})";
        
        auto validator = SchemaBuilder::build(schema_json);
        CHECK(validator != nullptr);
        CHECK(validator->name() == "str");
    }

    TEST_CASE("SchemaBuilder - build bool validator") {
        std::string schema_json = R"({"type": "bool"})";
        
        auto validator = SchemaBuilder::build(schema_json);
        CHECK(validator != nullptr);
        CHECK(validator->name() == "bool");
    }

    TEST_CASE("SchemaBuilder - build float validator") {
        std::string schema_json = R"({"type": "float"})";
        
        auto validator = SchemaBuilder::build(schema_json);
        CHECK(validator != nullptr);
        CHECK(validator->name() == "float");
    }

    TEST_CASE("SchemaBuilder - build list validator") {
        std::string schema_json = R"({"type": "list"})";
        
        auto validator = SchemaBuilder::build(schema_json);
        CHECK(validator != nullptr);
        CHECK(validator->name() == "list");
    }

    TEST_CASE("SchemaBuilder - build dict validator") {
        std::string schema_json = R"({"type": "dict"})";
        
        auto validator = SchemaBuilder::build(schema_json);
        CHECK(validator != nullptr);
        CHECK(validator->name() == "dict");
    }

    TEST_CASE("SchemaBuilder - build nullable validator") {
        std::string schema_json = R"({"type": "nullable"})";
        
        auto validator = SchemaBuilder::build(schema_json);
        CHECK(validator != nullptr);
        CHECK(validator->name() == "nullable");
    }

    TEST_CASE("SchemaBuilder - build union validator") {
        std::string schema_json = R"({"type": "union"})";
        
        auto validator = SchemaBuilder::build(schema_json);
        CHECK(validator != nullptr);
        CHECK(validator->name() == "union");
    }

    TEST_CASE("SchemaBuilder - build with config") {
        std::string schema_json = R"({"type": "any"})";
        std::string config_json = R"({"title": "TestSchema"})";
        
        auto validator = SchemaBuilder::build(schema_json, config_json);
        CHECK(validator != nullptr);
    }

    TEST_CASE("SchemaBuilder - invalid schema throws") {
        std::string schema_json = R"({"type": "unknown_type"})";
        
        REQUIRE_THROWS_AS(SchemaBuilder::build(schema_json), std::runtime_error&);
    }

    TEST_CASE("SchemaBuilder - invalid JSON throws") {
        std::string schema_json = "not valid json";
        
        REQUIRE_THROWS_AS(SchemaBuilder::build(schema_json), std::runtime_error&);
    }
}
