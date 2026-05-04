#pragma once

#include <string>
#include <cstdint>

namespace pydantic_core {

// Temporal mode for datetime/time/timedelta serialization
enum class TemporalMode {
    Iso8601,      // ISO 8601 string format
    Seconds,      // Unix timestamp in seconds
    Milliseconds  // Unix timestamp in milliseconds
};

// Bytes mode for bytes serialization
enum class BytesMode {
    Utf8,   // UTF-8 string
    Base64, // Base64 encoded
    Hex     // Hex encoded
};

// Inf/NaN mode for float serialization
enum class InfNanMode {
    Null,       // Serialize as null
    Constants,  // Serialize as "Infinity", "-Infinity", "NaN"
    Strings     // Serialize as strings
};

// Serialization configuration - matches Rust's SerializationConfig
struct SerializationConfig {
    TemporalMode temporal_mode = TemporalMode::Iso8601;
    BytesMode bytes_mode = BytesMode::Utf8;
    InfNanMode inf_nan_mode = InfNanMode::Constants;

    static SerializationConfig default_config() {
        return SerializationConfig{};
    }
};

} // namespace pydantic_core
