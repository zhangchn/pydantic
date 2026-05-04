#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "../serializer.hpp"
#include "../result.hpp"
#include "../errors.hpp"
#include "../types.hpp"
#include "basic.hpp"

namespace pydantic_core {
namespace serializers {

// NullableSerializer - wraps another serializer and allows None values
class NullableSerializer : public Serializer, public BuildSerializer {
public:
    explicit NullableSerializer(std::shared_ptr<Serializer> inner_serializer)
        : inner_serializer_(std::move(inner_serializer))
        , name_("nullable[" + inner_serializer_->name() + "]")
    {}

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        if (!value) {
            return ValResult<std::shared_ptr<void>>(nullptr);
        }
        return inner_serializer_->serialize(value, mode, state);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        if (!value) {
            return ValResult<std::string>(std::string("null"));
        }
        return inner_serializer_->serialize_json(value, state);
    }

    std::string name() const override { return name_; }

    bool retry_with_lax_check() const override {
        return inner_serializer_->retry_with_lax_check();
    }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<NullableSerializer>(
            std::make_shared<AnySerializer>()
        );
    }

    std::string expected_type() const override { return "nullable"; }

private:
    std::shared_ptr<Serializer> inner_serializer_;
    std::string name_;
};

// UnionSerializer - tries multiple serializers in order, returns first success
class UnionSerializer : public Serializer, public BuildSerializer {
public:
    explicit UnionSerializer(std::vector<std::shared_ptr<Serializer>> serializers)
        : serializers_(std::move(serializers))
    {
        name_ = "union[";
        for (size_t i = 0; i < serializers_.size(); ++i) {
            if (i > 0) name_ += ", ";
            name_ += serializers_[i]->name();
        }
        name_ += "]";
    }

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        // Try each serializer in order, return first success
        for (const auto& serializer : serializers_) {
            auto result = serializer->serialize(value, mode, state);
            if (result.is_ok()) {
                return result;
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            Location{},
            "No union variant matched"
        );
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        for (const auto& serializer : serializers_) {
            auto result = serializer->serialize_json(value, state);
            if (result.is_ok()) {
                return result;
            }
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            Location{},
            "No union variant matched for JSON serialization"
        );
    }

    std::string name() const override { return name_; }

    bool retry_with_lax_check() const override {
        for (const auto& serializer : serializers_) {
            if (serializer->retry_with_lax_check()) {
                return true;
            }
        }
        return false;
    }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<UnionSerializer>(
            std::vector<std::shared_ptr<Serializer>>{
                std::make_shared<AnySerializer>()
            }
        );
    }

    std::string expected_type() const override { return "union"; }

private:
    std::vector<std::shared_ptr<Serializer>> serializers_;
    std::string name_;
};

// TaggedUnionSerializer - union with discriminator tag
class TaggedUnionSerializer : public Serializer, public BuildSerializer {
public:
    TaggedUnionSerializer(
        std::string tag,
        std::unordered_map<std::string, std::shared_ptr<Serializer>> cases
    )
        : tag_(std::move(tag))
        , cases_(std::move(cases))
        , name_("tagged-union[" + tag_ + "]")
    {}

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)value;
        (void)mode;
        (void)state;
        // Stub: would look up tag and use appropriate serializer
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)value;
        (void)state;
        return ValResult<std::string>(std::string("{}"));
    }

    std::string name() const override { return name_; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<TaggedUnionSerializer>(
            "tag",
            std::unordered_map<std::string, std::shared_ptr<Serializer>>{}
        );
    }

    std::string expected_type() const override { return "tagged-union"; }

private:
    std::string tag_;
    std::unordered_map<std::string, std::shared_ptr<Serializer>> cases_;
    std::string name_;
};

// WithDefaultSerializer - provides default value if input is missing
class WithDefaultSerializer : public Serializer, public BuildSerializer {
public:
    WithDefaultSerializer(
        std::shared_ptr<Serializer> inner_serializer,
        std::shared_ptr<void> default_value
    )
        : inner_serializer_(std::move(inner_serializer))
        , default_value_(std::move(default_value))
        , name_("default[" + inner_serializer_->name() + "]")
    {}

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        if (!value && default_value_) {
            return ValResult<std::shared_ptr<void>>(default_value_);
        }
        return inner_serializer_->serialize(value, mode, state);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        if (!value && default_value_) {
            return inner_serializer_->serialize_json(default_value_, state);
        }
        return inner_serializer_->serialize_json(value, state);
    }

    std::string name() const override { return name_; }

    bool retry_with_lax_check() const override {
        return inner_serializer_->retry_with_lax_check();
    }

    std::optional<std::shared_ptr<void>> default_value() const override {
        return default_value_;
    }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<WithDefaultSerializer>(
            std::make_shared<AnySerializer>(),
            nullptr
        );
    }

    std::string expected_type() const override { return "default"; }

private:
    std::shared_ptr<Serializer> inner_serializer_;
    std::shared_ptr<void> default_value_;
    std::string name_;
};

} // namespace serializers
} // namespace pydantic_core
