"""The `version` module holds the version information for Pydantic."""

from __future__ import annotations as _annotations

import sys

# Prefer pydantic_core_cpp (C++ backend), fall back to pydantic_core (Rust backend)
try:
    import pydantic_core_cpp as _core_pkg
except ImportError:
    import pydantic_core as _core_pkg

__pydantic_core_version__ = _core_pkg.__version__
__pydantic_core_name__ = _core_pkg.__name__

__all__ = 'VERSION', 'version_info', '__pydantic_core_version__', '__pydantic_core_name__'

VERSION = '2.14.0a0+dev'
"""The version of Pydantic.

This version specifier is guaranteed to be compliant with the [specification],
introduced by [PEP 440].

[specification]: https://packaging.python.org/en/latest/specifications/version-specifiers/
[PEP 440]: https://peps.python.org/pep-0440/
"""

# Keep this in sync with the version constraint in the `pyproject.toml` dependencies:
_COMPATIBLE_PYDANTIC_CORE_VERSION = '2.46.4'


def version_short() -> str:
    """Return the `major.minor` part of Pydantic version.

    It returns '2.1' if Pydantic version is '2.1.1'.
    """
    return '.'.join(VERSION.split('.')[:2])


def version_info() -> str:
    """Return complete version information for Pydantic and its dependencies."""
    import importlib.metadata
    import platform
    from pathlib import Path

    from ._internal import _git as git

    # Use whichever core backend is active
    _pdc = _core_pkg

    # get data about packages that are closely related to pydantic, use pydantic or often conflict with pydantic
    package_names = {
        'email-validator',
        'fastapi',
        'mypy',
        'pydantic-extra-types',
        'pydantic-settings',
        'pyright',
        'typing_extensions',
    }
    related_packages = []

    for dist in importlib.metadata.distributions():
        name = dist.metadata['Name']
        if name in package_names:
            related_packages.append(f'{name}-{dist.version}')

    pydantic_dir = Path(__file__).parents[1].resolve()
    most_recent_commit = (
        git.git_revision(pydantic_dir) if git.is_git_repo(pydantic_dir) and git.have_git() else 'unknown'
    )

    build_info = getattr(_pdc, 'build_info', None) or getattr(_pdc, 'build_profile', None)

    info = {
        'pydantic version': VERSION,
        'pydantic-core version': __pydantic_core_version__,
        'pydantic-core variant': __pydantic_core_name__,
        'pydantic-core build': build_info,
        'python version': sys.version,
        'platform': platform.platform(),
        'related packages': ' '.join(related_packages),
        'commit': most_recent_commit,
    }
    return '\n'.join('{:>30} {}'.format(k + ':', str(v).replace('\n', ' ')) for k, v in info.items())


def check_pydantic_core_version() -> bool:
    """Check that the installed `pydantic-core` (or `pydantic-core-cpp`) dependency is compatible."""
    return __pydantic_core_version__ == _COMPATIBLE_PYDANTIC_CORE_VERSION


def _ensure_pydantic_core_version() -> None:  # pragma: no cover
    if not check_pydantic_core_version():
        raise_error = True
        # Do not raise the error if pydantic is installed in editable mode (i.e. in development):
        if sys.version_info >= (3, 13):  # origin property added in 3.13
            from importlib.metadata import distribution

            dist = distribution('pydantic')
            if getattr(getattr(dist.origin, 'dir_info', None), 'editable', False):
                raise_error = False

        if raise_error:
            raise SystemError(
                f'The installed {_core_pkg.__name__} version ({__pydantic_core_version__}) is incompatible '
                f'with the current pydantic version, which requires {_COMPATIBLE_PYDANTIC_CORE_VERSION}. '
                f"If you encounter this error, make sure that you haven't upgraded {_core_pkg.__name__} manually."
            )


def parse_mypy_version(version: str) -> tuple[int, int, int]:
    """Parse `mypy` string version to a 3-tuple of ints.

    It parses normal version like `1.11.0` and extra info followed by a `+` sign
    like `1.11.0+dev.d6d9d8cd4f27c52edac1f537e236ec48a01e54cb.dirty`.

    Args:
        version: The mypy version string.

    Returns:
        A triple of ints, e.g. `(1, 11, 0)`.
    """
    return tuple(map(int, version.partition('+')[0].split('.')))  # pyright: ignore[reportReturnType]
