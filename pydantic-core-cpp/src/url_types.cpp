#include "pydantic_core/url_types.hpp"
#include <sstream>
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstdint>

namespace pydantic_core {

static std::string to_lower(std::string s) {
    // ASCII-only: bytes >= 0x80 are left untouched so UTF-8 stays valid.
    for (char& ch : s) {
        unsigned char u = static_cast<unsigned char>(ch);
        if (u >= 0x41 && u <= 0x5A) ch = static_cast<char>(u + 32);
    }
    return s;
}

// WHATWG special schemes: their empty path normalizes to "/" (url.rs scheme_is_special).
bool is_special_scheme(const std::string& s) {
    return s == "http" || s == "https" || s == "ws" || s == "wss" || s == "ftp" || s == "file";
}

namespace {

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// One octet of a dotted quad: the reference takes no leading zero and nothing over 255.
bool valid_octet(const std::string& s, unsigned& value) {
    if (s.empty() || s.size() > 3 || (s.size() > 1 && s[0] == '0')) return false;
    unsigned v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + static_cast<unsigned>(ch - '0');
    }
    if (v > 255) return false;
    value = v;
    return true;
}

} // namespace

// IPv6 literal (no brackets), spelled out here rather than asked of libc: <arpa/inet.h> has no
// Windows spelling (MinGW stops the build on it) and winsock's inet_pton wants a WSAStartup and
// answers differently again.  Asking libc also means the answer depends on which libc shipped --
// macOS and glibc both take a "fe80::1%eth0" scope id and a five-zero group, and the reference
// refuses both.  These are the reference's rules (url crate parser::parse_ipv6_addr): 1-4 hex
// digits per group, at most one "::" standing for one or more zero groups, an IPv4 dotted quad only
// as the trailing 32 bits, and no scope id.  On success out[0..8] holds the address itself, which
// is what makes a canonical spelling possible as well as a yes/no.
static bool parse_ipv6_groups(const std::string& host, uint16_t out[8]) {
    if (host.empty()) return false;

    std::vector<uint16_t> head, tail;               // groups spelled before / after the "::"
    std::vector<uint16_t>* sink = &head;
    bool compressed = false;
    size_t i = 0;
    if (host[0] == ':') {
        if (host.size() < 2 || host[1] != ':') return false;  // ":1::" is not a host
        compressed = true;
        sink = &tail;                                         // everything spelled comes after it
        i = 2;
        if (i == host.size()) {                               // "::" is every bit zero
            for (int g = 0; g < 8; ++g) out[g] = 0;
            return true;
        }
    }

    while (i < host.size()) {
        size_t start = i;
        unsigned value = 0;
        while (i < host.size() && hex_value(host[i]) >= 0) {
            if (i - start == 4) return false;                 // "12345::1" is not a group
            value = value * 16 + static_cast<unsigned>(hex_value(host[i]));
            ++i;
        }
        if (i == start) return false;                         // nothing where a group belongs

        if (i < host.size() && host[i] == '.') {
            // A dotted quad owns the rest of the host and stands for its last two groups, so
            // "1:2.3.4.5" or "1.2.3.4:80" fails here rather than being read as a hex group.
            const std::string rest = host.substr(start);
            unsigned octet[4] = {0, 0, 0, 0};
            size_t p = 0, n = 0;
            for (;; ++n) {
                if (n == 4) return false;
                size_t dot = rest.find('.', p);
                if (!valid_octet(rest.substr(p, dot == std::string::npos ? std::string::npos
                                                                        : dot - p),
                                 octet[n]))
                    return false;
                if (dot == std::string::npos) break;
                p = dot + 1;
            }
            if (n != 3) return false;                                     // "1.2.3" is three
            sink->push_back(static_cast<uint16_t>((octet[0] << 8) | octet[1]));
            sink->push_back(static_cast<uint16_t>((octet[2] << 8) | octet[3]));
            break;                                            // nothing may follow the quad
        }

        sink->push_back(static_cast<uint16_t>(value));
        if (i == host.size()) break;                          // the host ends with this group
        if (host[i] != ':') return false;                     // '%', ' ', '_', anything else
        ++i;
        if (i < host.size() && host[i] == ':') {
            if (compressed) return false;                     // only one "::" run
            compressed = true;
            sink = &tail;
            ++i;
            if (i == host.size()) break;                      // a trailing "::" stands for zeroes
        } else if (i == host.size()) {
            return false;                                     // a lone trailing ':' is not a group
        }
    }

    size_t spelled = head.size() + tail.size();
    if (!compressed) {
        if (spelled != 8) return false;
        for (size_t g = 0; g < 8; ++g) out[g] = head[g];
        return true;
    }
    if (spelled > 7) return false;                            // "::" has to stand for a group
    size_t k = 0;
    for (uint16_t g : head) out[k++] = g;
    while (k < 8 - tail.size()) out[k++] = 0;
    for (uint16_t g : tail) out[k++] = g;
    return true;
}

bool is_valid_ipv6(const std::string& host) {
    uint16_t groups[8];
    return parse_ipv6_groups(host, groups);
}

// The spelling the reference hands back for an address it accepted, because it parses the host
// instead of echoing it.  Measured on the 2.50.0 wheel: the longest run of at least two zero
// groups folds into "::" and a tie goes to the first run -- "1:0:0:2:0:0:3:4" returns
// "1::2:0:0:3:4" while "1:0:0:2:0:0:0:3" returns "1:0:0:2::3" -- a lone zero group is never
// folded, groups drop their leading zeros and print lower case ("0a:0b::1" returns "a:b::1"), and
// a dotted quad comes back as the two hex groups it is ("::ffff:1.2.3.4" returns "::ffff:102:304")
// rather than in the dotted form a std Ipv6Addr would print for a v4-mapped address.
// Returns "" for a host that is not an IPv6 literal, leaving whatever it is uncanonicalized.
std::string canonical_ipv6_host(const std::string& host) {
    uint16_t g[8];
    if (!parse_ipv6_groups(host, g)) return {};

    int best = -1, best_len = 0, run = 0;
    for (int i = 0; i <= 8; ++i) {
        if (i < 8 && g[i] == 0) { ++run; continue; }
        if (run >= 2 && run > best_len) { best = i - run; best_len = run; }
        run = 0;
    }

    auto join = [&g](int from, int to) {
        std::string s;
        for (int i = from; i < to; ++i) {
            if (!s.empty()) s += ':';
            std::ostringstream part;
            part << std::hex << g[i];
            s += part.str();
        }
        return s;
    };
    if (best < 0) return join(0, 8);
    return join(0, best) + "::" + join(best + best_len, 8);
}

void strip_url_whitespace(std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && static_cast<unsigned char>(s[b]) <= 0x20) ++b;
    while (e > b && static_cast<unsigned char>(s[e - 1]) <= 0x20) --e;
    s = s.substr(b, e - b);
}

static bool has_non_ascii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return true;
    return false;
}

// url crate port_or_known_default: scheme-inherent default ports.
static std::optional<int> default_port_for_scheme(const std::string& s) {
    if (s == "http") return 80;
    if (s == "https") return 443;
    if (s == "ws") return 80;
    if (s == "wss") return 443;
    if (s == "ftp") return 21;
    return std::nullopt;
}

// IDN -> punycode for non-ASCII hosts (matches url crate idna handling).
static std::string idna_encode(const std::string& host) {
    if (!has_non_ascii(host)) return host;
    PyObject* decoded = PyUnicode_DecodeUTF8(host.c_str(), host.size(), nullptr);
    if (!decoded) { PyErr_Clear(); return host; }
    try {
        py::object h = py::reinterpret_steal<py::object>(decoded);
        return h.attr("encode")("idna").attr("decode")("ascii").cast<std::string>();
    } catch (...) { if (PyErr_Occurred()) PyErr_Clear(); }
    return host;
}

namespace {

// The URL Standard's userinfo percent-encode set -- with a backslash added, which the Standard does
// not list but the reference does escape ("non-special://a\b@host" comes back as "a%5Cb"; a special
// scheme never shows it because "\" ends its authority first) -- and with "%" deliberately absent:
// the reference passes "%41" and "%ZZ" through untouched instead of decoding and re-encoding, which
// is what makes its own escaped spelling a fixed point.  "#" and "?" are in the set too and simply
// never reach it: they end the authority before a userinfo starts.
bool userinfo_needs_escape(unsigned char c) {
    if (c <= 0x1F || c > 0x7E) return true;   // C0 controls, DEL, and every non-ASCII byte
    switch (c) {
        case ' ':  case '"': case '<': case '>': case '`': case '{': case '}':
        case '/':  case ':': case ';': case '=': case '@': case '[': case ']':
        case '^':  case '|': case '\\':
            return true;
        default:
            return false;
    }
}

} // namespace

// A userinfo in the reference's spelling.  Tab/CR/LF are not escaped but dropped, as they are
// everywhere else in the reference's input, and a non-ASCII byte becomes one %XX per UTF-8 byte.
std::string encode_userinfo(const std::string& raw) {
    static const char HEX[] = "0123456789ABCDEF";
    std::string out;
    for (char ch : raw) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\t' || c == '\r' || c == '\n') continue;
        if (!userinfo_needs_escape(c)) { out.push_back(ch); continue; }
        out.push_back('%');
        out.push_back(HEX[c >> 4]);
        out.push_back(HEX[c & 0xF]);
    }
    return out;
}

// The reference's username and password, from the region before the authority's last "@": the first
// ":" separates them and each half travels escaped on its own ("a:b:c" is user "a", pass "b%3Ac").
// An empty half is not an empty string -- the reference answers no value at all, so ":@" carries no
// userinfo and "@host" serializes without the "@".
void split_userinfo_halves(const std::string& userinfo,
                           std::optional<std::string>& user,
                           std::optional<std::string>& pass) {
    std::string clean;
    for (char ch : userinfo) {
        if (ch != '\t' && ch != '\r' && ch != '\n') clean.push_back(ch);
    }
    size_t colon = clean.find(':');
    std::string user_part = encode_userinfo(clean.substr(0, colon));
    std::string pass_part = (colon == std::string::npos) ? std::string()
                                                         : encode_userinfo(clean.substr(colon + 1));
    if (!user_part.empty()) user = user_part;
    if (!pass_part.empty()) pass = pass_part;
}

// The authority region urllib reads as the netloc: after "scheme:" and an exact "//", up to the
// first "/?#".  Anything else -- no "//", no scheme -- leaves urllib with an empty netloc and the
// whole remainder in the path, so there is no userinfo to rewrite either.  This has to agree with
// urllib's own scan, because the caller keeps urllib for everything past the authority.
static bool authority_span(const std::string& s, size_t& start, size_t& end) {
    start = end = 0;
    if (s.empty()) return false;
    unsigned char first = static_cast<unsigned char>(s[0]);
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z'))) return false;
    size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 1; i < colon; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        bool scheme_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!scheme_char) return false;
    }
    if (s.compare(colon + 1, 2, "//") != 0) return false;
    start = colon + 3;
    size_t e = s.find_first_of("/?#", start);
    end = (e == std::string::npos) ? s.size() : e;
    return true;
}

// Split a string on a single-char delimiter (keeping empty trailing fields).
static std::vector<std::string> split_comma(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// Parse "user:pass@host:port" (userinfo optional) into a HostSpec.
// host is lowercased only when lower=true (special schemes; opaque hosts kept as-is).
static HostSpec parse_host_segment(const std::string& seg, bool lower) {
    HostSpec h;
    std::string s = seg;
    // The last "@", not the first: a userinfo may hold "@" of its own, and it is the host that has
    // to start after the separator ("a@b@h1" is user "a%40b" at host "h1").
    size_t at = s.rfind('@');
    if (at != std::string::npos) {
        split_userinfo_halves(s.substr(0, at), h.username, h.password);
        s = s.substr(at + 1);
    }
    if (!s.empty() && s[0] == '[') {
        size_t be = s.find(']');
        if (be != std::string::npos) {
            h.host = s.substr(0, be + 1);
            // The same rule Url applies, for every host in the list rather than the last: the
            // reference answers the address it parsed, not the spelling it was handed.
            std::string canon = canonical_ipv6_host(s.substr(1, be - 1));
            if (!canon.empty()) h.host = "[" + canon + "]";
            if (be + 1 < s.size() && s[be + 1] == ':') {
                try { h.port = std::stoi(s.substr(be + 2)); }
                catch (...) { throw std::invalid_argument("invalid port: " + seg); }
            }
        } else {
            h.host = s;
        }
    } else {
        size_t colon = s.rfind(':');
        if (colon != std::string::npos) {
            std::string port_str = s.substr(colon + 1);
            // Only treat as port if it is all digits (else it is part of the host).
            bool all_digits = !port_str.empty() &&
                std::all_of(port_str.begin(), port_str.end(),
                            [](char c){ return std::isdigit(static_cast<unsigned char>(c)); });
            if (all_digits) {
                h.host = s.substr(0, colon);
                try { h.port = std::stoi(port_str); }
                catch (...) { throw std::invalid_argument("invalid port: " + seg); }
            } else {
                h.host = s;
            }
        } else {
            h.host = s;
        }
    }
    if (lower) h.host = to_lower(h.host);
    return h;
}

// Parse the tail (starting at first / ? #) into path/query/fragment using urllib.
static void parse_tail(const std::string& scheme, const std::string& tail,
                       std::string& path, std::string& query, std::string& fragment) {
    if (tail.empty()) { path.clear(); query.clear(); fragment.clear(); return; }
    py::object urllib = py::module_::import("urllib.parse");
    py::object parsed = urllib.attr("urlparse")(scheme + "://dummy" + tail);
    path = py::str(parsed.attr("path")).cast<std::string>();
    query = py::str(parsed.attr("query")).cast<std::string>();
    fragment = py::str(parsed.attr("fragment")).cast<std::string>();
}

Url::Url(const std::string& url_str, bool preserve_empty_path, const UrlDefaults& defaults)
    : url_(url_str) {
    std::string trimmed = url_str;
    strip_url_whitespace(trimmed);

    // urllib splits the netloc, and it reads a stray bracket anywhere in it -- a userinfo's "[user"
    // included -- as a broken IPv6 host and refuses the whole URL.  The reference escapes a userinfo
    // instead of refusing it, so the region is escaped here, before urllib is asked, and the escaped
    // text is what every later reader sees: user_, password_, and url_ reassembled below.
    std::string to_parse = trimmed;
    size_t auth_beg = 0, auth_end = 0;
    if (authority_span(trimmed, auth_beg, auth_end)) {
        std::string authority = trimmed.substr(auth_beg, auth_end - auth_beg);
        size_t at_pos = authority.rfind('@');
        if (at_pos != std::string::npos) {
            split_userinfo_halves(authority.substr(0, at_pos), user_, password_);
            std::string userinfo;
            if (user_) userinfo += *user_;
            if (password_) { userinfo += ":"; userinfo += *password_; }
            to_parse = trimmed.substr(0, auth_beg) + userinfo + trimmed.substr(auth_beg + at_pos);
        }
    }

    py::object urllib = py::module_::import("urllib.parse");
    py::object parsed = urllib.attr("urlparse")(to_parse);

    scheme_ = py::str(parsed.attr("scheme")).cast<std::string>();
    if (scheme_.empty()) {
        throw std::invalid_argument("URL must have a scheme: " + url_str);
    }
    scheme_ = to_lower(scheme_);

    std::string netloc = py::str(parsed.attr("netloc")).cast<std::string>();

    // The userinfo is the region before the netloc's last "@" -- the same separator authority_span
    // cut at, now the only "@" left since an escaped userinfo carries none.
    size_t at_pos = netloc.rfind('@');
    if (at_pos != std::string::npos) netloc = netloc.substr(at_pos + 1);

    // Extract port from netloc if present.
    size_t colon_pos = netloc.rfind(':');
    if (colon_pos != std::string::npos && netloc[0] != '[') {
        std::string port_str = netloc.substr(colon_pos + 1);
        bool all_digits = !port_str.empty() &&
            std::all_of(port_str.begin(), port_str.end(),
                        [](char c){ return std::isdigit(static_cast<unsigned char>(c)); });
        if (all_digits) {
            host_ = netloc.substr(0, colon_pos);
            port_ = std::stoi(port_str);
        } else {
            host_ = netloc;
        }
    } else if (!netloc.empty() && netloc[0] == '[' && netloc.find(']:') != std::string::npos) {
        size_t bracket_end = netloc.find(']');
        host_ = netloc.substr(0, bracket_end + 1);
        try { port_ = std::stoi(netloc.substr(bracket_end + 2)); } catch (...) {}
    } else {
        host_ = netloc;
    }

    path_ = py::str(parsed.attr("path")).cast<std::string>();
    query_ = py::str(parsed.attr("query")).cast<std::string>();
    fragment_ = py::str(parsed.attr("fragment")).cast<std::string>();

    // Special schemes parse the host as a domain: lowercase + IDN punycode.
    // Non-special schemes keep the (opaque) host exactly as given.
    if (is_special_scheme(scheme_)) {
        host_ = idna_encode(to_lower(host_));
    }

    // A bracketed host is an address, not a spelling: the reference answers the address it parsed
    // rather than the bytes it was handed, so the canonical text is what every later reader has to
    // see -- host_, the url_ reassembled below, and through them unicode_string, __str__, repr and
    // the json leaf.  A host that will not parse keeps its bytes: refusing is is_valid_ipv6's job, not
    // this one's, and a Url built from a string the validator already blessed never hits it.
    if (host_.size() > 2 && host_.front() == '[' && host_.back() == ']') {
        std::string canon = canonical_ipv6_host(host_.substr(1, host_.size() - 2));
        if (!canon.empty()) host_ = "[" + canon + "]";
    }

    // check_sub_defaults: substitute default_host / default_port / default_path.
    if (host_.empty() && defaults.host) host_ = *defaults.host;
    if (!port_ && defaults.port) port_ = defaults.port;
    bool default_path_applied = false;
    if (defaults.path && (path_.empty() || path_ == "/")) { path_ = *defaults.path; default_path_applied = true; }

    // Special scheme: an empty path on an authority becomes "/" (url crate).
    if (!host_.empty() && path_.empty() && is_special_scheme(scheme_) && !preserve_empty_path && !default_path_applied)
        path_ = "/";

    // file scheme: a "localhost" host is dropped (url crate normalization).
    if (scheme_ == "file" && host_ == "localhost") host_ = "";

    // file scheme: the path is normalized to start with exactly one slash.
    if (scheme_ == "file") {
        size_t lead = 0;
        while (lead < path_.size() && path_[lead] == '/') ++lead;
        path_ = "/" + path_.substr(lead);
    }

    std::string out = scheme_ + "://";
    bool has_user = user_.has_value();
    bool has_pass = password_.has_value();
    if (has_user || has_pass) {
        if (has_user) out += *user_;
        if (has_pass) out += ":" + *password_;
        out += "@";
    }
    out += host_;
    if (port_ && *port_ != default_port_for_scheme(scheme_).value_or(-1)) out += ":" + std::to_string(*port_);
    out += path_;
    if (!query_.empty()) out += "?" + query_;
    if (!fragment_.empty()) out += "#" + fragment_;
    url_ = out;
}

std::optional<int> Url::port_or_default() const {
    return port_ ? port_ : default_port_for_scheme(scheme_);
}

bool MultiHostUrl::has_host() const {
    return !hosts_.empty() && !hosts_.back().host.empty();
}

std::optional<std::string> MultiHostUrl::user() const {
    if (hosts_.empty()) return std::nullopt;
    return hosts_.front().username;
}

std::optional<std::string> MultiHostUrl::password() const {
    if (hosts_.empty()) return std::nullopt;
    return hosts_.front().password;
}

MultiHostUrl::MultiHostUrl(const std::string& url_str, bool preserve_empty_path,
                           const UrlDefaults& defaults) : url_(url_str) {
    std::string trimmed = url_str;
    strip_url_whitespace(trimmed);
    if (trimmed.empty()) {
        throw std::invalid_argument("empty input");
    }

    // scheme
    std::regex scheme_regex("^([a-zA-Z][a-zA-Z0-9+.-]*):");
    std::smatch scheme_match;
    if (!std::regex_search(trimmed, scheme_match, scheme_regex)) {
        throw std::invalid_argument("relative URL without a base");
    }
    scheme_ = to_lower(scheme_match[1].str());
    std::string rest = trimmed.substr(scheme_match[0].length());

    // consume any number of slashes/backslashes after the scheme colon
    size_t si = 0;
    while (si < rest.size() && (rest[si] == '/' || rest[si] == '\\')) ++si;
    std::string remaining = rest.substr(si);

    // host section ends at the first '/', '?' or '#'
    size_t hend = remaining.find_first_of("/?#");
    std::string host_section = (hend == std::string::npos) ? remaining : remaining.substr(0, hend);
    std::string tail = (hend == std::string::npos) ? std::string() : remaining.substr(hend);

    parse_tail(scheme_, tail, path_, query_, fragment_);

    // split host section on ','; with a comma present none may be empty
    std::vector<std::string> segs = split_comma(host_section);
    bool has_comma = host_section.find(',') != std::string::npos;
    if (has_comma) {
        for (const auto& s : segs) {
            if (s.empty()) throw UrlEmptyHostError();
        }
    }
    if (segs.empty()) segs.push_back("");  // single (possibly empty) host, allows default host

    bool lower = is_special_scheme(scheme_);
    for (const auto& seg : segs) {
        hosts_.push_back(parse_host_segment(seg, lower));
    }

    // check_sub_defaults applies to the reference (last) host
    HostSpec& ref = hosts_.back();
    if (ref.host.empty() && defaults.host) ref.host = *defaults.host;
    if (!ref.port && defaults.port) ref.port = defaults.port;
    bool default_path_applied = false;
    if (defaults.path && (path_.empty() || path_ == "/")) { path_ = *defaults.path; default_path_applied = true; }

    // Special scheme: empty path on an authority becomes "/" (url crate).
    if (has_host() && path_.empty() && is_special_scheme(scheme_) && !preserve_empty_path && !default_path_applied)
        path_ = "/";

    // Build the canonical string: scheme://[user:pass@]host1[:port1],host2[:port2],.../path?query#fragment
    std::string out = scheme_ + "://";
    for (size_t k = 0; k < hosts_.size(); ++k) {
        const HostSpec& h = hosts_[k];
        bool has_user = h.username.has_value();
        bool has_pass = h.password.has_value();
        if (has_user || has_pass) {
            if (has_user) out += *h.username;
            if (has_pass) out += ":" + *h.password;
            out += "@";
        }
        out += h.host;
        if (h.port && *h.port != default_port_for_scheme(scheme_).value_or(-1))
            out += ":" + std::to_string(*h.port);
        if (k + 1 < hosts_.size()) out += ",";
    }
    out += path_;
    if (!query_.empty()) out += "?" + query_;
    if (!fragment_.empty()) out += "#" + fragment_;
    url_ = out;
}

} // namespace pydantic_core
