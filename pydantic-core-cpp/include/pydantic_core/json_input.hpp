#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <simdjson.h>
#include "result.hpp"
#include "input.hpp"

namespace pydantic_core {

// JSON input implementation using simdjson
class JsonInput : public Input {
public:
    explicit JsonInput(simdjson::simdjson_result<simdjson::dom::element> element);
    explicit JsonInput(const simdjson::dom::element& element);

    InputType input_type() const override { return InputType::Json; }
    InputValue as_error_value() const override;
    bool is_none() const override;
    
    // Convert JSON to Python object
    py::object as_python_object() const override;

    ValResult<ValMatch<EitherString>> validate_str(bool strict, bool coerce_numbers = false) const override;
    ValResult<ValMatch<EitherBytes>> validate_bytes(bool strict) const override;
    ValResult<ValMatch<bool>> validate_bool(bool strict) const override;
    ValResult<ValMatch<EitherInt>> validate_int(bool strict) const override;
    ValResult<ValMatch<EitherFloat>> validate_float(bool strict) const override;

    // Date/time validation
    ValResult<ValMatch<EitherDate>> validate_date(bool strict, TimestampUnit unit) const override;
    ValResult<ValMatch<EitherDateTime>> validate_datetime(bool strict, TimestampUnit unit) const override;
    ValResult<ValMatch<EitherTime>> validate_time(bool strict) const override;
    ValResult<ValMatch<EitherTimedelta>> validate_timedelta(bool strict) const override;
    
    ValResult<std::unique_ptr<ValidatedDict>> validate_dict(bool strict) const override;
    ValResult<ValMatch<std::unique_ptr<ValidatedList>>> validate_list(bool strict) const override;
    ValResult<ValMatch<std::unique_ptr<ValidatedTuple>>> validate_tuple(bool strict) const override;
    
    const simdjson::dom::element& json_element() const { return element_; }

    // Create a JsonInput wrapping an existing element (shares the element, no parser needed)
    // The caller must ensure the element's parent parser stays alive.
    static std::unique_ptr<JsonInput> create_from_element(const simdjson::dom::element& element);

private:
    simdjson::dom::element element_;
    std::unique_ptr<simdjson::dom::parser> parser_;  // Parser must stay alive
    
    // Friend declaration for parse_json
    friend ValResult<std::unique_ptr<JsonInput>> parse_json(std::string_view json_str);
};

// JSON validated dict
class JsonValidatedDict : public ValidatedDict {
public:
    explicit JsonValidatedDict(simdjson::dom::object obj) : obj_(obj) {}
    
    size_t size() const override { return obj_.size(); }
    bool empty() const override { return obj_.size() == 0; }
    std::vector<Entry> entries() const override;
    std::vector<std::string> keys() const override;
    bool has_key(const std::string& key) const override;
    std::optional<Entry> get(const std::string& key) const override;
    std::optional<simdjson::dom::element> get_element(const std::string& key) const;
    std::optional<py::object> get_value(const std::string& key) const override;
    std::optional<py::object> get_key(const std::string& key) const override;
    
private:
    simdjson::dom::object obj_;
};

// JSON validated list
class JsonValidatedList : public ValidatedList {
public:
    explicit JsonValidatedList(simdjson::dom::array arr) : arr_(arr) {}
    
    size_t size() const override { return arr_.size(); }
    bool empty() const override { return arr_.size() == 0; }
    std::vector<Entry> entries() const override;
    py::object get_item(size_t index) const override;

private:
    simdjson::dom::array arr_;
};

// JSON validated tuple (arrays are used as tuples in JSON)
class JsonValidatedTuple : public ValidatedTuple {
public:
    explicit JsonValidatedTuple(simdjson::dom::array arr) : arr_(arr) {}

    size_t size() const override { return arr_.size(); }
    bool empty() const override { return arr_.size() == 0; }
    std::vector<Entry> entries() const override;
    py::object get_item(size_t index) const override;
    
private:
    simdjson::dom::array arr_;
};

// Parse JSON to JsonInput
ValResult<std::unique_ptr<JsonInput>> parse_json(std::string_view json_str);

// Describe the first deviation from the JSON grammar in jiter's wording, which
// is what pydantic exposes as the json_invalid message.  Returns nullopt when
// the text is well-formed, so callers keep their own parser as the authority
// and use this only to word the failure.
std::optional<std::string> json_diagnose_parse_error(const std::string& json_text);

struct JsonParseOutcome {
    bool ok = false;
    py::object value;               // meaningful when ok
    std::string error_description;  // jiter-style wording when not ok
};

// Decode JSON text into Python objects (json.loads is permissive in exactly the
// way jiter's allow_inf_nan mode is).
JsonParseOutcome json_parse_python(const std::string& json_text);

} // namespace pydantic_core