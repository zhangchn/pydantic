#pragma once

#include "pydantic_core/validator.hpp"
#include <memory>
#include <vector>
#include <string>

namespace pydantic_core {

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
    
    std::string name() const override { return "nullable"; }
    
private:
    std::shared_ptr<Validator> inner_;
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
        for (auto& validator : validators_) {
            auto result = validator->validate(input, state);
            if (result.is_ok()) {
                return result;
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "No union variant matched"
        );
    }
    
    std::string name() const override { return "union"; }
    
private:
    std::vector<std::shared_ptr<Validator>> validators_;
};

// TaggedUnionValidator - union with discriminator tag
class TaggedUnionValidator : public Validator {
public:
    TaggedUnionValidator() = default;
    TaggedUnionValidator(std::string tag, std::vector<std::shared_ptr<Validator>> validators)
        : tag_(std::move(tag)), validators_(std::move(validators)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // For now, just try all validators (Phase 2 will implement proper tag matching)
        for (auto& validator : validators_) {
            auto result = validator->validate(input, state);
            if (result.is_ok()) {
                return result;
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            state.location(),
            "No tagged union variant matched"
        );
    }
    
    std::string name() const override { return "tagged-union"; }
    
private:
    std::string tag_;
    std::vector<std::shared_ptr<Validator>> validators_;
};

// ModelValidator - validates model instances
class ModelValidator : public Validator {
public:
    ModelValidator() : fields_validator_(nullptr) {}
    explicit ModelValidator(std::shared_ptr<Validator> fields_validator)
        : fields_validator_(std::move(fields_validator)) {}
    
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        // Validate using fields validator
        if (fields_validator_) {
            return fields_validator_->validate(input, state);
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    ValResult<std::shared_ptr<void>> validate_assignment(
        const Input& input,
        const std::string& field_name,
        ValidationState& state
    ) override {
        if (fields_validator_) {
            return fields_validator_->validate_assignment(input, field_name, state);
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "model"; }
    
private:
    std::shared_ptr<Validator> fields_validator_;
};

// ModelFieldsValidator - validates model fields
class ModelFieldsValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_dict(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    ValResult<std::shared_ptr<void>> validate_assignment(
        const Input& input,
        const std::string& field_name,
        ValidationState& state
    ) override {
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "model-fields"; }
};

// TypedDictValidator - validates typed dict structures
class TypedDictValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_dict(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "typed-dict"; }
};

} // namespace pydantic_core