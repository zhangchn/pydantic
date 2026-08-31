# `tests/test_docs.py::test_docs_examples` — failure investigation (C++ backend)

**Date**: 2026-08
**Status**: 121 failed / 363 passed / 86 skipped (C++ backend, the default)
**Repro**: `python -m pytest tests/test_docs.py::test_docs_examples -q`
**Scope**: `pydantic-core-cpp/` — the C++ port of pydantic-core. The Rust implementation
(`pydantic-core/`) is the reference; every behavior below was verified against it.

The 121 failures reduce to **~20 distinct root causes**. None are doc bugs — they are
genuine behavioral/semantic gaps in the C++ port. Grouped by area below, with the
failing test IDs, the observed-vs-expected delta, and the code location of the bug.

---

## 1. Error-type code mismatches (largest group — 25+ failures)

Docs examples assert `exc.errors()[0]['type']` byte-for-byte. The C++ port emits the
wrong `ErrorType::Kind` (or a kind whose name is missing from the `Kind → string` map).

### 1a. Wrong Kind emitted by input validators (lax coercion)

Reference (Rust `pydantic-core/src/input/{input_python.rs,input_json.rs,shared.rs}`):
in lax mode, a string/int/float that *cannot* coerce to the target type produces the
`*_parsing` code; only a genuinely wrong type produces `*_type`.

| Test (docs line) | Expected | Got | Where |
|---|---|---|---|
| `validation_errors.md:60-75` | `bool_parsing` | `bool_type` | `src/input/python_input.cpp:369`, `src/input/json_input.cpp:165` |
| `validation_errors.md:614-629` | `decimal_parsing` | `decimal_type` | decimal path in `src/input/*.cpp` |
| `performance.md:192-207` | `bool_parsing` (1 error) | `bool_type` (2 errors — see §2e) | same as bool row |

Bool details (Rust `shared.rs::str_as_bool` / `int_as_bool`):
- Accepted strings: `'' f n no off false 0 t y on yes true 1` (case-insensitive).
  C++ (`python_input.cpp:355-372`, `json_input.cpp:152-163`) accepts only
  `true/1/True` and `false/0/False` (json: lowercased `true/1/false/0`).
- Lax int must be exactly 0/1, anything else → `bool_parsing`; C++ accepts any int (`v != 0`).
- Lax float → int → 0/1; C++ has no float path at all.
- `bool_parsing` vs `bool_type` is also the difference for `int | str`-style union
  member errors.

### 1b. Date/datetime/time error codes

| Test | Expected | Got | Root cause |
|---|---|---|---|
| `validation_errors.md:327-342` | `date_from_datetime_parsing` | `date_parsing` | datetime→date parse path uses `Kind::DateParsing` |
| `validation_errors.md:435-451` | `datetime_from_date_parsing` | `datetime_parsing` | date→datetime path uses `Kind::DateTimeParsing` |
| `validation_errors.md:1829-1844` | `time_delta_parsing` | `timedelta_parsing` | Kind name map (`error_types.cpp`) uses the wrong string |
| `validation_errors.md:1850-1865` | `time_delta_type` | `timedelta_type` | same |
| `validation_errors.md:478-500` | validates OK | `NotImplementedError: a tzinfo subclass must implement utcoffset()` | C++ datetime extractor calls `dt.utcoffset` on user tzinfo (`python_input.cpp:884-897`); Rust only reads `tzinfo is None` / `utcoffset` via safe pyo3 access and tolerates custom tzinfos. |
| `strict_mode.md:62-82` | `validate_json('"2000-01-01"', strict=True)` OK | `date_type` | Rust `input_json.rs::validate_date` ignores `strict` for JSON strings; C++ rejects in strict. (Also `json.md:18-46`.) |
| `standard_library_types.md:548-564` | `date` from unix-timestamp float OK | `date_type` | missing int/float→date lax path (Rust `int_as_date`/`float_as_date`) |
| `models.md:406-453` | `datetime_parsing` msg "invalid datetime separator…" | "in ISO 8601 format" | message text mismatch in `error_types.cpp` template |

### 1c. Collection / type-mapping error codes

| Test | Expected | Got | Root cause |
|---|---|---|---|
| `validation_errors.md:1137-1152` | `iterable_type` | `list_type` | no `iterable` schema support; field fell back to list validator |
| `validation_errors.md:1158-1176` | `iteration_error` | `list_type` | generator→list must iterate and report `iteration_error` on failure (Rust `iterable.rs`) |
| `validation_errors.md:1301-1330` | `mapping_type` | `dict_type` | `mapping` schema type maps to dict validator with wrong error kind |
| `validation_errors.md:1667-1680` | `set_type` | `list_type` | set validator error kind |
| `validation_errors.md:1810-1823` | `string_unicode` | `string_type` | bytes→str with invalid UTF-8 must be `string_unicode` |
| `validation_errors.md:909-934` | `get_attribute_error` | `missing` | from_attributes: attribute lookup error kind |
| `validation_errors.md:1438-1467` | `model_attributes_type` | `missing` | from_attributes: non-object input error kind |
| `validation_errors.md:1543-1558` | `needs_python_object` | `is_subclass_of` | is-subclass from JSON input (Rust `input_json.rs` special-cases `needs_python_object`) |
| `migration.md:711-730` | `int_from_float` ("got a number with a fractional part") | `int_type` | float→int path doesn't emit `Kind::IntFromFloat` |
| `validation_errors.md:1017-1043` | 4300-digit int string validates | `int_parsing` | big-int (i128) parse width too narrow |

### 1d. Kind enum entries missing from the `Kind → name` map

`src/errors/error_types.cpp::type_name()` returns `"unknown_error"` for any Kind absent
from its map. The map ends at `StringNotAscii`; the reverse map (line ~350) proves the
Kinds exist but were never added to the forward map:

- `url_type`, `url_parsing`, `url_syntax_violation`, `url_too_long` (5 URL tests:
  `validation_errors.md:2139-2228`) — the URL validator (`include/pydantic_core/url_types.hpp`)
  only ever emits `Kind::UrlType`; it needs distinct kinds per failure class, matching
  `url.rs` in Rust.
- `uuid_type`, `uuid_parsing` (`validation_errors.md:2234-2270`) — same pattern in
  `include/pydantic_core/validators/special.hpp:437-445`.
- `enum` (`validation_errors.md:727-746`, got `unknown_error`) — enum failure kind.

### 1e. `frozen_field` vs `frozen_instance`

`fields.md:638-658`: field-level `frozen=True` must produce `frozen_field` /
"Field is frozen"; C++ emits `frozen_instance` / "Instance is frozen".

---

## 2. Union / tagged-union semantics (8 union tests + several others)

`UnionValidator` / `TaggedUnionValidator` (`include/pydantic_core/validators/complex.hpp:50-230`)
is "try left-to-right, first success wins" with a single generic failure. Rust
(`union.rs`, `tagged_union.rs`) does much more:

| Behavior | Expected (Rust) | Got (C++) | Failing tests |
|---|---|---|---|
| Smart-union exact-match preference | `int | str`, input `'1'` → **str** branch (exact type match beats coercion) | int branch coerces `'1'` → `1` | `migration.md:624-634`, `unions.md:141-170` |
| Union failure aggregation | **all** branch errors listed, per-branch loc suffix (`id.str`, `id.int`), title of outer model | one `custom_error` "No union variant matched", loc `id` | `unions.md:37-61`, `types.md:499-625`, `types.md:834-904`, `unions.md:480-569` (4 nested errors expected) |
| Tagged-union tag mismatch | `union_tag_invalid` with loc `pet.cat` + per-variant error | `custom_error` "No tagged union variant matched", loc `pet` | `unions.md:396-451`, `validation_errors.md:2081-2104` |
| Tagged-union tag missing | `union_tag_not_found` | `custom_error` | `unions.md:319-365`, `validation_errors.md:2110-2133` |
| Callable discriminator non-string tag | `union_tag_not_found` "Unable to extract tag using discriminator …" | `custom_error` "tagged-union: discriminator did not return a string" | `unions.md:319-365`, `unions.md:480-569` |
| Model branch returns **model instance** | `ThanksgivingDinner(dessert=ApplePie(...))` | `dessert={'…', '__pydantic_fields_set__': …}` (raw model-fields dict) | `unions.md:260-313`, `fields.md:593-626`, `types.md:688-828` |
| `union` schema with `choices` dict (TypeAdapter) | builds | `SchemaError: dictionary update sequence element #0 has length 4; 2 is required` | `unions.md:578-617` |
| Model branch re-validates existing instance | instance accepted as-is (loc `nested.inner` chain) | `dict_type`, "input_type=dict" | `models.md:921-952`, `models.md:976-994` |

`UnionValidator` also has the `expected_class()` fast-path only for Python input, and the
`TaggedUnionValidator` callable path requires `dynamic_cast<PythonInput*>` — JSON input is
rejected outright.

---

## 3. Datetime / tzinfo representation (6+ tests)

Docs expect pydantic-core's `TzInfo` class (Rust `src/input/datetime.rs:729-800`,
`__repr__ = "TzInfo(9000)"`) and *plain* `datetime.datetime` instances.

| Issue | Location | Failing tests |
|---|---|---|
| C++ builds tz as `datetime.timezone(timedelta(minutes=…))` — repr mismatch | `src/schema_validator.cpp:844-868`, `include/pydantic_core/validators/containers.hpp:177-191` | `why.md:340-371`, `standard_library_types.md:474-493` |
| C++ constructs the datetime via `py::module_::import("datetime").attr("datetime")(...)` — the test mocks that exact attribute (`MockedDatetime`), so results carry the **mock class** | `src/schema_validator.cpp:833-868`, `src/input/python_input.cpp:915-917` | `index.md:55-91`, `why.md:139-371`, `dataclasses.md:7-25`, `models.md:406-453` |
| `TzInfo` class already exists in the Python wrapper (`pydantic_core_cpp/__init__.py:1810-1867`) but C++ never uses it | — | both of the above |
| pytz/custom tzinfo lost: validated value gets a fixed-offset tz instead of keeping the original tzinfo | datetime extraction in `src/input/python_input.cpp:740-780` | `examples/custom_validators.md:14-95` |

Fix shape: construct datetimes via the C API (`PyDateTimeAPI`) so the mock is bypassed,
and attach `pydantic_core_cpp.TzInfo` (or a C-level equivalent) for offsets.

---

## 4. Error-message formatting / `errors()` shape (~20 "Print output changed" tests)

The Python wrapper (`pydantic_core_cpp/__init__.py`) re-parses C++ error *text*
(`_parse_errors_from_message`, `_rust_style_message`) and a `__PYDANTIC_ERRORS__` JSON
blob (`src/errors/errors.cpp:61`) to reconstruct `errors()`. Fragile and lossy:

| Issue | Evidence / failing tests |
|---|---|
| `input_value` for str lacks Python repr quotes: `input_value=abcdef` vs `input_value='abcdef'` | `config.md:11-30`, `json_schema.md:804-837`, `validators.md:326-383`, `fields.md:199-216` |
| `input_value` for containers is `list(len=5)` | `containers.hpp:48-59,243-254,375-438,534-545` — `types.md:99-144` |
| `input_type` wrong for class objects (`str` instead of `type`/`NotFoo`) | `json_schema.md:845-893`, `standard_library_types.md:1360-1391` |
| `errors()` key order: `url` must come **after** `ctx` | `errors.md:39-138`, `validation_decorator.md:277-324` |
| `url` key sometimes missing entirely | `errors.md:151-206` |
| `__PYDANTIC_ERRORS__:[…]` raw blob leaks into the rendered message | `types.md:28-48`, `standard_library_types.md:1207-1228` |
| Duplicate/extra error rows (two rows for one field) | `fields.md:199-216`, `standard_library_types.md:1207-1228` |
| Indentation: loc line indented +2 and message +4 vs Rust's loc +0 / message +2 | `fields.md:638-658`, `models.md:1483-1513` |
| Title: always `Schema` (`src/schema_validator.cpp:22`) instead of Rust's `config['title'] or validator.get_name()` (e.g. `User`, `list[User]`, `constrained-int`, `repeat`, `Response[int]`, `OuterT[int]`) | `types.md:28-144`, `validation_decorator.md:202-372`, `type_adapter.md:15-45`, `models.md:406-952`, `config.md:56-90`, `performance.md:192-207`, `json.md:18-46`, `strict_mode.md:62-82`, `forward_annotations.md:58-143` |
| Generic model name in errors: `Response` vs `Response[int]` | `models.md:708-743` — generic `model_name` not propagated |
| Long `input_value` truncation (Rust truncates dict repr with `…`) | `errors.md:39-138` |
| `recursion_loop` `input_value` is the error text, not the input repr | `forward_annotations.md:58-84`, `forward_annotations.md:90-143` |

Structural note: many of these would disappear if C++ built the `errors()` payload
natively (structured list of dicts with `type/loc/msg/input/url/ctx`) instead of the
text-reparse bridge.

---

## 5. Custom errors / validator-exception mapping (4 tests)

- `PydanticCustomError('the_answer_error', …)` raised in a validator must keep its
  **custom error type**; C++ maps every non-ValueError/AssertionError exception to
  `value_error`/`custom_error` with a `Value error, ` prefix
  (`functions.hpp::function_error_from_exception`). Failing: `validators.md:563-593`,
  `standard_library_types.md:1055-1079`.
- `{message}` placeholder not interpolated for custom errors:
  `validators.md:836-856` (`Model(name=None)` prints literal `{message}`).
- `TypeError` raised in a `field_validator` must propagate as `TypeError` (V1→V2
  migration guarantee), C++ converts it: `migration.md:445-461`
  (`DID NOT RAISE <class 'TypeError'>`).

---

## 6. Missing / broken schema types (5 tests, SchemaError at build)

| Schema type | Failing tests | Notes |
|---|---|---|
| `complex` | `validation_errors.md:207-241` | no `ComplexValidator`; needs `complex_type`/`complex_str_parsing` (Rust `complex.rs`) |
| `missing-sentinel` | `validation_errors.md:1418-1432` | `MissingSentinel` type (`pydantic.missing`); sentinel default also breaks pickling (`experimental.md:501-526` "Cannot pickle 'Sentinel' object") |
| `arguments-v3` | `experimental.md:430-492` | experimental arguments schema; only `arguments` (v2) exists in C++ (`combined_validator.cpp:2095+`) |
| `iterable` / `mapping` | see §1c | see §1c |

---

## 7. Validation semantics gaps (~15 tests)

| Gap | Evidence / tests |
|---|---|
| `ValidationInfo.config` is `None` — `make_validation_info` (`functions.hpp:76-104`) never sets `config` | `migration.md:415-433` |
| `coerce_numbers_to_str` config ignored (int→str for `list[str]`) | `config.md:83-90` (only a comment mentions it, `python_input.cpp:321`) |
| `str_to_lower` leaks into nested models (config boundary: parent config must NOT apply to a child model's fields) | `config.md:194-210` — `combined_validator.cpp:1273-1284` applies `cfg_bool("str_to_lower")` unconditionally |
| `alias_generator` / `AliasChoices` / `AliasPath` unsupported — `validation_alias` only parsed as plain string | `alias.md:27-97` (`combined_validator.cpp:696-698, 807-809, 1970-1973`); `alias.md:298-311` (`serialize_by_alias` also ignored in dump) |
| `validate_call` default values re-validated through the value schema, coercing `b''` → `''` (bytes default decoded via `py_default_to_json_str` round-trip, `combined_validator.cpp:19-38`) | `validation_decorator.md:13-40, 254-271` (`TypeError: sequence item 0: expected str instance, bytes found`) |
| Strict mode: field-level `strict=False` inside a strict model ignored | `strict_mode.md:163-176` |
| Strict JSON: strings allowed for date (see §1b) | `strict_mode.md:62-82`, `json.md:18-46` |
| bytes→int lax coercion missing (`X(x=b'1')`) | `dataclasses.md:185-219` |
| `Model(p=('1', 2))` — tuple-of-str for `tuple[str, int]`-like field rejected | `standard_library_types.md:891-910` (`arguments_type` from wrong validator) |
| `Model(f=[1, 2])` — int→str lax for constrained str fails | `standard_library_types.md:1207-1228` |
| `CookingModel(tool=2)` — int→str lax for `str` field fails | `standard_library_types.md:717-751` |
| URL: `str(AnyUrl('https://google.com'))` must be `'https://google.com/'` (trailing slash for schemes with empty path) | `migration.md:855-862` — `url_types.cpp` repr |
| `from_json` (partial JSON) delegates to **Rust** .so which isn't installed → `ImportError` | `json.md:63-184` — `pydantic_core_cpp/__init__.py:1585-1586`; `experimental_allow_partial` for `validate_json` likewise falls through to `json.loads` (`experimental.md:187-415` — 5 tests, `JSONDecodeError` on truncated JSON) |
| `FailFast` list: expected single first-error; C++ collects all | `performance.md:192-207` (also list-fail-fast: Rust list validator stops at first invalid item) |
| `experimental.md:398-415`: partial list `[20,30,4]` vs `Ge(10)` — partial semantics | see above |

---

## 8. Serialization gaps (7 tests)

| Gap | Evidence / tests |
|---|---|
| `model_dump_json(indent=2)` — `SchemaSerializer.to_json` path drops `indent` (standalone `to_json` supports it, `main_module.cpp:887`) | `serialization.md:100-129` |
| `model_dump(context=…)` — context never reaches `field_serializer` callables | `serialization.md:502-523` |
| `serialize_as_any=True` — model's field serializers applied where a plain dump is expected (and vice versa) | `serialization.md:710-779` |
| TypeAdapter dump of a model leaks `__pydantic_fields_set__`/`__pydantic_defaults__` keys (model not materialized to instance; serializer sees the raw dict) | `type_adapter.md:15-45`, `standard_library_types.md:1158-183` |
| `list[Item]` items are raw model-fields dicts, not instances | `type_adapter.md:68-84` |
| `computed_field` (`volume`) missing from `model_dump()` | `fields.md:879-897` |
| Generic TypeVar field serialized with bound schema drops extra attrs of the concrete subclass (`details` missing `bar`) | `models.md:1119-1166`, `models.md:1205-1260`; related crash: `models.md:1170-1198` `AttributeError: 'BaseModel' object has no attribute '__private_attributes__'` — `_build_model` (`__init__.py:1110`) does `object.__new__(BaseModel)` + `instance.__dict__ = data` for a TypeVar bound to `BaseModel` |
| Tuple-valued dict entry serialized as tuple in python mode where list is expected (list validator preserves input tuple instead of building a list) | `serialization.md:83-86` |

---

## 9. Suggested fix order (by impact / effort)

**P0 — cheap, high-impact (error codes & text)**
1. Complete the `Kind → name` forward map in `error_types.cpp` (url_*, uuid_*, enum,
   time_delta_*, complex_*, iterable_*, mapping_*, string_unicode, needs_python_object,
   model_attributes_type, get_attribute_error, …) — fixes ~15 tests immediately.
2. Fix lax-coercion error kinds in `python_input.cpp` / `json_input.cpp` / `string_input.cpp`
   (bool/decimal/float→int/date/datetime/timedelta), including the Rust-accepted token
   sets — fixes ~8 tests.
3. Error text: Python-repr for `input_value` (str quotes, container reprs instead of
   `list(len=N)`), correct `input_type`, `url` key placement, no `__PYDANTIC_ERRORS__`
   leak, indentation, `frozen_field` vs `frozen_instance` — fixes ~20 tests.
4. Title: replace hard-coded `"Schema"` with `config['title']` or a formatted
   validator name (`list[User]`, model `model_name`, generic `Response[int]`).

**P1 — semantics**
5. Union: smart-union exact-match preference, per-branch error aggregation with loc
   suffixes, tagged-union `union_tag_invalid`/`union_tag_not_found` + per-branch errors,
   model-branch materialization (instance, not dict), `union` with `choices`.
6. Datetime: C-API construction (no mock leak) + `TzInfo` class for offsets + preserve
   original tzinfo for datetime inputs + JSON-strict string dates + float/int→date.
7. `make_validation_info`: add `config` (and audit other `ValidationInfo` attrs).
8. Custom errors: propagate `PydanticCustomError` type/message/ctx; interpolate
   `{message}`; propagate `TypeError` from validators unchanged.
9. Missing schema types: `complex`, `missing-sentinel`, `iterable`, `mapping`,
   `arguments-v3`.

**P2 — config & aliases**
10. Config boundaries (`str_to_lower` must not cross model boundaries),
    `coerce_numbers_to_str`, field-level `strict` overrides, `validate_default`/default
    fidelity (bytes default `b''` must stay bytes).
11. `AliasChoices`/`AliasPath`/`alias_generator` parsing in model-fields +
    `serialize_by_alias` in serializer.

**P3 — serialization & partial JSON**
12. `indent` in SchemaSerializer, `context` in field serializers, `serialize_as_any`,
    computed fields, TypeVar/generic serialization fidelity, list materialization.
13. Native `from_json` + `experimental_allow_partial` (replace the Rust .so fallback).
14. URL trailing-slash repr.

---

## 10. Test → root-cause map (all 121)

| Failing test | Group |
|---|---|
| `migration.md:415-433` | §7 ValidationInfo.config |
| `migration.md:445-461` | §5 TypeError propagation |
| `migration.md:624-634` | §2 smart union |
| `migration.md:711-730` | §1b int_from_float |
| `migration.md:855-862` | §7 URL trailing slash |
| `index.md:55-91`, `why.md:139-371` (×4) | §3 MockedDatetime/TzInfo |
| `examples/custom_validators.md:14-95` | §3 tzinfo preservation |
| `json_schema.md:804-837`, `845-893` | §4 input_value repr / input_type |
| `unions.md` (×8) | §2 union semantics |
| `type_adapter.md:15-45`, `68-84` | §8 model materialization |
| `serialization.md:83-86`, `100-129`, `502-523`, `710-779` (×2) | §8 serialization |
| `json.md:18-46` | §1b/§7 JSON-strict date |
| `json.md:63-184` (×4) | §7 from_json |
| `dataclasses.md:7-25` | §3 MockedDatetime |
| `dataclasses.md:185-219` | §7 bytes→int |
| `forward_annotations.md:58-84`, `90-143` | §4 recursion error input_value |
| `performance.md:192-207` | §1a/§7 fail-fast |
| `experimental.md:187-415` (×5) | §7 partial JSON |
| `experimental.md:430-492` (×2) | §6 arguments-v3 |
| `experimental.md:501-526` | §6 Sentinel |
| `config.md:11-30` | §4 str repr |
| `config.md:56-76` | §4 title |
| `config.md:83-90` | §7 coerce_numbers_to_str |
| `config.md:194-210` | §7 config boundary |
| `alias.md:27-97` (×3) | §7 aliases |
| `alias.md:298-311` | §8 serialize_by_alias |
| `fields.md:199-216` | §4 dup rows/repr |
| `fields.md:593-626` | §2 model materialization |
| `fields.md:638-658` | §1e/§4 frozen + indent |
| `fields.md:879-897` | §8 computed fields |
| `models.md:406-453` | §3/§1b datetime + title |
| `models.md:708-743` | §4 generic title |
| `models.md:921-952`, `976-994` | §2 nested generic models |
| `models.md:1119-1166`, `1205-1260` | §8 TypeVar serialization |
| `models.md:1170-1198` | §8 `_build_model` crash |
| `models.md:1483-1513` | §1e/§4 frozen + indent |
| `validation_decorator.md:13-40`, `254-271` | §7 validate_call bytes default |
| `validation_decorator.md:202-231`, `277-324`, `337-372` | §4 title/url order |
| `strict_mode.md:62-82` | §1b JSON-strict date |
| `strict_mode.md:163-176` | §7 field strict override |
| `types.md:28-48` | §4 title/`__PYDANTIC_ERRORS__` |
| `types.md:99-144` | §4 list(len)/title |
| `types.md:499-625` | §2 union per-branch errors |
| `types.md:688-828` | §2 model materialization + title |
| `types.md:834-904` | §2 union per-branch errors |
| `validators.md:326-383` (×2) | §4 str repr |
| `validators.md:563-593` | §5 custom error type |
| `validators.md:836-856` | §5 {message} |
| `standard_library_types.md:474-493` | §3 TzInfo |
| `standard_library_types.md:548-564` | §1b float→date |
| `standard_library_types.md:717-751` | §7 int→str lax |
| `standard_library_types.md:891-910` | §7 tuple args |
| `standard_library_types.md:1055-1079` | §5 custom error type |
| `standard_library_types.md:1158-1183` | §8 model materialization |
| `standard_library_types.md:1207-1228` | §7 int→str lax |
| `standard_library_types.md:1360-1391` | §4 input_type |
| `errors.md:39-138` | §4 truncation/url order |
| `errors.md:151-206` | §4 url key |
| `validation_errors.md:60-75` | §1a bool_parsing |
| `validation_errors.md:207-241` (×2) | §6 complex |
| `validation_errors.md:327-342` | §1b date_from_datetime_parsing |
| `validation_errors.md:435-451` | §1b datetime_from_date_parsing |
| `validation_errors.md:478-500` | §1b custom tzinfo |
| `validation_errors.md:614-629` | §1a decimal_parsing |
| `validation_errors.md:680-702` | §7 default_factory sees partial data (`KeyError: 'a'`) |
| `validation_errors.md:727-746` | §1d enum |
| `validation_errors.md:909-934` | §1c get_attribute_error |
| `validation_errors.md:1017-1043` | §1c big int |
| `validation_errors.md:1137-1176` (×2) | §1c iterable |
| `validation_errors.md:1301-1330` | §1c mapping |
| `validation_errors.md:1418-1432` | §6 missing-sentinel |
| `validation_errors.md:1438-1467` | §1c model_attributes_type |
| `validation_errors.md:1543-1558` | §1c needs_python_object |
| `validation_errors.md:1667-1680` | §1c set_type |
| `validation_errors.md:1810-1823` | §1c string_unicode |
| `validation_errors.md:1829-1865` (×2) | §1b time_delta_* |
| `validation_errors.md:2081-2133` (×2) | §2 tagged-union tags |
| `validation_errors.md:2139-2228` (×4) | §1d url_* |
| `validation_errors.md:2234-2270` (×2) | §1d uuid_* |
