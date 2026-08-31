# P0 fixes — test_docs_examples (C++ backend)

Goal: clear the ~45 cheapest failing docs tests (error codes, error text, titles).

## Wave 1 — error-type codes
- [ ] 1.1 Add missing Kinds to `error_types.hpp` enum (url_parsing/syntax/too_long, uuid_parsing/version,
      decimal_*, iterable_type, iteration_error, mapping_type, string_unicode, needs_python_object,
      model_attributes_type, get_attribute_error, complex_*, date_from_datetime_parsing,
      union_tag_invalid/not_found, invalid_key, string_sub_type, int_parsing_size,
      bytes_invalid_encoding, set_item_not_hashable, missing_sentinel_error, json_type,
      dataclass_exact_type, default_factory_not_called, decimal_whole_digits)
- [ ] 1.2 Complete `Kind→name` forward map + Rust-exact message templates in `error_types.cpp`
      (enum→"enum", time_delta_* names, url_*, uuid_*, decimal_*, …)
- [ ] 1.3 Fix emitters: URL validator (url_parsing/syntax/too_long/type/scheme), UUID
      (uuid_parsing/type), enum (EnumError), bool (bool_parsing + Rust token set + int 0/1 + float),
      decimal (decimal_parsing), float→int (IntFromFloat), bytes→str (string_unicode),
      date (date_from_datetime_parsing, int/float→date), datetime (datetime_from_date_parsing),
      JSON strict accepts date/datetime strings, set (SetType), is-subclass JSON (needs_python_object),
      from_attributes (model_attributes_type/get_attribute_error), big-int strings
- [ ] 1.4 Rebuild + run validation_errors.md / migration.md error-type tests

## Wave 2 — error text & formatting
- [ ] 2.1 `input_value`: Python repr for containers (replace `list(len=N)` etc.), str already quoted
- [ ] 2.2 `input_type` in C++ display message (type qualname of input)
- [ ] 2.3 Rust-style 50-byte truncation of input in display message (keep full in errors() JSON)
- [ ] 2.4 Python wrapper: `url` key AFTER `ctx` in errors(); fix `from_exception_data` indentation
      (loc at col 0, msg at col 2); no `__PYDANTIC_ERRORS__` leak in str()
- [ ] 2.5 Rebuild + run formatting-sensitive docs tests (errors.md, config.md, types.md, json_schema.md,
      validators.md, validation_decorator.md, fields.md)

## Wave 3 — titles
- [ ] 3.1 Title = `config['title']` if set, else top-level validator display name
      (list[User], constrained-int, model name, union/nullable unwraps)
- [ ] 3.2 Rebuild + run title-sensitive tests (type_adapter.md, performance.md, strict_mode.md,
      models.md generic titles, validation_decorator.md)

## Definition of done
- Targeted subsets pass; full `test_docs_examples` failure count drops (baseline 121).
- No regressions in `tests/test_main.py`, `tests/test_types.py`, `tests/test_errors.py` (or the
  closest C++ test suites: pydantic-core-cpp/tests + a few pydantic suites).
