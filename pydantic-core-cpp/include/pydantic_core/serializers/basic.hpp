#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include "../serializer.hpp"
#include "../result.hpp"
#include "../errors.hpp"
#include "../types.hpp"

namespace pydantic_core {
namespace serializers {

// AnySerializer - accepts and passes through any value
class AnySerializer : public Serializer, public BuildSerializer {
public:
    AnySerializer() = default;

    // Serializer interface
    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
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
        // Stub: would use simdjson to serialize
        return ValResult<std::string>(std::string("{}"));
    }

    std::string name() const override { return "any"; }

    // BuildSerializer interface
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<AnySerializer>();
    }

    std::string expected_type() const override { return "any"; }
};

// NoneSerializer - serializes None/null values
class NoneSerializer : public Serializer, public BuildSerializer {
public:
    NoneSerializer() = default;

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)mode;
        (void)state;
        if (!value) {
            return ValResult<std::shared_ptr<void>>(nullptr);
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            Location{},
            "Expected None, got value"
        );
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)value;
        (void)state;
        return ValResult<std::string>(std::string("null"));
    }

    std::string name() const override { return "none"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<NoneSerializer>();
    }

    std::string expected_type() const override { return "none"; }
};

// BoolSerializer - serializes boolean values
class BoolSerializer : public Serializer, public BuildSerializer {
public:
    BoolSerializer() = default;

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)mode;
        (void)state;
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)state;
        if (value && *std::static_pointer_cast<bool>(value)) {
            return ValResult<std::string>(std::string("true"));
        }
        return ValResult<std::string>(std::string("false"));
    }

    std::string name() const override { return "bool"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<BoolSerializer>();
    }

    std::string expected_type() const override { return "bool"; }
};

// IntSerializer - serializes integer values
class IntSerializer : public Serializer, public BuildSerializer {
public:
    IntSerializer() = default;

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)mode;
        (void)state;
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)state;
        if (value) {
            return ValResult<std::string>(std::to_string(*std::static_pointer_cast<int64_t>(value)));
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            Location{},
            "Expected int, got None"
        );
    }

    std::string name() const override { return "int"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<IntSerializer>();
    }

    std::string expected_type() const override { return "int"; }
};

// FloatSerializer - serializes float values
class FloatSerializer : public Serializer, public BuildSerializer {
public:
    FloatSerializer() = default;

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)mode;
        (void)state;
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)state;
        if (value) {
            double d = *std::static_pointer_cast<double>(value);
            if (std::isnan(d)) {
                return ValResult<std::string>(std::string("NaN"));
            }
            if (std::isinf(d)) {
                return ValResult<std::string>(d > 0 ? std::string("Infinity") : std::string("-Infinity"));
            }
            return ValResult<std::string>(std::to_string(d));
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            Location{},
            "Expected float, got None"
        );
    }

    std::string name() const override { return "float"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<FloatSerializer>();
    }

    std::string expected_type() const override { return "float"; }
};

// StringSerializer - serializes string values
class StringSerializer : public Serializer, public BuildSerializer {
public:
    StringSerializer() = default;

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)mode;
        (void)state;
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)state;
        if (value) {
            std::string s = *std::static_pointer_cast<std::string>(value);
            // Escape special characters for JSON
            std::string escaped = "\"";
            for (char c : s) {
                switch (c) {
                    case '"': escaped += "\\\""; break;
                    case '\\': escaped += "\\\\"; break;
                    case '\n': escaped += "\\n"; break;
                    case '\r': escaped += "\\r"; break;
                    case '\t': escaped += "\\t"; break;
                    default: escaped += c;
                }
            }
            escaped += "\"";
            return ValResult<std::string>(escaped);
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            Location{},
            "Expected string, got None"
        );
    }

    std::string name() const override { return "str"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<StringSerializer>();
    }

    std::string expected_type() const override { return "str"; }
};

// BytesSerializer - serializes bytes values
class BytesSerializer : public Serializer, public BuildSerializer {
public:
    BytesSerializer() = default;

    ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const override {
        (void)mode;
        (void)state;
        return ValResult<std::shared_ptr<void>>(value);
    }

    ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const override {
        (void)state;
        if (value) {
            // Stub: would encode bytes as base64 or hex
            return ValResult<std::string>(std::string("\"<bytes>\""));
        }
        return ValError::line_error(
            ErrorType(ErrorType::Kind::CustomError),
            Location{},
            "Expected bytes, got None"
        );
    }

    std::string name() const override { return "bytes"; }

    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        (void)schema;
        (void)config;
        return std::make_shared<BytesSerializer>();
    }

    std::string expected_type() const override { return "bytes"; }
};

} // namespace serializers
} // namespace pydantic_core
