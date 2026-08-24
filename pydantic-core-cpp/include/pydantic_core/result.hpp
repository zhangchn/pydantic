#pragma once

#include <vector>
#include <variant>
#include <optional>
#include <exception>
#include <stdexcept>
#include <string>
#include <memory>
#include "types.hpp"
#include "errors.hpp"

namespace pydantic_core {

// Validation result type - analogous to Rust's ValResult<T>
template<typename T>
class ValResult {
public:
    using ValueType = T;
    
    // Success constructor
    ValResult(T value) : data_(std::move(value)) {}
    
    // Error constructor
    ValResult(ValError error) : data_(std::make_shared<ValError>(std::move(error))) {}
    
    // Check if successful
    bool is_ok() const { return std::holds_alternative<T>(data_); }
    bool is_err() const { return std::holds_alternative<std::shared_ptr<ValError>>(data_); }
    
    // Get value (throws if error)
    T& value() & {
        if (is_err()) {
            throw std::runtime_error("ValResult holds error, not value");
        }
        return std::get<T>(data_);
    }
    
    const T& value() const & {
        if (is_err()) {
            throw std::runtime_error("ValResult holds error, not value");
        }
        return std::get<T>(data_);
    }
    
    T&& value() && {
        if (is_err()) {
            throw std::runtime_error("ValResult holds error, not value");
        }
        return std::get<T>(std::move(data_));
    }
    
    // Get error (throws if value)
    ValError& error() & {
        if (is_ok()) {
            throw std::runtime_error("ValResult holds value, not error");
        }
        return *std::get<std::shared_ptr<ValError>>(data_);
    }
    
    const ValError& error() const & {
        if (is_ok()) {
            throw std::runtime_error("ValResult holds value, not error");
        }
        return *std::get<std::shared_ptr<ValError>>(data_);
    }
    
    // Get value or default
    T value_or(T default_value) const {
        return is_ok() ? std::get<T>(data_) : std::move(default_value);
    }
    
    // Map function over value
    template<typename F>
    auto map(F f) -> ValResult<decltype(f(std::declval<T&>()))> {
        using ResultType = decltype(f(std::declval<T&>()));
        if (is_ok()) {
            return ValResult<ResultType>(f(std::get<T>(data_)));
        } else {
            return ValResult<ResultType>(*std::get<std::shared_ptr<ValError>>(data_));
        }
    }
    
    // And_then for chaining
    template<typename F>
    auto and_then(F f) -> decltype(f(std::declval<T&>())) {
        using ResultType = decltype(f(std::declval<T&>()));
        if (is_ok()) {
            return f(std::get<T>(data_));
        } else {
            return ResultType(*std::get<std::shared_ptr<ValError>>(data_));
        }
    }
    
private:
    std::variant<T, std::shared_ptr<ValError>> data_;
};

// Validation match type - used for strict vs lax validation results
// Analogous to Rust's ValidationMatch<T>
template<typename T>
class ValMatch {
public:
    // Exact match - no coercion needed
    static ValMatch exact(T value) {
        return ValMatch(std::move(value), Exactness::Exact);
    }
    
    // Lax match - coercion was applied
    static ValMatch lax(T value) {
        return ValMatch(std::move(value), Exactness::Lax);
    }
    
    T& value() { return value_; }
    const T& value() const { return value_; }
    
    Exactness exactness() const { return exactness_; }
    
    bool is_exact() const { return exactness_ == Exactness::Exact; }
    bool is_lax() const { return exactness_ == Exactness::Lax; }
    
    // Require exact match, returns nullptr if lax
    std::optional<T> require_exact() const {
        if (exactness_ == Exactness::Exact) {
            return value_;
        }
        return std::nullopt;
    }
    
private:
    ValMatch(T value, Exactness exactness) 
        : value_(std::move(value)), exactness_(exactness) {}
    
    T value_;
    Exactness exactness_;
};

// Convenience typedef for ValResult<ValMatch<T>>
template<typename T>
using ValResultMatch = ValResult<ValMatch<T>>;

} // namespace pydantic_core