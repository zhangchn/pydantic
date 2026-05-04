#pragma once

#include <string>
#include <memory>
#include <variant>
#include <unordered_map>
#include "types.hpp"
#include "serialization_config.hpp"
#include "serialization_state.hpp"
#include "result.hpp"

namespace pydantic_core {

// Forward declarations for all serializer types
class AnySerializer;
class NoneSerializer;
class BoolSerializer;
class IntSerializer;
class FloatSerializer;
class StringSerializer;
class BytesSerializer;
class ListSerializer;
class DictSerializer;
class SetSerializer;
class FrozenSetSerializer;
class TupleSerializer;
class NullableSerializer;
class UnionSerializer;
class TaggedUnionSerializer;
class WithDefaultSerializer;
class DateSerializer;
class TimeSerializer;
class DatetimeSerializer;
class TimedeltaSerializer;
class UrlSerializer;
class UuidSerializer;
class LiteralSerializer;
class EnumSerializer;

// Serializer base trait - mirrors Rust's TypeSerializer trait
// All serializers implement this trait
class Serializer {
public:
    virtual ~Serializer() = default;

    // Serialize a value to a generic output
    // Returns a shared_ptr to the serialized value (type-erased)
    virtual ValResult<std::shared_ptr<void>> serialize(
        const std::shared_ptr<void>& value,
        SerMode mode,
        SerializationState& state
    ) const = 0;

    // Serialize a value to a JSON string
    virtual ValResult<std::string> serialize_json(
        const std::shared_ptr<void>& value,
        SerializationState& state
    ) const = 0;

    // Get serializer name for error messages
    virtual std::string name() const = 0;

    // Get default value (for WithDefaultSerializer)
    virtual std::optional<std::shared_ptr<void>> default_value() const {
        return std::nullopt;
    }

    // Whether to retry with lax check (for union serializers)
    virtual bool retry_with_lax_check() const {
        return false;
    }
};

// BuildSerializer trait - constructs serializers from schema dict
// Matches Rust's BuildSerializer trait
class BuildSerializer {
public:
    virtual ~BuildSerializer() = default;

    // Build serializer from schema
    virtual std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) = 0;

    // Get expected type name (e.g., "model", "int", "str")
    virtual std::string expected_type() const = 0;
};

// Schema parser for serialization schemas
class SerSchemaParser {
public:
    SerSchemaParser(const std::unordered_map<std::string, std::string>& schema)
        : schema_(schema) {}

    std::string type() const {
        auto it = schema_.find("type");
        return it != schema_.end() ? it->second : "";
    }

    std::string get(const std::string& key, const std::string& default_val = "") const {
        auto it = schema_.find(key);
        return it != schema_.end() ? it->second : default_val;
    }

    std::optional<bool> get_bool(const std::string& key) const {
        auto it = schema_.find(key);
        if (it == schema_.end()) return std::nullopt;
        return it->second == "true" || it->second == "1";
    }

    std::optional<int64_t> get_int(const std::string& key) const {
        auto it = schema_.find(key);
        if (it == schema_.end()) return std::nullopt;
        try {
            return std::stoll(it->second);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<double> get_float(const std::string& key) const {
        auto it = schema_.find(key);
        if (it == schema_.end()) return std::nullopt;
        try {
            return std::stod(it->second);
        } catch (...) {
            return std::nullopt;
        }
    }

private:
    const std::unordered_map<std::string, std::string>& schema_;
};

// Serializer factory - builds serializers from schema type
class SerializerFactory {
public:
    // Build serializer from schema dict
    static std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    );

    // Register custom serializer builder
    static void register_builder(const std::string& type, std::unique_ptr<BuildSerializer> builder);

private:
    static std::unordered_map<std::string, std::unique_ptr<BuildSerializer>>& builders();
};

} // namespace pydantic_core
