#pragma once

#include <string>
#include <optional>
#include <vector>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace pydantic_core {

// Strip leading/trailing C0 control + space chars (url crate input trimming).
void strip_url_whitespace(std::string& s);

// WHATWG special schemes (url.rs scheme_is_special).
bool is_special_scheme(const std::string& s);

// Host specification for MultiHostUrl
struct HostSpec {
    std::string host;
    std::optional<int> port;
};

// Url class - single URL with parsing and validation
class Url {
public:
    explicit Url(const std::string& url_str, bool preserve_empty_path = false);
    
    std::string str() const { return url_; }
    std::string scheme() const { return scheme_; }
    std::string host() const { return host_; }
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
    explicit MultiHostUrl(const std::string& url_str);
    
    std::string str() const { return url_; }
    std::string scheme() const { return scheme_; }
    std::vector<HostSpec> hosts() const { return hosts_; }
    std::string path() const { return path_; }
    std::string query() const { return query_; }
    std::string fragment() const { return fragment_; }
    std::optional<std::string> user() const { return user_; }
    std::optional<std::string> password() const { return password_; }

private:
    std::string url_;
    std::string scheme_;
    std::vector<HostSpec> hosts_;
    std::string path_;
    std::string query_;
    std::string fragment_;
    std::optional<std::string> user_;
    std::optional<std::string> password_;
};

} // namespace pydantic_core