#pragma once

#include "pydantic_core/validator.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

namespace pydantic_core {

// DefinitionsRegistry - stores named validators for recursive schema resolution
// Uses std::shared_ptr<Validator> to avoid circular dependency with CombinedValidator
class DefinitionsRegistry {
public:
    using ValidatorPtr = std::shared_ptr<Validator>;

    void add_definition(const std::string& ref, ValidatorPtr validator) {
        definitions_[ref] = std::move(validator);
    }

    ValidatorPtr get_definition(const std::string& ref) const {
        auto it = definitions_.find(ref);
        if (it != definitions_.end()) {
            return it->second;
        }
        return nullptr;
    }

    bool has_definition(const std::string& ref) const {
        return definitions_.find(ref) != definitions_.end();
    }

private:
    std::unordered_map<std::string, ValidatorPtr> definitions_;
};

// DefinitionRefValidator - resolves recursive schema references
class DefinitionRefValidator : public Validator {
public:
    DefinitionRefValidator() = default;
    
    void set_ref(const std::string& ref) {
        schema_ref_ = ref;
    }
    
    void set_definitions(std::shared_ptr<DefinitionsRegistry> definitions) {
        definitions_ = std::move(definitions);
    }

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (definitions_) {
            auto def = definitions_->get_definition(schema_ref_);
            if (def) {
                return def->validate(input, state);
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "Definition reference not found: " + schema_ref_
        );
    }

    std::string name() const override { return "definition-ref"; }

    const std::string& get_ref() const { return schema_ref_; }

private:
    std::string schema_ref_;
    std::shared_ptr<DefinitionsRegistry> definitions_;
};

// DateValidator - validates date values
class DateValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // In Phase 2, we'll parse date strings
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "date"; }
};

// TimeValidator - validates time values
class TimeValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // In Phase 2, we'll parse time strings
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "time"; }
};

// DatetimeValidator - validates datetime values
class DatetimeValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // In Phase 2, we'll parse datetime strings
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "datetime"; }
};

// TimedeltaValidator - validates timedelta values
class TimedeltaValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // In Phase 2, we'll parse timedelta strings
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "timedelta"; }
};

// UrlValidator - validates URL values
class UrlValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // In Phase 2, we'll parse and validate URLs
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "url"; }
};

// UuidValidator - validates UUID values
class UuidValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // In Phase 2, we'll parse and validate UUIDs
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "uuid"; }
};

// LiteralValidator - validates literal values
class LiteralValidator : public Validator {
public:
    LiteralValidator() = default;
    explicit LiteralValidator(std::vector<std::string> values)
        : values_(std::move(values)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (values_.empty()) {
            return ValError::line_error(
                PydanticKnownError::literal_mismatch(),
                state.location(),
                input.as_error_value().repr
            );
        }
        auto str_result = input.validate_str(state.strict_or(false), false);
        if (str_result.is_err()) {
            return str_result.error();
        }
        const auto& es = str_result.value().value();
        std::string str_val;
        if (auto* s = std::get_if<std::string>(&es.value)) {
            str_val = *s;
        } else {
            str_val = std::string(std::get<std::string_view>(es.value));
        }
        for (const auto& v : values_) {
            if (v == str_val) {
                return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(str_val));
            }
        }
        return ValError::line_error(
            PydanticKnownError::literal_mismatch(),
            state.location(),
            input.as_error_value().repr
        );
    }
    
    std::string name() const override { return "literal"; }
    
private:
    std::vector<std::string> values_;
};

// EnumValidator - validates enum values
class EnumValidator : public Validator {
public:
    EnumValidator() = default;
    explicit EnumValidator(std::unordered_set<std::string> valid_values)
        : valid_values_(std::move(valid_values)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        if (valid_values_.empty()) {
            return ValError::line_error(
                PydanticKnownError::enum_error(),
                state.location(),
                input.as_error_value().repr
            );
        }
        auto str_result = input.validate_str(state.strict_or(false), false);
        if (str_result.is_err()) {
            return str_result.error();
        }
        const auto& es = str_result.value().value();
        std::string str_val;
        if (auto* s = std::get_if<std::string>(&es.value)) {
            str_val = *s;
        } else {
            str_val = std::string(std::get<std::string_view>(es.value));
        }
        if (valid_values_.count(str_val)) {
            return ValResult<std::shared_ptr<void>>(std::make_shared<std::string>(str_val));
        }
        return ValError::line_error(
            PydanticKnownError::enum_error(),
            state.location(),
            input.as_error_value().repr
        );
    }
    
    std::string name() const override { return "enum"; }
    
private:
    std::unordered_set<std::string> valid_values_;
};

} // namespace pydantic_core