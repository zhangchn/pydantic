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

// ---------------------------------------------------------------------------
// Lazy-loaded reference to the Rust pydantic_core to_json function.
// ---------------------------------------------------------------------------
static py::object& _get_rust_to_json() {
    static bool initialized = false;
    static py::object cached = py::none();
    if (initialized) {
        return cached;
    }
    initialized = true;
    try {
        py::object rust_ext = py::module_::import("pydantic_core._pydantic_core");
        cached = rust_ext.attr("to_json");
    } catch (const py::error_already_set&) {
        // Rust backend not available — cached stays as py::none()
    }
    return cached;
}

// ---------------------------------------------------------------------------
// Helper: convert a py::object (dict, string, etc.) to a JSON string.
// Uses the Rust pydantic_core's to_json if available, since the schema
// may contain non-serializable Python objects (classes, functions, etc.).
// ---------------------------------------------------------------------------
static std::string pyobj_to_json(const py::object& obj) {
    // Always use json.dumps to properly serialize all Python types
    // (including strings which need quoting)
    py::object json_mod = py::module_::import("json");
    try {
        return json_mod.attr("dumps")(obj).cast<std::string>();
    } catch (const py::error_already_set&) {
        // Standard json failed — try Rust backend's to_json
        py::object to_json_fn = _get_rust_to_json();
        if (!to_json_fn.is_none()) {
            try {
                py::bytes result = to_json_fn(obj);
                return result.cast<std::string>();
            } catch (const py::error_already_set&) {
                // Rust to_json also failed — continue to fallback
            }
        }
        // Last resort: json.dumps with repr fallback for unknown types
        auto default_fn = py::cpp_function([](py::handle o) -> py::str {
            return py::str(py::repr(o));
        });
        return json_mod.attr("dumps")(obj, py::arg("default") = default_fn).cast<std::string>();
    }
}

// ---------------------------------------------------------------------------
// Helper: convert a JSON string back to a Python object
// ---------------------------------------------------------------------------
static py::object json_to_pyobj(const std::string& json_str) {
    py::object json_mod = py::module_::import("json");
    return json_mod.attr("loads")(json_str);
}

// ---------------------------------------------------------------------------
// Helper: extract ExtraBehavior from a Python object
// ---------------------------------------------------------------------------
static std::optional<ExtraBehavior> pyobj_to_extra(const py::object& obj) {
    if (obj.is_none()) return std::nullopt;
    if (py::isinstance<py::str>(obj)) {
        std::string s = obj.cast<std::string>();
        if (s == "allow") return ExtraBehavior::Allow;
        if (s == "forbid") return ExtraBehavior::Forbid;
        if (s == "ignore") return ExtraBehavior::Ignore;
    }
    return std::nullopt;
}

static std::optional<bool> pyobj_to_bool(const py::object& obj) {
    if (obj.is_none()) return std::nullopt;
    return obj.cast<bool>();
}

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
        .def(py::init([](const py::object& schema, const py::object& config) {
            std::string schema_json = pyobj_to_json(schema);
            std::string config_json = config.is_none() ? "" : pyobj_to_json(config);
            return std::make_unique<SchemaValidator>(schema_json, config_json);
        }), py::arg("schema"), py::arg("config") = py::none(),
             "Create a SchemaValidator from a CoreSchema dict or JSON string")
        // Overload that accepts _use_prebuilt kwarg (passed by pydantic plugin system)
        .def(py::init([](const py::object& schema, const py::object& config, bool /*_use_prebuilt*/) {
            std::string schema_json = pyobj_to_json(schema);
            std::string config_json = config.is_none() ? "" : pyobj_to_json(config);
            return std::make_unique<SchemaValidator>(schema_json, config_json);
        }), py::arg("schema"), py::arg("config") = py::none(), py::arg("_use_prebuilt") = true,
             "Create a SchemaValidator from a CoreSchema dict or JSON string")
        .def("validate_python", [](SchemaValidator& self, const py::object& input,
                                   py::object strict, py::object context,
                                   py::object self_instance) -> py::object {
            std::string input_json = pyobj_to_json(input);
            std::string result = self.validate_python(
                input_json, pyobj_to_bool(strict), std::nullopt);

            // If a self_instance was provided (pydantic model construction),
            // populate its __dict__ with the validated data and return the instance.
            if (!self_instance.is_none() && py::hasattr(self_instance, "__dict__")) {
                py::dict validated = json_to_pyobj(result).cast<py::dict>();
                py::dict instance_dict = self_instance.attr("__dict__");
                // Merge validated data into the instance __dict__
                for (auto item : validated) {
                    instance_dict[item.first] = item.second;
                }
                return self_instance;
            }
            return json_to_pyobj(result);
        }, py::arg("object"), py::arg("strict") = py::none(), py::arg("context") = py::none(),
             py::arg("self_instance") = py::none(),
             "Validate a Python object")
        .def("validate_json", [](SchemaValidator& self, const py::object& json_data,
                                 py::object strict) {
            std::string json_str = py::isinstance<py::bytes>(json_data)
                ? json_data.cast<std::string>()
                : json_data.cast<std::string>();
            std::string result = self.validate_json(json_str, pyobj_to_bool(strict));
            return json_to_pyobj(result);
        }, py::arg("json_data"), py::arg("strict") = py::none(),
             "Validate input from JSON string")
        .def("validate_strings", [](SchemaValidator& self, const py::object& string_data,
                                    py::object strict) {
            std::string str = pyobj_to_json(string_data);
            std::string result = self.validate_strings(str, pyobj_to_bool(strict));
            return json_to_pyobj(result);
        }, py::arg("string_data"), py::arg("strict") = py::none(),
             "Validate input from string mapping")
        .def("isinstance_python", [](SchemaValidator& self, const py::object& input,
                                     py::object strict) {
            std::string input_json = pyobj_to_json(input);
            return self.isinstance_python(input_json, pyobj_to_bool(strict));
        }, py::arg("object"), py::arg("strict") = py::none(),
             "Check if input is an instance of the schema")
        .def("get_default_value", [](SchemaValidator& self, py::object strict) -> py::object {
            auto result = self.get_default_value(pyobj_to_bool(strict));
            if (result) {
                return json_to_pyobj(*result);
            }
            return py::none();
        }, py::arg("strict") = py::none(),
             "Get the default value for the schema")
        .def("validate_assignment", [](SchemaValidator& self,
                                       const py::object& obj,
                                       const std::string& field_name,
                                       const py::object& field_value) {
            std::string obj_json = pyobj_to_json(obj);
            std::string val_json = pyobj_to_json(field_value);
            std::string result = self.validate_assignment(obj_json, field_name, val_json);
            return json_to_pyobj(result);
        }, py::arg("object"), py::arg("field_name"), py::arg("field_value"),
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
