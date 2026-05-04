#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include "pydantic_core/errors.hpp"
#include "pydantic_core/error_types.hpp"
#include "pydantic_core/types.hpp"
#include "pydantic_core/schema_validator.hpp"
#include "pydantic_core/serializer.hpp"
#include "pydantic_core/serialization_config.hpp"
#include "pydantic_core/serialization_state.hpp"

namespace py = pybind11;
using namespace pydantic_core;

std::string get_version() { return "2.46.0"; }

PYBIND11_MODULE(_pydantic_core_cpp, m) {
    m.doc() = "pydantic-core C++ implementation";
    
    m.attr("__version__") = get_version();
    
    // Enums
    py::enum_<InputType>(m, "InputType")
        .value("python", InputType::Python)
        .value("json", InputType::Json)
        .value("string", InputType::String);
    
    py::enum_<ExtraBehavior>(m, "ExtraBehavior")
        .value("allow", ExtraBehavior::Allow)
        .value("forbid", ExtraBehavior::Forbid)
        .value("ignore", ExtraBehavior::Ignore);
    
    py::enum_<StringCacheMode>(m, "StringCacheMode")
        .value("all", StringCacheMode::All)
        .value("keys", StringCacheMode::Keys)
        .value("none", StringCacheMode::None);
    
    py::enum_<SerMode>(m, "SerMode")
        .value("python", SerMode::Python)
        .value("json", SerMode::Json);
    
    py::enum_<TemporalMode>(m, "TemporalMode")
        .value("iso8601", TemporalMode::Iso8601)
        .value("seconds", TemporalMode::Seconds)
        .value("milliseconds", TemporalMode::Milliseconds);
    
    py::enum_<BytesMode>(m, "BytesMode")
        .value("utf8", BytesMode::Utf8)
        .value("base64", BytesMode::Base64)
        .value("hex", BytesMode::Hex);
    
    py::enum_<InfNanMode>(m, "InfNanMode")
        .value("null", InfNanMode::Null)
        .value("constants", InfNanMode::Constants)
        .value("strings", InfNanMode::Strings);
    
    // Error classes
    py::class_<ValidationError>(m, "ValidationError")
        .def(py::init<const std::string&, InputType, const ValError&>())
        .def_property_readonly("title", &ValidationError::title)
        .def_property_readonly("error_count", &ValidationError::error_count)
        .def("errors", &ValidationError::errors)
        .def("to_json", &ValidationError::to_json_string)
        .def("__str__", &ValidationError::to_json_string)
        .def("__repr__", [](const ValidationError& e) {
            return "ValidationError(" + e.to_json_string() + ")";
        });
    
    py::class_<SchemaError>(m, "SchemaError")
        .def(py::init<const std::string&>())
        .def("__str__", &SchemaError::what);
    
    py::class_<PydanticOmit>(m, "PydanticOmit")
        .def(py::init<>());
    
    py::class_<PydanticUseDefault>(m, "PydanticUseDefault")
        .def(py::init<>());
    
    // SchemaValidator
    py::class_<SchemaValidator>(m, "SchemaValidator")
        .def(py::init<const std::string&, const std::string&>(),
             py::arg("schema_json"),
             py::arg("config_json") = "",
             "Create a SchemaValidator from JSON schema and optional config JSON")
        .def("validate_python", [](SchemaValidator& self, const std::string& input_json,
                                   py::object strict, py::object extra) {
            std::optional<bool> strict_opt;
            if (!strict.is_none()) {
                strict_opt = strict.cast<bool>();
            }
            std::optional<ExtraBehavior> extra_opt;
            if (!extra.is_none()) {
                // Parse extra behavior from string
                std::string extra_str = extra.cast<std::string>();
                if (extra_str == "allow") extra_opt = ExtraBehavior::Allow;
                else if (extra_str == "forbid") extra_opt = ExtraBehavior::Forbid;
                else if (extra_str == "ignore") extra_opt = ExtraBehavior::Ignore;
            }
            return self.validate_python(input_json, strict_opt, extra_opt);
        }, py::arg("input_json"), py::arg("strict") = py::none(), py::arg("extra") = py::none(),
             "Validate input from Python object (as JSON)")
        .def("validate_json", [](SchemaValidator& self, const std::string& json_data,
                                 py::object strict) {
            std::optional<bool> strict_opt;
            if (!strict.is_none()) {
                strict_opt = strict.cast<bool>();
            }
            return self.validate_json(json_data, strict_opt);
        }, py::arg("json_data"), py::arg("strict") = py::none(),
             "Validate input from JSON string")
        .def("validate_strings", [](SchemaValidator& self, const std::string& string_data,
                                    py::object strict) {
            std::optional<bool> strict_opt;
            if (!strict.is_none()) {
                strict_opt = strict.cast<bool>();
            }
            return self.validate_strings(string_data, strict_opt);
        }, py::arg("string_data"), py::arg("strict") = py::none(),
             "Validate input from string mapping")
        .def("isinstance_python", [](SchemaValidator& self, const std::string& input_json,
                                     py::object strict) {
            std::optional<bool> strict_opt;
            if (!strict.is_none()) {
                strict_opt = strict.cast<bool>();
            }
            return self.isinstance_python(input_json, strict_opt);
        }, py::arg("input_json"), py::arg("strict") = py::none(),
             "Check if input is an instance of the schema")
        .def("get_default_value", [](SchemaValidator& self, py::object strict) {
            std::optional<bool> strict_opt;
            if (!strict.is_none()) {
                strict_opt = strict.cast<bool>();
            }
            return self.get_default_value(strict_opt);
        }, py::arg("strict") = py::none(),
             "Get the default value for the schema")
        .def("validate_assignment", &SchemaValidator::validate_assignment,
             py::arg("obj_json"),
             py::arg("field_name"),
             py::arg("field_value"),
             "Validate assignment to a field")
        .def_property_readonly("title", &SchemaValidator::title)
        .def("__repr__", &SchemaValidator::repr);
    
    // SerializationConfig
    py::class_<SerializationConfig>(m, "SerializationConfig")
        .def(py::init<>())
        .def_readwrite("temporal_mode", &SerializationConfig::temporal_mode)
        .def_readwrite("bytes_mode", &SerializationConfig::bytes_mode)
        .def_readwrite("inf_nan_mode", &SerializationConfig::inf_nan_mode)
        .def_static("default", &SerializationConfig::default_config);
    
    // SerializationState
    py::class_<SerializationState>(m, "SerializationState")
        .def(py::init<const SerializationConfig&>(),
             py::arg("config"),
             "Create a SerializationState with the given config")
        .def_property_readonly("config", &SerializationState::config);
}
