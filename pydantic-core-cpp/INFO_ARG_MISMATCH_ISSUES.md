# Systematic Info Argument Mismatch Issues in C++ Serializer/Validator

**Date**: 2026-08-13  
**Status**: Partially fixed - systematic issue identified

## Overview

A systematic issue was discovered where C++ serializer and validator functions are not correctly handling the `info` argument (SerializationInfo/ValidationInfo) when calling Python callable functions. This affects field serializers, wrap-mode serializers, and validators that expect an info parameter.

## Original Issue (FIXED)

**Test**: `tests/test_json.py::test_json_nested_encode_models`

**Problem**: 
- Field serializers with `when_used='unless-none'` were failing when field value was `None`
- Error: `TypeError: serialize_user() missing 1 required positional argument: '_info'`

**Root Cause**:
1. `when_used` parameter from serialization schema was not being extracted or handled
2. Field serializers with `mode='wrap'` were not creating handler functions
3. Fallback serialization path was trying to call field serializer without proper context

**Fix Applied**:
- Added `when_used` field to `SerNode` struct (line 295)
- Extract `when_used` from serialization schema during build (lines 1165-1167)
- Check `when_used` conditions before calling field serializer (lines 930-943, 1090-1103)
- Create handler function for `function-wrap` field serializers (lines 946-960, 1114-1135)
- Preserve inner schema as children for fallback serialization (line 1332)

**Status**: ✅ PASSING

## Systematic Pattern Discovered

After fixing the original issue, scanning other tests revealed a broader pattern of info argument mismatches across multiple test files.

### Affected Tests

#### test_serialize.py (7 tests)
1. **test_serialize_valid_signatures**
   - Field serializers with `FieldSerializationInfo` parameter
   - Error: `ser_f1() missing 1 required positional argument: 'info'`
   - Schema has `info_arg: true` but C++ not passing it

2. **test_serialize_ignore_info_wrap**
   - Wrap serializer that should NOT receive info argument
   - Error: `ser_x() takes 2 positional arguments but 3 were given`
   - C++ passing info when function doesn't expect it

3. **test_serialize_python_context**
   - Serializer with info arg in Python mode
   - Error: `serialize_x() missing 1 required positional argument: 'info'`

4. **test_serialize_json_context**
   - Serializer with info arg in JSON mode
   - Error: `serialize_x() missing 1 required positional argument: 'info'`

5. **test_serializer_annotated_wrap_always**
   - Wrap serializer without info parameter
   - Error: `ser_wrap() takes 2 positional arguments but 3 were given`
   - C++ passing info when function signature is `(v, handler)` only

6. **test_serializer_annotated_wrap_json**
   - Wrap serializer in JSON mode without info parameter
   - Error: `ser_wrap() takes 2 positional arguments but 3 were given`

7. **test_model_serializer_wrap**
   - Model-level wrap serializer without info parameter
   - Error: `_serialize() takes 2 positional arguments but 3 were given`

8. **test_forward_ref_for_serializers**
   - Forward reference serializer without info parameter
   - Error: `ser_model_func() takes 2 positional arguments but 3 were given`

#### test_validators.py (3 tests)
1. **test_annotated_validator_wrap**
   - Wrap validator with `ValidationInfo` parameter
   - Signature: `sixties_validator(val, handler, info)`
   - Error: `sixties_validator() missing 1 required positional argument: 'info'`
   - C++ calling with only `(val, handler)`, missing `info`

2. **test_info_field_name_data_before**
   - Before validator with info parameter
   - Error: `check_a() missing 1 required positional argument: 'info'`

3. **test_wrap_validator_field_name**
   - Wrap validator with info parameter
   - Signature: `validate_wrap(value, handler, info)`
   - Error: `validate_wrap() missing 1 required positional argument: 'info'`

#### test_model_validator.py (2 tests)
1. **test_model_validator_before**
   - Model validator (before mode) with info parameter
   - Error: `val_model() missing 1 required positional argument: 'info'`

2. **test_model_validator_before_revalidate_always**
   - Model validator with revalidate_always config
   - Error: `val_model() missing 1 required positional argument: 'info'`

## Root Cause Analysis

### Problem 1: Info Argument Detection
The C++ code extracts `info_arg` from the schema correctly, but doesn't consistently apply it when calling functions. The logic for when to pass the info argument needs refinement:

**Current behavior**:
- For `function-plain`: Checks `info_arg` and passes info if true ✅
- For `function-wrap`: Always passes info (hardcoded) ❌
- For validators: Similar inconsistency ❌

**Expected behavior**:
- Check `info_arg` flag from schema for all function types
- Pass info argument only when `info_arg=true`
- For wrap functions: `(val, handler)` or `(val, handler, info)` based on `info_arg`

### Problem 2: Function Signature Introspection
Python's `@field_serializer` and validator decorators introspect function signatures to determine if they expect an info argument. The C++ code relies on the `info_arg` flag in the schema, but this flag may not always be set correctly or may be ignored in certain code paths.

### Problem 3: Wrap Mode Handler Creation
For wrap-mode functions (both serializers and validators), the C++ code creates a handler callable but doesn't consistently pass it along with the info argument based on the function's signature.

## Code Locations

### Serializer Code
- **File**: `/Users/cuser/Documents/github/pydantic/pydantic-core-cpp/src/main_module.cpp`
- **SerNode struct**: Lines 276-310 (added `when_used` field)
- **build_ser_impl**: Lines 1140-1170 (extract `when_used`)
- **serialize_fields**: Lines 929-980 (field serializer invocation)
- **serialize_fields_json**: Lines 1114-1165 (JSON mode field serializer)
- **to_python**: Lines 417-444 (general function serializer)
- **to_json**: Lines 710-720 (JSON mode general serializer)

### Validator Code
- **File**: `/Users/cuser/Documents/github/pydantic/pydantic-core-cpp/src/main_module.cpp`
- Validator invocation logic needs similar fixes to serializer code

## Fix Strategy

### Phase 1: Serializer Info Argument (Partially Done)
- ✅ Add `when_used` support
- ✅ Create handler for wrap-mode field serializers
- ❌ Fix info argument passing for all serializer types
- ❌ Handle edge cases (ignore_info_wrap, context, etc.)

### Phase 2: Validator Info Argument (Not Started)
- ❌ Extract `info_arg` from validator schemas
- ❌ Pass info argument based on `info_arg` flag
- ❌ Create handler for wrap-mode validators
- ❌ Handle before/after/plain/wrap modes consistently

### Phase 3: Testing
- ❌ Fix all 6 identified failing tests
- ❌ Run full test suite to find any other affected tests
- ❌ Add regression tests for info argument handling

## Test Cases to Fix

```python
# Pattern 1: Field serializer with info arg
@field_serializer('field_name')
def serialize_field(self, v: Any, info: FieldSerializationInfo) -> Any:
    return v

# Pattern 2: Wrap serializer with info arg
@field_serializer('field_name', mode='wrap')
def serialize_field(self, v: Any, handler: SerializerFunctionWrapHandler, info: FieldSerializationInfo) -> Any:
    return handler(v)

# Pattern 3: Wrap validator with info arg
@field_validator('field_name', mode='wrap')
def validate_field(cls, v: Any, handler: ValidatorFunctionWrapHandler, info: ValidationInfo) -> Any:
    return handler(v)
```

All three patterns should work correctly when the function signature includes an info parameter.

## Impact

- **Severity**: High - affects core serialization/validation functionality
- **Scope**: Multiple test files, likely affects user code using info parameters
- **Risk**: Low - fixes are localized to function invocation logic

## Next Steps

1. Investigate why `test_serialize_valid_signatures` still fails after wrap-mode fix
2. Fix info argument passing for all serializer function types
3. Apply same fixes to validator code
4. Run full test suite to identify any other affected tests
5. Add comprehensive tests for info argument handling

## References

- Original issue: `tests/test_json.py::test_json_nested_encode_models`
- Pydantic serialization docs: https://docs.pydantic.dev/latest/concepts/serialization/
- Pydantic validator docs: https://docs.pydantic.dev/latest/concepts/validators/

## Scan Results Summary (2026-08-13)

**Total tests scanned**: 30+ test files  
**Total affected tests**: 13 tests across 3 files

### Breakdown by file:
- `test_serialize.py`: 8 tests
- `test_validators.py`: 3 tests  
- `test_model_validator.py`: 2 tests

### Breakdown by error type:
- **Missing info argument** (7 tests): Function expects info but C++ doesn't pass it
  - Affects: field serializers with info param, validators with info param
- **Unexpected info argument** (6 tests): Function doesn't expect info but C++ passes it
  - Affects: wrap-mode serializers/validators without info param

### Files scanned with NO info arg issues:
- test_types.py, test_generics.py, test_discriminated_union.py
- test_computed_fields.py, test_root_model.py, test_types_typeddict.py
- test_networks.py, test_datetime.py, test_serialize_as_any.py
- test_fields.py, test_annotated.py, test_callable.py
- test_decorators.py, test_config.py, test_validators_dataclass.py
- test_type_adapter.py, test_edge_cases.py, test_main.py
- test_private_attributes.py, test_create_model.py
- test_json_schema.py, test_parse.py, test_pipeline.py
- test_strict.py, test_types_namedtuple.py
