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

} // namespace pydantic_core