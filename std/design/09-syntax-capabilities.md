# 09. Core Refinement, Unified Shape/OpenFrame Model, Packrat PEG & `capabilities`

This document records the architectural refinements finalized during Compiler Steps 1–3, superseding overlapping sections in `01-comptime.md`, `04-control-flow.md`, `07-types-and-intrinsics.md`, and `08-syntax.md`.

## 1. Final Layer 0 Core (Extreme Minimalism & Symmetry)
Both `#compile`, `^Expr`, and `#unbound` are moved out of Core and realized via `capabilities` + unified `Shape` reflection + Layer 1 grammar extensions. Coroutine lifecycle is completed symmetrically with `#destroy(coro)` and `#coro_root_frame(coro)`.

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
4. `ValueExpr { Runtime::Value value; }` — Embeds any constant `Value` (including `Shape` and `Prototype` values).

*(`GroupExpr` and `PrototypeExpr` are removed from Core IR.)*

### 1.3 Core `#` Builtins (22 total — Fully Symmetric)
- **4 Nullary Environment & Coroutine Cursors**: `#this`, `#parent`, `#caller`, `#in` *(note: `#unbound` removed — replaced by reverse scanning `#this`'s `Shape` via `capabilities.field_map.at`)*
- **5 Evaluation & Coroutine Lifecycle / Reflection**:
  - `#evaluate(p)` — Execute `Prototype` `p` in current `#this` frame.
  - `#block(p)` — Push child `ConstructingFrame` (mounted as an active anonymous child slot in parent's `live_shape`), execute `p`, close & flatten on normal completion, pop frame.
  - `#new(p)` — Create a `Suspended` coroutine from `Prototype` `p`, returning an 8B generational `Handle`.
  - `#destroy(coro)` — Explicitly reclaim a `Suspended` (abandoned) coroutine's stack & C++20 frame, bump its generation to invalidate stale `Handle`s, returning `Boolean` (does not synthesize a body value).
  - `#coro_root_frame(coro)` — Return the fat `Pointer` to a `Suspended` coroutine's root `ConstructingFrame` (enabling Stdlib recursive Mark-Sweep GC and RAII cleanup).
- **5 Allocation & Three-State Slot Machine**: `#allocate(s, is_mut)`, `#allocate_val(v, is_mut)`, `#touch(ptr)`, `#bind(sym, ptr)`, `#drop(ptr)`
- **2 Raw Bit Read/Write (POD/RAII-agnostic)**: `#deref(ptr)`, `#write(dst_ptr, val)`
- **3 Navigation, Reflection & Freeze**: `#field(ptr, sym)`, `#shape(val)`, `#freeze(ptr)`
- **3 Control Flow**:
  - `#branch(cond, p1, p2)`
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

- **Ordinary Fat Pointer `#this` / `#parent` / `#coro_root_frame`**: Each returns a regular `PointerLayout { addr = frame_addr, shape_ptr = &frame->live_shape, mutability = 1 }`. Every slot's `FieldEntry.ptr.addr` stores the 64-bit unsigned delta `slot_addr - frame_addr` (mod $2^{64}$), so `ptr.addr + fe.ptr.addr == slot_addr`.
- **Single Path for `#field(ptr, sym)`**: Open frames, closed objects, and `addr == 0` zero-base reflection pointers all execute the exact same lookup (`fields.find(sym)` + `ptr.addr + fe.ptr.addr`), with zero open-frame special cases.
- **Unified Call Stack & Object Tree**: When `#block(p)` pushes child frame `B`, `B` is mounted as an active `Live` anonymous child slot inside parent frame `A`'s `live_shape` (at most one active child frame slot per frame). Consequently:
  1. Stdlib computes `top_frame` itself by descending from `#coro_root_frame(coro)` through the unique open-frame child slot.
  2. Stdlib RAII reverse-scans `entries[count - 1 .. 0]` via `capabilities.field_map.at` to invoke `__destroy` on every `Live` slot (both named bindings and anonymous temporaries) uniformly.
  3. Stdlib optional Mark-Sweep Coroutine GC uses a single recursive `Live`-slot scanner starting from `#coro_root_frame(coro)`, automatically traversing the entire nested `#block` stack, marking reachable `Handle`s, running `__destroy` on unreachable orphan cycles, and finally freeing their raw stacks via `#destroy(coro)` — giving **zero GC in Core** while achieving **full RAII-complete GC in Stdlib**.
- **Generational Coroutine Handles**: `Handle` encodes `(generation << 32) | slot_index`. Reclaiming a `Dead` or `#destroy`ed coroutine immediately destroys its stack/C++20 frame, increments `generation`, and recycles the slot, preventing both leaks and dangling-pointer UB.

## 3. Two-Layer Code Model & `Prototype` Positioning
- **Compile-Time Layer (Bootstrap / Build Script)**: Executed directly by the interpreter with `capabilities` mounted in root `#this`. Even without `^Expr` in Layer 0, straight-line `#bind` → `#evaluate(capabilities.ast.*)` → `#drop` in root `#this` constructs the first `Prototype` (the visitor for `^Expr`) and registers it via `capabilities.syntax.set_rule`.
- **Runtime Layer (Target Code)**: Pure runtime programs do not depend on `capabilities.syntax`, `capabilities.ast`, or `capabilities.compiler`.
- **`Prototype` vs. `Bytes`**:
  - In memory, `Prototype` is an **8-byte opaque handle** (`prototype_shape`, align 8) pointing to live, linked code inside a `RuntimeContext` (internal binary/backend format intentionally left open for AST / bytecode / LLVM IR / native machine code).
  - Across process boundaries (relocatable libraries, hot-reloading, or OS executables), code is exchanged as transparent 16-byte **`Bytes`** (`{ ptr: 8B, len: 8B }`). No separate `Executable` or `Module` core types are needed.

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
| **`constants`** | `true`, `false`, `u64_zero`, `u64_one`, `u64_max`; bootstrapped base `Shape`s (`unit_shape`, `raw1/4/8_shape`, `symbol_shape`, `boolean_shape`, `pointer_shape`, `bytes_shape`, `field_entry_shape`, `field_map_shape`, `shape_meta`, `prototype_shape`, `handle_shape`) | Bootstrap values needed before Layer 1 literals exist |
| **`field_map`** | `empty`, `is_empty`, `count`, `get`, `at`, `insert` | Works uniformly on both closed `ShapeLayout`s and open frames' `live_shape` (`at` reads ordered `FieldEntry` with `sym`/`state`/`ptr`; `get` queries `Live` bound hash index) |
| **`symbols`** | `byte_len`, `byte_at`, `to_bytes`, `intern` | Dynamic byte-slice ↔ `Symbol` conversion |
| **`intrinsics`** | Low-level integer/float/bitwise ops & comparisons (`add_i64`, `sub_i64`, `mul_i64`, `lt_i64`, `eq_i64`, `eq_sym`, etc.) | Hardware / LLVM IR instruction backing |
| **`ast`** | `word({sym})`, `builtin({kind: Symbol, 0, 1, ...})`, `seq({0, 1, ...})`, `value({val})`, `to_proto({ast}) -> Prototype` | 1:1 constructors for the 4 Core IR nodes + materializing a `Prototype` (N-ary via dynamic block positional fields) |
| **`syntax`** | `new_layer0`; atoms `lit({sym})`, `range({lo, hi})`, `ref({fe, name})`, `any`, `eof`; combinators `seq({0, 1, ...})`, `choice({0, 1, ...})`, `repeat({body, min, max})`, `lookahead({body, positive})`; rules `get_rule`, `set_rule({fe, name, expr, hidden, handler})`; explicit CST readers (`#this.node`): `cst_child_count`, `cst_view_sym`, `cst_eval_child` | Algebraic PEG construction using native `Symbol`s; Stdlib builds `prepend_choice` / `wrap_rule` on top of `get_rule` + `set_rule` |
| **`compiler`** | `parse({fe, src: Bytes}) -> Prototype`, `serialize_proto({proto}) -> Bytes`, `deserialize_proto({mod: Bytes}) -> Prototype`, `emit_executable({proto}) -> Bytes` | Pure `Bytes ↔ Prototype` transformations (zero file IO) |
| **`io`** | `read_file`, `write_file`, `print_*` | All filesystem and console IO |
