import dataclasses
import datetime
import decimal
import fractions
import ipaddress
import json
import math
import pathlib
import platform
import re
import subprocess
import sys
import uuid
import warnings
from collections import deque

import pytest
from dirty_equals import IsFloatNan, IsList

import pydantic_core_cpp
from pydantic_core_cpp import (
    CoreConfig,
    PydanticSerializationError,
    SchemaError,
    SchemaSerializer,
    SchemaValidator,
    ValidationError,
    core_schema,
    from_json,
    to_json,
    to_jsonable_python,
)

from .conftest import Err


@pytest.mark.parametrize(
    'input_value,output_value',
    [('false', False), ('true', True), ('0', False), ('1', True), ('"yes"', True), ('"no"', False)],
)
def test_bool(input_value, output_value):
    v = SchemaValidator(core_schema.bool_schema())
    assert v.validate_json(input_value) == output_value


@pytest.mark.parametrize(
    'input_value',
    [
        pytest.param('[1, 2, 3]', id='[1, 2, 3]_list'),
        pytest.param(b'[1, 2, 3]', id='[1, 2, 3]_bytes'),
        pytest.param(bytearray(b'[1, 2, 3]'), id='[1, 2, 3]_bytearray'),
    ],
)
def test_input_types(input_value):
    v = SchemaValidator(core_schema.list_schema(items_schema=core_schema.int_schema()))
    assert v.validate_json(input_value) == [1, 2, 3]


def test_input_type_invalid():
    v = SchemaValidator(core_schema.list_schema(items_schema=core_schema.int_schema()))
    with pytest.raises(ValidationError, match=r'JSON input should be string, bytes or bytearray \[type=json_type,'):
        v.validate_json([])


def test_null():
    assert SchemaValidator(core_schema.none_schema()).validate_json('null') is None


def test_str():
    s = SchemaValidator(core_schema.str_schema())
    assert s.validate_json('"foobar"') == 'foobar'
    with pytest.raises(ValidationError, match=r'Input should be a valid string \[type=string_type,'):
        s.validate_json('false')
    with pytest.raises(ValidationError, match=r'Input should be a valid string \[type=string_type,'):
        s.validate_json('123')


def test_bytes():
    s = SchemaValidator(core_schema.bytes_schema())
    assert s.validate_json('"foobar"') == b'foobar'
    with pytest.raises(ValidationError, match=r'Input should be a valid bytes \[type=bytes_type,'):
        s.validate_json('false')
    with pytest.raises(ValidationError, match=r'Input should be a valid bytes \[type=bytes_type,'):
        s.validate_json('123')


# A number well outside of i64 range
_BIG_NUMBER_STR = '1' + ('0' * 40)


@pytest.mark.parametrize(
    'input_value,expected',
    [
        ('123', 123),
        ('"123"', 123),
        ('123.0', 123),
        ('"123.0"', 123),
        (_BIG_NUMBER_STR, int(_BIG_NUMBER_STR)),
        ('123.4', Err('Input should be a valid integer, got a number with a fractional part [type=int_from_float,')),
        ('"123.4"', Err('Input should be a valid integer, unable to parse string as an integer [type=int_parsing,')),
        ('"string"', Err('Input should be a valid integer, unable to parse string as an integer [type=int_parsing,')),
    ],
)
def test_int(input_value, expected):
    v = SchemaValidator(core_schema.int_schema())
    if isinstance(expected, Err):
        with pytest.raises(ValidationError, match=re.escape(expected.message)):
            v.validate_json(input_value)
    else:
        assert v.validate_json(input_value) == expected


@pytest.mark.parametrize(
    'input_value,expected',
    [
        ('123.4', 123.4),
        ('123.0', 123.0),
        ('123', 123.0),
        ('"123.4"', 123.4),
        ('"123.0"', 123.0),
        ('"123"', 123.0),
        ('"string"', Err('Input should be a valid number, unable to parse string as a number [type=float_parsing,')),
    ],
)
def test_float(input_value, expected):
    v = SchemaValidator(core_schema.float_schema())
    if isinstance(expected, Err):
        with pytest.raises(ValidationError, match=re.escape(expected.message)):
            v.validate_json(input_value)
    else:
        assert v.validate_json(input_value) == expected


def test_typed_dict():
    v = SchemaValidator(
        core_schema.typed_dict_schema(
            fields={
                'field_a': core_schema.typed_dict_field(schema=core_schema.str_schema()),
                'field_b': core_schema.typed_dict_field(schema=core_schema.int_schema()),
            }
        )
    )

    # language=json
    input_str = '{"field_a": "abc", "field_b": 1}'
    assert v.validate_json(input_str) == {'field_a': 'abc', 'field_b': 1}
    # language=json
    input_str = '{"field_a": "a", "field_a": "b", "field_b": 1}'
    assert v.validate_json(input_str) == {'field_a': 'b', 'field_b': 1}
    assert v.validate_json(input_str) == {'field_a': 'b', 'field_b': 1}


def test_float_no_remainder():
    v = SchemaValidator(core_schema.int_schema())
    assert v.validate_json('123.0') == 123


def test_error_loc():
    v = SchemaValidator(
        core_schema.typed_dict_schema(
            fields={
                'field_a': core_schema.typed_dict_field(
                    schema=core_schema.list_schema(items_schema=core_schema.int_schema())
                )
            },
            extras_schema=core_schema.int_schema(),
            extra_behavior='allow',
        )
    )

    # assert v.validate_json('{"field_a": [1, 2, "3"]}') == ({'field_a': [1, 2, 3]}, {'field_a'})

    with pytest.raises(ValidationError) as exc_info:
        v.validate_json('{"field_a": [1, 2, "wrong"]}')
    assert exc_info.value.errors(include_url=False) == [
        {
            'type': 'int_parsing',
            'loc': ('field_a', 2),
            'msg': 'Input should be a valid integer, unable to parse string as an integer',
            'input': 'wrong',
        }
    ]


def test_dict():
    v = SchemaValidator(
        core_schema.dict_schema(keys_schema=core_schema.int_schema(), values_schema=core_schema.int_schema())
    )
    assert v.validate_json('{"1": 2, "3": 4}') == {1: 2, 3: 4}

    # duplicate keys, the last value wins, like with python
    assert json.loads('{"1": 1, "1": 2}') == {'1': 2}
    assert v.validate_json('{"1": 1, "1": 2}') == {1: 2}


def test_dict_any_value():
    v = SchemaValidator(core_schema.dict_schema(keys_schema=core_schema.str_schema()))
    assert v.validate_json('{"1": 1, "2": "a", "3": null}') == {'1': 1, '2': 'a', '3': None}


def test_json_invalid():
    v = SchemaValidator(core_schema.bool_schema())

    with pytest.raises(ValidationError) as exc_info:
        v.validate_json('"foobar')
    assert exc_info.value.errors(include_url=False) == [
        {
            'type': 'json_invalid',
            'loc': (),
            'msg': 'Invalid JSON: EOF while parsing a string at line 1 column 7',
            'input': '"foobar',
            'ctx': {'error': 'EOF while parsing a string at line 1 column 7'},
        }
    ]
    with pytest.raises(ValidationError) as exc_info:
        v.validate_json('[1,\n2,\n3,]')
    assert exc_info.value.errors(include_url=False) == [
        {
            'type': 'json_invalid',
            'loc': (),
            'msg': 'Invalid JSON: trailing comma at line 3 column 3',
            'input': '[1,\n2,\n3,]',
            'ctx': {'error': 'trailing comma at line 3 column 3'},
        }
    ]


class Foobar:
    def __str__(self):
        return 'Foobar.__str__'


def fallback_func(v):
    return f'fallback:{type(v).__name__}'


def test_to_json():
    assert to_json([1, 2]) == b'[1,2]'
    assert to_json([1, 2], indent=2) == b'[\n  1,\n  2\n]'
    assert to_json([1, b'x']) == b'[1,"x"]'
    assert to_json(['à', 'é']).decode('utf-8') == '["à","é"]'
    assert to_json(['à', 'é'], indent=2).decode('utf-8') == '[\n  "à",\n  "é"\n]'
    assert to_json(['à', 'é'], indent=2, ensure_ascii=True).decode('utf-8') == '[\n  "\\u00e0",\n  "\\u00e9"\n]'

    # kwargs required
    with pytest.raises(TypeError, match=r'to_json\(\) takes 1 positional arguments but 2 were given'):
        to_json([1, 2], 2)


def test_entry_points_refuse_positional_arguments():
    # Every argument but the value is keyword-only in Rust's signature, and pyo3 words the
    # refusal its own way -- "arguments" whatever the count, where CPython would say
    # "argument" for one -- so the text belongs to the entry point and not to the bindings.
    cases = (('to_json', to_json, [1, 2]), ('to_jsonable_python', to_jsonable_python, [1, 2]), ('from_json', from_json, b'[1]'))
    for name, fn, value in cases:
        with pytest.raises(TypeError, match=re.escape(f'{name}() takes 1 positional arguments but 2 were given')):
            fn(value, 2)
        with pytest.raises(TypeError, match=re.escape(f'{name}() takes 1 positional arguments but 4 were given')):
            fn(value, 2, 3, 4)
        with pytest.raises(TypeError, match=re.escape(f'{name}() missing 1 required positional argument')):
            fn()
        with pytest.raises(TypeError, match=re.escape(f"{name}() got an unexpected keyword argument 'not_real'")):
            fn(value, not_real=1)

    # the value itself may be named rather than positional, and naming it twice is the
    # signature's own complaint about an argument, not the bindings refusing the call
    assert to_json(value=[1, 2]) == b'[1,2]'
    assert to_jsonable_python(value=[1, 2]) == [1, 2]
    assert from_json(data=b'[2]') == [2]
    with pytest.raises(TypeError, match=re.escape("to_json() got multiple values for argument 'value'")):
        to_json([1, 2], value=[3])
    with pytest.raises(TypeError, match=re.escape("to_jsonable_python() got multiple values for argument 'value'")):
        to_jsonable_python([1, 2], value=[3])
    with pytest.raises(TypeError, match=re.escape("from_json() got multiple values for argument 'data'")):
        from_json(b'[1]', data=b'[2]')


def test_to_json_fallback():
    with pytest.raises(PydanticSerializationError, match=r'Unable to serialize unknown type: <.+\.Foobar'):
        to_json(Foobar())

    assert to_json(Foobar(), serialize_unknown=True) == b'"Foobar.__str__"'
    assert to_json(Foobar(), serialize_unknown=True, fallback=fallback_func) == b'"fallback:Foobar"'
    assert to_json(Foobar(), fallback=fallback_func) == b'"fallback:Foobar"'


def test_to_jsonable_python():
    assert to_jsonable_python([1, 2]) == [1, 2]
    assert to_jsonable_python({1, 2}) == IsList(1, 2, check_order=False)
    assert to_jsonable_python([1, b'x']) == [1, 'x']
    assert to_jsonable_python([0, 1, 2, 3, 4], exclude={1, 3}) == [0, 2, 4]


def test_to_jsonable_python_include_exclude():
    seq = [0, 1, 2, 3, 4]
    assert to_jsonable_python(seq, exclude={1, 3}) == [0, 2, 4]
    assert to_jsonable_python(seq, include={0, 2, 4}) == [0, 2, 4]
    assert to_jsonable_python((0, 1, 2), exclude={1}) == [0, 2]
    # an index counted from the end only means anything once the length is known
    assert to_jsonable_python(seq, exclude={-1}) == [0, 1, 2, 3]
    assert to_jsonable_python(seq, include={-1}) == [4]
    assert to_jsonable_python(['a', 'b', 'c'], include={-2}) == ['b']
    # a filter's index is taken modulo the length, so 9 is the same position as -1 is
    assert to_jsonable_python(seq, exclude={9}) == [0, 1, 2, 3]

    # `__all__` names every element, and `...` or True both mean 'this one, nothing below it'
    assert to_jsonable_python(seq, exclude={'__all__'}) == []
    assert to_jsonable_python(seq, exclude={'__all__': ...}) == []
    assert to_jsonable_python(seq, exclude={'__all__': None}) == seq
    assert to_jsonable_python(seq, exclude={1: ...}) == [0, 2, 3, 4]
    assert to_jsonable_python(seq, exclude={1: True}) == [0, 2, 3, 4]
    # an entry which is neither of those is the filter for the element's own contents instead
    assert to_jsonable_python(seq, exclude={1: None}) == seq
    assert to_jsonable_python([[0, 1], [2, 3]], exclude={0: {0: ...}}) == [[1], [2, 3]]
    assert to_jsonable_python([[0, 1], [2, 3]], include={0: {1: ...}}) == [[1]]
    assert to_jsonable_python([{'a': 1, 'b': 2}, {'a': 3}], include={0: {'a': ...}}) == [{'a': 1}]
    assert to_jsonable_python(seq, exclude={'__all__': ..., 1: None}) == [1]
    # anything with a `__contains__` gets asked, which is how a list or a str works as a filter
    assert to_jsonable_python(seq, exclude=[1]) == [0, 2, 3, 4]
    assert to_jsonable_python(seq, include=[1]) == [1]
    assert to_jsonable_python(seq, exclude=frozenset({1})) == [0, 2, 3, 4]
    assert to_jsonable_python([b'x', b'y'], exclude={0}, bytes_mode='base64') == ['eQ==']
    assert to_jsonable_python([(1, 2), (3, 4)], exclude={1}) == [[1, 2]]
    # a set's members are keyed by nothing and numbered by nothing, so no filter reaches one
    assert to_jsonable_python({5, 6, 7}, exclude={6}) == IsList(5, 6, 7, check_order=False)
    assert to_jsonable_python([{1, 2}], exclude={0: {1: ...}}) == [[1, 2]]
    assert to_jsonable_python([{1, 2}], include={0: {1: ...}}) == [[1, 2]]
    # an iterator has no length, so it is filtered as it goes and refuses a backwards index
    assert to_jsonable_python(iter(seq), exclude={0, 2}) == [1, 3, 4]
    assert to_jsonable_python((i for i in range(5)), exclude={1}) == [0, 2, 3, 4]
    assert to_jsonable_python(deque([1, 2, 3]), exclude={1}) == [1, 3]
    assert to_jsonable_python(deque([1, 2, 3]), exclude={-1}) == [1, 2]

    d = {'a': 1, 'b': 2}
    nested = {'a': {'b': 1, 'c': 2}, 'd': {'e': 3}}
    assert to_jsonable_python(d, exclude={'a'}) == {'b': 2}
    assert to_jsonable_python(d, include={'a'}) == {'a': 1}
    assert to_jsonable_python(d, exclude={'__all__'}) == {}
    assert to_jsonable_python(d, include={'__all__'}) == {'a': 1, 'b': 2}
    assert to_jsonable_python(d, exclude={'a': True}) == {'b': 2}
    assert to_jsonable_python(d, exclude='a') == {'b': 2}
    assert to_jsonable_python({'a': 1, 2: 'b'}, exclude={2}) == {'a': 1}
    assert to_jsonable_python(nested, exclude={'a': {'b': ...}}) == {'a': {'c': 2}, 'd': {'e': 3}}
    assert to_jsonable_python(nested, exclude={'a': {'b': None}}) == {'a': {'b': 1, 'c': 2}, 'd': {'e': 3}}
    assert to_jsonable_python(nested, include={'a': {'b': ...}}) == {'a': {'b': 1}}
    assert to_jsonable_python(nested, include={'a': ...}) == {'a': {'b': 1, 'c': 2}}
    assert to_jsonable_python({'a': {'b': {'c': 1, 'd': 2}}}, exclude={'a': {'b': {'c': ...}}}) == {
        'a': {'b': {'d': 2}}
    }
    # `__all__` is folded into whichever key is being asked about, dict or set either way
    assert to_jsonable_python(nested, exclude={'__all__': {'b': ...}}) == {'a': {'c': 2}, 'd': {'e': 3}}
    assert to_jsonable_python(nested, exclude={'__all__': {'b'}}) == {'a': {'c': 2}, 'd': {'e': 3}}
    assert to_jsonable_python(nested, exclude={'a': {'b': ...}, '__all__': {'e': ...}}) == {'a': {'c': 2}, 'd': {}}
    assert to_jsonable_python({'a': {'b': 1}}, exclude={'a': {'c': ...}, '__all__': {'b': ...}}) == {'a': {}}
    # an entry which names nothing the value holds drops nothing, and a key no include names is
    assert to_jsonable_python(d, exclude={'b': {'c': 1}}) == {'a': 1, 'b': 2}
    assert to_jsonable_python(d, include={'b': {'c': ...}}) == {'b': 2}
    assert to_jsonable_python(d, exclude={'__all__': 5}) == {'a': 1, 'b': 2}
    assert to_jsonable_python({'a': {'b': 1}}, exclude={'__all__': 'x'}) == {'a': {'b': 1}}
    # exclude_none is an option of a model's fields, not of an arbitrary dict
    assert to_jsonable_python({'a': None, 'b': 1}, exclude_none=True) == {'a': None, 'b': 1}


def test_to_jsonable_python_include_exclude_errors():
    with pytest.raises(TypeError, match=re.escape('`exclude` argument must be a set or dict.')):
        to_jsonable_python([0, 1], exclude='abc')

    with pytest.raises(TypeError, match=re.escape('`exclude` argument must be a set or dict.')):
        to_jsonable_python([0, 1], exclude=5)

    with pytest.raises(TypeError, match=re.escape('`include` argument must be a set or dict.')):
        to_jsonable_python([0, 1], include='ab')

    with pytest.raises(
        ValueError, match='Negative indices cannot be used to exclude items on unsized iterables'
    ):
        to_jsonable_python(iter([0, 1]), exclude={-1})

    # the filter is only ever asked about an element the walk actually reaches
    assert to_jsonable_python(iter([]), exclude={-1}) == []

    with pytest.raises(
        TypeError,
        match=re.escape('`include` and `exclude` must be of type `dict[str | int, <recursive> | ...] | set[str | int | ...]`'),
    ):
        to_jsonable_python({'a': 1}, exclude={'a': 5, '__all__': 7})

    with pytest.raises(
        TypeError,
        match=re.escape(
            "'__all__' key of `include` and `exclude` must be of type "
            '`dict[str | int, <recursive> | ...] | set[str | int | ...]`'
        ),
    ):
        to_jsonable_python({'a': {'b': 1}}, exclude={'a': {'b': 1}, '__all__': 5})


def test_to_jsonable_python_fallback():
    with pytest.raises(PydanticSerializationError, match=r'Unable to serialize unknown type: <.+\.Foobar'):
        to_jsonable_python(Foobar())

    assert to_jsonable_python(Foobar(), serialize_unknown=True) == 'Foobar.__str__'
    assert to_jsonable_python(Foobar(), serialize_unknown=True, fallback=fallback_func) == 'fallback:Foobar'
    assert to_jsonable_python(Foobar(), fallback=fallback_func) == 'fallback:Foobar'


def test_to_jsonable_python_schema_serializer():
    class Foobar:
        def __init__(self, my_foo: int, my_inners: list['Foobar']):
            self.my_foo = my_foo
            self.my_inners = my_inners

    # force a recursive model to ensure we exercise the transfer of definitions from the loaded
    # serializer
    c = core_schema.definitions_schema(
        core_schema.definition_reference_schema(schema_ref='foobar'),
        [
            core_schema.model_schema(
                Foobar,
                core_schema.typed_dict_schema(
                    {
                        'my_foo': core_schema.typed_dict_field(core_schema.int_schema(), serialization_alias='myFoo'),
                        'my_inners': core_schema.typed_dict_field(
                            core_schema.list_schema(core_schema.definition_reference_schema('foobar')),
                            serialization_alias='myInners',
                        ),
                    }
                ),
                ref='foobar',
            )
        ],
    )
    v = SchemaValidator(c)
    s = SchemaSerializer(c)

    Foobar.__pydantic_validator__ = v
    Foobar.__pydantic_serializer__ = s

    instance = Foobar(my_foo=1, my_inners=[Foobar(my_foo=2, my_inners=[])])
    assert to_jsonable_python(instance, by_alias=True) == {'myFoo': 1, 'myInners': [{'myFoo': 2, 'myInners': []}]}
    assert to_jsonable_python(instance, by_alias=False) == {'my_foo': 1, 'my_inners': [{'my_foo': 2, 'my_inners': []}]}
    assert to_json(instance, by_alias=True) == b'{"myFoo":1,"myInners":[{"myFoo":2,"myInners":[]}]}'
    assert to_json(instance, by_alias=False) == b'{"my_foo":1,"my_inners":[{"my_foo":2,"my_inners":[]}]}'


def test_to_json_by_alias():
    # A collection's members have no name of their own to argue about: the naming the run
    # was asked for reaches them too, because Rust hands the member serializer the state
    # it was handed itself (list.rs:105, deque.rs, generator.rs, set_frozenset.rs:100).
    member = core_schema.typed_dict_schema(
        {'my_foo': core_schema.typed_dict_field(core_schema.int_schema(), serialization_alias='myFoo')}
    )

    def holder(item):
        return core_schema.typed_dict_schema(
            {
                'my_foo': core_schema.typed_dict_field(core_schema.int_schema(), serialization_alias='myFoo'),
                'my_key': core_schema.typed_dict_field(item, serialization_alias='myKey'),
            }
        )

    d = {'my_foo': 2}
    ser = SchemaSerializer(holder(core_schema.list_schema(member)))
    assert ser.to_json({'my_foo': 1, 'my_key': [d]}, by_alias=True) == b'{"myFoo":1,"myKey":[{"myFoo":2}]}'
    assert ser.to_json({'my_foo': 1, 'my_key': [d]}, by_alias=False) == b'{"my_foo":1,"my_key":[{"my_foo":2}]}'
    assert ser.to_python({'my_foo': 1, 'my_key': [d]}, mode='json', by_alias=True) == {'myFoo': 1, 'myKey': [{'myFoo': 2}]}
    ser = SchemaSerializer(holder(core_schema.list_schema(core_schema.list_schema(member))))
    assert ser.to_json({'my_foo': 1, 'my_key': [[d]]}, by_alias=True) == b'{"myFoo":1,"myKey":[[{"myFoo":2}]]}'
    ser = SchemaSerializer(holder(core_schema.deque_schema(member)))
    assert ser.to_json({'my_foo': 1, 'my_key': deque([d])}, by_alias=True) == b'{"myFoo":1,"myKey":[{"myFoo":2}]}'
    # an iterator is read once, so each of these gets one of its own
    ser = SchemaSerializer(holder(core_schema.generator_schema(member)))
    assert ser.to_json({'my_foo': 1, 'my_key': iter([d])}, by_alias=True) == b'{"myFoo":1,"myKey":[{"myFoo":2}]}'
    assert ser.to_json({'my_foo': 1, 'my_key': iter([d])}, by_alias=False) == b'{"my_foo":1,"my_key":[{"my_foo":2}]}'

    @dataclasses.dataclass(frozen=True)
    class Frozen:
        my_foo: int = 2

    frozen_member = core_schema.dataclass_schema(
        Frozen,
        core_schema.dataclass_args_schema(
            'Frozen', [{'name': 'my_foo', 'schema': core_schema.int_schema(), 'serialization_alias': 'myFoo'}]
        ),
        ['my_foo'],
    )
    ser = SchemaSerializer(holder(core_schema.set_schema(frozen_member)))
    assert ser.to_json({'my_foo': 1, 'my_key': {Frozen(2)}}, by_alias=True) == b'{"myFoo":1,"myKey":[{"myFoo":2}]}'


def test_to_json_by_alias_entry_point():
    class Foobar:
        def __init__(self, my_foo: int, my_inners: list['Foobar']):
            self.my_foo = my_foo
            self.my_inners = my_inners

    c = core_schema.definitions_schema(
        core_schema.definition_reference_schema(schema_ref='foobar'),
        [
            core_schema.model_schema(
                Foobar,
                core_schema.typed_dict_schema(
                    {
                        'my_foo': core_schema.typed_dict_field(core_schema.int_schema(), serialization_alias='myFoo'),
                        'my_inners': core_schema.typed_dict_field(
                            core_schema.list_schema(core_schema.definition_reference_schema('foobar')),
                            serialization_alias='myInners',
                        ),
                    }
                ),
                ref='foobar',
            )
        ],
    )
    ser = SchemaSerializer(c)
    Foobar.__pydantic_serializer__ = ser
    instance = Foobar(my_foo=1, my_inners=[Foobar(my_foo=2, my_inners=[Foobar(my_foo=3, my_inners=[])])])
    aliased = b'{"myFoo":1,"myInners":[{"myFoo":2,"myInners":[{"myFoo":3,"myInners":[]}]}]}'
    plain = b'{"my_foo":1,"my_inners":[{"my_foo":2,"my_inners":[{"my_foo":3,"my_inners":[]}]}]}'

    # to_json's own `by_alias` defaults to True (mod.rs:216) and is a constant of the run,
    # so it is the model below the walk that reads it; the serializer method leaves the
    # choice unset, which is why the same value comes out named by field instead.
    assert to_json(instance) == aliased
    assert to_json(instance, by_alias=True) == aliased
    assert to_json(instance, by_alias=False) == plain
    assert ser.to_json(instance) == plain
    assert ser.to_json(instance, by_alias=True) == aliased
    assert ser.to_json(instance, by_alias=False) == plain


def test_cycle_same():
    def fallback_func_passthrough(obj):
        return obj

    f = Foobar()

    with pytest.raises(ValueError, match=r'Circular reference detected \(id repeated\)'):
        to_jsonable_python(f, fallback=fallback_func_passthrough)

    with pytest.raises(ValueError, match=r'Circular reference detected \(id repeated\)'):
        to_json(f, fallback=fallback_func_passthrough)


@pytest.mark.skipif(
    platform.python_implementation() == 'PyPy' and pydantic_core._pydantic_core.build_profile == 'debug',
    reason='PyPy does not have enough stack space for Rust debug builds to recurse very deep',
)
def test_cycle_change():
    def fallback_func_change_id(obj):
        return Foobar()

    f = Foobar()

    with pytest.raises(ValueError, match=r'Circular reference detected \(depth exceeded\)'):
        to_jsonable_python(f, fallback=fallback_func_change_id)

    with pytest.raises(ValueError, match=r'Circular reference detected \(depth exceeded\)'):
        to_json(f, fallback=fallback_func_change_id)


class FoobarHash:
    def __str__(self):
        return 'Foobar.__str__'

    def __hash__(self):
        return 1


def test_json_key_fallback():
    x = {FoobarHash(): 1}

    assert to_jsonable_python(x, serialize_unknown=True) == {'Foobar.__str__': 1}
    assert to_jsonable_python(x, fallback=fallback_func) == {'fallback:FoobarHash': 1}
    assert to_json(x, serialize_unknown=True) == b'{"Foobar.__str__":1}'
    assert to_json(x, fallback=fallback_func) == b'{"fallback:FoobarHash":1}'


class BedReprMeta(type):
    def __repr__(self):
        raise ValueError('bad repr')


class BadRepr(metaclass=BedReprMeta):
    def __repr__(self):
        raise ValueError('bad repr')

    def __hash__(self):
        return 1


def test_bad_repr():
    b = BadRepr()

    error_msg = '^Unable to serialize unknown type: <unprintable BedReprMeta object>$'
    with pytest.raises(PydanticSerializationError, match=error_msg):
        to_jsonable_python(b)

    assert to_jsonable_python(b, serialize_unknown=True) == '<Unserializable BadRepr object>'

    with pytest.raises(PydanticSerializationError, match=error_msg):
        to_json(b)

    assert to_json(b, serialize_unknown=True) == b'"<Unserializable BadRepr object>"'


def test_json_dict_keys_are_their_own_type():
    # A key is dispatched on its type rather than printed (Rust's infer_json_key,
    # infer.rs:530-641), and the jsonable walk asks the same question because it runs in json
    # mode too -- so a key leaves to_jsonable_python a str exactly as it leaves to_json a JSON
    # string, and `{1: "x"}` and `{'1': 'x'}` are the same run rather than two of them.
    import datetime
    import enum
    import uuid

    class Colour(enum.Enum):
        RED = 'red'

    assert to_json({1: 'v'}) == b'{"1":"v"}'
    assert to_json({True: 'v'}) == b'{"true":"v"}'
    assert to_json({False: 'v'}) == b'{"false":"v"}'
    assert to_json({None: 'v'}) == b'{"None":"v"}'
    assert to_json({1.5: 'v'}) == b'{"1.5":"v"}'
    assert to_json({Colour.RED: 'v'}) == b'{"red":"v"}'
    assert to_json({(1, 'a'): 'v'}) == b'{"1,a":"v"}'
    assert to_json({(1, (2, 3)): 'v'}) == b'{"1,2,3":"v"}'
    assert to_json({datetime.datetime(2024, 1, 2, 3, 4): 'v'}) == b'{"2024-01-02T03:04:00":"v"}'
    assert to_json({datetime.date(2024, 1, 2): 'v'}) == b'{"2024-01-02":"v"}'
    assert to_json({b'ab': 'v'}) == b'{"ab":"v"}'
    assert to_json({b'ab': 'v'}, bytes_mode='hex') == b'{"6162":"v"}'
    assert to_json({uuid.UUID('12345678-1234-5678-1234-567812345678'): 'v'}) == b'{"12345678-1234-5678-1234-567812345678":"v"}'

    assert to_jsonable_python({1: 'v'}) == {'1': 'v'}
    assert to_jsonable_python({True: 'v'}) == {'true': 'v'}
    assert to_jsonable_python({None: 'v'}) == {'None': 'v'}
    assert to_jsonable_python({Colour.RED: 'v'}) == {'red': 'v'}
    assert to_jsonable_python({(1, 'a'): 'v'}) == {'1,a': 'v'}
    assert to_jsonable_python({FoobarHash(): 'v'}, fallback=fallback_func) == {'fallback:FoobarHash': 'v'}

    # An unknown key is refused rather than printed -- which is what str() of every key used
    # to do -- and the run's fallback is asked about a key exactly as it is about a value.
    with pytest.raises(PydanticSerializationError, match=r"Unable to serialize unknown type: <class '.*\.FoobarHash'>"):
        to_json({FoobarHash(): 1})
    with pytest.raises(PydanticSerializationError, match=r"Unable to serialize unknown type: <class '.*\.FoobarHash'>"):
        to_jsonable_python({FoobarHash(): 1})
    assert to_json({'a': {FoobarHash(): 1}}, fallback=fallback_func) == b'{"a":{"fallback:FoobarHash":1}}'
    assert to_json({FoobarHash(): {1: FoobarHash()}}, fallback=fallback_func) == b'{"fallback:FoobarHash":{"1":"fallback:FoobarHash"}}'
    assert to_json({FoobarHash(): 1}, serialize_unknown=True) == b'{"Foobar.__str__":1}'
    assert to_json({BadRepr(): 1}, serialize_unknown=True) == b'{"<Unserializable BadRepr object>":1}'
    assert to_json({BadRepr(): 1}, fallback=fallback_func) == b'{"fallback:BadRepr":1}'
    with pytest.raises(PydanticSerializationError, match=r'^Unable to serialize unknown type: <unprintable BedReprMeta object>$'):
        to_jsonable_python({BadRepr(): 1})

    # A collection cannot name a key at all, so a hashable one of those gets this answer
    # rather than Python's own "unhashable" complaint; an int key is what a filter is asked
    # about, so the sequence filters still reach the elements they name.
    with pytest.raises(TypeError, match=r'`frozenset` not valid as object key'):
        to_jsonable_python({frozenset({1}): 1})
    assert to_jsonable_python({0: 'a', 1: 'b'}, include={0}) == {'0': 'a'}
    assert to_jsonable_python({0: 'a', 1: 'b'}, exclude={0}) == {'1': 'b'}


def test_json_key_fallback_termination():
    # Rust bounds nothing here -- a fallback that hands back another unknown recurses until
    # the C stack gives out, and the reference build dies with it.  The port reuses the value
    # walk's bound, so the run ends with that walk's own error instead of the interpreter.
    class Endless:
        def __hash__(self):
            return 1

    with pytest.raises(ValueError, match=r'Circular reference detected \(depth exceeded\)'):
        to_json({Endless(): 1}, fallback=lambda v: Endless())
    with pytest.raises(ValueError, match=r'Circular reference detected \(depth exceeded\)'):
        to_jsonable_python({Endless(): 1}, fallback=lambda v: Endless())


def test_json_float_parts_keep_their_digits():
    # A float is written in plain decimal notation, so an exponent form is not an answer
    # Rust has: a complex number's parts stay "-86400500" and "-0.00000015" however the C
    # library would abbreviate them.
    assert to_jsonable_python(complex(-8.64005e7, 1)) == '-86400500+1j'
    assert to_jsonable_python(complex(1, -8.64005e7)) == '1-86400500j'
    assert to_jsonable_python(complex(0.0, -1.5e-7)) == '-0.00000015j'
    assert to_jsonable_python(complex(-1e21, 2)) == '-1000000000000000000000+2j'
    assert to_jsonable_python(complex(-1.5e-7, 3e21)) == '-0.00000015+3000000000000000000000j'
    assert to_json(complex(-8.64005e7, 1)) == b'"-86400500+1j"'


def test_to_json_inf_nan_mode():
    # serialize_f64 (float.rs:59-75) asks the run's mode what a non-finite float is worth:
    # null takes the value away, strings writes its name as text, and constants -- the mode
    # the entry point is given when it says nothing -- keeps JSON's own spellings.
    nan, inf = float('nan'), float('inf')
    assert to_json([nan]) == b'[NaN]'
    assert to_json([nan], inf_nan_mode='null') == b'[null]'
    assert to_json([nan], inf_nan_mode='strings') == b'["NaN"]'
    assert to_json([nan], inf_nan_mode='constants') == b'[NaN]'
    assert to_json(nan, inf_nan_mode='null') == b'null'
    assert to_json(nan, inf_nan_mode='strings') == b'"NaN"'
    assert to_json([inf], inf_nan_mode='strings') == b'["Infinity"]'
    assert to_json([-inf], inf_nan_mode='strings') == b'["-Infinity"]'
    assert to_json([inf], inf_nan_mode='null') == b'[null]'
    assert to_json([inf], inf_nan_mode='constants') == b'[Infinity]'
    # A finite float has nothing the mode could answer to
    assert to_json([1.5], inf_nan_mode='null') == b'[1.5]'
    # A key is written as a string whatever the mode says, so only the mode that takes the
    # value away changes it -- to the same "None" a None key already got (infer.rs:546-553).
    assert to_json({nan: 'x'}) == b'{"nan":"x"}'
    assert to_json({nan: 'x'}, inf_nan_mode='null') == b'{"None":"x"}'
    assert to_json({inf: 'x'}, inf_nan_mode='null') == b'{"None":"x"}'
    assert to_json({-inf: 'x'}, inf_nan_mode='null') == b'{"None":"x"}'
    assert to_json({nan: 1}, inf_nan_mode='strings') == b'{"nan":1}'
    assert to_json({1.5: 'x'}, inf_nan_mode='null') == b'{"1.5":"x"}'
    # The jsonable walk keeps the float itself -- there is no JSON to be a constant in --
    # unless the mode asks for it to be gone.
    assert to_jsonable_python([nan], inf_nan_mode='null') == [None]
    assert math.isnan(to_jsonable_python([nan], inf_nan_mode='strings')[0])
    assert to_jsonable_python([inf], inf_nan_mode='null') == [None]
    assert math.isinf(to_jsonable_python([inf], inf_nan_mode='constants')[0])
    assert to_jsonable_python({nan: 'x'}, inf_nan_mode='null') == {'None': 'x'}


def test_a_float_leaf_asks_the_serializer_it_belongs_to():
    # A float that is neither finite nor a number is printed by the mode of the serializer
    # the run belongs to, which is asked of that serializer's own config (float.rs:45-56) --
    # and a config that does not name it falls back to InfNanMode::default(), the first
    # variant, Null (config.rs:141-147).  Only the module entry points print constants by
    # default, because there the default is the string their binding reads.
    nan, inf = float('nan'), float('inf')
    float_schema = core_schema.float_schema()
    assert SchemaSerializer(float_schema).to_json(nan) == b'null'
    assert SchemaSerializer(float_schema).to_json(inf) == b'null'
    assert SchemaSerializer(float_schema).to_json(-inf) == b'null'
    assert SchemaSerializer(float_schema).to_json(1.5) == b'1.5'
    # The config a caller does hand over is kept, in every mode
    for mode, expected in [('null', b'null'),
                           ('constants', b'NaN'),
                           ('strings', b'"NaN"')]:
        cfg = {'ser_json_inf_nan': mode}
        assert SchemaSerializer(float_schema, cfg).to_json(nan) == expected
    assert SchemaSerializer(float_schema, {'ser_json_inf_nan': 'constants'}).to_json(inf) == b'Infinity'
    assert SchemaSerializer(float_schema, {'ser_json_inf_nan': 'constants'}).to_json(-inf) == b'-Infinity'
    assert SchemaSerializer(float_schema, {'ser_json_inf_nan': 'strings'}).to_json(inf) == b'"Infinity"'
    assert SchemaSerializer(float_schema, {'ser_json_inf_nan': 'strings'}).to_json(-inf) == b'"-Infinity"'
    # Below a typed node the mode is the same one, so a collection answers as one voice
    assert SchemaSerializer(core_schema.list_schema(float_schema)).to_json([nan]) == b'[null]'
    assert SchemaSerializer(core_schema.nullable_schema(float_schema)).to_json(nan) == b'null'
    assert SchemaSerializer(core_schema.dict_schema(core_schema.str_schema(),
                                                    float_schema)).to_json({'a': nan}) == b'{"a":null}'
    assert SchemaSerializer(core_schema.list_schema(float_schema),
                            {'ser_json_inf_nan': 'constants'}).to_json([nan]) == b'[NaN]'
    # An untyped value is printed by the infer walk, which asks the run's config too
    # (infer.rs:401), so it answers null where a typed leaf does.
    assert SchemaSerializer(core_schema.any_schema()).to_json(nan) == b'null'
    assert SchemaSerializer(core_schema.any_schema(),
                            {'ser_json_inf_nan': 'constants'}).to_json(nan) == b'NaN'
    assert SchemaSerializer(core_schema.list_schema(core_schema.any_schema()),
                            {'ser_json_inf_nan': 'constants'}).to_json([nan]) == b'[NaN]'
    # Only the config the serializer was constructed with is asked: a schema that embeds
    # the mode in its own `config` is never read for this (shared.rs threads the
    # constructor's config down unchanged and asks a schema's own `config` for nothing but
    # polymorphic_serialization, shared.rs:252).
    assert SchemaSerializer(dict(float_schema, config={'ser_json_inf_nan': 'constants'})).to_json(nan) == b'null'
    assert SchemaSerializer(dict(float_schema, config={'ser_json_inf_nan': 'constants'}),
                            {'ser_json_inf_nan': 'strings'}).to_json(nan) == b'"NaN"'
    # The entry point is the one caller that keeps JSON's own spellings when told to
    assert to_json([nan]) == b'[NaN]'
    # A json-mode to_python run has JSON's rule for the values it hands back (infer.rs:115-121):
    # the mode that takes a non-finite float away replaces it with None.  A typed float leaf
    # is not asked, and a python-mode run has no JSON to be a null in.
    assert SchemaSerializer(core_schema.any_schema()).to_python(nan, mode='json') is None
    assert SchemaSerializer(core_schema.any_schema()).to_python([nan, inf, 1.5], mode='json') == [None, None, 1.5]
    assert math.isnan(SchemaSerializer(core_schema.any_schema(),
                                       {'ser_json_inf_nan': 'constants'}).to_python(nan, mode='json'))
    assert SchemaSerializer(core_schema.any_schema(),
                            {'ser_json_inf_nan': 'null'}).to_python(inf, mode='json') is None
    assert math.isnan(SchemaSerializer(float_schema).to_python(nan, mode='json'))
    assert math.isnan(SchemaSerializer(float_schema).to_python(nan))
    assert math.isnan(SchemaSerializer(float_schema, {'ser_json_inf_nan': 'null'}).to_python(nan))
    assert math.isnan(SchemaSerializer(core_schema.any_schema()).to_python(nan))
    assert math.isnan(SchemaSerializer(core_schema.any_schema(),
                                       {'ser_json_inf_nan': 'null'}).to_python(nan))


def test_a_model_takes_its_own_config_for_non_finite_floats():
    # "models ignore the parent config and always use the config from this model"
    # (model.rs:111-121, and dataclass.rs:103 likewise): a model builds its whole subtree
    # with the config its own schema carries, so what a float below it is worth is settled
    # by the nearest model -- whether or not an ancestor named a mode, and whether or not
    # the caller handed one to the serializer.
    nan = float('nan')

    class Inner:
        def __init__(self):
            self.f = nan

    class Outer:
        def __init__(self):
            self.inner = Inner()

    def fields_schema(**fields):
        return {'type': 'model-fields', 'model': None,
                'fields': {name: {'type': 'model-field', 'schema': schema}
                           for name, schema in fields.items()}}

    def model_of(cls, schema, config):
        return {'type': 'model', 'cls': cls, 'schema': schema, 'config': config}

    float_schema = core_schema.float_schema()
    named = model_of(Inner, fields_schema(f=float_schema),
                     {'ser_json_inf_nan': 'constants', 'extra_fields_behavior': 'ignore'})
    bare = model_of(Inner, fields_schema(f=float_schema), {'extra_fields_behavior': 'ignore'})
    assert SchemaSerializer(named).to_json(Inner()) == b'{"f":NaN}'
    assert SchemaSerializer(named, {'ser_json_inf_nan': 'null'}).to_json(Inner()) == b'{"f":NaN}'
    assert SchemaSerializer(bare).to_json(Inner()) == b'{"f":null}'
    outer_strings = model_of(Outer, fields_schema(inner=bare),
                             {'ser_json_inf_nan': 'strings', 'extra_fields_behavior': 'ignore'})
    assert SchemaSerializer(outer_strings).to_json(Outer()) == b'{"inner":{"f":null}}'
    assert SchemaSerializer(outer_strings,
                            {'ser_json_inf_nan': 'strings'}).to_json(Outer()) == b'{"inner":{"f":null}}'
    outer_bare = model_of(Outer, fields_schema(inner=named), {'extra_fields_behavior': 'ignore'})
    assert SchemaSerializer(outer_bare).to_json(Outer()) == b'{"inner":{"f":NaN}}'
    assert SchemaSerializer(outer_bare, {'ser_json_inf_nan': 'null'}).to_json(Outer()) == b'{"inner":{"f":NaN}}'


def test_a_value_that_brings_its_own_serializer_is_printed_by_that_serializer():
    # infer hands a value that carries `__pydantic_serializer__` to that serializer
    # (infer.rs:649), and call_pydantic_serializer (infer.rs:662-673) puts *its* config in
    # place for the call and writes the result into this run's text.  So what the delegated
    # value is worth is settled by the serializer it belongs to and never by the call that
    # happened to reach it -- pydantic gives each model its own serializer; the two classes
    # here are handed theirs the same way.
    nan = float('nan')

    def fields(**fs):
        return {'type': 'model-fields', 'model': None,
                'fields': {name: {'type': 'model-field', 'schema': schema}
                           for name, schema in fs.items()}}

    def model_of(cls, schema, config):
        return {'type': 'model', 'cls': cls, 'schema': schema, 'config': config}

    ignore = {'extra_fields_behavior': 'ignore'}

    class Bare:
        def __init__(self):
            self.f = nan

    class Named:
        def __init__(self):
            self.f = nan

    Bare.__pydantic_serializer__ = SchemaSerializer(
        model_of(Bare, fields(f=core_schema.float_schema()), ignore))
    Named.__pydantic_serializer__ = SchemaSerializer(
        model_of(Named, fields(f=core_schema.float_schema()),
                 dict(ignore, ser_json_inf_nan='constants')))

    assert to_json(Bare()) == b'{"f":null}'
    assert to_json(Bare(), inf_nan_mode='strings') == b'{"f":null}'
    assert to_json(Named()) == b'{"f":NaN}'
    assert to_json(Named(), inf_nan_mode='null') == b'{"f":NaN}'
    assert to_json(Named(), inf_nan_mode='strings') == b'{"f":NaN}'
    # Where the value sits changes nothing, and the indent stays this run's -- it is applied
    # to the whole text at the end -- while the value is the other serializer's.
    assert to_json([Named()]) == b'[{"f":NaN}]'
    assert to_json({'m': Bare()}) == b'{"m":{"f":null}}'
    assert to_json(Bare(), indent=2) == b'{\n  "f": null\n}'
    assert to_json({'m': Named()}, indent=4) == b'{\n    "m": {\n        "f": NaN\n    }\n}'
    # Not only a float: bytes, and anything else the modes speak of, answer the same way.
    class Bytes:
        def __init__(self):
            self.b = b'ab'

    Bytes.__pydantic_serializer__ = SchemaSerializer(
        model_of(Bytes, fields(b=core_schema.bytes_schema()), dict(ignore, ser_json_bytes='base64')))
    assert to_json(Bytes()) == b'{"b":"YWI="}'
    assert to_json(Bytes(), bytes_mode='utf8') == b'{"b":"YWI="}'
    assert to_json(Bytes(), bytes_mode='hex') == b'{"b":"YWI="}'
    # `serialize_unknown` and `fallback` are the run's own asking (Rust hands the same Extra
    # down, infer.rs:668), so they reach a delegated value -- carried beside the call, since
    # neither binding's to_json takes them as an argument.
    class Unknown:
        def __str__(self):
            return 'unknown-str'

    class Holder:
        def __init__(self):
            self.u = Unknown()

    Holder.__pydantic_serializer__ = SchemaSerializer(
        model_of(Holder, fields(u=core_schema.any_schema()), ignore))
    assert to_json(Holder(), serialize_unknown=True) == b'{"u":"unknown-str"}'
    assert to_json(Holder(), fallback=lambda v: 'FB') == b'{"u":"FB"}'
    # A failure a serializer raised itself keeps the name it had; only an error that came in
    # from Python is renamed by the boundary (errors.rs:63 renames serde's own errors).
    with pytest.raises(PydanticSerializationError) as exc_info:
        to_json(Holder())
    assert str(exc_info.value).startswith('Unable to serialize unknown type:')

    def boom(value):
        raise ValueError('boom')

    with pytest.raises(PydanticSerializationError) as exc_info:
        to_json(Holder(), fallback=boom)
    assert str(exc_info.value) == 'Error serializing to JSON: ValueError: boom'
    with pytest.raises(PydanticSerializationError) as exc_info:
        to_json(Unknown(), fallback=boom)
    assert str(exc_info.value) == 'Error serializing to JSON: ValueError: boom'


def test_a_dict_key_in_a_json_run_is_asked_for_its_text():
    # A map key is asked what it is written as, not what it serializes to: dict.rs:87-94
    # calls json_key for it, and an inferred map does the same with the key it holds
    # (shared.rs:737-740).  So a float key leaves a json run as "1.5" and a tuple key as
    # "1,2", while the same serializer in python mode hands both back as they came.  An
    # inferred key follows the run's modes and a typed key the mode of the serializer the
    # schema named -- the same split the values of the same dict obey.
    nan = float('nan')
    D = datetime.date(2024, 1, 2)
    DT = datetime.datetime(2024, 1, 2, 3, 4, 5)
    TD = datetime.timedelta(hours=1, seconds=30)
    UUID = uuid.UUID(int=1)
    uuid_text = '00000000-0000-0000-0000-000000000001'
    any_ser = SchemaSerializer(core_schema.any_schema())

    assert any_ser.to_python({'a': 'v'}, mode='json') == {'a': 'v'}
    assert any_ser.to_python({7: 'v'}, mode='json') == {'7': 'v'}
    assert any_ser.to_python({1.5: 'v'}, mode='json') == {'1.5': 'v'}
    assert any_ser.to_python({True: 'v'}, mode='json') == {'true': 'v'}
    assert any_ser.to_python({None: 'v'}, mode='json') == {'None': 'v'}
    assert any_ser.to_python({b'x': 'v'}, mode='json') == {'x': 'v'}
    assert any_ser.to_python({(1, 2): 'v'}, mode='json') == {'1,2': 'v'}
    assert any_ser.to_python({decimal.Decimal('1.5'): 'v'}, mode='json') == {'1.5': 'v'}
    assert any_ser.to_python({UUID: 'v'}, mode='json') == {uuid_text: 'v'}
    assert any_ser.to_python({D: 'v'}, mode='json') == {'2024-01-02': 'v'}
    assert any_ser.to_python({DT: 'v'}, mode='json') == {'2024-01-02T03:04:05': 'v'}
    assert any_ser.to_python({TD: 'v'}, mode='json') == {'PT1H30S': 'v'}
    assert any_ser.to_python([{1.5: 'v'}], mode='json') == [{'1.5': 'v'}]
    assert any_ser.to_python({'a': {(1, 2): 'v'}}, mode='json') == {'a': {'1,2': 'v'}}
    # Python mode leaves a key as it came in, jsonable has always turned it, and the JSON
    # text is written from the very same answers.
    assert any_ser.to_python({7: 'v', (1, 2): 'v'}) == {7: 'v', (1, 2): 'v'}
    assert to_jsonable_python({7: 'v', (1, 2): 'v'}) == {'7': 'v', '1,2': 'v'}
    assert any_ser.to_json({1.5: 'v'}) == b'{"1.5":"v"}'
    any_dict = SchemaSerializer(
        core_schema.dict_schema(core_schema.any_schema(), core_schema.any_schema()))
    assert any_dict.to_json({True: 'v'}) == b'{"true":"v"}'
    assert any_dict.to_json({None: 'v'}) == b'{"None":"v"}'
    assert any_dict.to_json({b'x': 'v'}) == b'{"x":"v"}'
    assert any_dict.to_json({(1, 2): 'v'}) == b'{"1,2":"v"}'
    # The modes of the run reach a key as they reach a value.
    assert SchemaSerializer(core_schema.any_schema(),
                            {'ser_json_inf_nan': 'null'}).to_python({nan: 'v'}, mode='json') == {'None': 'v'}
    assert SchemaSerializer(core_schema.any_schema(),
                            {'ser_json_inf_nan': 'constants'}).to_python({nan: 'v'}, mode='json') == {'nan': 'v'}
    assert SchemaSerializer(core_schema.any_schema(),
                            {'ser_json_bytes': 'base64'}).to_python({b'x': 'v'}, mode='json') == {'eA==': 'v'}
    assert SchemaSerializer(core_schema.any_schema(),
                            {'ser_json_bytes': 'hex'}).to_python({b'x': 'v'}, mode='json') == {'78': 'v'}
    assert SchemaSerializer(core_schema.any_schema(),
                            {'ser_json_temporal': 'seconds'}).to_python({D: 'v'}, mode='json') == {'1704153600': 'v'}
    assert SchemaSerializer(core_schema.any_schema(),
                            {'ser_json_temporal': 'milliseconds'}).to_python({D: 'v'}, mode='json') == {
        '1704153600000': 'v'}
    assert SchemaSerializer(core_schema.any_schema(),
                            {'ser_json_timedelta': 'float'}).to_python({TD: 'v'}, mode='json') == {'3630': 'v'}

    def typed(keys, value, config=None):
        return SchemaSerializer(core_schema.dict_schema(keys, core_schema.str_schema()),
                                config).to_python({value: 'v'}, mode='json')

    assert typed(core_schema.int_schema(), 7) == {'7': 'v'}
    # A bool is one of the ints as far as the type lookup is concerned, so it is spelled
    # Python's way under an int schema and JSON's way under its own.
    assert typed(core_schema.int_schema(), True) == {'True': 'v'}
    assert typed(core_schema.bool_schema(), True) == {'true': 'v'}
    assert typed(core_schema.bool_schema(), False) == {'false': 'v'}
    assert typed(core_schema.str_schema(), 'a') == {'a': 'v'}
    assert typed(core_schema.none_schema(), None) == {'None': 'v'}
    # A float key is str(float): a key is text either way, so the run's inf_nan mode has
    # nothing left to rewrite, and an int answers as a subclass of the type.
    assert typed(core_schema.float_schema(), 1.5) == {'1.5': 'v'}
    assert typed(core_schema.float_schema(), nan) == {'nan': 'v'}
    assert typed(core_schema.float_schema(), 7) == {'7': 'v'}
    assert typed(core_schema.date_schema(), D) == {'2024-01-02': 'v'}
    # The number a temporal key takes under seconds or milliseconds is written without a
    # trailing ".0", the way Rust's own Display of it is.
    assert typed(core_schema.date_schema(), D, {'ser_json_temporal': 'seconds'}) == {'1704153600': 'v'}
    assert typed(core_schema.datetime_schema(), DT,
                 {'ser_json_temporal': 'milliseconds'}) == {'1704164645000': 'v'}
    assert typed(core_schema.timedelta_schema(), TD) == {'PT1H30S': 'v'}
    assert typed(core_schema.timedelta_schema(), TD, {'ser_json_timedelta': 'float'}) == {'3630': 'v'}
    assert typed(core_schema.uuid_schema(), UUID) == {uuid_text: 'v'}
    assert typed(core_schema.decimal_schema(), decimal.Decimal('1.5')) == {'1.5': 'v'}
    # A tuple key is its items' keys joined with ",", a nullable key that is None is
    # "None", and a union asks each choice until one of them answers.
    two_any = [core_schema.any_schema(), core_schema.any_schema()]
    assert typed(core_schema.tuple_schema(two_any), (1, 'a')) == {'1,a': 'v'}
    assert typed(core_schema.tuple_schema(two_any), (True, None)) == {'true,None': 'v'}
    assert typed(core_schema.nullable_schema(core_schema.int_schema()), None) == {'None': 'v'}
    assert typed(core_schema.nullable_schema(core_schema.int_schema()), 7) == {'7': 'v'}
    int_or_str = [core_schema.int_schema(), core_schema.str_schema()]
    assert typed(core_schema.union_schema(int_or_str), 7) == {'7': 'v'}
    assert typed(core_schema.union_schema(int_or_str), 'a') == {'a': 'v'}
    # A collection is refused as a key by its own type, and by name only for None: any
    # other value is handed to the infer walk, which refuses it in its own words.
    int_list = core_schema.list_schema(core_schema.int_schema())
    with pytest.raises(TypeError, match=re.escape('`list` not valid as object key')):
        typed(int_list, None)
    assert typed(int_list, 7) == {'7': 'v'}
    with pytest.raises(PydanticSerializationError,
                       match=re.escape('Error serializing to JSON: TypeError: `list` not valid as object key')):
        SchemaSerializer(core_schema.dict_schema(int_list, core_schema.str_schema())).to_json({None: 'v'})
    with pytest.raises(TypeError, match=re.escape('`frozenset` not valid as object key')):
        any_ser.to_python({frozenset([1]): 'v'}, mode='json')
    # A key this run cannot name at all fails the way a value would, and the json-mode run
    # reports it bare: there is no serde boundary to name it first.
    with pytest.raises(PydanticSerializationError) as exc_info:
        any_ser.to_python({pathlib.PurePosixPath('a/b'): 'v'}, mode='json')
    assert str(exc_info.value).startswith('Unable to serialize unknown type:')


def test_to_json_temporal_mode():
    dt = datetime.datetime(2024, 1, 2, 3, 4, 5)
    dtc = datetime.datetime(2024, 1, 2, 3, 4, 5, 123456,
                            tzinfo=datetime.timezone(datetime.timedelta(hours=2)))
    d, t = datetime.date(2024, 1, 2), datetime.time(3, 4, 5)
    td = datetime.timedelta(hours=1, seconds=30)
    tdneg = datetime.timedelta(days=-1, microseconds=-500000)
    assert to_json([dt]) == b'["2024-01-02T03:04:05"]'
    assert to_json([dt], temporal_mode='seconds') == b'[1704164645.0]'
    assert to_json([dt], temporal_mode='milliseconds') == b'[1704164645000.0]'
    assert to_json([dtc], temporal_mode='seconds') == b'[1704157445.123456]'
    # timedelta_mode="float" is the same switch with a delta's name on it: one mode answers
    # for every temporal kind (config.rs:58-74), so a datetime goes to seconds with it too.
    assert to_json([dt], timedelta_mode='float') == b'[1704164645.0]'
    assert to_json([dt], timedelta_mode='float', temporal_mode='iso8601') == b'[1704164645.0]'
    assert to_json([dt], temporal_mode='seconds', timedelta_mode='iso8601') == b'[1704164645.0]'
    assert to_jsonable_python([dt], temporal_mode='seconds') == [1704164645.0]
    assert to_jsonable_python([dt]) == ['2024-01-02T03:04:05']
    # The mode does not stop at a datetime: a date, a time and a delta all follow it.
    assert to_json([d], temporal_mode='seconds') == b'[1704153600.0]'
    assert to_json([d], temporal_mode='milliseconds') == b'[1704153600000.0]'
    assert to_jsonable_python([d]) == ['2024-01-02']
    assert to_json([t], temporal_mode='seconds') == b'[11045.0]'
    assert to_json([td]) == b'["PT1H30S"]'
    assert to_json([td], temporal_mode='seconds') == b'[3630.0]'
    assert to_json([td], temporal_mode='milliseconds') == b'[3630000.0]'
    assert to_json([tdneg], temporal_mode='seconds') == b'[-86400.5]'
    assert to_json([tdneg], temporal_mode='milliseconds') == b'[-86400500.0]'
    assert to_jsonable_python([tdneg], temporal_mode='seconds') == [-86400.5]
    # A key takes the same number in Rust's own Display of it, which writes an integral one
    # as "1704164645" -- Python's str() of the float would add ".0".
    assert to_json({dt: 'x'}, temporal_mode='seconds') == b'{"1704164645":"x"}'
    assert to_json({dt: 'x'}, temporal_mode='milliseconds') == b'{"1704164645000":"x"}'
    assert to_json({dtc: 'x'}, temporal_mode='seconds') == b'{"1704157445.123456":"x"}'
    assert to_json({tdneg: 'x'}, temporal_mode='milliseconds') == b'{"-86400500":"x"}'
    assert to_json({t: 'x'}, temporal_mode='seconds') == b'{"11045":"x"}'
    assert to_json({td: 'x'}, temporal_mode='seconds') == b'{"3630":"x"}'
    assert to_jsonable_python({dt: 'x'}, temporal_mode='seconds') == {'1704164645': 'x'}
    assert to_json({d: 'x'}) == b'{"2024-01-02":"x"}'


def test_jsonable_temporal_types_cold_to_a_process():
    # Asking a temporal value what kind it is reads through the datetime module's imported
    # API pointer, which is null until something has asked for it.  A process whose first
    # serialization is a jsonable datetime therefore died on the null where every other
    # shape of the same call -- a to_json, a list, a second call -- answered fine, so only a
    # fresh interpreter can ask the question at all.
    src = (
        'import json, datetime\n'
        'from pydantic_core_cpp import to_jsonable_python\n'
        'print(json.dumps(['
        'to_jsonable_python(datetime.datetime(2024, 1, 2, 3, 4)), '
        'to_jsonable_python(datetime.date(2024, 1, 2)), '
        'to_jsonable_python(datetime.time(3, 4)), '
        "to_jsonable_python(datetime.datetime(2024, 1, 2, 3, 4), temporal_mode='seconds'), "
        "to_jsonable_python(datetime.date(2024, 1, 2), temporal_mode='milliseconds')]))"
    )
    out = subprocess.run([sys.executable, '-c', src], capture_output=True, text=True, timeout=120)
    assert out.returncode == 0, out.stderr
    assert json.loads(out.stdout) == ['2024-01-02T03:04:00', '2024-01-02', '03:04:00',
                                      1704164640.0, 1704153600000.0]


def test_serialization_modes_are_validated_in_rusts_order():
    # The modes an entry point takes are read in a fixed order (config.rs:58-74), and the
    # first one that is wrong is the only one named -- so a call that gets three of them
    # wrong is told about the temporal mode alone.  Each message carries Rust's trailing
    # "or " because its from_str is generated by a macro that lists the variants that way.
    def refused(msg, **kw):
        with pytest.raises(SchemaError, match=re.escape(msg)):
            to_json([1], **kw)
        with pytest.raises(SchemaError, match=re.escape(msg)):
            to_jsonable_python([1], **kw)

    refused('Invalid TemporalMode serialization mode: `unix`, expected iso8601 or seconds or '
            'milliseconds or ', temporal_mode='unix')
    refused('Invalid TimedeltaMode serialization mode: `bogus`, expected iso8601 or float or ',
            timedelta_mode='bogus')
    refused('Invalid InfNanMode serialization mode: `bogus`, expected null or constants or '
            'strings or ', inf_nan_mode='bogus')
    refused('Invalid BytesMode serialization mode: `b`, expected utf8 or base64 or hex or ',
            bytes_mode='b')
    refused('Invalid TemporalMode serialization mode: `t`, expected iso8601 or seconds or '
            'milliseconds or ', temporal_mode='t', timedelta_mode='b', bytes_mode='b',
            inf_nan_mode='b')
    refused('Invalid BytesMode serialization mode: `b`, expected utf8 or base64 or hex or ',
            bytes_mode='b', inf_nan_mode='b')
    # The timedelta mode is only asked when the temporal mode says nothing of its own, so a
    # bogus one rides along unanswered once a temporal mode has been named.
    assert to_json([1], timedelta_mode='bogus', temporal_mode='seconds') == b'[1]'


def test_inf_nan_allow():
    v = SchemaValidator(core_schema.float_schema(allow_inf_nan=True))
    assert v.validate_json('Infinity') == float('inf')
    assert v.validate_json('-Infinity') == float('-inf')
    assert v.validate_json('NaN') == IsFloatNan()


def test_partial_parse():
    with pytest.raises(ValueError, match='EOF while parsing a string at line 1 column 15'):
        from_json('["aa", "bb", "c')
    assert from_json('["aa", "bb", "c', allow_partial=True) == ['aa', 'bb']

    with pytest.raises(ValueError, match='EOF while parsing a string at line 1 column 15'):
        from_json(b'["aa", "bb", "c')
    assert from_json(b'["aa", "bb", "c', allow_partial=True) == ['aa', 'bb']


def test_json_bytes_base64_round_trip():
    data = b'\xd8\x07\xc1Tx$\x91F%\xf3\xf3I\xca\xd8@\x0c\xee\xc3\xab\xff\x7f\xd3\xcd\xcd\xf9\xc2\x10\xe4\xa1\xb01e'
    encoded_std = b'"2AfBVHgkkUYl8/NJythADO7Dq/9/083N+cIQ5KGwMWU="'
    encoded_url = b'"2AfBVHgkkUYl8_NJythADO7Dq_9_083N-cIQ5KGwMWU="'
    assert to_json(data, bytes_mode='base64') == encoded_url

    v = SchemaValidator(core_schema.bytes_schema(), config=CoreConfig(val_json_bytes='base64'))
    assert v.validate_json(encoded_url) == data
    assert v.validate_json(encoded_std) == data

    with pytest.raises(ValidationError) as exc:
        v.validate_json('"wrong!"')
    [details] = exc.value.errors()
    assert details['type'] == 'bytes_invalid_encoding'

    assert to_json({'key': data}, bytes_mode='base64') == b'{"key":' + encoded_url + b'}'
    v = SchemaValidator(
        core_schema.dict_schema(keys_schema=core_schema.str_schema(), values_schema=core_schema.bytes_schema()),
        config=CoreConfig(val_json_bytes='base64'),
    )
    assert v.validate_json(b'{"key":' + encoded_url + b'}') == {'key': data}


def test_json_bytes_base64_no_padding():
    v = SchemaValidator(core_schema.bytes_schema(), config=CoreConfig(val_json_bytes='base64'))
    base_64_without_padding = 'bm8tcGFkZGluZw'
    assert v.validate_json(json.dumps(base_64_without_padding)) == b'no-padding'


def test_json_bytes_base64_invalid():
    v = SchemaValidator(core_schema.bytes_schema(), config=CoreConfig(val_json_bytes='base64'))
    wrong_input = 'wrong!'
    with pytest.raises(ValidationError) as exc_info:
        v.validate_json(json.dumps(wrong_input))
    assert exc_info.value.errors(include_url=False, include_context=False) == [
        {
            'type': 'bytes_invalid_encoding',
            'loc': (),
            'msg': f'Data should be valid base64: Invalid symbol {ord("!")}, offset {len(wrong_input) - 1}.',
            'input': wrong_input,
        }
    ]


def test_json_bytes_hex_round_trip():
    data = b'hello'
    encoded = b'"68656c6c6f"'
    assert to_json(data, bytes_mode='hex') == encoded

    v = SchemaValidator(core_schema.bytes_schema(), config=CoreConfig(val_json_bytes='hex'))
    assert v.validate_json(encoded) == data

    assert to_json({'key': data}, bytes_mode='hex') == b'{"key":"68656c6c6f"}'
    v = SchemaValidator(
        core_schema.dict_schema(keys_schema=core_schema.str_schema(), values_schema=core_schema.bytes_schema()),
        config=CoreConfig(val_json_bytes='hex'),
    )
    assert v.validate_json('{"key":"68656c6c6f"}') == {'key': data}


def test_json_bytes_hex_invalid():
    v = SchemaValidator(core_schema.bytes_schema(), config=CoreConfig(val_json_bytes='hex'))

    wrong_input = 'a'
    with pytest.raises(ValidationError) as exc_info:
        v.validate_json(json.dumps(wrong_input))
    assert exc_info.value.errors(include_url=False, include_context=False) == [
        {
            'type': 'bytes_invalid_encoding',
            'loc': (),
            'msg': 'Data should be valid hex: Odd number of digits',
            'input': wrong_input,
        }
    ]

    wrong_input = 'ag'
    with pytest.raises(ValidationError) as exc_info:
        v.validate_json(json.dumps(wrong_input))
    assert exc_info.value.errors(include_url=False, include_context=False) == [
        {
            'type': 'bytes_invalid_encoding',
            'loc': (),
            'msg': "Data should be valid hex: Invalid character 'g' at position 1",
            'input': wrong_input,
        }
    ]


def test_a_value_a_json_run_cannot_serialize_is_refused_not_handed_back():
    # infer's json arm ends at ObType::Unknown (infer.rs:221-231): the run's `fallback` has
    # its turn, then `serialize_unknown` takes str() of the value -- a placeholder when str()
    # itself raises -- and with neither the run refuses by naming the value's type.  A
    # Python run asks none of that and hands the object straight back (:279-286), which is
    # what lets a python dump hold a value a json dump of the same value refuses.  Only a
    # value Rust's own type table does not name is asked at all (ob_type.rs:215-407): the
    # table names pathlib.Path but not a PurePosixPath, and neither `range` nor a
    # `memoryview`, however iterable or buffer-like they look; a dict or str subclass
    # reaches the table through its base and is never asked.
    class Unknown:
        def __repr__(self):
            return '<Unknown>'

        def __str__(self):
            return 'unknown-str'

    class NoStr:
        def __str__(self):
            raise RuntimeError('str() always raises')

    refused = 'Unable to serialize unknown type: '
    value = Unknown()
    any_ser = SchemaSerializer(core_schema.any_schema())

    # A Python run keeps the object; a json run refuses it, in a plain call, nested in a
    # container, or as the JSON text.
    assert any_ser.to_python(value) is value
    assert any_ser.to_python([value]) == [value]
    with pytest.raises(PydanticSerializationError) as exc:
        any_ser.to_python(value, mode='json')
    assert str(exc.value).startswith(refused), str(exc.value)
    assert 'Unknown' in str(exc.value), str(exc.value)
    with pytest.raises(PydanticSerializationError) as exc:
        any_ser.to_python([{'k': value}], mode='json')
    assert str(exc.value).startswith(refused), str(exc.value)
    with pytest.raises(PydanticSerializationError):
        any_ser.to_json(value)
    with pytest.raises(PydanticSerializationError):
        any_ser.to_json([value])

    # It is the run's mode that asks, not the node's type: every node that lets a value
    # through to inference carries the same refusal.
    for schema in [
        core_schema.list_schema(core_schema.any_schema()),
        core_schema.dict_schema(values_schema=core_schema.any_schema()),
        core_schema.tuple_schema([core_schema.any_schema()]),
        core_schema.nullable_schema(core_schema.any_schema()),
        core_schema.with_default_schema(core_schema.any_schema(), default=1),
        core_schema.typed_dict_schema({'a': core_schema.typed_dict_field(core_schema.any_schema())}),
    ]:
        ser = SchemaSerializer(schema)
        held = {'a': value} if schema['type'] in ('dict', 'typed-dict') else [value]
        with pytest.raises(PydanticSerializationError) as exc:
            ser.to_python(held, mode='json')
        assert str(exc.value).startswith(refused), (schema['type'], str(exc.value))

    # `fallback` is asked first, and its answer goes through the same walk in the value's
    # place, so a run that was given one refuses nothing.  A serializer binding takes no
    # `serialize_unknown` at all -- only the jsonable entry points do -- and their own
    # refusal of an unknown value is what it has always been.
    assert any_ser.to_python(value, mode='json', fallback=lambda v: 'FB') == 'FB'
    with pytest.raises(PydanticSerializationError) as exc:
        to_jsonable_python(value)
    assert str(exc.value).startswith(refused), str(exc.value)
    assert to_jsonable_python(value, serialize_unknown=True) == 'unknown-str'
    # The placeholder names the class the way Rust's type table does, by its qualified name.
    assert to_jsonable_python(NoStr(), serialize_unknown=True) == f'<Unserializable {NoStr.__qualname__} object>'

    # Unknown to Rust's table: a PurePosixPath (the table names pathlib.Path, whose bases a
    # PurePosixPath never reaches), a range, a memoryview, and a plain object.
    for unknown in [pathlib.PurePosixPath('a/b'), range(3), memoryview(b'x'), object()]:
        with pytest.raises(PydanticSerializationError) as exc:
            any_ser.to_python(unknown, mode='json')
        assert str(exc.value).startswith(refused), (unknown, str(exc.value))
    assert any_ser.to_python(range(3)) == range(3)
    with pytest.raises(PydanticSerializationError) as exc:
        to_jsonable_python(pathlib.PurePosixPath('a/b'))
    assert str(exc.value).startswith(refused), str(exc.value)

    # Known to that table: reached through a base type, so never asked.  (What each of them
    # then *leaves* as is a different question -- see the notes on the json leaf forms.)
    dict_subclass = type('D', (dict,), {})
    str_subclass = type('S', (str,), {})
    assert any_ser.to_python(dict_subclass({'a': 1}), mode='json') == {'a': 1}
    assert any_ser.to_python(str_subclass('x'), mode='json') == 'x'
    assert any_ser.to_python([1, {'a': (1, 2)}], mode='json') == [1, {'a': [1, 2]}]


def test_a_json_run_takes_the_string_form_of_the_types_rust_names():
    # infer's json arm converts the ObTypes whose JSON form is text (infer.rs:122, :184-190,
    # :191-194, :220) and a Python run converts none of them but Fraction (:278).  Each of
    # these is what a python dump holds while a json dump of the same value gives its text,
    # and the port's own json leaf (bytes, the temporal types, complex) already worked this
    # way -- these are the rest of that list.
    class DecimalSub(decimal.Decimal):
        pass

    class PathStr(pathlib.PosixPath):
        def __str__(self):
            return 'NOT-A-PATH'

    class UuidSub(uuid.UUID):
        pass

    class BoomDecimal(decimal.Decimal):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomPath(pathlib.PosixPath):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomFraction(fractions.Fraction):
        def __str__(self):
            raise RuntimeError('no str')

    any_ser = SchemaSerializer(core_schema.any_schema())

    def json_form(value):
        return any_ser.to_python(value, mode='json')

    # Decimal and Fraction are taken through pyo3's display, which cannot fail: a leaf whose
    # __str__ explodes is still serialized, as this placeholder naming its own class.
    assert json_form(decimal.Decimal('1.5')) == '1.5'
    assert json_form(decimal.Decimal('NaN')) == 'NaN'
    assert json_form(decimal.Decimal('sNaN')) == 'sNaN'
    assert json_form(DecimalSub('2.5')) == '2.5'
    assert json_form(fractions.Fraction(1, 3)) == '1/3'
    assert json_form(fractions.Fraction(2, 3)) == '2/3'
    assert any_ser.to_python(fractions.Fraction(1, 3)) == '1/3'
    assert json_form(BoomDecimal('1.5')) == '<unprintable BoomDecimal object>'
    assert json_form(BoomFraction(1, 2)) == '<unprintable BoomFraction object>'
    # ... but only a Fraction is asked in a Python run, and a Decimal is left alone.
    assert any_ser.to_python(decimal.Decimal('1.5')) == decimal.Decimal('1.5')

    # A UUID is not str()ed: uuid_to_string reads the value's own int and prints sixteen
    # big-endian bytes, so the form a UUID was built from is gone and a lying __str__ is not
    # believed.
    assert json_form(uuid.UUID(int=7)) == '00000000-0000-0000-0000-000000000007'
    assert json_form(uuid.UUID(int=0)) == '00000000-0000-0000-0000-000000000000'
    assert json_form(uuid.UUID('urn:uuid:12345678-1234-5678-1234-567812345678')) == '12345678-1234-5678-1234-567812345678'
    assert json_form(uuid.UUID('12345678123456781234567812345678')) == '12345678-1234-5678-1234-567812345678'
    assert json_form(UuidSub(int=9)) == '00000000-0000-0000-0000-000000000009'

    # The rest go through str() with the error left standing.
    assert json_form(pathlib.Path('a/b')) == 'a/b'
    assert json_form(pathlib.PosixPath('a/b')) == 'a/b'
    assert json_form(PathStr('a/b')) == 'NOT-A-PATH'
    assert json_form(ipaddress.IPv4Address('1.2.3.4')) == '1.2.3.4'
    assert json_form(ipaddress.IPv6Address('::1')) == '::1'
    assert json_form(ipaddress.IPv4Network('1.2.3.0/24')) == '1.2.3.0/24'
    assert json_form(ipaddress.IPv6Network('::/64')) == '::/64'
    # An interface is an address subclass to Rust's table, which is why it keeps its prefix.
    assert json_form(ipaddress.IPv4Interface('1.2.3.4/24')) == '1.2.3.4/24'
    assert json_form(ipaddress.IPv6Interface('::1/64')) == '::1/64'
    assert json_form(pydantic_core_cpp.Url('https://example.com/x')) == 'https://example.com/x'
    assert json_form(pydantic_core_cpp.MultiHostUrl('redis://host1:1/host2')) == 'redis://host1:1/host2'
    # A pattern gives its pattern attribute, because str() of one is its repr.
    assert json_form(re.compile('a+')) == 'a+'
    assert json_form(re.compile('a+', re.IGNORECASE)) == 'a+'

    # A Python run asks for none of this.
    for value in [decimal.Decimal('1.5'), uuid.UUID(int=7), pathlib.Path('a/b'),
                  ipaddress.IPv4Address('1.2.3.4'), re.compile('a+')]:
        assert any_ser.to_python(value) == value

    # The refusal is unchanged for a value Rust's table does not name, and the containers
    # reach the same leaf.
    with pytest.raises(PydanticSerializationError) as exc:
        json_form(pathlib.PurePosixPath('a/b'))
    assert str(exc.value).startswith('Unable to serialize unknown type: '), str(exc.value)
    assert json_form([decimal.Decimal('1.5'), {'k': uuid.UUID(int=7)}]) == [
        '1.5',
        {'k': '00000000-0000-0000-0000-000000000007'},
    ]

    # str() failing on one of these is the value's own error, not a serialization refusal --
    # except that pyo3's display, which Decimal and Fraction go through, cannot fail at all.
    with pytest.raises(RuntimeError):
        json_form(BoomPath('a/b'))


def test_a_json_text_run_prints_the_leaf_forms_the_json_object_run_prints():
    # The JSON-text walk and the json arm of the object walk are two writers over one table
    # (infer.rs:122, :184-190, :191-194, :220), so neither may refuse what the other prints nor
    # print a different text for the same value.  The text walk had its own narrower list --
    # plain str() of the classes it happened to know -- which left a Fraction refused outright,
    # a UUID printed by its own __str__ rather than rebuilt from its int, and a Decimal whose
    # __str__ raises ending the run instead of becoming pyo3's placeholder.
    class FractionSub(fractions.Fraction):
        pass

    class DecimalSub(decimal.Decimal):
        pass

    class BoomDecimal(decimal.Decimal):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomFraction(fractions.Fraction):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomPath(pathlib.PosixPath):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomIP(ipaddress.IPv4Address):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomUUID(uuid.UUID):
        def __str__(self):
            raise RuntimeError('no str')

    class LyingPath(pathlib.PosixPath):
        def __str__(self):
            return 'NOT-A-PATH'

    class UuidSub(uuid.UUID):
        pass

    class BigUuid(uuid.UUID):
        int = property(lambda self: 2**200)

    class NegUuid(uuid.UUID):
        int = property(lambda self: -1)

    class NotIntUuid(uuid.UUID):
        int = property(lambda self: 'x')

    any_ser = SchemaSerializer(core_schema.any_schema())

    # What the two writers say about one value.  The separators are spelled out because
    # json.dumps puts a space after its commas and a serializer does not.
    def both_writers_agree(value):
        text = any_ser.to_json(value)
        assert text == json.dumps(any_ser.to_python(value, mode='json'), separators=(',', ':')).encode(), repr(
            (text, value)
        )

    for value in [
        decimal.Decimal('1.5'),
        decimal.Decimal('NaN'),
        decimal.Decimal('sNaN'),
        DecimalSub('2.5'),
        fractions.Fraction(1, 3),
        FractionSub(2, 3),
        uuid.UUID(int=7),
        uuid.UUID(int=0),
        uuid.UUID('urn:uuid:12345678-1234-5678-1234-567812345678'),
        uuid.UUID('12345678123456781234567812345678'),
        UuidSub(int=9),
        pathlib.Path('a/b'),
        LyingPath('a/b'),
        ipaddress.IPv4Address('1.2.3.4'),
        ipaddress.IPv6Address('::1'),
        ipaddress.IPv4Network('1.2.3.0/24'),
        ipaddress.IPv6Network('::/64'),
        ipaddress.IPv4Interface('1.2.3.4/24'),
        ipaddress.IPv6Interface('::1/64'),
        pydantic_core_cpp.Url('https://example.com/x'),
        pydantic_core_cpp.MultiHostUrl('redis://host1:1/host2'),
        re.compile('a+'),
        re.compile('a+', re.IGNORECASE),
        [decimal.Decimal('1.5'), {'k': uuid.UUID(int=7)}],
        {'k': [ipaddress.IPv4Address('1.2.3.4')]},
    ]:
        both_writers_agree(value)

    # Pinned against the reference rather than against each other, since agreeing with itself is
    # worth nothing unless the form they agree on is the one Rust prints.
    assert any_ser.to_json(fractions.Fraction(1, 3)) == b'"1/3"'
    assert any_ser.to_json(DecimalSub('2.5')) == b'"2.5"'
    assert any_ser.to_json(decimal.Decimal('sNaN')) == b'"sNaN"'
    assert any_ser.to_json(uuid.UUID('urn:uuid:12345678-1234-5678-1234-567812345678')) == b'"12345678-1234-5678-1234-567812345678"'
    assert any_ser.to_json(LyingPath('a/b')) == b'"NOT-A-PATH"'
    assert any_ser.to_json(re.compile('a+', re.IGNORECASE)) == b'"a+"'
    assert any_ser.to_json({'k': [ipaddress.IPv4Address('1.2.3.4')]}) == b'{"k":["1.2.3.4"]}'

    # pyo3's display cannot fail, so a Decimal or Fraction whose __str__ raises is printed
    # anyway, as the placeholder naming its own class, and a UUID is still rebuilt from the int
    # its lying __str__ would have hidden.  The types that go through serialize_via_str end the
    # run with the value's own error, which the serde boundary renames to name the error it
    # wrapped (errors.rs:21, :63).
    assert any_ser.to_json(BoomDecimal('1.5')) == b'"<unprintable BoomDecimal object>"'
    assert any_ser.to_json(BoomFraction(1, 2)) == b'"<unprintable BoomFraction object>"'
    assert any_ser.to_json(BoomUUID(int=3)) == b'"00000000-0000-0000-0000-000000000003"'
    for boom in [BoomPath('a/b'), BoomIP('1.2.3.4')]:
        with pytest.raises(PydanticSerializationError) as exc:
            any_ser.to_json(boom)
        assert str(exc.value) == 'Error serializing to JSON: RuntimeError: no str', str(exc.value)

    # A UUID's int is extracted the way CPython extracts an unsigned integer, so the refusals of
    # that extraction are CPython's own words arriving through the same boundary.  These are
    # built past UUID.__init__ because UUID gives `int` a slot, which a subclass attribute would
    # not shadow by assignment.
    for cls, message in [
        (BigUuid, 'Error serializing to JSON: OverflowError: int too big to convert'),
        (NegUuid, "Error serializing to JSON: OverflowError: can't convert negative int to unsigned"),
        (NotIntUuid, "Error serializing to JSON: TypeError: 'str' object cannot be interpreted as an integer"),
    ]:
        with pytest.raises(PydanticSerializationError) as exc:
            any_ser.to_json(cls.__new__(cls))
        assert str(exc.value) == message, str(exc.value)

    # A value Rust's table does not name is refused by both writers, in the same words.
    for value in [pathlib.PurePosixPath('a/b'), object()]:
        with pytest.raises(PydanticSerializationError) as exc:
            any_ser.to_json(value)
        assert str(exc.value).startswith('Unable to serialize unknown type: '), str(exc.value)
        with pytest.raises(PydanticSerializationError) as obj_exc:
            any_ser.to_python(value, mode='json')
        assert str(obj_exc.value) == str(exc.value), str(obj_exc.value)


def test_the_jsonable_entry_point_prints_the_same_leaf_forms_as_the_other_json_runs():
    # to_jsonable_python is to_python with SerMode::Json (mod.rs:275), so it prints what the
    # other two json writers print.  It had its own copy of the leaf list -- plain str() of the
    # classes it knew, no ipaddress interface beyond those four, no placeholder for a display
    # that raises, and str() of a UUID rather than the bytes of its int.
    class BoomDecimal(decimal.Decimal):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomFraction(fractions.Fraction):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomPath(pathlib.PosixPath):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomIP(ipaddress.IPv4Address):
        def __str__(self):
            raise RuntimeError('no str')

    class BoomUUID(uuid.UUID):
        def __str__(self):
            raise RuntimeError('no str')

    class LyingPath(pathlib.PosixPath):
        def __str__(self):
            return 'NOT-A-PATH'

    any_ser = SchemaSerializer(core_schema.any_schema())

    for value in [
        decimal.Decimal('1.5'),
        decimal.Decimal('sNaN'),
        fractions.Fraction(1, 3),
        uuid.UUID(int=7),
        uuid.UUID('urn:uuid:12345678-1234-5678-1234-567812345678'),
        pathlib.Path('a/b'),
        LyingPath('a/b'),
        ipaddress.IPv4Address('1.2.3.4'),
        ipaddress.IPv6Network('::/64'),
        ipaddress.IPv4Interface('1.2.3.4/24'),
        pydantic_core_cpp.Url('https://example.com/x'),
        re.compile('a+', re.IGNORECASE),
        {'k': [decimal.Decimal('1.5'), uuid.UUID(int=0)]},
    ]:
        assert pydantic_core_cpp.to_jsonable_python(value) == any_ser.to_python(value, mode='json'), repr(value)

    # A display that raises is still the placeholder, and a UUID is still its int -- neither is
    # changed by serialize_unknown, which only speaks for values the table does not name.  A str
    # that raises on a type that goes through serialize_via_str ends the run with the value's own
    # error: this entry point has no serde boundary to rename it, so it arrives as it was raised.
    jsonable = pydantic_core_cpp.to_jsonable_python
    assert jsonable(BoomDecimal('1.5')) == '<unprintable BoomDecimal object>'
    assert jsonable(BoomFraction(1, 2)) == '<unprintable BoomFraction object>'
    assert jsonable(BoomUUID(int=3)) == '00000000-0000-0000-0000-000000000003'
    assert jsonable(BoomUUID(int=3), serialize_unknown=True) == '00000000-0000-0000-0000-000000000003'
    for boom in [BoomPath('a/b'), BoomIP('1.2.3.4')]:
        with pytest.raises(RuntimeError, match='no str'):
            jsonable(boom)
        with pytest.raises(RuntimeError, match='no str'):
            jsonable(boom, serialize_unknown=True)

    # Only an unknown value is serialize_unknown's to answer for: it takes str() of the value,
    # and a value whose str raises becomes the placeholder naming its qualified name.
    with pytest.raises(PydanticSerializationError) as exc:
        jsonable(pathlib.PurePosixPath('a/b'))
    assert str(exc.value) == 'Unable to serialize unknown type: <class \'pathlib.PurePosixPath\'>', str(exc.value)
    assert jsonable(pathlib.PurePosixPath('a/b'), serialize_unknown=True) == 'a/b'


def test_a_value_that_rewrites_its_own_text_is_asked_for_it():
    # A text form is asked of Python, not of the object's C type.  pybind's py::str is a checked
    # cast: shown a str subclass it hands back the content and never runs __str__, while Python's
    # str() -- and pyo3's .str(), which is what Rust calls here (infer.rs:188, :221, :613, :627) --
    # do.  A compiled pattern is where that shows, since Rust gives its pattern source's text, and
    # a pattern source is an ordinary Python object: a str subclass that rewrites its own text, or
    # one whose __str__ refuses.  The other leaves cannot show it -- CPython refuses to lay out a
    # class that is both a str and a UUID/Path/Decimal/IP -- so they are here only to say the
    # answer did not move.
    from typing import Any

    from pydantic import BaseModel, ConfigDict, TypeAdapter

    jsonable = pydantic_core_cpp.to_jsonable_python
    ser = SchemaSerializer(core_schema.any_schema())
    ta = TypeAdapter(Any)
    dump_python = ta.dump_python
    dump_json = ta.dump_json

    class RewrittenSource(str):
        def __str__(self):
            return 'REWRITTEN'

    class RefusingSource(str):
        def __str__(self):
            raise RuntimeError('no str')

    plain = type('Plain', (str,), {})('orig')
    rewritten = re.compile(RewrittenSource('b+'))
    refusing = re.compile(RefusingSource('b+'))

    assert ser.to_json(rewritten) == b'"REWRITTEN"'
    assert jsonable(rewritten) == 'REWRITTEN'
    assert dump_python({'k': [rewritten]}, mode='json') == {'k': ['REWRITTEN']}
    assert dump_json({'k': [rewritten]}) == b'{"k":["REWRITTEN"]}'
    # in a python run nothing is asked of the pattern at all
    assert dump_python(rewritten) is rewritten

    # The key path asks the same question, alone, inside a tuple key, and nested in a dict.
    assert dump_json({rewritten: 1}) == b'{"REWRITTEN":1}'
    assert ser.to_json({(rewritten,): 1}) == b'{"REWRITTEN":1}'
    assert dump_json({'k': {rewritten: 1}}) == b'{"k":{"REWRITTEN":1}}'
    assert jsonable({rewritten: 1}) == {'REWRITTEN': 1}
    assert dump_python({rewritten: 1}, mode='json') == {'REWRITTEN': 1}

    assert ta.dump_python(rewritten, mode='json') == 'REWRITTEN'
    assert ta.dump_json(rewritten) == b'"REWRITTEN"'

    class ModelWithPattern(BaseModel):
        f: Any = None

    assert ModelWithPattern(f=rewritten).model_dump(mode='json') == {'f': 'REWRITTEN'}
    assert ModelWithPattern(f=rewritten).model_dump_json() == '{"f":"REWRITTEN"}'

    class ModelWithExtra(BaseModel):
        model_config = ConfigDict(extra='allow')

    assert ModelWithExtra.model_validate({'x': rewritten}).model_dump(mode='json') == {'x': 'REWRITTEN'}

    # A source that refuses to speak ends the run with its own error.  Only the json-*text* walk has
    # a serde boundary to rename it with; the object-json walk and to_jsonable_python hand over the
    # error as raised.  The key path is no exception, where the port used to swallow the refusal
    # into an unknown-type refusal instead.
    with pytest.raises(RuntimeError, match='no str'):
        jsonable(refusing)
    with pytest.raises(RuntimeError, match='no str'):
        jsonable({refusing: 1})
    # serialize_unknown only speaks for values no kind names, and a pattern is named
    with pytest.raises(RuntimeError, match='no str'):
        jsonable(refusing, serialize_unknown=True)
    with pytest.raises(PydanticSerializationError) as text_exc:
        dump_json(refusing)
    assert str(text_exc.value) == 'Error serializing to JSON: RuntimeError: no str', str(text_exc.value)
    with pytest.raises(PydanticSerializationError) as key_exc:
        dump_json({refusing: 1})
    assert str(key_exc.value) == str(text_exc.value), str(key_exc.value)
    with pytest.raises(RuntimeError, match='no str'):
        ser.to_python(refusing, mode='json')
    with pytest.raises(RuntimeError, match='no str'):
        ser.to_python({refusing: 1}, mode='json')

    # A str that does not rewrite its text is asked for nothing: its content is its text, and the
    # key path uses the content just as Rust's Str arm does.
    assert ser.to_json(plain) == b'"orig"'
    assert dump_json({plain: 1}) == b'{"orig":1}'
    assert jsonable(plain) == 'orig'


def test_a_wrong_typed_value_is_warned_about_and_written_by_inference():
    # Rust's typed serializers never print a value their input type refuses: OnErr::Warn
    # (serializers/mod.rs) registers "Expected `int` - serialized value may not be as expected
    # [input_value='x', input_type=str]" and the value is then serialized the way inference
    # would print it.  The port wrote the value into the JSON buffer from str() instead, which
    # is not JSON at all (b'x', b'[a,b]', b"['a']"), and pybind's checked cast raised
    # "RuntimeError: Unable to cast Python instance of type <class 'str'> to C++ type 'double'"
    # for a str shown to the float, bool, bytes and str writers, so those runs answered nothing
    # at all.  A subclass is not a mismatch (IsType::Subclass), so bool at an int node, int at a
    # float node and a str/dict subclass ask for no warning.
    import warnings
    from collections import deque
    from typing import List

    from pydantic import BaseModel, TypeAdapter

    class StrSub(str):
        pass

    class DictSub(dict):
        pass

    int_ser = SchemaSerializer(core_schema.int_schema())
    list_int = SchemaSerializer(core_schema.list_schema(core_schema.int_schema()))
    dict_ser = SchemaSerializer(core_schema.dict_schema(core_schema.str_schema(), core_schema.int_schema()))

    # Every answer here leaves a warning behind; the blocks below ask for them and for none.
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        assert int_ser.to_json('x') == b'"x"'
        assert int_ser.to_python('x') == 'x'
        assert SchemaSerializer(core_schema.float_schema()).to_json('x') == b'"x"'
        assert SchemaSerializer(core_schema.bool_schema()).to_json('x') == b'"x"'
        assert SchemaSerializer(core_schema.bytes_schema()).to_json(1) == b'1'
        assert SchemaSerializer(core_schema.str_schema()).to_json(1) == b'1'

        # A container handed the wrong collection is inferred as a whole, not iterated
        # item-by-item through the declared item serializer.
        assert list_int.to_json('ab') == b'"ab"'
        assert list_int.to_python('ab') == 'ab'
        assert list_int.to_json({'a': 1}) == b'{"a":1}'
        assert list_int.to_json(deque([1])) == b'[1]'
        assert SchemaSerializer(core_schema.set_schema(core_schema.int_schema())).to_python(['a']) == ['a']
        assert dict_ser.to_json([1]) == b'[1]'
        assert SchemaSerializer(core_schema.tuple_positional_schema([core_schema.int_schema()])).to_python(['a']) == ['a']

        # Items of the right collection are each asked on their own.
        assert list_int.to_json(['a', 'b']) == b'["a","b"]'
        assert list_int.to_python(['a', 'b']) == ['a', 'b']

        # pydantic reaches this without touching the core API only through a value that
        # skipped validation: model_construct and model_copy(update=...).
        class M(BaseModel):
            v: int = 1
            xs: List[int] = [1]

        built = M.model_construct(v='x', xs=['a'])
        assert json.loads(built.model_dump_json()) == {'v': 'x', 'xs': ['a']}
        assert built.model_dump() == {'v': 'x', 'xs': ['a']}
        assert TypeAdapter(List[int]).dump_json(['a']) == b'["a"]'
        assert TypeAdapter(int).dump_json('x') == b'"x"'

    with pytest.warns(UserWarning, match='Expected `int` - serialized value may not be as expected'):
        int_ser.to_json('x')

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        SchemaSerializer(core_schema.str_schema()).to_json(StrSub('x'))
        dict_ser.to_json(DictSub({'k': 1}))
        int_ser.to_python(True)
        SchemaSerializer(core_schema.float_schema()).to_json(1)
    assert [str(w.message) for w in caught] == [], [str(w.message) for w in caught]


def test_a_json_run_of_a_tuple_asks_its_items_declared_serializers():
    import warnings

    def upper(v):
        return 'ZZZ'

    plain = core_schema.int_schema(
        serialization=core_schema.plain_serializer_function_ser_schema(upper, return_schema=core_schema.str_schema())
    )
    fn_ser = SchemaSerializer(core_schema.tuple_positional_schema([plain]))
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        # Inference has no idea a function was declared, so this used to print the input.
        assert fn_ser.to_json((1,)) == b'["ZZZ"]'
        assert fn_ser.to_python((1,)) == ('ZZZ',)

        pos = SchemaSerializer(core_schema.tuple_positional_schema([core_schema.int_schema()]))
        assert pos.to_json((1, 'a')) == b'[1,"a"]'
        var = SchemaSerializer(core_schema.tuple_variable_schema(core_schema.int_schema()))
        assert var.to_json((1, 'a')) == b'[1,"a"]'

        # The index filter belongs to the same item/serializer pairing.
        three = SchemaSerializer(
            core_schema.tuple_positional_schema(
                [core_schema.int_schema(), core_schema.str_schema(), core_schema.int_schema()]
            )
        )
        assert three.to_json((1, 'a', 3), include=[0]) == b'[1]'
        assert three.to_json((1, 'a', 3), include={0: True}) == b'[1]'
        assert three.to_json((1, 'a', 3), exclude=[1]) == b'[1,3]'
        assert three.to_json((1, 'a', 3), include=[-1]) == b'[]'
        assert three.to_python((1, 'a', 3), include=[0]) == (1,)
        assert var.to_json((1, 'a', 3), exclude=[0]) == b'["a",3]'

        # A declared item serializer runs at any depth of the json walk.
        nested = SchemaSerializer(
            core_schema.list_schema(core_schema.tuple_positional_schema([plain]))
        )
        assert nested.to_json([(1,), (2,)]) == b'[["ZZZ"],["ZZZ"]]'

    with pytest.warns(UserWarning, match='Expected `int` - serialized value may not be as expected'):
        pos.to_json(('a',))
    with pytest.warns(UserWarning, match='Expected `int` - serialized value may not be as expected'):
        var.to_json((1, 'a'), include=[1])


def test_a_tuples_item_count_is_checked_against_its_declaration():
    import warnings

    int_node = core_schema.int_schema()
    two = SchemaSerializer(core_schema.tuple_positional_schema([int_node, core_schema.str_schema()]))
    one = SchemaSerializer(core_schema.tuple_positional_schema([int_node]))
    var = SchemaSerializer(core_schema.tuple_variable_schema(int_node))

    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        # A short tuple answers with what it has; an extra item is not forced through the
        # last declared serializer, so nothing about it is expected to be an int.
        assert two.to_json((1,)) == b'[1]'
        assert two.to_python((1,)) == (1,)
        assert two.to_json(()) == b'[]'
        assert one.to_json((1, 2, 'x')) == b'[1,2,"x"]'
        assert one.to_python((1, 2, 'x')) == (1, 2, 'x')
        # A variadic tuple repeats its one serializer, at any length.
        assert var.to_json((1, 2, 3, 'a')) == b'[1,2,3,"a"]'
        # The count warning is about the declaration, not about what the filter kept.
        assert one.to_json((1, 2, 3), include=[0]) == b'[1]'

    with pytest.warns(UserWarning, match='Unexpected too few items present in tuple'):
        two.to_json((1,))
    with pytest.warns(UserWarning, match='Unexpected too few items present in tuple'):
        two.to_python((1,))
    with pytest.warns(UserWarning, match='Unexpected extra items present in tuple'):
        one.to_json((1, 2, 'x'))

    # Once, and only the count: the extra str owes no Expected `int` of its own.
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        one.to_json((1, 2, 'x'))
    assert [str(w.message) for w in caught] == [
        "Pydantic serializer warnings:\n  PydanticSerializationUnexpectedValue(Unexpected extra items present in tuple)"
    ], [str(w.message) for w in caught]

    # A variadic tuple never mentions the count, whatever its length.
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        var.to_json((1, 2, 3))
        var.to_json(())
    assert [str(w.message) for w in caught] == [], [str(w.message) for w in caught]


def test_none_is_what_every_node_answers_to_a_none():
    import warnings

    def TP(*items):
        return core_schema.tuple_positional_schema(list(items))

    typed = [
        core_schema.int_schema(),
        core_schema.float_schema(),
        core_schema.bool_schema(),
        core_schema.str_schema(),
        core_schema.bytes_schema(),
        core_schema.date_schema(),
        core_schema.decimal_schema(),
        core_schema.uuid_schema(),
        core_schema.list_schema(core_schema.int_schema()),
        core_schema.dict_schema(core_schema.str_schema(), core_schema.int_schema()),
        core_schema.set_schema(core_schema.int_schema()),
        TP(core_schema.int_schema()),
        core_schema.tuple_variable_schema(core_schema.int_schema()),
        core_schema.nullable_schema(core_schema.int_schema()),
        core_schema.union_schema([core_schema.int_schema(), core_schema.str_schema()]),
        core_schema.any_schema(),
        core_schema.none_schema(),
    ]
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        for sch in typed:
            ser = SchemaSerializer(sch)
            assert ser.to_json(None) == b'null', sch['type']
            assert ser.to_python(None) is None, sch['type']
    # Null is not a surprise: none of those nodes warns about it.
    assert [str(w.message) for w in caught] == [], [str(w.message) for w in caught]

    class Inner:
        def __init__(self, x: int = 1):
            self.x = x

    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        model_ser = SchemaSerializer(
            core_schema.model_schema(
                Inner,
                core_schema.typed_dict_schema({'x': core_schema.typed_dict_field(core_schema.int_schema())}),
                root_model=False,
            )
        )
        assert model_ser.to_json(None) == b'null'

        # A None item inside a container is nulled where the container's item node is typed.
        assert SchemaSerializer(core_schema.list_schema(core_schema.int_schema())).to_json([1, None]) == b'[1,null]'
        assert SchemaSerializer(TP(core_schema.int_schema(), core_schema.str_schema())).to_json((1, None)) == b'[1,null]'
        assert SchemaSerializer(core_schema.dict_schema(core_schema.str_schema(), core_schema.int_schema())).to_json({'a': None}) == b'{"a":null}'
        assert SchemaSerializer(core_schema.list_schema(core_schema.str_schema())).to_python([None]) == [None]

        # The key path is the one place None is not nulled -- it is stringified there.
        assert SchemaSerializer(core_schema.dict_schema(core_schema.int_schema(), core_schema.str_schema())).to_json({None: 'v'}) == b'{"None":"v"}'
        assert SchemaSerializer(core_schema.dict_schema(core_schema.int_schema(), core_schema.str_schema())).to_python({None: 'v'}) == {None: 'v'}


def test_a_tuple_owes_the_name_of_every_item_serializer():
    import re
    import warnings

    def TP(*items):
        return core_schema.tuple_positional_schema(list(items))

    def expected_text(fn, value='k'):
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter('always')
            assert fn(value) == b'"k"'
        assert len(caught) == 1, [str(w.message) for w in caught]
        return ' '.join(str(caught[0].message).split())

    def warned_name(inner, wrap=True):
        schema = core_schema.dict_schema(core_schema.str_schema(), inner) if wrap else inner
        text = expected_text(SchemaSerializer(schema).to_json)
        return re.search(r'Expected `([^`]*)`', text).group(1)

    I, S = core_schema.int_schema(), core_schema.str_schema()
    # Rust builds a tuple's name from every item serializer, with '...' after the variadic
    # one, so the name says which of them the value disagreed with.
    assert warned_name(core_schema.tuple_variable_schema(I)) == 'dict[str, tuple[int, ...]]'
    assert warned_name(TP(I, S)) == 'dict[str, tuple[int, str]]'
    assert warned_name(TP()) == 'dict[str, tuple[]]'
    assert warned_name(TP(TP(I, S))) == 'dict[str, tuple[tuple[int, str]]]'
    assert warned_name(core_schema.tuple_variable_schema(core_schema.list_schema(I))) == 'dict[str, tuple[list[int], ...]]'
    # Asked about directly, the tuple owes its own name and nothing around it.
    assert warned_name(core_schema.tuple_variable_schema(I), wrap=False) == 'tuple[int, ...]'
    assert warned_name(TP(I, S), wrap=False) == 'tuple[int, str]'
    assert warned_name(TP(), wrap=False) == 'tuple[]'



def test_a_node_with_a_value_type_of_its_own_says_so_about_a_foreign_one():
    import warnings

    class MyDate(datetime.date):
        pass

    class MyDecimal(decimal.Decimal):
        pass

    class MyUrl(pydantic_core_cpp.Url):
        pass

    def gen():
        yield 1

    def warned(schema, value):
        s = SchemaSerializer(schema)
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter('always')
            out = s.to_json(value)
        name = None
        for w in caught:
            m = re.search(r'Expected `([^`]*)`', re.sub(r'\s+', ' ', str(w.message)))
            if m:
                name = m.group(1)
        return name, out

    # A leaf node that has a value type of its own refuses anything else: it warns with its own
    # name and the value is then inferred on its own, so an int at a date node costs b'1'.
    for schema, value, name, jbytes in [
        (core_schema.date_schema(), 1, 'date', b'1'),
        (core_schema.date_schema(), b'x', 'date', b'"x"'),
        (core_schema.date_schema(), 2 + 3j, 'date', b'"2+3j"'),
        (core_schema.date_schema(), datetime.datetime(2020, 1, 2, 3, 4),
         'date', b'"2020-01-02T03:04:00"'),
        (core_schema.datetime_schema(), datetime.date(2020, 1, 2), 'datetime', b'"2020-01-02"'),
        (core_schema.time_schema(), 'x', 'time', b'"x"'),
        (core_schema.timedelta_schema(), 'x', 'timedelta', b'"x"'),
        (core_schema.decimal_schema(), 1, 'decimal', b'1'),
        (core_schema.uuid_schema(), 1, 'uuid', b'1'),
        (core_schema.complex_schema(), 1, 'complex', b'1'),
        (core_schema.url_schema(), 'x', 'url', b'"x"'),
        (core_schema.multi_host_url_schema(), pydantic_core_cpp.Url('https://example.com/x'),
         'multi-host-url', b'"https://example.com/x"'),
        (core_schema.generator_schema(core_schema.int_schema()), 'abc', 'generator', b'"abc"'),
        (core_schema.generator_schema(core_schema.int_schema()), b'abc', 'generator', b'"abc"'),
        (core_schema.generator_schema(core_schema.int_schema()), 1, 'generator', b'1'),
        (core_schema.generator_schema(core_schema.int_schema()), ipaddress.IPv4Network('127.0.0.0/24'),
         'generator', b'"127.0.0.0/24"'),
        # The generator node asks for the iterator protocol, so an iterable is foreign even
        # when it would print the same array.
        (core_schema.generator_schema(core_schema.int_schema()), [1, 2], 'generator', b'[1,2]'),
    ]:
        assert warned(schema, value) == (name, jbytes), (name, value)

    # A subclass is fine, and the exact type is fine: neither says anything.
    for schema, value, jbytes in [
        (core_schema.date_schema(), MyDate(2020, 1, 2), b'"2020-01-02"'),
        (core_schema.decimal_schema(), MyDecimal('1.5'), b'"1.5"'),
        (core_schema.url_schema(), pydantic_core_cpp.Url('https://example.com/x'),
         b'"https://example.com/x"'),
        (core_schema.url_schema(), MyUrl('https://example.com/x'), b'"https://example.com/x"'),
        (core_schema.uuid_schema(), uuid.UUID('12345678-1234-5678-1234-567812345678'),
         b'"12345678-1234-5678-1234-567812345678"'),
        (core_schema.complex_schema(), 2 + 3j, b'"2+3j"'),
        (core_schema.timedelta_schema(), datetime.timedelta(seconds=5), b'"PT5S"'),
        (core_schema.multi_host_url_schema(), pydantic_core_cpp.MultiHostUrl('redis://host1:1/host2'),
         b'"redis://host1:1/host2"'),
        (core_schema.generator_schema(core_schema.int_schema()), gen(), b'[1]'),
        (core_schema.generator_schema(core_schema.int_schema()), iter([1, 2]), b'[1,2]'),
    ]:
        assert warned(schema, value) == (None, jbytes), value


def test_a_float_is_written_as_the_shortest_text_that_reads_back():
    # serde_json writes a float through ryu: the shortest digits that round-trip, in plain
    # decimal while the leading digit sits between 1e-5 and 1e16, in exponential form outside
    # it with an exponent that is neither padded nor signed for the negative case, and with a
    # ".0" on a value that has no fraction.  Six-decimal text was the alternative here, which
    # is a different number: 3.14159265358979 came out as 3.141593 and 1e-7 as 0.0.
    float_schema = core_schema.float_schema()
    typed = SchemaSerializer(float_schema)
    any_node = SchemaSerializer(core_schema.any_schema())
    for value, text in [
        (0.0, b'0.0'),
        (-0.0, b'-0.0'),
        (1.0, b'1.0'),
        (1.5, b'1.5'),
        (3.14159265358979, b'3.14159265358979'),
        (123456.789, b'123456.789'),
        (0.0001, b'0.0001'),
        (0.00001, b'0.00001'),
        (1e-6, b'1e-6'),
        (1e-7, b'1e-7'),
        (1e15, b'1000000000000000.0'),
        (1e16, b'1e+16'),
        (1e300, b'1e+300'),
    ]:
        assert typed.to_json(value) == text, value
        assert any_node.to_json(value) == text, value
        assert typed.to_python(value) == value, value

    # An int at a float node is asked for its number, so a too-big one leaves as the double it
    # becomes -- digits beyond the double are gone, and no ".0" tail stands in for them.
    assert typed.to_json(2 ** 70) == b'1.1805916207174113e+21'

    # A non-finite float is still the mode's business, not the number form's: the entry point's
    # own answer for these is b'null', mode config notwithstanding.
    assert typed.to_json(math.inf) == b'null'


def test_a_numeric_node_writes_its_own_number_rather_than_the_values_shape():
    import warnings

    int_node = SchemaSerializer(core_schema.int_schema())
    float_node = SchemaSerializer(core_schema.float_schema())

    def warn_names(fn):
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter('always')
            out = fn()
        names = []
        for w in caught:
            m = re.search(r'Expected `([^`]*)`', re.sub(r'\s+', ' ', str(w.message)))
            names.append(m.group(1) if m else str(w.message)[:30])
        return names, out

    # Rust's numeric leaves serialize the node's number -- an int node an i64, a float node a
    # double -- so a bool costs 1 or 1.0 rather than true, in both JSON shapes.  A python run
    # takes the other arm of the same IsType match and hands back the object it was given.
    assert int_node.to_json(True) == b'1'
    assert int_node.to_json(False) == b'0'
    assert float_node.to_json(True) == b'1.0'
    assert float_node.to_json(1) == b'1.0'
    for value in (True, False):
        assert type(int_node.to_python(value, mode='json')) is int
        assert type(float_node.to_python(value, mode='json')) is float
    assert int_node.to_python(True) is True
    assert float_node.to_python(True) is True
    assert type(float_node.to_python(1)) is int and float_node.to_python(1) == 1

    # The same arm extracts an exact int/float, so a subclass does not ride along with the answer.
    class MyInt(int):
        pass

    class MyFloat(float):
        pass

    assert type(int_node.to_python(MyInt(3), mode='json')) is int
    assert type(float_node.to_python(MyFloat(3.5), mode='json')) is float
    assert type(float_node.to_python(MyInt(3), mode='json')) is float
    assert int_node.to_python(MyInt(3)) is None or type(int_node.to_python(MyInt(3))) is MyInt

    # Items of a collection are each answered by their own node, so a list of ints answers for
    # every bool in it -- and a mixed list of ints and bools says nothing while doing so.
    def kinds(v):
        return [type(x).__name__ for x in v]

    ints = SchemaSerializer(core_schema.list_schema(core_schema.int_schema()))
    floats = SchemaSerializer(core_schema.list_schema(core_schema.float_schema()))
    assert ints.to_json([True, 1]) == b'[1,1]'
    assert kinds(ints.to_python([True, 1], mode='json')) == ['int', 'int']
    assert kinds(ints.to_python([True, 1])) == ['bool', 'int']
    assert floats.to_json([True, 1]) == b'[1.0,1.0]'
    assert kinds(floats.to_python([True, 1], mode='json')) == ['float', 'float']

    # The float node is asked before the mismatch rule, because Rust's serde arm asks its
    # converter and nothing else: a Decimal becomes its double here and says nothing, while the
    # very same node in a jsonable run warns and infers the text instead.
    names, out = warn_names(lambda: float_node.to_json(decimal.Decimal('1.5')))
    assert out == b'1.5' and names == []
    names, out = warn_names(lambda: float_node.to_json(fractions.Fraction(1, 2)))
    assert out == b'0.5' and names == []
    names, out = warn_names(lambda: float_node.to_python(decimal.Decimal('1.5'), mode='json'))
    assert out == '1.5' and names == ['float']

    # An int past the range of a double is the one value the node's own type check accepts and
    # this writer must not: the json run warns and prints the digits, the jsonable one lets
    # float() raise, which is what the wheel does.
    names, out = warn_names(lambda: float_node.to_json(10 ** 400))
    assert out == b'1' + b'0' * 400 and names == ['float']
    with pytest.raises(OverflowError):
        float_node.to_python(10 ** 400, mode='json')


def test_a_json_run_answers_a_scalar_subclass_as_the_exact_type_it_subclasses():
    import warnings

    # Rust's json run rebuilds a scalar subclass as the exact type it subclasses, both at a leaf
    # that took it as a subclass and in the infer walk a mismatched node falls back to
    # (infer.rs:106-124, string.rs:47-49): what such a run holds is a number or a text, not an
    # object that merely behaves like one.  A python run has no arm of that match at all, which is
    # why model_dump() keeps the class while model_dump(mode='json') does not.
    class MyInt(int):
        def __int__(self):
            return 99

    class MyFloat(float):
        def __float__(self):
            return 99.5

    class MyStr(str):
        def __str__(self):
            return 'nope'

    class BigMyInt(int):
        pass

    int_node = SchemaSerializer(core_schema.int_schema())
    float_node = SchemaSerializer(core_schema.float_schema())
    str_node = SchemaSerializer(core_schema.str_schema())
    ints_node = SchemaSerializer(core_schema.list_schema(core_schema.int_schema()))
    any_node = SchemaSerializer(core_schema.any_schema())

    # The answer carries its type as well as its value, because MyInt(3) == 3 and a cell that
    # compared values alone would read as a pass through the whole cluster.
    def answered(fn):
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            out = fn()
        return type(out).__name__, out

    # The rebuild is a read of what the value holds, not of what its class converts to, so a
    # subclass that writes its own converter still answers what it was built from.
    assert answered(lambda: any_node.to_python(MyInt(3), mode='json')) == ('int', 3)
    assert answered(lambda: any_node.to_python(MyFloat(3.5), mode='json')) == ('float', 3.5)
    assert answered(lambda: any_node.to_python(MyStr('ab'), mode='json')) == ('str', 'ab')
    assert answered(lambda: any_node.to_python(BigMyInt(2**70), mode='json')) == ('int', 2**70)
    assert any_node.to_json(MyInt(3)) == b'3'
    assert any_node.to_json(MyFloat(3.5)) == b'3.5'
    assert any_node.to_json(MyStr('ab')) == b'"ab"'

    # A leaf of the node's own type takes the same arm of the same match.
    assert answered(lambda: int_node.to_python(MyInt(3), mode='json')) == ('int', 3)
    assert answered(lambda: float_node.to_python(MyFloat(3.5), mode='json')) == ('float', 3.5)
    assert answered(lambda: str_node.to_python(MyStr('ab'), mode='json')) == ('str', 'ab')

    # So does a node that warned the value in and inferred it, which is where the bytes of the run
    # were already right and only the type of the object was wrong.
    assert answered(lambda: ints_node.to_python(MyInt(3), mode='json')) == ('int', 3)
    assert answered(lambda: int_node.to_python(MyStr('ab'), mode='json')) == ('str', 'ab')
    assert answered(lambda: int_node.to_python(MyFloat(3.5), mode='json')) == ('float', 3.5)

    # The standalone entry runs the same walk, and the python mode of both keeps the class.
    assert answered(lambda: to_jsonable_python(MyInt(3))) == ('int', 3)
    assert answered(lambda: to_jsonable_python(MyFloat(3.5))) == ('float', 3.5)
    assert answered(lambda: to_jsonable_python(MyStr('ab'))) == ('str', 'ab')
    for value in (MyInt(3), MyFloat(3.5), MyStr('ab')):
        assert type(any_node.to_python(value)) is type(value)
        assert type(int_node.to_python(value)) is type(value)

    # A bool is an exact type of its own and no rebuild turns it into an int.
    assert answered(lambda: any_node.to_python(True, mode='json')) == ('bool', True)


def test_the_walk_refuses_a_repeated_id_and_a_run_that_only_gets_absurdly_deep():
    # Rust takes a recursion guard for every value the infer walks, not only for containers
    # (infer.rs:52-67, recursion_guard.rs:32-42), and it answers two questions at once: an id
    # already open on the way down is a reference cycle, and a walk with more than 255 values
    # open at once is refused the same way it is stopped. The caller splits by mode (extra.rs:93-94):
    # a python run swallows either answer and hands back the value it was given, while a json run
    # -- and mode='json' is a json run -- lets the ValueError out, which to_json then wraps.
    ser = SchemaSerializer(core_schema.any_schema())

    def answered(fn):
        try:
            return ('ok', fn())
        except BaseException as e:
            return (type(e).__name__, str(e))

    def nest(n):
        v = 1
        for _ in range(n):
            v = [v]
        return v

    selfdict = {}
    selfdict['self'] = selfdict

    # A cycle comes back as the same cyclic shape and a 300-deep list comes back whole.
    assert ser.to_python(selfdict)['self'] is selfdict
    assert ser.to_python(nest(300)) == nest(300)

    assert answered(lambda: ser.to_python(selfdict, mode='json')) == (
        'ValueError', 'Circular reference detected (id repeated)')
    assert answered(lambda: to_jsonable_python(selfdict)) == (
        'ValueError', 'Circular reference detected (id repeated)')
    assert answered(lambda: ser.to_json(selfdict)) == (
        'PydanticSerializationError',
        'Error serializing to JSON: ValueError: Circular reference detected (id repeated)')

    # The bound is on values open at once, so the walk goes 255 deep and refuses the 256th.
    assert answered(lambda: to_jsonable_python(nest(254))) == ('ok', nest(254))
    assert answered(lambda: to_jsonable_python(nest(255))) == (
        'ValueError', 'Circular reference detected (depth exceeded)')

    # A fallback that answers with the value it was handed repeats an id at the second step, and
    # one that invents a fresh object never repeats an id at all, so only the depth bound can stop
    # it. Neither value is a container, so before this guard the second of them recursed until the
    # C stack ran out -- a segfault rather than an error. The call count is where the walk gave up.
    class Unknown:
        pass

    asked = []

    def fresh(v):
        asked.append(v)
        return Unknown()

    def same(v):
        asked.append(v)
        return v

    assert type(ser.to_python(Unknown(), fallback=same)).__name__ == 'Unknown'
    assert len(asked) == 1
    asked.clear()
    assert type(ser.to_python(Unknown(), fallback=fresh)).__name__ == 'Unknown'
    assert len(asked) == 255

    assert answered(lambda: ser.to_python(Unknown(), mode='json', fallback=same)) == (
        'ValueError', 'Circular reference detected (id repeated)')
    assert answered(lambda: ser.to_python(Unknown(), mode='json', fallback=fresh)) == (
        'ValueError', 'Circular reference detected (depth exceeded)')


def test_a_warning_a_filter_turns_into_an_error_escapes_a_json_run_as_itself():
    # Rust asks the warnings a run collected for their say only after the encoded bytes have come
    # back through the serde mapping (mod.rs:189 runs after to_json_bytes), so an escalated
    # warning never takes the "Error serializing to JSON: " name that a failure of the encoding
    # itself does: in warn mode a filter that turns the UserWarning into an error lets that
    # UserWarning out, and warnings='error' raises a bare PydanticSerializationError.  A run that
    # failed on the way to its bytes never reaches the flush and says nothing at all.
    ser = SchemaSerializer(core_schema.list_schema(core_schema.int_schema()))

    def caught(fn):
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter('always')
            try:
                out = ('ok', fn())
            except BaseException as e:
                out = (type(e).__name__, str(e).split('\n')[0])
            return out, len(w)

    def under_error_filter(fn):
        with warnings.catch_warnings():
            warnings.simplefilter('error')
            try:
                return ('ok', fn())
            except BaseException as e:
                return (type(e).__name__, str(e).split('\n')[0])

    assert under_error_filter(lambda: ser.to_json(['a'])) == (
        'UserWarning', 'Pydantic serializer warnings:')
    assert under_error_filter(lambda: ser.to_python(['a'])) == (
        'UserWarning', 'Pydantic serializer warnings:')

    assert caught(lambda: ser.to_json(['a'], warnings='error')) == (
        ('PydanticSerializationError', 'Pydantic serializer warnings:'), 0)
    assert caught(lambda: ser.to_python(['a'], warnings='error')) == (
        ('PydanticSerializationError', 'Pydantic serializer warnings:'), 0)

    # The bytes still come back -- the warning is what the run has to say afterwards.
    assert caught(lambda: ser.to_json(['a'])) == (('ok', b'["a"]'), 1)

    # A run that never got its bytes keeps its warnings to itself, and a finished run leaves
    # nothing behind for the next one.
    assert caught(lambda: ser.to_json(['a', object()])) == (
        ('PydanticSerializationError', "Unable to serialize unknown type: <class 'object'>"), 0)
    assert caught(lambda: ser.to_json(['b'])) == (('ok', b'["b"]'), 1)


def test_an_iterator_handed_to_the_walk_comes_back_as_a_lazy_view_of_itself():
    # infer.rs:264-271 (ObType::Generator): a python run does not consume the iterator it was
    # handed.  It hands the caller a SerializationIterator that serializes each item as that item
    # is pulled, so the items are never materialized -- which is also the answer a node of another
    # type gives once it has warned about the value and fallen through to inference.  A json run
    # has nowhere to keep the laziness, so it drains the iterator into a list.  A range is not an
    # iterator and stays exactly what it is.
    ser = SchemaSerializer(core_schema.any_schema())

    it = iter([1, 2])
    view = ser.to_python(it)
    assert type(view).__name__ == 'SerializationIterator'
    assert iter(view) is view
    assert view.index == 0
    assert repr(view) == 'SerializationIterator(index=0, iterator=%s)' % repr(it)

    assert next(view) == 1
    assert view.index == 1
    assert list(view) == [2]
    assert view.index == 2
    # Single use, like the iterator underneath it.
    assert list(view) == []
    assert repr(view) == 'SerializationIterator(index=2, iterator=%s)' % repr(it)

    # The items go through the walk itself, so an iterator among the items is a view too.
    nested = next(ser.to_python(iter([iter([7])])))
    assert type(nested).__name__ == 'SerializationIterator'
    assert list(nested) == [7]

    assert list(ser.to_python(iter([]))) == []

    int_ser = SchemaSerializer(core_schema.int_schema())
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        refused = int_ser.to_python(iter([1, 2]))
    assert type(refused).__name__ == 'SerializationIterator'
    assert len(caught) == 1

    assert type(ser.to_python(range(3))).__name__ == 'range'
    assert ser.to_python(range(3)) == range(3)

    assert ser.to_python(iter([1, 2]), mode='json') == [1, 2]
    assert ser.to_json(iter([1, 2])) == b'[1,2]'
    assert to_jsonable_python(iter([1, 2])) == [1, 2]
