#pragma once

#include <Python.h>
#include <datetime.h>

#include <pybind11/pybind11.h>

#include "pydantic_core/speedate.hpp"

namespace py = pybind11;

namespace pydantic_core {

// Rust creates temporal results through PyDateTimeAPI, so a validated value is
// exactly datetime.date/time/datetime even when a caller has rebound those
// names in the datetime module.
inline py::object timezone_from_offset_minutes(int minutes) {
    py::object datetime_mod = py::module_::import("datetime");
    py::object tz_delta = datetime_mod.attr("timedelta")(py::arg("minutes") = minutes);
    return datetime_mod.attr("timezone")(tz_delta);
}

inline py::object py_date_object(const Date& date) {
    PyDateTime_IMPORT;
    PyObject* obj = PyDateTimeAPI->Date_FromDate(date.year, date.month, date.day, PyDateTimeAPI->DateType);
    if (!obj) {
        throw py::error_already_set();
    }
    return py::reinterpret_steal<py::object>(obj);
}

inline py::object py_time_object(const Time& time) {
    PyDateTime_IMPORT;
    PyObject* tz = Py_None;
    if (time.tz_offset.has_value()) {
        tz = timezone_from_offset_minutes(*time.tz_offset).release().ptr();
    }
    PyObject* obj = PyDateTimeAPI->Time_FromTime(
        time.hour, time.minute, time.second, time.microsecond, tz, PyDateTimeAPI->TimeType);
    if (time.tz_offset.has_value()) {
        Py_DECREF(tz);
    }
    if (!obj) {
        throw py::error_already_set();
    }
    return py::reinterpret_steal<py::object>(obj);
}

inline py::object py_datetime_object(const DateTime& dt) {
    PyDateTime_IMPORT;
    PyObject* tz = Py_None;
    if (dt.time.tz_offset.has_value()) {
        tz = timezone_from_offset_minutes(*dt.time.tz_offset).release().ptr();
    }
    PyObject* obj = PyDateTimeAPI->DateTime_FromDateAndTime(
        dt.date.year, dt.date.month, dt.date.day,
        dt.time.hour, dt.time.minute, dt.time.second, dt.time.microsecond,
        tz, PyDateTimeAPI->DateTimeType);
    if (dt.time.tz_offset.has_value()) {
        Py_DECREF(tz);
    }
    if (!obj) {
        throw py::error_already_set();
    }
    return py::reinterpret_steal<py::object>(obj);
}

}  // namespace pydantic_core
