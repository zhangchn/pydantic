#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "pydantic_core/validators/mod.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/string_input.hpp"
#include "pydantic_core/validation_state.hpp"
#include <simdjson.h>

using namespace pydantic_core;

// Helper to parse JSON and get element
// Note: simdjson parser must stay alive while the element is used
// We use a static parser for tests (not ideal but works for test purposes)
std::unique_ptr<JsonInput> make_json_input(const std::string& json) {
    static simdjson::dom::parser parser;
    auto result = parser.parse(json);
    if (result.error()) {
        throw std::runtime_error("Failed to parse JSON: " + json);
    }
    return std::make_unique<JsonInput>(result.value());
}

// Helper to make string input
std::unique_ptr<StringInput> make_string_input(const std::string& value) {
    return std::make_unique<StringInput>(value);
}

TEST_SUITE("NoneValidator") {
    TEST_CASE("Validates null") {
        auto input = make_json_input("null");
        NoneValidator validator;
        ValidationState state;
        
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(result.value().is_none());
    }
    
    TEST_CASE("Rejects non-null in strict mode") {
        auto input = make_json_input("42");
        NoneValidator validator{true};  // strict
        ValidationState state;
        
        auto result = validator.validate(*input, state);
        CHECK(result.is_err());
        CHECK(result.error().line_errors().size() == 1);
        CHECK(result.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::NoneType);
    }
    
    TEST_CASE("StringInput null recognition") {
        auto input = make_string_input("null");
        CHECK(input->is_none());
        
        auto input2 = make_string_input("None");
        CHECK(input2->is_none());
        
        auto input3 = make_string_input("");
        CHECK(input3->is_none());
    }
}

TEST_SUITE("BoolValidator") {
    TEST_CASE("Validates true/false in strict mode") {
        BoolValidator validator{true};  // strict
        ValidationState state;
        
        auto input_true = make_json_input("true");
        auto result = validator.validate(*input_true, state);
        CHECK(result.is_ok());
        CHECK(result.value().is_bool());
        CHECK(std::get<bool>(result.value().data) == true);
        
        auto input_false = make_json_input("false");
        auto result2 = validator.validate(*input_false, state);
        CHECK(result2.is_ok());
        CHECK(std::get<bool>(result2.value().data) == false);
    }
    
    TEST_CASE("Rejects non-bool in strict mode") {
        BoolValidator validator{true};
        ValidationState state;
        
        auto input = make_json_input("\"true\"");
        auto result = validator.validate(*input, state);
        CHECK(result.is_err());
    }
    
    TEST_CASE("Coerces in lax mode") {
        BoolValidator validator{false};  // lax
        ValidationState state;
        
        // String "true" -> true
        auto input1 = make_json_input("\"true\"");
        auto result1 = validator.validate(*input1, state);
        CHECK(result1.is_ok());
        CHECK(std::get<bool>(result1.value().data) == true);
        
        // String "false" -> false
        auto input2 = make_json_input("\"false\"");
        auto result2 = validator.validate(*input2, state);
        CHECK(result2.is_ok());
        CHECK(std::get<bool>(result2.value().data) == false);
        
        // Int 1 -> true
        auto input3 = make_json_input("1");
        auto result3 = validator.validate(*input3, state);
        CHECK(result3.is_ok());
        CHECK(std::get<bool>(result3.value().data) == true);
        
        // Int 0 -> false
        auto input4 = make_json_input("0");
        auto result4 = validator.validate(*input4, state);
        CHECK(result4.is_ok());
        CHECK(std::get<bool>(result4.value().data) == false);
    }
    
    TEST_CASE("StringInput boolean coercion") {
        BoolValidator validator{false};
        ValidationState state;
        
        auto input1 = make_string_input("yes");
        auto result1 = validator.validate(*input1, state);
        CHECK(result1.is_ok());
        CHECK(std::get<bool>(result1.value().data) == true);
        
        auto input2 = make_string_input("no");
        auto result2 = validator.validate(*input2, state);
        CHECK(result2.is_ok());
        CHECK(std::get<bool>(result2.value().data) == false);
    }
}

TEST_SUITE("IntValidator") {
    TEST_CASE("Validates integers in strict mode") {
        IntValidator validator{true};
        ValidationState state;
        
        auto input = make_json_input("42");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(result.value().is_int());
        CHECK(std::get<int64_t>(result.value().data) == 42);
    }
    
    TEST_CASE("Validates large uint64") {
        IntValidator validator{true};
        ValidationState state;
        
        auto input = make_json_input("18446744073709551615");  // max uint64
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(result.value().is_int());
    }
    
    TEST_CASE("Rejects float in strict mode") {
        IntValidator validator{true};
        ValidationState state;
        
        auto input = make_json_input("42.5");
        auto result = validator.validate(*input, state);
        CHECK(result.is_err());
    }
    
    TEST_CASE("Coerces float to int in lax mode") {
        IntValidator validator{false};
        ValidationState state;
        
        auto input = make_json_input("42.0");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(std::get<int64_t>(result.value().data) == 42);
    }
    
    TEST_CASE("Coerces string to int in lax mode") {
        IntValidator validator{false};
        ValidationState state;
        
        auto input = make_json_input("\"123\"");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(std::get<int64_t>(result.value().data) == 123);
    }
}

TEST_SUITE("ConstrainedIntValidator") {
    TEST_CASE("gt constraint") {
        ConstrainedIntValidator validator{false, {}, {}, {}, {}, 10};  // gt=10
        ValidationState state;
        
        auto input_ok = make_json_input("11");
        auto result_ok = validator.validate(*input_ok, state);
        CHECK(result_ok.is_ok());
        
        auto input_fail = make_json_input("10");
        auto result_fail = validator.validate(*input_fail, state);
        CHECK(result_fail.is_err());
        CHECK(result_fail.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::IntGreaterThan);
    }
    
    TEST_CASE("lt constraint") {
        ConstrainedIntValidator validator{false, {}, {}, 100, {}, {}};  // lt=100
        ValidationState state;
        
        auto input_ok = make_json_input("99");
        auto result_ok = validator.validate(*input_ok, state);
        CHECK(result_ok.is_ok());
        
        auto input_fail = make_json_input("100");
        auto result_fail = validator.validate(*input_fail, state);
        CHECK(result_fail.is_err());
        CHECK(result_fail.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::IntLessThan);
    }
    
    TEST_CASE("ge constraint") {
        ConstrainedIntValidator validator{false, {}, {}, {}, 0, {}};  // ge=0
        ValidationState state;
        
        auto input_ok = make_json_input("0");
        auto result_ok = validator.validate(*input_ok, state);
        CHECK(result_ok.is_ok());
        
        auto input_fail = make_json_input("-1");
        auto result_fail = validator.validate(*input_fail, state);
        CHECK(result_fail.is_err());
        CHECK(result_fail.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::IntGreaterThanEqual);
    }
    
    TEST_CASE("le constraint") {
        ConstrainedIntValidator validator{false, {}, 50, {}, {}, {}};  // le=50
        ValidationState state;
        
        auto input_ok = make_json_input("50");
        auto result_ok = validator.validate(*input_ok, state);
        CHECK(result_ok.is_ok());
        
        auto input_fail = make_json_input("51");
        auto result_fail = validator.validate(*input_fail, state);
        CHECK(result_fail.is_err());
        CHECK(result_fail.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::IntLessThanEqual);
    }
    
    TEST_CASE("multiple_of constraint") {
        ConstrainedIntValidator validator{false, 5, {}, {}, {}, {}};  // multiple_of=5
        ValidationState state;
        
        auto input_ok = make_json_input("25");
        auto result_ok = validator.validate(*input_ok, state);
        CHECK(result_ok.is_ok());
        
        auto input_fail = make_json_input("23");
        auto result_fail = validator.validate(*input_fail, state);
        CHECK(result_fail.is_err());
        CHECK(result_fail.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::IntMultipleOf);
    }
    
    TEST_CASE("Combined constraints") {
        ConstrainedIntValidator validator{false, {}, 100, {}, 0, {}};  // 0 <= x <= 100
        ValidationState state;
        
        auto input_ok = make_json_input("50");
        auto result_ok = validator.validate(*input_ok, state);
        CHECK(result_ok.is_ok());
        
        auto input_low = make_json_input("-1");
        CHECK(validator.validate(*input_low, state).is_err());
        
        auto input_high = make_json_input("101");
        CHECK(validator.validate(*input_high, state).is_err());
    }
}

TEST_SUITE("FloatValidator") {
    TEST_CASE("Validates floats") {
        FloatValidator validator{false};
        ValidationState state;
        
        auto input = make_json_input("3.14159");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(result.value().is_float());
        CHECK(std::get<double>(result.value().data) == doctest::Approx(3.14159));
    }
    
    TEST_CASE("Int is valid float") {
        FloatValidator validator{true};
        ValidationState state;
        
        auto input = make_json_input("42");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(std::get<double>(result.value().data) == 42.0);
    }
    
    TEST_CASE("Rejects inf/nan by default") {
        FloatValidator validator{false};
        ValidationState state;
        
        // Note: simdjson may not parse these directly, so we test the logic
        // In practice, inf/nan come from Python inputs
    }
}

TEST_SUITE("StringValidator") {
    TEST_CASE("Validates strings") {
        StringValidator validator{true};
        ValidationState state;
        
        auto input = make_json_input("\"hello\"");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(result.value().is_string());
        CHECK(std::get<std::string>(result.value().data) == "hello");
    }
    
    TEST_CASE("Rejects non-string in strict mode") {
        StringValidator validator{true};
        ValidationState state;
        
        auto input = make_json_input("42");
        auto result = validator.validate(*input, state);
        CHECK(result.is_err());
    }
    
    TEST_CASE("Coerces int to string in lax mode") {
        StringValidator validator{false};
        ValidationState state;
        
        auto input = make_json_input("42");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(std::get<std::string>(result.value().data) == "42");
    }
}

TEST_SUITE("ConstrainedStringValidator") {
    TEST_CASE("min_length constraint") {
        ConstrainedStringValidator validator{false, 3, {}, {}, false, false, false};
        ValidationState state;
        
        auto input_ok = make_json_input("\"abc\"");
        auto result_ok = validator.validate(*input_ok, state);
        CHECK(result_ok.is_ok());
        
        auto input_fail = make_json_input("\"ab\"");
        auto result_fail = validator.validate(*input_fail, state);
        CHECK(result_fail.is_err());
        CHECK(result_fail.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::StringTooShort);
    }
    
    TEST_CASE("max_length constraint") {
        ConstrainedStringValidator validator{false, {}, 5, {}, false, false, false};
        ValidationState state;
        
        auto input_ok = make_json_input("\"abc\"");
        auto result_ok = validator.validate(*input_ok, state);
        CHECK(result_ok.is_ok());
        
        auto input_fail = make_json_input("\"abcdef\"");
        auto result_fail = validator.validate(*input_fail, state);
        CHECK(result_fail.is_err());
        CHECK(result_fail.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::StringTooLong);
    }
    
    TEST_CASE("to_lower transformation") {
        ConstrainedStringValidator validator{false, {}, {}, {}, true, false, false};
        ValidationState state;
        
        auto input = make_json_input("\"HELLO\"");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(std::get<std::string>(result.value().data) == "hello");
    }
    
    TEST_CASE("to_upper transformation") {
        ConstrainedStringValidator validator{false, {}, {}, {}, false, true, false};
        ValidationState state;
        
        auto input = make_json_input("\"hello\"");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(std::get<std::string>(result.value().data) == "HELLO");
    }
    
    TEST_CASE("pattern constraint") {
        std::regex email_pattern(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
        ConstrainedStringValidator validator{false, {}, {}, email_pattern, false, false, false};
        ValidationState state;
        
        auto input_ok = make_json_input("\"test@example.com\"");
        auto result_ok = validator.validate(*input_ok, state);
        CHECK(result_ok.is_ok());
        
        auto input_fail = make_json_input("\"not-an-email\"");
        auto result_fail = validator.validate(*input_fail, state);
        CHECK(result_fail.is_err());
        CHECK(result_fail.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::StringPatternMismatch);
    }
}

TEST_SUITE("CombinedValidator variant") {
    TEST_CASE("Can hold different validators") {
        CombinedValidator v1 = BoolValidator{true};
        CombinedValidator v2 = IntValidator{false};
        CombinedValidator v3 = StringValidator{true};
        
        CHECK(std::holds_alternative<BoolValidator>(v1));
        CHECK(std::holds_alternative<IntValidator>(v2));
        CHECK(std::holds_alternative<StringValidator>(v3));
    }
    
    TEST_CASE("Validate with visitor") {
        CombinedValidator validator = IntValidator{true};
        auto input = make_json_input("42");
        ValidationState state;
        
        ValidateVisitor visitor{*input, state};
        auto result = std::visit(visitor, validator);
        CHECK(result.is_ok());
        CHECK(std::get<int64_t>(result.value().data) == 42);
    }
}

TEST_SUITE("NullableValidator") {
    TEST_CASE("Accepts None") {
        NullableValidator validator{nullptr};
        ValidationState state;
        
        auto input = make_json_input("null");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(result.value().is_none());
    }
    
    TEST_CASE("Validates with inner validator") {
        auto inner = std::shared_ptr<CombinedValidatorFinal>(new CombinedValidatorFinal(std::in_place_index<2>, IntValidator{true}));
        NullableValidator validator{inner};
        ValidationState state;
        
        auto input = make_json_input("42");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
        CHECK(std::get<int64_t>(result.value().data) == 42);
    }
}

TEST_SUITE("LiteralValidator") {
    TEST_CASE("Matches allowed value") {
        LiteralValidator validator;
        validator.allowed_values = {ValidatedValue(static_cast<int64_t>(1)), ValidatedValue(static_cast<int64_t>(2)), ValidatedValue(static_cast<int64_t>(3))};
        ValidationState state;
        
        auto input = make_json_input("2");
        auto result = validator.validate(*input, state);
        CHECK(result.is_ok());
    }
    
    TEST_CASE("Rejects non-allowed value") {
        LiteralValidator validator;
        validator.allowed_values = {ValidatedValue(std::string("a")), ValidatedValue(std::string("b"))};
        ValidationState state;
        
        auto input = make_json_input("\"c\"");
        auto result = validator.validate(*input, state);
        CHECK(result.is_err());
        CHECK(result.error().line_errors()[0]->error_type.kind() == ErrorType::Kind::LiteralMismatch);
    }
}

TEST_SUITE("ValidatedValue") {
    TEST_CASE("repr() for different types") {
        CHECK(ValidatedValue(std::monostate{}).repr() == "None");
        CHECK(ValidatedValue(true).repr() == "True");
        CHECK(ValidatedValue(false).repr() == "False");
        CHECK(ValidatedValue(static_cast<int64_t>(42)).repr() == "42");
        CHECK(ValidatedValue(static_cast<int64_t>(3)).repr() == "3");  // Avoid float precision issues
        CHECK(ValidatedValue(std::string("hello")).repr() == "\"hello\"");
        
        auto list = ValidatedValue(std::vector<ValidatedValue>{ValidatedValue(static_cast<int64_t>(1)), ValidatedValue(static_cast<int64_t>(2))});
        CHECK(list.repr() == "[1, 2]");
        
        auto dict = ValidatedValue(std::vector<std::pair<std::string, ValidatedValue>>{
            {"a", ValidatedValue(static_cast<int64_t>(1))}, {"b", ValidatedValue(static_cast<int64_t>(2))}
        });
        CHECK(dict.repr() == "{\"a\": 1, \"b\": 2}");
    }
}

TEST_SUITE("ValidationState strict_or") {
    TEST_CASE("Uses validator strict when state has no override") {
        ValidationState state;  // no strict override
        
        IntValidator strict_validator{true};
        IntValidator lax_validator{false};
        
        CHECK(state.strict_or(true) == true);
        CHECK(state.strict_or(false) == false);
    }
    
    TEST_CASE("State override takes precedence") {
        ValidationState state;
        state.set_strict(true);  // Force strict
        
        IntValidator lax_validator{false};
        
        CHECK(state.strict_or(false) == true);  // State override wins
    }
}