# pydantic-core-cpp Reliability Improvement Plan

**Memo**: lessons from the `test_docs_examples` segfault investigation (commit `ac1c2810e`)
**Date**: 2026-08
**Scope**: C++ port (`pydantic-core-cpp/`), which is the default runtime backend behind the shim in `pydantic/_pydantic_core_shim.py`

---

## 1. Context — one crash, three latent bugs

`tests/test_docs.py::test_docs_examples` segfaulted on a docs example
(`TypeAdapter(dict).dump_json(cyclic)`). The native backtrace pointed at
`FunctionAfterValidator::validate` calling a Python callable with a corrupted
argument. Investigation surfaced three independent standing bugs:

| # | Bug | Root pattern |
|---|-----|--------------|
| 1 | Unbounded recursion in `infer_json` / `serialize_any_value` on cyclic data | Missing recursion guard (Rust has `RecursionGuard`) |
| 2 | `ChainValidator` returned the *first* successful step instead of threading outputs; no `effective_result_name()` delegation → results converted to `None`/garbage | Semantic drift from the 2-line Rust fold in `chain.rs` |
| 3 | `is-instance` builder only accepted exact `py::type` for `cls`; ABCMeta classes and typing aliases (`typing.Sequence`) were **silently dropped** → validator became accept-all returning an **int payload under the `"py_raw_object"` name tag** → downstream reinterpreted as `PyObject*` → SIGSEGV | Silent fallback + broken payload/tag invariant |

All three were pre-existing and invisible until a broad suite exercised them.

---

## 2. Guiding principles

1. **Fail at build time, not at validate time.** A schema the port cannot fully
   honor must raise `SchemaError` during construction.
2. **Payload and type-tag are one invariant.** If they can disagree, they will.
   Make mismatch unrepresentable (or at least loud in debug builds).
3. **Port Rust's own unit tests**, not just pydantic's end-to-end suites —
   Rust's validators encode contracts that e2e tests rarely touch.
4. **Error strings are API.** Docs and tests assert them byte-for-byte
   (`"Circular reference detected (id repeated)"`).
5. **In a dual-backend repo, assert which backend ran** before analyzing any
   behavioral anomaly.

---

## 3. Workstreams

### WS-A — Result pipeline type safety  *(P0/P1, root-cause hardening)*

The result pipeline is `std::shared_ptr<void>` tagged by validator
`name()` / `effective_result_name()`. Consumers dispatch on the string and
`static_cast`. This is a hand-rolled variant with zero compiler enforcement;
bug #3 was exactly a tag/payload divergence.

- [ ] **A1 (quick win)** Add a debug-mode consistency checker: when compiled
      with `-DPYDANTIC_CORE_DEBUG` (or env `PYDANTIC_CORE_DEBUG=1`), every
      result crossing the binding layer is verified against its declared
      `effective_result_name()` (e.g. `"py_raw_object"` payload must be a
      plausible `PyObject*`, `"list"`/`"dict"`/`"py_object"` payloads must be
      `py::object`/`py::list`/`py::dict`, etc.). Mismatch → abort with both the
      tag and the producing validator's name.
- [ ] **A2** Introduce a single construction helper, e.g.
      `make_result<T>(value)` returning a small struct `{ shared_ptr<void> data; const char* tag; }`
      so payload+tag are written together. Migrate validators incrementally,
      starting with the ones whose payloads diverge from their class name:
      `IsInstanceValidator`/`IsSubclassValidator` (`"py_raw_object"`),
      function-* (`"function-wrap"` etc. holding `py::object`),
      model-fields (`ValidatedModelFieldsOutput`), union (`last_type_name_`).
- [ ] **A3** Audit every `return ValResult<...>(std::make_shared<int>(1))` /
      other non-conforming payloads left in the tree; replace with honest
      passthroughs (as done for the is-instance fallback in `basic.hpp`).

**Acceptance**: debug runs over validators/main/dataclasses/docs complete with
zero tag-mismatch aborts; no `static_cast` without a matching helper.

### WS-B — Fail-fast builders  *(P0, cheap and prevents whole bug classes)*

Bug #3's deeper cause: builders silently degrade schemas they cannot honor.

- [ ] **B1** Grep-audit all `build_from_py_dict` / `build_from_element` arms for
      silent-drop patterns: `if (schema.contains(k)) { ... }` with no else, bare
      `catch (...) {}` around required keys, optional-treatment of required
      schema keys. Each becomes either an honored path or a `SchemaError`.
- [ ] **B2** Specifically verify these known-tricky extractions reject loudly or
      handle fully: `cls` (is-instance/is-subclass — fixed in `ac1c2810e`,
      keep regression tests), discriminator callables, `custom_error`,
      `serialization` sub-schemas, `definitions` refs.
- [ ] **B3** Rule: **no empty catch blocks in builders.** Where third-party
      Python exceptions are expected, narrow to `py::error_already_set` and
      re-raise as `SchemaError` with context.

**Acceptance**: malformed/half-supported schemas fail validation-suite startup
with actionable messages instead of misbehaving at runtime. Add tests feeding
deliberately truncated schemas.

### WS-C — Recursion & resource guards, full coverage  *(P1)*

Fixed for inferred JSON/python serialization (`infer_json`, `serialize_any_value`)
in commit `ac1c2810e`. Remaining gaps:

- [ ] **C1** Schema-driven serialization recursion: self-referential *model*
      instances serialized via `SerNode::to_json` / `serialize_fields_json` /
      `to_python` model branches bypass `infer_json`'s guard. Thread the same
      thread-local ancestor stack through those entry points (Rust guards model
      serializers too — see `serializers/type_serializers/model.rs`).
- [ ] **C2** Validation-side depth guard for deeply nested inputs against
      recursive schemas (Rust has depth limits in `RecursionState`; the C++
      port currently relies on nothing). Convert "segfault" into
      `"Circular reference detected (depth exceeded)"`.
- [ ] **C3** Property-style tests: cyclic dict/list/set/tuple/model graphs
      through every public entrypoint (`to_json`, `to_python`,
      `dump_json`, `dump_python`, `validate_python`); assert either success or
      the exact Rust error message — never a crash.

### WS-D — Rust parity unit tests  *(P1, prevents drift)*

- [ ] **D1** Port focused unit tests from Rust sources into a new
      `pydantic-core-cpp/tests/test_core_parity.py` (raw `_pydantic_core_cpp`
      module level, bypassing the Python wrapper):
      - chain folding semantics (`validators/chain.rs`): each step sees the
        previous output; result = last step; nested chains flatten.
      - isinstance extraction: builtin types, ABCMeta classes
        (`collections.abc.Sequence`), `typing.Sequence`, str-only classes,
        classes with custom metaclass `__instancecheck__`.
      - definitions/refs incl. flat function definitions (regression for
        `970ee2da0`).
      - literal enum-member identity, lax-list iterables — existing fixes get
        pinned at raw-core level too.
- [ ] **D2** Error-message parity table test: for a list of inputs, compare CPP
      exception type + message against recorded RUST outputs (run once with
      `PYDANTIC_USE_CPP_CORE=0` to regenerate expectations).
- [ ] **D3** Wire `test_core_parity.py` into the standard sweep command used
      per-fix (see §5).

### WS-E — Debugging infrastructure & harness hygiene  *(P0 quick wins)*

- [ ] **E1** Backend pinning helper for tests/scripts:
      `assert_backend('cpp'|'rust')` checking `sys.modules['pydantic_core'].__name__`
      / presence of `_pydantic_core_cpp`, plus a note in AGENT.md documenting
      the aliasing trap: plain `import pydantic_core` may be Rust even when cpp
      is installed; `from pydantic_core import _pydantic_core_cpp` resolves via
      whatever the shim registered. All repro scripts must assert first.
- [ ] **E2** Pre-commit gate: `grep -rn "std::cerr" pydantic-core-cpp/src pydantic-core-cpp/include`
      must be empty (add to the manual ritual checklist in AGENT.md; optionally
      a git hook). Session `ac1c2810e` needed a cleanup pass for `[ASG]`/`[FE]`/
      `[JOP]` leftovers from prior work.
- [ ] **E3** Debugging playbook section in AGENT.md:
      - Suspected memory corruption → lldb native bt immediately
        (`lldb -b -o run -o "bt 30" python -- repro.py`); faulthandler stacks lie
        about the crime scene.
      - Bisect the *schema shape*, not code paths: strip one wrapper per probe
        down to a trivially readable case (`chain[list]` took 5 probes).
      - Instrument conversions (`value_to_python_with_type` call sites) before
        instrumenting validators.
      - Run repro scripts with `python -u` — stdout buffering hides prints on crash.

### WS-F — Process: broaden detection earlier  *(ongoing)*

All three bugs predated the session and were caught only when a new feature
widened the exercised surface.

- [ ] **F1** Per-fix verification battery stays mandatory (already practiced):
      targeted file + `test_validators.py` + `test_main.py` + dataclasses
      failure-set diff + Rust-mode green + 5-file sweep count comparison.
- [ ] **F2** Add `test_docs.py::test_docs_examples` to the periodic sweep now
      that it completes (354✓/130F baseline) — track the failure count; it is a
      wide net for semantic drift across many real snippets.
- [ ] **F3** Stash A/B attribution stays the rule for any new failure: confirm
      pre-existing vs regression before investigating.

---

## 4. Priority order & rough effort

| Item | Priority | Effort | Payoff |
|------|----------|--------|--------|
| E2 debug-print gate | P0 | minutes | hygiene |
| E1 backend-pinning helper | P0 | ~1h | kills false repros |
| A3 honest-payload audit | P0 | ~half day | removes remaining UB class |
| B1/B2 builder fail-fast audit | P0 | ~1 day | prevents whole bug family |
| A1 debug tag checker | P0/P1 | ~1 day | makes A-class bugs loud |
| D1 core-parity unit tests | P1 | ~1–2 days | pins semantics permanently |
| C1/C2 recursion coverage | P1 | ~1–2 days | closes remaining crash paths |
| C3 cycle property tests | P1 | ~half day | regression net |
| A2 tagged-result refactor | P2 | multi-day, incremental | structural fix |
| D2 message parity table | P2 | ~half day | API parity |
| F2 docs sweep tracking | ongoing | none | early warning |

## 5. Standard verification commands

```bash
# build (expect ERRS=0 before copying)
cd pydantic-core-cpp/build && ninja _pydantic_core_cpp.cpython-310-darwin.so
cp _pydantic_core_cpp.cpython-310-darwin.so ../pydantic_core_cpp/
cp _pydantic_core_cpp.cpython-310-darwin.so \
   ../../.venv/lib/python3.10/site-packages/pydantic_core_cpp/

# per-fix battery
python -m pytest tests/test_validators.py tests/test_main.py -q --tb=no
PYDANTIC_USE_CPP_CORE=0 python -m pytest tests/test_validators.py tests/test_main.py -q --tb=no
python -m pytest tests/test_dataclasses.py -q --tb=no   # diff failure-set vs parent
python -m pytest tests/test_types.py tests/test_edge_cases.py \
  tests/test_serialize.py tests/test_json_schema.py tests/test_internal.py -q --tb=no
python -m pytest tests/test_docs.py::test_docs_examples -q --tb=line   # must not crash
```

---

## Appendix: what each fix looked like (for future reference)

- Cycle guard: thread-local `std::vector<const void*>` ancestor stack inside
  `infer_json` / separate one for `serialize_any_value`; raise
  `PydanticSerializationError("Error serializing to JSON: ValueError: "
  "Circular reference detected (id repeated)")`; pop via RAII.
- Chain: fold with per-step conversion
  `current = value_to_python_with_type(carried, v->effective_result_name())`;
  `effective_result_name()` delegates to `validators_[last_used_]`.
- is-instance/subclass: accept `py::isinstance<py::type>(cls) || hasattr(cls, "__mro__")`;
  no-info fallback returns input as genuine `py_raw_object` payload.
