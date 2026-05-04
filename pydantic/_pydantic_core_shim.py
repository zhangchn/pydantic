"""
Shim module that switches between pydantic_core (Rust) and pydantic_core_cpp (C++) based on environment variable.

Set PYDANTIC_USE_CPP_CORE=1 to use the C++ implementation.

This module should be imported early in the pydantic package initialization
to ensure all pydantic_core imports are redirected to the correct backend.
"""
from __future__ import annotations

import os
import sys
from typing import Any

# Check if we should use the C++ implementation
_USE_CPP = os.environ.get("PYDANTIC_USE_CPP_CORE", "").lower() in ("1", "true", "yes")

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
            import pydantic_core as _core
            return _core, "rust"
    else:
        import pydantic_core as _core
        return _core, "rust"

def _setup_shim() -> None:
    """Set up the shim by redirecting pydantic_core imports in sys.modules."""
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
_setup_shim()

# Expose backend information
__backend__ = _USE_CPP and "cpp" or "rust"
__use_cpp__ = _USE_CPP
