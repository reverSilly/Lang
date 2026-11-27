# RFC-001: Standard Library RAII (`mem`) & POD Gate

- **Status**: Accepted (Implementation split into PR 2A: `lib/std/mem.lang`, PR 2B: `lib/std/syntax/raii.lang`)
- **Tracking Issue**: [#3](https://github.com/reverSilly/Lang/issues/3)
- **Prerequisite**: PR #10 (Stageable Tier 1 `RuntimeIntrinsicKind` including `field_map.*`, `shapes.intern_shape`, `symbols.*`, `bytes_*`)

## 1. Motivation & Design Philosophy
Lang Core (`CORE_SPEC.md`) is strictly RAII-agnostic and POD-agnostic: Core `#deref`, `#write`, `#drop`, `#block`, `#break`, `#resume` operate purely on raw bytes and the two-state slot machine (`Uninitialized` / `Live` / deleted).

To provide safe resource management (e.g., `libc.malloc`/`free`, file descriptors, custom allocators) without bloating Core or introducing C++-style moved-from zombie states, we adopt:
1. **Rust-style Destructive Move**: `move(.a)` transfers bytes and immediately Core-`#drop`s the source slot and binding (no moved-from empty shell).
2. **Closed-Object Invariant (Zero Internal Holes)**: `destroy(ptr)` runs destructors in place without `#drop`, allowing safe overwrite of mutable fields inside closed objects (`.p.f = new_val`) and zero-RAII `swap`.
3. **Zero-Capability Stageable Stdlib**: All RAII primitives live in pure stageable `lib/std/mem.lang` using Tier 1 `IntrinsicProto`s, running identically in isolated `backends.interpreter.run` and `lang_stub` executables.

## 2. Lifecycle Protocol (Standard Layer 1 Functions)
Objects opt into custom lifecycle hooks via optional fields holding regular Layer 1 function closures (`{ __env, __code }`), enabling environment capture (e.g., capturing `libc.free` or a custom allocator):
- **`__destroy := self -> ...`**: Receives `ptr: Pointer` to the object; releases owned resources without calling `#drop`.
- **`__clone := self -> ...`**: Receives `ptr: Pointer` to the object; returns a newly owned value (materialized into an unbound slot by the caller).

### Key Invariants
- **Do NOT skip `__destroy` / `__clone` during recursion**:
  - `destroy(ptr)` invokes `ptr.__destroy(ptr)` first (while `__destroy.__env` is still live), then recursively calls `destroy` on **all** `sym != 0 && state == Live` child fields in reverse `entries` order — including `__destroy` and `__clone` themselves so any non-POD captures inside their `__env` are properly destroyed. Since function closures `{ __env, __code }` carry no `__destroy` field of their own, recursion terminates cleanly.
  - `is_pod(s_ptr)` probes `#field(z_ptr, __destroy).shape_ptr != 0` using a zero-base `z_ptr` (`shape_ptr != 0` avoids false negatives when `__destroy` is at offset 0). If absent, it recursively checks all `Live` child fields (including `__clone.__env`), returning `true` on leaf shapes (`count == 0`).
- **No `is_pod` pre-pass inside `destroy`**: `destroy(ptr)` returns immediately on leaf shapes (`count == 0`); for non-leaf POD structs, it performs no destructor calls and returns in a single O(N) pass.
- **Depth-agnostic active-child skipping (`#parent`)**: `cleanup_unbound(frame_ptr)` and `cleanup_all(frame_ptr)` skip any entry where `#parent(entry_ptr).shape_ptr != 0` (active child open frame). Because Core `#parent` returns null for all value slots including 0-byte `Unit` structs, this accurately skips arbitrarily nested call/block frames with zero `skip_addr` parameter.

## 3. PR 2A: Pure Library Layer (`lib/std/mem.lang`)
Staged at compile time and injected as top-level `mem` by `lib/std/run.lang` and `lib/std/build.lang`:
- `recast(ptr, s_ptr) -> Pointer`
- `is_pod(s_ptr) -> Boolean`
- `destroy(ptr) -> Unit`
- `drop(ptr) -> Unit` (`destroy(ptr)` + `#drop(ptr)`)
- `move(ptr) -> Pointer` (returns unbound slot in caller via `p_apply_body`'s `#break`)
- `move_into(dst, src) -> Unit` (destroys `dst` if `Live`, writes `#deref(src)`, Core-`#drop(src)`)
- `swap(p1, p2) -> Unit` (zero-RAII byte swap via temp slot)
- `clone(ptr) -> Pointer` (`__clone` if present; bitwise copy if POD; else abort)
- `cleanup_unbound(frame_ptr) -> Unit` (reverse-`drop` `sym == 0 && state == Live` non-child slots)
- `cleanup_all(frame_ptr) -> Unit` (reverse-`drop` all `state == Live` non-child slots)

## 4. PR 2B: Syntax Instrumentation (`lib/std/syntax/raii.lang`)
Loaded at the end of `lib/std/syntax/layer1.lang`; embeds staged `mem` Prototypes directly via `ast.value` (self-contained). Internal stdlib code continues to use Core `#deref` / `#write` / `#block` / `#break`.
1. **`$ptr` (`Deref`)**: POD Gate (`is_pod(ptr_shape(ptr))`), else `pod_gate_deref_non_pod`.
2. **`dst = rhs` (`AssignInfix`)**: If `rhs` is an unbound child slot `Pointer` in `#this` with matching target shape → `move_into(dst, rhs)`; else POD Gate (`pod_gate_assign_non_pod`) + direct `#write(dst, rhs)` (no `destroy` needed for POD).
3. **`coro @ rhs` (`CoroExpr`)**: If `rhs` is an unbound child slot `Pointer` → `#deref` + `#drop` + `#resume`; else POD Gate (`pod_gate_resume_non_pod`) + `#resume`.
4. **`{ Expr }` (`NonEmptyBlock`)**: Calls `cleanup_unbound(#this)` before closing so named fields flatten while anonymous temps are destroyed in LIFO order.
5. **Stdlib `break expr` (reused by `p_apply_body` function return)**: Evaluates `expr` into `__brk_v` in Frame A → enters child `#block` (Frame B) to extract/move return value (applying POD Gate `pod_gate_break_non_pod` for non-unbound values) and `#drop(#parent.__brk_v)` → calls `cleanup_all(#parent(#this))` (which skips active child Frame B via `#parent`) → `#break`s return value to Frame A's single unbound slot `p_ret` → Frame A executes Core `#break(#deref(p_ret))`.

## 5. Error Tags
Core natively catches uninitialized/null `#deref`, immutable/shape-mismatched `#write` (including `swap`), and invalid `#drop` on closed-object fields. Stdlib adds 5 semantic abort tags (via missing-field `#deref`):
- `pod_gate_deref_non_pod`
- `pod_gate_assign_non_pod`
- `pod_gate_resume_non_pod`
- `pod_gate_break_non_pod`
- `mem_clone_unsupported_non_pod`
