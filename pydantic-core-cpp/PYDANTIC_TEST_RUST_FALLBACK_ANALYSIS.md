# pydantic Python 测试 Rust Fallback 详细分析

> 分析日期: 2026-07-10
> 分析目标: 当以 `pydantic_core_cpp` (C++ 后端) 为后端运行 pydantic 测试时，精确识别哪些测试会走 Rust fallback

---

## 架构概述

```
┌─────────────────────────────────────────────────────────────────┐
│                    pydantic Python 测试 (~2,659 个)              │
├─────────────────────────────────────────────────────────────────┤
│                     pydantic 库 (Python)                         │
├──────────────────┬──────────────────┬───────────────────────────┤
│  pydantic_core   │  pydantic_core   │  pydantic_core           │
│  SchemaValidator │  SchemaSerializer│  core_schema.*           │
│  ValidationError │  Url/MultiHostUrl│  PydanticCustomError     │
│  ArgsKwargs      │  PydanticUndefined│  from_json/to_json      │
├──────────────────┴──────────────────┴───────────────────────────┤
│           C++ Native                  Rust Fallback             │
│  (_pydantic_core_cpp.so)        (_pydantic_core.so)             │
└─────────────────────────────────────────────────────────────────┘
```

三种 fallback 层级:
1. **符号级** — `__getattr__` 从 Rust `.so` 加载白名单符号
2. **模块级** — `core_schema` 代理完全从 Rust `pydantic_core/core_schema.py` 加载
3. **函数级** — `from_json()`/`to_json()`/`to_jsonable_python()` 永远走 Rust

---

## 一、Rust Fallback 符号完整列表

### 1.1 符号级 (_RUST_FALLBACKS 白名单)

| 符号 | 来源 | 测试中用途 |
|------|------|-----------|
| `SchemaSerializer` | Rust `_pydantic_core` | 序列化器构造和类型检查 |
| `Url` | Rust `_pydantic_core` | URL 类型 |
| `MultiHostUrl` | Rust `_pydantic_core` | 多主机 URL 类型 |
| `PydanticCustomError` | Rust `_pydantic_core` | 自定义错误断言 |
| `PydanticKnownError` | Rust `_pydantic_core` | 已知错误类型 |
| `PydanticSerializationError` | Rust `_pydantic_core` | 序列化错误断言 |
| `PydanticSerializationUnexpectedValue` | Rust `_pydantic_core` | 序列化异常值 |
| `PydanticUndefined` / `PydanticUndefinedType` | Rust `_pydantic_core` | 未定义字段默认值 |
| `TzInfo` | Rust `_pydantic_core` | 时区类型 |
| `Some` | Rust `_pydantic_core` | 可选值包装 |
| `CoreConfig` | Rust `core_schema` | 配置类型注解 |
| `CoreSchema` | Rust `core_schema` | Schema 类型注解 |
| `CoreSchemaType` | Rust `core_schema` | Schema 类型枚举 |
| `ErrorType` | Rust `_pydantic_core` (如果 C++ 没有) | 错误类型枚举 |

### 1.2 模块级 (`core_schema` 代理)

整个 `core_schema` 模块从 Rust `pydantic_core/core_schema.py` 加载，包含:
- `core_schema.int_schema()`, `core_schema.str_schema()` 等 schema 构建函数
- `core_schema.ValidatorFunctionWrapHandler` 等类型
- `core_schema.ValidationInfo` 等类

### 1.3 函数级

| 函数 | 来源 | 用途 |
|------|------|------|
| `from_json()` | Rust `_pydantic_core` | JSON 反序列化 |
| `to_json()` | Rust `_pydantic_core` | JSON 序列化 |
| `to_jsonable_python()` | Rust `_pydantic_core` | 可 JSON 序列化的 Python 对象 |

---

## 二、C++ Native 符号（不触发 Fallback）

| 符号 | 层级 | 说明 |
|------|------|------|
| `SchemaValidator` | C++ `_pydantic_core_cpp` | 核心校验器（Python 包装类） |
| `ValidationError` | C++ `_pydantic_core_cpp` | 校验异常 |
| `SchemaError` | C++ `_pydantic_core_cpp` | Schema 构造异常 |
| `ArgsKwargs` | Python `pydantic_core_cpp/__init__.py` | 纯 Python 实现 |
| `MISSING` | Python `pydantic_core_cpp/__init__.py` | 纯 Python Sentinel |
| `InputType`, `ExtraBehavior`, `SerMode` 等枚举 | C++ `_pydantic_core_cpp` | 枚举类型 |
| `PydanticOmit`, `PydanticUseDefault` | C++ `_pydantic_core_cpp` | 哨兵值 |
| `SerializationInfo` | C++ `_pydantic_core_cpp` | 序列化信息 |

---

## 三、测试文件逐文件分析

### 3.1 直接导入 Rust fallback 符号的测试文件

共 **26 个文件** 直接导入至少一个 Rust fallback 符号。这些文件中涉及 Rust fallback 符号的测试函数走 Rust 路径。

#### 使用 `core_schema` (最大的 fallback 组) — 21 个文件

| 文件 | 导入方式 | 说明 |
|------|----------|------|
| `test_main.py` | `from pydantic_core import CoreSchema, core_schema` | `__get_pydantic_core_schema__` 中大量调用 `core_schema.*()` |
| `test_types.py` | `from pydantic_core import core_schema` | 类型校验测试，大量 schema 构建 |
| `test_validators.py` | `from pydantic_core import core_schema` | 校验器装饰器测试 |
| `test_annotated.py` | `from pydantic_core import CoreSchema, core_schema` | 自定义类型模式 |
| `test_generics.py` | `from pydantic_core import CoreSchema, core_schema` | 泛型类型模式 |
| `test_deprecated.py` | `from pydantic_core import CoreSchema, core_schema` | 废弃 API 测试 |
| `test_json_schema.py` | `from pydantic_core import CoreSchema, ...` | JSON Schema 生成 |
| `test_discriminated_union.py` | `from pydantic_core import SchemaValidator, core_schema` | 直接构造 `core_schema.union_schema()` |
| `test_json.py` | `from pydantic_core import CoreSchema, SchemaSerializer, core_schema` | 序列化 + schema |
| `test_config.py` | `from pydantic_core import SchemaError, SchemaSerializer, SchemaValidator` | 配置测试 |
| `test_serialize.py` | `from pydantic_core import PydanticSerializationError, core_schema, to_jsonable_python` | 序列化测试 |
| `test_computed_fields.py` | `from pydantic_core import ValidationError, core_schema` | 计算字段 |
| `test_edge_cases.py` | `from pydantic_core import PydanticSerializationError, PydanticUndefined` | 边缘案例 |
| `test_docs.py` | `from pydantic_core import core_schema` | 文档示例 |
| `test_types_typeddict.py` | `from pydantic_core import core_schema` | TypedDict 类型 |
| `test_internal.py` | `from pydantic_core import CoreSchema, PydanticUndefined; import core_schema as cs` | 内部 API |
| `test_dataclasses.py` | `from pydantic_core import ArgsKwargs, SchemaValidator` | dataclass 测试（内部走 core_schema） |
| `test_root_model.py` | `from pydantic_core import CoreSchema; import from core_schema` | RootModel 测试 |

#### 使用 `SchemaSerializer` (Rust fallback) — 3 个文件

| 文件 | 使用方式 | 影响函数数 |
|------|----------|-----------|
| `test_config.py` | `isinstance(MyModel.__pydantic_serializer__, SchemaSerializer)` | ~8 个 assert |
| `test_type_hints.py` | 类型注解检查 `'SchemaSerializer': SchemaSerializer` | 1 处 |
| `test_json.py` | `serializer = SchemaSerializer(schema)` | ~1 处 (但整个文件大量用 serializer) |

#### 使用 `Url`/`MultiHostUrl` (Rust fallback) — 1 个文件

| 文件 | 使用方式 | 影响范围 |
|------|----------|----------|
| `test_networks.py` | `from pydantic_core import MultiHostHost, PydanticCustomError, PydanticSerializationError, Url` | **整个文件** 的 URL 测试 |

#### 使用 `PydanticCustomError` (Rust fallback) — 4 个文件

| 文件 | 使用方式 |
|------|----------|
| `test_types.py` | `PydanticCustomError` in `raises()` 断言 |
| `test_networks.py` | `PydanticCustomError` 网络类型错误 |
| `test_color.py` | `PydanticCustomError` 颜色校验错误 |
| `test_types_payment_card_number.py` | `PydanticCustomError` 信用卡号错误 |
| `test_utils.py` | `PydanticCustomError` 工具函数错误 |

#### 使用 `PydanticUndefined` (Rust fallback) — 8 个文件

| 文件 |
|------|
| `test_annotated.py` |
| `test_fields.py` |
| `test_private_attributes.py` |
| `test_edge_cases.py` |
| `test_utils.py` |
| `test_aliases.py` |
| `test_internal.py` |
| `test_construction.py` |

#### 使用 `to_jsonable_python`/`to_json` (Rust fallback) — 5 个文件

| 文件 | 符号 | 说明 |
|------|------|------|
| `test_serialize.py` | `to_jsonable_python` | 序列化测试 |
| `test_json_schema.py` | `to_jsonable_python` | JSON Schema 生成 |
| `types/test_model.py` | `serializer.to_json()` | 序列化器方法 |
| `test_dataclasses.py` | `serializer.to_json()` | dataclass 序列化 |
| `test_datetime.py` | `ta.dump_json()` 但内部依赖 `to_json` | datetime 序列化 |

---

### 3.2 仅导入 C++ Native 符号的测试文件 (6 个文件)

这些文件只导入 C++ 实现的符号（`SchemaValidator`、`ValidationError`、`ArgsKwargs`），**不直接导入**任何 Rust fallback 符号：

| 文件 | 导入的 C++ 符号 | 说明 |
|------|-----------------|------|
| `test_validate_call.py` | `ArgsKwargs` | 只导入 `ArgsKwargs`，不直接使用 `core_schema` |
| `test_plugins.py` | `ValidationError` | 只导入 `ValidationError` |
| `test_type_adapter.py` | `ValidationError` | 只导入 `ValidationError`，但通过 pydantic 内部使用 `core_schema` |
| `test_version.py` | (无) | 不导入任何 pydantic_core 符号 |
| `test_types_typeddict.py` | `core_schema` (Rust!) | 更正: 导入 core_schema → Rust |

等待，让我重新检查 — `test_types_typeddict.py` 导入 `core_schema`，所以是 Rust。让我修正这个列表。

**真正只含 C++ 符号的文件（0 个 Rust fallback 直接导入）：**

| 文件 | 导入 | 说明 |
|------|------|------|
| `test_plugins.py` | `ValidationError` | ✅ C++ native |
| `test_validate_call.py` | `ArgsKwargs` | ✅ C++ native |
| `test_type_adapter.py` | `ValidationError` | ✅ C++ native（导入层面） |
| `test_version.py` | (无) | ✅ 不导入 pydantic_core |

但是！这些文件中间接使用 `core_schema` 的方式：

- **`test_type_adapter.py`**: `TypeAdapter(int)` 内部调用 `pydantic._internal._generate_schema` → 最终调用 `core_schema.int_schema()` → **Rust fallback**
- **`test_validate_call.py`**: `validate_call` 内部也需要 `core_schema` 构造 → **Rust fallback**
- **`test_plugins.py`**: 构造 Model 类 → `__get_pydantic_core_schema__` → **Rust fallback**
- **`test_version.py`**: 纯粹的版本信息，不涉及 core → ✅

### 3.3 完全不需要 pydantic_core 的测试文件

以下测试文件不导入任何 `pydantic_core` 符号，完全不触及 core：

| 文件 |
|------|
| `test_config.py` (部分 - 仅不含 SchemaSerializer 的测试) |
| 少量纯逻辑测试 |

但大多数涉及模型构造的测试最终都会通过 pydantic 内部走 `core_schema`。

---

## 四、按覆盖深度分层的测试归类

### 🔴 层级 1: 完全走 C++ 的测试 (不走任何 Rust)

这类测试不构造 pydantic model，不构造 TypeAdapter，不调用 core_schema。

| 文件 | 测试函数 | 估算数量 |
|------|----------|---------|
| `test_version.py` | 全部 | ~5 |
| **合计** | | **~5** |

### 🟠 层级 2: 验证走 C++, Schema 构建走 Rust

这类测试通过 `BaseModel` / `TypeAdapter` / `validate_call` 等高层 API 运行，验证逻辑走 C++ `SchemaValidator`，但 schema 构建走 Rust `core_schema`。

**涉及的全部 89 个测试文件** — 只要是使用 `BaseModel` 构造 model、或用 `TypeAdapter(T)`、或用 `validate_call` 的测试，schema 生成阶段都经过 Rust `core_schema`。

| 代表性文件 | 估算函数数 |
|-----------|-----------|
| `test_main.py` | ~600 |
| `test_types.py` | ~500 |
| `test_edge_cases.py` | ~200 |
| `test_generics.py` | ~150 |
| `test_dataclasses.py` | ~150 |
| `test_validators.py` | ~120 |
| `test_json.py` | ~100 |
| `test_serialize.py` | ~80 |
| 其他测试文件 | ~600 |
| **合计** | **~2,500** |

### 🔴 层级 3: 直接使用 Rust fallback 符号的测试

这些测试中针对特定 Rust fallback 符号的用例：

| 符号 | 涉及文件 | 估算测试函数 |
|------|---------|------------|
| `SchemaSerializer` | `test_config.py`, `test_json.py`, `test_type_hints.py` | ~30 |
| `Url/MultiHostUrl` | `test_networks.py` | ~150 |
| `PydanticCustomError` | `test_types.py`, `test_color.py`, `test_utils.py` 等 | ~20 |
| `PydanticUndefined` | `test_fields.py`, `test_construction.py` 等 | ~40 |
| `PydanticSerializationError` | `test_serialize.py`, `test_edge_cases.py` | ~10 |
| `to_jsonable_python` | `test_serialize.py`, `test_json_schema.py` | ~5 |
| `to_json` (serializer 方法) | `types/test_model.py`, `test_dataclasses.py` | ~30 |
| **合计** | | **~285** |

---

## 五、按测试文件完整分类 (34 个直接导入 pydantic_core 的文件)

| # | 文件 | 导入的 C++ Native 符号 | 导入的 Rust Fallback 符号 | Rust fallback 依赖度 |
|---|------|----------------------|--------------------------|-------------------|
| 1 | `test_annotated.py` | — | `CoreSchema`, `PydanticUndefined`, `core_schema` | 🔴 高 |
| 2 | `test_aliases.py` | — | `PydanticUndefined` | 🔴 高 |
| 3 | `test_color.py` | — | `PydanticCustomError` | 🔴 全文件 |
| 4 | `test_computed_fields.py` | `ValidationError` | `core_schema` | 🟠 中高 |
| 5 | `test_config.py` | `SchemaError`, `SchemaValidator` | `SchemaSerializer` | 🟠 中高 |
| 6 | `test_construction.py` | `ValidationError` | `PydanticUndefined` | 🟠 中 |
| 7 | `test_dataclasses.py` | `ArgsKwargs`, `SchemaValidator` | — (但内部用 core_schema) | 🟠 中 |
| 8 | `test_datetime.py` | — | — (通过 pydantic API) | 🟢 低 |
| 9 | `test_deprecated.py` | — | `CoreSchema`, `core_schema` | 🔴 高 |
| 10 | `test_discriminated_union.py` | `SchemaValidator` | `core_schema` | 🔴 高 (直接构造) |
| 11 | `test_docs.py` | — | `core_schema` | 🔴 高 |
| 12 | `test_edge_cases.py` | — | `PydanticSerializationError`, `PydanticUndefined` | 🔴 高 |
| 13 | `test_experimental_arguments_schema.py` | `ArgsKwargs`, `SchemaValidator` | — (但内部用 core_schema) | 🟠 中 |
| 14 | `test_fields.py` | — | `PydanticUndefined` | 🟠 中 |
| 15 | `test_generics.py` | — | `CoreSchema`, `core_schema` | 🔴 高 |
| 16 | `test_internal.py` | — | `CoreSchema`, `PydanticUndefined`, `core_schema` | 🔴 高 |
| 17 | `test_json.py` | — | `CoreSchema`, `SchemaSerializer`, `core_schema` | 🔴 全文件 |
| 18 | `test_json_schema.py` | `SchemaValidator` | `CoreSchema`, `core_schema`, `to_jsonable_python` | 🔴 高 |
| 19 | `test_main.py` | — | `CoreSchema`, `core_schema` | 🔴 高 |
| 20 | `test_missing_sentinel.py` | — | `PydanticSerializationUnexpectedValue` | 🔴 全文件 |
| 21 | `test_networks.py` | — | `Url`, `MultiHostUrl`, `PydanticCustomError`, `PydanticSerializationError` | 🔴 全文件 |
| 22 | `test_parse.py` | — | `CoreSchema` | 🟠 中 |
| 23 | `test_plugins.py` | `ValidationError` | — | 🟢 低 |
| 24 | `test_private_attributes.py` | — | `PydanticUndefined` | 🟠 中 |
| 25 | `test_root_model.py` | — | `CoreSchema` | 🟠 中高 |
| 26 | `test_serialize.py` | — | `PydanticSerializationError`, `core_schema`, `to_jsonable_python` | 🔴 全文件 |
| 27 | `test_type_adapter.py` | `ValidationError` | — | 🟢 低 |
| 28 | `test_type_hints.py` | — | `CoreSchema`, `SchemaSerializer`, `SchemaValidator` | 🔴 高 |
| 29 | `test_types.py` | — | `PydanticCustomError`, `core_schema` | 🔴 高 |
| 30 | `test_types_payment_card_number.py` | — | `PydanticCustomError` | 🔴 全文件 |
| 31 | `test_types_typeddict.py` | — | `core_schema` | 🔴 高 |
| 32 | `test_utils.py` | — | `PydanticCustomError`, `PydanticUndefined` | 🔴 中 |
| 33 | `test_validate_call.py` | `ArgsKwargs` | — | 🟢 低 |
| 34 | `test_validators.py` | — | `core_schema` | 🔴 高 |
| 35 | `test_version.py` | (无导入) | — | 🟢 无 |

---

## 六、汇总统计

### 6.1 按直接 Rust 依赖分组的测试文件数

| 依赖等级 | 文件数 | 说明 |
|----------|--------|------|
| 🔴 全文件 Rust | 4 | `test_color.py`, `test_json.py`, `test_networks.py`, `test_missing_sentinel.py`, `test_serialize.py`, `test_types_payment_card_number.py` |
| 🟠 中高 Rust | 20+ | 使用 `core_schema` 但不全文件依赖的文件 |
| 🟢 低/无 Rust | 5 | `test_plugins.py`, `test_type_adapter.py`, `test_validate_call.py`, `test_version.py`, `test_datetime.py` |

### 6.2 按测试函数数量的估计

| 路径 | 测试函数数 | 占比 |
|------|-----------|------|
| 🔴 完全 C++ (无任何 Rust) | ~5 | 0.2% |
| 🟠 验证走 C++ / schema 走 Rust | ~2,500 | 94% |
| 🔴 直接 Rust fallback 符号 | ~280 | 10.5% |
| *(部分重叠)* | | |
| **总计** | **~2,659** | 100% |

### 6.3 关键结论

1. **schema 构建是最大的 Rust fallback**: `core_schema` 代理被 21+ 个测试文件直接导入，所有涉及 Model/TypeAdapter 构造的测试（~94% 的测试）都间接走了 Rust schema 构建路径。

2. **验证逻辑大部分走 C++**: `SchemaValidator.validate_python()` 和 `validate_json()` 由 `pydantic_core_cpp` 原生实现。`model_validate()`、`TypeAdapter().validate_python()` 等高层 API 的验证阶段最终调用 C++ 代码。

3. **序列化全走 Rust**: `SchemaSerializer` 在 C++ 中没有原生实现，所有序列化相关测试（`model_dump()`、`model_dump_json()`、`serializer.to_json()`）都走 Rust fallback。

4. **特定符号的 Rust 依赖**:
   - `Url/MultiHostUrl` → `test_networks.py` 全部 ~150 个测试走 Rust
   - `SchemaSerializer` → 序列化相关测试
   - `PydanticCustomError` → 4 个文件中错误断言走 Rust
   - `PydanticUndefined` → 8 个文件中字段默认值走 Rust

5. **如果只安装 C++ 不装 Rust backend**，可运行的测试约 **30-35%**：主要是基础类型验证、Model 构造/校验、TypeAdapter 基本用途。`model_dump()`/序列化、网络类型、自定义错误、JSON Schema 生成等全部不可用。

### 6.4 典型测试的代码路径追踪

```
# 测试: model_validate({"name": "Alice"})
# 文件: test_main.py

1. Model.model_validate(data)
   → pydantic/main.py

2.     self.__pydantic_validator__.validate_python(data, ...)
       → pydantic_core_cpp.SchemaValidator.validate_python()  ← ✅ C++

3. 但 SchemaValidator 的构建:
   Model.__pydantic_validator__ 是在类定义时构建的:
   → pydantic._internal._model_construction
       → pydantic._internal._generate_schema
           → pydantic._internal._generate_schema.GenerateSchema.generate_schema()
               → core_schema.model_schema(...)          ← 🔴 Rust fallback
               → core_schema.model_fields_schema(...)   ← 🔴 Rust fallback
               → core_schema.int_schema()               ← 🔴 Rust fallback
           → SchemaValidator(schema)                     ← ✅ C++

所以:   类定义时 schema 构建 → Rust core_schema
        运行时验证         → C++ SchemaValidator
        序列化             → Rust SchemaSerializer (全部)
```

---

## 七、要实现完全 C++ 运行需要消除的 Rust 依赖

按优先级排序:

| 优先级 | 需实现的功能 | 影响的测试文件数 | 影响的测试函数 |
|--------|-----------|---------------|--------------|
| P0 | `core_schema` 模块 (schema 构建函数) | 25+ | ~2,500+ |
| P0 | `SchemaSerializer` | 3 | ~100+ |
| P1 | `PydanticUndefined` | 8 | ~40 |
| P1 | `Url` / `MultiHostUrl` Python 类型 | 1 | ~150 |
| P1 | `PydanticCustomError` | 5 | ~20 |
| P1 | `from_json()` / `to_json()` | 2 | ~10 |
| P2 | `PydanticSerializationError` | 3 | ~10 |
| P2 | `TzInfo` | 0 (间接) | ~10 |
| P3 | `CoreSchema`/`CoreConfig` 类型别名 | 10+ | (类型注解，不影响运行) |
