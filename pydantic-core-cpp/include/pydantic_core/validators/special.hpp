#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/py_compat.hpp"
#include "pydantic_core/url_types.hpp"
#include "pydantic_core/py_time.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <pybind11/pybind11.h>
#include <cstdio>
#include <stdexcept>
#include <regex>
#include <algorithm>
#include <cctype>

namespace py = pybind11;
namespace pydantic_core {

// DefinitionsRegistry - stores named validators for recursive schema resolution
// Uses std::shared_ptr<Validator> to avoid circular dependency with CombinedValidator
class DefinitionsRegistry {
public:
    using ValidatorPtr = std::shared_ptr<Validator>;

    void add_definition(const std::string& ref, ValidatorPtr validator) {
        pending_.erase(ref);
        definitions_[ref] = std::move(validator);
    }

    // Rust's DefinitionsBuilder hands out a reference to a definition that is
    // still being built; its name is not known yet, so it renders as "...".
    void add_placeholder(const std::string& ref, ValidatorPtr stub) {
        pending_.insert(ref);
        definitions_[ref] = std::move(stub);
    }

    bool is_pending(const std::string& ref) const { return pending_.count(ref) != 0; }

    ValidatorPtr get_definition(const std::string& ref) const {
        auto it = definitions_.find(ref);
        if (it != definitions_.end()) {
            return it->second;
        }
        return nullptr;
    }

    bool has_definition(const std::string& ref) const {
        return definitions_.find(ref) != definitions_.end();
    }

private:
    std::unordered_map<std::string, ValidatorPtr> definitions_;
    std::unordered_set<std::string> pending_;
};

// DefinitionRefValidator - resolves recursive schema references
class DefinitionRefValidator : public Validator {
public:
    DefinitionRefValidator() = default;
    
    void set_ref(const std::string& ref) {
        schema_ref_ = ref;
    }
    
    void set_definitions(std::shared_ptr<DefinitionsRegistry> definitions) {
        definitions_ = std::move(definitions);
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (definitions_) {
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                return def->validate(input, state);
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "Definition reference not found: " + schema_ref_
        );
    }

    std::string name() const override {
        if (definitions_) {
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                return def->name();
            }
        }
        return "definition-ref";
    }

    std::string effective_result_name() const override {
        if (definitions_) {
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                return def->effective_result_name();
            }
        }
        return "definition-ref";
    }

    // The referenced validator's own name. A definition reference is the only
    // way a schema can cycle, so this is also where the recursion marker comes
    // from: a reference already being named further out renders as "...",
    // matching Rust's not-yet-built definition.
    std::string display_name() const override {
        if (definitions_) {
            if (definitions_->is_pending(schema_ref_)) return "...";
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                display_name_detail::Guard g(def.get());
                if (g.duplicate) return "...";
                return def->display_name();
            }
        }
        return "...";
    }

    // Delegate expected-class lookup to the resolved definition (e.g. unions
    // use this to prefer the exact-class branch for model instance inputs).
    const py::object& expected_class() const override {
        if (definitions_) {
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                return def->expected_class();
            }
        }
        static const py::object none = py::none();
        return none;
    }

    const std::string& get_ref() const { return schema_ref_; }

private:
    std::string schema_ref_;
    std::shared_ptr<DefinitionsRegistry> definitions_;
};

// ---- Helpers for date/time/datetime constraint & now_op & tz_constraint ----
// Convert a C++ Date to a Python datetime.date
static py::object date_to_python_obj(const Date& d) {
    return py_date_object(d);
}

// Convert a C++ DateTime to a Python datetime.datetime
static py::object datetime_to_python_obj(const DateTime& dt) {
    return py_datetime_object(dt);
}

// Convert a C++ Time to a Python datetime.time
static py::object time_to_python_obj(const Time& t) {
    return py_time_object(t);
}

// Parse a constraint value (ISO string or already a Python object) into a
// comparable Python object of the given kind ("date"/"datetime"/"time").
static py::object parse_temporal_constraint(const py::object& val, const std::string& kind) {
    if (py::isinstance<py::str>(val)) {
        std::string s = val.cast<std::string>();
        py::object datetime_mod = py::module_::import("datetime");
        if (kind == "date") return datetime_mod.attr("date").attr("fromisoformat")(s);
        if (kind == "datetime") return datetime_mod.attr("datetime").attr("fromisoformat")(s);
        if (kind == "time") return datetime_mod.attr("time").attr("fromisoformat")(s);
    }
    return val;
}

// Apply gt/lt/ge/le constraints to a Python temporal object. Returns a
// ValError on the first violated constraint, or nullopt if all pass.
static std::optional<ValError> apply_temporal_constraints(
    const py::object& result,
    const py::object& gt, const py::object& lt,
    const py::object& ge, const py::object& le,
    const std::string& kind,
    const Input& input,
    ValidationState& state) {
    auto py_cmp = [](const py::object& a, const py::object& b, int op) -> bool {
        PyObject* r = PyObject_RichCompare(a.ptr(), b.ptr(), op);
        if (!r) return false;
        bool out = (r != Py_False);
        Py_DECREF(r);
        return out;
    };
    try {
        if (!gt.is_none()) {
            auto c = parse_temporal_constraint(gt, kind);
            if (!py_cmp(result, c, Py_GT)) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::GreaterThan, "gt", py::str(gt).cast<std::string>()),
                    state.location(), input.as_error_value().repr);
            }
        }
        if (!lt.is_none()) {
            auto c = parse_temporal_constraint(lt, kind);
            if (!py_cmp(result, c, Py_LT)) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::LessThan, "lt", py::str(lt).cast<std::string>()),
                    state.location(), input.as_error_value().repr);
            }
        }
        if (!ge.is_none()) {
            auto c = parse_temporal_constraint(ge, kind);
            if (!py_cmp(result, c, Py_GE)) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::GreaterThanEqual, "ge", py::str(ge).cast<std::string>()),
                    state.location(), input.as_error_value().repr);
            }
        }
        if (!le.is_none()) {
            auto c = parse_temporal_constraint(le, kind);
            if (!py_cmp(result, c, Py_LE)) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::LessThanEqual, "le", py::str(le).cast<std::string>()),
                    state.location(), input.as_error_value().repr);
            }
        }
    } catch (py::error_already_set& e) {
        e.restore();
        PyErr_Clear();
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Date/datetime cross-retries
// ---------------------------------------------------------------------------
// pydantic-core lets a date and a datetime schema fall back to each other
// before giving up, and reclassifies the parsing error accordingly. Doing the
// same here is what makes `date_from_datetime_parsing` and
// `datetime_from_date_parsing` appear instead of the plain parsing errors.

// Rewrites a parsing line error to `reclassified` in place, keeping its
// document text. Returns false when no parsing error was present.
inline bool reclassify_parsing_error(ValError& error, ErrorType::Kind from,
                                     ErrorType::Kind to) {
    if (!error.has_line_errors()) return false;
    bool found = false;
    for (const auto& line : error.line_errors()) {
        if (line->error_type.kind() != from) continue;
        auto it = line->error_type.context().find("error");
        std::string document = it != line->error_type.context().end() ? it->second : std::string();
        line->error_type = ErrorType(to, "error", std::move(document));
        found = true;
    }
    return found;
}

// Outcome of a cross-retry: either the value recovered from the other type, an
// error that replaces the original one, or neither (keep the original errors).
template <class T>
struct CrossRetry {
    std::optional<T> value;
    std::optional<ValError> error;
};

// A datetime at midnight is a valid date; anything else is
// date_from_datetime_inexact. Mirrors date.rs::date_from_datetime.
inline CrossRetry<EitherDate> date_from_datetime(const Input& input, TimestampUnit unit) {
    CrossRetry<EitherDate> out;
    auto dt_result = input.validate_datetime(false, unit);
    if (dt_result.is_err()) {
        ValError& error = dt_result.error();
        if (reclassify_parsing_error(error, ErrorType::Kind::DateTimeParsing,
                                     ErrorType::Kind::DateFromDatetimeParsing)) {
            out.error = std::move(error);
        }
        return out;
    }
    DateTime dt = dt_result.value().value().value;
    Time zero{0, 0, 0, 0, dt.time.tz_offset};
    if (dt.time == zero) {
        EitherDate either_date(dt.date);
        either_date.is_lax = true;
        out.value = either_date;
    } else {
        out.error = ValError::line_error(
            ErrorType(ErrorType::Kind::DateFromDatetimeInexact),
            input.current_location(), input.as_error_value().repr);
    }
    return out;
}

// A bare date is a valid datetime extended to midnight. Mirrors
// datetime.rs::datetime_from_date, including its use of the default
// (infer) unit rather than the configured one.
inline CrossRetry<EitherDateTime> datetime_from_date(const Input& input) {
    CrossRetry<EitherDateTime> out;
    auto date_result = input.validate_date(false, TimestampUnit::Infer);
    if (date_result.is_err()) {
        ValError& error = date_result.error();
        if (reclassify_parsing_error(error, ErrorType::Kind::DateParsing,
                                     ErrorType::Kind::DatetimeFromDateParsing)) {
            out.error = std::move(error);
        }
        return out;
    }
    out.value = EitherDateTime(DateTime{date_result.value().value().value,
                                        Time{0, 0, 0, 0, std::nullopt}});
    return out;
}

// DateValidator - validates date values
class DateValidator : public Validator {
public:
    explicit DateValidator(bool strict = false) : strict_(strict) {}

    py::object gt = py::none();
    py::object lt = py::none();
    py::object ge = py::none();
    py::object le = py::none();
    std::string now_op;  // "", "past", "future"

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        bool strict = state.strict_or(strict_);
        auto result = input.validate_date(strict, state.val_temporal_unit());
        if (result.is_err()) {
            if (!strict) {
                auto retry = date_from_datetime(input, state.val_temporal_unit());
                if (retry.value) {
                    result = ValMatch<EitherDate>::lax(std::move(*retry.value));
                } else if (retry.error) {
                    return std::move(*retry.error);
                } else {
                    return result.error();
                }
            } else {
                return result.error();
            }
        }
        auto match = std::move(result.value());
        Date d = match.value().value;

        // Python has no year 0, so pydantic-core turns it into a parsing error
        // instead of letting the datetime constructor raise.
        if (d.year == 0) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateParsing, "error", "year 0 is out of range"),
                input.current_location(), input.as_error_value().repr);
        }

        // Apply gt/lt/ge/le constraints
        try {
            py::object py_date = date_to_python_obj(d);
            auto err = apply_temporal_constraints(py_date, gt, lt, ge, le, "date", input, state);
            if (err) return *err;

            // Apply now_op (past/future) check
            if (!now_op.empty()) {
                py::object datetime_mod = py::module_::import("datetime");
                py::object today = datetime_mod.attr("date").attr("today")();
                auto py_cmp = [](const py::object& a, const py::object& b, int op) -> bool {
                    PyObject* r = PyObject_RichCompare(a.ptr(), b.ptr(), op);
                    if (!r) return false;
                    bool out = (r != Py_False);
                    Py_DECREF(r);
                    return out;
                };
                if (now_op == "past" && !py_cmp(py_date, today, Py_LT)) {
                    return ValError::line_error(ErrorType(ErrorType::Kind::DatePast),
                        state.location(), input.as_error_value().repr);
                }
                if (now_op == "future" && !py_cmp(py_date, today, Py_GT)) {
                    return ValError::line_error(ErrorType(ErrorType::Kind::DateFuture),
                        state.location(), input.as_error_value().repr);
                }
            }
        } catch (py::error_already_set& e) {
            e.restore();
            PyErr_Clear();
        }

        // Store as internal Date value (will be converted to Python datetime.date by result_to_python)
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherDate>(std::move(match.value()))
        );
    }

    std::string name() const override { return "date"; }

private:
    bool strict_ = false;
};

// TimeValidator - validates time values
class TimeValidator : public Validator {
public:
    explicit TimeValidator(bool strict = false) : strict_(strict) {}

    py::object gt = py::none();
    py::object lt = py::none();
    py::object ge = py::none();
    py::object le = py::none();

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_time(state.strict_or(strict_));
        if (result.is_err()) {
            return result.error();
        }
        auto match = std::move(result.value());
        Time t = match.value().value;

        try {
            py::object py_time = time_to_python_obj(t);
            auto err = apply_temporal_constraints(py_time, gt, lt, ge, le, "time", input, state);
            if (err) return *err;
        } catch (py::error_already_set& e) {
            e.restore();
            PyErr_Clear();
        }

        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherTime>(std::move(match.value()))
        );
    }

    std::string name() const override { return "time"; }

private:
    bool strict_ = false;
};

// DatetimeValidator - validates datetime values
class DatetimeValidator : public Validator {
public:
    explicit DatetimeValidator(bool strict = false) : strict_(strict) {}

    py::object gt = py::none();
    py::object lt = py::none();
    py::object ge = py::none();
    py::object le = py::none();
    std::string now_op;        // "", "past", "future"
    std::string tz_constraint; // "", "aware", "naive"

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        bool strict = state.strict_or(strict_);
        auto result = input.validate_datetime(strict, state.val_temporal_unit());
        if (result.is_err()) {
            if (!strict) {
                auto retry = datetime_from_date(input);
                if (retry.value) {
                    result = ValMatch<EitherDateTime>::lax(std::move(*retry.value));
                } else if (retry.error) {
                    return std::move(*retry.error);
                } else {
                    return result.error();
                }
            } else {
                return result.error();
            }
        }
        auto match = std::move(result.value());
        DateTime dt = match.value().value;

        if (dt.date.year == 0) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DateTimeParsing, "error", "year 0 is out of range"),
                input.current_location(), input.as_error_value().repr);
        }

        try {
            py::object py_dt = datetime_to_python_obj(dt);
            auto py_cmp = [](const py::object& a, const py::object& b, int op) -> bool {
                PyObject* r = PyObject_RichCompare(a.ptr(), b.ptr(), op);
                if (!r) return false;
                bool out = (r != Py_False);
                Py_DECREF(r);
                return out;
            };

            // Apply gt/lt/ge/le constraints
            auto err = apply_temporal_constraints(py_dt, gt, lt, ge, le, "datetime", input, state);
            if (err) return *err;

            // Apply tz_constraint (aware/naive)
            bool is_aware = dt.time.tz_offset.has_value();
            if (tz_constraint == "aware" && !is_aware) {
                return ValError::line_error(ErrorType(ErrorType::Kind::TimezoneAware),
                    state.location(), input.as_error_value().repr);
            }
            if (tz_constraint == "naive" && is_aware) {
                return ValError::line_error(ErrorType(ErrorType::Kind::TimezoneNaive),
                    state.location(), input.as_error_value().repr);
            }

            // Apply now_op (past/future) check
            if (!now_op.empty()) {
                py::object datetime_mod = py::module_::import("datetime");
                py::object now;
                if (is_aware) {
                    py::object utc = datetime_mod.attr("timezone").attr("utc");
                    now = datetime_mod.attr("datetime").attr("now")(utc);
                } else {
                    now = datetime_mod.attr("datetime").attr("now")();
                }
                if (now_op == "past" && !py_cmp(py_dt, now, Py_LT)) {
                    return ValError::line_error(ErrorType(ErrorType::Kind::DatetimePast),
                        state.location(), input.as_error_value().repr);
                }
                if (now_op == "future" && !py_cmp(py_dt, now, Py_GT)) {
                    return ValError::line_error(ErrorType(ErrorType::Kind::DatetimeFuture),
                        state.location(), input.as_error_value().repr);
                }
            }
        } catch (py::error_already_set& e) {
            e.restore();
            PyErr_Clear();
        }

        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherDateTime>(std::move(match.value()))
        );
    }

    std::string name() const override { return "datetime"; }

private:
    bool strict_ = false;
};

// TimedeltaValidator - validates timedelta values
class TimedeltaValidator : public Validator {
public:
    explicit TimedeltaValidator(bool strict = false) : strict_(strict) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_timedelta(state.strict_or(strict_));
        if (result.is_err()) {
            return result.error();
        }
        auto match = std::move(result.value());
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherTimedelta>(std::move(match.value()))
        );
    }

    std::string name() const override { return "timedelta"; }

private:
    bool strict_ = false;
};

// UrlValidator - validates URL values
class UrlValidator : public Validator {
public:
    std::optional<size_t> max_length;
    std::vector<std::string> allowed_schemes;
    bool preserve_empty_path = false;
    std::optional<bool> host_required;
    std::optional<std::string> default_host;
    std::optional<int> default_port;
    std::optional<std::string> default_path;
    bool strict = false;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Accept an already-validated Url object (Rust downcast_python_input::<PyUrl>)
        py::object input_py = input.as_python_object();
        try {
            py::object mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
            py::object url_class = mod.attr("Url");
            if (py::isinstance(input_py, url_class)) {
                if (max_length.has_value()) {
                    std::string s = py::str(input_py).cast<std::string>();
                    if (s.size() > max_length.value()) {
                        ErrorType err(ErrorType::Kind::UrlTooLong);
                        err.context()["max_length"] = std::to_string(max_length.value());
                        err.context()["s"] = max_length.value() == 1 ? "" : "s";
                        return ValError::line_error(err, state.location(),
                                                    input.as_error_value().repr);
                    }
                }
                UrlDefaults dflt;
                dflt.host = default_host;
                dflt.port = default_port;
                dflt.path = default_path;
                auto re = std::make_shared<Url>(py::str(input_py).cast<std::string>(),
                                                preserve_empty_path, dflt);
                return ValResult<std::shared_ptr<void>>(
                    std::static_pointer_cast<void>(re));
            }
        } catch (...) {}

        if (!py::isinstance<py::str>(input_py) && !py::isinstance<py::bytes>(input_py)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::UrlType),
                state.location(),
                input.as_error_value().repr
            );
        }

        // Lax bytes -> str coercion (matches input.validate_str).
        std::string raw = py::cast<std::string>(input_py);
        bool raw_empty = raw.empty();
        std::string url_str = raw;
        // url crate trims leading/trailing C0 control + space before parsing.
        strip_url_whitespace(url_str);

        // Length check happens BEFORE parsing (Rust UrlValidator::check_length)
        if (max_length.has_value() && url_str.size() > max_length.value()) {
            ErrorType err(ErrorType::Kind::UrlTooLong);
            err.context()["max_length"] = std::to_string(max_length.value());
            err.context()["s"] = max_length.value() == 1 ? "" : "s";
            return ValError::line_error(err, state.location(), url_str);
        }

        auto url_parsing_err = [&](const std::string& error) {
            ErrorType err(ErrorType::Kind::UrlParsing);
            err.context()["error"] = error;
            return ValError::line_error(err, state.location(), url_str);
        };
        auto syntax_violation_err = [&](const std::string& error) {
            ErrorType err(ErrorType::Kind::UrlSyntaxViolation);
            err.context()["error"] = error;
            return ValError::line_error(err, state.location(), url_str);
        };
        auto scheme_err = [&]() {
            ErrorType err(ErrorType::Kind::UrlScheme);
            std::string expected;
            for (size_t i = 0; i < allowed_schemes.size(); ++i) {
                if (i > 0) expected += i == allowed_schemes.size() - 1 ? " or " : ", ";
                expected += "'" + allowed_schemes[i] + "'";
            }
            err.context()["expected_schemes"] = expected;
            return ValError::line_error(err, state.location(), url_str);
        };

        const bool strict = state.strict_or(this->strict);

        // Empty input: url crate checks the raw input before trimming.
        if (raw_empty) {
            return url_parsing_err("input is empty");
        }
        // Strict mode surfaces leading/trailing whitespace as a syntax violation.
        if (strict && url_str != raw) {
            return syntax_violation_err("leading or trailing control or space character are ignored in URLs");
        }
        if (url_str.empty()) {
            return url_parsing_err("relative URL without a base");
        }

        // Scheme must start with an ASCII letter (url crate); otherwise no base.
        std::regex scheme_re("^([a-zA-Z][a-zA-Z0-9+.-]*):");
        std::smatch sm;
        if (!std::regex_search(url_str, sm, scheme_re)) {
            return url_parsing_err("relative URL without a base");
        }
        std::string scheme = sm[1].str();
        for (char& ch : scheme) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::string rest = url_str.substr(sm[0].length());
        bool special = is_special_scheme(scheme);

        // Authority region (optional userinfo@host[:port]), terminated by / ? #
        std::string authority;
        if (special) {
            size_t k = 0;
            while (k < rest.size() && (rest[k] == '/' || rest[k] == '\\')) ++k;
            // special schemes require exactly "//"; extra/missing slashes are a strict violation
            if (strict && k != 2) {
                return syntax_violation_err("expected //");
            }
            std::string after = rest.substr(k);
            size_t e = after.find_first_of("/?#");
            authority = (e == std::string::npos) ? after : after.substr(0, e);
        } else if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
            std::string after = rest.substr(2);
            size_t e = after.find_first_of("/?#");
            authority = (e == std::string::npos) ? after : after.substr(0, e);
        }

        // Validate the authority (userinfo@host[:port]) when there is one.
        if (!authority.empty() || special) {
            std::string hostport = authority;
            size_t at = hostport.rfind('@');
            if (at != std::string::npos) hostport = hostport.substr(at + 1);

            std::string host;
            std::string port_str;
            bool has_port = false;
            if (!hostport.empty() && hostport[0] == '[') {
                size_t be = hostport.find(']');
                if (be == std::string::npos) {
                    return url_parsing_err("invalid IPv6 address");
                }
                std::string inner = hostport.substr(1, be - 1);
                if (!is_valid_ipv6(inner)) {
                    return url_parsing_err("invalid IPv6 address");
                }
                host = hostport.substr(0, be + 1);
                std::string r = hostport.substr(be + 1);
                if (!r.empty()) {
                    if (r[0] != ':') return url_parsing_err("invalid port number");
                    port_str = r.substr(1);
                    has_port = true;
                }
            } else {
                size_t colon = hostport.find(':');
                if (colon != std::string::npos) {
                    host = hostport.substr(0, colon);
                    port_str = hostport.substr(colon + 1);
                    has_port = true;
                } else {
                    host = hostport;
                }
            }

            if (has_port) {
                bool digits = !port_str.empty() && std::all_of(port_str.begin(), port_str.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
                long p = 0;
                if (digits) { try { p = std::stol(port_str); } catch (...) { digits = false; } }
                if (!digits || p > 65535) {
                    return url_parsing_err("invalid port number");
                }
            }

            // Empty host: required for special schemes (file exempt; default_host substitutes).
            bool host_req = host_required.value_or(special && scheme != "file");
            if (host.empty() && !default_host && host_req) {
                return url_parsing_err("empty host");
            }
        }

        // Scheme allow-list (checked after parsing, matching Rust order).
        if (!allowed_schemes.empty()) {
            bool ok = false;
            for (const auto& s : allowed_schemes) { if (scheme == s) { ok = true; break; } }
            if (!ok) return scheme_err();
        }

        UrlDefaults defaults;
        defaults.host = default_host;
        defaults.port = default_port;
        defaults.path = default_path;

        try {
            auto url_obj = std::make_shared<Url>(url_str, preserve_empty_path, defaults);
            return ValResult<std::shared_ptr<void>>(std::static_pointer_cast<void>(url_obj));
        } catch (const std::exception& e) {
            return url_parsing_err(e.what());
        }
    }

    std::string name() const override { return "url"; }
};

// MultiHostUrlValidator - validates multi-host URLs (e.g., mongodb://host1,host2,host3/db)
class MultiHostUrlValidator : public Validator {
public:
    std::optional<size_t> max_length;
    std::vector<std::string> allowed_schemes;
    bool preserve_empty_path = false;
    std::optional<bool> host_required;
    std::optional<std::string> default_host;
    std::optional<int> default_port;
    std::optional<std::string> default_path;
    bool strict = false;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        py::object input_py = input.as_python_object();

        // Check if already a MultiHostUrl object
        try {
            py::object mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
            py::object mh_class = mod.attr("MultiHostUrl");
            if (py::isinstance(input_py, mh_class)) {
                std::string s = py::str(input_py).cast<std::string>();
                if (max_length.has_value() && s.size() > max_length.value()) {
                    ErrorType err(ErrorType::Kind::UrlTooLong);
                    err.context()["max_length"] = std::to_string(max_length.value());
                    err.context()["s"] = max_length.value() == 1 ? "" : "s";
                    return ValError::line_error(err, state.location(), input.as_error_value().repr);
                }
                UrlDefaults dflt;
                dflt.host = default_host;
                dflt.port = default_port;
                dflt.path = default_path;
                auto re = std::make_shared<MultiHostUrl>(py::str(input_py).cast<std::string>(),
                                                         preserve_empty_path, dflt);
                return ValResult<std::shared_ptr<void>>(
                    std::static_pointer_cast<void>(re));
            }
        } catch (...) {}

        if (!py::isinstance<py::str>(input_py) && !py::isinstance<py::bytes>(input_py)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::UrlType),
                state.location(),
                input.as_error_value().repr
            );
        }

        std::string url_str = py::cast<std::string>(input_py);
        strip_url_whitespace(url_str);

        if (max_length.has_value() && url_str.size() > max_length.value()) {
            ErrorType err(ErrorType::Kind::UrlTooLong);
            err.context()["max_length"] = std::to_string(max_length.value());
            err.context()["s"] = max_length.value() == 1 ? "" : "s";
            return ValError::line_error(err, state.location(), url_str);
        }

        UrlDefaults defaults;
        defaults.host = default_host;
        defaults.port = default_port;
        defaults.path = default_path;

        std::shared_ptr<MultiHostUrl> url_obj;
        try {
            url_obj = std::make_shared<MultiHostUrl>(url_str, preserve_empty_path, defaults);
        } catch (const UrlEmptyHostError&) {
            ErrorType err(ErrorType::Kind::UrlParsing);
            err.context()["error"] = "empty host";
            return ValError::line_error(err, state.location(), url_str);
        } catch (const std::invalid_argument& e) {
            ErrorType err(ErrorType::Kind::UrlParsing);
            err.context()["error"] = e.what();
            return ValError::line_error(err, state.location(), url_str);
        } catch (...) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::UrlType),
                state.location(), url_str);
        }

        // Scheme allow-list (checked after parsing, matching Rust order)
        if (!allowed_schemes.empty()) {
            std::string scheme = url_obj->scheme();
            bool ok = false;
            for (const auto& s : allowed_schemes) {
                if (scheme == s) { ok = true; break; }
            }
            if (!ok) {
                ErrorType err(ErrorType::Kind::UrlScheme);
                std::string expected;
                for (size_t i = 0; i < allowed_schemes.size(); ++i) {
                    if (i > 0) expected += i == allowed_schemes.size() - 1 ? " or " : ", ";
                    expected += "'" + allowed_schemes[i] + "'";
                }
                err.context()["expected_schemes"] = expected;
                return ValError::line_error(err, state.location(), url_str);
            }
        }

        // host_required: a single empty host (no default) is an empty-host parsing error
        bool host_req = host_required.value_or(false);
        if (host_req && !url_obj->has_host()) {
            ErrorType err(ErrorType::Kind::UrlParsing);
            err.context()["error"] = "empty host";
            return ValError::line_error(err, state.location(), url_str);
        }

        return ValResult<std::shared_ptr<void>>(
            std::static_pointer_cast<void>(url_obj)
        );
    }

    std::string name() const override { return "multi-host-url"; }
};

// DecimalValidator - validates decimal values (mirrors Rust validators/decimal.rs)
class DecimalValidator : public Validator {
public:
    bool strict = false;
    bool allow_inf_nan = false;
    py::object multiple_of = py::none();
    py::object gt = py::none();
    py::object lt = py::none();
    py::object ge = py::none();
    py::object le = py::none();
    std::optional<int64_t> max_digits;
    std::optional<int64_t> decimal_places;

    // Rust: check_digits — the digit limits are the only reason to reject inf/nan.
    bool check_digits() const {
        return max_digits.has_value() || decimal_places.has_value();
    }

    // Rust: extract_decimal_digits_info — returns (decimals, digits) read from
    // Decimal.as_tuple(). A negative exponent widens the digit count because it
    // contributes the leading zeros after the decimal point.
    static std::pair<uint64_t, uint64_t> digits_info(const py::object& decimal, bool normalize) {
        py::object target = normalize ? decimal.attr("normalize")() : decimal;
        py::tuple as_tuple = target.attr("as_tuple")().cast<py::tuple>();
        int64_t exponent = as_tuple[2].cast<int64_t>();
        uint64_t digits = static_cast<uint64_t>(py::len(as_tuple[1]));
        if (exponent >= 0) {
            // A positive exponent adds that many trailing zeros.
            digits += static_cast<uint64_t>(exponent);
            return {0, digits};
        }
        uint64_t decimals = static_cast<uint64_t>(-(exponent + 1)) + 1;  // unsigned_abs, without INT64_MIN UB
        return {decimals, std::max(digits, decimals)};
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        py::object decimal_cls = py::module_::import("decimal").attr("Decimal");
        py::object input_py = input.as_python_object();
        const bool strict_required = state.strict_or(strict);

        // Rust: input.validate_decimal(strict). Exact Decimals pass through, lax
        // mode coerces str / int / float / (sign, digits, exponent) tuples, and
        // Decimal subclasses are upcast to plain Decimal.
        py::object decimal;
        bool have_decimal = false;
        try {
            if (py::type::of(input_py).ptr() == decimal_cls.ptr()) {
                decimal = input_py;
                have_decimal = true;
            } else if (!strict_required) {
                if (py::isinstance<py::str>(input_py) ||
                    (py::isinstance<py::int_>(input_py) && !py::isinstance<py::bool_>(input_py))) {
                    decimal = decimal_cls(input_py);
                    have_decimal = true;
                } else if (py::isinstance<py::float_>(input_py)) {
                    // str() first: Decimal(0.1) would otherwise keep the exact
                    // binary expansion instead of Decimal('0.1').
                    decimal = decimal_cls(py::str(input_py));
                    have_decimal = true;
                } else if (py::type::of(input_py).ptr() == reinterpret_cast<PyObject*>(&PyTuple_Type) &&
                           py::len(input_py) == 3) {
                    decimal = decimal_cls(input_py);
                    have_decimal = true;
                }
            }
            if (!have_decimal && py::isinstance(input_py, decimal_cls)) {
                decimal = decimal_cls(input_py);
                have_decimal = true;
            }
        } catch (py::error_already_set& e) {
            e.restore();
            PyErr_Clear();
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DecimalParsing),
                state.location(),
                input.as_error_value().repr
            );
        }
        if (!have_decimal) {
            ErrorType err = strict_required
                ? ErrorType(ErrorType::Kind::IsInstanceType, "class", "Decimal")
                : ErrorType(ErrorType::Kind::DecimalType);
            return ValError::line_error(err, state.location(), input.as_error_value().repr);
        }

        if (!allow_inf_nan || check_digits()) {
            bool finite = false;
            try {
                finite = decimal.attr("is_finite")().cast<bool>();
            } catch (py::error_already_set& e) {
                e.restore();
                PyErr_Clear();
            }
            if (!finite) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::FiniteNumber),
                    state.location(),
                    input.as_error_value().repr
                );
            }

            if (check_digits()) {
                try {
                    auto [norm_decimals, norm_digits] = digits_info(decimal, true);
                    auto [decimals, digits] = digits_info(decimal, false);

                    const uint64_t limit = max_digits.has_value()
                        ? static_cast<uint64_t>(*max_digits) : 0;

                    // A limit is only violated when the raw *and* normalized
                    // forms exceed it, so e.g. 1E+2 is not "3 digits".
                    if (max_digits.has_value()) {
                        if (digits > limit && norm_digits > limit) {
                            ErrorType err(ErrorType::Kind::DecimalMaxDigits);
                            err.context()["max_digits"] = std::to_string(*max_digits);
                            err.context()["s"] = (*max_digits == 1) ? "" : "s";
                            return ValError::line_error(err, state.location(), input.as_error_value().repr);
                        }
                    }

                    if (decimal_places.has_value()) {
                        uint64_t places = static_cast<uint64_t>(*decimal_places);
                        if (decimals > places && norm_decimals > places) {
                            ErrorType err(ErrorType::Kind::DecimalMaxPlaces);
                            err.context()["decimal_places"] = std::to_string(*decimal_places);
                            err.context()["s"] = (*decimal_places == 1) ? "" : "s";
                            return ValError::line_error(err, state.location(), input.as_error_value().repr);
                        }
                        if (max_digits.has_value()) {
                            uint64_t whole = digits > decimals ? digits - decimals : 0;
                            uint64_t max_whole = limit > places ? limit - places : 0;
                            uint64_t norm_whole = norm_digits > norm_decimals ? norm_digits - norm_decimals : 0;
                            if (whole > max_whole && norm_whole > max_whole) {
                                ErrorType err(ErrorType::Kind::DecimalWholeDigits);
                                err.context()["whole_digits"] = std::to_string(max_whole);
                                err.context()["s"] = (max_whole == 1) ? "" : "s";
                                return ValError::line_error(err, state.location(), input.as_error_value().repr);
                            }
                        }
                    }
                } catch (py::error_already_set& e) {
                    // Rust ignores extraction failures here (the `if let Ok(..)` chain).
                    e.restore();
                    PyErr_Clear();
                }
            }
        }

        if (!multiple_of.is_none()) {
            try {
                py::object fraction = decimal.attr("__truediv__")(multiple_of).attr("__mod__")(py::int_(1));
                py::object zero = py::int_(0);
                int is_zero = PyObject_RichCompareBool(fraction.ptr(), zero.ptr(), Py_EQ);
                if (is_zero < 0) PyErr_Clear();
                if (is_zero == 0) {
                    ErrorType err(ErrorType::Kind::MultipleOf);
                    err.set_ctx_object("multiple_of", py::str(multiple_of).cast<std::string>(), multiple_of);
                    return ValError::line_error(err, state.location(), input.as_error_value().repr);
                }
            } catch (py::error_already_set& e) {
                e.restore();
                PyErr_Clear();
            }
        }

        // Comparing a NaN Decimal raises InvalidOperation, so Rust resolves
        // is_nan() once and short-circuits every comparison on it.
        bool nan_known = false;
        bool nan_value = false;
        auto is_nan = [&]() -> bool {
            if (!nan_known) {
                nan_known = true;
                try {
                    nan_value = decimal.attr("is_nan")().cast<bool>();
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Clear();
                }
            }
            return nan_value;
        };

        // PyObject_RichCompare rather than pybind11's operators, which return a
        // C++ bool without routing through Decimal.__lt__ etc.
        auto py_cmp = [](const py::object& a, const py::object& b, int op) -> bool {
            PyObject* r = PyObject_RichCompare(a.ptr(), b.ptr(), op);
            if (!r) {
                PyErr_Clear();
                return false;
            }
            bool out = (r != Py_False);
            Py_DECREF(r);
            return out;
        };

        // Constraint order matches Rust: le, lt, ge, gt.
        if (!le.is_none() && (is_nan() || !py_cmp(decimal, le, Py_LE))) {
            ErrorType err(ErrorType::Kind::LessThanEqual);
            err.set_ctx_object("le", py::str(le).cast<std::string>(), le);
            return ValError::line_error(err, state.location(), input.as_error_value().repr);
        }
        if (!lt.is_none() && (is_nan() || !py_cmp(decimal, lt, Py_LT))) {
            ErrorType err(ErrorType::Kind::LessThan);
            err.set_ctx_object("lt", py::str(lt).cast<std::string>(), lt);
            return ValError::line_error(err, state.location(), input.as_error_value().repr);
        }
        if (!ge.is_none() && (is_nan() || !py_cmp(decimal, ge, Py_GE))) {
            ErrorType err(ErrorType::Kind::GreaterThanEqual);
            err.set_ctx_object("ge", py::str(ge).cast<std::string>(), ge);
            return ValError::line_error(err, state.location(), input.as_error_value().repr);
        }
        if (!gt.is_none() && (is_nan() || !py_cmp(decimal, gt, Py_GT))) {
            ErrorType err(ErrorType::Kind::GreaterThan);
            err.set_ctx_object("gt", py::str(gt).cast<std::string>(), gt);
            return ValError::line_error(err, state.location(), input.as_error_value().repr);
        }

        return ValResult<std::shared_ptr<void>>(
            std::make_shared<py::object>(std::move(decimal))
        );
    }

    std::string name() const override { return "decimal"; }
    std::string effective_result_name() const override { return "py_object"; }
};

// UuidValidator - validates UUID values
class UuidValidator : public Validator {
public:
    bool strict = false;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Validate UUID using Python's uuid module
        py::object input_py = input.as_python_object();

        // If already a UUID object, accept it
        try {
            py::object uuid_mod = py::module_::import("uuid");
            py::object uuid_class = uuid_mod.attr("UUID");
            if (py::isinstance(input_py, uuid_class)) {
                return ValResult<std::shared_ptr<void>>(
                    std::make_shared<std::string>(py::str(input_py).cast<std::string>())
                );
            }
        } catch (...) {}

        // In strict mode (python input), only accept UUID objects (Rust: IsInstanceOf)
        if (state.strict_or(strict)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IsInstanceType, "class", "UUID"),
                state.location(),
                input.as_error_value().repr
            );
        }

        // String: parse it (Rust: UuidParsing on failure)
        if (py::isinstance<py::str>(input_py)) {
            std::string uuid_str = py::str(input_py).cast<std::string>();
            try {
                py::object uuid_mod = py::module_::import("uuid");
                py::object uuid_obj = uuid_mod.attr("UUID")(uuid_str);
                return ValResult<std::shared_ptr<void>>(
                    std::make_shared<std::string>(py::str(uuid_obj).cast<std::string>())
                );
            } catch (py::error_already_set& e) {
                std::string msg = e.what();
                // Swallow the error: restore() releases the fetched refs so
                // the destructor is a no-op and the error indicator stays clear
                e.restore();
                PyErr_Clear();
                ErrorType err(ErrorType::Kind::UuidParsing);
                err.context()["error"] = msg;
                return ValError::line_error(err, state.location(), uuid_str);
            }
        }

        // Bytes: parse (Rust: UuidType when not bytes, UuidParsing when invalid)
        if (py::isinstance<py::bytes>(input_py)) {
            std::string b = input_py.cast<std::string>();
            try {
                py::object uuid_mod = py::module_::import("uuid");
                py::object uuid_obj;
                try {
                    uuid_obj = uuid_mod.attr("UUID")(py::bytes(b));
                } catch (py::error_already_set& e1) {
                    e1.restore();
                    PyErr_Clear();
                    try {
                        uuid_obj = uuid_mod.attr("UUID")(b);
                    } catch (py::error_already_set& e2) {
                        std::string msg = e2.what();
                        e2.restore();
                        PyErr_Clear();
                        // Rust falls back to Uuid::from_slice when the utf-8 text
                        // form fails, which accepts a raw 16-byte value.
                        if (b.size() == 16) {
                            try {
                                uuid_obj = uuid_mod.attr("UUID")(py::arg("bytes") = input_py);
                                return ValResult<std::shared_ptr<void>>(
                                    std::make_shared<std::string>(py::str(uuid_obj).cast<std::string>())
                                );
                            } catch (py::error_already_set& e3) {
                                e3.restore();
                                PyErr_Clear();
                            }
                        }
                        ErrorType err(ErrorType::Kind::UuidParsing);
                        err.context()["error"] = msg;
                        return ValError::line_error(err, state.location(),
                                                    input.as_error_value().repr);
                    }
                }
                return ValResult<std::shared_ptr<void>>(
                    std::make_shared<std::string>(py::str(uuid_obj).cast<std::string>())
                );
            } catch (...) {}
        }

        return ValError::line_error(
            ErrorType(ErrorType::Kind::UuidType),
            state.location(),
            input.as_error_value().repr
        );
    }

    std::string name() const override { return "uuid"; }
};

// LiteralValidator - validates literal values
class LiteralValidator : public Validator {
public:
    LiteralValidator() = default;

    static bool is_exact_str(const py::object& o) {
        return Py_TYPE(o.ptr()) == &PyUnicode_Type;
    }

    // Build from Python expected values, mirroring Rust LiteralLookup::new:
    // exact bool/int/str maps plus a hash-based dict for everything else
    // (enum members hash-equal their underlying value, so a plain-string
    // input matches a StrEnum literal and yields the MEMBER back).
    explicit LiteralValidator(py::sequence expected, std::string expected_repr = "")
        : expected_repr_(std::move(expected_repr)) {
        size_t id = 0;
        for (auto handle : expected) {
            py::object k = py::reinterpret_borrow<py::object>(handle);
            if (py::isinstance<py::bool_>(k)) {
                bool_ids_.push_back(id);
            } else if (py::isinstance<py::int_>(k)) {
                try {
                    int_ids_.emplace(k.cast<long long>(), id);
                } catch (...) {}
            } else if (is_exact_str(k)) {
                str_ids_.emplace(k.cast<std::string>(), id);
            }
            try {
                expected_dict_[k] = py::int_(static_cast<long long>(id));
            } catch (...) {
                // Unhashable expected value: linear equality scan instead.
                eq_ids_.push_back(id);
            }
            values_.push_back(k);
            ++id;
        }
    }

    // Legacy convenience ctor (JSON-schema path): wrap plain strings.
    explicit LiteralValidator(std::vector<std::string> values, std::string expected_repr = "")
        : expected_repr_(std::move(expected_repr)) {
        py::list items;
        for (const auto& v : values) items.append(py::str(v));
        *this = LiteralValidator(items, expected_repr_);
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Build a literal_error carrying the ORIGINAL Python input object so
        // error serialization matches Rust (which keeps the raw input rather
        // than its repr).
        auto literal_error = [&]() -> ValError {
            ValError err = ValError::line_error(
                ErrorType(ErrorType::Kind::LiteralError, "expected", expected_repr_.empty() ? "" : expected_repr_),
                state.location(),
                input.as_error_value().repr
            );
#ifdef HAS_PYBIND11
            if (!err.line_errors().empty()) {
                err.line_errors()[0]->raw_input_obj = input.as_python_object();
            }
#endif
            return err;
        };

        if (values_.empty()) {
            return literal_error();
        }
        auto make_result = [&](size_t id) {
            return ValResult<std::shared_ptr<void>>(
                std::make_shared<py::object>(values_[id]));
        };

        py::object py_in = input.as_python_object();

        // 1. Exact bool match (before int: bool subclasses int).
        if (py::isinstance<py::bool_>(py_in)) {
            bool b = py_in.cast<bool>();
            for (size_t id : bool_ids_) {
                if (values_[id].cast<bool>() == b) return make_result(id);
            }
        } else {
            // 2. Exact int match (no lax str/float coercion, unlike Rust's
            //    generic str validation — literals are strict about type).
            if (py::isinstance<py::int_>(py_in)) {
                try {
                    long long i = py_in.cast<long long>();
                    auto it = int_ids_.find(i);
                    if (it != int_ids_.end()) return make_result(it->second);
                } catch (...) {}
            }
            // 3. Exact (non-subclass) str match.
            if (is_exact_str(py_in)) {
                auto it = str_ids_.find(py_in.cast<std::string>());
                if (it != str_ids_.end()) return make_result(it->second);
            }
        }
        // 4. Hash-based lookup: covers enum-member literals matched by their
        //    plain value (StrEnum members hash/eq like their string value)
        //    and any other hashable expected objects.
        bool hashed = false;
        try {
            hashed = expected_dict_.contains(py_in);
        } catch (py::error_already_set&) {
            PyErr_Clear();  // unhashable input
        }
        if (hashed) {
            return make_result(expected_dict_[py_in].cast<size_t>());
        }
        // 5. Equality scan for unhashable expected values.
        for (size_t id : eq_ids_) {
            try {
                if (values_[id].equal(py_in)) return make_result(id);
            } catch (...) {}
        }
        return literal_error();
    }

    std::string name() const override { return "literal"; }

private:
    std::vector<py::object> values_;
    std::vector<size_t> bool_ids_;
    std::unordered_map<long long, size_t> int_ids_;
    std::unordered_map<std::string, size_t> str_ids_;
    py::dict expected_dict_;
    std::vector<size_t> eq_ids_;
    std::string expected_repr_;
};

// EnumValidator - validates enum values
//
// Rust (validators/enum_.rs) keeps the Enum members themselves as the accepted
// values and returns the matched member, so a use_enum_values post-processor
// (operator.attrgetter('value')) sees an Enum instance instead of a bare value.
class EnumValidator : public Validator {
public:
    EnumValidator() = default;
    explicit EnumValidator(std::unordered_set<std::string> valid_values)
        : valid_values_(std::move(valid_values)) {}

    struct Member {
        py::object member;
        py::object value;
    };

    // cls/members/sub_type/expected_repr come straight from the core schema,
    // mirroring EnumValidator::from_config.
    void configure_class(py::object cls, std::vector<Member> members,
                         std::string sub_type, std::string expected_repr,
                         std::string class_repr,
                         std::optional<bool> declared_strict) {
        cls_ = std::move(cls);
        members_ = std::move(members);
        sub_type_ = std::move(sub_type);
        expected_repr_ = std::move(expected_repr);
        class_repr_ = std::move(class_repr);
        declared_strict_ = declared_strict;
    }

    std::string name() const override { return "enum"; }

    // The validated value is the Enum member itself, so the result-to-Python
    // dispatch has to pass the object through untouched.
    std::string effective_result_name() const override {
        return cls_.ptr() ? "py_object" : "enum";
    }

    // Rust's validators::literal::expected_repr: all but the last joined with
    // ", ", and " or " immediately before the last one.
    static std::string join_expected(const std::vector<std::string>& reprs) {
        std::string out;
        for (size_t i = 0; i + 1 < reprs.size(); ++i) {
            if (i) out += ", ";
            out += reprs[i];
        }
        if (reprs.size() > 1) out += " or ";
        if (!reprs.empty()) out += reprs.back();
        return out;
    }

    static std::string value_repr(const py::object& o) {
        PyObject* r = PyObject_Repr(o.ptr());
        if (!r) {
            PyErr_Clear();
            return std::string();
        }
        return py::reinterpret_steal<py::str>(r).cast<std::string>();
    }

    // PyType::name() in Rust reads __qualname__, which is what the is-instance
    // message shows (e.g. "test_strict_enum.<locals>.Demo").
    static std::string type_qualname(PyObject* t) {
        PyObject* q = PyObject_GetAttrString(t, "__qualname__");
        if (!q) {
            PyErr_Clear();
            q = PyObject_GetAttrString(t, "__name__");
        }
        if (!q) {
            PyErr_Clear();
            return std::string();
        }
        return py::reinterpret_steal<py::str>(q).cast<std::string>();
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (cls_.ptr()) return validate_class(input, state);
        return validate_legacy(input, state);
    }

private:
    static ValResult<std::shared_ptr<void>> member_result(const py::object& member) {
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(member));
    }

    ValError enum_error_for(const Input& input, ValidationState& state) {
        ErrorType err(ErrorType::Kind::EnumError);
        err.context()["expected"] = expected_repr_;
        return ValError::line_error(std::move(err), state.location(),
                                    input.as_error_value().repr);
    }

    ValResult<std::shared_ptr<void>> validate_class(const Input& input, ValidationState& state) {
        py::object input_py = input.as_python_object();

        // Rust accepts an exact instance of the Enum class before anything else.
        if (input_py && Py_TYPE(input_py.ptr()) == reinterpret_cast<PyTypeObject*>(cls_.ptr())) {
            return member_result(input_py);
        }

        // The strict short-circuit is Python-input only; JSON inputs keep their
        // usual coercions.
        if (strict_active(state) && state.input_type() == InputType::Python) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IsInstanceType, "class", class_repr_),
                state.location(), input.as_error_value().repr);
        }

        // Match on the member value, with the coercion Rust applies per sub_type.
        std::vector<PyObject*> candidates;
        if (input_py) candidates.push_back(input_py.ptr());
        py::object coerced;
        if (sub_type_ == "int") {
            auto r = input.validate_int(false);
            if (r.is_ok()) coerced = r.value().value().to_python();
        } else if (sub_type_ == "float") {
            auto r = input.validate_float(false);
            if (r.is_ok()) coerced = py::float_(r.value().value().as_double());
        } else if (sub_type_ == "str") {
            auto r = input.validate_str(false, false);
            if (r.is_ok()) coerced = py::str(r.value().value().to_string());
        }
        if (coerced) candidates.push_back(coerced.ptr());

        for (PyObject* cand : candidates) {
            for (const auto& m : members_) {
                if (!m.value) continue;
                int eq = PyObject_RichCompareBool(cand, m.value.ptr(), Py_EQ);
                if (eq == 1) return member_result(m.member);
                if (eq < 0) PyErr_Clear();
            }
        }

        // Then call the class itself, which resolves value lookups and runs any
        // user-defined _missing_ hook.
        if (input_py) {
            PyObject* out = PyObject_CallOneArg(cls_.ptr(), input_py.ptr());
            if (!out) {
                PyErr_Clear();
            } else {
                py::object out_obj = py::reinterpret_steal<py::object>(out);
                if (PyObject_IsInstance(out_obj.ptr(), cls_.ptr()) == 1) {
                    return member_result(out_obj);
                }
            }
        }
        return enum_error_for(input, state);
    }

    ValResult<std::shared_ptr<void>> validate_legacy(const Input& input, ValidationState& state) {
        if (valid_values_.empty()) {
            return ValError::line_error(
                PydanticKnownError::enum_error(),
                state.location(),
                input.as_error_value().repr
            );
        }

        // Accept Enum member instances directly (matches Rust's exact-instance check),
        // e.g. `MyEnum.val` passed as input regardless of strict mode.
        try {
            py::object py_in = input.as_python_object();
            if (!py_in.is_none() && py_hasattr(py_in, "_name_") && py_hasattr(py_in, "_value_")) {
                std::string name = py::str(py_in).cast<std::string>();
                if (valid_values_.count(name)) {
                    return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(name));
                }
                auto dot_pos = name.rfind('.');
                std::string short_name = (dot_pos != std::string::npos) ? name.substr(dot_pos + 1) : name;
                if (valid_values_.count(short_name)) {
                    return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(short_name));
                }
                // Value-based match (e.g. int/str-valued enums whose names differ from values)
                py::object val = py::getattr(py_in, "_value_");
                if (py::isinstance<py::str>(val)) {
                    std::string v = val.cast<std::string>();
                    if (valid_values_.count(v)) {
                        return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(v));
                    }
                }
                return ValError::line_error(
                    PydanticKnownError::enum_error(),
                    state.location(),
                    input.as_error_value().repr
                );
            }
        } catch (...) {}

        auto str_result = input.validate_str(state.strict_or(false), false);
        if (str_result.is_err()) {
            return str_result.error();
        }
        auto es = str_result.value().value();
        std::string str_val;
        if (auto* s = std::get_if<std::string>(&es.value)) {
            str_val = *s;
        } else {
            str_val = std::string(std::get<std::string_view>(es.value));
        }
        if (valid_values_.count(str_val)) {
            return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(str_val));
        }
        // Also try matching against the short form (without class prefix)
        for (const auto& vv : valid_values_) {
            auto dot_pos = vv.rfind('.');
            if (dot_pos != std::string::npos && dot_pos + 1 < vv.size()) {
                auto short_name = vv.substr(dot_pos + 1);
                if (short_name == str_val) {
                    return ValResult<std::shared_ptr<void>>(
                        std::make_shared<std::string>(str_val));
                }
            }
        }
        return ValError::line_error(
            PydanticKnownError::enum_error(),
            state.location(),
            input.as_error_value().repr
        );
    }

    std::unordered_set<std::string> valid_values_;
    py::object cls_;
    std::vector<Member> members_;
    std::string sub_type_;
    std::string expected_repr_;
    std::string class_repr_;
    // Rust keeps is_strict(schema, config) per validator, so a field-level
    // Field(strict=False) overrides the model-wide config. The C++ state cannot
    // tell that config apart from a call-time override, so a schema-declared
    // value simply wins here.
    std::optional<bool> declared_strict_;

    bool strict_active(const ValidationState& state) const {
        return declared_strict_.value_or(state.strict_or(false));
    }
};

// CustomErrorValidator - wraps an inner validator, replacing its error with a custom one
class CustomErrorValidator : public Validator {
public:
    CustomErrorValidator() = default;
    CustomErrorValidator(std::shared_ptr<Validator> inner, std::string msg, std::string error_type)
        : inner_(std::move(inner)), msg_(std::move(msg)), error_type_(std::move(error_type)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (inner_) {
            auto result = inner_->validate(input, state);
            if (result.is_ok()) return result;
        }
        // Inner failed (or no inner) — raise custom error.
        // Build an ErrorType whose type_name matches error_type_, and whose
        // message is the rendered msg_ (no template processing needed).
        std::string type_key = error_type_.empty() ? "custom_error" : error_type_;
        std::string rendered_msg = msg_.empty() ? "Validation error" : msg_;
        auto error_type = ErrorType(type_key, rendered_msg);
        ValError val_err = ValError::line_error(
            std::move(error_type),
            state.location(),
            input.as_error_value().repr
        );
        // Preserve original Python object for accurate serialization (Rust parallel:
        // as_val_error(input) where input holds Py<PyAny>).
#ifdef HAS_PYBIND11
        if (!val_err.line_errors().empty()) {
            val_err.line_errors()[0]->raw_input_obj = input.as_python_object();
        }
#endif
        return val_err;
    }

    std::string name() const override {
        if (inner_) return inner_->name();
        return "custom-error";
    }

    std::string effective_result_name() const override {
        if (inner_) return inner_->effective_result_name();
        return "custom-error";
    }

private:
    std::shared_ptr<Validator> inner_;
    std::string msg_;
    std::string error_type_;
};

} // namespace pydantic_core