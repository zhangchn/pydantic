#include "pydantic_core/serializer.hpp"
#include "pydantic_core/serializers/basic.hpp"
#include "pydantic_core/serializers/containers.hpp"
#include "pydantic_core/serializers/complex.hpp"
#include "pydantic_core/serializers/special.hpp"

namespace pydantic_core {

// BuildSerializer implementations for factory registration

class AnyBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::AnySerializer>();
    }
    std::string expected_type() const override { return "any"; }
};

class NoneBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::NoneSerializer>();
    }
    std::string expected_type() const override { return "none"; }
};

class BoolBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::BoolSerializer>();
    }
    std::string expected_type() const override { return "bool"; }
};

class IntBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::IntSerializer>();
    }
    std::string expected_type() const override { return "int"; }
};

class FloatBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::FloatSerializer>();
    }
    std::string expected_type() const override { return "float"; }
};

class StringBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::StringSerializer>();
    }
    std::string expected_type() const override { return "str"; }
};

class BytesBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::BytesSerializer>();
    }
    std::string expected_type() const override { return "bytes"; }
};

class ListBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::ListSerializer>(
            std::make_shared<serializers::AnySerializer>()
        );
    }
    std::string expected_type() const override { return "list"; }
};

class DictBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::DictSerializer>(
            std::make_shared<serializers::StringSerializer>(),
            std::make_shared<serializers::AnySerializer>()
        );
    }
    std::string expected_type() const override { return "dict"; }
};

class SetBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::SetSerializer>(
            std::make_shared<serializers::AnySerializer>()
        );
    }
    std::string expected_type() const override { return "set"; }
};

class FrozenSetBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::FrozenSetSerializer>(
            std::make_shared<serializers::AnySerializer>()
        );
    }
    std::string expected_type() const override { return "frozenset"; }
};

class TupleBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::TupleSerializer>(
            std::vector<std::shared_ptr<Serializer>>{}
        );
    }
    std::string expected_type() const override { return "tuple"; }
};

class NullableBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::NullableSerializer>(
            std::make_shared<serializers::AnySerializer>()
        );
    }
    std::string expected_type() const override { return "nullable"; }
};

class UnionBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::UnionSerializer>(
            std::vector<std::shared_ptr<Serializer>>{
                std::make_shared<serializers::AnySerializer>()
            }
        );
    }
    std::string expected_type() const override { return "union"; }
};

class TaggedUnionBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::TaggedUnionSerializer>(
            "tag",
            std::unordered_map<std::string, std::shared_ptr<Serializer>>{}
        );
    }
    std::string expected_type() const override { return "tagged-union"; }
};

class WithDefaultBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::WithDefaultSerializer>(
            std::make_shared<serializers::AnySerializer>(),
            nullptr
        );
    }
    std::string expected_type() const override { return "default"; }
};

class DateBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::DateSerializer>();
    }
    std::string expected_type() const override { return "date"; }
};

class TimeBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::TimeSerializer>();
    }
    std::string expected_type() const override { return "time"; }
};

class DatetimeBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::DatetimeSerializer>();
    }
    std::string expected_type() const override { return "datetime"; }
};

class TimedeltaBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::TimedeltaSerializer>();
    }
    std::string expected_type() const override { return "timedelta"; }
};

class UrlBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::UrlSerializer>();
    }
    std::string expected_type() const override { return "url"; }
};

class UuidBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::UuidSerializer>();
    }
    std::string expected_type() const override { return "uuid"; }
};

class LiteralBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::LiteralSerializer>(
            std::vector<std::string>{}
        );
    }
    std::string expected_type() const override { return "literal"; }
};

class EnumBuildSerializer : public BuildSerializer {
public:
    std::shared_ptr<Serializer> build(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    ) override {
        return std::make_shared<serializers::EnumSerializer>(
            std::make_shared<serializers::AnySerializer>(),
            "Enum"
        );
    }
    std::string expected_type() const override { return "enum"; }
};

// Factory implementation

std::unordered_map<std::string, std::unique_ptr<BuildSerializer>>& SerializerFactory::builders() {
    static std::unordered_map<std::string, std::unique_ptr<BuildSerializer>> instance = [] {
        std::unordered_map<std::string, std::unique_ptr<BuildSerializer>> m;
        m["any"] = std::make_unique<AnyBuildSerializer>();
        m["none"] = std::make_unique<NoneBuildSerializer>();
        m["bool"] = std::make_unique<BoolBuildSerializer>();
        m["int"] = std::make_unique<IntBuildSerializer>();
        m["float"] = std::make_unique<FloatBuildSerializer>();
        m["str"] = std::make_unique<StringBuildSerializer>();
        m["bytes"] = std::make_unique<BytesBuildSerializer>();
        m["list"] = std::make_unique<ListBuildSerializer>();
        m["dict"] = std::make_unique<DictBuildSerializer>();
        m["set"] = std::make_unique<SetBuildSerializer>();
        m["frozenset"] = std::make_unique<FrozenSetBuildSerializer>();
        m["tuple"] = std::make_unique<TupleBuildSerializer>();
        m["nullable"] = std::make_unique<NullableBuildSerializer>();
        m["union"] = std::make_unique<UnionBuildSerializer>();
        m["tagged-union"] = std::make_unique<TaggedUnionBuildSerializer>();
        m["default"] = std::make_unique<WithDefaultBuildSerializer>();
        m["date"] = std::make_unique<DateBuildSerializer>();
        m["time"] = std::make_unique<TimeBuildSerializer>();
        m["datetime"] = std::make_unique<DatetimeBuildSerializer>();
        m["timedelta"] = std::make_unique<TimedeltaBuildSerializer>();
        m["url"] = std::make_unique<UrlBuildSerializer>();
        m["uuid"] = std::make_unique<UuidBuildSerializer>();
        m["literal"] = std::make_unique<LiteralBuildSerializer>();
        m["enum"] = std::make_unique<EnumBuildSerializer>();
        return m;
    }();
    return instance;
}

std::shared_ptr<Serializer> SerializerFactory::build(
    const std::unordered_map<std::string, std::string>& schema,
    const std::unordered_map<std::string, std::string>& config
) {
    SerSchemaParser parser(schema);
    std::string type = parser.type();

    auto& b = builders();
    auto it = b.find(type);
    if (it == b.end()) {
        throw std::runtime_error("Unknown serializer type: " + type);
    }

    return it->second->build(schema, config);
}

void SerializerFactory::register_builder(
    const std::string& type,
    std::unique_ptr<BuildSerializer> builder
) {
    builders()[type] = std::move(builder);
}

} // namespace pydantic_core
