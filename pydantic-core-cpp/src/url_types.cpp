#include "pydantic_core/url_types.hpp"
#include <sstream>
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace pydantic_core {

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// WHATWG special schemes: their empty path normalizes to "/" (url.rs scheme_is_special).
static bool is_special_scheme(const std::string& s) {
    return s == "http" || s == "https" || s == "ws" || s == "wss" || s == "ftp" || s == "file";
}

Url::Url(const std::string& url_str, bool preserve_empty_path) : url_(url_str) {
    // Parse URL using Python's urllib.parse via pybind11
    py::object urllib = py::module_::import("urllib.parse");
    py::object parsed = urllib.attr("urlparse")(url_str);
    
    scheme_ = py::str(parsed.attr("scheme")).cast<std::string>();
    if (scheme_.empty()) {
        throw std::invalid_argument("URL must have a scheme: " + url_str);
    }
    
    // Get host (netloc)
    std::string netloc = py::str(parsed.attr("netloc")).cast<std::string>();
    
    // Extract user:password from netloc if present
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
    
    // Extract port from netloc if present
    size_t colon_pos = netloc.rfind(':');
    if (colon_pos != std::string::npos && netloc[0] != '[') {
        // Has port (not IPv6)
        host_ = netloc.substr(0, colon_pos);
        try {
            port_ = std::stoi(netloc.substr(colon_pos + 1));
        } catch (...) {
            // Invalid port, ignore
        }
    } else if (netloc[0] == '[' && netloc.find(']:') != std::string::npos) {
        // IPv6 with port
        size_t bracket_end = netloc.find(']');
        host_ = netloc.substr(0, bracket_end + 1);
        try {
            port_ = std::stoi(netloc.substr(bracket_end + 2));
        } catch (...) {
            // Invalid port, ignore
        }
    } else {
        host_ = netloc;
    }
    
    path_ = py::str(parsed.attr("path")).cast<std::string>();
    query_ = py::str(parsed.attr("query")).cast<std::string>();
    fragment_ = py::str(parsed.attr("fragment")).cast<std::string>();

    // Canonicalize to match Rust's `url` crate serialization: lowercase the
    // scheme and host, and an empty path on an authority becomes "/". The
    // component getters above are left untouched; only the string form changes.
    scheme_ = to_lower(scheme_);
    host_ = to_lower(host_);
    if (!host_.empty() && path_.empty() && is_special_scheme(scheme_) && !preserve_empty_path) path_ = "/";

    std::string out = scheme_ + "://";
    bool has_user = user_ && !user_->empty();
    bool has_pass = password_ && !password_->empty();
    if (has_user || has_pass) {
        if (has_user) out += *user_;
        if (has_pass) out += ":" + *password_;
        out += "@";
    }
    out += host_;
    if (port_) out += ":" + std::to_string(*port_);
    out += path_;
    if (!query_.empty()) out += "?" + query_;
    if (!fragment_.empty()) out += "#" + fragment_;
    url_ = out;
}

MultiHostUrl::MultiHostUrl(const std::string& url_str) : url_(url_str) {
    // Parse multi-host URL
    // Format: scheme://[user:pass@]host1:port1,host2:port2,.../path
    
    // Extract scheme
    std::regex scheme_regex("^([a-zA-Z][a-zA-Z0-9+.-]*)://");
    std::smatch scheme_match;
    if (!std::regex_search(url_str, scheme_match, scheme_regex)) {
        throw std::invalid_argument("MultiHostUrl must have a scheme: " + url_str);
    }
    scheme_ = scheme_match[1].str();
    std::string rest = url_str.substr(scheme_match[0].length());
    
    // Extract user:pass if present
    std::regex auth_regex("^([^@]+)@");
    std::smatch auth_match;
    if (std::regex_search(rest, auth_match, auth_regex)) {
        std::string auth = auth_match[1].str();
        size_t colon_pos = auth.find(':');
        if (colon_pos != std::string::npos) {
            user_ = auth.substr(0, colon_pos);
            password_ = auth.substr(colon_pos + 1);
        } else {
            user_ = auth;
        }
        rest = rest.substr(auth_match[0].length());
    }
    
    // Find path (starts with /)
    size_t path_pos = rest.find('/');
    std::string hosts_part;
    if (path_pos != std::string::npos) {
        hosts_part = rest.substr(0, path_pos);
        std::string remaining = rest.substr(path_pos);
        // Parse path/query/fragment
        py::object urllib = py::module_::import("urllib.parse");
        py::object parsed = urllib.attr("urlparse")(scheme_ + "://dummy" + remaining);
        path_ = py::str(parsed.attr("path")).cast<std::string>();
        query_ = py::str(parsed.attr("query")).cast<std::string>();
        fragment_ = py::str(parsed.attr("fragment")).cast<std::string>();
    } else {
        hosts_part = rest;
        path_ = "";
        query_ = "";
        fragment_ = "";
    }
    
    // Parse hosts (comma-separated)
    std::stringstream ss(hosts_part);
    std::string host_spec;
    while (std::getline(ss, host_spec, ',')) {
        if (host_spec.empty()) {
            throw std::invalid_argument("Empty host specification in MultiHostUrl: " + url_str);
        }

        HostSpec spec;

        // Check for IPv6 with port
        if (host_spec[0] == '[' && host_spec.find(']:') != std::string::npos) {
            size_t bracket_end = host_spec.find(']');
            spec.host = host_spec.substr(0, bracket_end + 1);
            try {
                spec.port = std::stoi(host_spec.substr(bracket_end + 2));
            } catch (...) {
                throw std::invalid_argument("Invalid port in host: " + host_spec);
            }
        } else if (host_spec.find(':') != std::string::npos) {
            // Has port
            size_t colon_pos = host_spec.rfind(':');
            spec.host = host_spec.substr(0, colon_pos);
            try {
                spec.port = std::stoi(host_spec.substr(colon_pos + 1));
            } catch (...) {
                throw std::invalid_argument("Invalid port in host: " + host_spec);
            }
        } else {
            spec.host = host_spec;
        }
        
        if (spec.host.empty()) {
            throw std::invalid_argument("Empty host in MultiHostUrl: " + url_str);
        }
        
        hosts_.push_back(spec);
    }
    
    if (hosts_.empty()) {
        throw std::invalid_argument("MultiHostUrl must have at least one host: " + url_str);
    }
}

} // namespace pydantic_core