# 09. Core Refinement, Unified Shape/OpenFrame Model, Packrat PEG, `capabilities` & Layer 1 Bootstrap

This document records the architectural refinements finalized during Compiler Steps 1–4 planning, superseding overlapping sections in `01-comptime.md`, `04-control-flow.md`, `07-types-and-intrinsics.md`, and `08-syntax.md`.

## 1. Final Layer 0 Core (Extreme Minimalism & Symmetry)
`#compile`, `^Expr`, and `#unbound` are moved out of Core and realized via `capabilities` + unified `Shape` reflection + Layer 1 grammar extensions. Coroutine lifecycle is completed symmetrically with `#destroy(coro)` and `#coro_root_frame(coro)`.

### 1.1 Layer 0 Hardcoded Symbols (5 total)
1. `\` — Full lexical escape (`\x` forces the next byte into the current `Word`, escaping any Layer 0 or Layer 1 delimiter).
2. `! ... !` — Comment (multiline, supports `\!` escape inside comments).
3. `#` — Core builtin invocation prefix (`#name` or `#name(arg1, arg2, ...)`).
4. `( , )` — Strictly non-nullable grouping `(Expr)` (`(E) ≡ E`) and builtin argument lists. Empty parentheses `()` and nullable empty expressions `ε` are **syntax errors**; an empty `Unit` (`{}` of `0B/1`) is simply any `#block` that binds no fields.
5. `;` — Strictly non-nullable sequence `a ; b` (evaluates left, discards value, returns right; dangling `a ;` is a syntax error).

*(Note: `^` is no longer a hardcoded Layer 0 symbol; in pure Layer 0 it is an ordinary `Word` character.)*

### 1.2 Core IR (`compiler/ast/Ast.h` — 4 Node Types)
1. `WordExpr { std::string text; }` — Delayed symbol interning (resolved to 4B `SymbolId` in `RuntimeContext`, keeping ASTs decoupled for module relocation).
2. `BuiltinExpr { BuiltinKind kind; std::vector<ExprPtr> args; }`
3. `SequenceExpr { std::vector<ExprPtr> items; }`
4. `ValueExpr { AstValue value; }` — Embeds any environment-independent stageable constant (8-variant `AstValue`: `RawBytes`, `SymbolVal`, `ShapeVal`, `AstProtoVal`, `IntrinsicVal`, `FfiExternVal`, `FfiBindPtrVal`, `StructVal`; strictly rejects all `Pointer`, `Handle`, `NativeProto`, and `BoundFfiProto`).

*(`GroupExpr` and `PrototypeExpr` are removed from Core IR.)*

### 1.3 Core `#` Builtins (21 total — Fully Symmetric)
- **3 Nullary Environment & Coroutine Cursors**: `#this`, `#caller`, `#in` *(note: `#unbound` removed — replaced by reverse scanning `#this`'s `Shape` via `capabilities.field_map.at`)*
- **5 Evaluation & Coroutine Lifecycle / Reflection**:
  - `#evaluate(p)` — Execute `Prototype` `p` in current `#this` frame.
  - `#block(p)` — Push child `ConstructingFrame` (mounted as an active anonymous child slot in parent's `live_shape`), execute `p`, close & flatten on normal completion, pop frame.
  - `#new(p)` — Create a `Suspended` coroutine from `Prototype` `p` (with root frame eagerly initialized), returning an 8B generational `Handle`.
  - `#destroy(coro)` — Explicitly reclaim a `Suspended` (abandoned) coroutine's stack & C++20 frame, bump its generation to invalidate stale `Handle`s, returning `bytes(1, 1)` (`0x01`/`0x00`; does not synthesize a body value).
  - `#coro_root_frame(coro)` — Return the fat `Pointer` to a `Suspended` coroutine's root `ConstructingFrame` (enabling Stdlib recursive Mark-Sweep GC and RAII cleanup).
- **5 Allocation & Three-State Slot Machine**: `#allocate(s, is_mut)`, `#allocate_val(v, is_mut)`, `#touch(ptr)`, `#bind(sym, ptr)`, `#drop(ptr)`
- **2 Raw Bit Read/Write (POD/RAII-agnostic)**: `#deref(ptr)`, `#write(dst_ptr, val)`
- **3 Navigation & Reflection**: `#parent(ptr)` (returns the parent OpenFrame pointer for an active `ConstructingFrame`, or null for root/non-open-frame pointers, enabling multi-level `#parent(#parent(#this))` traversal), `#field(ptr, sym)`, `#shape(val)`
- **3 Control Flow**:
  - `#branch(cond, p1, p2)` — `cond` is `bytes(1, 1)` (non-zero is truthy, `0x00` is falsy).
  - `#break(val)` — Single-layer exit from the innermost `#block` (pops `#this` and returns `val` directly). At a coroutine's root frame, terminates the coroutine (`Dead`), delivers `val` into `#caller`'s `#in` slot, immediately reclaims its resources, and symmetrically transfers to `#caller`.
  - `#resume(coro, val)` — Symmetric coroutine transfer: suspends current coroutine, writes `val` into `coro`'s `#in` slot, sets `coro->caller = current`, and transfers control to `coro`.

## 2. Unified `Shape` & Open `ConstructingFrame` Model
Open constructing frames and closed objects share the **exact same 40-byte `ShapeLayout`**, **24-byte `FieldMapInline`**, and **32-byte `FieldEntry`** — zero separate `OpenFrameLayout` or `SlotDesc` types:

```text
FieldEntry (32B, align 8):
  @0   sym:   Symbol  (4B)  — 0 = unbound anonymous slot (open frames only); != 0 for closed fields
  @4   state: u8      (1B)  — 0 = Uninitialized, 1 = Live, 2 = Empty (closed fields are always 1 = Live)
  @5   pad:   3B
  @8   ptr:   Pointer (24B) — relative fat-pointer { offset, sub_shape_ptr, mutability }

FieldMapInline (24B, align 8):
  @0   entries_ptr: u64 (8B) — ordered FieldEntry[count] (open frames keep all bind/unbind/3-state slots; closed shapes keep only Live bound fields)
  @8   count:       u32 (4B)
  @12  bucket_mask: u32 (4B)
  @16  buckets_ptr: u64 (8B) — open-addressing hash index (indexes ONLY sym != 0 && state == Live entries)

ShapeLayout (40B, align 8):
  @0   size:   u64 (8B)      — closed byte size (0 for open frames)
  @8   align:  u64 (8B)
  @16  fields: FieldMapInline (24B)
  @40  __shape_tag: {} (0B)
```

- **Ordinary Fat Pointer `#this` / `#parent(ptr)` / `#coro_root_frame(coro)`**: Each returns a regular `PointerLayout { addr = frame_addr, shape_ptr = &frame->live_shape, mutability = 1 }`. Every slot's `FieldEntry.ptr.addr` stores the 64-bit unsigned delta `slot_addr - frame_addr` (mod $2^{64}$), so `ptr.addr + fe.ptr.addr == slot_addr`.
- **Single Path for `#field(ptr, sym)`**: Open frames, closed objects, and `addr == 0` zero-base reflection pointers all execute the exact same lookup (`fields.find(sym)` + `ptr.addr + fe.ptr.addr`), with zero open-frame special cases.
- **Unified Call Stack & Object Tree**: When `#block(p)` pushes child frame `B`, `B` is mounted as an active `Live` anonymous child slot inside parent frame `A`'s `live_shape` (at most one active child frame slot per frame). Consequently:
  1. Stdlib computes `top_frame` itself by descending from `#coro_root_frame(coro)` through the unique open-frame child slot.
  2. Stdlib RAII reverse-scans `entries[count - 1 .. 0]` via `capabilities.field_map.at` to invoke `__destroy` on every `Live` slot (both named bindings and anonymous temporaries) uniformly.
  3. Stdlib optional Mark-Sweep Coroutine GC uses a single recursive `Live`-slot scanner starting from `#coro_root_frame(coro)`, automatically traversing the entire nested `#block` stack, marking reachable `Handle`s, running `__destroy` on unreachable orphan cycles, and finally freeing their raw stacks via `#destroy(coro)` — achieving **zero GC in Core** with **full RAII-complete GC in Stdlib**.
- **Generational Coroutine Handles**: `Handle` encodes `(generation << 32) | slot_index`. Reclaiming a `Dead` or `#destroy`ed coroutine immediately destroys its stack/C++20 frame, increments `generation`, and recycles the slot, preventing both leaks and dangling-pointer UB.

## 3. Two-Layer Code Model, `Prototype` vs. `RawBytes` & FFI Direction
- **Compile-Time Layer (Bootstrap / Build Script)**: Executed directly by the interpreter with `capabilities` mounted in root `#this`. Even without `^Expr` in Layer 0, straight-line `#bind` → `#evaluate(capabilities.ast.*)` → `#drop` in root `#this` constructs the first `Prototype` (the visitor for `^Expr`) and registers it via `capabilities.syntax.set_rule`.
- **Runtime Layer (Target Code)**: Target programs run in a completely isolated `RuntimeContext` (Phase 1 `backends.interpreter.run`) or native binary (Phase 2 `backends.llvm.emit_executable`) with zero `capabilities` mounted.
- **`Prototype` vs. `RawBytes` (`Shape{size, 1}`)**:
  - In memory, `Prototype` is an **8-byte opaque handle** (`prototype_shape`, align 8) pointing to live, linked code inside a `RuntimeContext` (internal binary/backend format intentionally left open for AST / bytecode / LLVM IR / native machine code).
  - String literals and raw byte buffers (`io.read_file`, `syntax.cst_view_bytes`, `symbols.to_bytes`, `intrinsics.bytes_slice`) are represented directly as `RawBytes` values of leaf `Shape{ size = N, align = 1, fields = empty }` (zero `#freeze` or `.rodata` pointer indirection anywhere in Core or `Ast`).
- **Unified Declarative FFI (`capabilities.ffi`)**:
  - `ffi.extern_symbol({ lib, name, abi })` produces pure-data `FfiProto` (resolved lazily per-`RuntimeContext` in Phase 1; `declare external` in Phase 2 LLVM).
  - `ffi.bind_ptr({ fn: Symbol, abi })` provides two-stage indirect C function pointer binding (clean argument frame, zero `libffi` in Phase 2 AOT).
  - Portable IR persistence uses `ast.serialize` / `ast.deserialize` on standalone `Ast` (retiring `serialize_proto` / `deserialize_proto`). See [10-ffi-and-capabilities.md](10-ffi-and-capabilities.md).

## 4. Unified Packrat PEG Engine (`compiler/parser/Peg.h`)
Layer 0 and Layer 1+ share **one** runtime-extensible Packrat PEG engine with strict **O(N · R)** linear time:
- **Three Decoupled Components**:
  1. `Grammar`: Pure syntax graph (`RuleId -> Rule{expr, hidden, cacheable}`), owning no visitor.
  2. `PackratParser`: Ephemeral per-parse session borrowing `const Grammar&` + input bytes, returning an immutable CST (`SyntaxNodePtr`). Position-indexed memoization (`memo_by_pos_`), no left-recursion (cycle detection throws `ParseError`), furthest-failure `line:col` diagnostics.
  3. `Evaluator<R>` (Visitor): Pure CST-to-target reduction table indexed by `RuleId` in O(1), owning no grammar.
- **Two Orthogonal Extension Axes**:
  - **Horizontal (same-tier alternatives)**: `prepend_choice` / `append_choice`.
  - **Vertical (precedence layers)**: `wrap_rule(target, layer_name, builder)` inserts a named recursive-descent layer with zero left-recursion.

## 5. `capabilities` Interface Specification
All capability operations are exposed as standard `Native` `Prototype` values (or constant `Value`s) under root `#this.capabilities`, invoked via `#evaluate` and reading named or ordered positional (`0, 1, 2, ...`) fields from `#this`:

| Module | Primitives / Constants | Role |
| :--- | :--- | :--- |
| **`constants`** | `true` (`0x01`), `false` (`0x00`) as `bytes(1, 1)`, `u64_zero`, `u64_one`, `u64_max`; bootstrapped base `Shape`s (`unit_shape`, `raw1/2/4/8_shape`, `symbol_shape`, `pointer_shape`, `field_entry_shape`, `field_map_shape`, `shape_meta`, `prototype_shape`, `handle_shape`) | Bootstrap values needed before Layer 1 literals exist (no Core `boolean_shape`) |
| **`field_map`** | `empty`, `is_empty`, `count`, `get`, `at`, `insert`, `ptr_shape` | Works uniformly on both closed `ShapeLayout`s and open frames' `live_shape` (`at` reads ordered `FieldEntry` with `sym`/`state`/`ptr`; `get` queries `Live` bound hash index; `ptr_shape` reads the `Shape` pointed to by any fat pointer) |
| **`symbols`** | `byte_len`, `byte_at`, `to_bytes`, `intern_bytes` | Dynamic `RawBytes` ↔ `Symbol` conversion (`intern_bytes` is strictly for constructing dynamic identifier `Symbol`s, never numeric values) |
| **`intrinsics`** | Prototype-Factory generators parameterized by width `N ∈ {1,2,4,8}`, `signed: bytes(1,1)`, and slot `Symbol`s (`add/sub/mul/div/rem_int`, `bit_and/or/xor/not`, `shl`, `shr`, `ext_int`, `trunc_int`, `add/sub/mul/div_float`, `ext/trunc_float`, `int_to_float`, `float_to_int`, `eq_bytes`, `eq_shape`, `lt/le_int`, `eq/lt/le_float`) + `bytes_at`, `bytes_slice` | Pure-data `IntrinsicProto` factories for hardware / LLVM IR instructions + `RawBytes` indexing |
| **`ast`** | `new_arena`, `destroy_arena`, `word({arena, sym})`, `builtin({arena, kind, 0, ...})`, `seq({arena, 0, ...})`, `value({arena, val})`, `to_proto({ast}) -> Prototype`, future `serialize`/`deserialize` | Standalone Core IR constructors + staging into current compile-time env (`to_proto`) |
| **`syntax`** | `new_layer0`, `destroy_frontend`; atoms `lit({fe, sym})`, `range({fe, lo, hi})`, `ref({fe, name})`, `any`, `eof`; combinators `seq`, `choice`, `repeat`, `lookahead`; rules `get_rule`, `set_rule`, `parse({fe, src: RawBytes, arena})`; explicit CST readers (`#this.node`): `cst_child_count`, `cst_view_sym`, `cst_view_bytes`, `cst_child_view_sym`, `cst_eval_child` | Algebraic PEG construction using native `Symbol`s; Stdlib builds `prepend_choice` / `wrap_rule` on top of `get_rule` + `set_rule` |
| **`backends`** | `interpreter.run({ast}) -> Value`; future `llvm.emit_ir` / `emit_object` / `emit_executable` | Isolated target backends consuming standalone `Ast` directly |
| **`io`** | `read_file({path: RawBytes}) -> RawBytes`, `write_file({path: RawBytes, bytes: RawBytes}) -> Unit` | Minimal compile-script file IO for Phase 1 MVP (superseded by `ffi.extern_symbol` + `libc`) |

## 6. Layer 1 Pure-Lang Two-Stage Self-Bootstrap Architecture
Layer 1 syntax is **100% self-bootstrapped from Layer 0** with zero hardcoded C++ `Layer1.h`:

### 6.1 Stage 1: `lib/std/boot0.lang` (Pure 5-Symbol Layer 0)
Registers only the 4 minimal rules needed to unlock readable syntax, producing `fe_boot1`:
1. `^Expr` (prefix, right-associative → `ValueExpr(AstProtoVal)`)
2. `.a` (prefix → `#field(#this, a)`) and `p.a` (postfix chain → `#field(p, a)`)
3. `$p` (prefix → `#deref(p)`)
4. `x := expr` (basic infix → `#bind(x, #allocate_val(expr, false))`)

`boot0.lang` then reads `lib/std/layer1.lang` via `capabilities.io.read_file`, parses it with `fe_boot1` via `capabilities.syntax.parse`, stages it via `ast.to_proto`, and `#evaluate`s it.

### 6.2 Stage 2: `lib/std/layer1.lang` (Written in `fe_boot1`)
Extends `fe_boot1` into the full Layer 1 frontend `fe_layer1`, adhering to **location-owned mutability** and **Shape-driven Stdlib dispatch**:
- **Smart `sym := RHS` Dispatcher (No `mut x :=` or `::=` Sugar)**:
  - Mutability belongs strictly to locations created by `#allocate(S, true)` / `#allocate_val(v, true)`.
  - `sym := RHS` checks if `#shape(RHS) == pointer_shape`: if so, it attempts Core `#bind(sym, RHS)` — returning `true` binds an unbound direct child slot of `#this` (e.g. `x := #allocate_val(42, true)` directly binds a mutable slot); returning `false` (external or already-bound pointer such as `p := .x`) falls back to `#bind(sym, #allocate_val(RHS, false))` storing the pointer value itself. Non-pointer values go straight to `#bind(sym, #allocate_val(RHS, false))`.
- **Blocks & Assignment**: `{}` → `#block(^#this)`, `{ expr }` → `#block(^expr)`, `ptr = val` → `assign` dispatcher (`#write` for POD; future `__destroy` + `#write` for RAII).
- **Literals (Zero `Symbol` Abuse)**:
  - Integer literals (`123`): visitor reads raw ASCII bytes via `cst_view_bytes`, derives `'0'` (`48`) via `symbols.byte_at(0, 0)` and `10` from `u64_one`, computes `acc = acc * 10 + (b - 48)` in Lang, and emits `ast.value(i64)`.
  - Byte string literals (`"..."`): visitor reads raw bytes via `cst_view_bytes` (stripping quotes) and emits `ast.value(RawBytes)` (`Shape{ size = N, align = 1 }`).
  - Booleans (`true` / `false`): emit `ast.value(bytes(1, 1))` (`0x01` / `0x00`).
- **Shape-Dispatched Operators** (vertical precedence ladder, tightest to loosest): `*` → `+` / `-` → `<` / `==` (syntax stays type-agnostic; dispatchers inspect `#shape`).
- **English-Order Suffix Control Flow & Coroutines**:
  - `e1 if cond else e2` → `#branch(cond, ^e1, ^e2)`
  - `body while cond` → recursive `#branch` + `#evaluate` loop prototype
  - `coro @ val` → `#resume(coro, val)`
