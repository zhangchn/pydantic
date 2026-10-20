#pragma once

#include <Python.h>
#include <datetime.h>

#include <optional>

#include <pybind11/pybind11.h>

#include "pydantic_core/py_compat.hpp"
#include "pydantic_core/speedate.hpp"

namespace py = pybind11;

namespace pydantic_core {

// Rust creates temporal results through PyDateTimeAPI, so a validated value is
// exactly datetime.date/time/datetime even when a caller has rebound those
// names in the datetime module.
inline py::object timezone_from_offset_minutes(int minutes) {
    // Rust's temporal parsers hand back a pydantic_core.TzInfo for a fixed
    // offset, not datetime.timezone, which is what makes a parsed value render
    // as tzinfo=TzInfo(9000). The wrapper owns that class, so reach for it
    // lazily and keep datetime.timezone as the fallback while it is still
    // importing.
    try {
        return py::module_::import("pydantic_core_cpp").attr("TzInfo")(minutes * 60);
    } catch (const py::error_already_set&) {
        PyErr_Clear();
    }
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

// validators/datetime.rs:83 and time.rs:56 read a value's UTC offset only where a constraint forces
// it into a speedate temporal value, not while the object is being read -- which is why a run with
// no constraint never calls utcoffset() at all, and why a tzinfo that raises is heard from at that
// moment rather than at the start of validation.  ``argument`` is what the reference hands
// utcoffset(): the datetime itself on the datetime path, None on the time path
// (datetime.rs:311 -> :361-363).  The answer is minutes, the unit this port holds offsets in
// (speedate.hpp:25); an exception the tzinfo throws travels on as the exception it is.
inline std::optional<int> python_tz_offset_minutes(const py::object& obj,
                                                   const py::object& argument) {
    if (obj.is_none() || !py_hasattr(obj, "tzinfo")) return std::nullopt;
    py::object tzinfo = obj.attr("tzinfo");
    if (tzinfo.is_none() || !py_hasattr(tzinfo, "utcoffset")) return std::nullopt;
    py::object offset = tzinfo.attr("utcoffset")(argument);
    if (offset.is_none()) return std::nullopt;
    return static_cast<int>(offset.attr("total_seconds")().cast<double>() / 60);
}

}  // namespace pydantic_core
