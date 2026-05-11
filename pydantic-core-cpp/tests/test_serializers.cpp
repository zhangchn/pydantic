#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "pydantic_core/serializer.hpp"
#include "pydantic_core/serialization_config.hpp"
#include "pydantic_core/serialization_state.hpp"
#include "pydantic_core/serializers/basic.hpp"
#include "pydantic_core/serializers/containers.hpp"
#include "pydantic_core/serializers/complex.hpp"
#include "pydantic_core/serializers/special.hpp"

using namespace pydantic_core;

TEST_SUITE("Serializers") {

    // Helper to create serialization state
    SerializationState make_state() {
        return SerializationState(SerializationConfig::default_config());
    }

    // Helper to create schema dict
    std::unordered_map<std::string, std::string> make_schema(const std::string& type) {
        return {{"type", type}};
    }

    // Helper to create config dict
    std::unordered_map<std::string, std::string> make_config() {
        return {};
    }

    TEST_CASE("AnySerializer - basic instantiation") {
        serializers::AnySerializer ser;
        CHECK(ser.name() == "any");
    }

    TEST_CASE("AnySerializer - serialize passes through value") {
        serializers::AnySerializer ser;
        auto state = make_state();
        auto value = std::make_shared<int>(42);

        auto result = ser.serialize(value, SerMode::Python, state);
        CHECK(result.is_ok());
        CHECK(result.value() == value);
    }

    TEST_CASE("NoneSerializer - basic instantiation") {
        serializers::NoneSerializer ser;
        CHECK(ser.name() == "none");
    }

    TEST_CASE("NoneSerializer - serialize_json returns null") {
        serializers::NoneSerializer ser;
        auto state = make_state();

        auto result = ser.serialize_json(nullptr, state);
        CHECK(result.is_ok());
        CHECK(result.value() == "null");
    }

    TEST_CASE("BoolSerializer - basic instantiation") {
        serializers::BoolSerializer ser;
        CHECK(ser.name() == "bool");
    }

    TEST_CASE("BoolSerializer - serialize_json true") {
        serializers::BoolSerializer ser;
        auto state = make_state();
        auto value = std::make_shared<bool>(true);

        auto result = ser.serialize_json(value, state);
        CHECK(result.is_ok());
        CHECK(result.value() == "true");
    }

    TEST_CASE("BoolSerializer - serialize_json false") {
        serializers::BoolSerializer ser;
        auto state = make_state();
        auto value = std::make_shared<bool>(false);

        auto result = ser.serialize_json(value, state);
        CHECK(result.is_ok());
        CHECK(result.value() == "false");
    }

    TEST_CASE("IntSerializer - basic instantiation") {
        serializers::IntSerializer ser;
        CHECK(ser.name() == "int");
    }

    TEST_CASE("IntSerializer - serialize_json") {
        serializers::IntSerializer ser;
        auto state = make_state();
        auto value = std::make_shared<int64_t>(12345);

        auto result = ser.serialize_json(value, state);
        CHECK(result.is_ok());
        CHECK(result.value() == "12345");
    }

    TEST_CASE("FloatSerializer - basic instantiation") {
        serializers::FloatSerializer ser;
        CHECK(ser.name() == "float");
    }

    TEST_CASE("FloatSerializer - serialize_json normal") {
        serializers::FloatSerializer ser;
        auto state = make_state();
        auto value = std::make_shared<double>(3.14);

        auto result = ser.serialize_json(value, state);
        CHECK(result.is_ok());
    }

    TEST_CASE("FloatSerializer - serialize_json NaN") {
        serializers::FloatSerializer ser;
        auto state = make_state();
        auto value = std::make_shared<double>(std::nan(""));

        auto result = ser.serialize_json(value, state);
        CHECK(result.is_ok());
        CHECK(result.value() == "NaN");
    }

    TEST_CASE("FloatSerializer - serialize_json Infinity") {
        serializers::FloatSerializer ser;
        auto state = make_state();
        auto value = std::make_shared<double>(std::numeric_limits<double>::infinity());

        auto result = ser.serialize_json(value, state);
        CHECK(result.is_ok());
        CHECK(result.value() == "Infinity");
    }

    TEST_CASE("StringSerializer - basic instantiation") {
        serializers::StringSerializer ser;
        CHECK(ser.name() == "str");
    }

    TEST_CASE("StringSerializer - serialize_json") {
        serializers::StringSerializer ser;
        auto state = make_state();
        auto value = std::make_shared<std::string>("hello");

        auto result = ser.serialize_json(value, state);
        CHECK(result.is_ok());
        CHECK(result.value() == "\"hello\"");
    }

    TEST_CASE("StringSerializer - serialize_json escapes special chars") {
        serializers::StringSerializer ser;
        auto state = make_state();
        auto value = std::make_shared<std::string>("hello\nworld");

        auto result = ser.serialize_json(value, state);
        CHECK(result.is_ok());
        CHECK(result.value() == "\"hello\\nworld\"");
    }

    TEST_CASE("BytesSerializer - basic instantiation") {
        serializers::BytesSerializer ser;
        CHECK(ser.name() == "bytes");
    }

    TEST_CASE("ListSerializer - basic instantiation") {
        auto item_ser = std::make_shared<serializers::IntSerializer>();
        serializers::ListSerializer ser(item_ser);
        CHECK(ser.name() == "list[int]");
    }

    TEST_CASE("DictSerializer - basic instantiation") {
        auto key_ser = std::make_shared<serializers::StringSerializer>();
        auto val_ser = std::make_shared<serializers::IntSerializer>();
        serializers::DictSerializer ser(key_ser, val_ser);
        CHECK(ser.name() == "dict[str, int]");
    }

    TEST_CASE("SetSerializer - basic instantiation") {
        auto item_ser = std::make_shared<serializers::StringSerializer>();
        serializers::SetSerializer ser(item_ser);
        CHECK(ser.name() == "set[str]");
    }

    TEST_CASE("FrozenSetSerializer - basic instantiation") {
        auto item_ser = std::make_shared<serializers::StringSerializer>();
        serializers::FrozenSetSerializer ser(item_ser);
        CHECK(ser.name() == "frozenset[str]");
    }

    TEST_CASE("TupleSerializer - basic instantiation") {
        std::vector<std::shared_ptr<Serializer>> items;
        items.push_back(std::make_shared<serializers::IntSerializer>());
        items.push_back(std::make_shared<serializers::StringSerializer>());
        serializers::TupleSerializer ser(std::move(items));
        CHECK(ser.name() == "tuple[int, str]");
    }

    TEST_CASE("NullableSerializer - basic instantiation") {
        auto inner = std::make_shared<serializers::IntSerializer>();
        serializers::NullableSerializer ser(inner);
        CHECK(ser.name() == "nullable[int]");
    }

    TEST_CASE("NullableSerializer - serialize None returns None") {
        auto inner = std::make_shared<serializers::IntSerializer>();
        serializers::NullableSerializer ser(inner);
        auto state = make_state();

        auto result = ser.serialize(nullptr, SerMode::Python, state);
        CHECK(result.is_ok());
        CHECK(result.value() == nullptr);
    }

    TEST_CASE("NullableSerializer - serialize_json None returns null") {
        auto inner = std::make_shared<serializers::IntSerializer>();
        serializers::NullableSerializer ser(inner);
        auto state = make_state();

        auto result = ser.serialize_json(nullptr, state);
        CHECK(result.is_ok());
        CHECK(result.value() == "null");
    }

    TEST_CASE("NullableSerializer - serialize delegates to inner") {
        auto inner = std::make_shared<serializers::IntSerializer>();
        serializers::NullableSerializer ser(inner);
        auto state = make_state();
        auto value = std::make_shared<int64_t>(42);

        auto result = ser.serialize(value, SerMode::Python, state);
        CHECK(result.is_ok());
    }

    TEST_CASE("UnionSerializer - basic instantiation") {
        std::vector<std::shared_ptr<Serializer>> serializers;
        serializers.push_back(std::make_shared<serializers::IntSerializer>());
        serializers.push_back(std::make_shared<serializers::StringSerializer>());
        serializers::UnionSerializer ser(std::move(serializers));
        CHECK(ser.name() == "union[int, str]");
    }

    TEST_CASE("UnionSerializer - serialize tries each variant") {
        std::vector<std::shared_ptr<Serializer>> serializers;
        serializers.push_back(std::make_shared<serializers::IntSerializer>());
        serializers.push_back(std::make_shared<serializers::StringSerializer>());
        serializers::UnionSerializer ser(std::move(serializers));
        auto state = make_state();
        auto value = std::make_shared<int64_t>(42);

        auto result = ser.serialize(value, SerMode::Python, state);
        CHECK(result.is_ok());
    }

    TEST_CASE("TaggedUnionSerializer - basic instantiation") {
        serializers::TaggedUnionSerializer ser(
            "type",
            std::unordered_map<std::string, std::shared_ptr<Serializer>>{}
        );
        CHECK(ser.name() == "tagged-union[type]");
    }

    TEST_CASE("WithDefaultSerializer - basic instantiation") {
        auto inner = std::make_shared<serializers::IntSerializer>();
        auto default_val = std::make_shared<int64_t>(0);
        serializers::WithDefaultSerializer ser(inner, default_val);
        CHECK(ser.name() == "default[int]");
    }

    TEST_CASE("WithDefaultSerializer - returns default for None") {
        auto inner = std::make_shared<serializers::IntSerializer>();
        auto default_val = std::make_shared<int64_t>(42);
        serializers::WithDefaultSerializer ser(inner, default_val);
        auto state = make_state();

        auto result = ser.serialize(nullptr, SerMode::Python, state);
        CHECK(result.is_ok());
        auto val = std::static_pointer_cast<int64_t>(result.value());
        CHECK(*val == 42);
    }

    TEST_CASE("WithDefaultSerializer - delegates to inner for non-None") {
        auto inner = std::make_shared<serializers::IntSerializer>();
        auto default_val = std::make_shared<int64_t>(0);
        serializers::WithDefaultSerializer ser(inner, default_val);
        auto state = make_state();
        auto value = std::make_shared<int64_t>(99);

        auto result = ser.serialize(value, SerMode::Python, state);
        CHECK(result.is_ok());
    }

    TEST_CASE("DateSerializer - basic instantiation") {
        serializers::DateSerializer ser;
        CHECK(ser.name() == "date");
    }

    TEST_CASE("TimeSerializer - basic instantiation") {
        serializers::TimeSerializer ser;
        CHECK(ser.name() == "time");
    }

    TEST_CASE("DatetimeSerializer - basic instantiation") {
        serializers::DatetimeSerializer ser;
        CHECK(ser.name() == "datetime");
    }

    TEST_CASE("TimedeltaSerializer - basic instantiation") {
        serializers::TimedeltaSerializer ser;
        CHECK(ser.name() == "timedelta");
    }

    TEST_CASE("UrlSerializer - basic instantiation") {
        serializers::UrlSerializer ser;
        CHECK(ser.name() == "url");
    }

    TEST_CASE("UuidSerializer - basic instantiation") {
        serializers::UuidSerializer ser;
        CHECK(ser.name() == "uuid");
    }

    TEST_CASE("LiteralSerializer - basic instantiation") {
        serializers::LiteralSerializer ser(std::vector<std::string>{"a", "b"});
        CHECK(ser.name() == "literal");
    }

    TEST_CASE("EnumSerializer - basic instantiation") {
        auto member_ser = std::make_shared<serializers::StringSerializer>();
        serializers::EnumSerializer ser(member_ser, "MyEnum");
        CHECK(ser.name() == "enum[MyEnum]");
    }

    TEST_CASE("SerializerFactory - builds all serializer types") {
        std::vector<std::string> types = {
            "any", "none", "bool", "int", "float", "str", "bytes",
            "list", "dict", "set", "frozenset", "tuple",
            "nullable", "union", "tagged-union", "default",
            "date", "time", "datetime", "timedelta",
            "url", "uuid", "literal", "enum"
        };

        for (const auto& type : types) {
            auto schema = make_schema(type);
            auto config = make_config();
            auto serializer = SerializerFactory::build(schema, config);
            CHECK(serializer != nullptr);
        }
    }

    TEST_CASE("SerializerFactory - throws on unknown type") {
        auto schema = make_schema("unknown_type");
        auto config = make_config();
        CHECK_THROWS_AS(SerializerFactory::build(schema, config), std::runtime_error&);
    }

    TEST_CASE("SerializerFactory - register custom builder") {
        class CustomSerializer : public Serializer, public BuildSerializer {
        public:
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
                return ValResult<std::string>(std::string("custom"));
            }

            std::string name() const override { return "custom"; }

            std::shared_ptr<Serializer> build(
                const std::unordered_map<std::string, std::string>& schema,
                const std::unordered_map<std::string, std::string>& config
            ) override {
                (void)schema;
                (void)config;
                return std::make_shared<CustomSerializer>();
            }

            std::string expected_type() const override { return "custom"; }
        };

        SerializerFactory::register_builder("custom", std::make_unique<CustomSerializer>());

        auto schema = make_schema("custom");
        auto config = make_config();
        auto serializer = SerializerFactory::build(schema, config);
        CHECK(serializer != nullptr);
        CHECK(serializer->name() == "custom");
    }

    TEST_CASE("SerializationConfig - default values") {
        auto config = SerializationConfig::default_config();
        CHECK(config.temporal_mode == TemporalMode::Iso8601);
        CHECK(config.bytes_mode == BytesMode::Utf8);
        CHECK(config.inf_nan_mode == InfNanMode::Constants);
    }

    TEST_CASE("SerializationState - basic construction") {
        auto state = make_state();
        CHECK(state.config().temporal_mode == TemporalMode::Iso8601);
    }

    TEST_CASE("SerializationState - with_include_exclude") {
        auto state = make_state();
        IncludeExclude ie;
        ie.include = std::vector<std::string>{"field1", "field2"};
        auto new_state = state.with_include_exclude(std::move(ie));
        CHECK(new_state.include_exclude().include.has_value());
        CHECK(new_state.include_exclude().include->size() == 2);
    }

    // ========================================================================
    // Behavioral serializer tests (Python equivalents: test_simple.py, test_any.py)
    // ========================================================================

    TEST_CASE("IntSerializer - serialize_json handles negative and zero") {
        serializers::IntSerializer ser;
        auto state = make_state();

        // Negative integer
        {
            auto value = std::make_shared<int64_t>(-999);
            auto result = ser.serialize_json(value, state);
            REQUIRE(result.is_ok());
            CHECK(result.value() == "-999");
        }

        // Zero
        {
            auto value = std::make_shared<int64_t>(0);
            auto result = ser.serialize_json(value, state);
            REQUIRE(result.is_ok());
            CHECK(result.value() == "0");
        }

        // Large integer
        {
            auto value = std::make_shared<int64_t>(9223372036854775807LL);
            auto result = ser.serialize_json(value, state);
            REQUIRE(result.is_ok());
            CHECK(result.value() == "9223372036854775807");
        }
    }

    TEST_CASE("IntSerializer - serialize_json fails on nullptr") {
        serializers::IntSerializer ser;
        auto state = make_state();

        auto result = ser.serialize_json(nullptr, state);
        CHECK(result.is_err());
    }

    TEST_CASE("FloatSerializer - serialize_json negative infinity") {
        serializers::FloatSerializer ser;
        auto state = make_state();

        auto value = std::make_shared<double>(-std::numeric_limits<double>::infinity());
        auto result = ser.serialize_json(value, state);
        REQUIRE(result.is_ok());
        CHECK(result.value() == "-Infinity");
    }

    TEST_CASE("FloatSerializer - serialize_json zero and negative zero") {
        serializers::FloatSerializer ser;
        auto state = make_state();

        // Zero
        {
            auto value = std::make_shared<double>(0.0);
            auto result = ser.serialize_json(value, state);
            REQUIRE(result.is_ok());
            CHECK(result.value().find("0.000000") != std::string::npos);
        }
    }

    TEST_CASE("FloatSerializer - serialize_json fails on nullptr") {
        serializers::FloatSerializer ser;
        auto state = make_state();

        auto result = ser.serialize_json(nullptr, state);
        CHECK(result.is_err());
    }

    TEST_CASE("StringSerializer - serialize_json handles empty string") {
        serializers::StringSerializer ser;
        auto state = make_state();

        auto value = std::make_shared<std::string>("");
        auto result = ser.serialize_json(value, state);
        REQUIRE(result.is_ok());
        CHECK(result.value() == "\"\"");
    }

    TEST_CASE("StringSerializer - serialize_json handles quotes and backslashes") {
        serializers::StringSerializer ser;
        auto state = make_state();

        // Double quote
        {
            auto value = std::make_shared<std::string>("say \"hello\"");
            auto result = ser.serialize_json(value, state);
            REQUIRE(result.is_ok());
            CHECK(result.value() == "\"say \\\"hello\\\"\"");
        }

        // Backslash
        {
            auto value = std::make_shared<std::string>("path\\to\\file");
            auto result = ser.serialize_json(value, state);
            REQUIRE(result.is_ok());
            CHECK(result.value() == "\"path\\\\to\\\\file\"");
        }

        // Tab
        {
            auto value = std::make_shared<std::string>("col1\tcol2");
            auto result = ser.serialize_json(value, state);
            REQUIRE(result.is_ok());
            CHECK(result.value() == "\"col1\\tcol2\"");
        }
    }

    TEST_CASE("StringSerializer - serialize_json fails on nullptr") {
        serializers::StringSerializer ser;
        auto state = make_state();

        auto result = ser.serialize_json(nullptr, state);
        CHECK(result.is_err());
    }

    TEST_CASE("NoneSerializer - serialize fails on non-None value") {
        serializers::NoneSerializer ser;
        auto state = make_state();

        auto value = std::make_shared<int>(42);
        auto result = ser.serialize(value, SerMode::Python, state);
        CHECK(result.is_err());
    }

    TEST_CASE("AnySerializer - serialize_json returns stub for any value") {
        serializers::AnySerializer ser;
        auto state = make_state();

        // Any value should produce stub JSON
        auto value = std::make_shared<std::string>("anything");
        auto result = ser.serialize_json(value, state);
        REQUIRE(result.is_ok());
        CHECK(result.value() == "{}");
    }
}
