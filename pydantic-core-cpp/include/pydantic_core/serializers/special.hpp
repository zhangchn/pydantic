#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include "../serializer.hpp"
#include "../result.hpp"
#include "../errors.hpp"
#include "../serialization_config.hpp"

namespace pydantic_core {
namespace serializers {

// DateSerializer - serializes date values
class DateSerializer : public Serializer, public BuildSerializer {
public:
    DateSerializer() = default;

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
        // Stub: would format date as ISO 8601
        return ValResult<std::string>(std::string("\"2024-01-01\""));
    }

    std::string name() const override { return "date"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<DateSerializer>();
    }

    std::string expected_type() const override { return "date"; }
};

// TimeSerializer - serializes time values
class TimeSerializer : public Serializer, public BuildSerializer {
public:
    TimeSerializer() = default;

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
        return ValResult<std::string>(std::string("\"12:00:00\""));
    }

    std::string name() const override { return "time"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<TimeSerializer>();
    }

    std::string expected_type() const override { return "time"; }
};

// DatetimeSerializer - serializes datetime values
class DatetimeSerializer : public Serializer, public BuildSerializer {
public:
    DatetimeSerializer() = default;

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
        return ValResult<std::string>(std::string("\"2024-01-01T12:00:00\""));
    }

    std::string name() const override { return "datetime"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<DatetimeSerializer>();
    }

    std::string expected_type() const override { return "datetime"; }
};

// TimedeltaSerializer - serializes timedelta values
class TimedeltaSerializer : public Serializer, public BuildSerializer {
public:
    TimedeltaSerializer() = default;

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
        return ValResult<std::string>(std::string("\"P1D\""));
    }

    std::string name() const override { return "timedelta"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<TimedeltaSerializer>();
    }

    std::string expected_type() const override { return "timedelta"; }
};

// UrlSerializer - serializes URL values
class UrlSerializer : public Serializer, public BuildSerializer {
public:
    UrlSerializer() = default;

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
        return ValResult<std::string>(std::string("\"http://example.com\""));
    }

    std::string name() const override { return "url"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<UrlSerializer>();
    }

    std::string expected_type() const override { return "url"; }
};

// UuidSerializer - serializes UUID values
class UuidSerializer : public Serializer, public BuildSerializer {
public:
    UuidSerializer() = default;

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
        return ValResult<std::string>(std::string("\"00000000-0000-0000-0000-000000000000\""));
    }

    std::string name() const override { return "uuid"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<UuidSerializer>();
    }

    std::string expected_type() const override { return "uuid"; }
};

// LiteralSerializer - serializes literal values (exact match)
class LiteralSerializer : public Serializer, public BuildSerializer {
public:
    explicit LiteralSerializer(std::vector<std::string> allowed_values)
        : allowed_values_(std::move(allowed_values))
        , name_("literal")
    {}

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)value;
        (void)mode;
        (void)state;
        // Stub: would check if value matches allowed values
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)value;
        (void)state;
        return ValResult<std::string>(std::string("\"value\""));
    }

    std::string name() const override { return name_; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<LiteralSerializer>(
            std::vector<std::string>{}
        );
    }

    std::string expected_type() const override { return "literal"; }

private:
    std::vector<std::string> allowed_values_;
    std::string name_;
};

// EnumSerializer - serializes enum values
class EnumSerializer : public Serializer, public BuildSerializer {
public:
    EnumSerializer(
        std::shared_ptr<Serializer> member_serializer,
        std::string enum_class_name
    )
        : member_serializer_(std::move(member_serializer))
        , enum_class_name_(std::move(enum_class_name))
        , name_("enum[" + enum_class_name_ + "]")
    {}

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)value;
        (void)mode;
        (void)state;
        // Stub: would extract enum value and serialize
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)value;
        (void)state;
        return ValResult<std::string>(std::string("\"member\""));
    }

    std::string name() const override { return name_; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<EnumSerializer>(
            std::make_shared<AnySerializer>(),
            "Enum"
        );
    }

    std::string expected_type() const override { return "enum"; }

private:
    std::shared_ptr<Serializer> member_serializer_;
    std::string enum_class_name_;
    std::string name_;
};

} // namespace serializers
} // namespace pydantic_core
