#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/py_compat.hpp"
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/string_input.hpp"
#include "pydantic_core/py_time.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pydantic_core {

// Forward declaration — defined in schema_validator.cpp
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name);

// ListValidator - validates list/array values
class ListValidator : public Validator {
public:
    std::optional<bool> strict;
    bool fail_fast = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<Validator> items_schema;  // Inner validator for list items
    mutable std::optional<std::string> display_name_cache_;

    ListValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto result = input.validate_list(state.strict_or_declared(strict));
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        auto& list_match = result.value();
        auto& list = list_match.value();
        size_t list_size = list->size();

        // Length checks
        if (min_length.has_value() && list_size < min_length.value()) {
            ErrorType err(ErrorType::Kind::ListTooShort);
            err.context()["field_type"] = "List";
            err.context()["min_length"] = std::to_string(min_length.value());
            err.context()["actual_length"] = std::to_string(list_size);
            return ValError::line_error(
                std::move(err),
                state.location(),
                input.as_error_value().repr
            );
        }
        if (max_length.has_value() && list_size > max_length.value()) {
            ErrorType err(ErrorType::Kind::ListTooLong);
            err.context()["field_type"] = "List";
            err.context()["max_length"] = std::to_string(max_length.value());
            err.context()["actual_length"] = std::to_string(list_size);
            return ValError::line_error(
                std::move(err),
                state.location(),
                input.as_error_value().repr
            );
        }

        // Validate each item against items_schema
        if (items_schema) {
            auto entries = list->entries();
            py::list result_list;
            std::vector<std::shared_ptr<ValLineError>> errors;
            size_t last_index = entries.empty() ? 0 : entries.back().index;
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
                    // Partial mode: drop line errors for the last item (Rust:
                    // enumerate_last_partial — the last element is omitted from
                    // the output when its validation fails).
                    bool is_last_partial = state.is_partial() && (entry.index == last_index);
                    if (fail_fast && !is_last_partial) {
                        state.location().pop();
                        return item_result.error();
                    }
                    // Collect errors in non-fail-fast mode (Rust aggregates them)
                    if (item_result.error().has_line_errors()) {
                        if (!is_last_partial) {
                            for (auto& le : item_result.error().line_errors()) {
                                errors.push_back(le);
                            }
                        }
                    } else if (!is_last_partial) {
                        errors.push_back(std::make_shared<ValLineError>(ValLineError{
                            ErrorType(ErrorType::Kind::CustomError), state.location(), "Item validation failed"}));
                    }
                } else {
                    // Validation succeeded — convert the validated result to a Python object
                    // (the inner validator may have transformed the value, e.g. AfterValidator)
                    py::object validated_item;
                    try {
                        validated_item = value_to_python_with_type(item_result.value(), items_schema->effective_result_name());
                    } catch (...) {
                        // Fallback: use the original input element
                        validated_item = element;
                    }
                    result_list.append(validated_item);
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

    std::string display_name() const override {
        if (display_name_cache_) return *display_name_cache_;
        std::string inner = items_schema ? items_schema->display_name() : std::string("any");
        // Rust caches the list name only once its item name resolves; while the
        // item is a definition still being built it stays "list[...]" and is
        // recomputed on the next call.
        if (inner == "...") return "list[...]";
        display_name_cache_ = "list[" + inner + "]";
        return *display_name_cache_;
    }
};

// DictValidator - validates dict/object values
class DictValidator : public Validator {
public:
    std::optional<bool> strict;
    bool fail_fast = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<Validator> keys_schema;   // Inner validator for dict keys
    std::shared_ptr<Validator> values_schema; // Inner validator for dict values
    mutable std::optional<std::string> display_name_cache_;

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
                return py_date_object(d);
            }
        }
        if (name == "time") {
            if (auto* et = static_cast<EitherTime*>(value.get())) {
                auto& t = et->value;
                return py_time_object(t);
            }
        }
        if (name == "datetime") {
            if (auto* edt = static_cast<EitherDateTime*>(value.get())) {
                auto& dt = edt->value;
                return py_datetime_object(dt);
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
        // Note: "json-or-python" is NOT handled here — its effective_result_name()
        // delegates to the inner validator, so the name is always the inner type.
        if (name == "function-after" || name == "function-before" ||
            name == "function-wrap" || name == "function-plain" ||
            name == "py_object" || name == "is-instance" || name == "is-subclass") {
            if (auto* o = static_cast<py::object*>(value.get())) return *o;
        }
        return std::nullopt;
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Rust combines the schema-level flag with the state override.
        auto result = input.validate_dict(state.strict_or_declared(strict));
        if (result.is_err()) {
            return result.error();
        }
        auto& dict = result.value();
        size_t dict_size = dict->size();

        // Length checks
        if (min_length.has_value() && dict_size < min_length.value()) {
            ErrorType err(ErrorType::Kind::DictTooShort);
            err.context()["field_type"] = "Dictionary";
            err.context()["min_length"] = std::to_string(min_length.value());
            err.context()["actual_length"] = std::to_string(dict_size);
            return ValError::line_error(
                std::move(err),
                state.location(),
                input.as_error_value().repr
            );
        }
        if (max_length.has_value() && dict_size > max_length.value()) {
            ErrorType err(ErrorType::Kind::DictTooLong);
            err.context()["field_type"] = "Dictionary";
            err.context()["max_length"] = std::to_string(max_length.value());
            err.context()["actual_length"] = std::to_string(dict_size);
            return ValError::line_error(
                std::move(err),
                state.location(),
                input.as_error_value().repr
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

        // Validate keys and values if schemas provided
        if (keys_schema || values_schema) {
            py::dict result_dict;
            std::vector<std::shared_ptr<ValLineError>> errors;
            // Partial mode: the last entry's value errors are dropped (Rust:
            // enumerate_last_partial — the last item is omitted from the output
            // when its value validation fails).
            bool last_entry = false;
            for (size_t ei = 0; ei < entries.size(); ei++) {
                const auto& entry = entries[ei];
                last_entry = (ei == entries.size() - 1);
                bool is_last_partial = state.is_partial() && last_entry;
                state.location().push(entry.key);
                ValidationState sub_state = state.sub_copy(state.coerce_strings());
                py::object key_obj = dict->get_key(entry.key).value_or(py::str(entry.key));
                py::object val_obj = dict->get_value(entry.key).value_or(py::none());
                bool skip_entry = false;

                // Validate key against keys_schema
                if (keys_schema) {
                    auto key_input = make_sub_input(key_obj);
                    key_input->set_current_location(state.location());
                    auto key_result = keys_schema->validate(*key_input, sub_state);
                    if (key_result.is_err()) {
                        if (fail_fast && !is_last_partial) {
                            state.location().pop();
                            return key_result.error();
                        }
                        if (!is_last_partial && key_result.error().has_line_errors()) {
                            const size_t key_depth =
                                state.location().items.size() - 1;
                            for (auto& le : key_result.error().line_errors()) {
                                // Rust marks a failing dict key with "[key]" right
                                // after the key itself.
                                size_t at = std::min(key_depth + 1, le->location.items.size());
                                le->location.items.insert(le->location.items.begin() + at,
                                                          LocItem("[key]"));
                                errors.push_back(le);
                            }
                        }
                        skip_entry = true;
                    } else {
                        auto converted = validated_to_py(key_result.value(), keys_schema->effective_result_name());
                        if (converted) key_obj = *converted;
                    }
                }
                // Validate value against values_schema
                if (values_schema && !skip_entry) {
                    auto val_input = make_sub_input(val_obj);
                    val_input->set_current_location(state.location());
                    auto val_result = values_schema->validate(*val_input, sub_state);
                    if (val_result.is_err()) {
                        if (fail_fast && !is_last_partial) {
                            state.location().pop();
                            return val_result.error();
                        }
                        if (!is_last_partial && val_result.error().has_line_errors()) {
                            for (auto& le : val_result.error().line_errors()) {
                                errors.push_back(le);
                            }
                        }
                        skip_entry = true;
                    } else {
                        auto converted = validated_to_py(val_result.value(), values_schema->effective_result_name());
                        if (converted) val_obj = *converted;
                    }
                }
                if (!skip_entry) {
                    result_dict[key_obj] = val_obj;
                }
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

    std::string display_name() const override {
        if (display_name_cache_) return *display_name_cache_;
        std::string k = keys_schema ? keys_schema->display_name() : std::string("any");
        std::string v = values_schema ? values_schema->display_name() : std::string("any");
        display_name_cache_ = "dict[" + k + "," + v + "]";
        return *display_name_cache_;
    }
};

// SetValidator - validates set values
class SetValidator : public Validator {
public:
    std::shared_ptr<Validator> items_schema;
    mutable std::optional<std::string> display_name_cache_;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    bool fail_fast = false;
    std::optional<bool> strict;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto seq_result = input.validate_set(state.strict_or_declared(strict));
        if (seq_result.is_err()) {
            return seq_result.error();
        }
        state.floor_exactness(seq_result.value().exactness());
        auto& seq = seq_result.value().value();
        std::vector<py::object> elements;
        for (const auto& entry : seq->entries()) {
            elements.push_back(seq->get_item(entry.index));
        }

        // Rust fills the set item by item: max_length applies to the
        // deduplicated output as soon as an item lands in it (and reports no
        // actual length, because the input may have held far more items), while
        // min_length is only checked once the set is complete.
        py::set result_set;
        std::vector<std::shared_ptr<ValLineError>> errors;
        for (size_t index = 0; index < elements.size(); ++index) {
            const py::object& element = elements[index];
            py::object validated = element;
            if (items_schema) {
                state.location().push(static_cast<int64_t>(index));
                PythonInput elem_input(element);
                elem_input.set_current_location(state.location());
                auto item_result = items_schema->validate(elem_input, state);
                state.location().pop();
                if (item_result.is_err()) {
                    if (item_result.error().has_line_errors()) {
                        for (auto& le : item_result.error().line_errors()) {
                            errors.push_back(le);
                        }
                        if (fail_fast) {
                            return ValError::line_errors(std::move(errors));
                        }
                    } else {
                        return item_result.error();
                    }
                    continue;
                }
                validated = value_to_python_with_type(item_result.value(),
                                                      items_schema->effective_result_name());
            }
            result_set.add(validated);
            if (max_length.has_value() && py::len(result_set) > max_length.value()) {
                ErrorType err(ErrorType::Kind::SetTooLong);
                err.context()["field_type"] = "Set";
                err.context()["max_length"] = std::to_string(max_length.value());
                err.set_ctx_object("actual_length", "more", py::none());
                return ValError::line_error(
                    std::move(err), state.location(), input.as_error_value().repr);
            }
        }
        if (!errors.empty()) {
            return ValError::line_errors(std::move(errors));
        }
        size_t set_size = py::len(result_set);
        if (min_length.has_value() && set_size < min_length.value()) {
            ErrorType err(ErrorType::Kind::SetTooShort);
            err.context()["field_type"] = "Set";
            err.context()["min_length"] = std::to_string(min_length.value());
            err.context()["actual_length"] = std::to_string(set_size);
            return ValError::line_error(
                std::move(err), state.location(), input.as_error_value().repr);
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(result_set)));
    }

    std::string name() const override { return "set"; }

    std::string display_name() const override {
        if (display_name_cache_) return *display_name_cache_;
        std::string inner = items_schema ? items_schema->display_name() : std::string("any");
        display_name_cache_ = "set[" + inner + "]";
        return *display_name_cache_;
    }
};

// FrozenSetValidator - validates frozenset values
class FrozenSetValidator : public Validator {
public:
    std::shared_ptr<Validator> items_schema;
    mutable std::optional<std::string> display_name_cache_;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    bool fail_fast = false;
    std::optional<bool> strict;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Collect source items. Mirrors the Rust implementation: frozensets
        // match exactly; sets, lists, tuples and general iterables are
        // accepted in lax mode only (never str/bytes/dict-like inputs).
        std::vector<py::object> items;
        bool matched = false;

        if (auto* py_input = dynamic_cast<const PythonInput*>(&input)) {
            const py::object& obj = py_input->py_object();
            const bool strict_mode = state.strict_or_declared(strict);

            if (py::isinstance<py::frozenset>(obj)) {
                matched = true;
                for (auto handle : obj) {
                    items.push_back(py::reinterpret_borrow<py::object>(handle));
                }
            } else if (!strict_mode && !py::isinstance<py::str>(obj)
                       && !py::isinstance<py::bytes>(obj)
                       && !py::isinstance<py::bytearray>(obj)
                       && !py::isinstance<py::dict>(obj)) {
                // Anything but a frozenset reaches the value through iteration.
                state.floor_exactness(Exactness::Lax);
                try {
                    for (auto handle : obj) {
                        items.push_back(py::reinterpret_borrow<py::object>(handle));
                    }
                    matched = true;
                } catch (const py::error_already_set&) {
                    PyErr_Clear();
                }
            }
        } else {
            // Non-Python inputs (e.g. JSON): accept array-like values.
            auto result = input.validate_list(state.strict_or(false));
            if (result.is_ok()) {
                auto& list = result.value().value();
                matched = true;
                for (const auto& entry : list->entries()) {
                    items.push_back(list->get_item(entry.index));
                }
            }
        }

        if (!matched) {
            return ValError::line_error(
                PydanticKnownError::frozenset_type(),
                state.location(),
                input.as_error_value().repr
            );
        }

        // Same order as Rust: max_length watches the deduplicated output while
        // items are added (without reporting an actual length), and min_length
        // runs once the whole collection is built.
        py::set result_set;
        std::vector<std::shared_ptr<ValLineError>> errors;
        size_t index = 0;
        for (auto& element : items) {
            py::object validated = element;
            if (items_schema) {
                state.location().push(static_cast<int64_t>(index));
                PythonInput elem_input(element);
                elem_input.set_current_location(state.location());
                auto item_result = items_schema->validate(elem_input, state);
                state.location().pop();
                if (item_result.is_ok()) {
                    validated = value_to_python_with_type(item_result.value(), items_schema->effective_result_name());
                } else if (item_result.error().has_line_errors()) {
                    for (auto& le : item_result.error().line_errors()) errors.push_back(le);
                    if (fail_fast) return ValError::line_errors(std::move(errors));
                    index++;
                    continue;
                } else {
                    return item_result.error();
                }
            }
            result_set.add(validated);
            if (max_length.has_value() && py::len(result_set) > max_length.value()) {
                ErrorType err(ErrorType::Kind::SetTooLong);
                err.context()["field_type"] = "Frozenset";
                err.context()["max_length"] = std::to_string(max_length.value());
                err.set_ctx_object("actual_length", "more", py::none());
                return ValError::line_error(
                    std::move(err),
                    state.location(),
                    input.as_error_value().repr
                );
            }
            index++;
        }
        if (!errors.empty()) {
            return ValError::line_errors(std::move(errors));
        }
        size_t set_size = py::len(result_set);
        if (min_length.has_value() && set_size < min_length.value()) {
            ErrorType err(ErrorType::Kind::SetTooShort);
            err.context()["field_type"] = "Frozenset";
            err.context()["min_length"] = std::to_string(min_length.value());
            err.context()["actual_length"] = std::to_string(set_size);
            return ValError::line_error(
                std::move(err),
                state.location(),
                input.as_error_value().repr
            );
        }
        py::frozenset fs = py::frozenset(result_set);
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(fs)));
    }

    std::string name() const override { return "frozenset"; }

    std::string display_name() const override {
        if (display_name_cache_) return *display_name_cache_;
        std::string inner = items_schema ? items_schema->display_name() : std::string("any");
        display_name_cache_ = "frozenset[" + inner + "]";
        return *display_name_cache_;
    }
};

// TupleValidator - validates tuple values with positional items
class TupleValidator : public Validator {
public:
    std::optional<bool> strict;
    bool variadic = false;  // If true, last item_schema is repeated for remaining items
    std::vector<std::shared_ptr<Validator>> items; // Positional item validators
    mutable std::optional<std::string> display_name_cache_;
    bool fail_fast = false;

    TupleValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
auto result = input.validate_tuple(state.strict_or_declared(strict));
        if (result.is_err()) {
            return result.error();
        }
        state.floor_exactness(result.value().exactness());
        auto& tuple_match = result.value();
        auto& tuple = tuple_match.value();
        size_t tuple_size = tuple->size();

        // Build result tuple, validating items if we have item schemas.
        // Collect all errors (Rust aggregates them) so partial mode can
        // suppress the last-partial-key errors.
        std::vector<std::shared_ptr<ValLineError>> errors;
        py::tuple result_tuple(tuple_size);
        auto entries = tuple->entries();
        for (size_t i = 0; i < entries.size(); i++) {
            py::object element = tuple->get_item(i);
            if (!items.empty()) {
                size_t schema_idx = (variadic && i >= items.size()) ? items.size() - 1 : i;
                if (schema_idx < items.size() && items[schema_idx]) {
                    state.location().push(static_cast<int64_t>(i));
                    PythonInput elem_input(element);
                    elem_input.set_current_location(state.location());
                    auto item_result = items[schema_idx]->validate(elem_input, state);
                    state.location().pop();
                    if (item_result.is_ok()) {
                        element = value_to_python_with_type(item_result.value(), items[schema_idx]->effective_result_name());
                    } else {
                        if (item_result.error().has_line_errors()) {
                            for (auto& le : item_result.error().line_errors()) {
                                errors.push_back(le);
                            }
                            if (fail_fast) return ValError::line_errors(std::move(errors));
                        } else {
                            return item_result.error();
                        }
                    }
                }
            }
            result_tuple[i] = element;
        }

        // Enforce fixed tuple length (Rust: tuple.rs pushes Missing for absent
        // items, Extra for surplus items when not variadic).
        if (!items.empty() && !variadic) {
            size_t expected = items.size();
            if (tuple_size < expected) {
                for (size_t i = tuple_size; i < expected; i++) {
                    state.push_loc(static_cast<int64_t>(i));
                    errors.push_back(std::make_shared<ValLineError>(
                        ValLineError{PydanticKnownError::missing(), state.location(), input.as_error_value().repr}));
                    state.pop_loc();
                }
            } else if (tuple_size > expected) {
                for (size_t i = expected; i < tuple_size; i++) {
                    state.push_loc(static_cast<int64_t>(i));
                    errors.push_back(std::make_shared<ValLineError>(
                        ValLineError{ErrorType(ErrorType::Kind::ExtraForbidden), state.location(), input.as_error_value().repr}));
                    state.pop_loc();
                }
            }
        }

        if (!errors.empty()) {
            return ValError::line_errors(std::move(errors));
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(result_tuple)));
    }

    std::string name() const override { return "tuple"; }

    std::string display_name() const override {
        if (display_name_cache_) return *display_name_cache_;
        std::string descr;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) descr += ", ";
            descr += items[i] ? items[i]->display_name() : std::string("any");
        }
        display_name_cache_ = "tuple[" + descr + "]";
        return *display_name_cache_;
    }
};

// GeneratorValidator - validates generator/iterable values, returns list
class GeneratorValidator : public Validator {
public:
    std::shared_ptr<Validator> items_schema;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        py::object py_in = input.as_python_object();

        // Accept any iterable (generators, lists, tuples, etc.); Rust reports a
        // value that cannot be iterated as iterable_type, not list_type.
        if (!py_hasattr(py_in, "__iter__") && !py_hasattr(py_in, "__next__")) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::IterableType),
                state.location(),
                input.as_error_value().repr
            );
        }

        // For list/tuple inputs, validate eagerly and return a list (matching Rust)
        // For generators/iterators, return a lazy ValidatorIterator
        bool is_eager = py::isinstance<py::list>(py_in) || py::isinstance<py::tuple>(py_in);

        if (is_eager) {
            py::list result;
            size_t idx = 0;
            for (auto item : py::iter(py_in)) {
                state.location().push(idx);
                ValidationState sub_state = state.sub_copy(state.coerce_strings());
                if (items_schema) {
                    PythonInput py_item(py::reinterpret_borrow<py::object>(item));
                    py_item.set_current_location(state.location());
                    auto item_result = items_schema->validate(py_item, sub_state);
                    if (item_result.is_err()) {
                        return item_result.error();
                    }
                    auto py_val = value_to_python_with_type(item_result.value(), items_schema->effective_result_name());
                    result.append(py_val);
                } else {
                    result.append(py::reinterpret_borrow<py::object>(item));
                }
                state.location().pop();
                idx++;
            }
            return ValResult<std::shared_ptr<void>>(
                std::make_shared<py::object>(std::move(result))
            );
        }

        // Lazy path: return a ValidatorIterator that validates on demand
        py::object source_iter = py::iter(py_in);
        std::string type_name = items_schema ? items_schema->effective_result_name() : "";
        std::string schema_repr = items_schema ? items_schema->name() : "None";

        // Create a C++ callable that validates a single item
        auto validator = items_schema;
        auto validate_fn = py::cpp_function(
            [validator, type_name](const py::object& item, size_t index) -> py::object {
                if (!validator) {
                    return item;
                }
                PythonInput py_item(py::reinterpret_borrow<py::object>(item));
                ValidationState sub_state;
                sub_state.location().push(index);
                auto result = validator->validate(py_item, sub_state);
                if (result.is_err()) {
                    auto err = result.error();
                    // Leaf validators don't propagate the accumulated location,
                    // so prepend the item index here (matching Rust's
                    // ValidatorIterator, which reports errors at (index, ...)).
                    for (const auto& le : err.line_errors()) {
                        le->location.items.insert(le->location.items.begin(), static_cast<int64_t>(index));
                    }
                    ValidationError ve("validation", InputType::Python, err, py::object(item));
                    throw ve;
                }
                return value_to_python_with_type(result.value(), type_name);
            }
        );

        // Create the Python-level lazy iterator
        py::module_ mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
        py::object LazyValidator = mod.attr("_LazyValidator");
        py::object py_iter = LazyValidator(source_iter, validate_fn, schema_repr);

        return ValResult<std::shared_ptr<void>>(
            std::make_shared<py::object>(py_iter)
        );
    }

    std::string name() const override { return "generator"; }
    std::string effective_result_name() const override { return "py_object"; }
};

} // namespace pydantic_core