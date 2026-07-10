"""
pydantic_core_cpp — C++ backend for pydantic-core.

Provides the same public API as pydantic_core (Rust) by combining:
  1. Native C++ extension symbols (_pydantic_core_cpp)
  2. Lazy fallbacks to the Rust pydantic_core for unimplemented symbols
  3. Local core_schema module (type definitions and builder functions)
"""
from __future__ import annotations

import sys as _sys
from typing import Any as _Any

# ============================================================================
# 1. Native C++ extension exports (always available)
# ============================================================================
from ._pydantic_core_cpp import (
    BytesMode,
    ExtraBehavior,
    InfNanMode,
    InputType,
    PydanticOmit,
    PydanticUseDefault,
    SchemaError,
    SchemaValidator as _SchemaValidatorBase,
    SerMode,
    SerializationInfo,
    StringCacheMode,
    TemporalMode,
    ValidationError,
    __version__,
)

# Wrapper for SchemaValidator that stores the schema for model construction
class SchemaValidator:
    def __init__(self, schema, config=None, _use_prebuilt=True):
        self._schema = schema
        self._config = config
        # Convert dict to JSON string if needed
        if isinstance(schema, dict):
            import json
            schema_str = json.dumps(schema)
        elif isinstance(schema, str):
            schema_str = schema
        elif schema is None:
            schema_str = ""
        else:
            import json
            schema_str = json.dumps(schema)
        if isinstance(config, dict):
            import json
            config_str = json.dumps(config)
        elif config is None:
            config_str = None
        elif isinstance(config, str):
            config_str = config
        else:
            import json
            config_str = json.dumps(config)
        self._base = _SchemaValidatorBase(schema_str, config_str, _use_prebuilt)
        # Build a map of definition refs to model classes for recursive model construction
        self._model_classes = self._extract_model_classes(schema)

    @staticmethod
    def _extract_model_classes(schema):
        """Extract a map of definition-ref -> model class from the schema."""
        classes = {}
        if not isinstance(schema, dict):
            return classes

        # Handle definitions wrapper
        if schema.get("type") == "definitions":
            for defn in schema.get("definitions", []):
                ref = defn.get("ref")
                if ref and defn.get("type") == "model":
                    cls = defn.get("cls")
                    if cls is not None and callable(cls):
                        classes[ref] = cls
        
        # Handle direct model schema
        if schema.get("type") == "model":
            cls = schema.get("cls")
            if cls is not None and callable(cls):
                # Use a special key for the top-level model
                classes["__root__"] = cls

        return classes

    def _dict_to_model(self, data, schema=None):
        """Recursively convert dicts to model instances based on schema."""
        if schema is None:
            schema = self._schema

        if not isinstance(schema, dict):
            return data

        # Unwrapping handlers - these must run before the data type check
        # so we can find the actual inner schema type
        if schema.get("type") == "default":
            inner = schema.get("schema", {})
            return self._dict_to_model(data, inner)

        if schema.get("type") == "nullable":
            if data is None:
                return None
            inner = schema.get("schema", {})
            return self._dict_to_model(data, inner)

        if schema.get("type") == "definitions":
            inner = schema.get("schema", {})
            if inner.get("type") == "definition-ref":
                ref = inner.get("schema_ref", "__root__")
                cls = self._model_classes.get(ref)
                if cls:
                    return self._build_model(data, cls, ref)
            return self._dict_to_model(data, inner)

        if schema.get("type") == "definition-ref":
            ref = schema.get("schema_ref", "__root__")
            cls = self._model_classes.get(ref)
            if cls:
                return self._build_model(data, cls, ref)
            return data

        if schema.get("type") == "union":
            for choice in schema.get("choices", []):
                if isinstance(choice, dict):
                    result = self._dict_to_model(data, choice)
                    if result != data:
                        return result
            return data

        # Now that schema is unwrapped, check data types
        # Handle list data type
        if schema.get("type") == "list":
            items_schema = schema.get("items_schema", {})
            if isinstance(items_schema, dict) and isinstance(data, list):
                return [self._dict_to_model(item, items_schema) for item in data]
            return data

        # Handle non-dict data (return as-is for non-list schemas)
        if not isinstance(data, dict):
            return data

        if schema.get("type") == "model":
            cls = schema.get("cls")
            if cls is not None and callable(cls):
                # Direct model schema (not in definitions) — use schema's own inner schema
                instance = object.__new__(cls)
                inner_schema = schema.get("schema", {})
                if isinstance(inner_schema, dict) and inner_schema.get("type") in ("model-fields", "typed-dict"):
                    data = self._process_model_fields(data, inner_schema)
                instance.__dict__ = data
                object.__setattr__(instance, '__pydantic_private__', {})
                object.__setattr__(instance, '__pydantic_extra__', None)
                object.__setattr__(instance, '__pydantic_fields_set__', set(data.keys()))
                return instance
            return data

        if schema.get("type") == "model-fields":
            return self._process_model_fields(data, schema)

        return data

    def _build_model(self, data, cls, ref):
        """Build a model instance from dict data by looking up the definition ref.

        Looks up the definition by ref in self._schema['definitions'],
        extracts its model-fields schema, recursively processes nested fields,
        and creates the model instance.
        """
        instance = object.__new__(cls)

        # Find the definition by ref in the top-level definitions list
        for defn in self._schema.get("definitions", []):
            if defn.get("ref") == ref:
                inner_schema = defn.get("schema", {})
                if isinstance(inner_schema, dict) and inner_schema.get("type") in ("model-fields", "typed-dict"):
                    data = self._process_model_fields(data, inner_schema)
                break

        instance.__dict__ = data
        # Initialize pydantic slot attributes expected by BaseModel
        object.__setattr__(instance, '__pydantic_private__', {})
        object.__setattr__(instance, '__pydantic_extra__', None)
        object.__setattr__(instance, '__pydantic_fields_set__', set(data.keys()))
        return instance

    def _process_model_fields(self, data, fields_schema):
        """Process model fields, recursively converting nested dicts to models."""
        fields = fields_schema.get("fields", {})
        if not isinstance(fields, dict):
            return data

        result = dict(data)
        for field_name, field_def in fields.items():
            if field_name not in result:
                continue
            
            field_schema = field_def.get("schema", {})
            if isinstance(field_schema, dict):
                result[field_name] = self._dict_to_model(result[field_name], field_schema)

        return result

    @property
    def title(self):
        return self._base.title

    def validate_python(self, obj, *, strict=None, context=None, self_instance=None,
                        extra=None, from_attributes=None, by_alias=None, by_name=None):
        result = self._base.validate_python(
            obj, strict=strict, context=context, self_instance=self_instance,
            extra=extra, from_attributes=from_attributes, by_alias=by_alias, by_name=by_name)

        if isinstance(result, dict):
            # Recursively convert nested dicts to model instances
            result = self._dict_to_model(result)
            
            # If result is a model instance, return it
            if not isinstance(result, dict):
                return result
                
            # Otherwise, try to construct a model from the top-level schema
            schema_type = self._schema.get("type") if hasattr(self._schema, "get") else None
            if schema_type == "definitions":
                inner = self._schema.get("schema", {})
                if inner.get("type") == "definition-ref":
                    ref = inner.get("schema_ref", "__root__")
                    cls = self._model_classes.get(ref)
                    if cls:
                        return self._build_model(result, cls, ref)
            elif schema_type == "model":
                cls = self._schema.get("cls")
                if cls is not None and callable(cls):
                    # Process nested models in the result dict
                    inner_schema = self._schema.get("schema", {})
                    if isinstance(inner_schema, dict) and inner_schema.get("type") in ("model-fields", "typed-dict"):
                        result = self._process_model_fields(result, inner_schema)
                    instance = object.__new__(cls)
                    instance.__dict__ = result
                    object.__setattr__(instance, '__pydantic_private__', {})
                    object.__setattr__(instance, '__pydantic_extra__', None)
                    object.__setattr__(instance, '__pydantic_fields_set__', set(result.keys()))
                    return instance

        elif self_instance is not None:
            # self_instance was provided and C++ returned it.
            # Recursively convert nested dicts in __dict__ to model instances
            processed = self._dict_to_model(dict(self_instance.__dict__))
            if isinstance(processed, dict):
                self_instance.__dict__.clear()
                self_instance.__dict__.update(processed)
            else:
                # _dict_to_model returned a model instance - copy its __dict__
                if hasattr(processed, '__dict__'):
                    self_instance.__dict__.clear()
                    self_instance.__dict__.update(processed.__dict__)

        return result

    def validate_json(self, json_data, *, strict=None, context=None, extra=None,
                      from_attributes=None, by_alias=None, by_name=None):
        result = self._base.validate_json(json_data, strict=strict)
        if isinstance(result, dict):
            result = self._dict_to_model(result)
        return result

    def validate_strings(self, string_data, *, strict=None):
        return self._base.validate_strings(string_data, strict=strict)

    def isinstance_python(self, obj, *, strict=None):
        return self._base.isinstance_python(obj, strict=strict)

    def get_default_value(self, *, strict=None):
        return self._base.get_default_value(strict=strict)

    def validate_assignment(self, obj, field_name, field_value):
        return self._base.validate_assignment(obj, field_name, field_value)

    def __repr__(self):
        return self._base.__repr__()

# ============================================================================
# 2. C++ symbols that may be conditionally available
# ============================================================================

# ErrorType enum
try:
    from ._pydantic_core_cpp import ErrorType
except ImportError:
    ErrorType = None  # type: ignore[misc,assignment]

# SchemaSerializer — not yet in C++, will fall back to Rust
# Do NOT set it to None here, or __getattr__ won't be called.
try:
    from ._pydantic_core_cpp import SchemaSerializer
except ImportError:
    pass  # __getattr__ will resolve from Rust

# ============================================================================
# 2b. Pure-Python symbols (not in any native extension)
# ============================================================================

from typing_extensions import Sentinel
MISSING = Sentinel('MISSING')

# ============================================================================
# 3. Rust backend fallback (lazy, only on first access)
# ============================================================================

# Symbols implemented in Rust but not yet in C++.
# These are resolved lazily via __getattr__ to avoid importing
# the Rust backend at module load time.
_RUST_FALLBACKS = frozenset({
    # Data types (from native extension)
    'MultiHostUrl',
    'Some',
    'TzInfo',
    'Url',
    # Sentinels (from native extension)
    'PydanticUndefined',
    'PydanticUndefinedType',
    # Errors / exceptions (from native extension)
    'PydanticSerializationError',
    'PydanticSerializationUnexpectedValue',
    # Serializer (from native extension)
    'SchemaSerializer',
    # Error type enum (from core_schema if C++ doesn't have it)
    'ErrorType',
})

# Symbols that come from core_schema rather than the native extension
_CORE_SCHEMA_FALLBACKS = frozenset({
    'CoreConfig',
    'CoreSchema',
    'CoreSchemaType',
})


_RUST_NATIVE_MODULE = None


def _find_rust_native_path() -> str | None:
    """Find the Rust pydantic_core native extension .so file on disk.

    Since the shim may have replaced sys.modules['pydantic_core'] with this
    C++ module, we can't rely on standard import machinery. Instead, we
    search site-packages for the actual .so file.
    """
    import site
    import os
    import glob
    for sp in site.getsitepackages() + [site.getusersitepackages()]:
        # Pattern: pydantic_core/_pydantic_core.cpython-*.so (or .pyd on Windows)
        pattern = os.path.join(sp, 'pydantic_core', '_pydantic_core.*.so')
        matches = glob.glob(pattern)
        if matches:
            return matches[0]
        pattern = os.path.join(sp, 'pydantic_core', '_pydantic_core.*.pyd')
        matches = glob.glob(pattern)
        if matches:
            return matches[0]
    return None


def _rust() -> _Any:
    """
    Lazy import of the Rust pydantic_core backend.

    We load the native extension `_pydantic_core` directly from its
    .so file on disk, because the shim may have replaced
    sys.modules['pydantic_core'] with this module (cpp), which would
    cause import machinery to fail.
    """
    global _RUST_NATIVE_MODULE
    if _RUST_NATIVE_MODULE is not None:
        return _RUST_NATIVE_MODULE

    import importlib.util
    import types

    # Find the .so file directly on disk
    so_path = _find_rust_native_path()
    if so_path is None:
        raise ImportError(
            "Cannot find Rust pydantic_core native extension. "
            "Install pydantic-core (Rust) as a fallback backend."
        )

    # Load the extension module directly from the .so file
    spec = importlib.util.spec_from_file_location(
        'pydantic_core._pydantic_core', so_path
    )
    _native = importlib.util.module_from_spec(spec)
    # Add to sys.modules temporarily so relative imports work inside the extension
    _sys.modules.setdefault('pydantic_core._pydantic_core', _native)
    spec.loader.exec_module(_native)  # type: ignore[union-attr]

    # Build a wrapper that exposes all native symbols
    _wrapper: dict[str, _Any] = {}
    for name in dir(_native):
        if not name.startswith('_'):
            _wrapper[name] = getattr(_native, name)

    # Also grab __version__ from the rust package
    try:
        _rust_pkg = importlib.import_module('pydantic_core')
        if hasattr(_rust_pkg, '__version__'):
            _wrapper['__version__'] = _rust_pkg.__version__
    except Exception:
        pass

    _RUST_NATIVE_MODULE = types.SimpleNamespace(**_wrapper)
    return _RUST_NATIVE_MODULE


def __getattr__(name: str) -> _Any:
    if name in _CORE_SCHEMA_FALLBACKS:
        return getattr(core_schema, name)
    if name in _RUST_FALLBACKS:
        return getattr(_rust(), name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


# ============================================================================
# 4. Standalone functions (delegate to Rust until C++ implements them)
# ============================================================================

def from_json(*args: _Any, **kwargs: _Any) -> _Any:
    return _rust().from_json(*args, **kwargs)


def to_json(*args: _Any, **kwargs: _Any) -> _Any:
    return _rust().to_json(*args, **kwargs)


def to_jsonable_python(*args: _Any, **kwargs: _Any) -> _Any:
    return _rust().to_jsonable_python(*args, **kwargs)


# ============================================================================
# 4.5. Data types implemented in Python
# ============================================================================

class ArgsKwargs:
    """Container for positional and keyword arguments.
    
    Used by dataclass validation to support positional arguments in __init__.
    Matches pydantic_core.ArgsKwargs from Rust implementation.
    """
    
    __slots__ = ('args', 'kwargs')
    
    def __init__(self, args: tuple = (), kwargs: dict = None) -> None:
        if kwargs is None:
            kwargs = {}
        self.args = args if isinstance(args, tuple) else tuple(args)
        self.kwargs = kwargs if isinstance(kwargs, dict) else dict(kwargs)
    
    def __repr__(self) -> str:
        if not self.kwargs:
            return f"ArgsKwargs({self.args!r})"
        return f"ArgsKwargs({self.args!r}, {self.kwargs!r})"
    
    def __eq__(self, other: object) -> bool:
        if not isinstance(other, ArgsKwargs):
            return NotImplemented
        return self.args == other.args and self.kwargs == other.kwargs
    
    def __hash__(self) -> int:
        return hash((self.args, frozenset(self.kwargs.items())))
    
    def to_call_args(self) -> tuple:
        """Convert to arguments suitable for a function call.
        
        Returns a tuple of (args, kwargs) that can be used as:
        func(*args, **kwargs)
        """
        return (self.args, self.kwargs)


# ============================================================================
# 4.6. Exception classes (pure Python, no Rust dependency)
# ============================================================================

class PydanticCustomError(ValueError):
    """A custom exception providing flexible error handling for Pydantic validators.

    Matches Rust's PydanticCustomError pyclass.

    Arguments:
        error_type: The error type.
        message_template: The message template.
        context: The data to inject into the message template.
    """

    def __init__(
        self,
        error_type: str,
        message_template: str,
        context: dict[str, _Any] | None = None,
    ) -> None:
        self._type = error_type
        self._message_template = message_template
        self._context = context or {}
        super().__init__(message_template.format(**self._context))

    @property
    def type(self) -> str:
        """The error type associated with the error."""
        return self._type

    @property
    def message_template(self) -> str:
        """The message template associated with the error."""
        return self._message_template

    @property
    def context(self) -> dict[str, _Any] | None:
        """Values which are required to render the error message."""
        return self._context

    def message(self) -> str:
        """The formatted message associated with the error."""
        return self._message_template.format(**self._context)


class PydanticKnownError(ValueError):
    """A helper class for raising exceptions that mimic Pydantic's built-in exceptions.

    Unlike PydanticCustomError, the error_type argument must be a known ErrorType.

    Arguments:
        error_type: The error type.
        context: The data to inject into the message template.
    """

    def __init__(
        self,
        error_type: str,
        context: dict[str, _Any] | None = None,
    ) -> None:
        self._type = error_type
        self._context = context or {}
        # PydanticKnownError does not format the message at init time
        super().__init__(str(error_type))

    @property
    def type(self) -> str:
        """The type of the error."""
        return self._type

    @property
    def message_template(self) -> str:
        """The message template associated with the provided error type."""
        return self._type

    @property
    def context(self) -> dict[str, _Any] | None:
        """Values which are required to render the error message."""
        return self._context

    def message(self) -> str:
        """The formatted message associated with the error."""
        return self._type


# ============================================================================
# 5. core_schema module — now a local Python module
# ============================================================================

# The core_schema module contains all the schema builder functions and type
# definitions (int_schema, str_schema, model_fields_schema, etc.).
# Pydantic imports `from pydantic_core import core_schema` extensively.
#
# Previously this was proxied to the Rust pydantic_core.core_schema module.
# Now it's a local copy in pydantic_core_cpp/core_schema.py.

from . import core_schema
_sys.modules['pydantic_core_cpp.core_schema'] = core_schema


def __dir__() -> list[str]:
    return list(__all__)

# ============================================================================
# 6. Module metadata
# ============================================================================

if _sys.version_info < (3, 11):
    from typing_extensions import NotRequired as _NotRequired
else:
    from typing import NotRequired as _NotRequired

if _sys.version_info < (3, 12):
    from typing_extensions import TypedDict as _TypedDict
else:
    from typing import TypedDict as _TypedDict

__all__: list[str] = [
    '__version__',
    # Core validation
    'SchemaValidator',
    'SchemaSerializer',
    'ValidationError',
    'SchemaError',
    # Sentinels
    'PydanticUndefined',
    'PydanticUndefinedType',
    'MISSING',
    # Exceptions
    'PydanticCustomError',
    'PydanticKnownError',
    'PydanticOmit',
    'PydanticUseDefault',
    'PydanticSerializationError',
    'PydanticSerializationUnexpectedValue',
    # Functions
    'from_json',
    'to_json',
    'to_jsonable_python',
    # Types
    'ArgsKwargs',
    'Some',
    'Url',
    'MultiHostUrl',
    'TzInfo',
    # Type aliases
    'CoreConfig',
    'CoreSchema',
    'CoreSchemaType',
    # Enums
    'InputType',
    'ExtraBehavior',
    'StringCacheMode',
    'SerMode',
    'TemporalMode',
    'BytesMode',
    'InfNanMode',
    'ErrorType',
    # Sub-modules
    'core_schema',
]
