#pragma once

// ValBytesMode (Rust validators/config.rs) reads a str input as bytes through the
// config's `val_json_bytes`. Its failure text is the Display text of the base64
// 0.22 / hex 0.4 crate errors, which pydantic-core copies into the
// `bytes_invalid_encoding` ctx, so the decoders below reproduce those messages --
// positions included -- instead of merely accepting or rejecting the input.

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "errors.hpp"

namespace pydantic_core {

struct BytesModeError {
    std::string encoding;         // ctx `encoding`: "base64" or "hex"
    std::string text;             // ctx `encoding_error`: the crate's Display text
    unsigned char bad_byte = 0;   // symbol the failure names, for the base64 retry
};

struct BytesModeResult {
    std::vector<uint8_t> bytes;
    BytesModeError error;
    bool ok() const { return error.text.empty(); }
};

namespace bytes_mode_detail {

// Rust's `char::escape_debug`, which hex's FromHexError Display applies to the
// offending byte. Control characters become \t \n \r \0 or \u{...} with the
// shortest lowercase hex; a double quote stays as it is.
inline std::string escape_debug_char(unsigned int cp) {
    switch (cp) {
        case 0x00: return "\\0";
        case '\t': return "\\t";
        case '\n': return "\\n";
        case '\r': return "\\r";
        case '\\': return "\\\\";
        case '\'': return "\\'";
        default: break;
    }
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "\\u{%x}", cp);
        return std::string(buf);
    }
    if (cp < 0x80) return std::string(1, static_cast<char>(cp));
    std::string out;
    if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

inline int hex_nibble(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// hex::decode: the odd length is checked over the whole input before any
// character is looked at, so "abc" reports the length and not the 'c'.
inline BytesModeResult hex_decode(const std::string& s) {
    BytesModeResult res;
    res.error.encoding = "hex";
    if (s.size() % 2 != 0) {
        res.error.text = "Odd number of digits";
        return res;
    }
    for (size_t i = 0; i < s.size(); i += 2) {
        const unsigned char hi_c = static_cast<unsigned char>(s[i]);
        const unsigned char lo_c = static_cast<unsigned char>(s[i + 1]);
        const int hi = hex_nibble(hi_c);
        if (hi < 0) {
            res.error.text = "Invalid character '" + escape_debug_char(hi_c) +
                             "' at position " + std::to_string(i);
            res.error.bad_byte = hi_c;
            return res;
        }
        const int lo = hex_nibble(lo_c);
        if (lo < 0) {
            res.error.text = "Invalid character '" + escape_debug_char(lo_c) +
                             "' at position " + std::to_string(i + 1);
            res.error.bad_byte = lo_c;
            return res;
        }
        res.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return res;
}

inline std::array<signed char, 256> make_b64_table(const char* alphabet) {
    std::array<signed char, 256> table;
    table.fill(-1);
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(alphabet[i])] = static_cast<signed char>(i);
    return table;
}

// base64::alphabet::STANDARD and ::URL_SAFE; '=' is deliberately not a symbol,
// it is padding and only legal at the very end of the input.
inline const std::array<signed char, 256>& b64_table(bool url_safe) {
    static const std::array<signed char, 256> standard = make_b64_table(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
    static const std::array<signed char, 256> url_safe_table = make_b64_table(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
    return url_safe ? url_safe_table : standard;
}

// GeneralPurpose::decode with DecodePaddingMode::Indifferent: padding is
// optional, but a '=' anywhere that is not the last one or two symbols of the
// input is an ordinary invalid symbol, and the bits of the final data symbol
// that do not fit into a whole byte have to be zero.
inline BytesModeResult base64_decode(const std::string& s, bool url_safe) {
    BytesModeResult res;
    res.error.encoding = "base64";
    const std::array<signed char, 256>& lut = b64_table(url_safe);
    const size_t n = s.size();
    if (n == 0) return res;

    auto symbol_error = [&](size_t off) {
        res.bytes.clear();
        res.error.text = "Invalid symbol " + std::to_string(static_cast<unsigned char>(s[off])) +
                         ", offset " + std::to_string(off) + ".";
        res.error.bad_byte = static_cast<unsigned char>(s[off]);
    };
    auto last_symbol_error = [&](size_t off) {
        res.bytes.clear();
        res.error.text = "Invalid last symbol " + std::to_string(static_cast<unsigned char>(s[off])) +
                         ", offset " + std::to_string(off) + ".";
    };

    // A single leftover byte that is not a symbol at all is blamed before the
    // chunks are scanned -- "AAA=!" fails on the '!' rather than on the '=' that
    // sits earlier. A trailing '=' is left to the scan, which is why "QQ=A="
    // still reports the '=' inside the first chunk.
    if (n % 4 == 1 && s[n - 1] != '=' && lut[static_cast<unsigned char>(s[n - 1])] < 0) {
        symbol_error(n - 1);
        return res;
    }

    size_t i = 0;
    while (i < n) {
        const size_t len = (n - i < 4) ? (n - i) : 4;
        const bool last = (i + len == n);
        size_t data = len;
        bool bad_padding = false;
        size_t bad_padding_at = 0;
        if (last && len >= 3 && s[i + 2] == '=') {
            data = 2;
            if (len == 4 && s[i + 3] != '=') {
                bad_padding = true;
                bad_padding_at = i + 2;
            }
        } else if (last && len == 4 && s[i + 3] == '=') {
            data = 3;
        }

        if (len == 1) {
            // Only reachable as the leftover byte of a `n % 4 == 1` input: an
            // invalid symbol is still reported here, otherwise the length is.
            if (s[i] == '=' || lut[static_cast<unsigned char>(s[i])] < 0) {
                symbol_error(i);
                return res;
            }
            break;
        }

        for (size_t k = 0; k < data; ++k) {
            if (lut[static_cast<unsigned char>(s[i + k])] < 0) {
                symbol_error(i + k);
                return res;
            }
        }
        if (bad_padding) {
            symbol_error(bad_padding_at);
            return res;
        }

        const int v0 = lut[static_cast<unsigned char>(s[i])];
        const int v1 = lut[static_cast<unsigned char>(s[i + 1])];
        res.bytes.push_back(static_cast<uint8_t>((v0 << 2) | (v1 >> 4)));
        if (data >= 3) {
            const int v2 = lut[static_cast<unsigned char>(s[i + 2])];
            res.bytes.push_back(static_cast<uint8_t>(((v1 & 0x0F) << 4) | (v2 >> 2)));
            if (data == 4) {
                const int v3 = lut[static_cast<unsigned char>(s[i + 3])];
                res.bytes.push_back(static_cast<uint8_t>(((v2 & 0x03) << 6) | v3));
            }
        }

        if (last && (data == 2 || data == 3)) {
            const size_t off = i + data - 1;
            const int leftover = lut[static_cast<unsigned char>(s[off])] & (data == 2 ? 0x0F : 0x03);
            if (leftover != 0) {
                last_symbol_error(off);
                return res;
            }
        }
        i += len;
    }

    if (n % 4 == 1) {
        res.bytes.clear();
        res.error.text = "Invalid input length: " + std::to_string(n);
    }
    return res;
}

}  // namespace bytes_mode_detail

// Rust's ValBytesMode::deserialize_string. The base64 attempt runs on URL_SAFE
// and is retried once through STANDARD, but only when URL_SAFE complained about
// a '+' or '/', the two symbols STANDARD has and URL_SAFE does not.
inline BytesModeResult val_bytes_deserialize(const std::string& mode, const std::string& s) {
    if (mode == "base64") {
        BytesModeResult first = bytes_mode_detail::base64_decode(s, true);
        if (first.ok()) return first;
        if (first.error.bad_byte == '/' || first.error.bad_byte == '+') {
            return bytes_mode_detail::base64_decode(s, false);
        }
        return first;
    }
    if (mode == "hex") return bytes_mode_detail::hex_decode(s);
    BytesModeResult res;
    res.bytes.assign(s.begin(), s.end());
    return res;
}

// BytesMode::from_str (serializers/config.rs:98), which the validation-side mode
// shares: the message says "serialization mode" even for `val_json_bytes`.
inline void check_val_bytes_mode(const std::string& mode) {
    if (mode != "utf8" && mode != "base64" && mode != "hex") {
        throw SchemaError("Invalid BytesMode serialization mode: `" + mode +
                          "`, expected utf8 or base64 or hex or ");
    }
}

// The error both inputs raise when the decode failed.
inline ErrorType bytes_invalid_encoding_error(const BytesModeError& error) {
    return ErrorType(ErrorType::Kind::BytesInvalidEncoding,
                     "encoding", error.encoding, "encoding_error", error.text);
}

}  // namespace pydantic_core
