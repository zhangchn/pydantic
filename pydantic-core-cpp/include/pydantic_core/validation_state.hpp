#pragma once

#include <optional>
#include <string>
#include "speedate.hpp"
#include "types.hpp"
#include "recursion_guard.hpp"
#include "errors.hpp"

#ifdef HAS_PYBIND11
#include <pybind11/pybind11.h>
namespace py = pybind11;
#endif

namespace pydantic_core {

// Validation state passed through validation chain
// Contains configuration, context, recursion guard, and tracking info
// Matches Rust's ValidationState struct
class ValidationState {
public:
    // Configuration for validation
    struct Config {
        std::optional<bool> strict;
        std::optional<ExtraBehavior> extra_behavior;
        std::optional<bool> from_attributes;
        StringCacheMode cache_strings = StringCacheMode::All;
        std::optional<bool> by_alias;
        std::optional<bool> by_name;
        TimestampUnit val_temporal_unit = TimestampUnit::Infer;
    };
    
    ValidationState() = default;
    
    // Create new validation state with config
    explicit ValidationState(const Config& config)
        : config_(config) {}
    
    // Create with all parameters
    ValidationState(
        const Config& config,
        RecursionState* recursion_state,
        PartialMode allow_partial = PartialMode::Off,
        std::optional<std::string> field_name = std::nullopt,
        const void* self_instance = nullptr
    ) : config_(config), recursion_state_(recursion_state ? recursion_state : &default_recursion_),
        allow_partial_(allow_partial), field_name_(field_name), self_instance_(self_instance) {}
    
    // Accessors
    std::optional<bool> strict() const { return config_.strict; }
    void set_strict(bool value) { config_.strict = value; }
    std::optional<ExtraBehavior> extra_behavior() const { return config_.extra_behavior; }
    void set_extra_behavior(ExtraBehavior value) { config_.extra_behavior = value; }
    std::optional<bool> from_attributes() const { return config_.from_attributes; }
    void set_from_attributes(bool value) { config_.from_attributes = value; }
    StringCacheMode cache_strings() const { return config_.cache_strings; }
    std::optional<bool> by_alias() const { return config_.by_alias; }
    void set_by_alias(bool value) { config_.by_alias = value; }
    std::optional<bool> by_name() const { return config_.by_name; }
    void set_by_name(bool value) { config_.by_name = value; }
    TimestampUnit val_temporal_unit() const { return config_.val_temporal_unit; }
    
    // Determine strict mode - use state setting or validator's default
    bool strict_or(bool default_strict) const {
        return config_.strict.value_or(default_strict);
    }

    // Rust resolves is_strict(schema, config) once per validator, and an explicit
    // schema flag outranks the config value.  The port keeps the model-wide config
    // in the state, so a schema-declared flag has to outrank it here as well.
    bool strict_or_declared(std::optional<bool> declared, bool default_strict = false) const {
        if (declared.has_value()) return *declared;
        return config_.strict.value_or(default_strict);
    }
    
    // Extra behavior - use state setting or default
    ExtraBehavior extra_behavior_or(ExtraBehavior default_behavior) const {
        return config_.extra_behavior.value_or(default_behavior);
    }
    
    // From attributes
    bool from_attributes_or(bool default_val) const {
        return config_.from_attributes.value_or(default_val);
    }
    
    // Partial validation
    PartialMode allow_partial() const { return allow_partial_; }
    void set_allow_partial(PartialMode mode) { allow_partial_ = mode; }
    bool is_partial() const { return allow_partial_ != PartialMode::Off; }
    bool is_partial_trailing_strings() const {
        return allow_partial_ == PartialMode::TrailingStrings;
    }
    
    // Field name (for error context)
    std::optional<std::string> field_name() const { return field_name_; }
    void set_field_name(const std::string& name) { field_name_ = name; }
    
    // Self instance (for model validation)
    const void* self_instance() const { return self_instance_; }
    void set_self_instance(const void* instance) { self_instance_ = instance; }
    
    // Input type
    InputType input_type() const { return input_type_; }
    void set_input_type(InputType type) { input_type_ = type; }

    // Strings mode (validate_strings): string values always coerce regardless
    // of strict, matching Rust's StringInput semantics.
    bool coerce_strings() const { return coerce_strings_; }
    void set_coerce_strings(bool v) { coerce_strings_ = v; }

    // Return a copy of this state (fresh construction, since the implicit copy
    // constructor is deleted).  When force_lax is set, strict is disabled.
    // Container validators use this for sub-validation in strings mode.
    // Set the field name from an optional (used by model-fields validation
    // which scopes a per-field name, matching Rust's scoped_set_field_name).
    void set_field_name_opt(std::optional<std::string> name) { field_name_ = std::move(name); }

    ValidationState sub_copy(bool force_lax = false) const {
        ValidationState s(config_);
        if (force_lax) {
            s.config_.strict = false;
        }
        s.recursion_state_ = recursion_state_;
        s.allow_partial_ = allow_partial_;
        s.field_name_ = field_name_;
        s.self_instance_ = self_instance_;
        s.input_type_ = input_type_;
        s.exactness_ = exactness_;
        s.context_ = context_;
        s.coerce_strings_ = coerce_strings_;
        s.location_ = location_;
#ifdef HAS_PYBIND11
        s.context_py_ = context_py_;
        s.data_ = data_;
#endif
        return s;
    }
    
    // Exactness tracking
    Exactness exactness() const { return exactness_; }
    void set_exactness(Exactness e) { exactness_ = e; }
    
    // Context (for validation functions)
    void* context() const { return context_; }
    void set_context(void* ctx) { context_ = ctx; }

#ifdef HAS_PYBIND11
    // Python context (for Python callable validators)
    py::object context_py() const { return context_py_; }
    void set_context_py(py::object ctx) { context_py_ = std::move(ctx); }

    // Accumulated validated field data (Rust's state.data).  Model-fields
    // validation scopes a dict here while fields are validated; Python
    // callable validators receive it as ValidationInfo.data so V1-style
    // validators observe previously-validated fields.
    py::object data() const { return data_; }
    void set_data(py::object d) { data_ = std::move(d); }

    // Pointer identity of the TOP-LEVEL user input (seeded by the binding
    // for BaseModel.__init__ validation).  Used by function-after to detect
    // that IT is the outermost validator (nested validators see sub-objects).
    const void* top_input_ptr() const { return top_input_ptr_; }
    void set_top_input_ptr(const void* p) { top_input_ptr_ = p; }

    // The BaseModel.__init__ self instance (Python object).  When set, the
    // outermost model-level after-function constructs/populates THIS object
    // instead of a fresh instance (Rust validate_init semantics).
    const py::object& init_self_py() const { return init_self_py_; }
    void set_init_self_py(py::object o) { init_self_py_ = std::move(o); }

    // True while running model validate_assignment: unknown validator
    // exceptions (RuntimeError etc.) propagate instead of becoming errors.
    bool in_assignment = false;

    // Pre-function-call snapshot of the validated fields (3-tuple or flattened
    // dunder-dict) taken by the outermost fields-position function-after.
    // The BaseModel.__init__ binding uses it to populate self_instance even
    // when the validator returns a different instance (Rust validate_init
    // semantics: non-self returns are ignored, self keeps its own fields).
    const py::object& init_fields_snapshot() const { return init_fields_snapshot_; }
    void set_init_fields_snapshot(py::object s) { init_fields_snapshot_ = std::move(s); }
#endif
    
    // Recursion management
    RecursionState::RecursionEntry enter_recursion(const void* obj) {
        return recursion_state_->enter(obj);
    }
    
    int recursion_depth() const { return recursion_state_->depth(); }
    bool at_recursion_limit() const { return recursion_state_->at_limit(); }
    
    // Location tracking for errors
    Location& location() { return location_; }
    const Location& location() const { return location_; }
    
    void push_loc(int64_t index) { location_.push(index); }
    void push_loc(const std::string& key) { location_.push(key); }
    void pop_loc() { location_.pop(); }
    
    // Convenience methods for validators
    void push_index(size_t index) { location_.push(static_cast<int64_t>(index)); }
    void push_key(const std::string& key) { location_.push(key); }
    void pop_location() { location_.pop(); }
    
    // Create child state for nested validation
    ValidationState child(int64_t index) {
        ValidationState child(config_);
        child.recursion_state_ = recursion_state_;
        child.location_ = location_;
        child.push_loc(index);
        child.allow_partial_ = allow_partial_;
        child.field_name_ = field_name_;
        child.self_instance_ = self_instance_;
        child.input_type_ = input_type_;
        child.exactness_ = exactness_;
        child.context_ = context_;
        child.coerce_strings_ = coerce_strings_;
#ifdef HAS_PYBIND11
        child.data_ = data_;
        child.top_input_ptr_ = top_input_ptr_;
#endif
        return child;
    }

    ValidationState child(const std::string& key) {
        ValidationState child(config_);
        child.recursion_state_ = recursion_state_;
        child.location_ = location_;
        child.push_loc(key);
        child.allow_partial_ = allow_partial_;
        child.field_name_ = field_name_;
        child.self_instance_ = self_instance_;
        child.input_type_ = input_type_;
        child.exactness_ = exactness_;
        child.context_ = context_;
        child.coerce_strings_ = coerce_strings_;
#ifdef HAS_PYBIND11
        child.data_ = data_;
        child.top_input_ptr_ = top_input_ptr_;
#endif
        return child;
    }

private:
    Config config_;
    RecursionState* recursion_state_ = &default_recursion_;
    RecursionState default_recursion_;

    PartialMode allow_partial_ = PartialMode::Off;
    std::optional<std::string> field_name_;
    const void* self_instance_ = nullptr;
    InputType input_type_ = InputType::Python;
    Exactness exactness_ = Exactness::Unknown;
    void* context_ = nullptr;
    bool coerce_strings_ = false;
#ifdef HAS_PYBIND11
    py::object context_py_ = py::none();
    py::object data_ = py::none();
    const void* top_input_ptr_ = nullptr;
    py::object init_fields_snapshot_ = py::none();
    py::object init_self_py_ = py::none();
#endif

    Location location_;

    // Rust returns ValError::InternalErr through `?`, so an internal error that
    // carries a Python exception aborts validation instead of joining the
    // aggregated line errors. Containers stash it here for their caller to
    // return once the stack has unwound far enough.
public:
    void set_hard_error(ValError err) { hard_error_ = std::move(err); }
    bool has_hard_error() const { return hard_error_.has_value(); }
    void clear_hard_error() { hard_error_.reset(); }
    ValError take_hard_error() {
        ValError out = std::move(*hard_error_);
        hard_error_.reset();
        return out;
    }

private:
    std::optional<ValError> hard_error_;
};

#ifdef HAS_PYBIND11
// RAII helper mirroring Rust's ValidationState::scoped_set_data: installs a
// data dict for the enclosing scope and restores the previous one afterwards
// (including on exception unwind).
class ScopedValidationData {
public:
    ScopedValidationData(ValidationState& state, py::object data)
        : state_(state), previous_(state.data()) {
        state_.set_data(std::move(data));
    }
    ~ScopedValidationData() { state_.set_data(std::move(previous_)); }

private:
    ValidationState& state_;
    py::object previous_;
};
#endif

} // namespace pydantic_core