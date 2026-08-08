# pydantic-core Architecture Reference

> Analysis of the Rust pydantic-core implementation, written as a design guide for the C++ port (`pydantic-core-cpp`).
>
> Date: 2026-07-19
> Rust version analyzed: 2.47.0

---

## 1. High-Level Architecture

```
┌─────────────────────────────────────────────┐
│              Python (pydantic)               │
│  pydantic/_internal/_generate_schema.py       │
│  pydantic/main.py (BaseModel)                │
├─────────────────────────────────────────────┤
│          pydantic_core (Python package)       │
│  __init__.py       core_schema.py            │
│  _pydantic_core.pyi (type stubs)            │
├─────────────────────────────────────────────┤
│       _pydantic_core (Rust native .so)       │
│  ┌──────────────────┬────────────────────┐   │
│  │   Validators      │   Serializers      │   │
│  │   (src/valid/)    │   (src/serial/)    │   │
│  ├──────────────────┼────────────────────┤   │
│  │   Input layer     │   Error types      │   │
│  │   (src/input/)    │   (src/errors/)    │   │
│  ├──────────────────┼────────────────────┤   │
│  │   build.rs        │   pyproject.toml   │   │
│  └──────────────────┴────────────────────┘   │
└─────────────────────────────────────────────┘
```

### Build Chain
```
pyproject.toml → maturin/maturin2 → Cargo.toml → build.rs → Rust src/ → _pydantic_core.so
```

- **maturin**: Build tool for Rust-based Python extensions
- **pyo3**: Rust ↔ Python binding framework (similar to pybind11)
- **build.rs**: Cargo build script that generates Python version detection code, handles WASM compilation flags

---

## 2. Rust Source Structure (src/)

```
src/
├── lib.rs                    # Module registration, PyO3 bindings
├── build_tools.rs            # Schema building helpers
├── validators/               # Validation engine
│   ├── mod.rs                # CombinedValidator enum (enum_dispatch)
│   ├── model.rs              # ModelValidator
│   ├── model_fields.rs       # ModelFieldsValidator
│   ├── typed_dict.rs         # TypedDictValidator
│   ├── dataclass.rs          # DataclassValidator
│   ├── list.rs               # ListValidator
│   ├── tuple.rs              # TupleValidator
│   ├── dict.rs               # DictValidator
│   ├── set.rs                # SetValidator
│   ├── frozenset.rs          # FrozensetValidator
│   ├── int.rs                # IntValidator
│   ├── float.rs              # FloatValidator
│   ├── bool.rs               # BoolValidator
│   ├── string.rs             # StrValidator
│   ├── bytes.rs              # BytesValidator
│   ├── decimal.rs            # DecimalValidator
│   ├── uuid.rs               # UuidValidator
│   ├── url.rs                # UrlValidator, MultiHostUrlValidator
│   ├── date.rs               # DateValidator
│   ├── time.rs               # TimeValidator
│   ├── datetime.rs           # DatetimeValidator
│   ├── timedelta.rs          # TimedeltaValidator
│   ├── literal.rs            # LiteralValidator
│   ├── enum.rs               # EnumValidator
│   ├── any.rs                # AnyValidator
│   ├── none.rs               # NoneValidator
│   ├── nullable.rs           # NullableValidator
│   ├── union.rs              # UnionValidator
│   ├── tagged_union.rs       # TaggedUnionValidator
│   ├── chain.rs              # ChainValidator (function-wrap chain)
│   ├── definitions.rs        # DefinitionsValidator + DefinitionRef
│   ├── function.rs           # FunctionBefore/After/Wrap/Plain
│   ├── with_default.rs       # WithDefaultValidator
│   ├── lax_or_strict.rs      # LaxOrStrictValidator
│   ├── json_or_python.rs     # JsonOrPythonValidator
│   ├── callable.rs           # CallableValidator
│   ├── is_instance.rs        # IsInstanceValidator
│   ├── is_subclass.rs        # IsSubclassValidator
│   └── generation.rs         # Schema generation helpers
├── serializers/              # Serialization engine
│   ├── mod.rs                # CombinedSerializer enum
│   ├── fields.rs             # SerField, dict/set serialization
│   ├── type_serializers/     # Type-specific serializers
│   │   ├── model.rs
│   │   ├── typed_dict.rs
│   │   ├── list.rs
│   │   ├── dict.rs
│   │   ├── string.rs
│   │   ├── int.rs
│   │   ├── float.rs
│   │   ├── bool.rs
│   │   ├── any.rs
│   │   ├── function.rs
│   │   ├── nullable.rs
│   │   └── recursive.rs
│   └── shared.rs             # Shared serialization utilities
├── input/                    # Input trait + implementations
│   ├── mod.rs                # Input trait definition, Either* types
│   ├── python.rs             # PythonInput (validate_python)
│   ├── json.rs               # JsonInput (validate_json)
│   └── string.rs             # StringInput (from JSON string)
├── errors/                   # Error system
│   ├── mod.rs                # ValError, ValResult, ErrorType
│   └── line_error.rs         # ValLineError
├── tools/                    # Utilities
│   └── schema.rs             # Schema transformation helpers
└── serialization_state.rs    # SerializationState
```

---

## 3. Python ↔ Rust Interface

### 3.1 Module Registration (lib.rs)

Using PyO3's `#[pymodule]` macro:

```rust
#[pymodule]
fn _pydantic_core(_py: Python, m: &PyModule) -> PyResult<()> {
    // Register classes
    m.add_class::<SchemaValidator>()?;
    m.add_class::<SchemaSerializer>()?;
    m.add_class::<ValidationError>()?;
    // ...
    // Register exceptions
    m.add("ValidationError", _py.get_type::<ValidationError>())?;
    // Register enums
    m.add("__version__", VERSION)?;
}
```

Key difference from our C++ port: **PyO3 uses Rust structs directly as Python classes** via `#[pyclass]`, while our C++ port uses `pybind11::class_<>`. The Rust `SchemaValidator` struct IS the Python `pydantic_core.SchemaValidator`.

### 3.2 SchemaValidator (Rust)

```rust
#[pyclass(module = "pydantic_core._pydantic_core")]
struct SchemaValidator {
    schema: Py<PyDict>,              // Original schema dict
    validator: CombinedValidator,    // The composed validator tree
    title: String,                   // Schema title
    // ...
}
```

- `CombinedValidator` is an `enum_dispatch` enum — at runtime, it dispatches to the correct validator's methods via a generated vtable-like mechanism
- Constructor takes `schema: &PyDict` and `config: &PyDict` directly, NOT JSON strings

**Critical difference from C++ port**: Rust receives the Python dict directly. There's **no JSON serialization step**. This means:
- Python class/function references in the schema are preserved
- No `_default_serializer` workaround needed
- The `function` key in `function-after`/`function-before` schemas just works

**Note (2026-08)**: The C++ port now matches this — `SchemaBuilder::build_from_py` passes the Python dict directly to `build_from_py_dict` (no JSON round-trip). Python class references are handled by `_schema_clean_cls_keys`, which keeps `cls` on `model`/`dataclass`/`is-instance`/`is-subclass` schemas in the deep-copied schema passed to C++ (needed for smart-union class matching and root models) and strips it elsewhere; `_extract_model_classes` keeps a Python-side `ref → class` map for dict-to-model conversion.

### 3.3 CoreSchema (Python TypedDicts)

`python/pydantic_core/core_schema.py` defines TypedDicts like:

```python
class IntSchema(TypedDict, total=False):
    type: Required[str]           # "int"
    multiple_of: int
    le: int
    ge: int
    lt: int
    gt: int
    default: Any
    ...
```

These are **pure Python type hints** used by pydantic's schema generator. The Rust validators don't use them at runtime — they parse the Python dict directly. This means:

```
pydantic generates schema:
    core_schema.int_schema(multiple_of=3)
    → returns a Python dict: {"type": "int", "multiple_of": 3}

Rust SchemaValidator receives:
    the dict directly, extracts "type" → looks up IntValidator
    → validates, returns IntValidator's validate()
```

---

## 4. Validation Architecture

### 4.1 The Input Trait System

The most important architectural pattern. Every validator operates on an abstract `Input` trait:

```rust
enum ValResult<T> {
    Ok(T),
    Err(ValError),
}

enum InputType {
    Python,    // PythonInput: wraps pyo3 PyAny
    Json,      // JsonInput: wraps simdjson DOM
    String,    // StringInput: wraps &str for JSON string parsing
}

trait Input {
    fn input_type(&self) -> InputType;
    fn is_none(&self) -> bool;
    fn as_python(&self) -> &PyAny;    // Convert to Python object
    fn validate_str(&self, strict: bool) -> ValResult<EitherString>;
    fn validate_int(&self, strict: bool) -> ValResult<EitherInt>;
    fn validate_float(&self, strict: bool) -> ValResult<EitherFloat>;
    fn validate_bool(&self, strict: bool) -> ValResult<EitherBool>;
    fn validate_list(&self, strict: bool) -> ValResult<Vec<PyObject>>;
    fn validate_dict(&self, strict: bool) -> ValResult<Vec<(String, PyObject)>>;
    // ...
}
```

**Architecture impact for C++ port**: Our C++ `Input` base class mirrors this, with all three implementations:
- `EitherString`/`EitherBytes` (`include/pydantic_core/input.hpp`) for owned/borrowed value representation
- `as_python_object()` for Python callable interaction
- `ValidatedList`/`ValidatedDict`/`ValidatedTuple` containers with entry iteration; `ValidatedDict` also exposes `get_value()`/`get_key()` so the dict validator can sub-validate keys and values (keys may be non-string in Python input)

### 4.2 CombinedValidator Dispatch

```rust
#[enum_dispatch(Validator)]
enum CombinedValidator {
    Int(IntValidator),
    Float(FloatValidator),
    Str(StrValidator),
    List(ListValidator),
    Model(ModelValidator),
    ModelFields(ModelFieldsValidator),
    // ...30+ variants
}

#[enum_dispatch]
trait Validator {
    fn validate(&self, input: &Input, state: &mut ValidationState) -> ValResult<PyObject>;
    fn name(&self) -> &str;
}
```

`enum_dispatch` generates a vtable-based dispatch at compile time — faster than `Box<dyn Validator>` (trait objects) and doesn't require allocation. Equivalent in C++: **`std::variant` + `std::visit`** or a base class with virtual methods.

**Our C++ choice**: We use `std::variant` inside `CombinedValidator`, passed through `std::shared_ptr<Validator>` in the validator tree. This matches Rust's approach but through a different mechanism.

### 4.3 ValidationState

```rust
struct ValidationState {
    errors: Vec<ValLineError>,        // Collected errors
    loc: Location,                     // Current path in schema (e.g., ["a", 0, "name"])
    config: CoreConfig,               // Schema config (strict, etc.)
    context: Option<PyObject>,        // User-provided context
    definitions: Option<DefinitionsBuilder>,
    field_name: Option<String>,       // Current field name
    // ...
}
```

Key methods:
- `push_loc(key)` / `pop_loc()` — RAII location tracking
- `strict_or(bool)` — config-aware strict mode
- `set_field_name(name)` — for function validators' `ValidationInfo`

**Our C++ state** (`ValidationState` in `include/pydantic_core/validation_state.hpp`) has all of these features, plus:
- `coerce_strings()` — strings-mode flag set by `validate_strings` so string values always coerce regardless of strict (matches Rust's StringInput semantics)
- `sub_copy(force_lax)` — constructs a fresh sub-state (used by list/dict validators to force lax key/value validation in strings mode)
- `extra_behavior_or(default)` — lets call-level `extra='allow'` override the build-time config

### 4.4 Error Collection

```rust
enum ValError {
    LineErrors(Vec<ValLineError>),    // Normal validation errors
    Omit,                              // Skip field (exclude_unset)
    UseDefault,                        // Use field default value
}

struct ValLineError {
    kind: ErrorType,                   // The error kind enum
    location: Location,                // Path in input
    message: String,                   // Human-readable message
    input_value: PyObject,            // The input that caused the error
    ctx: BTreeMap<String, PyObject>,  // Error context
}
```

**Special errors for control flow**:
- `ValError::Omit` — signals "skip this field" to Wrap/After validators
- `ValError::UseDefault` — signals "use the default value" to WithDefault

**Our C++ errors** (`ValError` in `include/pydantic_core/errors.hpp`) have the same structure with `ValError::Omit`/`ValError::UseDefault` equivalents.

### 4.5 Validation Pipeline Example

For `SchemaValidator.validate_python({'a': 42})` with a model schema:

```
1. SchemaValidator::validate_python(input_dict)
2.   ↓ input_dict → PythonInput
3.   CombinedValidator::validate(input, state)
4.     dispatch → ModelValidator::validate(input, state)
5.       dispatch → ModelFieldsValidator::validate(input, state)
6.         input.validate_dict() → validated dict entries
7.         for each field:
8.           push_loc(field_name)
9.           PythonInput for field value ← create sub-input
10.          field.validator.validate(sub_input, state)
11.            dispatch → IntValidator::validate(sub_input, state)
12.              → validated PyObject (42)
13.          store in output dict
14.          pop_loc()
15.       return ValidatedModelFieldsOutput as PyDict
16.  return PyDict to Python
```

---

## 5. Serialization Architecture

### 5.1 SchemaSerializer

```rust
#[pyclass]
struct SchemaSerializer {
    schema: Py<PyDict>,
    serializer: CombinedSerializer,
    // ...
}
```

The serializer mirrors the validator architecture but in reverse:

```rust
#[enum_dispatch(Serializer)]
enum CombinedSerializer {
    Model(ModelSerializer),
    List(ListSerializer),
    Dict(DictSerializer),
    Int(IntSerializer),
    Float(FloatSerializer),
    Str(StrSerializer),
    // ...
}
```

### 5.2 Two Output Modes

Every serializer has two output methods:

```rust
trait Serializer {
    fn to_python(&self, value: &PyAny, state: &mut SerializationState) -> PyResult<PyObject>;
    fn to_json(&self, value: &PyAny, state: &mut SerializationState) -> PyResult<String>;
}
```

- `to_python` → returns Python objects (dicts, lists, etc.)
- `to_json` → returns JSON strings directly

This avoids the "construct Python dict then serialize" two-pass approach, though Rust does both depending on context.

### 5.3 Field Order in Serialization

The serializer's `SerField` struct stores field metadata in an `AHashMap<PyBackedStr, SerField>`. Output order is determined by iterating the **input model's `__dict__`** keys, not the hashmap. The hashmap is for O(1) lookup:

```rust
// In ModelSerializer:
let dict = value.getattr("__dict__")?;  // Python dict with insertion order
for (key, value) in dict.iter_items() {
    if let Some(field) = self.fields.get(&key) {
        // serialize field
    }
}
```

**Our C++ fix**: This is exactly the pattern we implemented — `SerNode::field_order` vector + unordered_map lookup.

### 5.4 Serializer capabilities now in the C++ port

Beyond the base pattern, the C++ `SerNode` serializer (`src/main_module.cpp`) implements:
- **by_alias** — field aliases read from `serialization_alias` (fallback `alias`) and applied in `serialize_fields`/`serialize_fields_json`
- **ser_json_inf_nan='strings'** — float `to_json` emits quoted `"NaN"`/`"Infinity"`; the mode is propagated from schema config (`inf_nan_mode`)
- **Root models** — `root_model` flag; `to_python`/`to_json` extract the `.root` attribute before delegating to the inner serializer
- **Union discrimination** — model/dataclass serializer nodes store `class_` (from schema `cls`) and throw for wrong-class instances, so the union serializer falls through to the matching branch (to_python; `to_json` lacks union/enum/date branches and falls back to `infer_json`)
- **exclude_unset** — reads `__pydantic_fields_set__`; `exclude_defaults` is currently dead code (`__pydantic_defaults__` is never set)

---

## 6. Error Types (ErrorType)

`ErrorType` is generated from a Strum enum:

```rust
#[derive(strum::Display, strum::EnumString)]
#[strum(serialize_all = "snake_case")]
enum ErrorType {
    // Type validation
    IntParsing,        // "int_parsing"
    FloatParsing,      // "float_parsing"  
    StringType,        // "string_type"
    BytesType,         // "bytes_type"
    BoolParsing,       // "bool_parsing"
    DateTimeParsing,   // "date_time_parsing"
    DateParsing,       // "date_parsing"
    TimeParsing,       // "time_parsing"
    // Type mismatch
    IntType,           // "int_type"
    FloatType,         // "float_type"
    // General
    Missing,           // "missing"
    ExtraForbidden,    // "extra_forbidden"
    LiteralMismatch,   // "literal_mismatch"
    EnumError,         // "enum"
    // ... 80+ variants
}
```

The `strum` derive macro auto-generates the `to_string()` (as snake_case) and `from_string()` methods. Error type names are the canonical identifier used by pydantic tests.

**Our C++ error types** (`ErrorType::Kind` enum in `include/pydantic_core/error_types.hpp`) now define ~90 variants covering most core kinds: type mismatches (`int_type`, `model_type`, ...), parsing (`int_parsing`, `bool_parsing`, `float_parsing`, `date_parsing`, `date_from_datetime_inexact`, `time_parsing`, `datetime_parsing`, `timedelta_parsing`), constraints (`int_multiple_of`, `float_greater_than`, `string_too_short`, `bytes_too_long`, `list_too_short`, ...), and general (`missing`, `extra_forbidden`, `literal_mismatch`, `enum`, `recursion_error`, ...). String-input parse failures report the Rust-compatible `*_parsing` kinds. Still missing: the full tail of exotic kinds (e.g. some `ipaddress`-family errors).

---

## 7. Key Design Patterns to Replicate

### 7.1 enum_dispatch → CombinedValidator/Variant

**Rust**: `#[enum_dispatch(Validator)]` generates a trait dispatcher enum
**C++**: We use `std::variant<shared_ptr<IntValidator>, shared_ptr<StrValidator>, ...>` with `std::visit` on a visitor lambda

### 7.2 Input Trait with Three Implementations

**Rust**: `Input` trait with `PythonInput`, `JsonInput`, `StringInput`
**C++**: `Input` abstract class with `PythonInput`, `JsonInput`, and `StringInput` (added 2026) — `StringInput::validate_int/float/bool/date/...` ignore the strict flag and always coerce, matching Rust; `validate_strings` routes through `validate_strings_object` with `coerce_strings` mode

### 7.3 Either Types for Borrowed Data

**Rust**: 
```rust
enum EitherString { 
    Py(Py<PyString>),    // Borrowed from Python
    Str(String),         // Owned from JSON
}
```

**C++**: `EitherString`/`EitherBytes` structs in `include/pydantic_core/input.hpp` (either a Python object or an owned string/bytes), used by Str/Bytes validators and converted back to Python in `result_to_python`

### 7.4 ValResult / ValError for Control Flow

**Rust**: `ValResult<T>` with Ok/Err(LineErrors | Omit | UseDefault)
**C++**: Same pattern — `ValResult<T>` with `is_ok()`, `is_err()`, and `ValError` variants.

### 7.5 Definitions System for Recursive Schemas

**Rust**: 
```rust
struct DefinitionsBuilder {
    definitions: HashMap<String, OnceLock<ParsedSchema>>,
    schema: Option<ParsedSchema>,
}
```

1. Create registry with empty `OnceLock` entries
2. Register weak ref (`DefinitionRef`) pointing to the registry
3. Build each definition → fill `OnceLock`
4. When `DefinitionRef::validate()` is called, the `OnceLock` is already populated

**C++**: `DefinitionsRegistry` with `unordered_map<string, shared_ptr<Validator>>` and `DefinitionRefValidator` that resolves at validate time. Our approach is equivalent — no `OnceLock` needed because `shared_ptr` is populated eagerly.

### 7.6 ValidationInfo for Python Validators

```rust
#[pyclass]
struct ValidationInfo {
    #[pyo3(get)]
    pub context: Option<PyObject>,
    #[pyo3(get)]
    pub field_name: Option<String>,
}
```

Passed as the second argument to Python callable validators (`function-before`/`after`/`wrap`/`plain`).

---

## 8. Build System Comparison

| Aspect | Rust | C++ |
|--------|------|-----|
| Build tool | maturin + Cargo | scikit-build-core + CMake + Ninja |
| Binding framework | pyo3 0.28 | pybind11 2.13 |
| JSON library | serde_json + simdjson | simdjson v3.9 |
| Date/time | speedate | Custom ISO-8601 parsing in the input layer (`Date`/`Time`/`DateTime` structs), Python `datetime` objects at the boundary |
| Hashing | ahash | std::unordered_map (default) |
| Regex | regex crate | std::regex |
| LRU cache | lru crate | (future) |
| Serialization | Custom (no serde) | Custom (in main_module.cpp) |

---

## 9. Python Package Structure

```
python/pydantic_core/
├── __init__.py               # Re-exports from _pydantic_core + core_schema
├── _pydantic_core.pyi        # Type stubs for the Rust native module
├── core_schema.py            # TypedDict definitions (~4,500 lines)
└── py.typed                  # PEP 561 marker
```

The `core_schema.py` file defines ALL schema TypedDicts used by pydantic. It's imported directly (not through Rust) — pydantic uses it for type-checked schema construction. At runtime, the TypedDicts become plain dicts.

**For our C++ port**: We copied `core_schema.py` (line-for-line) and import it directly from `pydantic_core_cpp/__init__.py`. This eliminates the need to generate schema types from C++.

---

## 10. Summary: What Makes Rust's Architecture Work

1. **PyO3 native integration**: Rust receives Python objects directly, no JSON serialization. The C++ port now does the same (`build_from_py_dict`); it still strips `cls` keys into a Python-side `_model_classes` map because the C++ native layer keeps the schema as a plain `py::dict`, but there is no JSON serialization step.

2. **enum_dispatch**: Compile-time generated dispatch table with zero overhead vs runtime trait objects. The C++ port uses `std::variant` inside `CombinedValidator` + virtual methods on a `Validator` base class.

3. **Input trait abstraction**: Clean separation of input sources enables the same validator to work with Python dicts, JSON strings, or string values. The C++ port now has all three: `PythonInput`, `JsonInput`, `StringInput` (with strings-mode coercion).

4. **Either types**: Zero-copy string handling by borrowing Python strings when possible, owning when converting from JSON.

5. **ValError as control flow**: `Omit` and `UseDefault` variants eliminate the need for special-case handling of missing/default fields.

6. **RAII location tracking**: `push_loc`/`pop_loc` via scoped guards automatically tracks the validation path.

7. **Separate to_python / to_json**: The serializer can produce either output format directly without intermediate conversion.

8. **Definitions via OnceLock + Weak**: Forward references for recursive types without reference cycles.
