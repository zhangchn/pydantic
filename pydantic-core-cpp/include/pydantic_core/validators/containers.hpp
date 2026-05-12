#pragma once

#include "pydantic_core/validator.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace pydantic_core {

// ListValidator - validates list/array values
class ListValidator : public Validator {
public:
    bool strict = false;
    bool fail_fast = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<Validator> items_schema;  // Inner validator for list items

    ListValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_list(strict);
        if (result.is_err()) {
            return result.error();
        }
        auto& list_match = result.value();
        auto& list = list_match.value();
        size_t list_size = list->size();

        // Length checks
        if (min_length.has_value() && list_size < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TooShort),
                state.location(),
                "list(len=" + std::to_string(list_size) + ")"
            );
        }
        if (max_length.has_value() && list_size > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TooLong),
                state.location(),
                "list(len=" + std::to_string(list_size) + ")"
            );
        }

        // Validate each item against items_schema
        if (items_schema) {
            auto entries = list->entries();
            std::vector<std::shared_ptr<void>> validated_items;
            validated_items.reserve(entries.size());

            for (const auto& entry : entries) {
                state.location().push(entry.index);
                // Create a sub-input for this list item
                // For now, we rely on the input's validate_list to have validated the structure
                // In a full implementation, we'd extract each element and validate it
                state.location().pop();
            }
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(static_cast<int>(list_size)));
    }

    std::string name() const override { return "list"; }
};

// DictValidator - validates dict/object values
class DictValidator : public Validator {
public:
    bool strict = false;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;
    std::shared_ptr<Validator> keys_schema;   // Inner validator for dict keys
    std::shared_ptr<Validator> values_schema; // Inner validator for dict values

    DictValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_dict(strict);
        if (result.is_err()) {
            return result.error();
        }
        auto& dict = result.value();
        size_t dict_size = dict->size();

        // Length checks
        if (min_length.has_value() && dict_size < min_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TooShort),
                state.location(),
                "dict(len=" + std::to_string(dict_size) + ")"
            );
        }
        if (max_length.has_value() && dict_size > max_length.value()) {
            return ValError::line_error(
                ErrorType(ErrorType::Kind::TooLong),
                state.location(),
                "dict(len=" + std::to_string(dict_size) + ")"
            );
        }

        // Validate keys and values if schemas provided
        if (keys_schema || values_schema) {
            auto entries = dict->entries();
            for (const auto& entry : entries) {
                state.location().push(entry.key);
                // Validate key/value against schemas
                state.location().pop();
            }
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(static_cast<int>(dict_size)));
    }

    std::string name() const override { return "dict"; }
};

// SetValidator - validates set values
class SetValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_list(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "set"; }
};

// FrozenSetValidator - validates frozenset values
class FrozenSetValidator : public Validator {
public:
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_list(state.strict_or(false));
        if (result.is_err()) {
            return result.error();
        }
        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(1));
    }
    
    std::string name() const override { return "frozenset"; }
};

// TupleValidator - validates tuple values with positional items
class TupleValidator : public Validator {
public:
    bool strict = false;
    bool variadic = false;  // If true, last item_schema is repeated for remaining items
    std::vector<std::shared_ptr<Validator>> items; // Positional item validators

    TupleValidator() = default;

    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) override {
        auto result = input.validate_tuple(strict);
        if (result.is_err()) {
            return result.error();
        }
        auto& tuple_match = result.value();
        auto& tuple = tuple_match.value();
        size_t tuple_size = tuple->size();

        // Validate positional items
        if (!items.empty()) {
            auto entries = tuple->entries();
            for (size_t i = 0; i < entries.size(); i++) {
                state.location().push(i);
                // In a full implementation, validate each element against items[i]
                // or items.back() if variadic and i >= items.size()
                state.location().pop();
            }
        }

        return ValResult<std::shared_ptr<void>>(std::make_shared<int>(static_cast<int>(tuple_size)));
    }

    std::string name() const override { return "tuple"; }
};

} // namespace pydantic_core