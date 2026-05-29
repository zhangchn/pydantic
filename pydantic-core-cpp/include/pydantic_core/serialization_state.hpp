#pragma once

#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include "serialization_config.hpp"
#include "recursion_guard.hpp"

namespace pydantic_core {

// Include/exclude filter for serialization
struct IncludeExclude {
    std::optional<std::vector<std::string>> include;
    std::optional<std::vector<std::string>> exclude;

    static IncludeExclude empty() {
        return IncludeExclude{};
    }
};

// Serialization state - passed through serialization tree
// Matches Rust's SerializationState
class SerializationState {
public:
    SerializationState(
        const SerializationConfig& config,
        IncludeExclude include_exclude = IncludeExclude::empty(),
        bool round_trip = false
    )
        : config_(config)
        , include_exclude_(std::move(include_exclude))
        , round_trip_(round_trip)
        , recursion_state_(std::make_shared<RecursionState>())
    {}

    const SerializationConfig& config() const { return config_; }
    const IncludeExclude& include_exclude() const { return include_exclude_; }
    bool round_trip() const { return round_trip_; }

    // Recursion state for circular reference detection
    RecursionState& recursion_state() { return *recursion_state_; }
    const RecursionState& recursion_state() const { return *recursion_state_; }

    // Scoped include/exclude for nested structures
    SerializationState with_include_exclude(IncludeExclude next) const {
        SerializationState result(config_, std::move(next), round_trip_);
        result.recursion_state_ = recursion_state_;
        return result;
    }

private:
    SerializationConfig config_;
    IncludeExclude include_exclude_;
    bool round_trip_;
    std::shared_ptr<RecursionState> recursion_state_;
};

} // namespace pydantic_core
