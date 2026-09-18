#pragma once

// Python C-API compatibility helpers for pydantic-core-cpp.

#include <Python.h>

#include <cstddef>
#include <optional>

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

// collections.deque has no static type in the C-API, so the type object is
// imported once and kept for the lifetime of the process (Rust's
// common::deque::get_deque_type).
inline const pybind11::object& py_deque_type() {
    // Leaked on purpose: destroying a cached pybind11 object during
    // interpreter shutdown aborts the process.
    static pybind11::object* deque_type = new pybind11::object(
        pybind11::module_::import("collections").attr("deque"));
    return *deque_type;
}

// The `maxlen` of a deque value: nullopt for an unbounded deque and for
// any value that is not a deque (Rust's common::deque::deque_maxlen).
inline std::optional<size_t> py_deque_maxlen(const pybind11::handle& value) {
    if (!pybind11::isinstance(value, py_deque_type())) return std::nullopt;
    try {
        pybind11::object maxlen = pybind11::getattr(value, "maxlen");
        if (maxlen.is_none()) return std::nullopt;
        return pybind11::cast<size_t>(maxlen);
    } catch (const pybind11::error_already_set&) {
        PyErr_Clear();
        return std::nullopt;
    }
}

// Build a deque holding `items`, giving it `maxlen` when the source had one
// (Rust's common::deque::new_deque).
inline pybind11::object py_deque_new(const pybind11::handle& items,
                                     const std::optional<size_t>& maxlen) {
    if (maxlen.has_value()) {
        return py_deque_type()(items, pybind11::arg("maxlen") = *maxlen);
    }
    return py_deque_type()(items);
}

// fractions.Fraction has no static type in the C-API either, so the type
// object is imported once and kept for the lifetime of the process (Rust's
// ObTypeLookup::fraction_object).
inline const pybind11::object& py_fraction_type() {
    // Leaked on purpose: destroying a cached pybind11 object during
    // interpreter shutdown aborts the process.
    static pybind11::object* fraction_type = new pybind11::object(
        pybind11::module_::import("fractions").attr("Fraction"));
    return *fraction_type;
}

// `frozendict` became a builtin on Python 3.15; below that there is no type
// to validate against (Rust's common::frozendict::get_frozendict_type).
inline bool py_has_frozendict() {
    static const bool present = [] {
        try {
            pybind11::getattr(pybind11::module_::import("builtins"), "frozendict");
            return true;
        } catch (const pybind11::error_already_set&) {
            PyErr_Clear();
            return false;
        }
    }();
    return present;
}

// Why a `frozendict` schema fails to build. Rust's FrozenDictValidator::build
// stops at get_frozendict_type; the port has no frozendict validator yet, so
// even a 3.15 interpreter gets an error rather than a half-built type.
inline const char* frozendict_build_blocker() {
    if (!py_has_frozendict()) {
        return "The `frozendict` builtin type is only available on Python 3.15 and above";
    }
    return "The `frozendict` type is not implemented in the C++ port yet";
}

}  // namespace pydantic_core
