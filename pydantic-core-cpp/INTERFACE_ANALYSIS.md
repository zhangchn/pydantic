# pydantic-core Interface Analysis for C++ Port

## Overview

pydantic-core is a Rust library (~32,627 lines) that provides the core validation and serialization
engine for pydantic. It's built on PyO3 (Python bindings for Rust) and exposes a Python module
`_pydantic_core` with two main classes: `SchemaValidator` and `SchemaSerializer`.

---

## 1. Public Python API (from `_pydantic_core.pyi`)

### Core Classes

#### SchemaValidator
```python
class SchemaValidator:
    def __init__(self, schema: CoreSchema, config: CoreConfig | None = None)
    
    # Validation methods
    def validate_python(input, *, strict=None, extra=None, from_attributes=None, 
                        context=None, self_instance=None, allow_partial=False,
                        by_alias=None, by_name=None) -> Any
    
    def validate_json(input, *, strict=None, extra=None, context=None,
                      self_instance=None, allow_partial=False, by_alias=None, 
                      by_name=None) -> Any
    
    def validate_strings(input, *, strict=None, extra=None, context=None,
                         allow_partial=False, by_alias=None, by_name=None) -> Any
    
    def isinstance_python(input, ...) -> bool  # Returns True/False instead of raising
    
    def validate_assignment(obj, field_name, field_value, ...) -> dict | tuple
    
    def get_default_value(*, strict=None, context=None) -> Some | None
    
    @property
    def title(self) -> str
```

#### SchemaSerializer
```python
class SchemaSerializer:
    def __init__(self, schema: CoreSchema, config: CoreConfig | None = None)
    
    def to_python(value, *, mode=None, include=None, exclude=None, by_alias=None,
                  exclude_unset=False, exclude_defaults=False, exclude_none=False,
                  round_trip=False, warnings=True, fallback=None, 
                  serialize_as_any=False, context=None) -> Any
    
    def to_json(value, *, indent=None, ensure_ascii=False, include=None, 
                exclude=None, ...) -> bytes
```

### Standalone Functions
```python
def from_json(data, *, allow_inf_nan=True, cache_strings=True, allow_partial=False) -> Any
def to_json(value, ...) -> bytes
def to_jsonable_python(value, ...) -> Any
def list_all_errors() -> list[dict]
```

### Error Types
```python
class ValidationError(Exception)
class SchemaError(Exception)
class PydanticCustomError(Exception)
class PydanticKnownError(Exception)
class PydanticOmit(Exception)
class PydanticUseDefault(Exception)
class PydanticSerializationError(Exception)
```

### Utility Types
```python
class Some(Generic[T]):  # Rust-style Option::Some wrapper
    @property
    def value(self) -> T

class Url:  # URL type with properties: scheme, host, port, path, query, fragment
class MultiHostUrl:
class TzInfo:
class PydanticUndefinedType:
class ArgsKwargs:
```

---

## 2. Schema Types (from `core_schema.py`)

### CoreSchema Union (~50+ schema types)
The schema is a TypedDict with a `type` field that determines the validator/serializer behavior:

```python
CoreSchema: TypeAlias = (
    NoneSchema | BoolSchema | IntSchema | FloatSchema | StringSchema |
    BytesSchema | DecimalSchema | DateSchema | TimeSchema | DatetimeSchema |
    TimedeltaSchema | ListSchema | SetSchema | FrozenSetSchema |
    TupleSchema | DictSchema | ModelSchema | DataclassSchema |
    TypedDictSchema | ModelFieldsSchema | UnionSchema | TaggedUnionSchema |
    NullableSchema | LiteralSchema | EnumSchema | IsInstanceSchema |
    IsSubclassSchema | CallableSchema | ArgumentsSchema | ArgumentsV3Schema |
    FunctionSchema | FunctionWrapSchema | FunctionAfterSchema | FunctionBeforeSchema |
    WithDefaultSchema | ChainSchema | LaxOrStrictSchema | JsonOrPythonSchema |
    JsonSchema | UrlSchema | MultiHostUrlSchema | UuidSchema |
    CustomErrorSchema | GeneratorSchema | AnySchema | DefinitionsSchema |
    DefinitionRefSchema | ComplexSchema | InvalidSchema
)
```

### CoreConfig TypedDict
```python
class CoreConfig(TypedDict, total=False):
    title: str
    strict: bool
    extra_fields_behavior: ExtraBehavior  # 'allow', 'forbid', 'ignore'
    typed_dict_total: bool
    from_attributes: bool
    loc_by_alias: bool
    revalidate_instances: Literal['always', 'never', 'subclass-instances']
    validate_default: bool
    str_max_length: int
    str_min_length: int
    str_strip_whitespace: bool
    str_to_lower: bool
    str_to_upper: bool
    allow_inf_nan: bool
    ser_json_timedelta: Literal['iso8601', 'float']
    ser_json_temporal: Literal['iso8601', 'seconds', 'milliseconds']
    ser_json_bytes: Literal['utf8', 'base64', 'hex']
    ser_json_inf_nan: Literal['null', 'constants', 'strings']
    val_json_bytes: Literal['utf8', 'base64', 'hex']
    hide_input_in_errors: bool
    validation_error_cause: bool
    coerce_numbers_to_str: bool
    regex_engine: Literal['rust-regex', 'python-re']
    cache_strings: bool | Literal['all', 'keys', 'none']
    validate_by_alias: bool
    validate_by_name: bool
    serialize_by_alias: bool
    polymorphic_serialization: bool
    url_preserve_empty_path: bool
```

---

## 3. Rust Architecture (Core Design Patterns)

### Validator Trait (validators/mod.rs)
```rust
pub trait Validator: Send + Sync + Debug {
    fn validate<'py>(
        &self,
        py: Python<'py>,
        input: &(impl Input<'py> + ?Sized),
        state: &mut ValidationState<'_, 'py>,
    ) -> ValResult<Py<PyAny>>;
    
    fn default_value<'py>(...) -> ValResult<Option<Py<PyAny>>>;
    fn validate_assignment<'py>(...) -> ValResult<Py<PyAny>>;
    fn get_name(&self) -> &str;
}
```

### CombinedValidator Enum (~50 validator types)
Uses `enum_dispatch` for zero-cost dispatch:
```rust
pub enum CombinedValidator {
    TypedDict(TypedDictValidator),
    Union(UnionValidator),
    TaggedUnion(TaggedUnionValidator),
    Nullable(NullableValidator),
    Model(ModelValidator),
    ModelFields(ModelFieldsValidator),
    Dataclass(DataclassValidator),
    Str(StrValidator),
    StrConstrained(StrConstrainedValidator),
    Int(IntValidator),
    ConstrainedInt(ConstrainedIntValidator),
    Bool(BoolValidator),
    Float(FloatValidator),
    // ... ~50 more variants
}
```

### BuildValidator Trait (Schema -> Validator)
```rust
pub trait BuildValidator: Sized {
    const EXPECTED_TYPE: &'static str;  // e.g., "str", "int", "model"
    
    fn build(
        schema: &Bound<'_, PyDict>,
        config: Option<&Bound<'_, PyDict>>,
        definitions: &mut DefinitionsBuilder<Arc<CombinedValidator>>,
    ) -> PyResult<Arc<CombinedValidator>>;
}
```

### Input Trait (input/input_abstract.rs)
Abstracts input source (Python object, JSON, or string):
```rust
pub(crate) trait Input<'py>: fmt::Debug {
    fn as_error_value(&self) -> InputValue;
    fn is_none(&self) -> bool;
    fn as_python(&self) -> Option<&Bound<'py, PyAny>>;
    fn as_json(&self) -> Option<&JsonValue<'_>>;
    
    // Type-specific validation methods
    fn validate_str(&self, strict: bool, coerce_numbers_to_str: bool) -> ValMatch<EitherString>;
    fn validate_bytes(&self, strict: bool, mode: ValBytesMode) -> ValMatch<EitherBytes>;
    fn validate_bool(&self, strict: bool) -> ValMatch<bool>;
    fn validate_int(&self, strict: bool) -> ValMatch<EitherInt>;
    fn validate_float(&self, strict: bool) -> ValMatch<EitherFloat>;
    fn validate_dict(&self, strict: bool) -> ValResult<Self::Dict<'_>>;
    fn validate_list(&self, strict: bool) -> ValMatch<Self::List<'_>>;
    // ... more validate_* methods
}
```

### InputType Enum
```rust
pub enum InputType {
    Python,  // Python objects via PyO3
    Json,    // jiter::JsonValue (parsed JSON)
    String,  // StringMapping (string key-value pairs)
}
```

---

## 4. Serializer Architecture

### Serializer Trait (serializers/shared.rs)
```rust
pub trait Serializer: Send + Sync + Debug {
    fn to_python(
        &self,
        value: &Bound<'_, PyAny>,
        state: &mut SerializationState,
    ) -> PyResult<Py<PyAny>>;
    
    fn to_json(
        &self,
        value: &Bound<'_, PyAny>,
        state: &mut SerializationState,
    ) -> PyResult<Vec<u8>>;
}
```

### CombinedSerializer Enum
```rust
pub enum CombinedSerializer {
    None(NoneSerializer),
    Nullable(NullableSerializer),
    Int(IntSerializer),
    Bool(BoolSerializer),
    Float(FloatSerializer),
    Str(StrSerializer),
    Bytes(BytesSerializer),
    List(ListSerializer),
    Dict(DictSerializer),
    Model(ModelSerializer),
    // ... ~30 more variants
}
```

---

## 5. Error Handling

### ValError / ValResult (errors/line_error.rs)
```rust
pub enum ValError {
    LineErrors(Vec<ValLineError>),  // Validation errors with location
    InternalErr(PyErr),             // Python exception
    Omit,                           // Skip this field (PydanticOmit)
    UseDefault,                     // Use default value (PydanticUseDefault)
}

pub type ValResult<T> = Result<T, ValError>;
```

### ErrorType Enum (~100+ error types)
```rust
pub enum ErrorType {
    // Type errors
    BoolType, IntType, FloatType, StringType, BytesType, ...
    
    // Value errors
    StringTooShort { min_length: usize },
    StringTooLong { max_length: usize },
    IntGreaterThan { gt: Number },
    IntLessThan { lt: Number },
    FloatGreaterThan { gt: Number },
    // ... many more
}
```

---

## 6. Key Dependencies

| Rust Crate | Purpose | C++ Alternative |
|------------|---------|-----------------|
| pyo3 | Python bindings | pybind11 / nanobind |
| jiter | JSON parsing | simdjson / rapidjson |
| serde_json | JSON serialization | rapidjson / nlohmann/json |
| regex | Pattern matching | C++ std::regex or RE2 |
| speedate | Date/time parsing | C++ chrono + custom parsing |
| url | URL handling | C++ custom or libcurl |
| uuid | UUID handling | C++ custom or libuuid |
| num-bigint | Arbitrary precision | GMP / Boost.Multiprecision |
| hashbrown | HashMap | C++ unordered_map |
| lru | LRU cache | C++ custom implementation |
| smallvec | Small vector optimization | C++ custom or boost::small_vector |

---

## 7. C++ Port Strategy

### Phase 1: Core Infrastructure
1. Python bindings setup (pybind11/nanobind)
2. Error types and result handling
3. Input abstraction layer
4. ValidationState and recursion guard

### Phase 2: Basic Validators
1. None, Bool, Int, Float, String, Bytes
2. List, Dict, Set, FrozenSet, Tuple
3. Literal, Enum, Nullable, Union

### Phase 3: Complex Validators  
1. Model, ModelFields, TypedDict, Dataclass
2. Function validators (before/after/plain/wrap)
3. Date, Time, Datetime, Timedelta
4. URL, UUID, Decimal, JSON

### Phase 4: Serialization
1. Basic serializers (mirroring validators)
2. JSON serialization
3. Python object serialization

### Phase 5: JSON Parsing
1. Integrate simdjson for fast JSON parsing
2. from_json() standalone function

---

## 8. File Structure Mapping

| Rust File | Purpose | C++ Equivalent |
|-----------|---------|----------------|
| lib.rs | Module entry, pyfunction exports | main_module.cpp |
| validators/mod.rs | Validator enum + trait | validators/validator.hpp |
| validators/string.rs | String validator | validators/string_validator.cpp |
| validators/int.rs | Int validator | validators/int_validator.cpp |
| validators/model.rs | Model validator | validators/model_validator.cpp |
| input/input_abstract.rs | Input trait | input/input.hpp |
| input/input_python.rs | Python input impl | input/python_input.cpp |
| input/input_json.rs | JSON input impl | input/json_input.cpp |
| errors/mod.rs | Error types | errors/errors.hpp |
| errors/types.rs | ErrorType enum | errors/error_types.hpp |
| serializers/mod.rs | Serializer enum + trait | serializers/serializer.hpp |
| definitions.rs | Recursive schema definitions | definitions.hpp |
| build_tools.rs | Schema building utilities | build_tools.hpp |
| tools.rs | General utilities | tools.hpp |

---

## 9. Critical Implementation Details

### Recursion Guard
- Limits recursion depth (RECURSION_GUARD_LIMIT = 100)
- Prevents infinite loops in self-referencing schemas
- Uses HashSet to track visited objects

### Definitions (Self-referencing schemas)
- `Definitions` stores reusable validators/serializers
- `DefinitionRefValidator` references stored definitions
- Supports recursive models like `class Node(TreeNode): children: list[Node]`

### Strict vs Lax Validation
- Strict: Exact type match (no coercion)
- Lax: Allow coercion (e.g., "123" -> 123)
- Each validator has `strict_*` and `lax_*` methods

### Partial Validation
- `allow_partial=True`: Allow incomplete JSON
- `trailing-strings`: Include unfinished strings
- Useful for streaming/incremental parsing

### String Caching
- Cache short strings during validation
- Reduces Python object creation overhead
- `StringCacheMode::All`, `Keys`, `None`

---

## 10. Performance Considerations

1. **Zero-cost dispatch**: Use variants instead of virtual functions where possible
2. **Arc/shared_ptr**: Shared ownership for recursive schemas
3. **Small vector optimization**: For small collections
4. **Lazy validation**: Don't validate if isinstance returns False
5. **JSON direct parsing**: Parse JSON directly to Python objects without intermediate repr

---

## Next Steps

1. Choose Python binding library (pybind11 vs nanobind)
2. Choose JSON library (simdjson recommended for speed)
3. Design C++ class hierarchy mirroring Rust enum_dispatch pattern
4. Implement error handling matching ValError semantics
5. Start with simplest validators (None, Bool, Int) as proof of concept