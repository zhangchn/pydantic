#pragma once

// Python C-API compatibility helpers for pydantic-core-cpp.

#include <Python.h>

#include <pybind11/pybind11.h>

namespace pydantic_core {

// Attribute-existence check with Python hasattr() semantics.
//
// On Python 3.13+ this uses PyObject_GetOptionalAttrString, which silences
// AttributeError cleanly (no unraisable-exception side effects). On older
// interpreters the API does not exist, so it falls back to
// PyObject_HasAttrString.
//
// Returns true if the attribute exists, false if it does not (AttributeError
// is silenced). Throws pybind11::error_already_set if the lookup raises an
// exception other than AttributeError.
inline bool py_hasattr(PyObject* obj, const char* name) {
#if PY_VERSION_HEX >= 0x030D0000  // Python 3.13+
    PyObject* result = nullptr;
    int rc = PyObject_GetOptionalAttrString(obj, name, &result);
    if (rc == 1) {
        Py_DECREF(result);
        return true;
    }
    if (rc == -1) {
        throw pybind11::error_already_set();
    }
    return false;
#else
    int rc = PyObject_HasAttrString(obj, name);
    if (rc == -1) {
        throw pybind11::error_already_set();
    }
    return rc == 1;
#endif
}

// Overload for pybind11 object types (py::object, py::handle, ...).
inline bool py_hasattr(const pybind11::handle& obj, const char* name) {
    return py_hasattr(obj.ptr(), name);
}

}  // namespace pydantic_core
