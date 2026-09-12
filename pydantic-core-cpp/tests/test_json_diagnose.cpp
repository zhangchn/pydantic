#include "doctest/doctest.h"

#include "pydantic_core/json_input.hpp"

#include <optional>
#include <string>

using pydantic_core::json_diagnose_parse_error;

namespace {

std::string diagnose(const std::string& json_text) {
    std::optional<std::string> error = json_diagnose_parse_error(json_text);
    return error.has_value() ? *error : std::string("<valid>");
}

}  // namespace

TEST_SUITE_BEGIN("JsonDiagnose");

TEST_CASE("well-formed JSON has no diagnosis") {
    CHECK_FALSE(json_diagnose_parse_error(R"({"a": 1, "b": [2, 3]})").has_value());
    CHECK_FALSE(json_diagnose_parse_error("[1, 2.5, \"x\", true, false, null]").has_value());
    CHECK_FALSE(json_diagnose_parse_error("  \n\t [1,\n2] \n").has_value());
    // jiter's allow_inf_nan mode accepts these
    CHECK_FALSE(json_diagnose_parse_error("NaN").has_value());
    CHECK_FALSE(json_diagnose_parse_error("[Infinity, -Infinity]").has_value());
}

TEST_CASE("expected value") {
    CHECK(diagnose("(1, 2, 3)") == "expected value at line 1 column 1");
    CHECK(diagnose("[}") == "expected value at line 1 column 2");
    CHECK(diagnose("[1,,2]") == "expected value at line 1 column 4");
    CHECK(diagnose("]") == "expected value at line 1 column 1");
    CHECK(diagnose(",") == "expected value at line 1 column 1");
    CHECK(diagnose(R"({"a":})") == "expected value at line 1 column 6");
}

TEST_CASE("object keys and separators") {
    CHECK(diagnose("{a: 1, b: [2, 3]}") == "key must be a string at line 1 column 2");
    CHECK(diagnose("{,}") == "key must be a string at line 1 column 2");
    CHECK(diagnose("{1:2}") == "key must be a string at line 1 column 2");
    CHECK(diagnose(R"({"a" 1})") == "expected `:` at line 1 column 6");
    CHECK(diagnose(R"({"a":1 "b":2})") == "expected `,` or `}` at line 1 column 8");
    CHECK(diagnose(R"({"a":01})") == "invalid number at line 1 column 7");
}

TEST_CASE("list separators") {
    CHECK(diagnose("[1 \"b\"]") == "expected `,` or `]` at line 1 column 4");
    CHECK(diagnose("[0x1]") == "expected `,` or `]` at line 1 column 3");
    CHECK(diagnose("[1,]") == "trailing comma at line 1 column 4");
    CHECK(diagnose(R"({"a":1,})") == "trailing comma at line 1 column 8");
    CHECK(diagnose(R"({"a":1, })") == "trailing comma at line 1 column 9");
    CHECK(diagnose("[1, ]") == "trailing comma at line 1 column 5");
}

TEST_CASE("trailing characters") {
    CHECK(diagnose("[1] extra") == "trailing characters at line 1 column 5");
    CHECK(diagnose(R"({"a":1}2)") == "trailing characters at line 1 column 8");
    CHECK(diagnose("1.2.3") == "trailing characters at line 1 column 4");
    CHECK(diagnose("0x1") == "trailing characters at line 1 column 2");
    CHECK(diagnose("true false") == "trailing characters at line 1 column 6");
}

TEST_CASE("numbers") {
    CHECK(diagnose("01") == "invalid number at line 1 column 2");
    CHECK(diagnose("--1") == "invalid number at line 1 column 2");
    CHECK(diagnose("-") == "EOF while parsing a value at line 1 column 1");
    CHECK(diagnose("1e") == "EOF while parsing a value at line 1 column 2");
}

TEST_CASE("identifiers") {
    CHECK(diagnose("nan") == "expected ident at line 1 column 2");
    CHECK(diagnose("Nan") == "expected ident at line 1 column 3");
    CHECK(diagnose("tru") == "EOF while parsing a value at line 1 column 3");
    CHECK(diagnose("fals") == "EOF while parsing a value at line 1 column 4");
    CHECK(diagnose("nul") == "EOF while parsing a value at line 1 column 3");
}

TEST_CASE("strings") {
    CHECK(diagnose("\"unterminated") == "EOF while parsing a string at line 1 column 13");
    CHECK(diagnose("\"a\\\"") == "EOF while parsing a string at line 1 column 4");
    CHECK(diagnose("\"\\q\"") == "invalid escape at line 1 column 3");
    CHECK(diagnose("\"\\uZZZZ\"") == "invalid escape at line 1 column 4");
    CHECK(diagnose("\"\\uD800\"") == "unexpected end of hex escape at line 1 column 8");
    CHECK(diagnose("\"a\tb\"") ==
          "control character (\\u0000-\\u001F) found while parsing a string at line 1 column 3");
}

TEST_CASE("end of input names the enclosing context") {
    CHECK(diagnose("") == "EOF while parsing a value at line 1 column 0");
    CHECK(diagnose("   ") == "EOF while parsing a value at line 1 column 3");
    CHECK(diagnose("[") == "EOF while parsing a list at line 1 column 1");
    CHECK(diagnose("[[") == "EOF while parsing a list at line 1 column 2");
    CHECK(diagnose("[1,2") == "EOF while parsing a list at line 1 column 4");
    CHECK(diagnose("[1,") == "EOF while parsing a value at line 1 column 3");
    CHECK(diagnose("{") == "EOF while parsing an object at line 1 column 1");
    CHECK(diagnose("{\"a\"") == "EOF while parsing an object at line 1 column 4");
    CHECK(diagnose("{\"a\":1") == "EOF while parsing an object at line 1 column 6");
    CHECK(diagnose("{\"a\":1,") == "EOF while parsing a value at line 1 column 7");
    CHECK(diagnose("{\"a\":") == "EOF while parsing a value at line 1 column 5");
    CHECK(diagnose("{\"a\":{\"b\":[") == "EOF while parsing a list at line 1 column 11");
    CHECK(diagnose("{\"a\":{\"b\"") == "EOF while parsing an object at line 1 column 9");
    CHECK(diagnose("[\"aa\", \"bb\", \"c") == "EOF while parsing a string at line 1 column 15");
}

TEST_CASE("line numbers follow newlines") {
    CHECK(diagnose("\n\n[") == "EOF while parsing a list at line 3 column 1");
    CHECK(diagnose("{\n  \"a\": 1,\n  \"b\"\n") == "EOF while parsing an object at line 4 column 0");
    CHECK(diagnose("{\n  \"a\": [1,\n  2\n  x") == "expected `,` or `]` at line 4 column 3");
}

TEST_SUITE_END();
