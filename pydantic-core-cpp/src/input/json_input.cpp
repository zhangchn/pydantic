#include "pydantic_core/json_input.hpp"
#include "pydantic_core/result.hpp"
#include "pydantic_core/error_types.hpp"
#include <simdjson.h>
#include <cstring>
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
// Date/time validation
// ============================================================================
//
// Parsing is delegated to the speedate port (speedate.hpp). JSON has no native
// temporal type, so only strings reach these parsers.

ValResult<ValMatch<EitherDate>> JsonInput::validate_date(bool strict, TimestampUnit unit) const {
    if (!element_.is_string()) {
        return type_error(ErrorType::Kind::DateType, *this, this->current_location());
    }
    std::string_view sv;
    if (element_.get_string().get(sv)) {
        return type_error(ErrorType::Kind::DateType, *this, this->current_location());
    }
    auto parsed = parse_date_bytes(sv.data(), sv.size(), unit);
    if (parsed.ok) {
        return ValMatch<EitherDate>::lax(EitherDate(parsed.value));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::DateParsing, "error", parsed.error),
        this->current_location(), this->as_error_value().repr
    );
}

ValResult<ValMatch<EitherDateTime>> JsonInput::validate_datetime(
    bool strict, TimestampUnit unit) const {
    if (!element_.is_string()) {
        return type_error(ErrorType::Kind::DateTimeType, *this, this->current_location());
    }
    std::string_view sv;
    if (element_.get_string().get(sv)) {
        return type_error(ErrorType::Kind::DateTimeType, *this, this->current_location());
    }
    auto parsed = parse_datetime_bytes(sv.data(), sv.size(), unit);
    if (parsed.ok) {
        return ValMatch<EitherDateTime>::lax(EitherDateTime(parsed.value));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::DateTimeParsing, "error", parsed.error),
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
    auto parsed = parse_time_bytes(sv.data(), sv.size());
    if (parsed.ok) {
        return ValMatch<EitherTime>::lax(EitherTime(parsed.value));
    }
    return ValError::line_error(
        ErrorType(ErrorType::Kind::TimeParsing, "error", parsed.error),
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
        ErrorType(ErrorType::Kind::TimedeltaParsing, "error", "unable to parse string as an ISO 8601 duration"),
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


namespace {

class JsonGrammarScan {
public:
    explicit JsonGrammarScan(const std::string& text) : s_(text) {
        line_starts_.push_back(0);
        for (size_t i = 0; i < s_.size(); ++i) {
            if (s_[i] == '\n') {
                line_starts_.push_back(i + 1);
            }
        }
    }

    std::optional<std::string> run() {
        skip_ws();
        if (parse_value() != Step::Ok) {
            return msg_;
        }
        skip_ws();
        if (i_ < s_.size()) {
            return error_at("trailing characters", i_);
        }
        return std::nullopt;
    }

private:
    enum class Step { Ok, Err };

    static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    static bool is_hex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
            ++i_;
        }
    }

    size_t line_index_at(size_t idx) const {
        size_t lo = 0;
        size_t hi = line_starts_.size();
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (line_starts_[mid] <= idx) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    std::string error_at(const std::string& phrase, size_t idx) const {
        size_t line = line_index_at(idx) + 1;
        size_t col = idx - line_starts_[line_index_at(idx)] + 1;
        return phrase + " at line " + std::to_string(line) + " column " + std::to_string(col);
    }

    // End of input is reported at the position after the last character, one to
    // the left of where error_at() would place it.
    std::string error_at_eof(const char* context) {
        size_t idx = s_.size();
        size_t line = line_index_at(idx) + 1;
        size_t col = idx - line_starts_[line_index_at(idx)];
        return std::string("EOF while parsing ") + context + " at line " + std::to_string(line) +
               " column " + std::to_string(col);
    }

    Step fail(std::string message) {
        msg_ = std::move(message);
        return Step::Err;
    }

    Step parse_value() {
        skip_ws();
        if (i_ >= s_.size()) {
            return fail(error_at_eof("a value"));
        }
        char c = s_[i_];
        if (c == '{') {
            return parse_object();
        }
        if (c == '[') {
            return parse_list();
        }
        if (c == '"') {
            return parse_string();
        }
        if (c == 't') {
            return parse_ident("true");
        }
        if (c == 'f') {
            return parse_ident("false");
        }
        if (c == 'n') {
            return parse_ident("null");
        }
        if (c == 'N') {
            return parse_ident("NaN");
        }
        if (c == 'I') {
            return parse_ident("Infinity");
        }
        if (c == '-' || is_digit(c)) {
            return parse_number();
        }
        return fail(error_at("expected value", i_));
    }

    Step parse_ident(const char* word) {
        size_t word_len = std::strlen(word);
        size_t matched = 0;
        while (matched < word_len && i_ + matched < s_.size() && s_[i_ + matched] == word[matched]) {
            ++matched;
        }
        if (i_ + matched >= s_.size() && matched < word_len) {
            return fail(error_at_eof("a value"));
        }
        if (matched < word_len) {
            return fail(error_at("expected ident", i_ + matched));
        }
        i_ += word_len;
        return Step::Ok;
    }

    Step parse_number() {
        size_t start = i_;
        if (s_[i_] == '-') {
            ++i_;
            if (i_ >= s_.size()) {
                return fail(error_at_eof("a value"));
            }
            // allow_inf_nan also covers the negated constants
            if (s_[i_] == 'I') {
                return parse_ident("Infinity");
            }
            if (s_[i_] == 'N') {
                return parse_ident("NaN");
            }
            if (!is_digit(s_[i_])) {
                return fail(error_at("invalid number", i_));
            }
        }
        if (s_[i_] == '0') {
            ++i_;
            if (i_ < s_.size() && is_digit(s_[i_])) {
                return fail(error_at("invalid number", i_));
            }
        } else {
            size_t digits_begin = i_;
            while (i_ < s_.size() && is_digit(s_[i_])) {
                ++i_;
            }
            if (i_ == digits_begin) {
                return fail(error_at("expected value", start));
            }
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (i_ >= s_.size()) {
                return fail(error_at_eof("a value"));
            }
            if (!is_digit(s_[i_])) {
                return fail(error_at("invalid number", i_));
            }
            while (i_ < s_.size() && is_digit(s_[i_])) {
                ++i_;
            }
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) {
                ++i_;
            }
            if (i_ >= s_.size()) {
                return fail(error_at_eof("a value"));
            }
            if (!is_digit(s_[i_])) {
                return fail(error_at("invalid number", i_));
            }
            while (i_ < s_.size() && is_digit(s_[i_])) {
                ++i_;
            }
        }
        return Step::Ok;
    }

    Step parse_unicode_escape() {
        // s_[i_] is just past the 'u' of \uXXXX
        unsigned int first = 0;
        for (int k = 0; k < 4; ++k) {
            if (i_ >= s_.size()) {
                return fail(error_at_eof("a string"));
            }
            if (!is_hex(s_[i_])) {
                return fail(error_at("invalid escape", i_));
            }
            char c = s_[i_];
            unsigned int digit = is_digit(c) ? static_cast<unsigned int>(c - '0')
                                            : static_cast<unsigned int>((c | 0x20) - 'a' + 10);
            first = (first << 4) | digit;
            ++i_;
        }
        if (first < 0xD800 || first > 0xDBFF) {
            return Step::Ok;
        }
        // A high surrogate must be followed by its low half
        if (i_ + 5 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
            unsigned int low = 0;
            size_t cur = i_ + 2;
            bool ok = true;
            for (int k = 0; k < 4 && ok; ++k) {
                if (!is_hex(s_[cur])) {
                    ok = false;
                } else {
                    char c = s_[cur];
                    unsigned int digit = is_digit(c) ? static_cast<unsigned int>(c - '0')
                                                    : static_cast<unsigned int>((c | 0x20) - 'a' + 10);
                    low = (low << 4) | digit;
                    ++cur;
                }
            }
            if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                i_ = cur;
                return Step::Ok;
            }
        }
        return fail(error_at("unexpected end of hex escape", i_));
    }

    Step parse_string() {
        ++i_;  // opening quote
        for (;;) {
            if (i_ >= s_.size()) {
                return fail(error_at_eof("a string"));
            }
            char c = s_[i_];
            if (c == '"') {
                ++i_;
                return Step::Ok;
            }
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size()) {
                    return fail(error_at_eof("a string"));
                }
                char escape = s_[i_];
                if (escape == 'u') {
                    ++i_;
                    Step step = parse_unicode_escape();
                    if (step != Step::Ok) {
                        return step;
                    }
                    continue;
                }
                if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' ||
                    escape == 'n' || escape == 'r' || escape == 't') {
                    ++i_;
                    continue;
                }
                return fail(error_at("invalid escape", i_));
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail(error_at("control character (\\u0000-\\u001F) found while parsing a string", i_));
            }
            ++i_;
        }
    }

    Step parse_list() {
        ++i_;  // '['
        skip_ws();
        if (i_ >= s_.size()) {
            return fail(error_at_eof("a list"));
        }
        if (s_[i_] == ']') {
            ++i_;
            return Step::Ok;
        }
        for (;;) {
            Step step = parse_value();
            if (step != Step::Ok) {
                return step;
            }
            skip_ws();
            if (i_ >= s_.size()) {
                return fail(error_at_eof("a list"));
            }
            if (s_[i_] == ']') {
                ++i_;
                return Step::Ok;
            }
            if (s_[i_] != ',') {
                return fail(error_at("expected `,` or `]`", i_));
            }
            ++i_;
            skip_ws();
            if (i_ >= s_.size()) {
                return fail(error_at_eof("a value"));
            }
            if (s_[i_] == ']') {
                return fail(error_at("trailing comma", i_));
            }
        }
    }

    Step parse_object() {
        ++i_;  // '{'
        skip_ws();
        if (i_ >= s_.size()) {
            return fail(error_at_eof("an object"));
        }
        if (s_[i_] == '}') {
            ++i_;
            return Step::Ok;
        }
        for (;;) {
            skip_ws();
            if (i_ >= s_.size()) {
                return fail(error_at_eof("an object"));
            }
            if (s_[i_] != '"') {
                return fail(error_at("key must be a string", i_));
            }
            Step step = parse_string();
            if (step != Step::Ok) {
                return step;
            }
            skip_ws();
            if (i_ >= s_.size()) {
                return fail(error_at_eof("an object"));
            }
            if (s_[i_] != ':') {
                return fail(error_at("expected `:`", i_));
            }
            ++i_;
            step = parse_value();
            if (step != Step::Ok) {
                return step;
            }
            skip_ws();
            if (i_ >= s_.size()) {
                return fail(error_at_eof("an object"));
            }
            if (s_[i_] == '}') {
                ++i_;
                return Step::Ok;
            }
            if (s_[i_] != ',') {
                return fail(error_at("expected `,` or `}`", i_));
            }
            ++i_;
            skip_ws();
            if (i_ >= s_.size()) {
                return fail(error_at_eof("a value"));
            }
            if (s_[i_] == '}') {
                return fail(error_at("trailing comma", i_));
            }
        }
    }

    const std::string& s_;
    size_t i_ = 0;
    std::vector<size_t> line_starts_;
    std::string msg_;
};

}  // namespace

std::optional<std::string> json_diagnose_parse_error(const std::string& json_text) {
    JsonGrammarScan scan(json_text);
    return scan.run();
}

JsonParseOutcome json_parse_python(const std::string& json_text) {
    JsonParseOutcome outcome;
    try {
        py::object json_mod = py::module_::import("json");
        outcome.value = json_mod.attr("loads")(json_text);
        outcome.ok = true;
    } catch (const py::error_already_set& e) {
        auto diagnosis = json_diagnose_parse_error(json_text);
        outcome.error_description = diagnosis.has_value() ? *diagnosis : py::str(e.value()).cast<std::string>();
    }
    return outcome;
}

} // namespace pydantic_core