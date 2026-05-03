#include "pydantic_core/json_input.hpp"
#include "pydantic_core/result.hpp"
#include <simdjson.h>
#include "pydantic_core/error_types.hpp"
#include <sstream>

namespace pydantic_core {

JsonInput::JsonInput(simdjson::simdjson_result<simdjson::dom::element> element) {
    auto err = element.get(element_);
    if (err) {
        // Handle error - element will be default initialized
        element_ = simdjson::dom::element();
    }
}

JsonInput::JsonInput(const simdjson::dom::element& element) : element_(element) {}

InputValue JsonInput::as_error_value() const {
    std::ostringstream oss;
    
    auto type = element_.type();
    switch (type.value_unsafe()) {
        case simdjson::dom::element_type::STRING:
            return InputValue("'" + std::string(element_.get_string().value_unsafe()) + "'");
        case simdjson::dom::element_type::INT64:
            return InputValue(std::to_string(element_.get_int64().value_unsafe()));
        case simdjson::dom::element_type::UINT64:
            return InputValue(std::to_string(element_.get_uint64().value_unsafe()));
        case simdjson::dom::element_type::DOUBLE:
            return InputValue(std::to_string(element_.get_double().value_unsafe()));
        case simdjson::dom::element_type::BOOL:
            return InputValue(element_.get_bool().value_unsafe() ? "True" : "False");
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

ValResultMatch<EitherString> JsonInput::validate_str(bool strict, bool coerce_numbers) {
    if (element_.type() == simdjson::dom::element_type::STRING) {
        auto str_result = element_.get_string();
        if (!str_result.error()) {
            return ValMatch::exact(EitherString(std::string(str_result.value_unsafe())));
        }
    }
    
    // Lax mode - coerce from numbers
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::INT64) {
            return ValMatch::lax(EitherString(std::to_string(element_.get_int64().value_unsafe())));
        }
        if (element_.type() == simdjson::dom::element_type::UINT64) {
            return ValMatch::lax(EitherString(std::to_string(element_.get_uint64().value_unsafe())));
        }
        if (element_.type() == simdjson::dom::element_type::DOUBLE) {
            return ValMatch::lax(EitherString(std::to_string(element_.get_double().value_unsafe())));
        }
        if (element_.type() == simdjson::dom::element_type::BOOL) {
            return ValMatch::lax(EitherString(element_.get_bool().value_unsafe() ? "true" : "false"));
        }
    }
    
    return ValError::line_error(PydanticKnownError::string_type(), 
                               Location(), as_error_value().repr);
}

ValResultMatch<EitherBytes> JsonInput::validate_bytes(bool strict) {
    // JSON doesn't have native bytes type - treat string as bytes in lax mode
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = element_.get_string().value_unsafe();
            return ValMatch::lax(EitherBytes(std::string_view(str)));
        }
    }
    
    return ValError::line_error(PydanticKnownError::bytes_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<bool> JsonInput::validate_bool(bool strict) {
    if (element_.type() == simdjson::dom::element_type::BOOL) {
        return ValMatch::exact(element_.get_bool().value_unsafe());
    }
    
    // Lax mode - coerce from strings/ints
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::INT64) {
            int64_t i = element_.get_int64().value_unsafe();
            if (i == 0 || i == 1) {
                return ValMatch::lax(static_cast<bool>(i));
            }
        }
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = std::string(element_.get_string().value_unsafe());
            std::transform(str.begin(), str.end(), str.begin(), ::tolower);
            if (str == "true" || str == "1") {
                return ValMatch::lax(true);
            }
            if (str == "false" || str == "0") {
                return ValMatch::lax(false);
            }
        }
    }
    
    return ValError::line_error(PydanticKnownError::bool_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<EitherInt> JsonInput::validate_int(bool strict) {
    if (element_.type() == simdjson::dom::element_type::INT64) {
        return ValMatch::exact(EitherInt(element_.get_int64().value_unsafe()));
    }
    if (element_.type() == simdjson::dom::element_type::UINT64) {
        return ValMatch::exact(EitherInt(element_.get_uint64().value_unsafe()));
    }
    
    // Lax mode - coerce from float/string
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::DOUBLE) {
            double d = element_.get_double().value_unsafe();
            if (std::floor(d) == d) {
                return ValMatch::lax(EitherInt(static_cast<int64_t>(d)));
            }
        }
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = std::string(element_.get_string().value_unsafe());
            try {
                if (str.find('.') == std::string::npos) {
                    int64_t i = std::stoll(str);
                    return ValMatch::lax(EitherInt(i));
                }
            } catch (...) {}
        }
        if (element_.type() == simdjson::dom::element_type::BOOL) {
            return ValMatch::lax(EitherInt(static_cast<int64_t>(element_.get_bool().value_unsafe())));
        }
    }
    
    return ValError::line_error(PydanticKnownError::int_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<EitherFloat> JsonInput::validate_float(bool strict) {
    if (element_.type() == simdjson::dom::element_type::DOUBLE) {
        return ValMatch::exact(EitherFloat(element_.get_double().value_unsafe()));
    }
    
    // Ints are valid floats
    if (element_.type() == simdjson::dom::element_type::INT64) {
        double d = static_cast<double>(element_.get_int64().value_unsafe());
        if (strict) {
            return ValMatch::exact(EitherFloat(d));
        }
        return ValMatch::lax(EitherFloat(d));
    }
    if (element_.type() == simdjson::dom::element_type::UINT64) {
        double d = static_cast<double>(element_.get_uint64().value_unsafe());
        if (strict) {
            return ValMatch::exact(EitherFloat(d));
        }
        return ValMatch::lax(EitherFloat(d));
    }
    
    // Lax mode - string coercion
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = std::string(element_.get_string().value_unsafe());
            try {
                double d = std::stod(str);
                return ValMatch::lax(EitherFloat(d));
            } catch (...) {}
        }
    }
    
    return ValError::line_error(PydanticKnownError::float_type(),
                               Location(), as_error_value().repr);
}

ValResult<std::unique_ptr<ValidatedDict>> JsonInput::validate_dict(bool strict) {
    if (element_.type() == simdjson::dom::element_type::OBJECT) {
        auto obj = element_.get_object();
        if (!obj.error()) {
            return std::make_unique<JsonValidatedDict>(obj.value_unsafe());
        }
    }
    
    return ValError::line_error(PydanticKnownError::dict_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<std::unique_ptr<ValidatedList>> JsonInput::validate_list(bool strict) {
    if (element_.type() == simdjson::dom::element_type::ARRAY) {
        auto arr = element_.get_array();
        if (!arr.error()) {
            return ValMatch::exact(std::make_unique<JsonValidatedList>(arr.value_unsafe()));
        }
    }
    
    return ValError::line_error(PydanticKnownError::list_type(),
                               Location(), as_error_value().repr);
}

ValResultMatch<std::unique_ptr<ValidatedTuple>> JsonInput::validate_tuple(bool strict) {
    // JSON doesn't distinguish tuples - treat arrays as tuples in lax mode
    if (element_.type() == simdjson::dom::element_type::ARRAY) {
        auto arr = element_.get_array();
        if (!arr.error()) {
            if (strict) {
                return ValMatch::exact(std::make_unique<JsonValidatedList>(arr.value_unsafe()));
            }
            return ValMatch::lax(std::make_unique<JsonValidatedList>(arr.value_unsafe()));
        }
    }
    
    return ValError::line_error(PydanticKnownError::tuple_type(),
                               Location(), as_error_value().repr);
}

// JsonValidatedDict implementation
size_t JsonValidatedDict::size() const {
    return obj_.size();
}

bool JsonValidatedDict::empty() const {
    return obj_.size() == 0;
}

std::vector<ValidatedDict::Entry> JsonValidatedDict::entries() const {
    std::vector<Entry> result;
    for (auto& [key, value] : obj_) {
        Entry e;
        e.key = std::string(key);
        // Create input representation for value
        std::ostringstream oss;
        auto type = value.type();
        switch (type.value_unsafe()) {
            case simdjson::dom::element_type::STRING:
                e.value_repr = InputValue("'" + std::string(value.get_string().value_unsafe()) + "'");
                break;
            case simdjson::dom::element_type::INT64:
                e.value_repr = InputValue(std::to_string(value.get_int64().value_unsafe()));
                break;
            default:
                e.value_repr = InputValue("...");
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
                e.value_repr = InputValue("'" + std::string(value.get_string().value_unsafe()) + "'");
            } else {
                e.value_repr = InputValue("...");
            }
            return e;
        }
    }
    return std::nullopt;
}

// JsonValidatedList implementation
size_t JsonValidatedList::size() const {
    return arr_.size();
}

bool JsonValidatedList::empty() const {
    return arr_.size() == 0;
}

std::vector<ValidatedList::Entry> JsonValidatedList::entries() const {
    std::vector<Entry> result;
    size_t idx = 0;
    for (auto& item : arr_) {
        Entry e;
        e.index = idx++;
        std::ostringstream oss;
        auto type = item.type();
        if (type == simdjson::dom::element_type::STRING) {
            e.value_repr = InputValue("'" + std::string(item.get_string().value_unsafe()) + "'");
        } else {
            e.value_repr = InputValue("...");
        }
        result.push_back(e);
    }
    return result;
}

// JSON parsing functions
ValResult<std::unique_ptr<JsonInput>> parse_json(const std::string_view& json_str) {
    simdjson::dom::parser parser;
    auto result = parser.parse(json_str);
    if (result.error()) {
        return ValError::line_error(
            ErrorType::custom("Invalid JSON", "json_invalid"),
            Location(),
            std::string(json_str.substr(0, 50))
        );
    }
    auto input = std::make_unique<JsonInput>(result.value());
    input->owned_ = true;
    return input;
}

ValResult<std::unique_ptr<JsonInput>> parse_json(const std::vector<uint8_t>& json_bytes) {
    simdjson::dom::parser parser;
    auto result = parser.parse(json_bytes);
    if (result.error()) {
        return ValError::line_error(
            ErrorType::custom("Invalid JSON", "json_invalid"),
            Location(),
            std::string(reinterpret_cast<const char*>(json_bytes.data()), 
                       std::min(json_bytes.size(), size_t(50)))
        );
    }
    auto input = std::make_unique<JsonInput>(result.value());
    input->owned_ = true;
    return input;
}

} // namespace pydantic_core