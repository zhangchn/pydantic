#include <doctest/doctest.h>
#include "pydantic_core/errors.hpp"
#include "pydantic_core/error_types.hpp"
#include "pydantic_core/types.hpp"

using namespace pydantic_core;

TEST_SUITE("Error Types") {

TEST_CASE("ErrorType construction") {
    SUBCASE("Basic error type") {
        ErrorType err(ErrorType::Kind::IntType);
        CHECK(err.kind() == ErrorType::Kind::IntType);
        CHECK(err.type_name() == "int_type");
        CHECK(err.message() == "Input should be a valid integer");
    }
    
    SUBCASE("String too short") {
        auto err = ErrorType(ErrorType::Kind::StringTooShort, static_cast<int64_t>(5));
        CHECK(err.kind() == ErrorType::Kind::StringTooShort);
        CHECK(err.message() == "String should have at least 5 characters");
    }
    
    SUBCASE("String too long") {
        auto err = ErrorType(ErrorType::Kind::StringTooLong, static_cast<int64_t>(100));
        CHECK(err.kind() == ErrorType::Kind::StringTooLong);
        CHECK(err.message() == "String should have at most 100 characters");
    }
    
    SUBCASE("Int greater than") {
        auto err = ErrorType(ErrorType::Kind::IntGreaterThan, static_cast<int64_t>(10));
        CHECK(err.kind() == ErrorType::Kind::IntGreaterThan);
        CHECK(err.message() == "Input should be greater than 10");
    }
    
    SUBCASE("Missing field") {
        // Missing field doesn't have context in new simplified design
        auto err = ErrorType(ErrorType::Kind::Missing);
        CHECK(err.kind() == ErrorType::Kind::Missing);
        CHECK(err.message() == "Missing field");
    }
}

TEST_CASE("PydanticKnownError shortcuts") {
    auto err = PydanticKnownError::none_required();
    CHECK(err.kind() == ErrorType::Kind::NoneRequired);
    
    auto err2 = PydanticKnownError::bool_type();
    CHECK(err2.kind() == ErrorType::Kind::BoolType);
    
    auto err3 = PydanticKnownError::int_type();
    CHECK(err3.kind() == ErrorType::Kind::IntType);
    
    auto err4 = PydanticKnownError::string_type();
    CHECK(err4.kind() == ErrorType::Kind::StringType);
}

TEST_CASE("ValError construction") {
    SUBCASE("Line error") {
        Location loc;
        loc.push("field");
        loc.push(0);
        
        auto err = ValError::line_error(PydanticKnownError::int_type(), loc, "abc");
        CHECK(err.has_line_errors());
        CHECK(!err.is_omit());
        CHECK(!err.is_use_default());
        
        auto& line_errors = err.line_errors();
        CHECK(line_errors.size() == 1);
        CHECK(line_errors[0]->error_type.kind() == ErrorType::Kind::IntType);
        CHECK(line_errors[0]->location.to_string() == "field.0");
        CHECK(line_errors[0]->input_value == "abc");
    }
    
    SUBCASE("Internal error") {
        auto err = ValError::internal_err("Something went wrong");
        CHECK(err.is_internal());
        CHECK(err.internal_message() == "Something went wrong");
    }
    
    SUBCASE("Omit error") {
        auto err = ValError::omit();
        CHECK(err.is_omit());
    }
    
    SUBCASE("Use default error") {
        auto err = ValError::use_default();
        CHECK(err.is_use_default());
    }
}

} // TEST_SUITE

TEST_SUITE("Location") {

TEST_CASE("Location construction") {
    Location loc;
    CHECK(loc.to_string() == "");
    
    loc.push("field");
    CHECK(loc.to_string() == "field");
    
    loc.push(0);
    CHECK(loc.to_string() == "field.0");
    
    loc.pop();
    CHECK(loc.to_string() == "field");
}

} // TEST_SUITE

TEST_SUITE("ValidationError") {

TEST_CASE("Single error") {
    Location loc;
    loc.push("name");
    
    auto val_err = ValError::line_error(PydanticKnownError::int_type(), loc, "'abc'");
    ValidationError validation_err("TestModel", InputType::Python, val_err);
    
    CHECK(validation_err.title() == "TestModel");
    CHECK(validation_err.input_type() == InputType::Python);
    CHECK(validation_err.error_count() == 1);
    
    auto& errors = validation_err.errors();
    CHECK(errors.size() == 1);
    CHECK(errors[0].type == "int_type");
    CHECK(errors[0].loc == "name");
}

TEST_CASE("Multiple errors") {
    auto err1 = ValError::line_error(PydanticKnownError::int_type(), Location(), "'abc'");
    auto err2 = ValError::line_error(PydanticKnownError::string_type(), Location(), "123");
    
    ValError combined(ValError::Kind::LineErrors);
    combined.merge(std::move(err1));
    combined.merge(std::move(err2));
    
    ValidationError validation_err("TestModel", InputType::Json, combined);
    CHECK(validation_err.error_count() == 2);
}

} // TEST_SUITE

TEST_SUITE("Exceptions") {

TEST_CASE("PydanticOmit") {
    PydanticOmit omit;
    CHECK(std::string(omit.what()) == "PydanticOmit");
}

TEST_CASE("PydanticUseDefault") {
    PydanticUseDefault ud;
    CHECK(std::string(ud.what()) == "PydanticUseDefault");
}

TEST_CASE("SchemaError") {
    SchemaError err("Invalid schema");
    CHECK(std::string(err.what()) == "Invalid schema");
}

} // TEST_SUITE