#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pydantic_core/combined_validator.hpp"
#include "pydantic_core/validator.hpp"
#include "pydantic_core/errors.hpp"
#include "pydantic_core/validation_state.hpp"
#include "pydantic_core/validators/basic.hpp"
#include "pydantic_core/validators/complex.hpp"
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

TEST_CASE("IsInstanceValidator requires a Python input") {
    // Rust asks input.as_python() and reports needs_python_object when the value
    // never existed as a Python object, so a non-Python input is rejected even
    // with no class configured.
    auto v = std::make_shared<IsInstanceValidator>();
    ValidationState state;

    StringInput str_input("hello");
    CHECK(v->validate(str_input, state).is_err());

    StringInput int_input("42");
    CHECK(v->validate(int_input, state).is_err());

    // A real Python object with no class info passes straight through.
    PythonInput py_input(py::str("hello"));
    CHECK(v->validate(py_input, state).is_ok());
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

// ---------------------------------------------------------------------------
// TaggedUnionValidator: which question the tag lookup asks
// ---------------------------------------------------------------------------
TEST_SUITE("TaggedUnionValidator") {

// The error type the union reports when it cannot read its tag, or the error it
// reports once the tag is read ("<ok>" for a matched choice).
static std::string tag_union_error(const py::object& input, bool json_run, bool from_attributes) {
    TaggedUnionValidator::PathStep key;
    key.key = "t";
    std::vector<TaggedUnionValidator::PathStep> path{key};
    std::vector<TaggedUnionValidator::Choice> choices{
        TaggedUnionValidator::Choice{py::str("A"),
                                     std::make_shared<LiteralValidator>(py::list{py::str("A")})}};
    TaggedUnionValidator union_v({path}, std::move(choices));
    union_v.set_from_attributes(from_attributes);
    ValidationState state;
    if (json_run) state.set_input_type(InputType::Json);
    PythonInput py_input(input);
    auto result = union_v.validate(py_input, state);
    if (result.is_ok()) return "<ok>";
    return result.error().line_errors()[0]->error_type.type_name();
}

TEST_CASE("A document is only ever asked for a dict") {
    // Rust's validate_model_fields drops from_attributes for every input but
    // Python's (input_abstract.rs:177-179) and JSON's validate_dict drops strict
    // with it (input_json.rs:230-235), so whatever the schema asked for, a
    // document that is not an object reports dict_type.  The port runs a JSON
    // document through PythonInput once it is materialized, so the run kind held
    // by the state is the only thing keeping it from asking attributes of a
    // number and reporting model_attributes_type.
    for (bool from_attributes : {false, true}) {
        CHECK(tag_union_error(py::int_(1), true, from_attributes) == "dict_type");
        CHECK(tag_union_error(py::list{}, true, from_attributes) == "dict_type");
        CHECK(tag_union_error(py::str("A"), true, from_attributes) == "dict_type");
        CHECK(tag_union_error(py::none(), true, from_attributes) == "dict_type");
    }

    // A Python run keeps the two questions apart.
    CHECK(tag_union_error(py::int_(1), false, true) == "model_attributes_type");
    CHECK(tag_union_error(py::int_(1), false, false) == "dict_type");

    // An object still gives up its tag in a JSON run.
    py::dict doc;
    doc["t"] = py::str("B");
    CHECK(tag_union_error(doc, true, true) == "union_tag_invalid");
    CHECK(tag_union_error(doc, true, false) == "union_tag_invalid");
}

} // TEST_SUITE

// ---------------------------------------------------------------------------
// PyDataclassValidator: the name it reports
// ---------------------------------------------------------------------------
TEST_SUITE("PyDataclassValidator") {

TEST_CASE("A dataclass names itself after its class") {
    // Rust's DataclassValidator::get_name() returns the class name (dataclass.rs:617
    // with :480-483), so a run over a dataclass titles itself
    // `1 validation error for D` and a node above it composes `list[D]`.
    PyDataclassValidator v;
    CHECK(v.name() == "dataclass");
    CHECK(v.display_name() == "dataclass");  // nothing has named the class yet
    v.set_class_name("D");
    CHECK(v.display_name() == "D");
    // name() still reports the schema type: result-to-Python dispatch reads it.
    CHECK(v.name() == "dataclass");
}

} // TEST_SUITE
