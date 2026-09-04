#include "pydantic_core/url_types.hpp"
#include <sstream>
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <cctype>

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
    size_t at = s.find('@');
    if (at != std::string::npos) {
        std::string auth = s.substr(0, at);
        size_t colon = auth.find(':');
        if (colon != std::string::npos) {
            h.username = auth.substr(0, colon);
            h.password = auth.substr(colon + 1);
        } else {
            h.username = auth;
        }
        s = s.substr(at + 1);
    }
    if (!s.empty() && s[0] == '[') {
        size_t be = s.find(']');
        if (be != std::string::npos) {
            h.host = s.substr(0, be + 1);
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
    py::object urllib = py::module_::import("urllib.parse");
    py::object parsed = urllib.attr("urlparse")(trimmed);

    scheme_ = py::str(parsed.attr("scheme")).cast<std::string>();
    if (scheme_.empty()) {
        throw std::invalid_argument("URL must have a scheme: " + url_str);
    }
    scheme_ = to_lower(scheme_);

    std::string netloc = py::str(parsed.attr("netloc")).cast<std::string>();

    // Extract user:password from netloc if present.
    size_t at_pos = netloc.find('@');
    if (at_pos != std::string::npos) {
        std::string auth = netloc.substr(0, at_pos);
        size_t colon_pos = auth.find(':');
        if (colon_pos != std::string::npos) {
            user_ = auth.substr(0, colon_pos);
            password_ = auth.substr(colon_pos + 1);
        } else {
            user_ = auth;
        }
        netloc = netloc.substr(at_pos + 1);
    }

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
