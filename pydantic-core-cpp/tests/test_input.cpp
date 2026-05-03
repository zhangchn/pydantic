#include <doctest/doctest.h>
#include "pydantic_core/input.hpp"
#include "pydantic_core/json_input.hpp"
#include "pydantic_core/string_input.hpp"
#include "pydantic_core/result.hpp"
#include "pydantic_core/error_types.hpp"

using namespace pydantic_core;
    
    TEST_CASE("Parse valid JSON") {
        SUBCASE("Parse string") {
            auto result = parse_json("\"hello\"");
            CHECK(result.is_ok());
            
            auto& input = result.value();
            CHECK(input->input_type() == InputType::Json);
            CHECK(!input->is_none());
        }
        
        SUBCASE("Parse number") {
            auto result = parse_json("42");
            CHECK(result.is_ok());
            
            auto& input = result.value();
            auto int_result = input->validate_int(true);
            CHECK(int_result.is_ok());
            CHECK(int_result.value().is_exact());
            CHECK(int_result.value().value().as_i64().value() == 42);
        }
        
        SUBCASE("Parse float") {
            auto result = parse_json("3.14");
            CHECK(result.is_ok());
            
            auto& input = result.value();
            auto float_result = input->validate_float(true);
            CHECK(float_result.is_ok());
            CHECK(float_result.value().as_double() == doctest::Approx(3.14));
        }
        
        SUBCASE("Parse boolean") {
            auto result_true = parse_json("true");
            CHECK(result_true.is_ok());
            auto bool_result = result_true.value()->validate_bool(true);
            CHECK(bool_result.is_ok());
            CHECK(bool_result.value().value() == true);
            
            auto result_false = parse_json("false");
            CHECK(result_false.is_ok());
            auto bool_result2 = result_false.value()->validate_bool(true);
            CHECK(bool_result2.is_ok());
            CHECK(bool_result2.value().value() == false);
        }
        
        SUBCASE("Parse null") {
            auto result = parse_json("null");
            CHECK(result.is_ok());
            CHECK(result.value()->is_none());
        }
        
        SUBCASE("Parse array") {
            auto result = parse_json("[1, 2, 3]");
            CHECK(result.is_ok());
            
            auto& input = result.value();
            auto list_result = input->validate_list(true);
            CHECK(list_result.is_ok());
            CHECK(list_result.value().value()->size() == 3);
        }
        
        SUBCASE("Parse object") {
            auto result = parse_json("{\"name\": \"test\", \"value\": 42}");
            CHECK(result.is_ok());
            
            auto& input = result.value();
            auto dict_result = input->validate_dict(true);
            CHECK(dict_result.is_ok());
            CHECK(dict_result.value()->size() == 2);
            CHECK(dict_result.value()->has_key("name"));
            CHECK(dict_result.value()->has_key("value"));
        }
    }
    
    TEST_CASE("Invalid JSON") {
        auto result = parse_json("not valid json");
        CHECK(result.is_err());
        CHECK(result.error().is_internal());
    }
    
    TEST_CASE("String validation") {
        SUBCASE("Exact string") {
            auto result = parse_json("\"hello\"");
            auto str_result = result.value()->validate_str(true);
            CHECK(str_result.is_ok());
            CHECK(str_result.value().is_exact());
            CHECK(str_result.value().value().to_string() == "hello");
        }
        
        SUBCASE("Lax string from number") {
            auto result = parse_json("42");
            auto str_result = result.value()->validate_str(false);  // lax mode
            CHECK(str_result.is_ok());
            CHECK(str_result.value().is_lax());
            CHECK(str_result.value().value().to_string() == "42");
        }
        
        SUBCASE("Strict rejection of number") {
            auto result = parse_json("42");
            auto str_result = result.value()->validate_str(true);  // strict mode
            CHECK(str_result.is_err());
        }
    }
    
    TEST_CASE("Integer validation") {
        SUBCASE("Exact integer") {
            auto result = parse_json("123");
            auto int_result = result.value()->validate_int(true);
            CHECK(int_result.is_ok());
            CHECK(int_result.value().is_exact());
            CHECK(int_result.value().value().as_i64().value() == 123);
        }
        
        SUBCASE("Lax integer from float") {
            auto result = parse_json("42.0");
            auto int_result = result.value()->validate_int(false);  // lax mode
            CHECK(int_result.is_ok());
            CHECK(int_result.value().value().as_i64().value() == 42);
        }
        
        SUBCASE("Lax integer from string") {
            auto result = parse_json("\"123\"");
            auto int_result = result.value()->validate_int(false);  // lax mode
            CHECK(int_result.is_ok());
            CHECK(int_result.value().value().as_i64().value() == 123);
        }
        
        SUBCASE("Strict rejection of string") {
            auto result = parse_json("\"123\"");
            auto int_result = result.value()->validate_int(true);  // strict mode
            CHECK(int_result.is_err());
        }
    }
    
    TEST_CASE("Float validation") {
        SUBCASE("Exact float") {
            auto result = parse_json("3.14");
            auto float_result = result.value()->validate_float(true);
            CHECK(float_result.is_ok());
            CHECK(float_result.value().as_double() == doctest::Approx(3.14));
        }
        
        SUBCASE("Float from integer") {
            auto result = parse_json("42");
            auto float_result = result.value()->validate_float(true);  // ints are valid floats
            CHECK(float_result.is_ok());
            CHECK(float_result.value().as_double() == 42.0);
        }
        
        SUBCASE("Lax float from string") {
            auto result = parse_json("\"3.14\"");
            auto float_result = result.value()->validate_float(false);  // lax mode
            CHECK(float_result.is_ok());
            CHECK(float_result.value().as_double() == doctest::Approx(3.14));
        }
    }
    
    TEST_CASE("Boolean validation") {
        SUBCASE("Exact boolean") {
            auto result = parse_json("true");
            auto bool_result = result.value()->validate_bool(true);
            CHECK(bool_result.is_ok());
            CHECK(bool_result.value().is_exact());
            CHECK(bool_result.value().value() == true);
        }
        
        SUBCASE("Lax boolean from int") {
            auto result = parse_json("1");
            auto bool_result = result.value()->validate_bool(false);  // lax mode
            CHECK(bool_result.is_ok());
            CHECK(bool_result.value().value() == true);
            
            auto result2 = parse_json("0");
            auto bool_result2 = result2.value()->validate_bool(false);
            CHECK(bool_result2.is_ok());
            CHECK(bool_result2.value().value() == false);
        }
        
        SUBCASE("Lax boolean from string") {
            auto result = parse_json("\"true\"");
            auto bool_result = result.value()->validate_bool(false);  // lax mode
            CHECK(bool_result.is_ok());
            CHECK(bool_result.value().value() == true);
        }
    }
    
    TEST_CASE("Dict validation") {
        SUBCASE("Valid dict") {
            auto result = parse_json("{\"key\": \"value\"}");
            auto dict_result = result.value()->validate_dict(true);
            CHECK(dict_result.is_ok());
            CHECK(dict_result.value()->has_key("key"));
            
            auto entry = dict_result.value()->get("key");
            CHECK(entry.has_value());
            CHECK(entry->key == "key");
        }
        
        SUBCASE("Invalid dict - not object") {
            auto result = parse_json("42");
            auto dict_result = result.value()->validate_dict(true);
            CHECK(dict_result.is_err());
        }
    }
    
    TEST_CASE("List validation") {
        SUBCASE("Valid list") {
            auto result = parse_json("[1, 2, 3]");
            auto list_result = result.value()->validate_list(true);
            CHECK(list_result.is_ok());
            CHECK(list_result.value().value()->size() == 3);
            CHECK(list_result.value().value()->entries().size() == 3);
        }
        
        SUBCASE("Invalid list - not array") {
            auto result = parse_json("42");
            auto list_result = result.value()->validate_list(true);
            CHECK(list_result.is_err());
        }
    }
}

TEST_SUITE("StringInput") {
    
    TEST_CASE("Single value") {
        SUBCASE("String validation") {
            StringInput input("hello");
            CHECK(input.is_single_value());
            CHECK(!input.is_mapping());
            
            auto str_result = input.validate_str(true);
            CHECK(str_result.is_ok());
            CHECK(str_result.value().to_string() == "hello");
        }
        
        SUBCASE("Integer parsing") {
            StringInput input("123");
            auto int_result = input.validate_int(false);
            CHECK(int_result.is_ok());
            CHECK(int_result.value().value().as_i64().value() == 123);
        }
        
        SUBCASE("Float parsing") {
            StringInput input("3.14");
            auto float_result = input.validate_float(false);
            CHECK(float_result.is_ok());
            CHECK(float_result.value().as_double() == doctest::Approx(3.14));
        }
        
        SUBCASE("Boolean parsing") {
            StringInput true_input("true");
            auto bool_result = true_input.validate_bool(false);
            CHECK(bool_result.is_ok());
            CHECK(bool_result.value().value() == true);
            
            StringInput false_input("false");
            auto bool_result2 = false_input.validate_bool(false);
            CHECK(bool_result2.is_ok());
            CHECK(bool_result2.value().value() == false);
        }
        
        SUBCASE("None detection") {
            StringInput none_input("null");
            CHECK(none_input.is_none());
            
            StringInput empty_input("");
            CHECK(empty_input.is_none());
        }
    }
    
    TEST_CASE("Mapping") {
        SUBCASE("Valid dict") {
            std::unordered_map<std::string, std::string> mapping;
            mapping["name"] = "test";
            mapping["value"] = "42";
            
            StringInput input(mapping);
            CHECK(input.is_mapping());
            CHECK(!input.is_single_value());
            
            auto dict_result = input.validate_dict(true);
            CHECK(dict_result.is_ok());
            CHECK(dict_result.value()->has_key("name"));
            CHECK(dict_result.value()->has_key("value"));
        }
        
        SUBCASE("Not a list") {
            StringInput input("value");
            auto list_result = input.validate_list(true);
            CHECK(list_result.is_err());
        }
    }
}