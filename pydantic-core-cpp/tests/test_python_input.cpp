#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/validators/basic.hpp"
#include "pydantic_core/validation_state.hpp"

namespace py = pybind11;
using namespace pydantic_core;

TEST_SUITE("PythonInput") {

TEST_CASE("PythonInput - detects None") {
    py::scoped_interpreter guard;
    PythonInput input{py::none()};
    CHECK(input.is_none());
    CHECK(!input.is_bool());
    CHECK(!input.is_int());
}

TEST_CASE("PythonInput - detects bool") {
    py::scoped_interpreter guard;
    PythonInput input_true{py::bool_(true)};
    CHECK(input_true.is_bool());
    CHECK(!input_true.is_int());

    PythonInput input_false{py::bool_(false)};
    CHECK(input_false.is_bool());
}

TEST_CASE("PythonInput - detects int") {
    py::scoped_interpreter guard;
    PythonInput input{py::int_(42)};
    CHECK(!input.is_bool());
    CHECK(input.is_int());
    CHECK(!input.is_float());
}

TEST_CASE("PythonInput - detects float") {
    py::scoped_interpreter guard;
    PythonInput input{py::float_(3.14)};
    CHECK(!input.is_bool());
    CHECK(!input.is_int());
    CHECK(input.is_float());
}

TEST_CASE("PythonInput - detects str") {
    py::scoped_interpreter guard;
    PythonInput input{py::str("hello")};
    CHECK(input.is_str());
    CHECK(!input.is_bytes());
}

TEST_CASE("PythonInput - validates str strict") {
    py::scoped_interpreter guard;
    PythonInput input{py::str("hello")};
    auto result = input.validate_str(true, false);
    CHECK(result.is_ok());
    CHECK(result.value().exactness() == Exactness::Exact);
}

TEST_CASE("PythonInput - rejects int for str strict") {
    py::scoped_interpreter guard;
    PythonInput input{py::int_(42)};
    auto result = input.validate_str(true, false);
    CHECK(result.is_err());
}

TEST_CASE("PythonInput - coerces int to str lax") {
    py::scoped_interpreter guard;
    PythonInput input{py::int_(42)};
    auto result = input.validate_str(false, false);
    CHECK(result.is_ok());
    CHECK(result.value().exactness() == Exactness::Lax);
}

TEST_CASE("PythonInput - validates int strict") {
    py::scoped_interpreter guard;
    PythonInput input{py::int_(42)};
    auto result = input.validate_int(true);
    CHECK(result.is_ok());
    CHECK(result.value().exactness() == Exactness::Exact);
}

TEST_CASE("PythonInput - rejects str for int strict") {
    py::scoped_interpreter guard;
    PythonInput input{py::str("42")};
    auto result = input.validate_int(true);
    CHECK(result.is_err());
}

TEST_CASE("PythonInput - coerces str to int lax") {
    py::scoped_interpreter guard;
    PythonInput input{py::str("42")};
    auto result = input.validate_int(false);
    CHECK(result.is_ok());
    CHECK(result.value().exactness() == Exactness::Lax);
}

TEST_CASE("PythonInput - validates float strict") {
    py::scoped_interpreter guard;
    PythonInput input{py::float_(3.14)};
    auto result = input.validate_float(true);
    CHECK(result.is_ok());
}

TEST_CASE("PythonInput - coerces int to float lax") {
    py::scoped_interpreter guard;
    PythonInput input{py::int_(42)};
    auto result = input.validate_float(false);
    CHECK(result.is_ok());
    CHECK(result.value().exactness() == Exactness::Lax);
}

TEST_CASE("PythonInput - validates bool strict") {
    py::scoped_interpreter guard;
    PythonInput input{py::bool_(true)};
    auto result = input.validate_bool(true);
    CHECK(result.is_ok());
}

TEST_CASE("PythonInput - coerces int to bool lax") {
    py::scoped_interpreter guard;
    PythonInput input{py::int_(1)};
    auto result = input.validate_bool(false);
    CHECK(result.is_ok());
    CHECK(result.value().value() == true);
}

TEST_CASE("PythonInput - validates dict") {
    py::scoped_interpreter guard;
    py::dict d;
    d["a"] = py::int_(1);
    d["b"] = py::str("hello");
    PythonInput input{d};

    auto result = input.validate_dict(false);
    CHECK(result.is_ok());
    auto dict = std::move(result.value());
    CHECK(dict->size() == 2);
}

TEST_CASE("PythonInput - validates list") {
    py::scoped_interpreter guard;
    py::list l;
    l.append(1);
    l.append(2);
    l.append(3);
    PythonInput input{l};

    auto result = input.validate_list(false);
    CHECK(result.is_ok());
    auto list = std::move(result.value().value());
    CHECK(list->size() == 3);
}

TEST_CASE("PythonInput - as_error_value") {
    py::scoped_interpreter guard;
    PythonInput input{py::int_(42)};
    auto value = input.as_error_value();
    CHECK(value.repr == "42");
}

} // TEST_SUITE
