from __future__ import annotations

import sys as _sys
from typing import Any as _Any

from typing_extensions import Sentinel

# Import everything from the native C++ extension
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

# Optional exports that may not yet be implemented in the C++ backend
try:
    from ._pydantic_core_cpp import SchemaSerializer
except ImportError:
    SchemaSerializer = None  # type: ignore[misc,assignment]

try:
    from ._pydantic_core_cpp import (
        PydanticOmit,
        PydanticUseDefault,
    )
except ImportError:
    PydanticOmit = None  # type: ignore[misc,assignment]
    PydanticUseDefault = None  # type: ignore[misc,assignment]

# Standalone convenience functions — delegate to Rust for now
import pydantic_core as _rust_core
from_json = _rust_core.from_json
to_json = _rust_core.to_json
to_jsonable_python = _rust_core.to_jsonable_python

# Error types — delegate to Rust for now
PydanticCustomError = _rust_core.PydanticCustomError
PydanticKnownError = _rust_core.PydanticKnownError

# Core schema types — delegate to Rust for type hints
from pydantic_core.core_schema import (
    CoreConfig,
    CoreSchema,
    CoreSchemaType,
)

# Error type enum — delegate to Rust
ErrorType = _rust_core.ErrorType

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
    'CoreConfig',
    'CoreSchema',
    'CoreSchemaType',
    'SchemaValidator',
    'SchemaSerializer',
    'ValidationError',
    'SchemaError',
    'PydanticOmit',
    'PydanticUseDefault',
    'PydanticCustomError',
    'PydanticKnownError',
    'from_json',
    'to_json',
    'to_jsonable_python',
    'ArgsKwargs',
    'MultiHostUrl',
    'PydanticSerializationError',
    'PydanticSerializationUnexpectedValue',
    'PydanticUndefined',
    'PydanticUndefinedType',
    'Some',
    'TzInfo',
    'Url',
    'ErrorType',
    'InputType',
    'ExtraBehavior',
    'StringCacheMode',
    'SerMode',
    'TemporalMode',
    'BytesMode',
    'InfNanMode',
]

# Sentinel value for "unset"
UNSET = Sentinel('UNSET')


def __getattr__(name: str) -> _Any:
    if name in ('ArgsKwargs', 'MultiHostUrl', 'PydanticSerializationError',
                'PydanticSerializationUnexpectedValue', 'PydanticUndefined',
                'PydanticUndefinedType', 'Some', 'TzInfo', 'Url'):
        return getattr(_rust_core, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
