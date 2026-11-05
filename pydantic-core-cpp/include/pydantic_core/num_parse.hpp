#pragma once

// A float read out of text. The reference does this in ONE function -- str_as_float
// (input/shared.rs:94) -- which every text input shares: a Python str (input_python.rs:321), a
// JSON string (input_json.rs:196) and strings mode (input_string.rs:157). It trims, parses the
// WHOLE remaining string as f64, and only when that fails tries once more with underscores
// removed; that second attempt does NOT trim and refuses edge or doubled underscores
// (shared.rs:53), which is why "1_0" is 10.0 while " 1_0 " is refused.
//
// std::stod is a different grammar and cannot stand in for it: it stops at the first character it
// cannot use, so "2020-01-01" read as 2020.0 and "1d5" as 1.0; it reads "0x10" as a hex float; it
// throws on out-of-range where Rust returns +/-inf and 0.0; and it skips the C whitespace set
// where Rust's str::trim trims the Unicode White_Space property. So the grammar is checked here
// first, and strtod is left with only what it is good at: the correctly rounded value of a token
// already known to be a decimal number.

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale>
#include <string>

namespace pydantic_core {
namespace num_parse_detail {

// Rust trims through char::is_whitespace, i.e. the Unicode White_Space property. NBSP is the one
// that bites in practice -- it is what an HTML entity or a Windows path decodes to.
inline bool is_cp_whitespace(uint32_t cp) {
    return cp == 0x09 || (cp >= 0x0A && cp <= 0x0D) || cp == 0x20 || cp == 0x85 || cp == 0xA0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// Decodes the code point at `p` and reports its width. A byte that does not start a well-formed
// sequence is its own one-byte code point, so trimming can never run off the end of the string.
inline uint32_t next_cp(const char* p, const char* end, size_t* width) {
    unsigned char b0 = static_cast<unsigned char>(*p);
    if (b0 < 0x80) {
        *width = 1;
        return b0;
    }
    size_t len = b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : b0 >= 0xC0 ? 2 : 1;
    if (len == 1 || static_cast<size_t>(end - p) < len) {
        *width = 1;
        return b0;
    }
    uint32_t cp = b0 & (0x7F >> len);
    for (size_t i = 1; i < len; i++) {
        unsigned char b = static_cast<unsigned char>(p[i]);
        if ((b & 0xC0) != 0x80) {
            *width = 1;
            return b0;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    *width = len;
    return cp;
}

inline void trim_rust(const std::string& s, size_t* begin, size_t* end) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b) {
        size_t w = 1;
        if (!is_cp_whitespace(next_cp(s.data() + a, s.data() + b, &w))) break;
        a += w;
    }
    while (b > a) {
        // Continuation bytes (0x80..0xBF) belong to the code point that starts at the first
        // non-continuation byte before them.
        size_t start = b - 1;
        while (start > a && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;
        size_t w = 1;
        if (!is_cp_whitespace(next_cp(s.data() + start, s.data() + b, &w))) break;
        b = start;
    }
    *begin = a;
    *end = b;
}

inline bool word_equal(const char* p, const char* end, const char* word) {
    size_t n = std::strlen(word);
    if (static_cast<size_t>(end - p) != n) return false;
    for (size_t i = 0; i < n; i++) {
        char a = p[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != word[i]) return false;
    }
    return true;
}

// Rust's f64::from_str grammar, scanned so what is left over is refused rather than ignored.
// Digits are ASCII only: "٣" and "１２３" are what Python's float() accepts, and Rust refuses them.
inline bool decimal_grammar(const char* p, const char* end) {
    if (p < end && (*p == '+' || *p == '-')) p++;
    if (p == end) return false;
    if (word_equal(p, end, "inf") || word_equal(p, end, "infinity") || word_equal(p, end, "nan")) {
        return true;
    }
    const char* digits = p;
    while (p < end && *p >= '0' && *p <= '9') p++;
    bool int_digits = p > digits;
    bool frac_digits = false;
    if (p < end && *p == '.') {
        p++;
        const char* frac = p;
        while (p < end && *p >= '0' && *p <= '9') p++;
        frac_digits = p > frac;
    }
    if (!int_digits && !frac_digits) return false;  // "", ".", ".e3"
    if (p == end) return true;
    if (*p != 'e' && *p != 'E') return false;       // trailing junk, "0x10", "1d5", "1_0"
    p++;
    if (p < end && (*p == '+' || *p == '-')) p++;
    const char* exp = p;
    while (p < end && *p >= '0' && *p <= '9') p++;
    return p > exp && p == end;                     // "1e" and "1e5x" are refused
}

inline const std::string& local_decimal_point() {
    static const std::string point = [] {
        const char* dp = std::localeconv()->decimal_point;
        return std::string((dp && *dp) ? dp : ".");
    }();
    return point;
}

// The token already matches the grammar above, so the only locale-sensitive character in it is the
// decimal separator. Hand strtod the one this process's LC_NUMERIC expects rather than assuming
// the port runs under "C": strtod stops at a separator it does not know, and the value would come
// back truncated rather than refused.
inline double convert_decimal(const char* p, const char* end) {
    std::string token(p, end);
    const std::string& dp = local_decimal_point();
    if (dp != ".") {
        size_t dot = token.find('.');
        if (dot != std::string::npos) token.replace(dot, 1, dp);
    }
    errno = 0;
    // Out of range is not an error here: strtod gives +/-inf and 0 or a subnormal, which is what
    // the reference returns for "1e400" and "1e-400".
    return std::strtod(token.c_str(), nullptr);
}

inline bool parse_rust_f64(const char* p, const char* end, double* out) {
    if (!decimal_grammar(p, end)) return false;
    const char* word = p;
    bool negative = false;
    if (word < end && (*word == '+' || *word == '-')) {
        negative = *word == '-';
        word++;
    }
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (word_equal(word, end, "nan")) {
        *out = negative ? -nan : nan;
        return true;
    }
    if (word_equal(word, end, "inf") || word_equal(word, end, "infinity")) {
        *out = negative ? -inf : inf;
        return true;
    }
    // A decimal keeps its sign: strtod is handed the whole token. `word` only ever pointed past the
    // sign so the inf/nan words above could be compared without it.
    *out = convert_decimal(p, end);
    return true;
}

}  // namespace num_parse_detail

inline bool str_as_float(const std::string& s, double* out) {
    size_t begin = 0;
    size_t end = s.size();
    num_parse_detail::trim_rust(s, &begin, &end);
    if (num_parse_detail::parse_rust_f64(s.data() + begin, s.data() + end, out)) return true;

    // strip_underscores (input/shared.rs:53) answers None -- so no second attempt -- for a string
    // that starts or ends with an underscore, has none, or has a doubled one. It is handed the
    // untrimmed string, so " 1_0 " stays refused after its one replacement pass.
    if (s.empty() || s.front() == '_' || s.back() == '_' || s.find('_') == std::string::npos ||
        s.find("__") != std::string::npos) {
        return false;
    }
    std::string stripped;
    stripped.reserve(s.size());
    for (char c : s) {
        if (c != '_') stripped.push_back(c);
    }
    return num_parse_detail::parse_rust_f64(stripped.data(), stripped.data() + stripped.size(), out);
}

}  // namespace pydantic_core
