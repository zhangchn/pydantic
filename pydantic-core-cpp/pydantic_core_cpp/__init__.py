from __future__ import annotations

import sys as _sys
from typing import Any as _Any

from typing_extensions import Sentinel

# ============================================================================
# Native C++ extension exports
# ============================================================================
from ._pydantic_core_cpp import (
    BytesMode,
    ExtraBehavior,
    InfNanMode,
    InputType,
    SchemaError,
    SchemaValidator,
    SerMode,
    StringCacheMode,
    TemporalMode,
    ValidationError,
    __version__,
)

# Optional exports that may not yet be in the C++ build
try:
    from ._pydantic_core_cpp import SchemaSerializer
except ImportError:
    SchemaSerializer = None  # type: ignore[misc,assignment]

try:
    from ._pydantic_core_cpp import PydanticOmit, PydanticUseDefault
except ImportError:
    PydanticOmit = None  # type: ignore[misc,assignment]
    PydanticUseDefault = None  # type: ignore[misc,assignment]

try:
    from ._pydantic_core_cpp import PydanticKnownError, PydanticCustomError
except ImportError:
    PydanticKnownError = None  # type: ignore[misc,assignment]
    PydanticCustomError = None  # type: ignore[misc,assignment]

try:
    from ._pydantic_core_cpp import ErrorType
except ImportError:
    ErrorType = None  # type: ignore[misc,assignment]

# ============================================================================
# Standalone functions (delegate to Rust backend until C++ implements them)
# ============================================================================

def _rust() -> _Any:
    """Lazy import of the Rust pydantic_core backend."""
    import pydantic_core
    return pydantic_core


def from_json(*args: _Any, **kwargs: _Any) -> _Any:
    return _rust().from_json(*args, **kwargs)


def to_json(*args: _Any, **kwargs: _Any) -> _Any:
    return _rust().to_json(*args, **kwargs)


def to_jsonable_python(*args: _Any, **kwargs: _Any) -> _Any:
    return _rust().to_jsonable_python(*args, **kwargs)


# ============================================================================
# Type-only re-exports from Rust (for IDE/type-checker compatibility)
# ============================================================================

def __getattr__(name: str) -> _Any:
    if name in (
        'ArgsKwargs', 'MultiHostUrl', 'PydanticSerializationError',
        'PydanticSerializationUnexpectedValue', 'PydanticUndefined',
        'PydanticUndefinedType', 'Some', 'TzInfo', 'Url',
        # Type hints
        'CoreConfig', 'CoreSchema', 'CoreSchemaType',
    ):
        return getattr(_rust(), name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


# ============================================================================
# Module metadata
# ============================================================================

if _sys.version_info < (3, 11):
    from typing_extensions import NotRequired as _NotRequired
else:
    from typing import NotRequired as _NotRequired

if _sys.version_info < (3, 12):
    from typing_extensions import TypedDict as _TypedDict
else:
    from typing import TypedDict as _TypedDict

__all__ = [
    '__version__',
    # Core classes
    'SchemaValidator',
    'SchemaSerializer',
    'ValidationError',
    'SchemaError',
    # Exceptions / sentinels
    'PydanticOmit',
    'PydanticUseDefault',
    'PydanticCustomError',
    'PydanticKnownError',
    # Standalone functions
    'from_json',
    'to_json',
    'to_jsonable_python',
    # Enums
    'InputType',
    'ExtraBehavior',
    'StringCacheMode',
    'SerMode',
    'TemporalMode',
    'BytesMode',
    'InfNanMode',
    'ErrorType',
    # Re-exported from Rust (type hints)
    'CoreConfig',
    'CoreSchema',
    'CoreSchemaType',
    'ArgsKwargs',
    'MultiHostUrl',
    'PydanticSerializationError',
    'PydanticSerializationUnexpectedValue',
    'PydanticUndefined',
    'PydanticUndefinedType',
    'Some',
    'TzInfo',
    'Url',
]

UNSET = Sentinel('UNSET')
