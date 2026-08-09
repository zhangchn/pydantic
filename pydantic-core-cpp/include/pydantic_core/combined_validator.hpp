#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include "pydantic_core/validator.hpp"
#include "pydantic_core/result.hpp"
#include "pydantic_core/errors.hpp"
#include "pydantic_core/validators/basic.hpp"
#include "pydantic_core/validators/containers.hpp"
#include "pydantic_core/validators/complex.hpp"
#include "pydantic_core/validators/functions.hpp"
#include "pydantic_core/validators/special.hpp"
#include "pydantic_core/validators/model_fields.hpp"
#include "pydantic_core/validators/special.hpp"
#include <simdjson.h>

namespace pydantic_core {

// CombinedValidator - variant type wrapping all validator types
// Matches Rust's CombinedValidator enum
class CombinedValidator {
public:
    // Variant type holding any validator
    using VariantType = std::variant<
        std::shared_ptr<AnyValidator>,
        std::shared_ptr<NoneValidator>,
        std::shared_ptr<BoolValidator>,
        std::shared_ptr<IntValidator>,
        std::shared_ptr<ConstrainedIntValidator>,
        std::shared_ptr<FloatValidator>,
        std::shared_ptr<ConstrainedFloatValidator>,
        std::shared_ptr<StringValidator>,
        std::shared_ptr<StrConstrainedValidator>,
        std::shared_ptr<BytesValidator>,
        std::shared_ptr<BytesConstrainedValidator>,
        std::shared_ptr<ListValidator>,
        std::shared_ptr<DictValidator>,
        std::shared_ptr<SetValidator>,
        std::shared_ptr<FrozenSetValidator>,
        std::shared_ptr<TupleValidator>,
        std::shared_ptr<NullableValidator>,
        std::shared_ptr<UnionValidator>,
        std::shared_ptr<TaggedUnionValidator>,
        std::shared_ptr<ModelValidator>,
        std::shared_ptr<ModelFieldsValidator>,
        std::shared_ptr<TypedDictValidator>,
        std::shared_ptr<DataclassValidator>,
        std::shared_ptr<LiteralValidator>,
        std::shared_ptr<EnumValidator>,
        std::shared_ptr<DateValidator>,
        std::shared_ptr<TimeValidator>,
        std::shared_ptr<DatetimeValidator>,
        std::shared_ptr<TimedeltaValidator>,
        std::shared_ptr<UrlValidator>,
        std::shared_ptr<MultiHostUrlValidator>,
        std::shared_ptr<UuidValidator>,
        std::shared_ptr<FunctionBeforeValidator>,
        std::shared_ptr<FunctionAfterValidator>,
        std::shared_ptr<FunctionPlainValidator>,
        std::shared_ptr<FunctionWrapValidator>,
        std::shared_ptr<WithDefaultValidator>,
        std::shared_ptr<ChainValidator>,
        std::shared_ptr<LaxOrStrictValidator>,
        std::shared_ptr<JsonOrPythonValidator>,
        std::shared_ptr<JsonValidator>,
        std::shared_ptr<DefinitionRefValidator>,
        std::shared_ptr<Validator>  // Fallback for base-class pointers
    >;

    CombinedValidator() = default;

    // Construct from any validator type
    template<typename T>
    CombinedValidator(std::shared_ptr<T> validator)
        : variant_(std::move(validator))
    {}

    // Validate input
    ValResult<std::shared_ptr<void>> validate(
        const Input& input,
        ValidationState& state
    ) const {
        return std::visit([&](const auto& v) -> ValResult<std::shared_ptr<void>> {
            if (v) {
                return v->validate(input, state);
            }
            return ValError::line_error(
                ErrorType(ErrorType::Kind::CustomError),
                state.location(),
                "Validator not implemented"
            );
        }, variant_);
    }

    // Get validator name
    std::string name() const {
        return std::visit([](const auto& v) -> std::string {
            if (v) {
                return v->name();
            }
            return "unknown";
        }, variant_);
    }

    // Get default value
    ValResult<std::shared_ptr<void>> default_value(ValidationState& state) const {
        return std::visit([&](const auto& v) -> ValResult<std::shared_ptr<void>> {
            if (v) {
                return v->default_value(state);
            }
            return ValError::omit();
        }, variant_);
    }

    // Check if variant holds a value
    bool has_value() const {
        return variant_.index() != 0 || std::holds_alternative<std::shared_ptr<AnyValidator>>(variant_);
    }

    // For root models: return the inner validator's name; otherwise return nullopt
    std::optional<std::string> root_model_inner_name() const {
        return std::visit([](const auto& v) -> std::optional<std::string> {
            if (v) {
                std::string inner = v->root_model_inner_name();
                if (!inner.empty()) {
                    return inner;
                }
            }
            return std::nullopt;
        }, variant_);
    }

    // For validators whose actual result type differs from name() (e.g. a
    // model wrapping a function-after/wrap/plain validator).
    std::string result_dispatch_name() const {
        return std::visit([](const auto& v) -> std::string {
            if (v) {
                return v->result_dispatch_name();
            }
            return "";
        }, variant_);
    }

    // The type name matching the validator's actual result value.
    std::string effective_result_name() const {
        return std::visit([](const auto& v) -> std::string {
            if (v) {
                return v->effective_result_name();
            }
            return "unknown";
        }, variant_);
    }

private:
    VariantType variant_;
};

// Schema parser for building validators from JSON schema
class SchemaBuilder {
public:
    // Build validator from JSON schema string
    static std::shared_ptr<CombinedValidator> build(
        const std::string& schema_json,
        const std::string& config_json = ""
    );

    // Build validator directly from Python dict (like Rust — no JSON round-trip)
    static std::shared_ptr<CombinedValidator> build_from_py(
        const py::dict& schema,
        const py::dict& config
    );

private:
    // Parse schema JSON and build validator
    static std::shared_ptr<CombinedValidator> build_from_dict(
        const std::unordered_map<std::string, std::string>& schema,
        const std::unordered_map<std::string, std::string>& config
    );
};

} // namespace pydantic_core
