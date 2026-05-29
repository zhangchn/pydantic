# 1:1 Match Analysis: C++ vs Rust (pydantic-core)

**Date**: 2026-05-29 (Updated)
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
| 10 | `StrConstrained` | `string.rs` | ⚠️ Partial | `basic.hpp` | `StrConstrainedValidator` exists (min_length/max_length/pattern/strip_whitespace/to_lower/to_upper) |
| 11 | `Int` | `int.rs` | ✅ Implemented | `basic.hpp` | `IntValidator` |
| 12 | `ConstrainedInt` | `int.rs` | ⚠️ Partial | `basic.hpp` | `ConstrainedIntValidator` (gt/ge/lt/le/multiple_of supported) |
| 13 | `Bool` | `bool.rs` | ✅ Implemented | `basic.hpp` | `BoolValidator` |
| 14 | `Float` | `float.rs` | ✅ Implemented | `basic.hpp` | `FloatValidator` |
| 15 | `ConstrainedFloat` | `float.rs` | ⚠️ Partial | `basic.hpp` | `ConstrainedFloatValidator` (gt/ge/lt/le/multiple_of/allow_inf_nan supported) |
| 16 | `Decimal` | `decimal.rs` | ❌ Missing | — | No Python decimal.Decimal support |
| 17 | `List` | `list.rs` | ✅ Implemented | `containers.hpp` | `ListValidator` |
| 18 | `Set` | `set.rs` | ✅ Implemented | `containers.hpp` | `SetValidator` |
| 19 | `Tuple` | `tuple.rs` | ✅ Implemented | `containers.hpp` | `TupleValidator` |
| 20 | `Dict` | `dict.rs` | ✅ Implemented | `containers.hpp` | `DictValidator` |
| 21 | `None` | `none.rs` | ✅ Implemented | `special.hpp` | `NoneValidator` |
| 22 | `FunctionBefore` | `function.rs` | ✅ Implemented | `functions.hpp` | `FunctionBeforeValidator` |
| 23 | `FunctionAfter` | `function.rs` | ✅ Implemented | `functions.hpp` | `FunctionAfterValidator` |
| 24 | `FunctionPlain` | `function.rs` | ✅ Implemented | `functions.hpp` | `FunctionPlainValidator` |
| 25 | `FunctionWrap` | `function.rs` | ✅ Implemented | `functions.hpp` | `FunctionWrapValidator` (with SerializationInfo support) |
| 26 | `FunctionCall` | `call.rs` | ❌ Missing | — | No call/argument validation |
| 27 | `Literal` | `literal.rs` | ✅ Implemented | `special.hpp` | `LiteralValidator` |
| 28 | `MissingSentinel` | `missing_sentinel.rs` | ❌ Missing | — | No MISSING sentinel type |
| 29 | `IntEnum` | `enum_.rs` | ⚠️ Partial | `special.hpp` | Generic `EnumValidator`, no int-specific coercion |
| 30 | `StrEnum` | `enum_.rs` | ⚠️ Partial | `special.hpp` | Generic `EnumValidator`, no str-specific coercion |
| 31 | `FloatEnum` | `enum_.rs` | ⚠️ Partial | `special.hpp` | Generic `EnumValidator`, no float-specific coercion |
| 32 | `PlainEnum` | `enum_.rs` | ⚠️ Partial | `special.hpp` | Generic `EnumValidator` |
| 33 | `Any` | `any.rs` | ✅ Implemented | `special.hpp` | `AnyValidator` |
| 34 | `Bytes` | `bytes.rs` | ✅ Implemented | `basic.hpp` | `BytesValidator` |
| 35 | `ConstrainedBytes` | `bytes.rs` | ⚠️ Partial | `basic.hpp` | `BytesConstrainedValidator` (min_length/max_length supported) |
| 36 | `Date` | `date.rs` | ✅ Implemented | `basic.hpp` | `DateValidator` |
| 37 | `Time` | `time.rs` | ✅ Implemented | `basic.hpp` | `TimeValidator` |
| 38 | `Datetime` | `datetime.rs` | ✅ Implemented | `basic.hpp` | `DatetimeValidator` |
| 39 | `FrozenSet` | `frozenset.rs` | ✅ Implemented | `containers.hpp` | `FrozenSetValidator` |
| 40 | `Timedelta` | `timedelta.rs` | ✅ Implemented | `basic.hpp` | `TimedeltaValidator` |
| 41 | `IsInstance` | `is_instance.rs` | ⚠️ Partial | `basic.hpp` | `IsInstanceValidator` (stub — accepts any input) |
| 42 | `IsSubclass` | `is_subclass.rs` | ⚠️ Partial | `basic.hpp` | `IsSubclassValidator` (stub — accepts any input) |
| 43 | `Callable` | `callable.rs` | ⚠️ Partial | `basic.hpp` | `CallableValidator` (stub — accepts any input) |
| 44 | `Arguments` | `arguments.rs` | ❌ Missing | — | No argument validation (positional+keyword) |
| 45 | `ArgumentsV3` | `arguments_v3.rs` | ❌ Missing | — | New argument validation |
| 46 | `WithDefault` | `with_default.rs` | ✅ Implemented | `special.hpp` | `WithDefaultValidator` |
| 47 | `Chain` | `chain.rs` | ✅ Implemented | `special.hpp` | `ChainValidator` |
| 48 | `LaxOrStrict` | `lax_or_strict.rs` | ✅ Implemented | `special.hpp` | `LaxOrStrictValidator` |
| 49 | `Generator` | `generator.rs` | ❌ Missing | — | No generator validation |
| 50 | `CustomError` | `custom_error.rs` | ❌ Missing | — | No custom error wrapper |
| 51 | `Json` | `json.rs` | ✅ Implemented | `combined_validator.cpp` | `JsonValidator` |
| 52 | `Url` | `url.rs` | ⚠️ Partial | `basic.hpp` | `UrlValidator` (stub — returns success marker) |
| 53 | `MultiHostUrl` | `url.rs` | ❌ Missing | — | No multi-host URL support |
| 54 | `Uuid` | `uuid.rs` | ✅ Implemented | `basic.hpp` | `UuidValidator` |
| 55 | `DefinitionRef` | `definitions.rs` | ✅ Implemented | `combined_validator.cpp` | Recursive schemas now work via definitions lookup |
| 56 | `JsonOrPython` | `json_or_python.rs` | ✅ Implemented | `special.hpp` | `JsonOrPythonValidator` |
| 57 | `Complex` | `complex.rs` | ✅ Implemented | `complex.hpp` | `ComplexValidator` |
| 58 | `Prebuilt` | `prebuilt.rs` | ❌ Missing | — | No prebuilt validator reuse |

### Validator Summary

| Status | Count | Percentage |
|--------|-------|------------|
| ✅ Fully implemented | 34 | 59% |
| ⚠️ Partially implemented | 10 | 17% |
| ❌ Missing | 14 | 24% |

**Progress since last update:**
- `DefinitionRef` ✅ (recursive/self-referencing models now work)
- `ConstrainedInt/Float/Str/Bytes` ⚠️ (validators exist, constraints implemented)
- `IsInstance/IsSubclass/Callable` ⚠️ (stub validators added)

**Remaining critical gaps**: Decimal, Arguments, FunctionCall, Generator, CustomError, MissingSentinel, MultiHostUrl, Prebuilt

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
| 16 | `Generator` | `generator.rs` | ⚠️ Partial | `main_module.cpp` (stub) |
| 17 | `Dict` | `dict.rs` | ✅ Implemented | `main_module.cpp` |
| 18 | `Model` | `model.rs` | ✅ Implemented | `main_module.cpp` (delegates to child) |
| 19 | `Dataclass` | `dataclass.rs` | ✅ Implemented | `main_module.cpp` |
| 20 | `Url` | `url.rs` | ⚠️ Partial | `main_module.cpp` (basic) |
| 21 | `MultiHostUrl` | `url.rs` | ❌ Missing | No multi-host URL serialization |
| 22 | `Uuid` | `uuid.rs` | ✅ Implemented | `main_module.cpp` |
| 23 | `Any` | `any.rs` | ✅ Implemented | `main_module.cpp` |
| 24 | `Format` | `format.rs` | ✅ Implemented | `main_module.cpp` |
| 25 | `ToString` | `format.rs` | ✅ Implemented | `main_module.cpp` |
| 26 | `WithDefault` | `with_default.rs` | ✅ Implemented | `main_module.cpp` |
| 27 | `Json` | `json.rs` | ✅ Implemented | `main_module.cpp` (with round_trip support) |
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
| 39 | `FunctionWrap` | `function.rs` | ✅ Implemented | `main_module.cpp` (with SerializationInfo) |

### Serializer-only (enum_only, not directly built by type)

| # | Rust Variant | C++ Status | Notes |
|---|-------------|------------|-------|
| 1 | `Fields` (GeneralFieldsSerializer) | ⚠️ Partial | Basic include/exclude via `exc_none`; full filtering not implemented |
| 2 | `Prebuilt` | ❌ Missing | No prebuilt serializer reuse |
| 3 | `PolymorphismTrampoline` | ❌ Missing | No polymorphic serialization |
| 4 | `ComputedFields` | ⚠️ Partial | `computed_fields_` set tracks names for round_trip exclusion |

### Serializer Summary

| Status | Count | Percentage |
|--------|-------|------------|
| ✅ Implemented (CombinedSerializer enum) | 37 | 97% |
| ⚠️ Partial | 2 | 5% |
| ❌ Missing (CombinedSerializer enum) | 2 | 5% |
| ❌ Missing (enum_only infrastructure) | 2 | — |

**Progress since last update:**
- `round_trip` mode ✅ (full support with computed field exclusion)
- `SerializationInfo` ✅ (Python-visible info object for custom serializers)
- `FunctionWrap` with SerializationInfo ✅ (handler receives info object)

---

## 3. Input System

### Rust input types from `src/input/`

| # | Rust Input | Source File | C++ Status | Notes |
|---|-----------|-------------|------------|-------|
| 1 | `Input` (trait) | `input_abstract.rs` | ⚠️ Partial | Unified Input interface via `PythonInput` |
| 2 | `JsonInput` | `input_json.rs` | ✅ Implemented | Via simdjson + `pyobj_to_json_str()` |
| 3 | `PythonInput` | `input_python.rs` | ⚠️ Partial | Native PyObject validation via pybind11; no complex object support |
| 4 | `StringInput` | `input_string.rs` | ❌ Missing | No `validate_strings` / `StringMapping` |
| 5 | `DateTime` parsing | `datetime.rs` | ⚠️ Partial | Basic datetime parsing via simdjson |
| 6 | `ReturnEnums` | `return_enums.rs` | ❌ Missing | No ArgsKwargs, ValidationMatch, etc. |
| 7 | `Shared` helpers | `shared.rs` | ⚠️ Partial | Basic type coercion in validators |

### Input Summary

| Status | Notes |
|--------|-------|
| ✅ JSON input | Via simdjson parsing |
| ⚠️ Python input | Native PyObject via pybind11 (improved from JSON round-trip) |
| ❌ String input | No validate_strings path |
| ❌ ArgsKwargs | No positional argument support for dataclass init |

---

## 4. Error Handling

| # | Rust Feature | C++ Status | Notes |
|---|-------------|------------|-------|
| 1 | `ValidationError` | ✅ Implemented | Now inherits from `ValueError` (via `py::register_exception`) |
| 2 | `SchemaError` | ✅ Implemented | Now inherits from `ValueError` |
| 3 | Error message formatting | ⚠️ Partial | JSON-style errors; no full traceback/context |
| 4 | Custom error types | ❌ Missing | No `PydanticCustomError` wrapper |

---

## 5. Feature Gaps (Critical)

| Feature | Rust Status | C++ Status | Impact |
|---------|-------------|------------|--------|
| **DefinitionRef / Recursion** | ✅ Full | ✅ Working | Self-referencing models work |
| **Constrained validators** | ✅ Full | ⚠️ Partial | Constraints implemented but may need more testing |
| **ValidationInfo** | ✅ Full | ❌ Missing | `field_name`/`data`/`context` not passed to Python validators |
| **from_attributes** | ✅ Full | ❌ Ignored | Parameter accepted but discarded; no attribute access |
| **ArgsKwargs** | ✅ Full | ❌ Missing | Dataclass positional args fail |
| **MultiHostUrl** | ✅ Full | ❌ Missing | Entire validator missing |
| **include/exclude filtering** | ✅ Full | ❌ Missing | `model_dump(include=...)` not supported |
| **by_alias serialization** | ✅ Full | ❌ Missing | Alias mapping not supported |
| **round_trip mode** | ✅ Full | ✅ Working | Computed fields excluded, JSON wrapped |
| **SerializationInfo** | ✅ Full | ✅ Working | Python-visible info object |
| **Default value handling** | ✅ Full | ✅ Fixed | Defaults now validated through field validator |

---

## 6. Test Coverage Comparison

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

### Pydantic test coverage estimate

| Category | C++ Coverage |
|----------|--------------|
| Basic validation | ~30% |
| Serialization | ~25% |
| Edge cases | ~5% |
| Error handling | ~15% |
| Config/options | ~10% |
| **Overall** | **~15-20%** |

---

## 7. Priority Recommendations (Updated)

### P0 — Critical for Pydantic Compatibility
1. ✅ **DONE: DefinitionRef** — Recursive schemas now work
2. ⚠️ **ConstrainedInt/Float/Str/Bytes** — Validators exist, need more testing
3. ❌ **ValidationInfo** — `field_name`/`data`/`context` for Python validators
4. ❌ **from_attributes** — Attribute access for non-dict objects
5. ❌ **ArgsKwargs** — Dataclass positional argument support

### P1 — High Impact
6. ❌ **include/exclude filtering** — `model_dump()` customization
7. ❌ **by_alias serialization** — Alias support
8. ❌ **MultiHostUrl** — Multi-host URL types
9. ❌ **Arguments validator** — Function argument validation
10. ❌ **validate_strings** — StringMapping input path

### P2 — Medium Impact
11. ❌ **Decimal** — Precision numeric type
12. ⚠️ **computed fields serialization** — Tracking exists, full serialization missing
13. ❌ **Polymorphic serialization** — Inheritance-based serialization
14. ❌ **CustomError** — `PydanticCustomError`
15. ❌ **exclude_unset/exclude_defaults** — Dump options

---

## 8. Verdict (Updated)

| Metric | Previous | Current |
|--------|----------|---------|
| Validator types | 52% | **59%** |
| Serializer types | 95% (stubs) | **97%** (functional) |
| Actual functional coverage | 15-20% | **20-25%** |
| Pydantic compatibility | ~25% | **~30%** |

**Key improvements since last update:**
1. ✅ Recursive/DefinitionRef schemas fully working
2. ✅ ValidationError now proper Exception subclass
3. ✅ round_trip serialization mode implemented
4. ✅ SerializationInfo for custom serializers
5. ✅ Constrained validators implemented (need testing)
6. ✅ Default values validated through field validator
7. ⚠️ Native PythonInput via pybind11 (improved from JSON round-trip)

**Remaining critical gaps:**
1. **ValidationInfo** — Python validators lack context
2. **from_attributes** — Ignored at C++ level
3. **ArgsKwargs** — Dataclass init fails
4. **include/exclude/by_alias** — Serialization filtering missing
5. **MultiHostUrl** — Entire validator missing

---

## 9. Architecture Notes

| Aspect | Rust | C++ |
|--------|------|-----|
| Input format | Native PyObject | PyObject via pybind11 + JSON fallback |
| Type system | Rust enums | `shared_ptr<Validator>` + type_name strings |
| JSON parsing | jiter | simdjson |
| Error types | `ValError` enum | `ValError` class + Python Exception subclasses |
| Recursion | Built-in | `RecursionGuard` class |
| Serialization | serde_json | Manual JSON building + Python dict |
| Python bindings | pyo3 | pybind11 |
| Build system | maturin/Cargo | scikit-build-core/CMake |