#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/string_input.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace pydantic_core {

// Forward declaration — defined in schema_validator.cpp
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name);

// ListValidator - validates list/array values
class ListValidator : public Validator {
public:
    bool strict = false;
    bool fail_fast = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<Validator> items_schema;  // Inner validator for list items

    ListValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_list(strict);
        if (result.is_err()) {
            return result.error();
        }
        auto& list_match = result.value();
        auto& list = list_match.value();
        size_t list_size = list->size();

        // Length checks
        if (min_length.has_value() && list_size < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::ListTooShort, static_cast<int64_t>(min_length.value())),
                state.location(),
                "list(len=" + std::to_string(list_size) + ")"
            );
        }
        if (max_length.has_value() && list_size > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::ListTooLong, static_cast<int64_t>(max_length.value())),
                state.location(),
                "list(len=" + std::to_string(list_size) + ")"
            );
        }

        // Validate each item against items_schema
        if (items_schema) {
            auto entries = list->entries();
            py::list result_list;
            std::vector<std::shared_ptr<ValLineError>> errors;
            // In strings mode (validate_strings), items always coerce regardless of strict
            for (const auto& entry : entries) {
                state.location().push(entry.index);
                ValidationState item_state = state.sub_copy(state.coerce_strings());
                // Get the element and create an input for sub-validation
                py::object element = list->get_item(entry.index);
                std::unique_ptr<Input> element_input;
                if (state.coerce_strings() && py::isinstance<py::str>(element)) {
                    element_input = std::make_unique<StringInput>(py::str(element).cast<std::string>());
                } else {
                    element_input = std::make_unique<PythonInput>(element);
                }
                element_input->set_current_location(state.location());
                auto item_result = items_schema->validate(*element_input, item_state);
                if (item_result.is_err()) {
                    if (fail_fast) {
                        state.location().pop();
                        return item_result.error();
                    }
                    // Collect errors in non-fail-fast mode (Rust aggregates them)
                    if (item_result.error().has_line_errors()) {
                        for (auto& le : item_result.error().line_errors()) {
                            errors.push_back(le);
                        }
                    } else {
                        errors.push_back(std::make_shared<ValLineError>(ValLineError{
                            ErrorType(ErrorType::Kind::CustomError), state.location(), "Item validation failed"}));
                    }
                } else {
                    // Validation succeeded — use the original input element as the result
                    // (validated result type differs per validator, but input is always py::object)
                    result_list.append(element);
                }
                state.location().pop();
            }
            if (!errors.empty()) {
                return ValError::line_errors(std::move(errors));
            }
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::list>(std::move(result_list)));
        }

        // No items_schema: return the original items as a Python list
        {
            py::list result_list;
            auto entries = list->entries();
            for (const auto& entry : entries) {
                py::object element = list->get_item(entry.index);
                result_list.append(element);
            }
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::list>(std::move(result_list)));
        }
    }

    std::string name() const override { return "list"; }
};

// DictValidator - validates dict/object values
class DictValidator : public Validator {
public:
    bool strict = false;
    bool fail_fast = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<Validator> keys_schema;   // Inner validator for dict keys
    std::shared_ptr<Validator> values_schema; // Inner validator for dict values

    DictValidator() = default;

    // Convert a validated sub-result to a Python object based on the validator
    // name.  Returns nullopt when the stored type can't be determined safely
    // (caller falls back to the original input object).
    static std::optional<py::object> validated_to_py(
        const std::shared_ptr<void>& value, const std::string& name) {
        if (!value) return std::nullopt;
        if (name == "int" || name == "int-constrained" || name == "constrained-int" || name == "int64") {
            if (auto* i = static_cast<int64_t*>(value.get())) return py::int_(*i);
        }
        if (name == "float" || name == "float-constrained" || name == "constrained-float") {
            if (auto* d = static_cast<double*>(value.get())) return py::float_(*d);
        }
        if (name == "bool") {
            if (auto* b = static_cast<bool*>(value.get())) return py::bool_(*b);
        }
        if (name == "str" || name == "string" || name == "constr-str" || name == "str-constrained") {
            if (auto* s = static_cast<std::string*>(value.get())) return py::str(*s);
        }
        if (name == "list" || name == "list-constrained" || name == "constr-list") {
            if (auto* l = static_cast<py::list*>(value.get())) return *l;
        }
        if (name == "dict" || name == "dict-constrained") {
            if (auto* d = static_cast<py::dict*>(value.get())) return *d;
        }
        if (name == "date") {
            if (auto* ed = static_cast<EitherDate*>(value.get())) {
                auto& d = ed->value;
                return py::module_::import("datetime").attr("date")(d.year, d.month, d.day);
            }
        }
        if (name == "time") {
            if (auto* et = static_cast<EitherTime*>(value.get())) {
                auto& t = et->value;
                py::object datetime_mod = py::module_::import("datetime");
                if (t.tz_offset.has_value()) {
                    py::object tz_delta = datetime_mod.attr("timedelta")(py::arg("minutes") = *t.tz_offset);
                    py::object tz = datetime_mod.attr("timezone")(tz_delta);
                    return datetime_mod.attr("time")(t.hour, t.minute, t.second, t.microsecond, tz);
                }
                return datetime_mod.attr("time")(t.hour, t.minute, t.second, t.microsecond);
            }
        }
        if (name == "datetime") {
            if (auto* edt = static_cast<EitherDateTime*>(value.get())) {
                auto& dt = edt->value;
                py::object datetime_mod = py::module_::import("datetime");
                if (dt.time.tz_offset.has_value()) {
                    py::object tz_delta = datetime_mod.attr("timedelta")(py::arg("minutes") = *dt.time.tz_offset);
                    py::object tz = datetime_mod.attr("timezone")(tz_delta);
                    return datetime_mod.attr("datetime")(
                        dt.date.year, dt.date.month, dt.date.day,
                        dt.time.hour, dt.time.minute, dt.time.second, dt.time.microsecond, tz);
                }
                return datetime_mod.attr("datetime")(
                    dt.date.year, dt.date.month, dt.date.day,
                    dt.time.hour, dt.time.minute, dt.time.second, dt.time.microsecond);
            }
        }
        if (name == "timedelta") {
            if (auto* etd = static_cast<EitherTimedelta*>(value.get())) {
                auto& td = etd->value;
                return py::module_::import("datetime").attr("timedelta")(
                    py::arg("days") = td.days,
                    py::arg("seconds") = td.seconds,
                    py::arg("microseconds") = td.microseconds);
            }
        }
        // Validators that store py::object results.  Note: "any" results are
        // NOT py::object — the C++ AnyValidator stores a string repr — so "any"
        // falls through and callers keep the original input object.
        if (name == "function-after" || name == "function-before" ||
            name == "function-wrap" || name == "function-plain" || name == "json-or-python" ||
            name == "py_object" || name == "is-instance" || name == "is-subclass") {
            if (auto* o = static_cast<py::object*>(value.get())) return *o;
        }
        return std::nullopt;
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_dict(strict);
        if (result.is_err()) {
            return result.error();
        }
        auto& dict = result.value();
        size_t dict_size = dict->size();

        // Length checks
        if (min_length.has_value() && dict_size < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DictTooShort, static_cast<int64_t>(min_length.value())),
                state.location(),
                "dict(len=" + std::to_string(dict_size) + ")"
            );
        }
        if (max_length.has_value() && dict_size > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::DictTooLong, static_cast<int64_t>(max_length.value())),
                state.location(),
                "dict(len=" + std::to_string(dict_size) + ")"
            );
        }

        auto entries = dict->entries();
        InputType src_type = input.input_type();

        // Build a sub-input for a key/value from the source dict.
        // For StringInput (validate_strings), keys/values are raw strings that
        // always coerce regardless of strict (matching Rust's StringInput).
        auto make_sub_input = [&](const py::object& obj) -> std::unique_ptr<Input> {
            if (src_type == InputType::String) {
                std::string s;
                try { s = py::str(obj).cast<std::string>(); } catch (...) {}
                return std::make_unique<StringInput>(s);
            }
            return std::make_unique<PythonInput>(obj);
        };

        // In strings mode (validate_strings), keys/values always coerce
        // regardless of strict — match Rust's StringInput semantics.
        ValidationState sub_state = state.sub_copy(state.coerce_strings());

        // Validate keys and values if schemas provided
        if (keys_schema || values_schema) {
            py::dict result_dict;
            std::vector<std::shared_ptr<ValLineError>> errors;
            for (const auto& entry : entries) {
                state.location().push(entry.key);
                py::object key_obj = dict->get_key(entry.key).value_or(py::str(entry.key));
                py::object val_obj = dict->get_value(entry.key).value_or(py::none());

                // Validate key against keys_schema
                if (keys_schema) {
                    auto key_input = make_sub_input(key_obj);
                    key_input->set_current_location(state.location());
                    auto key_result = keys_schema->validate(*key_input, sub_state);
                    if (key_result.is_err()) {
                        if (fail_fast) {
                            state.location().pop();
                            return key_result.error();
                        }
                        if (key_result.error().has_line_errors()) {
                            for (auto& le : key_result.error().line_errors()) {
                                errors.push_back(le);
                            }
                        }
                    } else {
                        auto converted = validated_to_py(key_result.value(), keys_schema->effective_result_name());
                        if (converted) key_obj = *converted;
                    }
                }
                // Validate value against values_schema
                if (values_schema) {
                    auto val_input = make_sub_input(val_obj);
                    val_input->set_current_location(state.location());
                    auto val_result = values_schema->validate(*val_input, sub_state);
                    if (val_result.is_err()) {
                        if (fail_fast) {
                            state.location().pop();
                            return val_result.error();
                        }
                        if (val_result.error().has_line_errors()) {
                            for (auto& le : val_result.error().line_errors()) {
                                errors.push_back(le);
                            }
                        }
                    } else {
                        auto converted = validated_to_py(val_result.value(), values_schema->effective_result_name());
                        if (converted) val_obj = *converted;
                    }
                }
                result_dict[key_obj] = val_obj;
                state.location().pop();
            }
            if (!errors.empty()) {
                return ValError::line_errors(std::move(errors));
            }
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::dict>(std::move(result_dict)));
        }

        // No key/value schemas: build a dict from the original entries
        {
            py::dict result_dict;
            for (const auto& entry : entries) {
                py::object key_obj = dict->get_key(entry.key).value_or(py::str(entry.key));
                py::object val_obj = dict->get_value(entry.key).value_or(py::none());
                result_dict[key_obj] = val_obj;
            }
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::dict>(std::move(result_dict)));
        }
    }

    std::string name() const override { return "dict"; }
};

// SetValidator - validates set values
class SetValidator : public Validator {
public:
    std::shared_ptr<Validator> items_schema;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_list(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        auto& list_match = result.value();
        auto& list = list_match.value();
        auto entries = list->entries();
        size_t list_size = entries.size();

        // Length checks
        if (min_length.has_value() && list_size < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::SetTooShort, static_cast<int64_t>(min_length.value())),
                state.location(),
                "set(len=" + std::to_string(list_size) + ")"
            );
        }
        if (max_length.has_value() && list_size > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::SetTooLong, static_cast<int64_t>(max_length.value())),
                state.location(),
                "set(len=" + std::to_string(list_size) + ")"
            );
        }

        py::set result_set;
        for (const auto& entry : entries) {
            py::object element = list->get_item(entry.index);
            if (items_schema) {
                PythonInput elem_input(element);
                auto item_result = items_schema->validate(elem_input, state);
                if (item_result.is_ok()) {
                    py::object py_val = value_to_python_with_type(item_result.value(), items_schema->effective_result_name());
                    result_set.add(py_val);
                } else {
                    return item_result.error();
                }
            } else {
                result_set.add(element);
            }
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(result_set)));
    }

    std::string name() const override { return "set"; }
};

// FrozenSetValidator - validates frozenset values
class FrozenSetValidator : public Validator {
public:
    std::shared_ptr<Validator> items_schema;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_list(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        auto& list_match = result.value();
        auto& list = list_match.value();
        auto entries = list->entries();
        size_t list_size = entries.size();

        // Length checks
        if (min_length.has_value() && list_size < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::SetTooShort, static_cast<int64_t>(min_length.value())),
                state.location(),
                "frozenset(len=" + std::to_string(list_size) + ")"
            );
        }
        if (max_length.has_value() && list_size > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::SetTooLong, static_cast<int64_t>(max_length.value())),
                state.location(),
                "frozenset(len=" + std::to_string(list_size) + ")"
            );
        }

        py::set result_set;
        for (const auto& entry : entries) {
            py::object element = list->get_item(entry.index);
            if (items_schema) {
                PythonInput elem_input(element);
                auto item_result = items_schema->validate(elem_input, state);
                if (item_result.is_ok()) {
                    py::object py_val = value_to_python_with_type(item_result.value(), items_schema->effective_result_name());
                    result_set.add(py_val);
                } else {
                    return item_result.error();
                }
            } else {
                result_set.add(element);
            }
        }
        py::frozenset fs = py::frozenset(result_set);
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(fs)));
    }

    std::string name() const override { return "frozenset"; }
};

// TupleValidator - validates tuple values with positional items
class TupleValidator : public Validator {
public:
    bool strict = false;
    bool variadic = false;  // If true, last item_schema is repeated for remaining items
    std::vector<std::shared_ptr<Validator>> items; // Positional item validators

    TupleValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_tuple(strict);
        if (result.is_err()) {
            return result.error();
        }
        auto& tuple_match = result.value();
        auto& tuple = tuple_match.value();
        size_t tuple_size = tuple->size();

        // Build result tuple, validating items if we have item schemas
        py::tuple result_tuple(tuple_size);
        auto entries = tuple->entries();
        for (size_t i = 0; i < entries.size(); i++) {
            py::object element = tuple->get_item(i);
            if (!items.empty()) {
                size_t schema_idx = (variadic && i >= items.size()) ? items.size() - 1 : i;
                if (schema_idx < items.size() && items[schema_idx]) {
                    state.location().push(i);
                    PythonInput elem_input(element);
                    auto item_result = items[schema_idx]->validate(elem_input, state);
                    state.location().pop();
                    if (item_result.is_ok()) {
                        element = value_to_python_with_type(item_result.value(), items[schema_idx]->effective_result_name());
                    } else {
                        return item_result.error();
                    }
                }
            }
            result_tuple[i] = element;
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(result_tuple)));
    }

    std::string name() const override { return "tuple"; }
};

} // namespace pydantic_core