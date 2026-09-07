#pragma once

#include <string>
#include <optional>
#include <vector>
#include <stdexcept>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace pydantic_core {

// Strip leading/trailing C0 control + space chars (url crate input trimming).
void strip_url_whitespace(std::string& s);

// WHATWG special schemes (url.rs scheme_is_special).
bool is_special_scheme(const std::string& s);

// Validate an IPv6 literal (no brackets), matching url crate host parsing.
bool is_valid_ipv6(const std::string& host);

// Thrown when a host is required to be non-empty but is empty.
// Maps to the url_parsing error with message "empty host".
struct UrlEmptyHostError : public std::invalid_argument {
    UrlEmptyHostError() : std::invalid_argument("empty host") {}
};

// Host specification for MultiHostUrl.
struct HostSpec {
    std::optional<std::string> username;
    std::optional<std::string> password;
    std::string host;
    std::optional<int> port;
};

// default_host / default_port / default_path substitutions (url.rs check_sub_defaults).
struct UrlDefaults {
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<std::string> path;
};

// Url class - single URL with parsing and validation
class Url {
public:
    Url(const std::string& url_str, bool preserve_empty_path = false,
        const UrlDefaults& defaults = {});

    std::string str() const { return url_; }
    std::string scheme() const { return scheme_; }
    std::string host() const { return host_; }
    bool has_host() const { return !host_.empty(); }
    std::optional<int> port() const { return port_; }
    std::optional<int> port_or_default() const;
    std::string unicode_string() const { return url_; }
    std::string path() const { return path_; }
    std::string query() const { return query_; }
    std::string fragment() const { return fragment_; }
    std::optional<std::string> user() const { return user_; }
    std::optional<std::string> password() const { return password_; }

private:
    std::string url_;
    std::string scheme_;
    std::string host_;
    std::optional<int> port_;
    std::string path_;
    std::string query_;
    std::string fragment_;
    std::optional<std::string> user_;
    std::optional<std::string> password_;
};

// MultiHostUrl class - URL with multiple hosts
class MultiHostUrl {
public:
    MultiHostUrl(const std::string& url_str, bool preserve_empty_path = false,
                 const UrlDefaults& defaults = {});

    std::string str() const { return url_; }
    std::string scheme() const { return scheme_; }
    std::vector<HostSpec> hosts() const { return hosts_; }
    bool has_host() const;
    std::string path() const { return path_; }
    std::string query() const { return query_; }
    std::string fragment() const { return fragment_; }
    std::optional<std::string> user() const;
    std::optional<std::string> password() const;

private:
    std::string url_;
    std::string scheme_;
    std::vector<HostSpec> hosts_;  // in string order: extras..., ref(last)
    std::string path_;
    std::string query_;
    std::string fragment_;
};

} // namespace pydantic_core
