cmake_minimum_required(VERSION 3.15)
project(pydantic_core_cpp VERSION 2.47.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Always emit DWARF debug info: needed by the dSYM step (release) and for
# local lldb debugging (debug). Independent of whether symbols get stripped.
add_compile_options(-g)

# PDC_DEBUG=ON: unoptimized build that retains ALL symbols (local + global +
# DWARF) in the object files and the .so — nothing is stripped. Use for lldb.
option(PDC_DEBUG "Debug build: -O0 with full unstripped symbols" OFF)

if(PDC_DEBUG)
    set(PDC_OPT_FLAGS -O0)
else()
    set(PDC_OPT_FLAGS -O3)
endif()

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
# Prefer the vendored git submodule (offline builds); fall back to fetching
# from GitHub when the submodule has not been initialized.
set(SIMDJSON_SUBMODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/simdjson)
if(EXISTS ${SIMDJSON_SUBMODULE_DIR}/CMakeLists.txt)
    FetchContent_Declare(simdjson SOURCE_DIR ${SIMDJSON_SUBMODULE_DIR})
else()
    FetchContent_Declare(
        simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG v3.9.4
        EXCLUDE_FROM_ALL
    )
endif()
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
# macOS: strip binary symbols into a .dSYM bundle
# ---------------------------------------------------------------------------
# On Apple platforms, dsymutil extracts the DWARF debug info into a
# <binary>.dSYM bundle and strip removes local symbols from the shipped
# binary (exported symbols such as PyInit_* are kept).  Compile with -g so
# the dSYM contains usable debug info for symbolication (lldb, atos,
# CrashReporter).  Disable with -DPDC_STRIP_TO_DSYM=OFF; PDC_DEBUG=ON builds
# (and single-config Debug builds) are left untouched so they stay debuggable.
option(PDC_STRIP_TO_DSYM "Strip binary symbols into a .dSYM bundle (macOS)" ON)

if(APPLE AND PDC_STRIP_TO_DSYM)
    find_program(PDC_DSYMUTIL_EXECUTABLE dsymutil)
    find_program(PDC_STRIP_EXECUTABLE strip)
    if(PDC_DSYMUTIL_EXECUTABLE AND PDC_STRIP_EXECUTABLE)
        # Skip stripping for debug builds so the .so keeps all symbols
        if(PDC_DEBUG OR (NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE STREQUAL "Debug"))
            set(PDC_STRIP_ENABLED OFF)
        else()
            set(PDC_STRIP_ENABLED ON)
        endif()
        if(PDC_STRIP_ENABLED)
            function(pdc_strip_to_dsym target destination)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${PDC_DSYMUTIL_EXECUTABLE} "$<TARGET_FILE:${target}>" -o "$<TARGET_FILE:${target}>.dSYM"
                    COMMAND ${PDC_STRIP_EXECUTABLE} -x "$<TARGET_FILE:${target}>"
                    COMMENT "Stripped ${target} symbols into ${target}.dSYM"
                    VERBATIM
                )
                # Ship the dSYM bundle next to the installed binary so macOS
                # symbolication (lldb, atos, CrashReporter) finds it.  The
                # source path must NOT end in a slash, otherwise install
                # copies the bundle contents instead of the named directory.
                install(DIRECTORY "$<TARGET_FILE:${target}>.dSYM"
                        DESTINATION "${destination}")
            endfunction()
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Python extension module
# ---------------------------------------------------------------------------

set(CORE_SOURCES
    src/speedate.cpp
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
    src/url_types.cpp
    src/main_module.cpp
)

# NO_EXTRAS: skip pybind11's automatic extras — most importantly its
# POST_BUILD `strip -x`, which fires for any CMAKE_BUILD_TYPE other than
# Debug/RelWithDebInfo/None (pybind/pybind11#4454). scikit-build-core builds
# wheels as Release, so that strip removed local symbols + DWARF right after
# linking (before our dSYM step could run), leaving a stripped .so and a
# useless stub .dSYM.
pybind11_add_module(_pydantic_core_cpp NO_EXTRAS ${CORE_SOURCES})

# Re-add LTO for optimized builds so release codegen is unchanged; debug
# builds skip it (LTO defeats -O0 debugging).
if(NOT PDC_DEBUG AND NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    target_link_libraries(_pydantic_core_cpp PRIVATE pybind11::lto)
endif()

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
    ${PDC_OPT_FLAGS}
)

# Strip symbols into a .dSYM bundle (macOS, non-Debug builds) and ship the
# bundle next to the .so in the installed package (site-packages).
if(COMMAND pdc_strip_to_dsym)
    pdc_strip_to_dsym(_pydantic_core_cpp "pydantic_core_cpp")
endif()

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
    target_compile_definitions(test_errors PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN HAS_PYBIND11)
    target_link_libraries(test_errors PRIVATE doctest::doctest pybind11::module Python3::Python)
    target_include_directories(test_errors PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # Test validation state
    add_executable(test_validation_state tests/test_validation_state.cpp
        src/validation_state.cpp
        src/recursion_guard.cpp
    )
    target_compile_definitions(test_validation_state PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN HAS_PYBIND11)
    target_link_libraries(test_validation_state PRIVATE doctest::doctest pybind11::module Python3::Python)
    target_include_directories(test_validation_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # Test validators
    add_executable(test_validators tests/test_validators.cpp
        src/speedate.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/recursion_guard.cpp
        src/validation_state.cpp
        src/validator.cpp
        src/url_types.cpp
        src/schema_validator.cpp
        src/input/json_input.cpp
        src/input/string_input.cpp
        src/input/python_input.cpp
    )
    target_compile_definitions(test_validators PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN HAS_PYBIND11)
    target_link_libraries(test_validators PRIVATE doctest::doctest simdjson::simdjson pybind11::module Python3::Python)
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
    target_compile_definitions(test_serializers PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN HAS_PYBIND11)
    target_link_libraries(test_serializers PRIVATE doctest::doctest pybind11::module Python3::Python)
    target_include_directories(test_serializers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME Serializers COMMAND test_serializers)

    # Test integration
    add_executable(test_integration tests/test_integration.cpp
        src/speedate.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/recursion_guard.cpp
        src/validation_state.cpp
        src/validator.cpp
        src/serializer.cpp
        src/combined_validator.cpp
        src/schema_validator.cpp
        src/url_types.cpp
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
        src/speedate.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/recursion_guard.cpp
        src/validation_state.cpp
        src/validator.cpp
        src/combined_validator.cpp
        src/url_types.cpp
        src/schema_validator.cpp
        src/input/json_input.cpp
        src/input/string_input.cpp
        src/input/python_input.cpp
    )
    target_compile_definitions(test_model_fields PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN HAS_PYBIND11)
    target_link_libraries(test_model_fields PRIVATE doctest::doctest simdjson::simdjson pybind11::module Python3::Python)
    target_include_directories(test_model_fields PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME ModelFields COMMAND test_model_fields)

    # Constrained validators test suite
    add_executable(test_constrained_validators tests/test_constrained_validators.cpp
        src/speedate.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/recursion_guard.cpp
        src/validation_state.cpp
        src/validator.cpp
        src/combined_validator.cpp
        src/url_types.cpp
        src/schema_validator.cpp
        src/input/json_input.cpp
        src/input/string_input.cpp
        src/input/python_input.cpp
    )
    target_compile_definitions(test_constrained_validators PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN HAS_PYBIND11)
    target_link_libraries(test_constrained_validators PRIVATE doctest::doctest simdjson::simdjson pybind11::module Python3::Python)
    target_include_directories(test_constrained_validators PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME ConstrainedValidators COMMAND test_constrained_validators)

    # Test suite: New validators and features
    add_executable(test_new_validators tests/test_new_validators.cpp
        src/speedate.cpp
        src/combined_validator.cpp
        src/validator.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/url_types.cpp
        src/schema_validator.cpp
        src/input/string_input.cpp
        src/input/json_input.cpp
        src/input/python_input.cpp
    )
    target_compile_definitions(test_new_validators PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN HAS_PYBIND11)
    target_link_libraries(test_new_validators PRIVATE doctest::doctest simdjson::simdjson pybind11::module Python3::Python)
    target_include_directories(test_new_validators PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME NewValidators COMMAND test_new_validators)

    # Test suite: PythonInput (native Python object validation)
    add_executable(test_python_input tests/test_python_input.cpp
        src/speedate.cpp
        src/input/python_input.cpp
        src/errors/errors.cpp
        src/errors/error_types.cpp
        src/validation_state.cpp
        src/recursion_guard.cpp
    )
    target_compile_definitions(test_python_input PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN HAS_PYBIND11)
    target_link_libraries(test_python_input PRIVATE doctest::doctest pybind11::module Python3::Python)
    target_include_directories(test_python_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME PythonInput COMMAND test_python_input)

    # Test suite: speedate (pure C++ temporal parsing rules; no pybind11)
    add_executable(test_speedate tests/test_speedate.cpp
        src/speedate.cpp
    )
    target_link_libraries(test_speedate PRIVATE doctest::doctest)
    target_include_directories(test_speedate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_test(NAME Speedate COMMAND test_speedate)
endif()
