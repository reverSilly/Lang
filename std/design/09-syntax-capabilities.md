# 09. Syntax Engine & `capabilities.syntax` Addendum

This addendum records architectural decisions finalized during Compiler Steps 1–2 that refine `07-types-and-intrinsics.md` and `08-syntax.md`.

## 1. Strict Non-Nullable Layer 0 (Abolition of `()` and Empty `ε`)
- Parentheses `(Expr)` are **pure grouping** (`(E) ≡ E`). Empty parentheses `()` and trailing/dangling semicolons (`a ;`, `a ; ; b`) are **syntax errors**, not implicit `Unit`.
- An empty `Unit` object (`{}` of `0B/1`) is constructed in Layer 0 without special syntax by evaluating any `#block` that binds no fields (e.g. `#block(^#this)`). Layer 1 (`{}`) desugars to `#block(^#this)`.
- `Ast::GroupExpr` is unnecessary and removed from `compiler/ast/Ast.h`.

## 2. Unified Single-Path Packrat PEG Architecture
Layer 0 and Layer 1+ share **one** runtime-extensible Packrat PEG engine (`compiler/parser/Peg.h`):
- **Three decoupled layers**:
  1. `Grammar`: Pure syntax graph (`RuleId -> Rule{expr, hidden, cacheable}`), owning no visitor.
  2. `PackratParser`: Ephemeral per-parse session borrowing `const Grammar&` + input bytes, returning an immutable CST (`SyntaxNodePtr`). Position-indexed memoization (`memo_by_pos_`) guarantees strict **O(N · R)** linear time. Left-recursion is explicitly disallowed (cycle detection throws `ParseError`).
  3. `Evaluator<R>` (Visitor): Pure CST-to-target reduction table indexed by `RuleId` in O(1), owning no grammar. Two-phase separation guarantees backtracking safety and lets one CST serve multiple visitors.
- **Two orthogonal extension axes**:
  - **Horizontal (same-tier alternatives)**: `prepend_choice` / `append_choice` (e.g. extending `Delimiter`, `Atom`, `PostfixOp`).
  - **Vertical (precedence layers)**: `wrap_rule(target, layer_name, builder)` inserts a named recursive-descent layer (e.g. `Mul`, `Add`, `Cmp`, `Assign`) directly above an existing rule with zero left-recursion.
- **Delayed Symbol Interning**: `WordExpr` stores UTF-8 `text` rather than a process-local `SymbolId`, keeping ASTs decoupled from `RuntimeContext` for future relocatable `Prototype` module export and hot-reloading.

## 3. `capabilities.syntax` Minimal Algebraic Interface
Because Layer 0 has no string literal (`""`) and no function-call syntax yet, `capabilities.syntax` exposes **algebraic combinators** as standard `Native Prototype`s (invoked via `#evaluate`, reading arguments from `#this`) using native `Symbol`s for rule names and literals:

- **Atoms**:
  - `lit` (`{ sym: Symbol }`) — matches the UTF-8 bytes of `sym` (e.g. `\.`, `\{`, `\"` via Layer 0 `\` escape).
  - `range` (`{ lo: 1B, hi: 1B }`) — matches a single byte in `[lo, hi]`.
  - `ref` (`{ name: Symbol }`) — references a named rule.
  - `any` / `eof` — constant `Handle`s.
- **Combinators (N-ary via dynamic block positional fields)**:
  - `seq` / `choice` — read ordered positional fields (`0, 1, 2, ...` or `1, 2, 3, ...`) dynamically from the `#this` block until absent, supporting arbitrary-arity sequences and ordered choices without requiring arrays or nested binary wrappers.
  - `repeat` (`{ body: Handle, min: 8B, max: 8B }`) — unifies `opt`, `*`, `+`, and bounded repetition.
  - `lookahead` (`{ body: Handle, positive: Boolean }`) — unifies `&e` and `!e`.
- **Grammar & Handler Mutation**:
  - `get_rule` (`{ name: Symbol }`) -> current `PegExpr` `Handle`.
  - `set_rule` (`{ name: Symbol, expr: Handle, hidden: Boolean, handler: Prototype }`) — defines/updates a rule and binds its CST visitor `Prototype`. Higher-level helpers (`prepend_choice`, `append_choice`, `wrap_rule`, operator tables) compose in Stdlib on top of `get_rule` + `set_rule`.

## 4. `ShapeLayout` Size Sync (`07-types-and-intrinsics.md`)
With `FieldMapInline` (24B, align 8: `{ entries_ptr: 8B, count: 4B, bucket_mask: 4B, buckets_ptr: 8B }`) inlined directly into `ShapeLayout`, the transparent size of `Shape` is **40 bytes** (align 8): `{ size: 8B, align: 8B, fields: 24B, __shape_tag: 0B }`.
