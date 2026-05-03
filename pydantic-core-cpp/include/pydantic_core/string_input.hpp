#pragma once

#include <string>
#include <unordered_map>
#include "input.hpp"

namespace pydantic_core {

// String input implementation
// Used for string-based validation (URL params, query strings, etc.)
class StringInput : public Input {
public:
    // Construct from string mapping (key -> value)
    explicit StringInput(std::unordered_map<std::string, std::string> mapping)
        : mapping_(std::move(mapping)) {}
    
    // Construct from single string value
    explicit StringInput(const std::string& value) : single_value_(value) {}
    
    InputType input_type() const override { return InputType::String; }
    
    InputValue as_error_value() const override;
    
    bool is_none() const override;
    
    // Type validation implementations
    ValResult<ValMatch<EitherString>> validate_str(bool strict, bool coerce_numbers = false) const override;
    ValResult<ValMatch<EitherBytes>> validate_bytes(bool strict) const override;
    ValResult<ValMatch<bool>> validate_bool(bool strict) const override;
    ValResult<ValMatch<EitherInt>> validate_int(bool strict) const override;
    ValResult<ValMatch<EitherFloat>> validate_float(bool strict) const override;
    
    ValResult<std::unique_ptr<ValidatedDict>> validate_dict(bool strict) const override;
    ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_list(bool strict) const override;
    ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> validate_tuple(bool strict) const override;
    
    // Check if this is a single value vs mapping
    bool is_single_value() const { return single_value_.has_value(); }
    bool is_mapping() const { return !mapping_.empty(); }
    
    // Access underlying data
    const std::optional<std::string>& single_value() const { return single_value_; }
    const std::unordered_map<std::string, std::string>& mapping() const { return mapping_; }
    
private:
    std::unordered_map<std::string, std::string> mapping_;
    std::optional<std::string> single_value_;
};

// Validated dict for string mapping
class StringValidatedDict : public ValidatedDict {
public:
    explicit StringValidatedDict(const std::unordered_map<std::string, std::string>& mapping)
        : mapping_(mapping) {}
    
    size_t size() const override { return mapping_.size(); }
    bool empty() const override { return mapping_.empty(); }
    
    std::vector<Entry> entries() const override;
    std::vector<std::string> keys() const override;
    
    bool has_key(const std::string& key) const override;
    std::optional<Entry> get(const std::string& key) const override;
    
private:
    std::unordered_map<std::string, std::string> mapping_;
};

} // namespace pydantic_core