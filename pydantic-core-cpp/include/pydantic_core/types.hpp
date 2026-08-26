#pragma once

#include <string>
#include <variant>
#include <optional>
#include <cstdint>
#include <vector>
#include <limits>

namespace pydantic_core {

// Input type enumeration - determines how input is interpreted
enum class InputType {
    Python,  // Python objects via pybind11
    Json,    // JSON parsed by simdjson
    String   // String key-value mapping
};

// Extra behavior for handling extra fields in models/dicts
enum class ExtraBehavior {
    Allow,   // Allow extra fields
    Forbid,  // Raise error on extra fields
    Ignore   // Ignore extra fields (don't include in output)
};

// String cache mode for performance optimization
enum class StringCacheMode {
    All,     // Cache all strings
    Keys,    // Cache only dict keys
    None     // No caching
};

// Partial mode for incomplete data
enum class PartialMode {
    Off,             // No partial validation
    On,              // Allow partial validation
    TrailingStrings  // Include trailing unfinished strings
};

// Serialization mode
enum class SerMode {
    Python,  // Serialize to Python objects
    Json     // Serialize to JSON-compatible types
};

// Warnings mode for serialization
enum class WarningsMode {
    None,    // No warnings
    Warn,    // Log warnings
    Error    // Raise on warnings
};

// Exactness indicator for validation
enum class Exactness {
    Exact,    // Exact type match
    Lax,      // Coercion allowed
    Unknown   // Not yet determined
};

// Location item for error tracking
using LocItem = std::variant<int64_t, std::string>;

// Error location path
struct Location {
    std::vector<LocItem> items;
    
    void push(int64_t index) { items.push_back(index); }
    void push(const std::string& key) { items.push_back(key); }
    void pop() { if (!items.empty()) items.pop_back(); }
    void prepend(const std::string& key) { items.insert(items.begin(), key); }
    
    std::string to_string() const {
        std::string result;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) result += ".";
            if (auto* idx = std::get_if<int64_t>(&items[i])) {
                result += std::to_string(*idx);
            } else {
                result += std::get<std::string>(items[i]);
            }
        }
        return result;
    }
};

} // namespace pydantic_core