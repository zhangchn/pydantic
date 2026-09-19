#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/py_compat.hpp"
#include "pydantic_core/python_input.hpp"
#include "pydantic_core/string_input.hpp"
#include "pydantic_core/py_time.hpp"
#include "pydantic_core/validators/functions.hpp"
#include <memory>
#include <optional>
#include <algorithm>
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
        auto result = input.validate_list(state.container_strict(strict));
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
                    // Rust drops an item whose validator omitted it; the list
                    // simply gets shorter.
                    if (item_result.error().is_omit()) {
                        state.location().pop();
                        continue;
                    }
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

    void visit_refs(RefVisitor visit, void* arg) const override {
        if (!gc_detail::enter_node(this)) return;
        if (items_schema) items_schema->visit_refs(visit, arg);
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
        // A nested model stores its validated fields output (or the instance it
        // reused, behind the "maybe_wrapper:" prefix), which the list path
        // converts through value_to_python_with_type. Without it a dict value
        // kept the untouched input object and every coercion the value validator
        // made was dropped.
        if (name == "model" || name == "model-fields" || name == "typed-dict" ||
            name == "dataclass" || name == "dataclass-args" ||
            name.rfind("maybe_wrapper:", 0) == 0) {
            return value_to_python_with_type(value, name);
        }
        return std::nullopt;
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Rust combines the schema-level flag with the state override.
        auto result = input.validate_dict(state.container_strict(strict));
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
                py::object key_obj = dict->get_key(entry.key).value_or(py::str(entry.key));
                push_key_loc(state.location(), key_obj, entry.key);
                ValidationState sub_state = state.sub_copy(state.coerce_strings());
                py::object val_obj = dict->get_value(entry.key).value_or(py::none());
                bool skip_entry = false;

                // Validate key against keys_schema
                if (keys_schema) {
                    auto key_input = make_sub_input(key_obj);
                    key_input->set_current_location(state.location());
                    auto key_result = keys_schema->validate(*key_input, sub_state);
                    if (key_result.is_err()) {
                        // Rust skips the whole entry when the key omits.
                        if (key_result.error().is_omit()) {
                            state.location().pop();
                            continue;
                        }
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
                        // A value that omits drops the entry, as in Rust.
                        if (val_result.error().is_omit()) {
                            state.location().pop();
                            continue;
                        }
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

    // Rust builds the location item straight from the key object, so a dict with
    // int keys reports an integer index instead of the stringified key.
    static void push_key_loc(Location& location, const py::object& key_obj, const std::string& fallback) {
        if (py::isinstance<py::str>(key_obj)) {
            try {
                location.push(key_obj.cast<std::string>());
                return;
            } catch (...) {}
        }
        if (PyLong_Check(key_obj.ptr())) {
            try {
                location.push(py::cast<int64_t>(key_obj));
                return;
            } catch (...) {}
        }
        try {
            location.push(py::repr(key_obj).cast<std::string>());
            return;
        } catch (...) {}
        location.push(fallback);
    }

    std::string name() const override { return "dict"; }

    std::string display_name() const override {
        if (display_name_cache_) return *display_name_cache_;
        std::string k = keys_schema ? keys_schema->display_name() : std::string("any");
        std::string v = values_schema ? values_schema->display_name() : std::string("any");
        display_name_cache_ = "dict[" + k + "," + v + "]";
        return *display_name_cache_;
    }

    void visit_refs(RefVisitor visit, void* arg) const override {
        if (!gc_detail::enter_node(this)) return;
        if (keys_schema) keys_schema->visit_refs(visit, arg);
        if (values_schema) values_schema->visit_refs(visit, arg);
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
        auto seq_result = input.validate_set(state.container_strict(strict));
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
                    if (item_result.error().is_omit()) continue;
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

    void visit_refs(RefVisitor visit, void* arg) const override {
        if (!gc_detail::enter_node(this)) return;
        if (items_schema) items_schema->visit_refs(visit, arg);
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
            const bool strict_mode = state.container_strict(strict);

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
            auto result = input.validate_list(state.container_strict(strict));
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
                } else if (item_result.error().is_omit()) {
                    index++;
                    continue;
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

    void visit_refs(RefVisitor visit, void* arg) const override {
        if (!gc_detail::enter_node(this)) return;
        if (items_schema) items_schema->visit_refs(visit, arg);
    }
};

// DequeValidator - validates collections.deque values
class DequeValidator : public Validator {
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
        auto seq_result = input.validate_deque(state.container_strict(strict));
        if (seq_result.is_err()) {
            return seq_result.error();
        }
        state.floor_exactness(seq_result.value().exactness());
        auto& seq = seq_result.value().value();

        // `maxlen` is preserved from the input deque (if any). The validated
        // output can't have more items than the input, so the deque
        // constructor will never truncate the items here.
        std::optional<size_t> maxlen;
        if (const auto* py_input = dynamic_cast<const PythonInput*>(&input)) {
            maxlen = py_input->deque_maxlen();
        }

        // Rust watches max_length while items are appended and reports the
        // length of the input, which is ahead of the output while items are
        // still being validated; min_length only applies to the finished deque.
        const size_t actual_length = seq->size();
        py::list items;
        std::vector<std::shared_ptr<ValLineError>> errors;
        for (const auto& entry : seq->entries()) {
            const py::object element = seq->get_item(entry.index);
            py::object validated = element;
            if (items_schema) {
                state.location().push(entry.index);
                PythonInput elem_input(element);
                elem_input.set_current_location(state.location());
                auto item_result = items_schema->validate(elem_input, state);
                state.location().pop();
                if (item_result.is_err()) {
                    if (item_result.error().is_omit()) continue;
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
            items.append(validated);
            if (max_length.has_value() && py::len(items) > max_length.value()) {
                ErrorType err(ErrorType::Kind::SetTooLong);
                err.context()["field_type"] = "Deque";
                err.context()["max_length"] = std::to_string(max_length.value());
                err.context()["actual_length"] = std::to_string(actual_length);
                return ValError::line_error(
                    std::move(err), state.location(), input.as_error_value().repr);
            }
        }
        if (!errors.empty()) {
            return ValError::line_errors(std::move(errors));
        }
        size_t length = py::len(items);
        if (min_length.has_value() && length < min_length.value()) {
            ErrorType err(ErrorType::Kind::SetTooShort);
            err.context()["field_type"] = "Deque";
            err.context()["min_length"] = std::to_string(min_length.value());
            err.context()["actual_length"] = std::to_string(length);
            return ValError::line_error(
                std::move(err), state.location(), input.as_error_value().repr);
        }
        return ValResult<std::shared_ptr<void>>(
            std::make_shared<py::object>(py_deque_new(items, maxlen)));
    }

    std::string name() const override { return "deque"; }

    std::string display_name() const override {
        if (display_name_cache_) return *display_name_cache_;
        std::string inner = items_schema ? items_schema->display_name() : std::string("any");
        display_name_cache_ = "deque[" + inner + "]";
        return *display_name_cache_;
    }

    void visit_refs(RefVisitor visit, void* arg) const override {
        if (!gc_detail::enter_node(this)) return;
        if (items_schema) items_schema->visit_refs(visit, arg);
    }
};

// TupleValidator - validates tuple values with positional items
class TupleValidator : public Validator {
public:
    std::optional<bool> strict;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    bool variadic = false;  // If true, last item_schema is repeated for remaining items
    // A schema with items_schema and no variadic_item_index fixes the length,
    // including the empty tuple.
    bool fixed = false;
    std::vector<std::shared_ptr<Validator>> items; // Positional item validators
    mutable std::optional<std::string> display_name_cache_;
    bool fail_fast = false;

    TupleValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_tuple(state.container_strict(strict));
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
        // Rust measures max_length against what it has pushed into the output
        // (tuple.rs:246-265), and an item that failed was never pushed.
        size_t pushed = 0;
        auto entries = tuple->entries();
        for (size_t i = 0; i < entries.size(); i++) {
            py::object element = tuple->get_item(i);
            bool pushed_item = true;
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
                        pushed_item = false;
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
            if (pushed_item) pushed += 1;
            // The limit is checked as the item lands, and answers with this one
            // error instead of validating what is left of the input.
            if (max_length.has_value() && pushed > max_length.value()) {
                ErrorType err(ErrorType::Kind::TooLong);
                err.context()["field_type"] = "Tuple";
                err.set_ctx_object("max_length", std::to_string(max_length.value()),
                                   py::int_(static_cast<int>(max_length.value())));
                err.set_ctx_object("actual_length", std::to_string(tuple_size),
                                   py::int_(static_cast<int>(tuple_size)));
                return ValError::line_error(std::move(err), state.location(), input.as_error_value().repr);
            }
        }

        // Enforce fixed tuple length (Rust: tuple.rs pushes Missing for absent
        // items and replaces everything with one TooLong when items remain).
        if (fixed && !variadic) {
            size_t expected = items.size();
            if (tuple_size > expected) {
                ErrorType err(ErrorType::Kind::TooLong);
                err.context()["field_type"] = "Tuple";
                err.set_ctx_object("max_length", std::to_string(expected),
                                   py::int_(static_cast<int>(expected)));
                err.set_ctx_object("actual_length", std::to_string(tuple_size),
                                   py::int_(static_cast<int>(tuple_size)));
                return ValError::line_error(err, state.location(), input.as_error_value().repr);
            }
            if (tuple_size < expected) {
                for (size_t i = tuple_size; i < expected; i++) {
                    state.push_loc(static_cast<int64_t>(i));
                    errors.push_back(std::make_shared<ValLineError>(
                        ValLineError{PydanticKnownError::missing(), state.location(), input.as_error_value().repr}));
                    state.pop_loc();
                }
            }
        }

        // Rust checks min_length on the finished output, and joins it to the
        // item errors rather than replacing them (tuple.rs:292-305).
        if (min_length.has_value() && pushed < min_length.value()) {
            ErrorType err(ErrorType::Kind::TooShort);
            err.context()["field_type"] = "Tuple";
            err.set_ctx_object("min_length", std::to_string(min_length.value()),
                               py::int_(static_cast<int>(min_length.value())));
            err.set_ctx_object("actual_length", std::to_string(pushed),
                               py::int_(static_cast<int>(pushed)));
            errors.push_back(std::make_shared<ValLineError>(
                ValLineError{std::move(err), state.location(), input.as_error_value().repr}));
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

    void visit_refs(RefVisitor visit, void* arg) const override {
        if (!gc_detail::enter_node(this)) return;
        for (const auto& item : items) {
            if (item) item->visit_refs(visit, arg);
        }
    }
};

// NamedTupleValidator - rebuilds an instance of a named tuple class from a
// (named) tuple, list, dict, mapping or JSON array/object.  Rust: named_tuple.rs.
class NamedTupleValidator : public Validator {
public:
    struct Field {
        std::string name;
        std::shared_ptr<Validator> validator;
        std::string alias;  // validation_alias's lookup key; empty without an alias
    };

    py::object cls = py::none();
    std::string tuple_name;
    std::vector<Field> fields;
    bool loc_by_alias = true;
    std::optional<bool> validate_by_alias;
    std::optional<bool> validate_by_name;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // A named tuple is rebuilt as one instance, so a partially validated
        // item could never be handed to the class (Rust disables this too).
        state.set_allow_partial(PartialMode::Off);

        if (is_named_tuple_instance(input)) {
            auto tup = input.validate_tuple(false);
            if (tup.is_err()) {
                return tup.error();
            }
            auto items = validate_sequence(*tup.value().value(), input, state, true);
            if (items.is_err()) {
                return items.error();
            }
            // Exactness stays untouched: the nominal class has to remain the
            // best match in a smart union.  Flooring with the tuple match would
            // do exactly that here, since this port's input layer reports a
            // plain tuple as a lax match where Rust reports an exact one.
            return create_instance(items.value(), input, state);
        }

        // Strict mode is ignored, as for the 'call' schema named tuples used
        // to be built from (Rust).
        auto tup = input.validate_tuple(false);
        if (tup.is_ok()) {
            auto items = validate_sequence(*tup.value().value(), input, state, false);
            if (items.is_err()) {
                return items.error();
            }
            state.floor_exactness(Exactness::Strict);
            return create_instance(items.value(), input, state);
        }
        if (!tup.error().has_line_errors()) {
            return tup.error();
        }

        auto dict = input.validate_dict(false);
        if (dict.is_ok()) {
            auto items = validate_mapping(*dict.value(), input, state);
            if (items.is_err()) {
                return items.error();
            }
            state.floor_exactness(Exactness::Strict);
            return create_instance(items.value(), input, state);
        }
        if (!dict.error().has_line_errors()) {
            return dict.error();
        }

        // Both shapes were rejected, so the input is not something a named tuple
        // can be built from; the two rejected shapes go unreported (Rust).
        ErrorType err(ErrorType::Kind::NamedTupleType);
        err.context()["class_name"] = tuple_name;
        auto line = std::make_shared<ValLineError>(
            ValLineError{err, state.location(), input.as_error_value().repr});
        if (const auto* py_input = dynamic_cast<const PythonInput*>(&input)) {
            line->raw_input_obj = py_input->py_object();
        }
        return ValError::line_errors({std::move(line)});
    }

    std::string name() const override { return "named-tuple"; }

    std::string display_name() const override {
        return tuple_name.empty() ? std::string("named-tuple") : tuple_name;
    }

    // The instance is a live Python object, like the generator validator's.
    std::string effective_result_name() const override { return "py_object"; }

    void visit_refs(RefVisitor visit, void* arg) const override {
        if (!gc_detail::enter_node(this)) return;
        visit_ref(visit, arg, cls);
        for (const auto& field : fields) {
            if (field.validator) field.validator->visit_refs(visit, arg);
        }
    }

private:
    bool is_named_tuple_instance(const Input& input) const {
        if (!cls.ptr() || cls.is_none()) return false;
        const auto* py_input = dynamic_cast<const PythonInput*>(&input);
        if (!py_input) return false;
        try {
            return py::isinstance(py_input->py_object(), cls);
        } catch (py::error_already_set&) {
            PyErr_Clear();
            return false;
        }
    }

    // Items of a tuple/list input, located by field name when the input was an
    // instance of the class and by index otherwise.
    ValResult<std::vector<py::object>> validate_sequence(
        ValidatedTuple& coll,
        const Input& input,
        ValidationState& state,
        bool name_locs
    ) {
        const size_t actual_length = coll.size();
        std::vector<py::object> output;
        std::vector<std::shared_ptr<ValLineError>> errors;

        for (size_t i = 0; i < fields.size(); ++i) {
            const Field& field = fields[i];
            if (name_locs) state.push_loc(field.name);
            else state.push_loc(static_cast<int64_t>(i));

            if (i < actual_length) {
                py::object element = coll.get_item(i);
                PythonInput item_input(element);
                item_input.set_current_location(state.location());
                // Rust scoped_set_field_name: ValidationInfo.field_name is the named
                // tuple field, not whatever the enclosing validator left behind.
                auto outer_field_name = state.field_name();
                state.set_field_name(field.name);
                auto result = field.validator->validate(item_input, state);
                state.set_field_name_opt(std::move(outer_field_name));
                state.pop_loc();
                if (result.is_ok()) {
                    output.push_back(value_to_python_with_type(
                        result.value(), field.validator->effective_result_name()));
                    continue;
                }
                if (result.error().has_line_errors()) {
                    for (auto& le : result.error().line_errors()) errors.push_back(le);
                    continue;
                }
                return result.error();
            }

            // The input ran out: take the default, else report the field as
            // missing where its item would have been.
            auto def = field.validator->default_value(state);
            if (def.is_ok()) {
                py::object value = default_to_python(field.validator, def.value());
                state.pop_loc();
                output.push_back(std::move(value));
                continue;
            }
            ValError def_error = def.error();
            Location loc = state.location();
            state.pop_loc();
            if (!def_error.is_omit()) {
                // A default exists but was rejected; that is reported instead
                // of a missing-field error.
                if (def_error.has_line_errors()) {
                    for (auto& le : def_error.line_errors()) errors.push_back(le);
                    continue;
                }
                return def_error;
            }
            errors.push_back(std::make_shared<ValLineError>(ValLineError{
                PydanticKnownError::missing(), loc, input.as_error_value().repr}));
        }

        // Extra items collapse everything into one error for the whole input,
        // dropping the per-item errors collected so far (Rust).
        if (actual_length > fields.size()) {
            ErrorType err(ErrorType::Kind::TooLong);
            err.context()["field_type"] = "NamedTuple";
            err.set_ctx_object("max_length", std::to_string(fields.size()),
                               py::int_(static_cast<int>(fields.size())));
            err.set_ctx_object("actual_length", std::to_string(actual_length),
                               py::int_(static_cast<int>(actual_length)));
            auto line = std::make_shared<ValLineError>(
                ValLineError{err, state.location(), input.as_error_value().repr});
            if (const auto* py_input = dynamic_cast<const PythonInput*>(&input)) {
                line->raw_input_obj = py_input->py_object();
            }
            return ValError::line_errors({std::move(line)});
        }

        if (!errors.empty()) {
            return ValError::line_errors(std::move(errors));
        }
        return ValResult<std::vector<py::object>>(std::move(output));
    }

    // Fields of a dict/mapping input, looked up by alias then name.
    ValResult<std::vector<py::object>> validate_mapping(
        ValidatedDict& dict,
        const Input& input,
        ValidationState& state
    ) {
        const bool by_alias = validate_by_alias.value_or(state.by_alias().value_or(true));
        // Rust defaults to looking up by alias only; a field name is read from
        // the mapping only when validate_by_name is turned on.
        const bool by_name = validate_by_name.value_or(state.by_name().value_or(false));

        std::vector<py::object> output;
        std::vector<std::shared_ptr<ValLineError>> errors;
        std::vector<std::string> used_keys;

        for (const auto& field : fields) {
            std::vector<std::string> lookup;
            if (by_alias && !field.alias.empty()) lookup.push_back(field.alias);
            if (by_name) lookup.push_back(field.name);
            // A field whose only path was turned off still has to be read from
            // somewhere, so fall back to that path rather than always missing.
            if (lookup.empty()) lookup.push_back(field.alias.empty() ? field.name : field.alias);

            std::optional<py::object> value;
            std::string matched;
            for (const auto& key : lookup) {
                if (auto found = dict.get_value(key)) {
                    value = found;
                    matched = key;
                    break;
                }
            }

            // The alias names the location only when it is what the lookup
            // actually used, and only while loc_by_alias is on (Rust).
            std::string loc_key = field.name;
            if (value && matched != field.name && loc_by_alias) loc_key = matched;

            if (value) {
                used_keys.push_back(matched);
                state.push_loc(loc_key);
                PythonInput item_input(*value);
                item_input.set_current_location(state.location());
                // Rust scoped_set_field_name: ValidationInfo.field_name is the named
                // tuple field, not whatever the enclosing validator left behind.
                auto outer_field_name = state.field_name();
                state.set_field_name(field.name);
                auto result = field.validator->validate(item_input, state);
                state.set_field_name_opt(std::move(outer_field_name));
                state.pop_loc();
                if (result.is_ok()) {
                    output.push_back(value_to_python_with_type(
                        result.value(), field.validator->effective_result_name()));
                    continue;
                }
                if (result.error().has_line_errors()) {
                    for (auto& le : result.error().line_errors()) errors.push_back(le);
                    continue;
                }
                return result.error();
            }

            std::string missing_key = (by_alias && loc_by_alias && !field.alias.empty())
                ? field.alias : field.name;
            state.push_loc(missing_key);
            auto def = field.validator->default_value(state);
            Location loc = state.location();
            state.pop_loc();
            if (def.is_ok()) {
                output.push_back(default_to_python(field.validator, def.value()));
                continue;
            }
            ValError def_error = def.error();
            if (!def_error.is_omit()) {
                if (def_error.has_line_errors()) {
                    // A rejected default is located on the field name, even
                    // when an alias named the field otherwise (Rust).
                    for (auto& le : def_error.line_errors()) errors.push_back(le);
                    continue;
                }
                return def_error;
            }
            errors.push_back(std::make_shared<ValLineError>(ValLineError{
                PydanticKnownError::missing(), loc, input.as_error_value().repr}));
        }

        // A named tuple has nowhere to put extra keys, so they are always
        // forbidden (Rust).
        for (const auto& key : dict.keys()) {
            if (std::find(used_keys.begin(), used_keys.end(), key) != used_keys.end()) {
                continue;
            }
            py::object key_obj = dict.get_key(key).value_or(py::str(key));
            if (!py::isinstance<py::str>(key_obj)) {
                errors.push_back(object_line_error(ErrorType::Kind::InvalidKey,
                                                   loc_with(state, key), key_obj));
                continue;
            }
            std::optional<py::object> extra = dict.get_value(key);
            if (!extra) continue;
            errors.push_back(object_line_error(ErrorType::Kind::ExtraForbidden,
                                               loc_with(state, key), *extra));
        }

        if (!errors.empty()) {
            return ValError::line_errors(std::move(errors));
        }
        return ValResult<std::vector<py::object>>(std::move(output));
    }

    // An error that names the offending object itself as its input.
    static std::shared_ptr<ValLineError> object_line_error(
        ErrorType::Kind kind, const Location& loc, const py::object& obj) {
        auto le = std::make_shared<ValLineError>(
            ValLineError{ErrorType(kind), loc, py::repr(obj).cast<std::string>()});
        le->raw_input_obj = obj;
        return le;
    }

    static Location loc_with(const ValidationState& state, const std::string& key) {
        Location loc = state.location();
        loc.push(key);
        return loc;
    }

    ValResult<std::shared_ptr<void>> create_instance(
        std::vector<py::object>& items,
        const Input& input,
        ValidationState& state
    ) {
        try {
            py::tuple args(items.size());
            for (size_t i = 0; i < items.size(); ++i) args[i] = items[i];
            py::object instance = cls(*args);
            return ValResult<std::shared_ptr<void>>(std::make_shared<py::object>(std::move(instance)));
        } catch (py::error_already_set& e) {
            return function_error_from_exception(e, input, state);
        }
    }
};

// GeneratorValidator - validates generator/iterable values, returns list
class GeneratorValidator : public Validator {
public:
    std::shared_ptr<Validator> items_schema;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;

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

        // Rust's GeneratorValidator never consumes the input: even a plain list
        // becomes a lazy ValidatorIterator, so an item error surfaces on the
        // next() that produced it rather than during validation.
        py::object source_iter = py::iter(py_in);
        std::string type_name = items_schema ? items_schema->effective_result_name() : "";
        std::string schema_repr = items_schema ? items_schema->debug_repr() : "None";

        // Create a C++ callable that validates a single item
        auto validator = items_schema;
        // Rust's ValidatorIterator reports an item error at the index alone:
        // the enclosing field name is attached to a line error only when the
        // validator that owns it returns, and an iterator outlives that call.
        Location base_loc;
        auto validate_fn = py::cpp_function(
            [validator, type_name, base_loc](const py::object& item, size_t index) -> py::object {
                if (!validator) {
                    return item;
                }
                Location loc = base_loc;
                loc.push(static_cast<int64_t>(index));
                PythonInput py_item(py::reinterpret_borrow<py::object>(item));
                py_item.set_current_location(loc);
                ValidationState sub_state;
                sub_state.location() = loc;
                auto result = validator->validate(py_item, sub_state);
                if (result.is_err()) {
                    auto err = result.error();
                    // Leaf validators build their line error from the input's
                    // location, which the item input now carries; only fill in
                    // the path when a leaf reported none of it.
                    for (const auto& le : err.line_errors()) {
                        if (le->location.items.empty()) le->location = loc;
                    }
                    // Rust re-raises the ValidationError and the enclosing
                    // validator casts it back to these line errors, so a Python
                    // callable that consumes the iterator mid-validation keeps the
                    // item's error type and index instead of getting value_error.
                    // The line errors are relative to where the iterator was built,
                    // so the consuming validator re-anchors them on its own location;
                    // parking them here as well would let a stale error outlive it.
                    ValidationError ve("ValidatorIterator", InputType::Python, err, py::object(item));
                    throw ve;
                }
                return value_to_python_with_type(result.value(), type_name);
            }
        );

        // Create the Python-level lazy iterator
        py::module_ mod = py::module_::import("pydantic_core_cpp._pydantic_core_cpp");
        py::object LazyValidator = mod.attr("_LazyValidator");
        py::object py_iter =
            LazyValidator(source_iter, validate_fn, schema_repr, min_length, max_length, py_in);

        return ValResult<std::shared_ptr<void>>(
            std::make_shared<py::object>(py_iter)
        );
    }

    std::string name() const override { return "generator"; }
    std::string effective_result_name() const override { return "py_object"; }

    void visit_refs(RefVisitor visit, void* arg) const override {
        if (!gc_detail::enter_node(this)) return;
        if (items_schema) items_schema->visit_refs(visit, arg);
    }
};

} // namespace pydantic_core