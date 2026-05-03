#include "pydantic_core/json_input.hpp"
#include "pydantic_core/result.hpp"
#include <simdjson.h>
#include "pydantic_core/error_types.hpp"
#include <sstream>

namespace pydantic_core {

JsonInput::JsonInput(simdjson::simdjson_result<simdjson::dom::element> element) {
    auto err = element.get(element_);
    if (err) {
        element_ = simdjson::dom::element();
    }
}

JsonInput::JsonInput(const simdjson::dom::element& element) : element_(element) {}

InputValue JsonInput::as_error_value() const {
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
            return ValMatch<EitherString>::exact(EitherString(std::string(str_result.value_unsafe())));
        }
    }
    
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
            return ValMatch<EitherString>::lax(EitherString(element_.get_bool().value_unsafe() ? "true" : "false"));
        }
    }
    
    return ValError::line_error(PydanticKnownError::string_type(), Location(), as_error_value().repr);
}

ValResultMatch<EitherBytes> JsonInput::validate_bytes(bool strict) {
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = element_.get_string().value_unsafe();
            return ValMatch<EitherBytes>::lax(EitherBytes(std::string_view(str)));
        }
    }
    
    return ValError::line_error(PydanticKnownError::bytes_type(), Location(), as_error_value().repr);
}

ValResultMatch<bool> JsonInput::validate_bool(bool strict) {
    if (element_.type() == simdjson::dom::element_type::BOOL) {
        return ValMatch<bool>::exact(element_.get_bool().value_unsafe());
    }
    
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
    
    return ValError::line_error(PydanticKnownError::bool_type(), Location(), as_error_value().repr);
}

ValResultMatch<EitherInt> JsonInput::validate_int(bool strict) {
    if (element_.type() == simdjson::dom::element_type::INT64) {
        return ValMatch<EitherInt>::exact(EitherInt(element_.get_int64().value_unsafe()));
    }
    if (element_.type() == simdjson::dom::element_type::UINT64) {
        return ValMatch<EitherInt>::exact(EitherInt(element_.get_uint64().value_unsafe()));
    }
    
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
    
    return ValError::line_error(PydanticKnownError::int_type(), Location(), as_error_value().repr);
}

ValResultMatch<EitherFloat> JsonInput::validate_float(bool strict) {
    if (element_.type() == simdjson::dom::element_type::DOUBLE) {
        return ValMatch<EitherFloat>::exact(EitherFloat(element_.get_double().value_unsafe()));
    }
    
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
    
    if (!strict) {
        if (element_.type() == simdjson::dom::element_type::STRING) {
            auto str = std::string(element_.get_string().value_unsafe());
            try {
                double d = std::stod(str);
                return ValMatch<EitherFloat>::lax(EitherFloat(d));
            } catch (...) {}
        }
    }
    
    return ValError::line_error(PydanticKnownError::float_type(), Location(), as_error_value().repr);
}

ValResult<std::unique_ptr<ValidatedDict>> JsonInput::validate_dict(bool strict) {
    if (element_.type() == simdjson::dom::element_type::OBJECT) {
        auto obj = element_.get_object();
        if (!obj.error()) {
            return std::make_unique<JsonValidatedDict>(obj.value_unsafe());
        }
    }
    
    return ValError::line_error(PydanticKnownError::dict_type(), Location(), as_error_value().repr);
}

ValResultMatch<std::unique_ptr<ValidatedList>> JsonInput::validate_list(bool strict) {
    if (element_.type() == simdjson::dom::element_type::ARRAY) {
        auto arr = element_.get_array();
        if (!arr.error()) {
            return ValMatch<std::unique_ptr<ValidatedList>>::exact(std::make_unique<JsonValidatedList>(arr.value_unsafe()));
        }
    }
    
    return ValError::line_error(PydanticKnownError::list_type(), Location(), as_error_value().repr);
}

ValResultMatch<std::unique_ptr<ValidatedTuple>> JsonInput::validate_tuple(bool strict) {
    if (element_.type() == simdjson::dom::element_type::ARRAY) {
        auto arr = element_.get_array();
        if (!arr.error()) {
            if (strict) {
                return ValMatch<std::unique_ptr<ValidatedTuple>>::exact(std::make_unique<JsonValidatedTuple>(arr.value_unsafe()));
            }
            return ValMatch<std::unique_ptr<ValidatedTuple>>::lax(std::make_unique<JsonValidatedTuple>(arr.value_unsafe()));
        }
    }
    
    return ValError::line_error(PydanticKnownError::tuple_type(), Location(), as_error_value().repr);
}

// JsonValidatedDict implementation - entries() only (size/empty are inline in header)
std::vector<ValidatedDict::Entry> JsonValidatedDict::entries() const {
    std::vector<Entry> result;
    for (auto [key, value] : obj_) {
        Entry e;
        e.key = std::string(key);
        auto type = value.type();
        if (type == simdjson::dom::element_type::STRING) {
            e.value_repr = "'" + std::string(value.get_string().value_unsafe()) + "'";
        } else if (type == simdjson::dom::element_type::INT64) {
            e.value_repr = std::to_string(value.get_int64().value_unsafe());
        } else {
            e.value_repr = "...";
        }
        result.push_back(e);
    }
    return result;
}

std::vector<std::string> JsonValidatedDict::keys() const {
    std::vector<std::string> result;
    for (auto& [k, _] : obj_) {
        result.push_back(std::string(k));
    }
    return result;
}

bool JsonValidatedDict::has_key(const std::string& key) const {
    for (auto& [k, _] : obj_) {
        if (std::string(k) == key) return true;
    }
    return false;
}

std::optional<ValidatedDict::Entry> JsonValidatedDict::get(const std::string& key) const {
    for (auto& [k, value] : obj_) {
        if (std::string(k) == key) {
            Entry e;
            e.key = key;
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

// JsonValidatedList implementation - entries() only (size/empty are inline in header)
std::vector<ValidatedList::Entry> JsonValidatedList::entries() const {
    std::vector<Entry> result;
    size_t idx = 0;
    for (auto item : arr_) {
        Entry e;
        e.index = idx++;
        auto type = item.type();
        if (type == simdjson::dom::element_type::STRING) {
            e.value_repr = "'" + std::string(item.get_string().value_unsafe()) + "'";
        } else if (type == simdjson::dom::element_type::INT64) {
            e.value_repr = std::to_string(item.get_int64().value_unsafe());
        } else {
            e.value_repr = "...";
        }
        result.push_back(e);
    }
    return result;
}

// JsonValidatedTuple implementation
std::vector<ValidatedList::Entry> JsonValidatedTuple::entries() const {
    std::vector<Entry> result;
    size_t idx = 0;
    for (auto item : arr_) {
        Entry e;
        e.index = idx++;
        auto type = item.type();
        if (type == simdjson::dom::element_type::STRING) {
            e.value_repr = "'" + std::string(item.get_string().value_unsafe()) + "'";
        } else if (type == simdjson::dom::element_type::INT64) {
            e.value_repr = std::to_string(item.get_int64().value_unsafe());
        } else {
            e.value_repr = "...";
        }
        result.push_back(e);
    }
    return result;
}

} // namespace pydantic_core