"""
pydantic_core_cpp — C++ backend for pydantic-core.

Provides the same public API as pydantic_core (Rust) by combining:
  1. Native C++ extension symbols (_pydantic_core_cpp)
  2. Lazy fallbacks to the Rust pydantic_core for unimplemented symbols
  3. Local core_schema module (type definitions and builder functions)
"""
from __future__ import annotations

import copy as _copy
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
    MultiHostUrl,
    PydanticOmit,
    PydanticUseDefault,
    SchemaError,
    SchemaSerializer,
    SchemaValidator as _SchemaValidatorBase,
    SerMode,
    SerializationInfo,
    StringCacheMode,
    TemporalMode,
    Url,
    ValidationError,
    __version__,
    to_json,
    to_jsonable_python,
)

from typing import TypedDict as _TypedDict


class _ValidationInfo:
    """Helper class that wraps a dict with attribute access.

    Used by C++ FunctionAfterValidator/FunctionBeforeValidator to pass
    validation info to Python callable validators. Missing attributes
    return None instead of raising AttributeError.
    """

    def __init__(self, info_dict: dict):
        self._info_dict = dict(info_dict) if info_dict else {}

    def __getattr__(self, name: str) -> object:
        if name.startswith('_'):
            raise AttributeError(name)
        return self._info_dict.get(name, None)


class MultiHostHost(_TypedDict):
    """A host part of a multi-host URL."""
    username: str | None
    password: str | None
    host: str | None
    port: int | None


# Patch ValidationError.errors() to support include_url parameter
# (pybind11 register_exception doesn't support C++ cast for custom methods)
_orig_errors = ValidationError.errors

# Reverse mapping from error message text -> (type, user-facing message)
_ERROR_MSG_MAP: dict[str, tuple[str, str]] = {
    'Missing field': ('missing', 'Field required'),
    'Field required': ('missing', 'Field required'),
    'Input should be a valid integer': ('int_parsing', 'Input should be a valid integer, unable to parse string as an integer'),
    'Input should be a valid string': ('string_type', 'Input should be a valid string'),
    'Input should be a valid boolean': ('bool_type', 'Input should be a valid boolean'),
    'Input should be a valid float': ('float_parsing', 'Input should be a valid number, unable to parse string as a number'),
    'Input should be a valid list': ('list_type', 'Input should be a valid list'),
    'Input should be a valid dict': ('dict_type', 'Input should be a valid dictionary'),
    'Input should be a valid set': ('set_type', 'Input should be a valid set'),
    'Input should be a valid tuple': ('tuple_type', 'Input should be a valid tuple'),
    'none is not an allowed value': ('none_required', 'Input should be None'),
    'Value error, ': ('value_error', 'Value error'),
}


def _errors_with_include_url(self, *args, include_url: bool = True, **kwargs):
    """Return the list of validation error details.

    Args:
        include_url: Whether to include a ``url`` key linking to
            pydantic error documentation (default ``True``).
    """
    try:
        result = _orig_errors(self, *args, **kwargs)
        # C++ register_exception may silently return empty list (cast failure)
        if not result:
            raise RuntimeError("C++ errors() returned empty list, likely cast failure")
    except (RuntimeError, TypeError):
        # C++ register_exception doesn't support self.cast<>, so
        # _orig_errors fails. Parse from stored original C++ message.
        cpp_msg = getattr(self, '_cpp_msg', '')
        if not cpp_msg:
            # Fallback: get raw message before any Python patching
            cpp_msg = _orig_str(self)
        result = _parse_errors_from_message(cpp_msg)
    if not include_url:
        for err in result:
            err.pop("url", None)
    return result


def _parse_errors_from_message(msg: str) -> list[dict]:
    """Parse ValidationError.what() message into a list of error dicts.

    Format::
        N validation error(s) for <title>
        <loc>
          <msg> [type=<type>, input_value=<input>]
        <loc2>
          <msg2> [type=<type2>, input_value=<input2>]
    """
    import re as _re
    result: list[dict] = []
    lines = msg.split('\n')
    if not lines:
        return result

    # C++ message -> Rust-compatible message mapping
    _MSG_MAP = {
        'Field required': 'Field required',
        'Missing field': 'Field required',
    }

    def _parse_input(raw: str):
        """Try to parse input_value string as a Python literal (dict, list, etc.)."""
        import ast as _ast
        s = raw.strip().strip("'")
        if not s:
            return s
        try:
            return _ast.literal_eval(s)
        except Exception:
            return s

    # Type name mapping: C++ -> Rust-compatible
    # NOTE: float_type/int_type/bool_type should NOT be mapped to float_parsing/int_parsing/bool_parsing.
    # The Rust backend uses float_type when None is passed (type mismatch), and float_parsing when
    # a string fails to parse to a number. The C++ backend also produces float_type for None input,
    # so it must remain float_type.
    _TYPE_MAP = {
        'model_type': 'model_type',
        'url_type': 'url_parsing',
        'url_scheme': 'url_scheme',
        'url_host': 'url_host',
        'value_error': 'value_error',
        'missing': 'missing',
    }

    i = 1
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        if not line:
            continue

        # Check if this line has a [type=...] suffix
        match = _re.search(r'\[type=([^,\]]+)(?:,\s*input_value=([^\]]*))?\]$', line)
        if match:
            # Single-line error: just the message (no location or location on previous line)
            err_type = _TYPE_MAP.get(match.group(1), match.group(1))
            raw_input = (match.group(2) or '').strip()
            input_value = _parse_input(raw_input)
            cpp_msg = line[:match.start()].strip()
            display_msg = _MSG_MAP.get(cpp_msg, cpp_msg)
            result.append({
                'type': err_type,
                'loc': (),
                'msg': display_msg,
                'input': input_value,
                'url': f'https://errors.pydantic.dev/2.14/v/{err_type}',
            })
        elif i < len(lines):
            # Check if NEXT line is a message with [type=...]
            next_line = lines[i].strip()
            next_match = _re.search(r'\[type=([^,\]]+)(?:,\s*input_value=([^\]]*))?\]$', next_line)
            if next_match:
                # Two-line format: loc_line, then msg_line
                err_type = _TYPE_MAP.get(next_match.group(1), next_match.group(1))
                raw_input = (next_match.group(2) or '').strip()
                input_value = _parse_input(raw_input)
                cpp_msg = next_line[:next_match.start()].strip()
                display_msg = _MSG_MAP.get(cpp_msg, cpp_msg)
                result.append({
                    'type': err_type,
                    'loc': (line,),
                    'msg': display_msg,
                    'input': input_value,
                    'url': f'https://errors.pydantic.dev/2.14/v/{err_type}',
                })
                i += 1

    return result


ValidationError.errors = _errors_with_include_url


# ============================================================================
# Patch ValidationError.__str__ to match Rust error message format
# ============================================================================

# Map C++ message text -> Rust-compatible (type, msg, input_value, input_type)
_ERR_MSG_TO_RUST: dict[str, tuple[str, str, str, str]] = {
    'Missing field':           ('missing',         'Field required',                                                                                                   '{}', 'dict'),
    'Field required':          ('missing',         'Field required',                                                                                                   '{}', 'dict'),
    'none is not an allowed value': ('none_required', 'Input should be None',                                                                                          'None', 'NoneType'),
    'Input should be a valid integer':  ('int_parsing',  'Input should be a valid integer, unable to parse string as an integer',                                      "''", 'str'),
    'Input should be a valid string':   ('string_type',   'Input should be a valid string',                                                                            "''", 'str'),
    'Input should be a valid boolean':  ('bool_type',     'Input should be a valid boolean',                                                                           'False', 'bool'),
    'Input should be a valid number':   ('float_parsing', 'Input should be a valid number, unable to parse string as a number',                                       "''", 'str'),
    'Input should be a valid list':     ('list_type',      'Input should be a valid list',                                                                             '[]', 'list'),
    'Input should be a valid dict':     ('dict_type',      'Input should be a valid dictionary',                                                                       '{}', 'dict'),
    'Input should be a valid set':      ('set_type',       'Input should be a valid set',                                                                              'set()', 'set'),
    'Input should be a valid tuple':    ('tuple_type',     'Input should be a valid tuple',                                                                             '()', 'tuple'),
    'Value error, ':                    ('value_error',    'Value error',                                                                                              "''", 'str'),
}

# Map C++ error type_name -> human-readable message (Rust style)
_ERR_TYPE_TO_MSG: dict[str, str] = {
    'missing': 'Field required',
    'int_parsing': 'Input should be a valid integer, unable to parse string as an integer',
    'string_type': 'Input should be a valid string',
    'bool_type': 'Input should be a valid boolean',
    'float_parsing': 'Input should be a valid number, unable to parse string as a number',
    'list_type': 'Input should be a valid list',
    'dict_type': 'Input should be a valid dictionary',
    'set_type': 'Input should be a valid set',
    'tuple_type': 'Input should be a valid tuple',
    'none_required': 'Input should be None',
    'value_error': 'Value error',
    'field_required': 'Field required',
    'model_type': 'Input should be a valid dictionary or instance',
    'string_too_short': 'String should have at least {min_length} characters',
    'string_too_long': 'String should have at most {max_length} characters',
}


def _get_model_name(schema: dict | None) -> str:
    """Extract the model name from a pydantic schema dict."""
    if not isinstance(schema, dict):
        return ''
    title = schema.get('title', '')
    if title:
        return title
    ref = schema.get('ref', '')
    if ref:
        # ref format: 'module.ClassName:hex_id' — extract just ClassName
        name = ref.split(':')[0].rsplit('.', 1)[-1]
        if name:
            return name
        return ref
    cls = schema.get('cls')
    if cls is not None:
        return getattr(cls, '__name__', '')
    # Check definitions wrapper
    if schema.get('type') == 'definitions':
        inner = schema.get('schema', {})
        if isinstance(inner, dict):
            return _get_model_name(inner)
        defs = schema.get('definitions', [])
        if defs:
            first = defs[0]
            title = first.get('title', '')
            if title:
                return title
            ref = first.get('ref', '')
            if ref:
                return ref
            cls = first.get('cls')
            if cls is not None:
                return getattr(cls, '__name__', '')
    return ''


def _format_rust_error(msg: str, model_name: str = '') -> str:
    """Reformat a C++ ValidationError message to Rust-compatible format."""
    lines = msg.strip().split('\n')
    if not lines:
        return msg

    # Parse the C++ format: "N validation error(s) for Schema"
    header = lines[0].strip()
    count = 1
    if header.startswith('1 validation'):
        count = 1
    elif header.startswith('2 validation'):
        count = 2
    # extract count from string like "1 validation error(s) for Schema"
    import re
    m = re.match(r'(\d+)', header)
    if m:
        count = int(m.group(1))

    label = 'validation error' if count == 1 else 'validation errors'
    name = model_name or 'Schema'

    # Build the Rust-style header
    result = [f'{count} {label} for {name}']

    # Parse error entries: alternating loc and message lines
    i = 1
    while i < len(lines):
        loc_line = lines[i].strip()
        if not loc_line:
            i += 1
            continue
        # Message line follows (indented)
        if i + 1 < len(lines):
            msg_line = lines[i + 1].strip()
            if msg_line.startswith('  ') or msg_line:
                msg_line = msg_line.lstrip()
                # Look up Rust-compatible error info
                err_type = 'value_error'
                rust_msg = msg_line
                input_val = ''
                input_type = ''
                for pattern, (etype, emsg, ival, itype) in _ERR_MSG_TO_RUST.items():
                    if msg_line.startswith(pattern) or pattern in msg_line:
                        err_type = etype
                        rust_msg = emsg
                        input_val = ival
                        input_type = itype
                        break

                result.append(f'{loc_line}')
                result.append(f'  {rust_msg} [type={err_type}, input_value={input_val}, input_type={input_type}]')
                i += 2
                continue
        result.append(loc_line)
        i += 1

    return '\n'.join(result)


# Patch ValidationError.__str__
_orig_str = ValidationError.__str__


def _patched_str(self) -> str:
    """Rust-compatible error message string."""
    cpp_msg = _orig_str(self)
    model_name = getattr(self, '_model_name', '')
    return _format_rust_error(cpp_msg, model_name)


ValidationError.__str__ = _patched_str
ValidationError.__repr__ = _patched_str

def _extract_enum_classes(schema: dict) -> dict[str, type]:
    """Extract Python Enum classes from enum-type schema nodes.

    Returns a mapping ``{QualifiedName.name: EnumClass}`` for each enum member,
    so that ``"FooEnum.foo"`` can be resolved back to ``FooEnum.foo`` later.
    """
    from enum import Enum as _Enum

    enum_classes: dict[str, type] = {}

    def _walk(node):
        if not isinstance(node, dict):
            return
        if node.get('type') == 'enum':
            members = node.get('members')
            if isinstance(members, (list, tuple)):
                for m in members:
                    if isinstance(m, _Enum):
                        qualified_name = f'{type(m).__qualname__}.{m.name}'
                        enum_classes[qualified_name] = type(m)
                        # Also store value -> class mapping for value-based matching
                        value_key = str(m.value)
                        if value_key not in enum_classes:
                            enum_classes[value_key] = type(m)
                        # Map value -> qualified_name for reverse lookup
                        enum_classes[f'__value__:{value_key}'] = qualified_name
        for key, value in list(node.items()):
            if isinstance(value, dict):
                _walk(value)
            elif isinstance(value, list):
                for item in value:
                    if isinstance(item, dict):
                        _walk(item)

    _walk(schema)
    return enum_classes


def _convert_enum_members(schema: dict) -> None:
    """Recursively convert Enum member objects in schema to their string names.

    Pydantic's schema generation embeds Python Enum member objects (e.g.,
    ``FooEnum.foo``) in the ``members`` list of enum-type schemas. These must
    be converted to their qualified name representation (e.g. ``"FooEnum.foo"``)
    before JSON serialization.
    """
    from enum import Enum as _Enum

    if not isinstance(schema, dict):
        return

    # Handle this node
    if schema.get('type') == 'enum':
        members = schema.get('members')
        if isinstance(members, (list, tuple)):
            converted = []
            for m in members:
                if isinstance(m, _Enum):
                    # Enum member: use QualifiedName.name format
                    qualified_name = f'{type(m).__qualname__}.{m.name}'
                    converted.append(qualified_name)
                    # Also add the raw value so both 'Foo.FOO' and 'foo' match
                    if not isinstance(m.value, _Enum):
                        converted.append(str(m.value))
                else:
                    converted.append(m)
            schema['members'] = converted

    # Recurse into nested schema structures
    for key, value in list(schema.items()):
        if key == 'members':
            continue  # Already handled above for enum
        if isinstance(value, dict):
            _convert_enum_members(value)
        elif isinstance(value, list):
            for item in value:
                if isinstance(item, dict):
                    _convert_enum_members(item)


def _find_function_after_callable(schema: dict, callables: list | None = None, seen: set | None = None) -> list:
    """Recursively find all ``function-after`` schemas and extract their callables.

    Returns a list of unique callables (deduplicated by identity).
    """
    if callables is None:
        callables = []
        seen = set()
    if not isinstance(schema, dict):
        return callables
    if schema.get('type') == 'function-after':
        func_ref = schema.get('function', {})
        if isinstance(func_ref, dict):
            func = func_ref.get('function', func_ref)
        else:
            func = func_ref
        if callable(func) and id(func) not in seen:
            seen.add(id(func))
            callables.append(func)
        # Don't recurse into children — the callable is the only thing we need
        return callables
    for v in schema.values():
        if isinstance(v, dict):
            _find_function_after_callable(v, callables, seen)
        elif isinstance(v, list):
            for item in v:
                if isinstance(item, dict):
                    _find_function_after_callable(item, callables, seen)
    return callables


class SchemaValidator:
    def __init__(self, schema, config=None, _use_prebuilt=True):
        # Do NOT mutate the caller's schema dict: model `__pydantic_core_schema__`
        # dicts are shared and re-read by pydantic when the model appears as a
        # nested field of another model. Removing ``cls`` in place would lose the
        # nested model class for later validators, so class/enum extraction is
        # read-only and C++ receives a cls-free deep copy instead.
        self._schema = schema
        self._config = config
        # Extract & remove model class references BEFORE C++ construction
        self._model_classes = self._extract_model_classes(schema)

        # Extract enum classes for later string→Enum member conversion
        self._enum_classes = _extract_enum_classes(schema)

        # Convert config to dict for C++ (which accepts py::dict directly)
        if isinstance(config, dict):
            config_dict = config
        elif config is None:
            config_dict = {}
        else:
            config_dict = dict(config) if hasattr(config, 'items') else {}

        # Pass schema dict directly to C++ — no JSON serialization (like Rust!)
        cpp_schema = _copy.deepcopy(schema)
        _schema_clean_cls_keys(cpp_schema)
        self._base = _SchemaValidatorBase(cpp_schema, config_dict)

    @staticmethod
    def _extract_model_classes(schema):
        """Extract and remove model class references from the schema dict.

        Pydantic's schema generation embeds Python class objects under ``cls``
        keys both for the top-level model and for nested inline model schemas.
        They are indexed by ``ref`` (the top-level model also under ``__root__``)
        so later dict-to-model conversion can reconstruct model instances.
        """
        classes = {}
        if not isinstance(schema, dict):
            return classes

        def _collect(d):
            if isinstance(d, dict):
                if d.get("type") == "model":
                    ref = d.get("ref")
                    cls = d.get("cls")
                    if ref and cls is not None and callable(cls):
                        classes[ref] = cls
                for v in d.values():
                    _collect(v)
            elif isinstance(d, list):
                for item in d:
                    _collect(item)

        _collect(schema)

        # Use a special key for the top-level model
        top_cls = schema.get("cls")
        if top_cls is not None and callable(top_cls):
            classes["__root__"] = top_cls

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

        # Unwrap function-wrapper types (function-before, function-after, function-wrap)
        # These wrap the actual validator schema with a Python function
        if schema.get("type") in ("function-after", "function-before", "function-wrap"):
            inner = schema.get("schema", {})
            if isinstance(inner, dict):
                return self._dict_to_model(data, inner)
            return data

        # Convert enum strings back to Python Enum instances
        if schema.get("type") == "enum":
            if isinstance(data, str) and self._enum_classes:
                # Check if data is a qualified name like "Foo.FOO"
                enum_cls = self._enum_classes.get(data)
                if enum_cls is not None and isinstance(enum_cls, type) and hasattr(enum_cls, '__members__'):
                    member_name = data.rsplit('.', 1)[-1]
                    try:
                        return enum_cls[member_name]
                    except (KeyError, TypeError):
                        pass
                # Check if data is a value like "foo" — look up the qualified name first
                value_key = f'__value__:{data}'
                qualified_name = self._enum_classes.get(value_key)
                if qualified_name:
                    member_name = qualified_name.rsplit('.', 1)[-1]
                    enum_cls = self._enum_classes.get(qualified_name)
                    if enum_cls is not None and isinstance(enum_cls, type) and hasattr(enum_cls, '__members__'):
                        try:
                            return enum_cls[member_name]
                        except (KeyError, TypeError):
                            pass

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
            if cls is None:
                # Nested inline model schemas have their cls removed during
                # extraction; recover it via the schema ref
                cls = self._model_classes.get(schema.get("ref"))
            inner_schema = schema.get("schema", {})
            if isinstance(inner_schema, dict) and inner_schema.get("type") in ("model-fields", "typed-dict"):
                data = self._process_model_fields(data, inner_schema)
            if cls is not None and callable(cls):
                # Direct model schema (not in definitions) — use schema's own inner schema
                extra = data.pop('__pydantic_extra__', None)
                fields_set = data.pop('__pydantic_fields_set__', set(data.keys()))
                data.pop('__pydantic_defaults__', None)
                instance = object.__new__(cls)
                instance.__dict__ = data
                object.__setattr__(instance, '__pydantic_private__', {})
                object.__setattr__(instance, '__pydantic_extra__', extra)
                object.__setattr__(instance, '__pydantic_fields_set__', fields_set)
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

        # Extract __pydantic_extra__ from data if present
        extra = data.pop('__pydantic_extra__', None)
        fields_set = data.pop('__pydantic_fields_set__', set(data.keys()))
        data.pop('__pydantic_defaults__', None)

        instance.__dict__ = data
        # Initialize pydantic slot attributes expected by BaseModel
        object.__setattr__(instance, '__pydantic_private__', {})
        object.__setattr__(instance, '__pydantic_extra__', extra)
        object.__setattr__(instance, '__pydantic_fields_set__', fields_set)
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
                val = self._dict_to_model(result[field_name], field_schema)
                # For function-after wrapping with Python callable (e.g. SecretStr, use_enum_values)
                if field_schema.get("type") == "function-after":
                    inner = field_schema.get("schema", {})
                    if isinstance(inner, dict) and inner.get("type") in ("enum", "literal"):
                        # use_enum_values: extract .value from Enum member
                        if hasattr(val, 'value'):
                            val = val.value
                    else:
                        # Generic function-after: apply the Python callable to the validated value
                        func_ref = field_schema.get("function", {})
                        if isinstance(func_ref, dict):
                            func = func_ref.get("function", func_ref)
                        else:
                            func = func_ref
                        if callable(func):
                            try:
                                val = func(val)
                            except Exception:
                                pass
                # Also handle lax-or-strict -> json-or-python -> function-after chains
                elif field_schema.get("type") == "lax-or-strict":
                    callables = _find_function_after_callable(field_schema)
                    for func in callables:
                        if callable(func):
                            try:
                                val = func(val)
                            except Exception:
                                pass
                result[field_name] = val

        return result

    @property
    def title(self):
        return self._base.title

    def validate_python(self, obj, *, strict=None, context=None, self_instance=None,
                        extra=None, from_attributes=None, by_alias=None, by_name=None):
        try:
            result = self._base.validate_python(
                obj, strict=strict, context=context, self_instance=self_instance,
                extra=extra, from_attributes=from_attributes, by_alias=by_alias, by_name=by_name)
        except ValidationError as e:
            # Store original C++ message and model name for formatting
            e._cpp_msg = _orig_str(e)
            model_name = _get_model_name(self._schema)
            if model_name:
                e._model_name = model_name
            raise

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
                cls = self._model_classes.get("__root__") or self._schema.get("cls")
                if cls is not None and callable(cls):
                    # Process nested models in the result dict
                    inner_schema = self._schema.get("schema", {})
                    if isinstance(inner_schema, dict) and inner_schema.get("type") in ("model-fields", "typed-dict"):
                        result = self._process_model_fields(result, inner_schema)
                    # Extract special keys before setting __dict__
                    extra = result.pop('__pydantic_extra__', None)
                    fields_set = result.pop('__pydantic_fields_set__', set(result.keys()))
                    result.pop('__pydantic_defaults__', None)
                    instance = object.__new__(cls)
                    instance.__dict__ = result
                    object.__setattr__(instance, '__pydantic_private__', {})
                    object.__setattr__(instance, '__pydantic_extra__', extra)
                    object.__setattr__(instance, '__pydantic_fields_set__', fields_set)
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

    def validate_strings(self, string_data, *, strict=None, extra=None, context=None, by_alias=None, by_name=None, allow_partial=None):
        result = self._base.validate_strings(string_data, strict=strict)
        if isinstance(result, dict):
            result = self._dict_to_model(result)
            if not isinstance(result, dict):
                return result
            schema_type = self._schema.get("type") if hasattr(self._schema, "get") else None
            if schema_type == "definitions":
                inner = self._schema.get("schema", {})
                if inner.get("type") == "definition-ref":
                    ref = inner.get("schema_ref", "__root__")
                    cls = self._model_classes.get(ref)
                    if cls:
                        return self._build_model(result, cls, ref)
            elif schema_type == "model":
                cls = self._model_classes.get("__root__") or self._schema.get("cls")
                if cls is not None and callable(cls):
                    inner_schema = self._schema.get("schema", {})
                    if isinstance(inner_schema, dict) and inner_schema.get("type") in ("model-fields", "typed-dict"):
                        result = self._process_model_fields(result, inner_schema)
                    extra_fields = result.pop('__pydantic_extra__', None)
                    fields_set = result.pop('__pydantic_fields_set__', set(result.keys()))
                    result.pop('__pydantic_defaults__', None)
                    instance = object.__new__(cls)
                    instance.__dict__ = result
                    object.__setattr__(instance, '__pydantic_private__', {})
                    object.__setattr__(instance, '__pydantic_extra__', extra_fields)
                    object.__setattr__(instance, '__pydantic_fields_set__', fields_set)
                    return instance
        return result

    def isinstance_python(self, obj, *, strict=None):
        return self._base.isinstance_python(obj, strict=strict)

    def get_default_value(self, *, strict=None):
        return self._base.get_default_value(strict=strict)

    def validate_assignment(self, obj, field_name, field_value):
        return self._base.validate_assignment(obj, field_name, field_value)

    def __repr__(self):
        return self._base.__repr__()


def _schema_clean_cls_keys(d):
    """Recursively remove all ``cls`` keys from schema dicts,
    except for ``is-instance`` schemas which need ``cls`` for class checking.
    """
    if isinstance(d, dict):
        if d.get("type") != "is-instance":
            d.pop("cls", None)
        for v in d.values():
            _schema_clean_cls_keys(v)
    elif isinstance(d, list):
        for item in d:
            _schema_clean_cls_keys(item)


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
_RUST_FALLBACKS: frozenset[str] = frozenset()

# Symbols that come from core_schema rather than the native extension
_CORE_SCHEMA_FALLBACKS = frozenset({
    'CoreConfig',
    'CoreSchema',
    'CoreSchemaType',
    'ErrorType',
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
# 4. Standalone functions (still from Rust until C++ implements them)
# ============================================================================

def from_json(*args: _Any, **kwargs: _Any) -> _Any:
    return _rust().from_json(*args, **kwargs)


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


class Some:
    """Wrapper for optional values.

    Similar to Rust's Option::Some type, identifies a value as being present.
    Used in union with None to distinguish between 'some value which could be
    None' and 'no value'.

    Matches Rust's Some pyclass.
    """

    __match_args__ = ('value',)

    def __init__(self, value):
        self._value = value

    @property
    def value(self):
        """Returns the value wrapped by Some."""
        return self._value

    def __repr__(self):
        return f'Some({self._value!r})'

    def __eq__(self, other):
        if isinstance(other, Some):
            return self._value == other._value
        return NotImplemented

    def __hash__(self):
        return hash(self._value)

    @classmethod
    def __class_getitem__(cls, item):
        return cls


# ============================================================================
# 4.6. Sentinel types (pure Python, no Rust dependency)
# ============================================================================

class _PydanticUndefinedType:
    """A type used as a sentinel for undefined values.

    Matches Rust's PydanticUndefinedType. The singleton instance
    PydanticUndefined is falsy and supports copy/deepcopy.
    """

    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __bool__(self) -> bool:
        return False

    def __repr__(self) -> str:
        return 'PydanticUndefined'

    def __copy__(self):
        return self

    def __deepcopy__(self, memo):
        return self


PydanticUndefined = _PydanticUndefinedType()
PydanticUndefinedType = _PydanticUndefinedType


# ============================================================================
# 4.7. Exception classes (pure Python, no Rust dependency)
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


class PydanticSerializationError(ValueError):
    """An error raised when an issue occurs during serialization.

    In custom serializers, this error can be used to indicate that
    serialization has failed.

    Arguments:
        message: The message associated with the error.
    """

    def __init__(self, message: str) -> None:
        super().__init__(message)


class PydanticSerializationUnexpectedValue(PydanticSerializationError):
    """An error raised when an unexpected value is encountered during serialization.

    This error is often caught and coerced into a warning, as pydantic-core
    generally makes a best attempt at serializing values.

    Arguments:
        message: The message associated with the error.
    """

    def __init__(self, message: str) -> None:
        super().__init__(message)


import datetime as _datetime


class TzInfo(_datetime.tzinfo):
    """A fixed-offset timezone implementation.

    Matches Rust's TzInfo pyclass. Provides a ``datetime.tzinfo`` subclass
    with a fixed offset from UTC, similar to ``datetime.timezone`` but with
    custom formatting (e.g. ``+05:30``, ``UTC``).

    Arguments:
        seconds: The offset from UTC in seconds. Defaults to 0.0 (UTC).
    """

    _seconds: int

    def __init__(self, seconds: float = 0.0) -> None:
        self._seconds = int(seconds)

    def utcoffset(self, dt: _datetime.datetime | None) -> _datetime.timedelta | None:
        """Return the fixed offset from UTC."""
        return _datetime.timedelta(seconds=self._seconds)

    def tzname(self, dt: _datetime.datetime | None) -> str | None:
        """Return the timezone name."""
        return str(self)

    def dst(self, dt: _datetime.datetime | None) -> _datetime.timedelta | None:
        """Return the DST adjustment (always None for fixed offset)."""
        return None

    def fromutc(self, dt: _datetime.datetime) -> _datetime.datetime:
        """Return an equivalent datetime in the local timezone."""
        return dt + self.utcoffset(None)  # type: ignore[operator]

    def __repr__(self) -> str:
        return f'TzInfo({self._seconds})'

    def __str__(self) -> str:
        seconds = self._seconds
        if seconds == 0:
            return 'UTC'
        sign = '+' if seconds >= 0 else '-'
        mins, secs = divmod(abs(seconds), 60)
        hours, mins = divmod(mins, 60)
        result = f'{sign}{hours:02d}:{mins:02d}'
        if secs != 0:
            result += f':{secs:02d}'
        return result

    def __hash__(self) -> int:
        return hash(self._seconds)

    def __eq__(self, other: object) -> bool:
        if isinstance(other, TzInfo):
            return self._seconds == other._seconds
        return NotImplemented

    def __deepcopy__(self, memo: dict) -> 'TzInfo':
        return TzInfo(self._seconds)


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
