cmake_minimum_required(VERSION 3.15)
project(pydantic_core_cpp VERSION 2.46.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find Python
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)

# pybind11
include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
)
FetchContent_MakeAvailable(pybind11)

# simdjson for JSON parsing
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.9.4
)
FetchContent_MakeAvailable(simdjson)

# doctest for testing
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)

# Source files for the Python module
set(CORE_SOURCES
    src/errors/errors.cpp
    src/errors/error_types.cpp
    src/recursion_guard.cpp
    src/validation_state.cpp
    src/validator.cpp
    src/input/json_input.cpp
    src/input/string_input.cpp
    src/main_module.cpp
)

# Create Python module
pybind11_add_module(_pydantic_core_cpp ${CORE_SOURCES})

target_include_directories(_pydantic_core_cpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(_pydantic_core_cpp PRIVATE
    simdjson::simdjson
)

# Compile options
target_compile_options(_pydantic_core_cpp PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O3
)

# Tests
enable_testing()

# Test errors
add_executable(test_errors tests/test_errors.cpp 
    src/errors/errors.cpp 
    src/errors/error_types.cpp
)
target_compile_definitions(test_errors PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_link_libraries(test_errors PRIVATE doctest::doctest)
target_include_directories(test_errors PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Test validation state
add_executable(test_validation_state tests/test_validation_state.cpp 
    src/validation_state.cpp 
    src/recursion_guard.cpp
)
target_compile_definitions(test_validation_state PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_link_libraries(test_validation_state PRIVATE doctest::doctest)
target_include_directories(test_validation_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Test validators - Phase 2
add_executable(test_validators tests/test_validators.cpp
    src/errors/errors.cpp
    src/errors/error_types.cpp
    src/recursion_guard.cpp
    src/validation_state.cpp
    src/validator.cpp
    src/input/json_input.cpp
    src/input/string_input.cpp
)
target_compile_definitions(test_validators PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_link_libraries(test_validators PRIVATE doctest::doctest simdjson::simdjson)
target_include_directories(test_validators PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_test(NAME Errors COMMAND test_errors)
add_test(NAME ValidationState COMMAND test_validation_state)
add_test(NAME Validators COMMAND test_validators)