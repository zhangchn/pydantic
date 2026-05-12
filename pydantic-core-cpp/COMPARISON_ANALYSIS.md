# 1:1 Match Analysis: C++ vs Rust (pydantic-core)

**Date**: 2026-05-12  
**Rust baseline**: pydantic-core/src (77 .rs files)  
**C++ implementation**: pydantic-core-cpp/ (11 .hpp validators, 3 .hpp serializers + main_module.cpp)

---

## 1. Validators (CombinedValidator enum)

### Rust validators from `src/validators/mod.rs` (55 variants)

| # | Rust Variant | Source File | C++ Status | C++ Impl File | Notes |
|---|-------------|-------------|------------|---------------|-------|
| 1 | `TypedDict` | `typed_dict.rs` | ✅ Implemented | `model_fields.hpp` | Via `TypedDictValidator` (inherits `ModelFieldsValidator`) |
| 2 | `Union` | `union.rs` | ✅ Implemented | `special.hpp` | `UnionValidator` |
| 3 | `TaggedUnion` | `union.rs` | ✅ Implemented | `special.hpp` | `TaggedUnionValidator` |
| 4 | `Nullable` | `nullable.rs` | ✅ Implemented | `special.hpp` | `NullableValidator` |
| 5 | `Model` | `model.rs` | ✅ Implemented | `model_fields.hpp` | `ModelValidator` |
| 6 | `ModelFields` | `model_fields.rs` | ✅ Implemented | `model_fields.hpp` | `ModelFieldsValidator` |
| 7 | `DataclassArgs` | `dataclass.rs` | ✅ Implemented | `model_fields.hpp` | `DataclassArgsValidator` |
| 8 | `Dataclass` | `dataclass.rs` | ✅ Implemented | `model_fields.hpp` | `DataclassValidator` |
| 9 | `Str` | `string.rs` | ✅ Implemented | `basic.hpp` | `StringValidator` |
| 10 | `StrConstrained` | `string.rs` | ❌ Missing | — | No length/min/max/strip_whitespace/regex constraints |
| 11 | `Int` | `int.rs` | ✅ Implemented | `basic.hpp` | `IntValidator` |
| 12 | `ConstrainedInt` | `int.rs` | ❌ Missing | — | No gt/ge/lt/le/multiple_of constraints |
| 13 | `Bool` | `bool.rs` | ✅ Implemented | `basic.hpp` | `BoolValidator` |
| 14 | `Float` | `float.rs` | ✅ Implemented | `basic.hpp` | `FloatValidator` |
| 15 | `ConstrainedFloat` | `float.rs` | ❌ Missing | — | No gt/ge/lt/le/multiple_of constraints |
| 16 | `Decimal` | `decimal.rs` | ❌ Missing | — | No Python decimal.Decimal support |
| 17 | `List` | `list.rs` | ✅ Implemented | `containers.hpp` | `ListValidator` (no items_schema validation yet) |
| 18 | `Set` | `set.rs` | ✅ Implemented | `containers.hpp` | `SetValidator` |
| 19 | `Tuple` | `tuple.rs` | ✅ Implemented | `containers.hpp` | `TupleValidator` (no positional validation) |
| 20 | `Dict` | `dict.rs` | ✅ Implemented | `containers.hpp` | `DictValidator` (no key/value schema) |
| 21 | `None` | `none.rs` | ✅ Implemented | `special.hpp` | `NoneValidator` |
| 22 | `FunctionBefore` | `function.rs` | ✅ Implemented | `functions.hpp` | `FunctionBeforeValidator` |
| 23 | `FunctionAfter` | `function.rs` | ✅ Implemented | `functions.hpp` | `FunctionAfterValidator` |
| 24 | `FunctionPlain` | `function.rs` | ✅ Implemented | `functions.hpp` | `FunctionPlainValidator` (stub) |
| 25 | `FunctionWrap` | `function.rs` | ✅ Implemented | `functions.hpp` | `FunctionWrapValidator` |
| 26 | `FunctionCall` | `call.rs` | ❌ Missing | — | No call/argument validation |
| 27 | `Literal` | `literal.rs` | ✅ Implemented | `special.hpp` | `LiteralValidator` |
| 28 | `MissingSentinel` | `missing_sentinel.rs` | ❌ Missing | — | No MISSING sentinel type |
| 29 | `IntEnum` | `enum_.rs` | ⚠️ Partial | `special.hpp` | Generic `EnumValidator`, no int-specific coercion |
| 30 | `StrEnum` | `enum_.rs` | ⚠️ Partial | `special.hpp` | Generic `EnumValidator`, no str-specific coercion |
| 31 | `FloatEnum` | `enum_.rs` | ⚠️ Partial | `special.hpp` | Generic `EnumValidator`, no float-specific coercion |
| 32 | `PlainEnum` | `enum_.rs` | ⚠️ Partial | `special.hpp` | Generic `EnumValidator` |
| 33 | `Any` | `any.rs` | ✅ Implemented | `special.hpp` | `AnyValidator` |
| 34 | `Bytes` | `bytes.rs` | ✅ Implemented | `basic.hpp` | `BytesValidator` (no min/max length constraints) |
| 35 | `ConstrainedBytes` | `bytes.rs` | ❌ Missing | — | No length constraints |
| 36 | `Date` | `date.rs` | ✅ Implemented | `basic.hpp` | `DateValidator` |
| 37 | `Time` | `time.rs` | ✅ Implemented | `basic.hpp` | `TimeValidator` |
| 38 | `Datetime` | `datetime.rs` | ✅ Implemented | `basic.hpp` | `DatetimeValidator` |
| 39 | `FrozenSet` | `frozenset.rs` | ✅ Implemented | `containers.hpp` | `FrozenSetValidator` |
| 40 | `Timedelta` | `timedelta.rs` | ✅ Implemented | `basic.hpp` | `TimedeltaValidator` |
| 41 | `IsInstance` | `is_instance.rs` | ❌ Missing | — | No isinstance check |
| 42 | `IsSubclass` | `is_subclass.rs` | ❌ Missing | — | No issubclass check |
| 43 | `Callable` | `callable.rs` | ❌ Missing | — | No callable check |
| 44 | `Arguments` | `arguments.rs` | ❌ Missing | — | No argument validation (positional+keyword) |
| 45 | `ArgumentsV3` | `arguments_v3.rs` | ❌ Missing | — | New argument validation |
| 46 | `WithDefault` | `with_default.rs` | ✅ Implemented | `special.hpp` | `WithDefaultValidator` |
| 47 | `Chain` | `chain.rs` | ✅ Implemented | `special.hpp` | `ChainValidator` |
| 48 | `LaxOrStrict` | `lax_or_strict.rs` | ✅ Implemented | `special.hpp` | `LaxOrStrictValidator` |
| 49 | `Generator` | `generator.rs` | ❌ Missing | — | No generator validation |
| 50 | `CustomError` | `custom_error.rs` | ❌ Missing | — | No custom error wrapper |
| 51 | `Json` | `json.rs` | ✅ Implemented | `combined_validator.cpp` | `JsonValidator` |
| 52 | `Url` | `url.rs` | ✅ Implemented | `basic.hpp` | `UrlValidator` |
| 53 | `MultiHostUrl` | `url.rs` | ❌ Missing | — | No multi-host URL support |
| 54 | `Uuid` | `uuid.rs` | ✅ Implemented | `basic.hpp` | `UuidValidator` |
| 55 | `DefinitionRef` | `definitions.rs` | ⚠️ Partial | `combined_validator.cpp` | Stub (returns AnyValidator) |
| 56 | `JsonOrPython` | `json_or_python.rs` | ✅ Implemented | `special.hpp` | `JsonOrPythonValidator` |
| 57 | `Complex` | `complex.rs` | ✅ Implemented | `complex.hpp` | `ComplexValidator` |
| 58 | `Prebuilt` | `prebuilt.rs` | ❌ Missing | — | No prebuilt validator reuse |

### Validator Summary

| Status | Count | Percentage |
|--------|-------|------------|
| ✅ Fully implemented | 30 | 52% |
| ⚠️ Partially implemented | 5 | 9% |
| ❌ Missing | 23 | 39% |

**Missing critical validators**: Decimal, ConstrainedInt/Float/Str/Bytes, FunctionCall, Arguments, IsInstance, IsSubclass, Callable, Generator, CustomError, MissingSentinel, MultiHostUrl, DefinitionRef (stub), Prebuilt

---

## 2. Serializers (CombinedSerializer enum)

### Rust serializers from `src/serializers/shared.rs` (38 variants)

| # | Rust Variant | Source File | C++ Status | Notes |
|---|-------------|-------------|------------|-------|
| 1 | `None` | `simple.rs` | ✅ Implemented | `main_module.cpp` |
| 2 | `Nullable` | `nullable.rs` | ✅ Implemented | `main_module.cpp` |
| 3 | `Int` | `simple.rs` | ✅ Implemented | `main_module.cpp` |
| 4 | `Bool` | `simple.rs` | ✅ Implemented | `main_module.cpp` |
| 5 | `Float` | `float.rs` | ✅ Implemented | `main_module.cpp` |
| 6 | `Decimal` | `decimal.rs` | ❌ Missing | No decimal serialization |
| 7 | `Str` | `string.rs` | ✅ Implemented | `main_module.cpp` |
| 8 | `Bytes` | `bytes.rs` | ✅ Implemented | `main_module.cpp` (base64) |
| 9 | `Datetime` | `datetime_etc.rs` | ✅ Implemented | `main_module.cpp` |
| 10 | `TimeDelta` | `timedelta.rs` | ✅ Implemented | `main_module.cpp` |
| 11 | `Date` | `datetime_etc.rs` | ✅ Implemented | `main_module.cpp` |
| 12 | `Time` | `datetime_etc.rs` | ✅ Implemented | `main_module.cpp` |
| 13 | `List` | `list.rs` | ✅ Implemented | `main_module.cpp` |
| 14 | `Set` | `set_frozenset.rs` | ✅ Implemented | `main_module.cpp` |
| 15 | `FrozenSet` | `set_frozenset.rs` | ✅ Implemented | `main_module.cpp` |
| 16 | `Generator` | `generator.rs` | ✅ Implemented | `main_module.cpp` (stub) |
| 17 | `Dict` | `dict.rs` | ✅ Implemented | `main_module.cpp` |
| 18 | `Model` | `model.rs` | ✅ Implemented | `main_module.cpp` (delegates to child) |
| 19 | `Dataclass` | `dataclass.rs` | ✅ Implemented | `main_module.cpp` |
| 20 | `Url` | `url.rs` | ✅ Implemented | `main_module.cpp` |
| 21 | `MultiHostUrl` | `url.rs` | ❌ Missing | No multi-host URL serialization |
| 22 | `Uuid` | `uuid.rs` | ✅ Implemented | `main_module.cpp` |
| 23 | `Any` | `any.rs` | ✅ Implemented | `main_module.cpp` |
| 24 | `Format` | `format.rs` | ✅ Implemented | `main_module.cpp` |
| 25 | `ToString` | `format.rs` | ✅ Implemented | `main_module.cpp` |
| 26 | `WithDefault` | `with_default.rs` | ✅ Implemented | `main_module.cpp` |
| 27 | `Json` | `json.rs` | ✅ Implemented | `main_module.cpp` |
| 28 | `JsonOrPython` | `json_or_python.rs` | ✅ Implemented | `main_module.cpp` |
| 29 | `Union` | `union.rs` | ✅ Implemented | `main_module.cpp` |
| 30 | `TaggedUnion` | `union.rs` | ✅ Implemented | `main_module.cpp` |
| 31 | `Literal` | `literal.rs` | ✅ Implemented | `main_module.cpp` |
| 32 | `MissingSentinel` | `missing_sentinel.rs` | ❌ Missing | No MISSING handling |
| 33 | `Enum` | `enum_.rs` | ✅ Implemented | `main_module.cpp` |
| 34 | `Recursive` | `definitions.rs` | ✅ Implemented | `main_module.cpp` (definition-ref) |
| 35 | `Tuple` | `tuple.rs` | ✅ Implemented | `main_module.cpp` |
| 36 | `Complex` | `complex.rs` | ✅ Implemented | `main_module.cpp` |
| 37 | `TypedDict` | `typed_dict.rs` | ✅ Implemented | `main_module.cpp` |
| 38 | `Function` (plain) | `function.rs` | ✅ Implemented | `main_module.cpp` |
| 39 | `FunctionWrap` | `function.rs` | ✅ Implemented | `main_module.cpp` |

### Serializer-only (enum_only, not directly built by type)

| # | Rust Variant | C++ Status | Notes |
|---|-------------|------------|-------|
| 1 | `Fields` (GeneralFieldsSerializer) | ❌ Missing | No include/exclude filtering, no alias mapping, no extra/omit support |
| 2 | `Prebuilt` | ❌ Missing | No prebuilt serializer reuse |
| 3 | `PolymorphismTrampoline` | ❌ Missing | No polymorphic serialization |
| 4 | `ComputedFields` | ❌ Missing | No computed field serialization |

### Serializer-only (find_only builders)

| # | Rust Builder | C++ Status | Notes |
|---|-------------|------------|-------|
| 1 | `ChainBuilder` | ⚠️ | Chain schema type exists, but serializer handling is in `build_ser` |
| 2 | `CustomErrorBuilder` | ❌ | No custom error serialization |
| 3 | `CallBuilder` | ❌ | No call serialization |
| 4 | `LaxOrStrictBuilder` | ⚠️ | Schema exists, serializer handling limited |
| 5 | `ArgumentsBuilder` | ❌ | No arguments serialization |
| 6 | `IsInstanceBuilder` | ❌ | No is_instance serialization |
| 7 | `IsSubclassBuilder` | ❌ | No is_subclass serialization |
| 8 | `CallableBuilder` | ❌ | No callable serialization |
| 9 | `DefinitionsSerializerBuilder` | ⚠️ | definitions/definition-ref handled but basic |
| 10 | `DataclassArgsBuilder` | ⚠️ | dataclass-args handled |
| 11 | `FunctionBefore/After/Plain/WrapSerializerBuilder` | ✅ | All handled in `build_ser` |
| 12 | `ModelFieldsBuilder` | ⚠️ | model-fields handled but no computed fields |

### Serializer Summary

| Status | Count | Percentage |
|--------|-------|------------|
| ✅ Implemented (CombinedSerializer enum) | 36 | 95% |
| ❌ Missing (CombinedSerializer enum) | 2 | 5% |
| ❌ Missing (enum_only infrastructure) | 4 | — |
| ❌ Missing (find_only builders) | 7 | — |

---

## 3. Input System

### Rust input types from `src/input/`

| # | Rust Input | Source File | C++ Status | Notes |
|---|-----------|-------------|------------|-------|
| 1 | `Input` (trait) | `input_abstract.rs` | ❌ N/A | C++ uses JSON string as input format |
| 2 | `JsonInput` | `input_json.rs` | ✅ Implemented | Via simdjson + `pyobj_to_json_str()` |
| 3 | `PythonInput` | `input_python.rs` | ⚠️ Partial | Converted to JSON via Python `json.dumps()` |
| 4 | `StringInput` | `input_string.rs` | ❌ Missing | No `validate_strings` / `StringMapping` |
| 5 | `DateTime` parsing | `datetime.rs` | ⚠️ Partial | Basic datetime parsing via simdjson |
| 6 | `ReturnEnums` | `return_enums.rs` | ❌ Missing | No ArgKwargs, ValidationMatch, etc. |
| 7 | `Shared` helpers | `shared.rs` | ⚠️ Partial | Basic type coercion in validators |

### Input Summary

| Status | Notes |
|--------|-------|
| ✅ JSON input | Via simdjson parsing |
| ⚠️ Python input | Via `json.dumps()` conversion (lossy for complex objects) |
| ❌ String input | No validate_strings path |
| ❌ Python native input | No direct PyObject validation (all goes through JSON round-trip) |

---

## 4. Test Coverage Comparison

### C++ Tests (doctest) — 6 test suites, 282 tests

| Test File | Tests | Coverage |
|-----------|-------|----------|
| `test_validators.cpp` | 86 | Basic validator construction, simple validation |
| `test_serializers.cpp` | 68 | Basic serializer construction, serialization |
| `test_integration.cpp` | 44 | SchemaBuilder + SchemaValidator integration |
| `test_model_fields.cpp` | 47 | Model/TypedDict/Dataclass validation |
| `test_validation_state.cpp` | 18 | ValidationState, recursion guard, config |
| `test_errors.cpp` | 9 | SchemaError, ValidationError, ValError |
| `test_input.cpp` | 10 | JSON input parsing |
| **Total** | **282** | |

### Rust/Python Tests — 119 test files

| Category | Test Files | Approx. Tests | C++ Coverage |
|----------|-----------|---------------|--------------|
| `test_*.py` (validators) | ~55 | ~600+ | ~15% (basic validation only) |
| `test_*.py` (serializers) | ~25 | ~400+ | ~10% (basic serialization only) |
| `test_*.py` (edge cases) | ~20 | ~300+ | ~0% (no edge case coverage) |
| `test_*.py` (errors) | ~8 | ~100+ | ~5% (basic error types only) |
| `test_*.py` (config/misc) | ~11 | ~200+ | ~0% |
| **Total** | **119** | **~1600+** | **~5-10%** |

### Specific Test Gaps

#### Validators — Missing Test Depth

| Gap Area | Rust Tests | C++ Status |
|----------|-----------|------------|
| **Strict/lax mode matrices** | ~200 tests | ❌ Not tested |
| **Constraint validation** (gt/lt/ge/le/multiple_of) | ~80 tests | ❌ Not implemented |
| **String constraints** (min/max length, pattern, strip_whitespace, to_lower, etc.) | ~60 tests | ❌ Not implemented |
| **Function validators** (before/after/wrap/plain with Python callables) | ~46 tests | ⚠️ Stubs only |
| **Arguments validation** (positional + keyword) | ~31 tests | ❌ Not implemented |
| **Recursive/definition schemas** | ~41 tests | ⚠️ Stub (AnyValidator) |
| **Error message formatting** (exact messages, traceback, context) | ~39 tests | ❌ Not implemented |
| **Extra behavior** (forbid/allow/ignore) edge cases | ~25 tests | ⚠️ Basic only |
| **Union/tagged_union** (smart union, custom init) | ~60 tests | ⚠️ Basic only |
| **Model validation** (from_attributes, validate_assignment, root validators, computed fields) | ~148 tests | ⚠️ Basic only |
| **TypedDict** (typed_dict, total=False, extras) | ~30 tests | ⚠️ Basic only |
| **Dataclass** (slots, kw_only, init_only, frozen) | ~25 tests | ⚠️ Partial |

#### Serializers — Missing Test Depth

| Gap Area | Rust Tests | C++ Status |
|----------|-----------|------------|
| **include/exclude filtering** | ~40 tests | ❌ Not implemented |
| **by_alias serialization** | ~20 tests | ❌ Not implemented |
| **exclude_unset/exclude_defaults/exclude_none** | ~30 tests | ❌ Not implemented |
| **format specifiers** (date formats, float precision) | ~25 tests | ❌ Not implemented |
| **round_trip mode** | ~15 tests | ❌ Not implemented |
| **serialize_as_any / fallback** | ~20 tests | ❌ Not implemented |
| **indent / ensure_ascii** | ~10 tests | ⚠️ Basic only |
| **computed fields serialization** | ~15 tests | ❌ Not implemented |
| **warnings handling** | ~10 tests | ❌ Not implemented |
| **polymorphic serialization** | ~12 tests | ❌ Not implemented |
| **bytes_mode (utf8/base64)** | ~8 tests | ⚠️ Basic only |
| **timedelta_mode (iso8601/float)** | ~6 tests | ❌ Not implemented |

#### N/A Tests (Python-specific, not applicable to C++)

| Category | Count | Reason |
|----------|-------|--------|
| Pickling / `__reduce__` | ~12 | Python-specific |
| Garbage collection / `__traverse__` | ~8 | Python-specific |
| `isinstance` checks | ~15 | Python-specific |
| `__call__` behavior | ~5 | Python-specific |
| Type hints / stubs | ~10 | Python-specific |
| Hypothesis fuzzing | ~20 | Python-specific |
| **Total N/A** | **~70** | |

---

## 5. Key Architectural Differences

| Aspect | Rust pydantic-core | C++ pydantic-core-cpp |
|--------|-------------------|----------------------|
| **Input format** | Native PyObject or JSON | JSON-only (Python objects serialized via json.dumps) |
| **Type system** | Strongly typed Rust enums | `shared_ptr<Validator>` + `shared_ptr<void>` result type |
| **JSON parsing** | jiter (fast, custom) | simdjson (fast, but schema parsed at runtime) |
| **Schema parsing** | Python dict → Rust via pyo3 | JSON string → simdjson → C++ objects |
| **Recursion guard** | Rust struct with depth limit | `RecursionGuard` class |
| **Error handling** | `ValResult<T>` / `ValError` | `ValError` + `ValidationResult` |
| **Serialization** | serde_json (zero-copy) | Python json module + manual JSON building |
| **Build system** | maturin (Cargo.toml) | scikit-build-core (CMakeLists.txt) |
| **Python bindings** | pyo3 | pybind11 |
| **Definitions/recursion** | Full support | Stub (returns AnyValidator) |
| **String caching** | StringCacheMode | Not implemented |
| **Prebuilt validators** | Schema reuse | Not implemented |
| **Polymorphic serialization** | PolymorphismTrampoline | Not implemented |
| **Computed fields** | Full support | Not implemented |
| **Filter (include/exclude)** | Full support | Not implemented |

---

## 6. Priority Recommendations

### P0 — Critical for Pydantic Compatibility
1. **Implement DefinitionRef** — Recursive schemas (self-referencing models) are a core Pydantic feature
2. **Implement ConstrainedInt/Float/Str/Bytes** — Used extensively in Pydantic for field constraints
3. **Implement Arguments validator** — Required for function/callable validation
4. **Implement validate_strings** — StringMapping input type used by Pydantic
5. **Implement is_instance / is_subclass validators** — Used by `InstanceOf` and type validation

### P1 — High Impact
6. **Implement include/exclude filtering** — Critical for `model_dump()` customization
7. **Implement by_alias serialization** — Required for alias support
8. **Implement exclude_unset/exclude_defaults/exclude_none** — Model dump options
9. **Implement Function validators** (proper Python callable support) — Required for custom validators
10. **Implement Generator validator** — Used for generator/iterator fields

### P2 — Medium Impact
11. **Implement Decimal support** — For financial/precision-critical data
12. **Implement computed fields** — For `@computed_field` decorator
13. **Implement polymorphic serialization** — For inheritance-based serialization
14. **Implement MultiHostUrl** — For multi-host URL types
15. **Implement CustomError** — For `PydanticCustomError`

### P3 — Low Priority
16. **Implement Prebuilt validators** — Performance optimization
17. **Implement StringCacheMode** — Micro-optimization
18. **Implement validate_assignment** — For `validate_assignment=True` config
19. **Implement format specifiers** — For custom date/float formatting
20. **Implement ensure_ascii / indent** — JSON formatting options

---

## 7. Verdict

The C++ implementation covers **~52% of Rust validators** and **~95% of serializer types** (though most are stubs with basic functionality). The **actual functional coverage** is closer to **15-20%** when accounting for missing constraints, filtering, recursion support, and Python-native features.

**Biggest gaps:**
1. **No native PyObject validation** — Everything goes through JSON round-trip, which loses type information
2. **No recursion/definitions support** — Self-referencing models don't work
3. **No constraint validation** — gt/lt/ge/le/multiple_of/regex/length constraints missing
4. **No filtering** — include/exclude/by_alias not implemented for serialization
5. **No string input path** — `validate_strings` missing entirely
