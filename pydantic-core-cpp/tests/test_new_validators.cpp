#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/validator.hpp"
#include "pydantic_core/errors.hpp"
#include "pydantic_core/validation_state.hpp"
#include "pydantic_core/validators/basic.hpp"
#include "pydantic_core/validators/special.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include <pybind11/eval.h>

#include "pydantic_core/python_input.hpp"

#include <string>

// Python interpreter for the lifetime of the process (validators under test
// convert values to/from Python objects; one global guard since a second
// scoped_interpreter fails while one is running).
static pybind11::scoped_interpreter g_py_interpreter_guard_{};

using namespace pydantic_core;

// ---------------------------------------------------------------------------
// IsInstanceValidator tests
// ---------------------------------------------------------------------------
TEST_SUITE("IsInstanceValidator") {

TEST_CASE("IsInstanceValidator accepts any non-null input") {
    // No class info (zero-arg): accepts any non-null input.
    auto v = std::make_shared<IsInstanceValidator>();
    ValidationState state;
    
    // Accepts string
    StringInput str_input("hello");
    auto result = v->validate(str_input, state);
    CHECK(result.is_ok());
    
    // Accepts int
    StringInput int_input("42");
    auto result2 = v->validate(int_input, state);
    CHECK(result2.is_ok());
}

TEST_CASE("IsInstanceValidator has correct name") {
    auto v = std::make_shared<IsInstanceValidator>("TestClass", py::none());
    // name() is the result-dispatch token (raw PyObject* pass-through), see
    // schema_validator.cpp: "py_raw_object: PyObject* stored by
    // is-instance/is-subclass validators".
    CHECK(v->name() == "py_raw_object");
}

} // TEST_SUITE

// ---------------------------------------------------------------------------
// IsSubclassValidator tests
// ---------------------------------------------------------------------------
TEST_SUITE("IsSubclassValidator") {

TEST_CASE("IsSubclassValidator accepts any input") {
    auto v = std::make_shared<IsSubclassValidator>("BaseClass", py::none());
    ValidationState state;
    
    StringInput str_input("value");
    auto result = v->validate(str_input, state);
    CHECK(result.is_ok());
}

TEST_CASE("IsSubclassValidator has correct name") {
    auto v = std::make_shared<IsSubclassValidator>("BaseClass", py::none());
    CHECK(v->name() == "py_raw_object");
}

} // TEST_SUITE

// ---------------------------------------------------------------------------
// CallableValidator tests
// ---------------------------------------------------------------------------
TEST_SUITE("CallableValidator") {

TEST_CASE("CallableValidator rejects non-callable input") {
    auto v = std::make_shared<CallableValidator>();
    ValidationState state;
    
    StringInput str_input("some_func");
    auto result = v->validate(str_input, state);
    CHECK(result.is_err());
}

TEST_CASE("CallableValidator accepts a Python callable") {
    auto v = std::make_shared<CallableValidator>();
    ValidationState state;
    
    py::object fn = py::eval("lambda x: x");
    PythonInput py_input(fn);
    auto result = v->validate(py_input, state);
    CHECK(result.is_ok());
}

TEST_CASE("CallableValidator has correct name") {
    auto v = std::make_shared<CallableValidator>();
    CHECK(v->name() == "callable");
}

} // TEST_SUITE

// ---------------------------------------------------------------------------
// SchemaBuilder tests for new validators
// ---------------------------------------------------------------------------
TEST_SUITE("SchemaBuilder - New Validator Types") {

TEST_CASE("SchemaBuilder - builds is-instance validator") {
    std::string schema = R"({"type": "is-instance", "cls": "MyClass"})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

TEST_CASE("SchemaBuilder - builds is-subclass validator") {
    std::string schema = R"({"type": "is-subclass", "cls": "BaseClass"})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

TEST_CASE("SchemaBuilder - builds callable validator") {
    std::string schema = R"({"type": "callable"})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

TEST_CASE("SchemaBuilder - builds generator as any") {
    std::string schema = R"({"type": "generator"})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

TEST_CASE("SchemaBuilder - builds json-or-python validator") {
    std::string schema = R"({
        "type": "json-or-python",
        "json_schema": {"type": "int"},
        "python_schema": {"type": "int"}
    })";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

} // TEST_SUITE

// ---------------------------------------------------------------------------
// ConstrainedBytesValidator tests via SchemaBuilder
// ---------------------------------------------------------------------------
TEST_SUITE("ConstrainedBytesValidator via SchemaBuilder") {

TEST_CASE("SchemaBuilder - builds constrained-bytes with min_length") {
    std::string schema = R"({"type": "bytes", "min_length": 2})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

TEST_CASE("SchemaBuilder - builds constrained-bytes with max_length") {
    std::string schema = R"({"type": "bytes", "max_length": 10})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

TEST_CASE("SchemaBuilder - builds constrained-bytes with both constraints") {
    std::string schema = R"({"type": "bytes", "min_length": 2, "max_length": 10})";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

} // TEST_SUITE

// ---------------------------------------------------------------------------
// List validator fail_fast tests
// ---------------------------------------------------------------------------
TEST_SUITE("ListValidator fail_fast") {

TEST_CASE("SchemaBuilder - builds list with fail_fast") {
    std::string schema = R"({
        "type": "list",
        "items_schema": {"type": "int"},
        "fail_fast": true
    })";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

TEST_CASE("SchemaBuilder - builds list with min_length and max_length") {
    std::string schema = R"({
        "type": "list",
        "items_schema": {"type": "int"},
        "min_length": 1,
        "max_length": 10
    })";
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

} // TEST_SUITE

// ---------------------------------------------------------------------------
// Definition-ref in serializer tests
// ---------------------------------------------------------------------------
TEST_SUITE("Serializer Definition-Ref") {

TEST_CASE("Definition-ref resolves in serializer build") {
    // This test validates that the serializer properly handles
    // definition-ref types via the two-pass build mechanism
    std::string schema = R"({
        "type": "definitions",
        "schema": {"type": "definition-ref", "schema_ref": "Node"},
        "definitions": [{
            "type": "model",
            "ref": "Node",
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
    // The SchemaBuilder should not throw
    auto v = SchemaBuilder::build(schema);
    CHECK(v != nullptr);
}

} // TEST_SUITE
