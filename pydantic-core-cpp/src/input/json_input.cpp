#include "pydantic_core/json_input.hpp"
#include "pydantic_core/result.hpp"
#include "pydantic_core/error_types.hpp"
#include <simdjson.h>
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
    
    // Lax mode - coerce from strings/ints
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::INT64) {
            int64_t i = element_.get_int64().value_unsafe();
            if (i == 0 || i == 1) {
                return ValMatch<bool>::lax(static_cast<bool>(i));
            }
        }
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = std::string(element_.get_string().value_unsafe());
            std::transform(str.begin(), str.end(), str.begin(), ::tolower);
            if (str == "true" || str == "1") {
                return ValMatch<bool>::lax(true);
            }
            if (str == "false" || str == "0") {
                return ValMatch<bool>::lax(false);
            }
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