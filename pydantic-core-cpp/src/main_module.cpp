#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "pydantic_core/errors.hpp"
#include "pydantic_core/error_types.hpp"
#include "pydantic_core/types.hpp"

namespace py = pybind11;
using namespace pydantic_core;

std::string get_version() { return "2.46.0"; }

PYBIND11_MODULE(_pydantic_core_cpp, m) {
    m.doc() = "pydantic-core C++ implementation";
    
    m.attr("__version__") = get_version();
    
    py::enum_<InputType>(m, "InputType")
        .value("python", InputType::Python)
        .value("json", InputType::Json)
        .value("string", InputType::String);
    
    py::enum_<ExtraBehavior>(m, "ExtraBehavior")
        .value("allow", ExtraBehavior::Allow)
        .value("forbid", ExtraBehavior::Forbid)
        .value("ignore", ExtraBehavior::Ignore);
    
    py::class_<ValidationError>(m, "ValidationError")
        .def(py::init<const std::string&, InputType, const ValError&>())
        .def_property_readonly("title", &ValidationError::title)
        .def_property_readonly("error_count", &ValidationError::error_count)
        .def("__str__", &ValidationError::to_json_string);
    
    py::class_<SchemaError>(m, "SchemaError")
        .def(py::init<const std::string&>())
        .def("__str__", &SchemaError::what);
    
    py::class_<PydanticOmit>(m, "PydanticOmit")
        .def(py::init<>());
    
    py::class_<PydanticUseDefault>(m, "PydanticUseDefault")
        .def(py::init<>());
}