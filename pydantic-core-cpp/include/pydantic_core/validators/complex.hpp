#pragma once

#include "pydantic_core/validator.hpp"
#include "pydantic_core/python_input.hpp"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace pydantic_core {

// Forward declaration
py::object value_to_python_with_type(const std::shared_ptr<void>& value, const std::string& type_name);

// NullableValidator - wraps another validator and allows None
class NullableValidator : public Validator {
public:
    NullableValidator() : inner_(nullptr) {}
    explicit NullableValidator(std::shared_ptr<Validator> inner)
        : inner_(std::move(inner)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (input.is_none()) {
            return ValResult<std::shared_ptr<void>>(nullptr);
        }
        return inner_->validate(input, state);
    }
    
    std::string name() const override {
        // Delegate to the inner validator so result conversion uses the
        // actual stored value type instead of the "nullable" wrapper name.
        if (inner_) return inner_->name();
        return "nullable";
    }

    // The result comes straight from the inner validator
    std::string effective_result_name() const override {
        if (inner_) return inner_->effective_result_name();
        return "nullable";
    }

    std::string display_name() const override {
        if (display_name_cache_) return *display_name_cache_;
        std::string out = inner_ ? "nullable[" + inner_->display_name() + "]" : std::string("nullable");
        display_name_cache_ = out;
        return out;
    }
    
private:
    std::shared_ptr<Validator> inner_;
    mutable std::optional<std::string> display_name_cache_;
};

// UnionValidator - tries multiple validators in order
class UnionValidator : public Validator {
public:
    UnionValidator() = default;
    explicit UnionValidator(std::vector<std::shared_ptr<Validator>> validators)
        : validators_(std::move(validators)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // If the input is a Python model instance, prefer the branch whose
        // expected class matches (smart union exact match).  The instance is
        // reused as-is (it is already valid), matching Rust's model validator
        // behavior.  Fall back to left-to-right validation otherwise.
        if (input.input_type() == InputType::Python) {
            const auto& py_input = static_cast<const PythonInput&>(input);
            for (auto& v : validators_) {
                const py::object& cls = v->expected_class();
                if (!cls.is_none() && py::isinstance(py_input.py_object(), cls)) {
                    last_type_name_ = "py_object";
                    return ValResult<std::shared_ptr<void>>(
                        std::make_shared<py::object>(py_input.py_object()));
                }
            }
        }
        // Rust MaybeErrors: keep every choice's line errors and locate each
        // under the choice's schema name, so a failed union reports what went
        // wrong in every branch instead of one generic error. The choice name
        // goes at the union's own depth, after any index the enclosing
        // container already pushed.
        std::vector<std::shared_ptr<ValLineError>> choice_errors;
        const size_t own_loc_depth = state.location().items.size();
        for (auto& validator : validators_) {
            auto result = validator->validate(input, state);
            if (result.is_ok()) {
                // Record which inner validator matched so result conversion
                // can dispatch on the real value type (avoiding unsafe
                // blind casts of the type-erased shared_ptr<void>).
                last_type_name_ = validator->effective_result_name();
                return result;
            }
            if (custom_error_type_) continue;
            const ValError& err = result.error();
            if (!err.has_line_errors()) continue;
            const std::string label = validator->display_name();
            for (const auto& le : err.line_errors()) {
                auto copy = std::make_shared<ValLineError>(*le);
                size_t at = std::min(own_loc_depth, copy->location.items.size());
                copy->location.items.insert(copy->location.items.begin() + at, LocItem(label));
                choice_errors.push_back(std::move(copy));
            }
        }
        if (!choice_errors.empty()) {
            return ValError::line_errors(std::move(choice_errors));
        }
        if (custom_error_type_) {
            // Rust builds a PydanticKnownError when custom_error_type names a known
            // error, so the standard message template applies; only an unknown type
            // falls back to the caller-supplied custom_error_message.
            const std::string& type = *custom_error_type_;
            ErrorType et = ErrorType::build_known_type(type);
            if (et.is_custom()) {
                std::string message = custom_error_message_.value_or("");
                et = ErrorType(type, message.empty() ? "Validation error" : message);
            }
            return ValError::line_error(et, state.location(), input.as_error_value().repr);
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "No union variant matched"
        );
    }

    std::string name() const override {
        // After validation, report the inner validator that matched so result
        // conversion dispatches on the real value type instead of falling
        // into the unsafe try_all casts for "union".
        return last_type_name_.empty() ? "union" : last_type_name_;
    }

    std::string effective_result_name() const override {
        return last_type_name_.empty() ? "union" : last_type_name_;
    }

    std::string display_name() const override {
        if (display_name_cache_) return *display_name_cache_;
        std::string descr;
        for (size_t i = 0; i < validators_.size(); ++i) {
            if (i) descr += ",";
            descr += validators_[i] ? validators_[i]->display_name() : std::string("any");
        }
        display_name_cache_ = "union[" + descr + "]";
        return *display_name_cache_;
    }

    void set_custom_error(std::string type, std::string message) {
        custom_error_type_ = std::move(type);
        custom_error_message_ = std::move(message);
    }

private:
    std::vector<std::shared_ptr<Validator>> validators_;
    mutable std::string last_type_name_;
    mutable std::optional<std::string> display_name_cache_;
    std::optional<std::string> custom_error_type_;
    std::optional<std::string> custom_error_message_;
};

// TaggedUnionValidator - union with discriminator tag
class TaggedUnionValidator : public Validator {
public:
    // A choice keeps its schema key so the tag can be matched by value: pydantic
    // emits int, str and Enum-member keys, and Rust compares them with Python
    // equality (True matches 1, "1" does not).
    struct Choice {
        py::object tag;
        std::shared_ptr<Validator> validator;
    };

    TaggedUnionValidator() = default;

    TaggedUnionValidator(std::vector<std::vector<std::string>> paths,
                         std::string discriminator_repr,
                         std::vector<Choice> choices)
        : paths_(std::move(paths)), discriminator_repr_(std::move(discriminator_repr)),
          choices_(std::move(choices)) {
        build_tags_repr();
    }

    TaggedUnionValidator(py::object callable, std::string discriminator_repr,
                         std::vector<Choice> choices)
        : callable_(std::move(callable)), discriminator_repr_(std::move(discriminator_repr)),
          choices_(std::move(choices)) {
        build_tags_repr();
    }

    void set_custom_error(const std::string& type, const std::string& message) {
        custom_error_type_ = type;
        custom_error_message_ = message;
    }

    void set_from_attributes(bool value) { from_attributes_ = value; }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        py::object tag;
        if (!callable_.is_none()) {
            try {
                tag = py::object(callable_(input.as_python_object()));
            } catch (const py::error_already_set&) {
                throw;  // a raising discriminator belongs to its caller
            }
            if (tag.is_none()) return tag_not_found(input, state);
        } else {
            py::object source;
            if (!fields_source(input, state, &source)) {
                return ValError::line_error(
                    ErrorType(ErrorType::Kind::ModelAttributesType),
                    state.location(), input.as_error_value().repr);
            }
            bool found = false;
            for (const auto& path : paths_) {
                py::object value;
                if (read_key_path(source, path, &value)) {
                    tag = std::move(value);
                    found = true;
                    break;
                }
            }
            if (!found) return tag_not_found(input, state);
        }

        const size_t own_loc_depth = state.location().items.size();
        for (const auto& choice : choices_) {
            if (!choice.tag.ptr()) continue;
            // pybind11's operator== on object/handle is a pointer compare; the tag
            // must match its schema key by Python value.
            int equal = PyObject_RichCompareBool(tag.ptr(), choice.tag.ptr(), Py_EQ);
            if (equal < 0) { PyErr_Clear(); continue; }
            if (!equal) continue;
            auto result = choice.validator->validate(input, state);
            if (result.is_ok()) {
                last_type_name_ = choice.validator->effective_result_name();
                return result;
            }
            // Rust: err.with_outer_location(tag) — the matched tag sits in the
            // location right after the union's own path segment.
            if (result.error().has_line_errors()) {
                auto errors = result.error().line_errors();
                const LocItem loc_item = tag_loc_item(tag);
                for (auto& le : errors) {
                    size_t at = std::min(own_loc_depth, le->location.items.size());
                    le->location.items.insert(le->location.items.begin() + at, loc_item);
                }
                return ValError::line_errors(std::move(errors));
            }
            return result;
        }
        return tag_invalid(input, state, tag);
    }

    std::string name() const override { return "tagged-union"; }

    // The inner value is returned unconverted, so the matched choice decides
    // how it is turned back into a Python object.
    std::string effective_result_name() const override {
        return last_type_name_.empty() ? std::string("py_object") : last_type_name_;
    }

private:
    std::vector<std::vector<std::string>> paths_;
    std::vector<Choice> choices_;
    py::object callable_ = py::none();
    std::string tags_repr_;
    std::string discriminator_repr_;
    bool from_attributes_ = true;
    std::string custom_error_type_;
    std::string custom_error_message_;
    mutable std::string last_type_name_;

    void build_tags_repr() {
        bool first = true;
        for (const auto& choice : choices_) {
            if (!first) tags_repr_ += ", ";
            first = false;
            try { tags_repr_ += py::repr(choice.tag).cast<std::string>(); }
            catch (const py::error_already_set&) { PyErr_Clear(); }
        }
    }

    static LocItem tag_loc_item(const py::object& tag) {
        if (PyLong_Check(tag.ptr()) && !PyBool_Check(tag.ptr())) {
            int64_t value = PyLong_AsLongLong(tag.ptr());
            if (value != -1 || !PyErr_Occurred()) return LocItem(value);
            PyErr_Clear();
        }
        try { return LocItem(py::str(tag).cast<std::string>()); }
        catch (const py::error_already_set&) { PyErr_Clear(); return LocItem(std::string()); }
    }

    static bool read_one(py::handle cur, const std::string& key, py::object* out) {
        if (py::isinstance<py::dict>(cur)) {
            py::dict d = cur.cast<py::dict>();
            if (!d.contains(key)) return false;
            *out = d[py::str(key)];
            return true;
        }
        if (py_hasattr(cur, key.c_str())) {
            *out = py::getattr(cur, key.c_str());
            return true;
        }
        return false;
    }

    // Rust reads the tag through validate_model_fields, so an input that cannot
    // be read as a field container at all (a str, an int, ...) reports
    // model_attributes_type rather than union_tag_not_found.
    bool fields_source(const Input& input, ValidationState& state, py::object* out) const {
        if (auto* py_input = dynamic_cast<const PythonInput*>(&input)) {
            const py::object& obj = py_input->py_object();
            if (py::isinstance<py::dict>(obj)) { *out = obj; return true; }
            if (!state.strict_or(false) && PyMapping_Check(obj.ptr()) &&
                !PyUnicode_Check(obj.ptr()) && !PyBytes_Check(obj.ptr()) &&
                !PyByteArray_Check(obj.ptr()) && !PySequence_Check(obj.ptr())) { *out = obj; return true; }
            if (from_attributes_ && from_attributes_applicable(obj)) { *out = obj; return true; }
            return false;
        }
        auto dict_result = input.validate_dict(state.strict_or(false));
        if (dict_result.is_err()) return false;
        auto& dict = dict_result.value();
        py::dict py_dict;
        for (const auto& key : dict->keys()) {
            auto value = dict->get_value(key);
            if (value) py_dict[py::str(key)] = *value;
        }
        *out = std::move(py_dict);
        return true;
    }

    static bool from_attributes_applicable(py::handle obj) {
        try {
            std::string module = obj.get_type().attr("__module__").cast<std::string>();
            return module != "builtins" && module != "datetime" && module != "collections";
        } catch (const py::error_already_set&) {
            PyErr_Clear();
            return false;
        }
    }

    static bool read_key_path(const py::object& source, const std::vector<std::string>& path,
                              py::object* out) {
        if (path.empty()) return false;
        py::object cur = source;
        for (const auto& key : path) {
            py::object next;
            if (!read_one(cur, key, &next)) return false;
            cur = std::move(next);
        }
        *out = cur;
        return true;
    }

    bool has_custom_error() const { return !custom_error_type_.empty(); }

    ValError custom_error(const Input& input, ValidationState& state) const {
        ErrorType err(ErrorType::Kind::CustomError);
        err.context()["custom_error_type"] = custom_error_type_;
        if (!custom_error_message_.empty()) err.context()["msg"] = custom_error_message_;
        return ValError::line_error(std::move(err), state.location(), input.as_error_value().repr);
    }

    ValError tag_not_found(const Input& input, ValidationState& state) const {
        if (has_custom_error()) return custom_error(input, state);
        ErrorType err(ErrorType::Kind::UnionTagNotFound);
        err.context()["discriminator"] = discriminator_repr_;
        return ValError::line_error(std::move(err), state.location(), input.as_error_value().repr);
    }

    ValError tag_invalid(const Input& input, ValidationState& state, const py::object& tag) const {
        if (has_custom_error()) return custom_error(input, state);
        ErrorType err(ErrorType::Kind::UnionTagInvalid);
        err.context()["discriminator"] = discriminator_repr_;
        err.context()["expected_tags"] = tags_repr_;
        std::string tag_str;
        try { tag_str = py::str(tag).cast<std::string>(); }
        catch (const py::error_already_set&) { PyErr_Clear(); }
        err.context()["tag"] = tag_str;
        return ValError::line_error(std::move(err), state.location(), input.as_error_value().repr);
    }
};

} // namespace pydantic_core