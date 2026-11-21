# 09. Core Refinement, Packrat PEG Engine & `capabilities` Architecture

This document records the architectural refinements finalized during Compiler Steps 1–2, superseding overlapping sections in `01-comptime.md`, `07-types-and-intrinsics.md`, and `08-syntax.md`.

## 1. Final Layer 0 Core (Extreme Minimalism)
Both `#compile` and `^Expr` are moved out of Core and realized through `capabilities` + Layer 1 grammar extensions.

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

### 1.3 Core `#` Builtins (21 total; `#compile` removed)
- **5 Nullary Environment & Slot Cursors**: `#this`, `#parent`, `#unbound`, `#caller`, `#in`
- **3 Evaluation & Coroutine Entry**: `#evaluate(p)`, `#block(p)`, `#new(p)`
- **5 Allocation & Three-State Slot Machine**: `#allocate(s, is_mut)`, `#allocate_val(v, is_mut)`, `#touch(ptr)`, `#bind(sym, ptr)`, `#drop(ptr)`
- **2 Raw Bit Read/Write (POD/RAII-agnostic)**: `#deref(ptr)`, `#write(dst_ptr, val)`
- **3 Navigation, Reflection & Freeze**: `#field(ptr, sym)`, `#shape(val)`, `#freeze(ptr)`
- **3 Control Flow**:
  - `#branch(cond, p1, p2)`
  - `#break(val)` — Single-layer exit from the innermost `#block` (pops `#this` and returns `val` as the block's result). When executed at a coroutine's root frame, terminates the coroutine (`Dead`) and delivers `val` into `#caller`'s `#in` slot, resuming `#caller`.
  - `#resume(coro, val)` — Symmetric coroutine transfer: suspends current coroutine, writes `val` into `coro`'s `#in` slot, sets `coro->caller = current`, and transfers control to `coro`.

## 2. Two-Layer Code Model & `Prototype` Positioning
Lang separates **compile-time execution** from **runtime target code** without introducing a separate meta-language:
- **Compile-Time Layer (Bootstrap / Build Script)**: Executed directly by the interpreter, with the full `capabilities` object mounted in the root `#this` frame. Even without `^Expr` in Layer 0, straight-line `#bind` → `#evaluate(capabilities.ast.*)` → `#drop` in the root `#this` frame constructs the first `Prototype` (the visitor for `^Expr`) and registers it via `capabilities.syntax.set_rule`, bootstrapping Layer 1 syntax with zero deadlock.
- **Runtime Layer (Target Code)**: Produced as `Prototype` values. Pure runtime programs do not depend on `capabilities.syntax`, `capabilities.ast`, or `capabilities.compiler`.
- **`Prototype` Positioning**:
  - In memory, a `Prototype` is an **8-byte opaque handle** (`prototype_shape`, align 8) pointing to live, linked code within a `RuntimeContext` (fast O(1) passing and `#evaluate` dispatch). Its internal binary/backend representation (AST, bytecode, LLVM IR, native machine code) is intentionally left open for backend evolution.
  - Across process boundaries (module export/import, hot-reloading, or OS executable generation), code is exchanged as plain **`Bytes`** (`{ ptr, len }`). No separate `Executable` or `Module` core types are needed.

## 3. Unified Packrat PEG Engine (`compiler/parser/Peg.h`)
Layer 0 and Layer 1+ share **one** runtime-extensible Packrat PEG engine with strict **O(N · R)** linear time:
- **Three Decoupled Components**:
  1. `Grammar`: Pure syntax graph (`RuleId -> Rule{expr, hidden, cacheable}`), owning no visitor.
  2. `PackratParser`: Ephemeral per-parse session borrowing `const Grammar&` + input bytes, returning an immutable CST (`SyntaxNodePtr`). Position-indexed memoization (`memo_by_pos_`), no left-recursion (cycle detection throws `ParseError`), furthest-failure `line:col` diagnostics.
  3. `Evaluator<R>` (Visitor): Pure CST-to-target reduction table indexed by `RuleId` in O(1), owning no grammar. Two-phase separation ensures backtracking safety and allows one CST to serve multiple visitors.
- **Two Orthogonal Extension Axes**:
  - **Horizontal (same-tier alternatives)**: `prepend_choice` / `append_choice` (e.g. extending `Delimiter`, `Atom`, `PostfixOp`).
  - **Vertical (precedence layers)**: `wrap_rule(target, layer_name, builder)` inserts a named recursive-descent layer (e.g. `Mul`, `Add`, `Cmp`, `Assign`) directly above an existing rule with zero left-recursion.

## 4. `capabilities` Interface Specification
All capability operations are exposed as standard `Native` `Prototype` values (or constant `Value`s) mounted under root `#this.capabilities`, invoked via `#evaluate` and reading named or ordered positional (`0, 1, 2, ...`) fields from `#this`:

| Module | Primitives / Constants | Role |
| :--- | :--- | :--- |
| **`constants`** | `true`, `false`, `u64_zero`, `u64_one`, `u64_max`; bootstrapped base `Shape`s (`unit_shape`, `raw1/4/8_shape`, `symbol_shape`, `boolean_shape`, `pointer_shape`, `shape_meta`, `prototype_shape`, `handle_shape`) | Bootstrap values needed before Layer 1 literals exist |
| **`field_map`** | `empty`, `is_empty`, `count`, `get`, `at`, `insert` | Flat open-addressing field map for reflection & `Shape` construction |
| **`symbols`** | `byte_len`, `byte_at`, `intern` | Dynamic byte-slice ↔ `Symbol` conversion |
| **`intrinsics`** | Low-level integer/float/bitwise ops & comparisons (`add_i64`, `sub_i64`, `mul_i64`, `lt_i64`, `eq_bytes`, etc.) | Hardware / LLVM IR instruction backing |
| **`ast`** | `word({sym})`, `builtin({kind: Symbol, 0, 1, ...})`, `seq({0, 1, ...})`, `value({val})`, `to_proto({ast}) -> Prototype` | 1:1 constructors for the 4 Core IR nodes + materializing a `Prototype` (N-ary via dynamic block positional fields) |
| **`syntax`** | `new_layer0`; atoms `lit({sym})`, `range({lo, hi})`, `ref({fe, name})`, `any`, `eof`; combinators `seq({0, 1, ...})`, `choice({0, 1, ...})`, `repeat({body, min, max})`, `lookahead({body, positive})`; rules `get_rule`, `set_rule({fe, name, expr, hidden, handler})`; CST readers `cst_view_len`, `cst_view_byte`, `cst_view_sym`, `cst_child_count`, `cst_child_at`, `cst_eval_child` | Algebraic PEG construction using native `Symbol`s (no string-literal bootstrap deadlock); Stdlib builds `prepend_choice` / `wrap_rule` on top of `get_rule` + `set_rule` |
| **`compiler`** | `parse({fe, src_bytes}) -> Prototype`, `serialize_proto({proto}) -> Bytes`, `deserialize_proto({mod_bytes}) -> Prototype`, `emit_executable({proto}) -> Bytes` | Pure `Bytes ↔ Prototype` transformations (zero file IO; supports in-memory hot-reload and AOT binary emission) |
| **`io`** | `read_file`, `write_file`, `print_*` | All filesystem and console IO |

## 5. `ShapeLayout` Size Sync (`07-types-and-intrinsics.md`)
With `FieldMapInline` (24B, align 8: `{ entries_ptr: 8B, count: 4B, bucket_mask: 4B, buckets_ptr: 8B }`) inlined directly into `ShapeLayout`, the transparent size of `Shape` is **40 bytes** (align 8): `{ size: 8B, align: 8B, fields: 24B, __shape_tag: 0B }`.
