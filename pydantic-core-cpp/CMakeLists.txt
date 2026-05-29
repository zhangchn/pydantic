cmake_minimum_required(VERSION 3.15)
project(pydantic_core_cpp VERSION 2.47.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

include(FetchContent)

# When built via scikit-build-core, pybind11 is provided by the build frontend.
# For standalone CMake builds (e.g. running tests directly), fetch it ourselves.

# Python + pybind11
if(SKBUILD)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
    find_package(pybind11 CONFIG REQUIRED)
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# simdjson for JSON parsing (build-only dependency, don't install into wheel)
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.9.4
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(simdjson)

# doctest for testing (only for standalone builds)
if(NOT SKBUILD)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
    )
    FetchContent_MakeAvailable(doctest)
endif()

# ---------------------------------------------------------------------------
# Python extension module
# ---------------------------------------------------------------------------

set(CORE_SOURCES
    src/errors/errors.cpp
    src/errors/error_types.cpp
    src/recursion_guard.cpp
    src/validation_state.cpp
    src/validator.cpp
    src/serializer.cpp
    src/combined_validator.cpp
    src/schema_validator.cpp
    src/input/json_input.cpp
    src/input/string_input.cpp
    src/input/python_input.cpp
    src/main_module.cpp
)

pybind11_add_module(_pydantic_core_cpp ${CORE_SOURCES})

target_include_directories(_pydantic_core_cpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(_pydantic_core_cpp PRIVATE HAS_PYBIND11)

target_link_libraries(_pydantic_core_cpp PRIVATE
    simdjson::simdjson
)

target_compile_options(_pydantic_core_cpp PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O3
)

# Install the extension into the Python package directory for wheel building
install(TARGETS _pydantic_core_cpp LIBRARY DESTINATION pydantic_core_cpp)

# ---------------------------------------------------------------------------
# Tests (only for standalone builds, not during wheel building)
# ---------------------------------------------------------------------------

if(NOT SKBUILD)
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

    # Test validators
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

    # Test serializers
    add_executable(test_serializers tests/test_serializers.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/recursion_guard.cpp
        src/serializer.cpp
    )
    target_compile_definitions(test_serializers PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
    target_link_libraries(test_serializers PRIVATE doctest::doctest)
    target_include_directories(test_serializers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME Serializers COMMAND test_serializers)

    # Test integration
    add_executable(test_integration tests/test_integration.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/recursion_guard.cpp
        src/validation_state.cpp
        src/validator.cpp
        src/serializer.cpp
        src/combined_validator.cpp
        src/schema_validator.cpp
        src/input/json_input.cpp
        src/input/string_input.cpp
        src/input/python_input.cpp
    )
    target_compile_definitions(test_integration PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN HAS_PYBIND11)
    target_link_libraries(test_integration PRIVATE doctest::doctest simdjson::simdjson pybind11::module Python3::Python)
    target_include_directories(test_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME Integration COMMAND test_integration)

    # Test model/typed-dict/dataclass validators
    add_executable(test_model_fields tests/test_model_fields.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/recursion_guard.cpp
        src/validation_state.cpp
        src/validator.cpp
        src/combined_validator.cpp
        src/input/json_input.cpp
        src/input/string_input.cpp
    )
    target_compile_definitions(test_model_fields PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
    target_link_libraries(test_model_fields PRIVATE doctest::doctest simdjson::simdjson)
    target_include_directories(test_model_fields PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME ModelFields COMMAND test_model_fields)

    # Constrained validators test suite
    add_executable(test_constrained_validators tests/test_constrained_validators.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/recursion_guard.cpp
        src/validation_state.cpp
        src/validator.cpp
        src/combined_validator.cpp
        src/input/json_input.cpp
        src/input/string_input.cpp
    )
    target_compile_definitions(test_constrained_validators PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
    target_link_libraries(test_constrained_validators PRIVATE doctest::doctest simdjson::simdjson)
    target_include_directories(test_constrained_validators PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME ConstrainedValidators COMMAND test_constrained_validators)

    # Test suite: New validators and features
    add_executable(test_new_validators tests/test_new_validators.cpp
        src/combined_validator.cpp
        src/validator.cpp
        src/errors/error_types.cpp
        src/input/string_input.cpp
        src/input/json_input.cpp
    )
    target_compile_definitions(test_new_validators PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
    target_link_libraries(test_new_validators PRIVATE doctest::doctest simdjson::simdjson)
    target_include_directories(test_new_validators PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME NewValidators COMMAND test_new_validators)

    # Test suite: PythonInput (native Python object validation)
    add_executable(test_python_input tests/test_python_input.cpp
        src/input/python_input.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/validation_state.cpp
        src/recursion_guard.cpp
    )
    target_compile_definitions(test_python_input PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
    target_link_libraries(test_python_input PRIVATE doctest::doctest pybind11::module Python3::Python)
    target_include_directories(test_python_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME PythonInput COMMAND test_python_input)
endif()
