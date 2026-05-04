#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include "../serializer.hpp"
#include "../result.hpp"
#include "../errors.hpp"

namespace pydantic_core {
namespace serializers {

// ListSerializer - serializes list/array values
class ListSerializer : public Serializer, public BuildSerializer {
public:
    explicit ListSerializer(std::shared_ptr<Serializer> item_serializer)
        : item_serializer_(std::move(item_serializer))
        , name_("list[" + item_serializer_->name() + "]")
    {}

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)value;
        (void)mode;
        (void)state;
        // Stub: iterate list items and serialize each
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)value;
        (void)state;
        // Stub: would serialize each item
        return ValResult<std::string>(std::string("[]"));
    }

    std::string name() const override { return name_; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        // Stub: would build item_serializer from schema
        return std::make_shared<ListSerializer>(
            std::make_shared<AnySerializer>()
        );
    }

    std::string expected_type() const override { return "list"; }

private:
    std::shared_ptr<Serializer> item_serializer_;
    std::string name_;
};

// DictSerializer - serializes dict/object values
class DictSerializer : public Serializer, public BuildSerializer {
public:
    DictSerializer(
        std::shared_ptr<Serializer> key_serializer,
        std::shared_ptr<Serializer> value_serializer
    )
        : key_serializer_(std::move(key_serializer))
        , value_serializer_(std::move(value_serializer))
        , name_("dict[" + key_serializer_->name() + ", " + value_serializer_->name() + "]")
    {}

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)value;
        (void)mode;
        (void)state;
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
        return std::make_shared<DictSerializer>(
            std::make_shared<StringSerializer>(),
            std::make_shared<AnySerializer>()
        );
    }

    std::string expected_type() const override { return "dict"; }

private:
    std::shared_ptr<Serializer> key_serializer_;
    std::shared_ptr<Serializer> value_serializer_;
    std::string name_;
};

// SetSerializer - serializes set values
class SetSerializer : public Serializer, public BuildSerializer {
public:
    explicit SetSerializer(std::shared_ptr<Serializer> item_serializer)
        : item_serializer_(std::move(item_serializer))
        , name_("set[" + item_serializer_->name() + "]")
    {}

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)value;
        (void)mode;
        (void)state;
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)value;
        (void)state;
        return ValResult<std::string>(std::string("[]"));
    }

    std::string name() const override { return name_; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<SetSerializer>(
            std::make_shared<AnySerializer>()
        );
    }

    std::string expected_type() const override { return "set"; }

private:
    std::shared_ptr<Serializer> item_serializer_;
    std::string name_;
};

// FrozenSetSerializer - serializes frozenset values
class FrozenSetSerializer : public Serializer, public BuildSerializer {
public:
    explicit FrozenSetSerializer(std::shared_ptr<Serializer> item_serializer)
        : item_serializer_(std::move(item_serializer))
        , name_("frozenset[" + item_serializer_->name() + "]")
    {}

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)value;
        (void)mode;
        (void)state;
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)value;
        (void)state;
        return ValResult<std::string>(std::string("[]"));
    }

    std::string name() const override { return name_; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<FrozenSetSerializer>(
            std::make_shared<AnySerializer>()
        );
    }

    std::string expected_type() const override { return "frozenset"; }

private:
    std::shared_ptr<Serializer> item_serializer_;
    std::string name_;
};

// TupleSerializer - serializes tuple values
class TupleSerializer : public Serializer, public BuildSerializer {
public:
    explicit TupleSerializer(std::vector<std::shared_ptr<Serializer>> item_serializers)
        : item_serializers_(std::move(item_serializers))
    {
        name_ = "tuple[";
        for (size_t i = 0; i < item_serializers_.size(); ++i) {
            if (i > 0) name_ += ", ";
            name_ += item_serializers_[i]->name();
        }
        name_ += "]";
    }

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)value;
        (void)mode;
        (void)state;
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)value;
        (void)state;
        return ValResult<std::string>(std::string("[]"));
    }

    std::string name() const override { return name_; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<TupleSerializer>(
            std::vector<std::shared_ptr<Serializer>>{}
        );
    }

    std::string expected_type() const override { return "tuple"; }

private:
    std::vector<std::shared_ptr<Serializer>> item_serializers_;
    std::string name_;
};

} // namespace serializers
} // namespace pydantic_core
