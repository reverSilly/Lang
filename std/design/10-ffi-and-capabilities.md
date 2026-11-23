# 10. Stateless `capabilities`, Standalone `Ast`, Isolated Backends & Unified FFI

This document specifies (1) the **100% stateless** three-tier modularization of `capabilities` with explicit, symmetric resource handles (`new_arena`/`destroy_arena`, `new_layer0`/`destroy_frontend`), (2) the **environment-independent Standalone `Ast`** (`AstValue` + pure-value `AstShape`) and strict Stageability boundary (`ast.value`), (3) the decoupled `ast.to_proto` (compile-time staging) vs. `capabilities.backends.*` (isolated target execution / AOT codegen) pipeline, and (4) the unified pure-data `capabilities.ffi` (`extern_symbol` + two-stage `bind_ptr`).

---

## 1. Stateless Three-Tier `capabilities` Architecture

`capabilities` holds **zero hidden host state** — there is no `CapabilityHost` / `xxxHost` singleton owning internal pools. Every capability primitive is a pure stateless `NativeProto` `Prototype` operating solely on `#this` and `RuntimeContext`.

### 1.1 Three-Tier Logical Split (Compile-Time vs. Target Runtime Boundary)

In Lang, `capabilities` is mounted under root `#this.capabilities` strictly in the **compile-time script's** `RuntimeContext`. Architecturally, capabilities are partitioned into **three tiers**:

| Tier | Sub-modules | Available in Compile Script | Available in Target Runtime / AOT | Backing Mechanism |
| :--- | :--- | :---: | :---: | :--- |
| **1. Core Factories & Reflection** | `constants`, `field_map`, `symbols`, `intrinsics` | Yes | **No** *(baked into `Ast` at compile time)* | Interpreter C++ / LLVM IR instructions |
| **2. Declarative FFI Factories** | `ffi` (`extern_symbol`, `bind_ptr`) | Yes | **No** *(baked into `Ast` at compile time)* | Interpreter: lazy per-`RuntimeContext` `FfiDispatchCache` (`dlopen`/`dlsym` + `libffi`); AOT: `declare external` + static indirect thunks |
| **3. Compiler Frontend & Backends** | `ast`, `syntax`, `backends` (`interpreter`, future `llvm`) *(+ temporary `io`)* | Yes | **No** | Compiler process only; never mounted in target root frames or emitted executables |

- **Complete Target Isolation (Phase 1 & Phase 2 Isomorphism)**:
  - When a compile script invokes **`capabilities.backends.interpreter.run({ ast })`**, the interpreter backend creates a **brand-new, completely independent `RuntimeContext` + `Evaluator`** (`#parent(#this) = null`, **zero `capabilities`**, independent `SymbolPool` / `ShapeArena` / `PrototypeArena` / `CoroutinePool` / `FfiDispatchCache`), interns the standalone `Ast` into it, executes it, and tears down all target resources (including any lazily opened FFI libraries) on return.
  - When a compile script invokes **`capabilities.backends.llvm.emit_executable({ ast, ... })`** *(Phase 2)*, the LLVM backend lowers the **exact same standalone `Ast`**: embedded `IntrinsicVal` lowers to single LLVM instructions, `FfiExternVal` lowers to `declare external` + linker flags, `FfiBindPtrVal` lowers to a statically compiled indirect-call trampoline (zero runtime `libffi`), and no `capabilities` struct exists in the binary.
  - Runtime dynamic library loading in target programs requires no special compiler capability: target code simply declares `dlopen`/`dlsym`/`dlclose` (or Win32 `LoadLibraryA`/`GetProcAddress`) via `ffi.extern_symbol` and binds returned function pointers via `ffi.bind_ptr`.
- **Retirement of Temporary `capabilities.io`**:
  - `capabilities.io.read_file` and `capabilities.io.write_file` are Phase 1 bootstrap placeholders for reading source files before `libc` FFI is loaded.

### 1.2 Explicit Resource Lifecycles, `Ast` vs. `Prototype`, and Backends

Every heap-allocated compiler resource uses an explicit symmetric `new_*`/`destroy_*` handle pair — zero hidden GC, zero hidden host pools:

1. **Standalone AST & Staging (`capabilities.ast`)**:
   - `new_arena() -> Handle`: Allocates an `AstArena` (a simple pointer-stable `std::deque<Ast::ExprPtr>`, owning no `StringTable` or `ShapeTable` — every `ExprPtr` is 100% self-contained even after `destroy_arena`).
   - `destroy_arena({ arena: Handle }) -> Unit`: Frees the `AstArena` immediately.
   - `word({ arena, sym })`, `builtin({ arena, kind, 0, 1, ... })`, `seq({ arena, 0, 1, ... })`, `value({ arena, val })`: Construct standalone Core IR nodes inside `#this.arena` and return `Handle` (`Ast::ExprPtr*`).
   - `to_proto({ ast: Handle }) -> Prototype`: **Stages/embeds** a standalone `Ast` into the **current compile-time `RuntimeContext`** as a live `AstProto`, so the compile script can execute it via `#evaluate` / `#block` (e.g., bootstrapping `boot1`/`layer1`, or evaluating `lib/std/libc.lang` at compile time).
   - `serialize({ ast: Handle }) -> Bytes` / `deserialize({ arena: Handle, bytes }) -> Handle (Ast)` *(replaces retired `serialize_proto`/`deserialize_proto`)*: Lossless binary encoding/decoding of standalone `Ast` for incremental build caches and cross-process IR transfer.
2. **Frontend / PEG Arena (`capabilities.syntax`)**:
   - `new_layer0() -> Handle` / `destroy_frontend({ fe: Handle }) -> Unit`: Explicit lifecycle for `Parser::Frontend` + PEG expression arena.
   - `lit`, `range`, `ref`, `any`, `eof`, `seq`, `choice`, `repeat`, `lookahead`, `get_rule`, `set_rule`, `parse`, and CST readers (`cst_child_count`, `cst_view_sym`, `cst_view_bytes`, `cst_child_view_sym`, `cst_eval_child`).
3. **Target Backends (`capabilities.backends`)**:
   - Backends consume **standalone `Ast` handles** directly — never host `Prototype` objects.
   - **`capabilities.backends.interpreter.run({ ast: Handle }) -> Value`** *(Phase 1)*: Creates a fresh `RuntimeContext` + `Evaluator`, interns `ast`, executes it in an isolated root frame (`#parent(#this) = null`, zero `capabilities`), and reifies/interns any stageable return value back to the caller (or `Unit`).
   - **`capabilities.backends.llvm`** *(Phase 2)*: Symmetrical AOT codegen sub-namespace (`emit_ir`, `emit_object`, `emit_executable`) consuming the exact same standalone `Ast` handle.

```text
                                                               ┌──▶ ast.to_proto({ast}) ──▶ Prototype (stage into compile-time env: #evaluate / #block)
                                                               ├──▶ ast.serialize({ast}) ──▶ Bytes (portable binary IR cache)
BytePtr (source) ──▶ syntax.parse({fe, src, arena}) ──▶ Ast ──▶ [AST composition / lifting via ast.*] ──┤
                                                               ├──▶ backends.interpreter.run({ast}) ──▶ Fresh RuntimeContext + Evaluator (Phase 1)
                                                               └──▶ backends.llvm.emit_executable({ast, ...}) ──▶ Native Binary (Phase 2 AOT)
```

---

## 2. Environment-Independent Standalone `Ast` (`AstValue` & `AstShape`)

`compiler/ast/Ast.h` has **zero dependency on `compiler/runtime/`** (no `Runtime::Value`, no `SymbolId`, no `ShapeLayout*`, no `PrototypeObject*`, no `.rodata` concept).

### 2.1 Pure-Value `AstShape` & Non-Redundant `StructVal`

- **`AstShape`** (plain value type, zero `shared_ptr`) is used **only** where a layout is needed without a value (`ShapeVal`, `FfiExternVal.args/ret`, `FfiBindPtrVal.args/ret`):
  ```cpp
  struct AstField;
  struct AstShape {
      uint64_t size = 0;
      uint64_t align = 1;
      std::vector<AstField> fields;
  };
  struct AstField {
      std::string name;
      uint64_t offset = 0;
      AstShape shape;
  };
  ```
- **`StructVal`** does **not** carry a redundant `AstShape` — each field's `sub_shape` is derived directly from its `AstValue` during interning:
  ```cpp
  struct AstStructField {
      std::string name;
      uint64_t offset = 0;
      AstValue value;
  };
  struct StructVal {
      uint64_t size = 0;
      uint64_t align = 1;
      std::vector<AstStructField> fields;
  };
  ```
- Runtime `RuntimeContext` retains `ShapeArena` so 24-byte POD `PointerLayout::shape_ptr` stays a stable 8-byte address and `Shape` equality remains `O(1)` (`addr_a == addr_b`).

### 2.2 8-Variant Structured `AstValue` & Strict Stageability Rule

```cpp
using AstValue = std::variant<
    RawBytes,       // { uint64_t align; std::vector<uint8_t> bytes } — unit, raw1/2/4/8, "hello" (Shape{N, 1})
    SymbolVal,      // { std::string name } — delayed interning
    ShapeVal,       // { AstShape shape }
    AstProtoVal,    // { ExprPtr ast }
    IntrinsicVal,   // 5-variant IntrinsicSpec with std::string slot names
    FfiExternVal,   // { std::string lib, symbol, cc; AstShape args, ret }
    FfiBindPtrVal,  // { std::string fn_slot, cc; AstShape args, ret }
    StructVal       // { uint64_t size, align; std::vector<AstStructField> fields }
>;
```

- **Strict Stageability Boundary (`ast.value` Reification)**:
  - **Allowed**: Only the 8 stageable variants above (including string literals `"hello"` as pure `RawBytes` `Shape{ size = N, align = 1 }`).
  - **Strictly Rejected (compile-time error)**: **All `Pointer`s** (Core has no `#freeze`), **all `Handle`s**, **all `NativeProto`s** (including `capabilities`), **all runtime-bound `BoundFfiProto`s**, and any struct recursively containing them.
- **Lossless Round-Trip (`intern_ast_value(target_rt, av)`)**:
  - Interns leaf bytes via `target_rt.shapes.intern_leaf`, symbols via `target_rt.symbols.intern`, shapes via `intern_ast_shape` (automatically canonicalizing builtin transparent shapes like `pointer_shape` / `symbol_shape` to `target_rt.shapes.*`), prototypes via `target_rt.prototypes.create_*`, and structs by recursively interning each field value and packing bytes at `f.offset` into `target_rt.shapes.intern_struct`.

---

## 3. Unified Pure-Data `capabilities.ffi` Specification

Lang's Core has **no function-call syntax and no separate C type system**. Every `Shape` is already a transparent C-compatible memory layout (`size`, `align`, ordered `FieldEntry[]`), so FFI reuses `Shape` as the ABI contract and `#evaluate` as the sole invocation mechanism.

`capabilities.ffi` exposes **exactly 2 pure-data Prototype-Factories** (retiring `open_lib` / `load_symbol` / `close_lib` completely — zero OS handles leak into compile scripts or `Ast`):

### 3.1 Primitives (2 total)

1. **`capabilities.ffi.extern_symbol`** *(Direct External C Call)*
   - **Input fields in `#this`**:
     - `lib: RawBytes` (leaf `Shape{ size = N, align = 1 }`, or `Pointer` to leaf bytes during bootstrap) — library name (e.g., `""` for default process/libc symbols, `"m"`, `"sqlite3"`).
     - `name: RawBytes` — exact C symbol name (e.g., `"malloc"`, `"strlen"`, `"div"`, `"dlopen"`).
     - `abi: Abi`:
       ```text
       Abi {
         args: Shape,   ! Closed Shape whose ordered fields define C parameters (unit_shape = 0 args) !
         ret:  Shape,   ! Closed Shape defining the C return type (unit_shape = void) !
         cc:   Symbol   ! Calling convention: `c` (platform default C ABI) !
       }
       ```
   - **Return**: `Prototype` (`FfiProto` — pure data `{ lib_name, symbol_name, args_shape_addr, ret_shape_addr, cc }`, **zero `NativeFn` closure, zero `dlopen` handle**, 100% stageable into `Ast`).
   - **Phase 1 Execution**: Each `RuntimeContext` owns a lazy `FfiDispatchCache` that opens `lib_name` via `dlopen`, resolves `symbol_name` via `dlsym`, prepares `ffi_cif` once, and automatically calls `dlclose` on all cached libraries in `~RuntimeContext()`.
   - **Phase 2 Execution**: Emits LLVM `declare external` + direct C ABI call + linker flag `-l<lib_name>`.

2. **`capabilities.ffi.bind_ptr`** *(Two-Stage Indirect C Function Pointer Call)*
   - **Stage 1 (Compile-Time Factory)**:
     - **Input fields in `#this`**:
       - `fn: Symbol` — slot name in `#this` from which Stage 2 will read the 8-byte raw C function pointer (`c_ptr`, `raw8_shape`).
       - `abi: Abi` — compile-time fixed C ABI `{ args: Shape, ret: Shape, cc: Symbol }`.
     - **Return**: `Prototype` (`FfiBindPtrProto` — pure data `{ fn_slot, args_shape_addr, ret_shape_addr, cc }`, 100% stageable into `Ast`).
   - **Stage 2 (Runtime Binding — zero `capabilities`, zero slot collision with `abi.args`)**:
     - Evaluating `FfiBindPtrProto` reads **only** `#this.<fn_slot>` (8-byte raw address) and returns a first-class runtime `Prototype` (`BoundFfiProto { void* fn_ptr, args_shape_addr, ret_shape_addr, cc }`).
     - Because `BoundFfiProto` holds a live process `fn_ptr`, it is runtime-bound and cannot be lifted back into `Ast`.
   - **Stage 3 (Runtime Call — 100% identical to `extern_symbol`)**:
     - Evaluating `BoundFfiProto` reads only `abi.args` from `#this` (clean argument frame, no `fn` slot pollution) and invokes `fn_ptr`.
     - **Phase 1**: Dispatches via `target_rt`'s `FfiDispatchCache` (`ffi_cif` + `FFI_FN(fn_ptr)`).
     - **Phase 2 LLVM AOT**: Because `abi` is fixed in Stage 1, LLVM statically generates an indirect-call trampoline for `abi`, and Stage 2 simply pairs `{ fn_ptr, static_thunk }` — **zero `libffi` dependency in AOT binaries**.

### 3.2 Invocation Semantics (`#evaluate`)

For both `FfiProto` and `BoundFfiProto`:
1. For each non-zero-sized `fe` in `args_shape.fields.entries[0 .. count-1]` (0-byte tag fields skipped):
   - Look up `slot_ptr = #field(#this, fe.sym)` in the active `#this` frame, verify `Live` and `slot_ptr.shape_ptr == fe.sub_shape_addr`, and record `arg_ptrs[i] = reinterpret_cast<void*>(slot_ptr.addr)`.
2. Invoke the C function under `ffi_cif` (Phase 1) or native call (Phase 2).
3. Return `Value{ ret_shape_addr, bytes }` (or `unit` if `ret_shape.size == 0`).

- **Zero Implicit Pointer Unwrapping**: Lang fat `Pointer` is 24 bytes `{ addr, shape_ptr, mutability }`; C `void*` / `const char*` is an 8-byte raw address (`raw8_shape`). Callers explicitly pass `p.addr`.

---

## 4. Unified `Shape` → C ABI Type Mapping & Struct Layout Verification

Floating-point `Shape`s are identified by a zero-byte tag field (`__f32_tag` / `__f64_tag`), matching `Symbol` (`__sym_tag`):

| Lang `Shape` | Detection Rule | `libffi` Type (`Phase 1`) | LLVM Type (`Phase 2`) |
| :--- | :--- | :--- | :--- |
| `unit_shape` (`0B/1`, leaf) | `size == 0 && is_leaf()` | `ffi_type_void` *(return only; 0-arg `args` yields `nargs = 0`)* | `void` |
| `f32` (`4B/4` with `__f32_tag`) | `size == 4` and has `__f32_tag` (`0B`) | `ffi_type_float` | `float` |
| `f64` (`8B/8` with `__f64_tag`) | `size == 8` and has `__f64_tag` (`0B`) | `ffi_type_double` | `double` |
| 1/2/4/8-byte leaf or single-scalar tagged | `size ∈ {1,2,4,8}`, no float tag | `ffi_type_uint{8,16,32,64}` | `i{8,16,32,64}` / `ptr` |
| Composite struct (≥1 non-zero-sized fields, no float tag) | Non-empty non-tag fields | `ffi_type_struct` (skipping `0B` tags) | `%struct` matching `FieldEntry` offsets |

- **Strict By-Value Struct Layout Verification**: Passing/returning composite structs by value verifies `ffi_type.size == shape->size` and every field offset via `ffi_get_struct_offsets` against `FieldEntry::offset()`, failing fast on any mismatch.

---

## 5. Pure-Data `PrototypeObject` & `IntrinsicSpec` (`compiler/runtime/Prototype.h`)

`PrototypeObject` is a **pure-data closed sum type (`std::variant`)** with external exhaustive `std::visit` dispatch across all consumers:

```cpp
struct FfiBinding {
    std::string lib_name;      // "" = default process/libc, "m" = libm, ...
    std::string symbol_name;   // C symbol name
    uint64_t args_shape_addr;  // Canonical closed ShapeLayout*
    uint64_t ret_shape_addr;   // Canonical closed ShapeLayout*
    SymbolId cc;               // Calling convention symbol (e.g. "c")
};

struct FfiBindPtrSpec {
    SymbolId fn_slot;          // Slot name in #this holding raw 8B C fn ptr
    uint64_t args_shape_addr;
    uint64_t ret_shape_addr;
    SymbolId cc;
};

struct BoundFfiSpec {
    void* fn_ptr;              // Live runtime C function pointer (non-stageable)
    uint64_t args_shape_addr;
    uint64_t ret_shape_addr;
    SymbolId cc;
};

enum class IntBinKind   : uint8_t { Add, Sub, Mul, SDiv, UDiv, SRem, URem, BitAnd, BitOr, BitXor, Shl, AShr, LShr };
enum class IntUnaryKind : uint8_t { BitNot };
enum class FloatBinKind : uint8_t { Add, Sub, Mul, Div };
enum class CastKind     : uint8_t { SExt, ZExt, Trunc, FPExt, FPTrunc, SIToFP, UIToFP, FPToSI, FPToUI };
enum class CmpKind      : uint8_t { EqBytes, EqShape, SLt, ULt, SLe, ULe, FEq, FLt, FLe };

struct IntBinOp   { IntBinKind op;   uint8_t width; SymbolId lhs, rhs; };             // -> bytes(width)
struct IntUnaryOp { IntUnaryKind op; uint8_t width; SymbolId src; };                  // -> bytes(width)
struct FloatBinOp { FloatBinKind op; uint8_t width; SymbolId lhs, rhs; };             // -> bytes(width)
struct CastOp     { CastKind op;     uint8_t from_width, to_width; SymbolId src; };   // -> bytes(to_width)
struct CmpOp      { CmpKind op;      uint8_t width; SymbolId lhs, rhs; };             // -> bytes(1, 1)

using IntrinsicSpec = std::variant<IntBinOp, IntUnaryOp, FloatBinOp, CastOp, CmpOp>;

struct AstProto        { Ast::ExprPtr ast; };
struct NativeProto     { std::string extern_name; NativeFn fn; }; // Host-only (non-stageable)
struct FfiProto        { FfiBinding binding; };                   // Pure data (stageable)
struct FfiBindPtrProto { FfiBindPtrSpec spec; };                  // Pure data (stageable)
struct BoundFfiProto   { BoundFfiSpec spec; };                    // Runtime-bound (non-stageable)
struct IntrinsicProto  { IntrinsicSpec spec; };                   // Pure data (stageable)

struct PrototypeObject {
    std::variant<AstProto, NativeProto, FfiProto, FfiBindPtrProto, BoundFfiProto, IntrinsicProto> payload;
};
```

- `eq_shape` is promoted to a Prototype-Factory returning `IntrinsicProto{ CmpOp{ CmpKind::EqShape, 0, lhs, rhs } }` so `layer1.lang` emits 100% pure stageable `Ast` with zero `NativeProto` or `capabilities` leakage.
