import dataclasses
import json
import platform
import re
from collections import deque

import pytest
from dirty_equals import IsFloatNan, IsList

import pydantic_core_cpp
from pydantic_core_cpp import (
    CoreConfig,
    PydanticSerializationError,
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
