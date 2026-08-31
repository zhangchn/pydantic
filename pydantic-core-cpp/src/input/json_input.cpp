#include "pydantic_core/json_input.hpp"
#include "pydantic_core/result.hpp"
#include "pydantic_core/error_types.hpp"
#include <simdjson.h>
#include <memory>
#include <sstream>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace pydantic_core {

// Helper: convert simdjson element to Python object
static py::object json_element_to_py(const simdjson::dom::element& elem) {
    auto type = elem.type();
    switch (type) {
        case simdjson::dom::element_type::STRING:
            return py::str(std::string(elem.get_string().value()));
        case simdjson::dom::element_type::INT64:
            return py::int_(elem.get_int64().value());
        case simdjson::dom::element_type::UINT64:
            return py::int_(static_cast<int64_t>(elem.get_uint64().value()));
        case simdjson::dom::element_type::DOUBLE:
            return py::float_(elem.get_double().value());
        case simdjson::dom::element_type::BOOL:
            return py::bool_(elem.get_bool().value());
        case simdjson::dom::element_type::NULL_VALUE:
            return py::none();
        case simdjson::dom::element_type::ARRAY: {
            py::list lst;
            for (auto item : elem.get_array().value()) {
                lst.append(json_element_to_py(item));
            }
            return lst;
        }
        case simdjson::dom::element_type::OBJECT: {
            py::dict d;
            for (auto [key, value] : elem.get_object().value()) {
                d[py::str(std::string(key))] = json_element_to_py(value);
            }
            return d;
        }
        default:
            return py::none();
    }
}

JsonInput::JsonInput(simdjson::simdjson_result<simdjson::dom::element> element) {
    parser_ = std::make_unique<simdjson::dom::parser>();
    auto err = element.get(element_);
    if (err) {
        element_ = simdjson::dom::element();
    }
}

JsonInput::JsonInput(const simdjson::dom::element& element) : element_(element) {
    // For this constructor, we assume the parser is managed elsewhere
    // This is a shallow copy - use with caution
}

std::unique_ptr<JsonInput> JsonInput::create_from_element(const simdjson::dom::element& element) {
    auto input = std::make_unique<JsonInput>(element);
    // parser_ stays null - caller must ensure element's parser stays alive
    return input;
}

InputValue JsonInput::as_error_value() const {
    auto type = element_.type();
    switch (type) {
        case simdjson::dom::element_type::STRING:
            return InputValue("'" + std::string(element_.get_string().value()) + "'");
        case simdjson::dom::element_type::INT64:
            return InputValue(std::to_string(element_.get_int64().value()));
        case simdjson::dom::element_type::UINT64:
            return InputValue(std::to_string(element_.get_uint64().value()));
        case simdjson::dom::element_type::DOUBLE:
            return InputValue(std::to_string(element_.get_double().value()));
        case simdjson::dom::element_type::BOOL:
            return InputValue(element_.get_bool().value() ? "True" : "False");
        case simdjson::dom::element_type::NULL_VALUE:
            return InputValue("null");
        case simdjson::dom::element_type::ARRAY:
            return InputValue("[...]");
        case simdjson::dom::element_type::OBJECT:
            return InputValue("{...}");
        default:
            return InputValue("unknown");
    }
}

bool JsonInput::is_none() const {
    return element_.type() == simdjson::dom::element_type::NULL_VALUE;
}

py::object JsonInput::as_python_object() const {
    return json_element_to_py(element_);
}

ValResult<ValMatch<EitherString>> JsonInput::validate_str(bool strict, bool coerce_numbers) const {
    if (element_.type() == simdjson::dom::element_type::STRING) {
        auto str_result = element_.get_string();
        if (!str_result.error()) {
            return ValMatch<EitherString>::exact(EitherString(std::string(str_result.value_unsafe())));
        }
    }
    
    // Lax mode - coerce from numbers
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::INT64) {
            return ValMatch<EitherString>::lax(EitherString(std::to_string(element_.get_int64().value_unsafe())));
        }
        if (element_.type() == simdjson::dom::element_type::UINT64) {
            return ValMatch<EitherString>::lax(EitherString(std::to_string(element_.get_uint64().value_unsafe())));
        }
        if (element_.type() == simdjson::dom::element_type::DOUBLE) {
            return ValMatch<EitherString>::lax(EitherString(std::to_string(element_.get_double().value_unsafe())));
        }
        if (element_.type() == simdjson::dom::element_type::BOOL) {
            return ValMatch<EitherString>::lax(EitherString(element_.get_bool().value_unsafe() ? std::string("true") : std::string("false")));
        }
    }
    
    return ValError::line_error(PydanticKnownError::string_type(), 
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<EitherBytes>> JsonInput::validate_bytes(bool strict) const {
    // JSON doesn't have native bytes type - treat string as bytes in lax mode
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = element_.get_string().value_unsafe();
            return ValMatch<EitherBytes>::lax(EitherBytes(std::string_view(str)));
        }
    }
    
    return ValError::line_error(PydanticKnownError::bytes_type(),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<bool>> JsonInput::validate_bool(bool strict) const {
    if (element_.type() == simdjson::dom::element_type::BOOL) {
        return ValMatch<bool>::exact(element_.get_bool().value_unsafe());
    }

    // Lax mode - coerce from strings/ints (Rust: str_as_bool / int_as_bool)
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::INT64) {
            int64_t i = element_.get_int64().value_unsafe();
            if (i == 0) return ValMatch<bool>::lax(false);
            if (i == 1) return ValMatch<bool>::lax(true);
            return ValError::line_error(ErrorType(ErrorType::Kind::BoolParsing),
                                       Location(), as_error_value().repr);
        }
        if (element_.type() == simdjson::dom::element_type::UINT64) {
            uint64_t i = element_.get_uint64().value_unsafe();
            if (i == 0) return ValMatch<bool>::lax(false);
            if (i == 1) return ValMatch<bool>::lax(true);
            return ValError::line_error(ErrorType(ErrorType::Kind::BoolParsing),
                                       Location(), as_error_value().repr);
        }
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = std::string(element_.get_string().value_unsafe());
            std::string lower = str;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (str == "0" || lower == "f" || lower == "n" || lower == "no" ||
                lower == "off" || lower == "false") {
                return ValMatch<bool>::lax(false);
            }
            if (str == "1" || lower == "t" || lower == "y" || lower == "on" ||
                lower == "yes" || lower == "true") {
                return ValMatch<bool>::lax(true);
            }
            return ValError::line_error(ErrorType(ErrorType::Kind::BoolParsing),
                                       Location(), as_error_value().repr);
        }
        if (element_.type() == simdjson::dom::element_type::DOUBLE) {
            double d = element_.get_double().value_unsafe();
            if (std::isfinite(d) && std::floor(d) == d) {
                int64_t i = static_cast<int64_t>(d);
                if (i == 0) return ValMatch<bool>::lax(false);
                if (i == 1) return ValMatch<bool>::lax(true);
            }
            return ValError::line_error(ErrorType(ErrorType::Kind::BoolParsing),
                                       Location(), as_error_value().repr);
        }
    }

    return ValError::line_error(PydanticKnownError::bool_type(),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<EitherInt>> JsonInput::validate_int(bool strict) const {
    if (element_.type() == simdjson::dom::element_type::INT64) {
        return ValMatch<EitherInt>::exact(EitherInt(element_.get_int64().value_unsafe()));
    }
    if (element_.type() == simdjson::dom::element_type::UINT64) {
        return ValMatch<EitherInt>::exact(EitherInt(element_.get_uint64().value_unsafe()));
    }
    
    // Lax mode - coerce from float/string
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::DOUBLE) {
            double d = element_.get_double().value_unsafe();
            if (std::floor(d) == d) {
                return ValMatch<EitherInt>::lax(EitherInt(static_cast<int64_t>(d)));
            }
        }
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = std::string(element_.get_string().value_unsafe());
            try {
                if (str.find('.') == std::string::npos) {
                    int64_t i = std::stoll(str);
                    return ValMatch<EitherInt>::lax(EitherInt(i));
                }
            } catch (...) {}
        }
        if (element_.type() == simdjson::dom::element_type::BOOL) {
            return ValMatch<EitherInt>::lax(EitherInt(static_cast<int64_t>(element_.get_bool().value_unsafe())));
        }
    }
    
    return ValError::line_error(PydanticKnownError::int_type(),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<EitherFloat>> JsonInput::validate_float(bool strict) const {
    if (element_.type() == simdjson::dom::element_type::DOUBLE) {
        return ValMatch<EitherFloat>::exact(EitherFloat(element_.get_double().value_unsafe()));
    }
    
    // Ints are valid floats
    if (element_.type() == simdjson::dom::element_type::INT64) {
        double d = static_cast<double>(element_.get_int64().value_unsafe());
        if (strict) {
            return ValMatch<EitherFloat>::exact(EitherFloat(d));
        }
        return ValMatch<EitherFloat>::lax(EitherFloat(d));
    }
    if (element_.type() == simdjson::dom::element_type::UINT64) {
        double d = static_cast<double>(element_.get_uint64().value_unsafe());
        if (strict) {
            return ValMatch<EitherFloat>::exact(EitherFloat(d));
        }
        return ValMatch<EitherFloat>::lax(EitherFloat(d));
    }
    
    // Lax mode - string coercion
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = std::string(element_.get_string().value_unsafe());
            try {
                double d = std::stod(str);
                return ValMatch<EitherFloat>::lax(EitherFloat(d));
            } catch (...) {}
        }
    }
    
    return ValError::line_error(PydanticKnownError::float_type(),
                               Location(), as_error_value().repr);
}

ValResult<std::unique_ptr<ValidatedDict>> JsonInput::validate_dict(bool strict) const {
    if (element_.type() == simdjson::dom::element_type::OBJECT) {
        auto obj = element_.get_object();
        if (!obj.error()) {
            return ValResult<std::unique_ptr<ValidatedDict>>(
                std::unique_ptr<ValidatedDict>(std::make_unique<JsonValidatedDict>(obj.value_unsafe()).release()));
        }
    }
    
    return ValError::line_error(PydanticKnownError::dict_type(),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<std::unique_ptr<ValidatedList>>> JsonInput::validate_list(bool strict) const {
    if (element_.type() == simdjson::dom::element_type::ARRAY) {
        auto arr = element_.get_array();
        if (!arr.error()) {
            return ValMatch<std::unique_ptr<ValidatedList>>::exact(
                std::make_unique<JsonValidatedList>(arr.value_unsafe()));
        }
    }
    
    return ValError::line_error(PydanticKnownError::list_type(),
                               Location(), as_error_value().repr);
}

ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> JsonInput::validate_tuple(bool strict) const {
    // JSON doesn't distinguish tuples - treat arrays as tuples in lax mode
    if (element_.type() == simdjson::dom::element_type::ARRAY) {
        auto arr = element_.get_array();
        if (!arr.error()) {
            return ValMatch<std::unique_ptr<ValidatedTuple>>::lax(
                std::make_unique<JsonValidatedTuple>(arr.value_unsafe()));
        }
    }
    
    return ValError::line_error(PydanticKnownError::tuple_type(),
                               Location(), as_error_value().repr);
}

// ============================================================================
// ISO 8601 parsing helper
// ============================================================================

static std::optional<DateTime> try_parse_iso8601(const std::string& s) {
    try {
        if (s.size() < 19) return std::nullopt;
        if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') {
            return std::nullopt;
        }
        int year = std::stoi(s.substr(0, 4));
        int month = std::stoi(s.substr(5, 2));
        int day = std::stoi(s.substr(8, 2));
        int hour = std::stoi(s.substr(11, 2));
        int minute = std::stoi(s.substr(14, 2));
        int second = std::stoi(s.substr(17, 2));

        int microsecond = 0;
        size_t pos = 19;

        if (pos < s.size() && s[pos] == '.') {
            std::string frac;
            pos++;
            while (pos < s.size() && std::isdigit(s[pos])) {
                frac += s[pos];
                pos++;
            }
            if (frac.size() > 6) frac = frac.substr(0, 6);
            while (frac.size() < 6) frac += '0';
            if (!frac.empty()) microsecond = std::stoi(frac);
        }

        std::optional<int> tz_offset;
        if (pos < s.size()) {
            if (s[pos] == 'Z') {
                tz_offset = 0;
            } else if (s[pos] == '+' || s[pos] == '-') {
                char sign = s[pos];
                pos++;
                if (pos + 4 < s.size() && s[pos + 2] == ':') {
                    int tz_hour = std::stoi(s.substr(pos, 2));
                    int tz_min = std::stoi(s.substr(pos + 3, 2));
                    int offset = tz_hour * 60 + tz_min;
                    tz_offset = (sign == '-') ? -offset : offset;
                }
            }
        }

        return DateTime{Date{year, month, day}, Time{hour, minute, second, microsecond, tz_offset}};
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// Date/time validation
// ============================================================================

ValResult<ValMatch<EitherDate>> JsonInput::validate_date(bool strict) const {
    if (!element_.is_string()) {
        return type_error(ErrorType::Kind::DateType, *this, this->current_location());
    }
    std::string_view sv;
    if (element_.get_string().get(sv)) {
        return type_error(ErrorType::Kind::DateType, *this, this->current_location());
    }
    std::string s(sv);

    // Parse as date
    try {
        if (s.size() >= 10 && s[4] == '-' && s[7] == '-') {
            int year = std::stoi(s.substr(0, 4));
            int month = std::stoi(s.substr(5, 2));
            int day = std::stoi(s.substr(8, 2));
            if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
                if (s.size() > 10) {
                    auto dt = try_parse_iso8601(s);
                    if (dt && dt->time.hour == 0 && dt->time.minute == 0 &&
                        dt->time.second == 0 && dt->time.microsecond == 0) {
                        return ValMatch<EitherDate>::lax(EitherDate(dt->date));
                    }
                    if (dt) {
                        return ValError::line_error(
                            ErrorType(ErrorType::Kind::DateFromDatetimeInexact),
                            this->current_location(), this->as_error_value().repr
                        );
                    }
                }
                return ValMatch<EitherDate>::lax(EitherDate(Date{year, month, day}));
            }
        }
    } catch (...) {}
    return ValError::line_error(
        ErrorType(ErrorType::Kind::DateParsing),
        this->current_location(), this->as_error_value().repr
    );
}

ValResult<ValMatch<EitherDateTime>> JsonInput::validate_datetime(bool strict) const {
    if (!element_.is_string()) {
        return type_error(ErrorType::Kind::DateTimeType, *this, this->current_location());
    }
    std::string_view sv;
    if (element_.get_string().get(sv)) {
        return type_error(ErrorType::Kind::DateTimeType, *this, this->current_location());
    }
    std::string s(sv);

    auto parsed = try_parse_iso8601(s);
    if (parsed) {
        return ValMatch<EitherDateTime>::lax(EitherDateTime(*parsed));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::DateTimeParsing),
        this->current_location(), this->as_error_value().repr
    );
}

ValResult<ValMatch<EitherTime>> JsonInput::validate_time(bool strict) const {
    if (!element_.is_string()) {
        return type_error(ErrorType::Kind::TimeType, *this, this->current_location());
    }
    std::string_view sv;
    if (element_.get_string().get(sv)) {
        return type_error(ErrorType::Kind::TimeType, *this, this->current_location());
    }
    std::string s(sv);

    try {
        if (s.size() >= 8 && s[2] == ':' && s[5] == ':') {
            int hour = std::stoi(s.substr(0, 2));
            int minute = std::stoi(s.substr(3, 2));
            int second = std::stoi(s.substr(6, 2));
            int microsecond = 0;
            if (s.size() > 8 && s[8] == '.') {
                std::string frac = s.substr(9);
                if (frac.size() > 6) frac = frac.substr(0, 6);
                while (frac.size() < 6) frac += '0';
                microsecond = std::stoi(frac);
            }
            return ValMatch<EitherTime>::lax(EitherTime(Time{hour, minute, second, microsecond, std::nullopt}));
        }
    } catch (...) {}
    return ValError::line_error(
        ErrorType(ErrorType::Kind::TimeParsing),
        this->current_location(), this->as_error_value().repr
    );
}

ValResult<ValMatch<EitherTimedelta>> JsonInput::validate_timedelta(bool strict) const {
    if (!element_.is_string()) {
        return type_error(ErrorType::Kind::TimedeltaType, *this, this->current_location());
    }
    std::string_view sv;
    if (element_.get_string().get(sv)) {
        return type_error(ErrorType::Kind::TimedeltaType, *this, this->current_location());
    }
    std::string s(sv);
    auto parsed = try_parse_timedelta_str(s);
    if (parsed) {
        return ValMatch<EitherTimedelta>::lax(EitherTimedelta(*parsed));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::TimedeltaParsing),
        this->current_location(), this->as_error_value().repr
    );
}

// JsonValidatedDict implementation
std::vector<ValidatedDict::Entry> JsonValidatedDict::entries() const {
    std::vector<Entry> result;
    for (auto& [key, value] : obj_) {
        Entry e;
        e.key = std::string(key);
        std::ostringstream oss;
        auto type = value.type();
        if (type == simdjson::dom::element_type::STRING) {
            e.value_repr = "'" + std::string(value.get_string().value_unsafe()) + "'";
        } else {
            e.value_repr = "...";
        }
        result.push_back(e);
    }
    return result;
}

std::vector<std::string> JsonValidatedDict::keys() const {
    std::vector<std::string> result;
    for (auto& [key, _] : obj_) {
        result.push_back(std::string(key));
    }
    return result;
}

bool JsonValidatedDict::has_key(const std::string& key) const {
    for (auto& [k, _] : obj_) {
        if (std::string(k) == key) {
            return true;
        }
    }
    return false;
}

std::optional<ValidatedDict::Entry> JsonValidatedDict::get(const std::string& key) const {
    for (auto& [k, value] : obj_) {
        if (std::string(k) == key) {
            Entry e;
            e.key = key;
            std::ostringstream oss;
            auto type = value.type();
            if (type == simdjson::dom::element_type::STRING) {
                e.value_repr = "'" + std::string(value.get_string().value_unsafe()) + "'";
            } else {
                e.value_repr = "...";
            }
            return e;
        }
    }
    return std::nullopt;
}

std::optional<simdjson::dom::element> JsonValidatedDict::get_element(const std::string& key) const {
    for (auto& [k, value] : obj_) {
        if (std::string(k) == key) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<py::object> JsonValidatedDict::get_value(const std::string& key) const {
    auto elem = get_element(key);
    if (elem) {
        return json_element_to_py(*elem);
    }
    return std::nullopt;
}

std::optional<py::object> JsonValidatedDict::get_key(const std::string& key) const {
    // JSON object keys are always strings
    return py::str(key);
}

// JsonValidatedList implementation
std::vector<ValidatedList::Entry> JsonValidatedList::entries() const {
    std::vector<Entry> result;
    size_t idx = 0;
    for (const auto& item : arr_) {
        Entry e;
        e.index = idx++;
        std::ostringstream oss;
        auto type = item.type();
        if (type == simdjson::dom::element_type::STRING) {
            e.value_repr = "'" + std::string(item.get_string().value_unsafe()) + "'";
        } else {
            e.value_repr = "...";
        }
        result.push_back(e);
    }
    return result;
}

py::object JsonValidatedList::get_item(size_t index) const {
    size_t idx = 0;
    for (const auto& item : arr_) {
        if (idx == index) {
            switch (item.type()) {
                case simdjson::dom::element_type::STRING:
                    return py::str(std::string(item.get_string().value_unsafe()));
                case simdjson::dom::element_type::INT64:
                    return py::int_(item.get_int64().value_unsafe());
                case simdjson::dom::element_type::UINT64:
                    return py::int_(item.get_uint64().value_unsafe());
                case simdjson::dom::element_type::DOUBLE:
                    return py::float_(item.get_double().value_unsafe());
                case simdjson::dom::element_type::BOOL:
                    return py::bool_(item.get_bool().value_unsafe());
                case simdjson::dom::element_type::NULL_VALUE:
                    return py::none();
                case simdjson::dom::element_type::ARRAY:
                    return py::list();
                case simdjson::dom::element_type::OBJECT:
                    return py::dict();
                default:
                    return py::str("unknown");
            }
        }
        idx++;
    }
    return py::none();
}

// JsonValidatedTuple implementation
std::vector<ValidatedList::Entry> JsonValidatedTuple::entries() const {
    std::vector<Entry> result;
    size_t idx = 0;
    for (const auto& item : arr_) {
        Entry e;
        e.index = idx++;
        std::ostringstream oss;
        auto type = item.type();
        if (type == simdjson::dom::element_type::STRING) {
            e.value_repr = "'" + std::string(item.get_string().value_unsafe()) + "'";
        } else {
            e.value_repr = "...";
        }
        result.push_back(e);
    }
    return result;
}

py::object JsonValidatedTuple::get_item(size_t index) const {
    size_t idx = 0;
    for (const auto& item : arr_) {
        if (idx == index) {
            switch (item.type()) {
                case simdjson::dom::element_type::STRING:
                    return py::str(std::string(item.get_string().value_unsafe()));
                case simdjson::dom::element_type::INT64:
                    return py::int_(item.get_int64().value_unsafe());
                case simdjson::dom::element_type::UINT64:
                    return py::int_(item.get_uint64().value_unsafe());
                case simdjson::dom::element_type::DOUBLE:
                    return py::float_(item.get_double().value_unsafe());
                case simdjson::dom::element_type::BOOL:
                    return py::bool_(item.get_bool().value_unsafe());
                case simdjson::dom::element_type::NULL_VALUE:
                    return py::none();
                case simdjson::dom::element_type::ARRAY:
                    return py::list();
                case simdjson::dom::element_type::OBJECT:
                    return py::dict();
                default:
                    return py::str("unknown");
            }
        }
        idx++;
    }
    return py::none();
}

// JSON parsing function
ValResult<std::unique_ptr<JsonInput>> parse_json(std::string_view json_str) {
    auto parser = std::make_unique<simdjson::dom::parser>();
    auto result = parser->parse(json_str);
    if (result.error()) {
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            Location(),
            std::string(json_str.substr(0, std::min(json_str.size(), size_t(50))))
        );
    }
    // Create JsonInput with the parser
    auto input = std::make_unique<JsonInput>(result.value());
    input->parser_ = std::move(parser);
    return input;
}

} // namespace pydantic_core