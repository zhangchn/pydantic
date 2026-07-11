# 在 C++ 中实现 CoreConfig 和 CoreSchema 的分析

> 分析日期: 2026-07-10
> 目标: 消除两个 Rust fallback 符号 `CoreConfig` 和 `CoreSchema`

---

## 1. 它们到底是什么？

### 1.1 CoreConfig

**Python 层** — 一个 `TypedDict`，定义了约 30 个可选配置字段:

```python
class CoreConfig(TypedDict, total=False):
    title: str
    strict: bool
    extra_fields_behavior: Literal['allow', 'forbid', 'ignore']
    from_attributes: bool
    validate_default: bool
    str_max_length: int
    str_min_length: int
    str_strip_whitespace: bool
    str_to_lower: bool
    str_to_upper: bool
    allow_inf_nan: bool
    ser_json_timedelta: Literal['iso8601', 'float']
    ser_json_temporal: Literal['iso8601', 'seconds', 'milliseconds']
    # ... 还有 ~15 个
```

**Rust 层** — **不存在对应的 `CoreConfig` struct**。Rust 的 `SchemaValidator::py_new()` 直接从 `config: Option<&PyDict>` 中一个一个地提取字段：

```rust
// Rust: validators/mod.rs
pub fn py_new(schema, config, _use_prebuilt) {
    let hide_input_in_errors = config.get_as("hide_input_in_errors")?.unwrap_or(false);
    let validation_error_cause = config.get_as("validation_error_cause")?.unwrap_or(false);
    let cache_str = config.get_as("cache_strings")?.unwrap_or(StringCacheMode::All);
    // 没有 CoreConfig struct — 直接提取标量字段 + 把整个 config dict 传给 build_validator()
    // 每个 validator 的 build() 方法再从 config dict 里读自己关心的配置
}
```

**关键**：`CoreConfig` 只是一个 **Python 类型注解**，用于 IDE 类型提示和运行时检查。Rust 没有对应的 struct。

### 1.2 CoreSchema

**Python 层** — 一个递归的 TypedDict 联合类型：

```python
CoreSchema: TypeAlias = AnySchema | NoneSchema | BoolSchema | IntSchema | ...
# 总共约 40+ 个 TypedDict，每个对应一个 validator 类型
```

**Rust 层** — **同样不存在 `CoreSchema` struct**。Rust 的 `build_validator()` 函数接收 schema dict，读取 `type` 字段，然后分派到对应的 `Validator::build()`：

```rust
fn build_validator_inner(schema, config, definitions) {
    let type_ = dict.get_as_req("type")?;
    match type_ {
        "int"  => IntValidator::build(dict, config, definitions),
        "str"  => StrValidator::build(dict, config, definitions),
        "bool" => BoolValidator::build(dict, config, definitions),
        // ...
    }
}
```

**关键**：`CoreSchema` 也只是一个 **Python 类型别名**。它没有 Rust 对应物。

---

## 2. 当前的 C++ 实现对比

### 2.1 Config 处理

| 维度 | Rust | C++ |
|------|------|-----|
| Config 来源 | Python dict 传给 `SchemaValidator.__init__()` | JSON string 传给 `SchemaValidator::SchemaValidator()` |
| Config 存储 | 只在构造函数时提取字段，不持久化整个 dict | 持久化为 `config_json_` string + `config_` struct |
| 配置字段 | 从 `PyDict` 直接读取 `hide_input_in_errors`, `validation_error_cause`, `cache_strings` + 每个 validator 自行读取 | `ValidationState::Config` 只定义了 6 个字段: `strict`, `extra_behavior`, `from_attributes`, `cache_strings`, `by_alias`, `by_name` |
| 字段分发 | config dict 作为参数传递给 `build_validator()` -> 每个 validator 的 `build()` | 解析为 `unordered_map<string, string>` 后传递给 `build_from_element()` |

**核心差距**: C++ 的 `ValidationState::Config` 只覆盖了约 **20%** 的 CoreConfig 字段。其余字段（如 `str_max_length`, `allow_inf_nan`, `validate_default` 等）根本没有被读取和使用。

### 2.2 Schema 处理

| 维度 | Rust | C++ |
|------|------|-----|
| Schema 来源 | Python dict | JSON string |
| 类型分派 | `match type_ { "int" => IntValidator::build(...), ... }` | `if-else` 链 `if (type == "int") { ... }` |
| Validator 构建 | 每个 validator 有 `build()` 静态方法，接收 `&PyDict` | `build_from_element()` 接收 `simdjson::dom::element` |
| 递归 schema | `PyDict` 直接递归 | 递归解析 `simdjson::dom::element` |

---

## 3. 技术方案分析

### 3.1 方案 A: 纯 Python TypedDict（推荐, 最小工程量）

`CoreConfig` 和 `CoreSchema` 的类型定义存在于 `pydantic_core/core_schema.py`（Rust 端）。C++ 端可以通过在 `pydantic_core_cpp/` 下**直接提供同名的 Python 文件**来消除对 Rust 的导入依赖。

**做法**：

在 `pydantic-core-cpp/pydantic_core_cpp/` 下创建 `core_schema.py`，定义 `CoreConfig`、`CoreSchema` 及所有 schema 类型 TypedDict：

```python
# pydantic_core_cpp/core_schema.py
from __future__ import annotations
from typing import Any, Literal, TypedDict, TypeAlias

class CoreConfig(TypedDict, total=False):
    title: str
    strict: bool
    extra_fields_behavior: Literal['allow', 'forbid', 'ignore']
    # ... 全部 ~30 个字段

class IntSchema(TypedDict, total=False):
    type: Required[Literal['int']]
    strict: bool
    multiple_of: int
    # ...

CoreSchema: TypeAlias = IntSchema | StrSchema | BoolSchema | ...
```

然后修改 `__init__.py`：

```python
# 从 _RUST_FALLBACKS 中移除 CoreConfig, CoreSchema, CoreSchemaType
# 从 _CORE_SCHEMA_FALLBACKS 中移除
# 直接导入本地的 core_schema 模块
from . import core_schema
```

**优点**：
- 零 C++ 代码改动
- 完全与 Rust 版 API 兼容
- `from pydantic_core import CoreConfig` 不再触发 Rust fallback
- 可以作为 `core_schema` 模块代理的目标，消除整个 `core_schema` 的 Rust 依赖

**缺点**：
- 需要维护与 Rust 端相同的类型定义（但这是纯定义，几乎不变）
- `core_schema` 的 schema 构建函数（`int_schema()`, `str_schema()` 等）需要一起提供，否则还是走 Rust

### 3.2 方案 B: C++ 原生 struct + pybind11 绑定

在 C++ 中定义 `CoreConfig` struct，用 pybind11 暴露为 Python 类。

```cpp
// 在适当位置定义
namespace pydantic_core {

class CoreConfig {
public:
    std::optional<bool> strict;
    std::optional<std::string> extra_fields_behavior;
    std::optional<bool> from_attributes;
    // ...
};

}
```

然后在 pybind11 绑定中注册：

```cpp
py::class_<CoreConfig>(m, "CoreConfig")
    .def(py::init<>())
    .def_readwrite("strict", &CoreConfig::strict)
    .def_readwrite("from_attributes", &CoreConfig::from_attributes)
    // ...
```

**优点**：
- 真正的 C++ 实现，性能最优
- 类型安全

**缺点**：
- 工程量巨大，`CoreConfig` 有 **~30 个字段**，每种都有复杂的可选/联合类型
- `CoreSchema` 是递归 TypedDict 联合（**40+ 个变体**），在 C++ 中表示为 `std::variant` 或虚基类，复杂度极高
- Rust 本身都没有用 struct，说明这没有必要

### 3.3 方案 C: 混合方案 — Python 定义 + C++ config 处理

保留 `CoreConfig`/`CoreSchema` 的 Python TypedDict（方案 A），但**同时完善 C++ 端的 config 处理逻辑**，让 config 字段实际发挥作用。

**具体步骤**：

1. 在 `pydantic_core_cpp/` 下创建 `core_schema.py`（同方案 A）
2. 扩展 `ValidationState::Config` 以包含所有 CoreConfig 字段
3. 在 `combined_validator.cpp` 的 `build()` 中解析 config 字段并传递给各 validator
4. 各 validator 的 `build()` 方法读取 config 中的相关字段

**优点**：
- Python 导入层面消除 Rust 依赖
- C++ 运行时真正使用 config 字段
- 增量实现 — 可以先实现最常用的字段

**缺点**：
- 需要同时在 Python 和 C++ 两处改动

### 3.4 推荐方案: A + 渐进式 C++ C

**第一阶段（立即）**:
- 创建 `pydantic_core_cpp/core_schema.py`，定义 `CoreConfig` 和 `CoreSchema` 的 TypedDict
- 从 `_RUST_FALLBACKS` 中移除这两个符号
- 修改 `core_schema` 代理指向本地模块而不是 Rust

**第二阶段（逐步）**:
- 按需扩展 `ValidationState::Config` 的字段
- 在 validator `build()` 中读取新字段并应用到验证逻辑
- 最终所有 config 字段都由 C++ 处理

---

## 4. 具体实现细节

### 4.1 `core_schema.py` 文件结构

本地 `core_schema.py` 可以分为两部分：

**Part A: 类型定义**（供 Python 端类型检查用，约 400 行）

```python
# 只需要 TypedDict 和 TypeAlias — 纯静态类型，无运行时开销
class CoreConfig(TypedDict, total=False):
    ...

class AnySchema(TypedDict, total=False):
    ...

CoreSchema: TypeAlias = AnySchema | NoneSchema | ...
```

**Part B: Schema 构建函数**（可选，消除 `core_schema.int_schema()` 的 Rust 依赖）

```python
def int_schema(*, strict=None, multiple_of=None, ...):
    d = {'type': 'int'}
    if strict is not None: d['strict'] = strict
    ...
    return d
```

如果 Part B 也实现，则可以**消除整个 `core_schema` 模块的 Rust fallback**，这是当前最大的 fallback（影响 25+ 测试文件、~2500 个测试）。

### 4.2 `__init__.py` 修改

```python
# 替换这行:
# core_schema = _CoreSchemaModuleProxy('pydantic_core_cpp.core_schema')
# 为:
from . import core_schema

# 从 _RUST_FALLBACKS 中移除:
# 'CoreConfig', 'CoreSchema', 'CoreSchemaType',
# 从 _CORE_SCHEMA_FALLBACKS 中移除
```

### 4.3 C++ 端 Config 扩展（第二阶段）

`ValidationState::Config` 需要从当前的 6 个字段扩展到核心字段：

```cpp
struct Config {
    // 当前已有的
    std::optional<bool> strict;
    std::optional<ExtraBehavior> extra_behavior;
    std::optional<bool> from_attributes;
    StringCacheMode cache_strings = StringCacheMode::All;
    std::optional<bool> by_alias;
    std::optional<bool> by_name;

    // 需要新增的（核心验证字段）
    bool validate_default = false;
    std::optional<int> str_max_length;
    std::optional<int> str_min_length;
    bool str_strip_whitespace = false;
    bool str_to_lower = false;
    bool str_to_upper = false;
    std::optional<bool> allow_inf_nan;
    bool hide_input_in_errors = false;
    bool validation_error_cause = false;
    bool coerce_numbers_to_str = false;
    std::string regex_engine = "rust-regex";

    // 序列化相关（给 Serializer 用）
    // serialization config can remain in serialization_state.hpp
};
```

在 `SchemaBuilder::build()` 中解析这些字段：

```cpp
// 从 config_json 解析
auto config_it = config.find("str_max_length");
if (config_it != config.end()) {
    config_.str_max_length = std::stoi(config_it->second);
}
```

---

## 5. 影响评估

### 消除的 Rust fallback

| 符号 | 当前状态 | 实现后 | 影响 |
|------|---------|--------|------|
| `CoreConfig` | Rust fallback | Python TypedDict | 消除 1 个符号 |
| `CoreSchema` | Rust fallback | Python TypeAlias | 消除 1 个符号 |
| `CoreSchemaType` | Rust fallback | Python TypeAlias | 消除 1 个符号 |
| `core_schema` 模块 | Rust 代理 | 本地 Python | 消除最大 fallback |

### 对 pydantic 测试的影响

| 场景 | 当前行为 | 实现后 |
|------|---------|--------|
| `from pydantic_core import CoreConfig` | → Rust `_pydantic_core.so` | → 本地 Python `core_schema.py` |
| `core_schema.int_schema(...)` | → Rust `core_schema.py` | → 本地 `core_schema.py`（如果 Part B 实现） |
| 运行时 config 使用 | 仅 6 个字段生效 | 全部 ~30 个字段生效（如果 Phase 2 完整实现） |

### 风险

1. **API 一致性**：本地的 `core_schema.py` 需要与 Rust 版本保持 API 兼容。Rust 版本随 `pydantic-core` 更新，需要同步维护。
2. **Schema 构建函数的 Python 实现在性能上不如 Rust**：但这只影响 schema 构建（模型类定义时），不影响运行时验证性能。
3. **如果只实现 Part A 不实现 Part B**，`core_schema.xxx_schema()` 函数仍然需要走 Rust，这可以通过两步走解决：先消除类型符号的 fallback，再逐步实现构建函数。

---

## 6. 总结与建议

### 建议实施顺序

```
Week 1:  创建 pydantic_core_cpp/core_schema.py (Part A: 类型定义)
          从 _RUST_FALLBACKS 移除 CoreConfig/CoreSchema/CoreSchemaType
          修改 core_schema 代理 → 本地模块
          测试: import 不再触发 Rust

Week 2:  实现 Part B: schema 构建函数 (~40 个函数)
          替换 _CoreSchemaModuleProxy 为直接导入
          测试: core_schema.int_schema() 不再触发 Rust

Week 3+: 扩展 ValidationState::Config 字段
          在 SchemaBuilder::build() 中解析更多 config 字段
          各 validator 读取和使用 config 字段
```

### 工作量估计

| 组件 | 预估代码行 | 难度 |
|------|-----------|------|
| `core_schema.py` Part A (TypedDict) | ~500 行 | 低（搬运） |
| `core_schema.py` Part B (构建函数) | ~400 行 | 低（搬运 + 改逻辑） |
| `__init__.py` 修改 | ~10 行 | 低 |
| `ValidationState::Config` 扩展 | ~30 行 | 低 |
| `SchemaBuilder::build()` config 解析 | ~60 行 | 中 |
| 各 validator 使用 config 字段 | ~100 行 | 中高（需要理解每个 validator） |

**总量：约 1100 行代码**，其中大部分是 Python 类型定义的搬运。
