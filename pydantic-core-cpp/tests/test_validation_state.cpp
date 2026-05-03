#include <doctest/doctest.h>
#include "pydantic_core/validation_state.hpp"
#include "pydantic_core/recursion_guard.hpp"
#include "pydantic_core/types.hpp"
#include "pydantic_core/errors.hpp"

using namespace pydantic_core;

TEST_SUITE("RecursionGuard") {
    
    TEST_CASE("Basic recursion tracking") {
        RecursionGuard guard;
        
        // Start at depth 0
        CHECK(guard.depth() == 0);
        CHECK(!guard.at_limit());
        
        // First object
        int obj1 = 42;
        CHECK(guard.check_recurse(&obj1));
        CHECK(guard.depth() == 1);
        
        // Second object
        int obj2 = 100;
        CHECK(guard.check_recurse(&obj2));
        CHECK(guard.depth() == 2);
        
        // Done with first object
        guard.done_recurse(&obj1);
        CHECK(guard.depth() == 1);
        
        // Done with second object
        guard.done_recurse(&obj2);
        CHECK(guard.depth() == 0);
    }
    
    TEST_CASE("Reject same object") {
        RecursionGuard guard;
        
        int obj = 42;
        CHECK(guard.check_recurse(&obj));
        CHECK(guard.depth() == 1);
        
        // Same object rejected
        CHECK(!guard.check_recurse(&obj));
        CHECK(guard.depth() == 1);  // Depth unchanged
        
        // Done
        guard.done_recurse(&obj);
        CHECK(guard.depth() == 0);
        
        // Can recurse again after done
        CHECK(guard.check_recurse(&obj));
        CHECK(guard.depth() == 1);
    }
    
    TEST_CASE("Respect limit") {
        RecursionGuard guard;
        
        // Create many objects
        std::vector<int> objects(RECURSION_GUARD_LIMIT + 10);
        
        // Recurse up to limit
        for (int i = 0; i < RECURSION_GUARD_LIMIT; ++i) {
            CHECK(guard.check_recurse(&objects[i]));
        }
        CHECK(guard.at_limit());
        CHECK(guard.depth() == RECURSION_GUARD_LIMIT);
        
        // Should reject beyond limit
        CHECK(!guard.check_recurse(&objects[RECURSION_GUARD_LIMIT]));
        CHECK(guard.depth() == RECURSION_GUARD_LIMIT);  // unchanged
        
        // Reset
        guard.reset();
        CHECK(guard.depth() == 0);
        CHECK(!guard.at_limit());
    }
}

TEST_SUITE("RecursionState") {
    
    TEST_CASE("RAII-style entry") {
        RecursionState state;
        int obj = 42;
        
        {
            auto entry = state.enter(&obj);
            CHECK(entry.allowed());
            CHECK(state.depth() == 1);
            
            // Entry destructor will clean up
        }
        
        CHECK(state.depth() == 0);
    }
    
    TEST_CASE("Move semantics") {
        RecursionState state;
        int obj1 = 42, obj2 = 100;
        
        auto entry1 = state.enter(&obj1);
        CHECK(entry1.allowed());
        CHECK(state.depth() == 1);
        
        // Move entry
        auto entry2 = std::move(entry1);
        CHECK(state.depth() == 1);  // Still at depth 1
        
        // entry2 destructor cleans up
    }
    
    TEST_CASE("Rejected entry") {
        RecursionState state;
        int obj = 42;
        
        // First entry succeeds
        auto entry1 = state.enter(&obj);
        CHECK(entry1.allowed());
        CHECK(state.depth() == 1);
        
        // Second entry for same object rejected
        auto entry2 = state.enter(&obj);
        CHECK(!entry2.allowed());
        CHECK(state.depth() == 1);  // unchanged
        
        // Only entry1 destructor decrements
    }
}

TEST_SUITE("ValidationState") {
    
    TEST_CASE("Basic construction") {
        ValidationState state;
        
        CHECK(!state.strict().has_value());
        CHECK(!state.extra_behavior().has_value());
        CHECK(!state.from_attributes().has_value());
        CHECK(state.cache_strings() == StringCacheMode::All);
        CHECK(state.allow_partial() == PartialMode::Off);
    }
    
    TEST_CASE("Config construction") {
        ValidationState::Config config;
        config.strict = true;
        config.extra_behavior = ExtraBehavior::Forbid;
        config.from_attributes = false;
        config.cache_strings = StringCacheMode::Keys;
        
        ValidationState state(config);
        
        CHECK(state.strict().value() == true);
        CHECK(state.extra_behavior().value() == ExtraBehavior::Forbid);
        CHECK(state.from_attributes().value() == false);
        CHECK(state.cache_strings() == StringCacheMode::Keys);
    }
    
    TEST_CASE("strict_or helper") {
        ValidationState::Config config;
        config.strict = std::nullopt;
        
        ValidationState state(config);
        
        // Use default strict value
        CHECK(state.strict_or(true) == true);
        CHECK(state.strict_or(false) == false);
        
        // Set explicit strict
        ValidationState::Config config2;
        config2.strict = true;
        ValidationState state2(config2);
        
        CHECK(state2.strict_or(false) == true);  // Explicit true overrides default
    }
    
    TEST_CASE("extra_behavior_or helper") {
        ValidationState::Config config;
        config.extra_behavior = std::nullopt;
        
        ValidationState state(config);
        
        CHECK(state.extra_behavior_or(ExtraBehavior::Ignore) == ExtraBehavior::Ignore);
        
        ValidationState::Config config2;
        config2.extra_behavior = ExtraBehavior::Allow;
        ValidationState state2(config2);
        
        CHECK(state2.extra_behavior_or(ExtraBehavior::Ignore) == ExtraBehavior::Allow);
    }
    
    TEST_CASE("Location tracking") {
        ValidationState state;
        
        state.push_loc("field");
        CHECK(state.location().to_string() == "field");
        
        state.push_loc(0);
        CHECK(state.location().to_string() == "field.0");
        
        state.pop_loc();
        CHECK(state.location().to_string() == "field");
    }
    
    TEST_CASE("Child state") {
        ValidationState::Config config;
        config.strict = true;
        
        ValidationState state(config);
        state.push_loc("root");
        
        // Create child for array element
        auto child = state.child(0);
        CHECK(child.location().to_string() == "root.0");
        CHECK(child.strict().value() == true);
        
        // Original unchanged
        CHECK(state.location().to_string() == "root");
        
        // Create child for dict key
        auto child2 = state.child("nested");
        CHECK(child2.location().to_string() == "root.nested");
    }
    
    TEST_CASE("Partial mode") {
        ValidationState::Config config;
        
        ValidationState state(config);
        CHECK(!state.is_partial());
        CHECK(!state.is_partial_trailing_strings());
        
        // With partial mode
        ValidationState state2(config, nullptr, PartialMode::On);
        CHECK(state2.is_partial());
        CHECK(!state2.is_partial_trailing_strings());
        
        // With trailing strings
        ValidationState state3(config, nullptr, PartialMode::TrailingStrings);
        CHECK(state3.is_partial());
        CHECK(state3.is_partial_trailing_strings());
    }
    
    TEST_CASE("Recursion management") {
        ValidationState state;
        
        int obj = 42;
        auto entry = state.enter_recursion(&obj);
        CHECK(entry.allowed());
        CHECK(state.recursion_depth() == 1);
        
        // Same object rejected
        auto entry2 = state.enter_recursion(&obj);
        CHECK(!entry2.allowed());
    }
    
    TEST_CASE("Field name tracking") {
        ValidationState state;
        
        CHECK(!state.field_name().has_value());
        
        state.set_field_name("test_field");
        CHECK(state.field_name().value() == "test_field");
    }
    
    TEST_CASE("Self instance tracking") {
        ValidationState state;
        
        CHECK(state.self_instance() == nullptr);
        
        int instance = 42;
        state.set_self_instance(&instance);
        CHECK(state.self_instance() == &instance);
    }
    
    TEST_CASE("Input type tracking") {
        ValidationState state;
        
        CHECK(state.input_type() == InputType::Python);
        
        state.set_input_type(InputType::Json);
        CHECK(state.input_type() == InputType::Json);
        
        state.set_input_type(InputType::String);
        CHECK(state.input_type() == InputType::String);
    }
    
    TEST_CASE("Exactness tracking") {
        ValidationState state;
        
        CHECK(state.exactness() == Exactness::Unknown);
        
        state.set_exactness(Exactness::Exact);
        CHECK(state.exactness() == Exactness::Exact);
        
        state.set_exactness(Exactness::Lax);
        CHECK(state.exactness() == Exactness::Lax);
    }
}