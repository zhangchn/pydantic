"""
Shim module that switches between pydantic_core (Rust) and pydantic_core_cpp (C++).

C++ backend is used by default.
Set PYDANTIC_USE_CPP_CORE=0 (or "false"/"no") to fall back to the Rust implementation.
"""
from __future__ import annotations

import os
import sys
from typing import Any

# C++ is the default; opt-out by setting PYDANTIC_USE_CPP_CORE=0/false/no
_USE_CPP = os.environ.get("PYDANTIC_USE_CPP_CORE", "1").lower() not in ("0", "false", "no")

def _get_backend() -> tuple[Any, str]:
    """Get the appropriate pydantic_core backend based on environment variable."""
    if _USE_CPP:
        try:
            # Save original pydantic_core before importing C++ backend
            # The C++ backend needs to import from Rust for type stubs
            _saved_modules = {}
            for mod in list(sys.modules.keys()):
                if mod == 'pydantic_core' or mod.startswith('pydantic_core.'):
                    _saved_modules[mod] = sys.modules.pop(mod)
            
            try:
                import pydantic_core_cpp as _core
                return _core, "cpp"
            finally:
                # Restore saved modules
                sys.modules.update(_saved_modules)
        except ImportError:
            # Only warn when the C++ package is installed but failed to load
            # (e.g. a broken build) — that is surprising and hides regressions.
            # When it is simply not installed, falling back to Rust is the
            # expected default (pydantic's published dependency is the Rust
            # pydantic-core), so stay silent: a library should not emit an
            # import-time warning for a missing optional backend.
            import importlib.util
            if importlib.util.find_spec("pydantic_core_cpp") is not None:
                import warnings
                warnings.warn(
                    "pydantic-core-cpp is installed but failed to load; "
                    "falling back to the Rust pydantic-core backend. "
                    "Check the install/build of pydantic-core-cpp, or set "
                    "PYDANTIC_USE_CPP_CORE=0 to opt out explicitly (and "
                    "silence this warning).",
                    stacklevel=2,
                )
            import pydantic_core as _core
            return _core, "rust"
    else:
        import pydantic_core as _core
        return _core, "rust"

def _setup_shim() -> str:
    """Set up the shim by redirecting pydantic_core imports in sys.modules.

    Returns the backend actually selected ("cpp" or "rust"), which may differ
    from the requested one when the C++ backend is missing or fails to load.
    """
    _core, _backend = _get_backend()
    
    # Always replace pydantic_core in sys.modules with the selected backend
    # This ensures that any subsequent imports of pydantic_core will get the correct backend
    sys.modules['pydantic_core'] = _core
    
    # Also replace pydantic_core.core_schema if it exists
    try:
        if _backend == 'cpp':
            # For C++ backend, create a minimal core_schema shim
            # This is needed because pydantic imports from pydantic_core.core_schema
            _create_core_schema_shim(_core)
    except Exception:
        pass

    return _backend

def _create_core_schema_shim(_core: Any) -> None:
    """Create a minimal core_schema shim for the C++ backend."""
    # Check if core_schema is already available
    if hasattr(_core, 'core_schema'):
        sys.modules['pydantic_core.core_schema'] = _core.core_schema
        return
    
    # For C++ backend, we need to create a shim that provides the core_schema module
    # This is a temporary solution until the C++ backend has a proper core_schema module
    import types
    _shim = types.ModuleType('pydantic_core.core_schema')
    
    # Import core_schema from the Rust backend for type definitions
    # The C++ backend doesn't have core_schema yet, so we use the Rust one for types
    try:
        # Temporarily remove the shim from sys.modules to import Rust core_schema
        _original = sys.modules.get('pydantic_core')
        if 'pydantic_core' in sys.modules:
            del sys.modules['pydantic_core']
        import pydantic_core as _rust_core
        if hasattr(_rust_core, 'core_schema'):
            _shim.__dict__.update(_rust_core.core_schema.__dict__)
        # Restore the shim
        if _original:
            sys.modules['pydantic_core'] = _original
    except ImportError:
        pass
    
    sys.modules['pydantic_core.core_schema'] = _shim

# Set up the shim when this module is imported
__backend__ = _setup_shim()

# Expose backend information: __backend__ is the backend actually in use
# (it falls back to "rust" when pydantic-core-cpp is missing or broken),
# while __use_cpp__ is the requested preference from PYDANTIC_USE_CPP_CORE.
__use_cpp__ = _USE_CPP
