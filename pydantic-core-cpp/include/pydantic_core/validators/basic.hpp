#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/py_compat.hpp"
#include <memory>
#include <optional>
#include <string>
#include <cmath>
#include <regex>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace pydantic_core {

// AnyValidator - accepts any value
class AnyValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Accept any input - return the original Python object to preserve identity and type
        py::object obj = input.as_python_object();
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<py::object>(std::move(obj))
        );
    }

    std::string name() const override { return "any"; }
};

// NoneValidator - only accepts None/null
class NoneValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (input.is_none()) {
            return ValResult<std::shared_ptr<void>>(nullptr);
        }
        return ValError::line_error(
            PydanticKnownError::none_required(),
            state.location(),
            input.as_error_value().repr
        );
    }

    std::string name() const override { return "none"; }
    std::string debug_repr() const override { return "None(NoneValidator)"; }
};

// BoolValidator - validates boolean values
class BoolValidator : public Validator {
public:
    bool strict = false;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto result = input.validate_bool(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<bool>(result.value().value())
        );
    }

    std::string name() const override { return "bool"; }
    std::string debug_repr() const override { return "Bool(BoolValidator { strict: " + std::string(strict ? "true" : "false") + " })"; }
};

// IntValidator - validates integer values
class IntValidator : public Validator {
public:
    bool strict = false;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto result = input.validate_int(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        auto& either_int = result.value().value();
        // Return the actual validated integer (preserves both int64 and
        // uint64 range so values larger than 2^63-1 are not truncated).
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherInt>(either_int)
        );
    }

    std::string name() const override { return "int"; }
    std::string debug_repr() const override { return "Int(IntValidator { strict: " + std::string(strict ? "true" : "false") + " })"; }
};

// ConstrainedIntValidator - validates int with gt/lt/ge/le/multiple_of constraints
class ConstrainedIntValidator : public Validator {
public:
    // Rust types every int constraint as Int = I64 | Big (int.rs:88-95) and reads the value
    // with the same type (int.rs:131), so 2**64 is a bound it compares exactly and an int
    // past i64 is a value it compares.  A bound that fits an int64 keeps it in `small` and
    // needs no Python at all; one that does not is kept whole rather than narrowed, which
    // is what let `gt=2**64` build at all.
    struct IntBound {
        std::optional<int64_t> small;
        py::object whole = py::none();

        IntBound() = default;
        IntBound(int64_t value) : small(value) {}

        bool present() const { return small.has_value() || !whole.is_none(); }

        py::object to_object() const {
            if (!whole.is_none()) return whole;
            return py::int_(*small);
        }

        std::string text() const {
            if (!whole.is_none()) return py::str(whole).cast<std::string>();
            return std::to_string(*small);
        }
    };

    bool strict = false;
    IntBound gt;
    IntBound ge;
    IntBound lt;
    IntBound le;
    IntBound multiple_of;

    ConstrainedIntValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto result = input.validate_int(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        auto& either_int = result.value().value();
        const std::optional<int64_t> narrow = either_int.as_i64();
        // An int that does not fit an int64 is only talkable-to about in Python, and only
        // then: the common comparison below never asks for it.
        py::object value_obj;
        auto value_py = [&]() -> PyObject* {
            if (!value_obj) value_obj = either_int.to_python();
            return value_obj.ptr();
        };
        auto satisfies = [&](int op, const IntBound& bound) -> bool {
            if (narrow && bound.small) {
                const int64_t v = *narrow;
                const int64_t b = *bound.small;
                if (op == Py_GT) return v > b;
                if (op == Py_GE) return v >= b;
                if (op == Py_LT) return v < b;
                return v <= b;
            }
            int r = PyObject_RichCompareBool(value_py(), bound.to_object().ptr(), op);
            if (r < 0) {
                PyErr_Clear();
                return false;
            }
            return r == 1;
        };
        auto failed = [&](ErrorType::Kind kind, const char* key, const IntBound& bound) {
            ErrorType err(kind);
            err.set_ctx_object(key, bound.text(), bound.to_object());
            // A refused integer names the input it arrived as, not the integer it
            // was read into: the reference hands `input` to the error
            // (int.rs:132-159), so a text '3' that breaks a bound is refused as
            // the text. Carrying the object too is what says so -- the type is
            // read from it rather than guessed back from the repr, which cannot
            // tell '3' from 3.
            auto val = ValError::line_error(std::move(err), state.location(),
                                            input.as_error_value().repr);
#ifdef HAS_PYBIND11
            if (!val.line_errors().empty()) {
                val.line_errors()[0]->raw_input_obj = input.as_python_object();
            }
#endif
            return val;
        };

        // Rust asks divisibility before the range bounds (int.rs:133-141), so a value that
        // breaks both reports the divisibility it failed first.
        if (multiple_of.present()) {
            bool divides = false;
            if (narrow && multiple_of.small) {
                divides = *multiple_of.small != 0 && *narrow % *multiple_of.small == 0;
            } else {
                PyObject* rem = PyNumber_Remainder(value_py(), multiple_of.to_object().ptr());
                if (rem) {
                    divides = PyObject_IsTrue(rem) == 0;
                    Py_DECREF(rem);
                } else {
                    PyErr_Clear();
                }
            }
            if (!divides) return failed(ErrorType::Kind::MultipleOf, "multiple_of", multiple_of);
        }
        if (gt.present() && !satisfies(Py_GT, gt)) {
            return failed(ErrorType::Kind::GreaterThan, "gt", gt);
        }
        if (ge.present() && !satisfies(Py_GE, ge)) {
            return failed(ErrorType::Kind::GreaterThanEqual, "ge", ge);
        }
        if (lt.present() && !satisfies(Py_LT, lt)) {
            return failed(ErrorType::Kind::LessThan, "lt", lt);
        }
        if (le.present() && !satisfies(Py_LE, le)) {
            return failed(ErrorType::Kind::LessThanEqual, "le", le);
        }

        // Return the full validated integer (supports the uint64 range as well).
        return ValResult<std::shared_ptr<void>>(std::make_shared<EitherInt>(either_int));
    }

    std::string name() const override { return "constrained-int"; }
};

// FloatValidator - validates float values
class FloatValidator : public Validator {
public:
    bool strict = false;
    bool allow_inf_nan = true;

    FloatValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto result = input.validate_float(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        auto& either_float = result.value().value();
        double f = either_float.as_double();
        if (!allow_inf_nan && !std::isfinite(f)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FiniteNumber),
                state.location(),
                input.as_error_value().repr
            );
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<double>(f));
    }

    std::string name() const override { return "float"; }
    std::string debug_repr() const override { return "Float(FloatValidator { strict: " + std::string(strict ? "true" : "false") + ", allow_inf_nan: " + std::string(allow_inf_nan ? "true" : "false") + " })"; }
};

// ConstrainedFloatValidator - validates float with gt/lt/ge/le/multiple_of constraints
class ConstrainedFloatValidator : public Validator {
public:
    bool strict = false;
    bool allow_inf_nan = true;
    std::optional<double> gt;
    std::optional<double> ge;
    std::optional<double> lt;
    std::optional<double> le;
    std::optional<double> multiple_of;

    ConstrainedFloatValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto result = input.validate_float(state.strict_or(strict));
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        auto& either_float = result.value().value();
        double f = either_float.as_double();

        if (!allow_inf_nan && !std::isfinite(f)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::FiniteNumber),
                state.location(),
                input.as_error_value().repr
            );
        }
        // Rust puts the constraint in ctx as a float, so an integral bound has
        // to survive as 1.0 rather than the int a numeric string would decode to.
        auto float_ctx = [](ErrorType::Kind kind, const char* key, double value) {
            ErrorType err(kind);
            err.set_ctx_object(key, format_double(value), py::float_(value));
            return err;
        };
        // Rust applies multiple_of before the range bounds.
        if (multiple_of.has_value() && multiple_of.value() != 0.0) {
            double tolerance = 1e-9;
            double rounded_div = std::round(f / multiple_of.value());
            double diff = std::abs(f - (rounded_div * multiple_of.value()));
            if (diff > tolerance) {
                ErrorType err(ErrorType::Kind::MultipleOf);
                err.set_ctx_object("multiple_of", format_double(multiple_of.value()),
                                   py::float_(multiple_of.value()));
                return ValError::line_error(
                    std::move(err), state.location(), input.as_error_value().repr
                );
            }
        }
        if (gt.has_value() && f <= gt.value()) {
            return ValError::line_error(
                float_ctx(ErrorType::Kind::GreaterThan, "gt", gt.value()),
                state.location(),
                input.as_error_value().repr
            );
        }
        if (ge.has_value() && f < ge.value()) {
            return ValError::line_error(
                float_ctx(ErrorType::Kind::GreaterThanEqual, "ge", ge.value()),
                state.location(),
                input.as_error_value().repr
            );
        }
        if (lt.has_value() && f >= lt.value()) {
            return ValError::line_error(
                float_ctx(ErrorType::Kind::LessThan, "lt", lt.value()),
                state.location(),
                input.as_error_value().repr
            );
        }
        if (le.has_value() && f > le.value()) {
            return ValError::line_error(
                float_ctx(ErrorType::Kind::LessThanEqual, "le", le.value()),
                state.location(),
                input.as_error_value().repr
            );
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<double>(f));
    }

    std::string name() const override { return "constrained-float"; }
};

// ComplexValidator - validates complex values
class ComplexValidator : public Validator {
public:
    bool strict = false;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Rust asks the input itself (input_python.rs:705, input_json.rs:387,
        // input_string.rs:272), and the three answer differently enough that the
        // error type depends on where the value came from: a Python object is
        // handed to the complex() constructor and an object that refuses it is a
        // value of the wrong TYPE, while a JSON or validate_strings string is
        // expected to be a complex string and fails for not parsing.  The two
        // signals below have to be read together: a validate_strings input is a
        // StringInput of its own, while JSON is decoded into Python objects before
        // it is validated (main_module.cpp:5471), so what marks a JSON value is the
        // state it travels with, not the input class holding it.
        const InputType kind = input.input_type() == InputType::String
                                   ? InputType::String
                                   : state.input_type();
        py::object complex_cls = py::module_::import("builtins").attr("complex");
        py::object value = input.as_python_object();
        auto rejected = [&](ErrorType et) {
            return ValError::line_error(et, state.location(), input.as_error_value().repr);
        };

        if (kind == InputType::Json) {
            // simdjson has no complex, so a JSON value reaches this arm as the
            // Python object the parser produced; a JSON `true` is a bool, not a
            // number, and Rust answers it with complex_type.
            if (py::isinstance<py::bool_>(value)) return rejected(ErrorType(ErrorType::Kind::ComplexType));
            if (py::isinstance<py::str>(value)) {
                // A JSON string is parsed in strict mode too: Rust ignores strict
                // here and asks the string itself (input_json.rs:389).
                return parse_complex_string(value, input, state, complex_cls,
                                            ErrorType::Kind::ComplexStrParsing, false);
            }
            // complex.rs:56 hands the input the strict that was baked at build
            // time (is_strict(schema, config)), never the call's, so a
            // validate_python(..., strict=True) does not reach this validator.
            if (py::isinstance<py::int_>(value)) {
                // jiter turns a literal of at most 18 digits into an Int and keeps a
                // longer one as a big decimal, which Rust's complex arm has no case
                // for (input_json.rs:396) and calls complex_type.  The port decodes
                // JSON integers exactly, so it draws the line at the same digits
                // rather than handing the constructor a number it rounds to a float.
                // Rust asks this of the value before it asks anything about strict,
                // so an oversized literal is the wrong type even in a strict schema.
                if (!within_fast_int(value)) {
                    return rejected(ErrorType(ErrorType::Kind::ComplexType));
                }
                if (strict) return rejected(ErrorType(ErrorType::Kind::ComplexStrParsing));
                return coerced(complex_cls(value), state);
            }
            if (py::isinstance<py::float_>(value)) {
                if (strict) return rejected(ErrorType(ErrorType::Kind::ComplexStrParsing));
                return coerced(complex_cls(value), state);
            }
            return rejected(ErrorType(ErrorType::Kind::ComplexType));
        }

        if (kind == InputType::String) {
            // A validate_strings input is a string or a mapping; only the string
            // can be a complex string, and it is not coerced -- a mapping is the
            // wrong type (input_string.rs:272).
            return parse_complex_string(value, input, state, complex_cls,
                                        ErrorType::Kind::ComplexStrParsing, true);
        }

        if (py::isinstance(value, complex_cls)) {
            // Already a complex: Rust counts that as a strict match.
            state.floor_exactness(Exactness::Strict);
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(value)));
        }
        if (strict) {
            // Rust reports an is_instance_of against complex, not a complex_type
            // (input_python.rs:708) -- strict says what type it wanted.
            return rejected(ErrorType(ErrorType::Kind::IsInstanceType, "class", "complex"));
        }
        if (py::isinstance<py::str>(value)) {
            // A Python string that does not parse is NOT reported as a string
            // parsing error: Rust says "give any acceptable value" instead, since
            // a caller that passed a string here may have meant another type
            // entirely (input_python.rs:721).  A string is coerced laxly.
            auto parsed = try_complex(value, complex_cls);
            if (parsed) return coerced(*parsed, state);
        }
        // Anything else goes to the constructor, which is how a Decimal, a
        // Fraction or any object with __complex__ gets in (input_python.rs:733).
        auto made = try_complex(value, complex_cls);
        if (made) return coerced(*made, state);
        return rejected(ErrorType(ErrorType::Kind::ComplexType));
    }

private:
    static bool within_fast_int(const py::object& value) {
        std::string digits = py::str(value).cast<std::string>();
        if (!digits.empty() && digits.front() == '-') digits.erase(digits.begin());
        return digits.size() <= 18;
    }

    static std::optional<py::object> try_complex(const py::object& arg, const py::object& complex_cls) {
        try {
            return complex_cls(arg);
        } catch (const py::error_already_set& e) {
            PyErr_Clear();
            return std::nullopt;
        }
    }

    ValResult<std::shared_ptr<void>> coerced(py::object value, ValidationState& state) const {
        state.floor_exactness(Exactness::Lax);
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(value)));
    }

    // A string that has to BE a complex string: strict match on success, and the
    // given error type when it does not parse.  Rust keeps a mapping input of a
    // validate_strings schema out of this path -- it is the wrong type.
    ValResult<std::shared_ptr<void>> parse_complex_string(
        const py::object& value,
        const Input& input,
        ValidationState& state,
        const py::object& complex_cls,
        ErrorType::Kind kind,
        bool mapping_is_type_error
    ) const {
        if (mapping_is_type_error && !py::isinstance<py::str>(value)) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::ComplexType), state.location(), input.as_error_value().repr);
        }
        auto parsed = try_complex(value, complex_cls);
        if (!parsed) {
            return ValError::line_error(
                ErrorType(kind), state.location(), input.as_error_value().repr);
        }
        state.floor_exactness(Exactness::Strict);
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(*parsed)));
    }

public:
    std::string name() const override { return "complex"; }
    std::string effective_result_name() const override { return "py_object"; }
};

// StringValidator - validates string values
class StringValidator : public Validator {
public:
    bool strict = false;
    bool coerce_numbers_to_str = false;
    mutable std::string last_type_name_;

    StringValidator() = default;

    // The pointee differs per call -- a live py::object or the validator's own text -- so the
    // name result conversion dispatches on has to come from the call, as WithDefaultValidator
    // already does for the same reason.
    std::string effective_result_name() const override {
        return last_type_name_.empty() ? std::string("str") : last_type_name_;
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto result = input.validate_str(state.strict_or(strict), coerce_numbers_to_str,
                                       state.input_type() == InputType::Json);
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        // Rust ends with as_py_string on the input (string.rs:88): an input that already is an
        // exact str comes back as the object that went in, so that object -- not a copy of its
        // text -- is what this call produced.
        if (input_is_exact_str(input)) {
            last_type_name_ = "py_object";
            return ValResult<std::shared_ptr<void>>(
                std::make_shared<py::object>(input.as_python_object()));
        }
        last_type_name_.clear();
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<std::string>(result.value().value().to_string())
        );
    }

    std::string name() const override { return "str"; }
    std::string debug_repr() const override { return "Str(StrValidator { strict: " + std::string(strict ? "true" : "false") + ", coerce_numbers_to_str: " + std::string(coerce_numbers_to_str ? "true" : "false") + " })"; }
};


// Convert a std::string to its Python repr (single-quoted, escaped) so that
// error input values round-trip correctly through the Python wrapper's
// ast.literal_eval reconstruction (e.g. "00" must stay the string '00',
// not become the int 0).
inline std::string python_str_repr(const std::string& s) {
    std::string result = "'";
    for (char c : s) {
        switch (c) {
            case '\'' : result += "\\'"; break;
            case '\\' : result += "\\\\"; break;
            case '\n' : result += "\\n"; break;
            case '\r' : result += "\\r"; break;
            case '\t' : result += "\\t"; break;
            default : result += c;
        }
    }
    result += "'";
    return result;
}

// Rust trims `char::is_whitespace`, which is Unicode's White_Space property.  Python's own set is
// bigger by U+001C..U+001F, so the trim cannot be handed to str.strip(); the C locale's set is
// smaller than both.  Measured off the wheel over every code point: these twenty-five and no other.
static const unsigned kUnicodeWhitespace[] = {
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20, 0x85, 0xA0, 0x1680, 0x2000, 0x2001, 0x2002, 0x2003,
    0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200A, 0x2028, 0x2029, 0x202F, 0x205F,
    0x3000};

static bool utf8_is_whitespace_at(const std::string& s, size_t i, size_t end, size_t& width) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    unsigned cp = 0;
    if (b0 < 0x80) {
        cp = b0;
        width = 1;
    } else if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (i + 1 >= end) return false;
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        if ((b1 & 0xC0) != 0x80) return false;
        cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
        width = 2;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (i + 2 >= end) return false;
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
        cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (b0 == 0xE0 && b1 < 0xA0) return false;
        if (b0 == 0xED && b1 >= 0xA0) return false;
        width = 3;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (i + 3 >= end) return false;
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        const unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
        cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
        if (cp < 0x10000 || cp > 0x10FFFF) return false;
        width = 4;
    } else {
        // A continuation byte, an overlong lead, or a lone surrogate: not a whitespace
        // code point, and Rust leaves such a run standing.
        return false;
    }
    for (unsigned w : kUnicodeWhitespace) {
        if (w == cp) return true;
    }
    return false;
}

static void trim_unicode_whitespace(std::string& str) {
    size_t begin = 0;
    size_t end = str.size();
    for (;;) {
        size_t width = 0;
        if (begin >= end || !utf8_is_whitespace_at(str, begin, end, width)) break;
        begin += width;
    }
    for (;;) {
        if (begin >= end) break;
        // Walk back over the continuation bytes to the lead of the last code point.
        size_t last = end - 1;
        while (last > begin && (static_cast<unsigned char>(str[last]) & 0xC0) == 0x80) --last;
        size_t width = 0;
        if (!utf8_is_whitespace_at(str, last, end, width) || last + width != end) break;
        end = last;
    }
    if (begin != 0 || end != str.size()) str = str.substr(begin, end - begin);
    else if (begin == end) str.clear();
}

// StrConstrainedValidator - validates string with min_length/max_length/pattern/strip_whitespace/to_lower/to_upper
class StrConstrainedValidator : public Validator {
public:
    bool strict = false;
    bool coerce_numbers_to_str = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::string pattern;
    // A compiled re.Pattern from the schema: Rust keeps it and uses the
    // RegexEngine::PythonRe branch instead of compiling the source itself.
    py::object pattern_re;
    // The engine for a pattern given as text, chosen once when the schema is built
    // (prepare_str_pattern).  Unset with pattern_engine_ready set means neither engine
    // could compile it, which is the case the port has always skipped.
    std::optional<std::regex> pattern_std;
    bool pattern_engine_ready = false;
    bool strip_whitespace = false;
    bool to_lower = false;
    bool to_upper = false;
    bool ascii_only = false;
    mutable std::string last_type_name_;

    StrConstrainedValidator() = default;

    // The reference compiles a text pattern with the regex crate.  std::regex is asked first
    // here -- it is the engine that reads a POSIX class like [[:alpha:]] the same way -- and
    // python re, which the port already uses for a pattern the schema brings compiled, takes
    // what std::regex refuses to parse.  That is where inline flags and the absolute-start
    // anchor live: (?i)a means case-insensitive to the reference, while an engine that cannot
    // parse it used to leave the constraint dropped and the field accepting everything.
    // A pattern neither engine compiles is left to constrain nothing, as it always was: the
    // reference builds schemas this one cannot parse (\p{Greek}, \z), and refusing those
    // would break a model the reference validates.
    void prepare_pattern_engine() {
        pattern_engine_ready = true;
        if (pattern.empty() || (pattern_re.ptr() && !pattern_re.is_none())) return;
        try {
            pattern_std.emplace(pattern);
            return;
        } catch (const std::regex_error&) {
        }
        try {
            pattern_re = py::module_::import("re").attr("compile")(pattern);
        } catch (const py::error_already_set&) {
            PyErr_Clear();
        }
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto result = input.validate_str(state.strict_or(strict), coerce_numbers_to_str,
                                       state.input_type() == InputType::Json);
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        // Every check below measures the trimmed text but names the value that came in: Rust
        // hands `input` to each error (string.rs:125-166), not the string it rewrote.
        const std::string err_input = input.as_error_value().repr;
        const py::object err_object = input.as_python_object();
        std::string str = result.value().value().to_string();

        if (strip_whitespace) trim_unicode_whitespace(str);

        // Rust checks ascii_only right after stripping and before the lengths.
        if (ascii_only) {
            for (unsigned char c : str) {
                if (c > 127) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::StringNotAscii),
                        state.location(),
                        err_input,
                        err_object
                    );
                }
            }
        }

        // Rust measures the string as str.chars().count() - Unicode scalars, so
        // a snowman counts once however many bytes its UTF-8 form needs.
        size_t char_count = 0;
        for (unsigned char c : str) {
            if ((c & 0xC0) != 0x80) ++char_count;
        }
        if (min_length.has_value() && char_count < min_length.value()) {
            std::string s = min_length.value() == 1 ? "" : "s";
            return ValError::line_error(
                ErrorType(ErrorType::Kind::StringTooShort, "min_length", std::to_string(min_length.value()), "s", s),
                state.location(),
                err_input,
                err_object
            );
        }
        if (max_length.has_value() && char_count > max_length.value()) {
            std::string s = max_length.value() == 1 ? "" : "s";
            return ValError::line_error(
                ErrorType(ErrorType::Kind::StringTooLong, "max_length", std::to_string(max_length.value()), "s", s),
                state.location(),
                err_input,
                err_object
            );
        }

        // The pattern is asked of the trimmed text before any case map, so a `^A` pattern
        // fails on 'a' however to_upper is set (string.rs:157-166).
        // Pattern matching: python re for a compiled pattern, std::regex otherwise
        if (!pattern.empty() && pattern_re.ptr() && !pattern_re.is_none()) {
            bool matched = false;
            try {
                matched = !pattern_re.attr("search")(str).is_none();
            } catch (const py::error_already_set&) {
                PyErr_Clear();
                matched = true;
            }
            if (!matched) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::StringPatternMismatch, "pattern", pattern),
                    state.location(),
                    err_input,
                    err_object
                );
            }
        } else if (!pattern.empty()) {
            // The engine for a text pattern is chosen once, when the schema is built.  A
            // validator assembled by hand -- the C++ tests do -- still compiles per run the
            // way this always did, and a pattern neither engine compiles still constrains
            // nothing: the port has never refused a schema for that, and refusing one the
            // reference builds would be worse than the hole it closes.
            try {
                std::optional<std::regex> hand;
                const std::regex* re = pattern_std ? &*pattern_std : nullptr;
                if (!re && !pattern_engine_ready) {
                    hand.emplace(pattern);
                    re = &*hand;
                }
                if (re && !std::regex_search(str, *re)) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::StringPatternMismatch, "pattern", pattern),
                        state.location(),
                        err_input,
                        err_object
                    );
                }
            } catch (const std::regex_error&) {
                // Invalid regex, skip pattern check
            }
        }

        // Last comes the case map, and it is Rust's `else if` chain: to_lower wins over
        // to_upper rather than both running (string.rs:169-178).  The mapping is Unicode's
        // full one -- 'ß' becomes "SS", 'İ' becomes "i" plus a combining dot, a final sigma
        // gets its own letter -- which is what Python's own str.map already implements, byte
        // for byte the same SpecialCasing table.
        if (to_lower || to_upper) {
            try {
                str = py::str(str).attr(to_lower ? "lower" : "upper")().cast<std::string>();
            } catch (const py::error_already_set&) {
                PyErr_Clear();
            }
        }

        // Rust's chain ends in `else { either_str.as_py_string(...) }` (string.rs:176-178): when
        // no case map and no trim ran the value is still the input's own text and comes back as
        // the input object -- a different pointee, so a different reported name.
        if (!to_lower && !to_upper && !strip_whitespace && input_is_exact_str(input)) {
            last_type_name_ = "py_object";
            return ValResult<std::shared_ptr<void>>(
                std::make_shared<py::object>(input.as_python_object()));
        }
        last_type_name_.clear();
        return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(std::move(str)));
    }

    std::string name() const override { return "constrained-str"; }

    std::string effective_result_name() const override {
        return last_type_name_.empty() ? std::string("constrained-str") : last_type_name_;
    }

    void visit_refs(RefVisitor visit, void* arg) const override {
        visit_ref(visit, arg, pattern_re);
    }
};

// BytesValidator - validates bytes values
class BytesValidator : public Validator {
public:
    std::optional<bool> strict;
    std::string val_json_bytes = "utf8";  // "utf8", "base64", or "hex"

    BytesValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // A value that came out of a JSON document decodes whatever the strict,
        // because JSON has no str/bytes distinction (input_json.rs:135).
        const bool json_document = state.input_type() == InputType::Json;
        auto result =
            input.validate_bytes(state.strict_or_declared(strict), val_json_bytes, json_document);
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<EitherBytes>(result.value().value())
        );
    }

    std::string name() const override { return "bytes"; }
};

// BytesConstrainedValidator - validates bytes with min_length/max_length
class BytesConstrainedValidator : public Validator {
public:
    std::optional<bool> strict;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    // Rust builds both bytes validators with the same ValBytesMode, so the
    // length checks run on the decoded bytes.
    std::string val_json_bytes = "utf8";

    BytesConstrainedValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        const bool json_document = state.input_type() == InputType::Json;
        auto result =
            input.validate_bytes(state.strict_or_declared(strict), val_json_bytes, json_document);
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        auto& bytes = result.value().value();
        size_t len = bytes.size();

        if (min_length.has_value() && len < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::BytesTooShort, "min_length", std::to_string(min_length.value())),
                state.location(),
                input.as_error_value().repr
            );
        }
        if (max_length.has_value() && len > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::BytesTooLong, "max_length", std::to_string(max_length.value())),
                state.location(),
                input.as_error_value().repr
            );
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<EitherBytes>(bytes));
    }

    std::string name() const override { return "constrained-bytes"; }

};

// IsInstanceValidator - validates that input is an instance of a given Python class
class IsInstanceValidator : public Validator {
public:
    IsInstanceValidator() : py_class_(py::none()) {}
    IsInstanceValidator(std::string class_name, py::object py_class)
        : class_name_(std::move(class_name)), py_class_(std::move(py_class)) {}

    void set_class_name(const std::string& name) { class_name_ = name; }
    void set_py_class(py::object cls) { py_class_ = std::move(cls); }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Rust cannot apply isinstance to a value that only exists in JSON, so
        // the check asks the input itself whether it is one, not whether the
        // document came from JSON: the port parses JSON before validating, so
        // those values really are Python objects by now.
        if (input.input_type() != InputType::Python) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::NeedsPythonObject, "method_name", "isinstance"),
                state.location(), input.as_error_value().repr);
        }
        // Check if input is an instance of the specified Python class
        if (!py_class_.is_none()) {
            try {
                py::object input_py = input.as_python_object();
                if (!input_py) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                        state.location(),
                        input.as_error_value().repr
                    );
                }
                if (!py::isinstance(input_py, py_class_)) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                        state.location(),
                        input.as_error_value().repr
                    );
                }
                // Store as PyObject* — leak the reference to avoid GIL issues
                PyObject* raw = input_py.inc_ref().ptr();
                return ValResult<std::shared_ptr<void>>(
                    std::shared_ptr<void>(raw, [](void*){})
                );
            } catch (const std::exception& e) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                    state.location(),
                    std::string("Error checking isinstance: ") + e.what()
                );
            } catch (...) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                    state.location(),
                    "Unknown error checking isinstance"
                );
            }
        }
        // Fallback: if class name was specified but we don't have the Python class,
        // return error so UnionValidator can try other choices
        if (!class_name_.empty() && py_class_.is_none()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IsInstanceType, "class", class_name_),
                state.location(),
                input.as_error_value().repr
            );
        }
        // Accept if no class info at all — return the input as an honest
        // py_raw_object payload so result conversion stays type-safe.
        PyObject* passthrough = input.as_python_object().inc_ref().ptr();
        return ValResult<std::shared_ptr<void>>(
            std::shared_ptr<void>(passthrough, [](void*){}));
    }

    std::string name() const override { return "py_raw_object"; }

    // Rust names the validator after the class it checks, and that name is
    // the label every error inside it reports.
    std::string display_name() const override {
        return "is-instance[" + class_name_ + "]";
    }

    void visit_refs(RefVisitor visit, void* arg) const override {
        visit_ref(visit, arg, py_class_);
    }
private:
    std::string class_name_;
    py::object py_class_;
};

// IsSubclassValidator - validates that input is a subclass of a given Python class
class IsSubclassValidator : public Validator {
public:
    IsSubclassValidator() : py_class_(py::none()) {}
    IsSubclassValidator(std::string class_name, py::object py_class)
        : class_name_(std::move(class_name)), py_class_(std::move(py_class)) {}

    void set_class_name(const std::string& name) { class_name_ = name; }
    void set_py_class(py::object cls) { py_class_ = std::move(cls); }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // issubclass cannot be evaluated against a JSON value either.
        if (state.input_type() != InputType::Python) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::NeedsPythonObject, "method_name", "issubclass"),
                state.location(), input.as_error_value().repr);
        }
        // Check if input is a subclass of the specified Python class
        // Input must be a type/class itself
        if (!py_class_.is_none()) {
            py::object input_py = input.as_python_object();
            try {
                py::object type_obj = py::module_::import("builtins").attr("type");
                if (!py::isinstance(input_py, type_obj)) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::IsSubclassType, "class", class_name_),
                        state.location(),
                        input.as_error_value().repr,
                        input.as_python_object()
                    );
                }
                // Check subclass relationship using Python's issubclass()
                py::object builtins = py::module_::import("builtins");
                py::bool_ is_subclass = builtins.attr("issubclass")(input_py, py_class_);
                if (!is_subclass.cast<bool>()) {
                    return ValError::line_error(
                        ErrorType(ErrorType::Kind::IsSubclassType, "class", class_name_),
                        state.location(),
                        input.as_error_value().repr,
                        input.as_python_object()
                    );
                }
                // Store as PyObject* — leak the reference to avoid GIL issues
                // during shared_ptr destruction. The py::object returned by
                // result_to_python_with_type will hold its own reference.
                PyObject* raw = input_py.inc_ref().ptr();
                return ValResult<std::shared_ptr<void>>(
                    std::shared_ptr<void>(raw, [](void*){})
                );
            } catch (py::error_already_set& e) {
                std::string msg = e.what();
                e.restore();
                PyErr_Clear();
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::IsSubclassType, "class", class_name_),
                    state.location(),
                    input.as_error_value().repr,
                    input.as_python_object()
                );
            }
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }

    std::string name() const override { return "py_raw_object"; }

    // Rust names the validator after the class it checks, and that name is
    // the label every error inside it reports.
    std::string display_name() const override {
        return "is-subclass[" + class_name_ + "]";
    }

    void visit_refs(RefVisitor visit, void* arg) const override {
        visit_ref(visit, arg, py_class_);
    }
private:
    std::string class_name_;
    py::object py_class_;
};

// CallableValidator - validates that input is callable
class CallableValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Check if input is callable
        py::object input_py = input.as_python_object();
        if (!py_hasattr(input_py, "__call__")) {
            // Rust builds the line error from the input itself, so the error
            // reports the rejected value rather than a fixed sentence.
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CallableType),
                state.location(),
                input.as_error_value().repr,
                input_py
            );
        }
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<py::object>(input_py)
        );
    }

    std::string name() const override { return "callable"; }
};

// MissingSentinelValidator - validates the MISSING sentinel, and whatever the
// inner schema accepts for every other input. `int | MISSING` becomes a
// 'missing-sentinel' schema that wraps the `int` schema, so without the inner
// validator every non-sentinel input is refused.
class MissingSentinelValidator : public Validator {
public:
    MissingSentinelValidator() = default;
    explicit MissingSentinelValidator(std::shared_ptr<Validator> inner)
        : inner_(std::move(inner)) {}

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Result conversion dispatches on the reported type name, and the two
        // branches below produce different pointee types: the sentinel is a live
        // py::object, anything else is the inner validator's own value.
        last_type_name_.clear();

        py::object input_py = input.as_python_object();
        py::object missing = py::module_::import("pydantic_core_cpp").attr("MISSING");
        if (input_py.is(missing)) {
            last_type_name_ = "py_object";
            return ValResult<std::shared_ptr<void>>(
                std::make_shared<py::object>(input_py)
            );
        }
        // Without an inner schema the sentinel is the only valid input.
        if (!inner_) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::MissingSentinelError),
                state.location(),
                input.as_error_value().repr
            );
        }
        auto r = inner_->validate(input, state);
        if (r.is_ok()) last_type_name_ = inner_->effective_result_name();
        return r;
    }

    // Structural name, kept marker-free like the other wrappers: result
    // conversion adds exactly one "maybe_wrapper:" marker itself, and a name
    // that already carries one matches no handler.
    std::string name() const override {
        return inner_ ? inner_->name() : "py_object";
    }

    std::string effective_result_name() const override {
        if (!last_type_name_.empty()) return last_type_name_;
        return inner_ ? inner_->effective_result_name() : "py_object";
    }

    std::string display_name() const override {
        return inner_ ? "missing-sentinel[" + inner_->display_name() + "]" : "missing-sentinel";
    }

    void visit_refs(RefVisitor visit, void* arg) const override {
        if (!gc_detail::enter_node(this)) return;
        if (inner_) inner_->visit_refs(visit, arg);
    }

private:
    std::shared_ptr<Validator> inner_;
    std::string last_type_name_;
};

// EllipsisValidator - validates the `...` literal itself. Rust compares the
// input by identity with Ellipsis (ellipsis.rs:39-42), so a value that merely
// equals Ellipsis in some other spelling -- and every JSON input, which has no
// spelling for it at all -- is refused.
class EllipsisValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        py::object input_py = input.as_python_object();
        if (input_py.is(py::ellipsis())) {
            return ValResult<std::shared_ptr<void>>(
                std::make_shared<py::object>(input_py)
            );
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::EllipsisError),
            state.location(),
            input.as_error_value().repr
        );
    }

    std::string name() const override { return "py_object"; }
    std::string display_name() const override { return "ellipsis"; }
    std::string debug_repr() const override { return "Ellipsis(EllipsisValidator)"; }
};

} // namespace pydantic_core
