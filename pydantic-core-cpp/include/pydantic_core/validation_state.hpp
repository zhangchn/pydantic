#pragma once

#include <optional>
#include <string>
#include "types.hpp"
#include "recursion_guard.hpp"

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
    };
    
    ValidationState() = default;
    
    // Create new validation state with config
    explicit ValidationState(const Config& config)
        : config_(config), recursion_state_() {}
    
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
    std::optional<bool> from_attributes() const { return config_.from_attributes; }
    StringCacheMode cache_strings() const { return config_.cache_strings; }
    std::optional<bool> by_alias() const { return config_.by_alias; }
    std::optional<bool> by_name() const { return config_.by_name; }
    
    // Determine strict mode - use state setting or validator's default
    bool strict_or(bool default_strict) const {
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
    
    // Exactness tracking
    Exactness exactness() const { return exactness_; }
    void set_exactness(Exactness e) { exactness_ = e; }
    
    // Context (for validation functions)
    void* context() const { return context_; }
    void set_context(void* ctx) { context_ = ctx; }
    
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
    
    Location location_;
};

} // namespace pydantic_core