# 10. Stateless `capabilities`, Explicit Arena Handles & Foreign Function Interface (FFI)

This document specifies (1) the **100% stateless** three-tier modularization of `capabilities` with explicit, symmetric resource handles (`open_lib`/`close_lib`, `new_arena`/`destroy_arena`, `new_layer0`/`destroy_frontend`), (2) the decoupled `syntax.parse -> Ast Handle -> ast.to_proto -> Prototype` frontend pipeline, and (3) the `capabilities.ffi` design unifying dynamic external symbol loading in the Phase 1 interpreter (`dlopen`/`dlsym` + `libffi`) with Phase 2 LLVM AOT external linkage.

---

## 1. Stateless Three-Tier `capabilities` Architecture

`capabilities` holds **zero hidden host state** — there is no `CapabilityHost` / `xxxHost` singleton owning internal pools. Every capability primitive is a pure stateless `NativeProto` `Prototype` operating solely on `#this` and `RuntimeContext`.

### 1.1 Three-Tier Logical Split (Compile-Time vs. Runtime Boundary)

In Lang, `capabilities` remains mounted under root `#this.capabilities` with the same sub-namespace paths (`capabilities.constants`, `capabilities.ffi`, etc.). Architecturally, capabilities are partitioned into **three tiers** with explicit stateless installers:

| Tier | Sub-modules | Available at Compile-Time | Available at Runtime / AOT | Backing Mechanism |
| :--- | :--- | :---: | :---: | :--- |
| **1. Runtime Core** | `constants`, `field_map`, `symbols`, `intrinsics` | Yes | Yes | Interpreter C++ / `liblang_rt` + LLVM IR intrinsics |
| **2. Host FFI** | `ffi` (`open_lib`, `close_lib`, `load_symbol`) | Yes | Yes | Interpreter: `dlopen`/`dlsym`/`dlclose` + `libffi`; AOT: `declare external` + native linker |
| **3. Compiler Frontend** | `ast`, `syntax`, `compiler` *(+ temporary `io`)* | Yes | **No** | Compiler process only; stripped from runtime contexts and emitted executables |

- **Interpreter vs. AOT Runtime Semantics**:
  - In the **Phase 1 interpreter**, `install_runtime_capabilities(rt)` mounts Tier 1 only, `install_runtime_with_ffi(rt)` mounts Tier 1 + Tier 2, and `install_all_capabilities(rt)` mounts all three tiers.
  - In **Phase 2 AOT executables**, no monolithic `capabilities` struct is injected into the binary: `IntrinsicProto` and `FfiProto` `Prototype` values captured at compile time (e.g., `intrinsics.add_int(...)`, `ffi.load_symbol(...)`) are lowered directly into native LLVM instructions (`add i64`) or `declare external` calls, and `liblang_rt` is linked only for operations invoked dynamically at runtime (such as dynamic `field_map`/`symbols` reflection or runtime `dlopen`).
- **Retirement of Temporary `capabilities.io`**:
  - `capabilities.io.read_file` and `capabilities.io.write_file` are Phase 1 bootstrap placeholders. Immutable byte buffers returned by `io.read_file` and `syntax.cst_view_bytes` are allocated directly in `rt.shapes.freeze_bytes` (the same `.rodata` arena as `#freeze`), eliminating any separate byte-buffer pool.

### 1.2 Explicit Resource Lifecycles & Decoupled Frontend Pipeline

Every heap-allocated compiler or OS resource uses an explicit symmetric `new_*`/`destroy_*` (or `open_*`/`close_*`) handle pair — zero hidden GC, zero hidden RC, zero hidden host pools:

1. **AST Arena (`capabilities.ast`)**:
   - `new_arena() -> Handle`: Allocates an `AstArena` (backed by a pointer-stable `std::deque<Ast::ExprPtr>`) and returns its raw address as `Handle`.
   - `destroy_arena({ arena: Handle }) -> Unit`: Frees the `AstArena` immediately.
   - `word({ arena, sym })`, `builtin({ arena, kind, 0, 1, ... })`, `seq({ arena, 0, 1, ... })`, `value({ arena, val })`: Allocate Core IR nodes inside `#this.arena` and return `Handle` (`Ast::ExprPtr*`).
   - `to_proto({ ast: Handle }) -> Prototype`: Copies the underlying `Ast::ExprPtr` into `PrototypeArena`, so calling `destroy_arena(arena)` after `to_proto` leaves materialized `Prototype`s intact.
2. **Frontend / PEG Arena (`capabilities.syntax`)**:
   - `new_layer0() -> Handle`: Allocates a `Parser::Frontend` (which also owns a pointer-stable `std::deque<Peg::PegExprPtr>` for its PEG expressions) and returns its raw address as `Handle`.
   - `destroy_frontend({ fe: Handle }) -> Unit`: Frees the `Frontend`, its grammar, and its PEG expressions in one shot.
   - `lit({ fe, sym })`, `range({ fe, lo, hi })`, `ref({ fe, name })`, `seq({ fe, 0, ... })`, `choice({ fe, 0, ... })`, `repeat({ fe, body, min, max })`, `lookahead({ fe, body, positive })`, `get_rule({ fe, name })`: Allocate PEG handles inside `#this.fe`. (`any` and `eof` are immutable static singletons requiring no arena.)
   - `parse({ fe, src: BytePtr, arena: Handle }) -> Handle (Ast)`: Pure frontend step converting source bytes through the Packrat PEG grammar and rule handlers into a root Core IR `Ast` handle allocated in `arena`. When invoking CST rule handlers registered via `set_rule`, `parse` mounts both `node` (`&ctx` directly, zero CST pool) and `arena` into the handler's frame so `ast.*` and `cst_eval_child` work transparently.
3. **Backend (`capabilities.compiler`)**:
   - Reserved strictly for `Prototype`-level backend operations (`serialize_proto`, `deserialize_proto`, `emit_executable`).

```text
BytePtr (source) ──▶ syntax.parse({fe, src, arena}) ──▶ Ast Handle ──▶ [optional AST passes] ──▶ ast.to_proto ──▶ Prototype
```

### 1.3 C++ Implementation Split (`compiler/eval/`)

```text
compiler/eval/
  Capabilities.h          # Stateless free functions: install_runtime_capabilities / install_runtime_with_ffi / install_all_capabilities
  Capabilities.cpp        # Shared stateless helpers (read_field, read_bytes_ptr, build_struct, native_proto) + install_* orchestration
  RuntimeCaps.cpp         # Tier 1: constants, field_map, symbols, intrinsics
  FfiCaps.cpp             # Tier 2: ffi (open_lib, close_lib, load_symbol, Shape -> ffi_type mapper, struct layout verification)
  CompilerCaps.cpp        # Tier 3: ast (explicit AstArena), syntax (Frontend-owned PEG arena, syntax.parse), compiler (+ temporary io)
```

---

## 2. `capabilities.ffi` Specification

Lang's Core has **no function-call syntax and no separate C type system**. Because every Lang `Shape` is already a transparent, C-compatible memory layout (`size`, `align`, ordered `FieldEntry[]`), FFI reuses `Shape` directly as the ABI contract and `#evaluate` as the sole invocation mechanism.

### 2.1 Primitives (3 total)

1. **`capabilities.ffi.open_lib`**
   - **Input fields in `#this`**:
     - `path: Pointer` (immutable fat pointer to leaf `Shape{ size = N, align = 1 }` UTF-8 library path).
     - If `N == 0` (empty path `""`), opens the current process's global symbol table (`dlopen(nullptr, RTLD_LAZY)` on POSIX / `GetModuleHandleA(nullptr)` on Windows), making already-linked symbols such as `libc` immediately resolvable.
   - **Return**: `Handle` — the raw OS dynamic library handle (`void*` cast to `uint64_t`). Ownership belongs strictly to the caller (no hidden RC or handle table).
   - **Error**: Throws if the shared library cannot be loaded.

2. **`capabilities.ffi.close_lib`**
   - **Input fields in `#this`**:
     - `lib: Handle` — library handle returned by `open_lib`.
   - **Return**: `bytes(1, 1)` (`0x01` if `dlclose` / `FreeLibrary` succeeded, `0x00` otherwise).
   - **Semantics**: Directly releases the OS handle with zero hidden refcounting or dangling-handle tracking (mirroring manual C `dlclose`; Stdlib layers deterministic RAII via `__destroy`).

3. **`capabilities.ffi.load_symbol`**
   - **Input fields in `#this`**:
     - `lib: Handle` — library handle returned by `open_lib`.
     - `name: Pointer` (leaf `Shape{ size = N, align = 1 }`) — exact C symbol name (e.g., `"strlen"`, `"abs"`, `"strtod"`, `"div"`).
     - `abi: Abi` — a closed Lang struct describing the calling convention and parameter/return layouts:
       ```text
       Abi {
         args: Shape,   ! Closed Shape whose ordered fields define C parameters (unit_shape = 0 args) !
         ret:  Shape,   ! Closed Shape defining the C return type (unit_shape = void) !
         cc:   Symbol   ! Calling convention: `c` (platform default C ABI) !
       }
       ```
   - **Return**: `Prototype` (`FfiProto` variant), indistinguishable at the call site from any other `Prototype`.

### 2.2 Invocation Semantics (`#evaluate(native_proto)`)

Because Core has no function semantics, `#evaluate(p)` always executes in the caller's active `#this` frame. `abi.args` leverages the fact that `ShapeLayout.fields` is an **ordered array** of `FieldEntry[count]` carrying both field names (`sym`) and deterministic parameter order (`0 .. count-1`):

1. For each non-zero-sized `fe` in `args_shape.fields.entries[0 .. count-1]` (0-byte tag fields are skipped):
   - Look up `slot_ptr = #field(#this, fe.sym)` in the current `#this` frame.
   - Verify the slot exists, is `Live`, and satisfies `slot_ptr.shape_ptr == fe.sub_shape_addr`.
   - Record `arg_ptrs[i] = reinterpret_cast<void*>(slot_ptr.addr)`.
2. Invoke the resolved C function pointer with `arg_ptrs` under the precompiled `ffi_cif`.
3. Write the C return value into a `Value{ ret_shape_addr, bytes }` buffer (or `unit` if `ret_shape.size == 0`) and return it as the result of `#evaluate`.

**Key Properties**:
- **Named and Positional Calls Unified**:
  - Named ABI: `args = #shape({ fd := i32_0; buf := u64_0; count := u64_0 })` → invoke via `{ fd := 1; buf := p.addr; count := 5; #evaluate(c_write) }`.
  - Positional ABI: `args = #shape({ 0 := i32_0; 1 := u64_0; 2 := u64_0 })` → invoke via `{ 0 := 1; 1 := p.addr; 2 := 5; #evaluate(c_write) }`.
- **Zero Implicit Pointer Unwrapping**: A Lang fat `Pointer` is 24 bytes `{ addr, shape_ptr, mutability }`; a C pointer (`void*`, `const char*`) is an 8-byte raw address (`raw8_shape`). Callers explicitly pass `p.addr` (or wrap it in a Layer 1 helper), keeping ABI marshaling 100% transparent.

---

## 3. Unified `Shape` → C ABI Type Mapping & Struct Layout Verification

In hardware ABIs (SysV AMD64, AAPCS64, Win64), integer and floating-point values of the same size use different register classes (GPR vs. XMM/VFP), and C structs passed/returned by value follow platform register-classification rules.

Following the same zero-overhead convention used by `Symbol` (`__sym_tag: {}`), floating-point `Shape`s are identified by a zero-byte tag field:

| Lang `Shape` | Detection Rule | `libffi` Type (`Phase 1`) | LLVM Type (`Phase 2`) |
| :--- | :--- | :--- | :--- |
| `unit_shape` (`0B/1`, leaf) | `size == 0 && is_leaf()` | `ffi_type_void` *(return only; 0-arg `args` yields `nargs = 0`)* | `void` |
| `f32` (`4B/4` with `__f32_tag`) | `size == 4` and has field `__f32_tag` (`0B`) | `ffi_type_float` | `float` |
| `f64` (`8B/8` with `__f64_tag`) | `size == 8` and has field `__f64_tag` (`0B`) | `ffi_type_double` | `double` |
| 1-byte leaf / single-scalar tagged (`raw1`, Stdlib `Bool`) | `size == 1`, no float tag | `ffi_type_uint8` | `i8` |
| 2-byte leaf / single-scalar tagged | `size == 2`, no float tag | `ffi_type_uint16` | `i16` |
| 4-byte leaf / single-scalar tagged (`Symbol`, `raw4`) | `size == 4`, no float tag | `ffi_type_uint32` | `i32` |
| 8-byte leaf / single-scalar tagged (`raw8`, `Handle`, C ptr) | `size == 8`, no float tag | `ffi_type_uint64` | `i64` / `ptr` |
| Composite struct (`fields` with ≥1 non-zero-sized fields, no float tag) | Non-empty non-tag fields | `ffi_type_struct` (recursive `elements[]` skipping `0B` tag fields) | `%struct` (packed/aligned to match `FieldEntry` offsets) |

- **Strict By-Value Struct Layout Verification (`load_symbol`)**:
  - Passing arbitrary manually laid-out `Shape`s (packed, custom padding, over-aligned) **by pointer** (`p.addr` as `raw8_shape`) is always supported.
  - When passing or returning a composite struct **by value** through `libffi`, `load_symbol` verifies both total `ffi_type.size == shape->size` and every non-zero-sized field's offset via `ffi_get_struct_offsets(FFI_DEFAULT_ABI, st, offsets)` against `FieldEntry::offset()`. Any mismatch fails fast at `load_symbol` time rather than silently corrupting memory.

---

## 4. Two-Phase Unification: Interpreter vs. Phase 2 LLVM AOT

`PrototypeObject` is a **pure-data closed sum type (`std::variant`)** carrying no virtual dispatch or backend methods. Every consumer (Phase 1 `Evaluator`, Phase 2 `LlvmCodegen`, `serialize_proto`, debug dump) dispatches externally via `std::visit` with compile-time exhaustiveness checking:

```cpp
struct FfiBinding {
    std::string symbol_name;   // C symbol name
    uint64_t args_shape_addr;  // Canonical closed ShapeLayout*
    uint64_t ret_shape_addr;   // Canonical closed ShapeLayout*
    SymbolId cc;               // Calling convention symbol (e.g. "c")
};

enum class IntBinKind   : uint8_t { Add, Sub, Mul, SDiv, UDiv, SRem, URem, BitAnd, BitOr, BitXor, Shl, AShr, LShr };
enum class IntUnaryKind : uint8_t { BitNot };
enum class FloatBinKind : uint8_t { Add, Sub, Mul, Div };
enum class CastKind     : uint8_t { SExt, ZExt, Trunc, FPExt, FPTrunc, SIToFP, UIToFP, FPToSI, FPToUI };
enum class CmpKind      : uint8_t { EqBytes, SLt, ULt, SLe, ULe, FEq, FLt, FLe };

struct IntBinOp   { IntBinKind op;   uint8_t width; SymbolId lhs, rhs; };             // -> bytes(width)
struct IntUnaryOp { IntUnaryKind op; uint8_t width; SymbolId src; };                  // -> bytes(width)
struct FloatBinOp { FloatBinKind op; uint8_t width; SymbolId lhs, rhs; };             // -> bytes(width)
struct CastOp     { CastKind op;     uint8_t from_width, to_width; SymbolId src; };   // -> bytes(to_width)
struct CmpOp      { CmpKind op;      uint8_t width; SymbolId lhs, rhs; };             // -> bytes(1, 1)

using IntrinsicSpec = std::variant<IntBinOp, IntUnaryOp, FloatBinOp, CastOp, CmpOp>;

struct AstProto       { Ast::ExprPtr ast; };
struct NativeProto    { std::string extern_name; NativeFn fn; };
struct FfiProto       { FfiBinding binding; NativeFn fn; };
struct IntrinsicProto { IntrinsicSpec spec; }; // Pure POD data, zero std::function closure

struct PrototypeObject {
    std::variant<AstProto, NativeProto, FfiProto, IntrinsicProto> payload;
};
```

- **Phase 1 (Interpreter)**:
  - `Evaluator` uses `std::visit` on `PrototypeObject::payload`: `AstProto` runs the coroutine AST evaluator, `NativeProto` and `FfiProto` invoke `fn(*this)` (where `load_symbol` precompiles `ffi_cif` once into `FfiProto::fn`), and `IntrinsicProto` dispatches directly through C++23 width/type templates on `spec` with zero closure overhead.
- **Phase 2 (LLVM AOT & `serialize_proto`)**:
  - `serialize_proto` writes only the portable payload (`FfiBinding`, `IntrinsicSpec`, or serialized AST) — never raw pointers or OS library binaries.
  - `emit_executable` uses `std::visit` to lower `IntrinsicProto` directly into single LLVM IR instructions and `FfiProto` into `declare external` + native C ABI calls using the exact same `Shape -> llvm::Type` table in §3.
