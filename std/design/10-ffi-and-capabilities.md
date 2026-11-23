# 10. `capabilities` Modularization, Frontend Pipeline Fix & Foreign Function Interface (FFI)

This document specifies (1) the three-tier modularization of `capabilities` to separate **compile-time** and **runtime** boundaries, (2) the decoupled `syntax.parse -> Ast Handle -> ast.to_proto -> Prototype` frontend pipeline, and (3) the `capabilities.ffi` design unifying dynamic external symbol loading in the Phase 1 interpreter (`dlopen`/`dlsym` + `libffi`) with Phase 2 LLVM AOT external linkage.

---

## 1. Why & How to Split `capabilities`

As Lang grows beyond Phase 1 bootstrap, keeping all host capabilities in a single flat bundle blurs the boundary between compile-time metaprogramming and runtime execution, and turns `Capabilities.cpp` into a monolith.

### 1.1 Three-Tier Logical Split (Compile-Time vs. Runtime Boundary)

In Lang, `capabilities` remains mounted under root `#this.capabilities` with the same sub-namespace paths (`capabilities.constants`, `capabilities.ffi`, etc.). Architecturally, however, capabilities are partitioned into **three distinct tiers** with explicit installation gates:

| Tier | Sub-modules | Available at Compile-Time | Available at Runtime / AOT | Backing Mechanism |
| :--- | :--- | :---: | :---: | :--- |
| **1. Runtime Core** | `constants`, `field_map`, `symbols`, `intrinsics` | Yes | Yes | Interpreter C++ / `liblang_rt` + LLVM IR intrinsics |
| **2. Host FFI** | `ffi` (`open_lib`, `load_symbol`) | Yes | Yes | Interpreter: `dlopen`/`dlsym` + `libffi`; AOT: `declare external` + native linker |
| **3. Compiler Frontend** | `ast`, `syntax`, `compiler` *(+ temporary `io`)* | Yes | **No** | Compiler process only; stripped from runtime contexts and emitted executables |

- **Interpreter vs. AOT Runtime Semantics**:
  - In the **Phase 1 interpreter**, Tier 1 and Tier 2 are mounted in root `#this.capabilities` so pure runtime code can execute without Tier 3 (`ast`, `syntax`, `compiler`).
  - In **Phase 2 AOT executables**, no monolithic `capabilities` struct is injected into the binary: `NativeSym` `Prototype` values captured at compile time (e.g., `intrinsics.add_i64`, `ffi.load_symbol(...)`) are lowered directly into native LLVM instructions (`add i64`) or `declare external` calls, and `liblang_rt` is linked only for operations invoked dynamically at runtime (such as dynamic `field_map`/`symbols` reflection or runtime `dlopen`).
- **Retirement of Temporary `capabilities.io`**:
  - `capabilities.io.read_file` and `capabilities.io.write_file` were Phase 1 bootstrap placeholders. Once `capabilities.ffi` is live and Layer 1 stdlib wraps libc/OS file calls, `capabilities.io` will be removed from C++.

### 1.2 Decoupled Frontend Pipeline (`syntax.parse` → `Ast` → `ast.to_proto`)

Superseding `09-syntax-capabilities.md` §5 where `capabilities.compiler.parse` fused parsing and `Prototype` materialization:

- **`capabilities.syntax.parse({ fe, src: BytePtr }) -> Handle (Ast)`**: Pure frontend step converting source bytes through the Packrat PEG grammar and rule handlers into a root Core IR `Ast` handle.
- **`capabilities.ast.to_proto({ ast: Handle }) -> Prototype`**: Explicit materialization of an `Ast` handle into a live `Prototype`, leaving the `Ast` layer exposed for inspection, macro passes, or IR transforms before evaluation/compilation.
- **`capabilities.compiler`**: Strictly `Prototype`-level backend operations (`serialize_proto`, `deserialize_proto`, `emit_executable`).

```text
BytePtr (source) ──▶ syntax.parse ──▶ Ast Handle ──▶ [optional AST passes] ──▶ ast.to_proto ──▶ Prototype
```

### 1.3 C++ Implementation Split (`compiler/eval/`)

Instead of one monolithic `Capabilities.cpp`, split the implementation along the exact same three tiers (all linked into `lang_core`):

```text
compiler/eval/
  Capabilities.h          # CapabilityHost facade: install_runtime / install_ffi / install_compiler / install_all
  Capabilities.cpp        # Shared helpers (handle encoding, read_field, build_struct) + install_* orchestration
  RuntimeCaps.cpp         # constants, field_map, symbols, intrinsics
  FfiCaps.cpp             # ffi (dlopen/dlsym, Shape -> ffi_type mapper, ffi_cif cache, ffi_call bridge)
  CompilerCaps.cpp        # ast, syntax (including syntax.parse), compiler (+ temporary io)
```

---

## 2. `capabilities.ffi` Specification

Lang's Core has **no function-call syntax and no separate C type system**. Because every Lang `Shape` is already a transparent, C-compatible memory layout (`size`, `align`, ordered `FieldEntry[]`), FFI reuses `Shape` directly as the ABI contract and `#evaluate` as the sole invocation mechanism.

### 2.1 Primitives (2 total)

1. **`capabilities.ffi.open_lib`**
   - **Input fields in `#this`**:
     - `path: Pointer` (immutable fat pointer to leaf `Shape{ size = N, align = 1 }` UTF-8 library path).
     - If `N == 0` (empty path `""`), opens the current process's global symbol table (`dlopen(nullptr, RTLD_LAZY)` on POSIX), making already-linked symbols such as `libc` immediately resolvable.
   - **Return**: `Handle` (opaque library handle owned by `CapabilityHost`).
   - **Error**: Throws if the shared library cannot be loaded (`dlerror()`).

2. **`capabilities.ffi.load_symbol`**
   - **Input fields in `#this`**:
     - `lib: Handle` — library handle returned by `open_lib`.
     - `name: Pointer` (leaf `Shape{ size = N, align = 1 }`) — exact C symbol name (e.g., `"write"`, `"puts"`, `"cos"`).
     - `abi: Abi` — a closed Lang struct describing the calling convention and parameter/return layouts:
       ```text
       Abi {
         args: Shape,   ! Closed Shape whose ordered fields define C parameters !
         ret:  Shape,   ! Closed Shape defining the C return type (unit_shape = void) !
         cc:   Symbol   ! Calling convention: `c` (platform default C ABI) !
       }
       ```
   - **Return**: `Prototype` (`CodeKind::NativeSym`), indistinguishable at the call site from any other `Prototype`.

### 2.2 Invocation Semantics (`#evaluate(native_proto)`)

Because Core has no function semantics, `#evaluate(p)` always executes in the caller's active `#this` frame. `abi.args` leverages the fact that `ShapeLayout.fields` is an **ordered array** of `FieldEntry[count]` carrying both field names (`sym`) and deterministic parameter order (`0 .. count-1`):

1. For each `i` in `0 .. args_shape.fields.count - 1`:
   - Let `fe = args_shape.fields.entries[i]`.
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

## 3. Unified `Shape` → C ABI Type Mapping (`libffi` & LLVM)

In hardware ABIs (SysV AMD64, AAPCS64, Win64), integer and floating-point values of the same size use different register classes (GPR vs. XMM/VFP), and C structs follow platform register-classification rules.

Following the same zero-overhead convention used by `Symbol` (`__sym_tag: {}`) and `Boolean` (`__bool_tag: {}`), floating-point `Shape`s are identified by a zero-byte tag field:

| Lang `Shape` | Detection Rule | `libffi` Type (`Phase 1`) | LLVM Type (`Phase 2`) |
| :--- | :--- | :--- | :--- |
| `unit_shape` (`0B/1`, leaf) | `size == 0 && is_leaf()` | `ffi_type_void` *(return only)* | `void` |
| `f32` (`4B/4` with `__f32_tag`) | `size == 4` and has field `__f32_tag` (`0B`) | `ffi_type_float` | `float` |
| `f64` (`8B/8` with `__f64_tag`) | `size == 8` and has field `__f64_tag` (`0B`) | `ffi_type_double` | `double` |
| 1-byte leaf / single-scalar tagged (`Boolean`, `raw1`) | `size == 1`, no float tag | `ffi_type_uint8` | `i8` |
| 2-byte leaf / single-scalar tagged | `size == 2`, no float tag | `ffi_type_uint16` | `i16` |
| 4-byte leaf / single-scalar tagged (`Symbol`, `raw4`) | `size == 4`, no float tag | `ffi_type_uint32` | `i32` |
| 8-byte leaf / single-scalar tagged (`raw8`, `Handle`, C ptr) | `size == 8`, no float tag | `ffi_type_uint64` | `i64` / `ptr` |
| Composite struct (`fields` with ≥1 non-zero-sized fields, no float tag) | Non-empty non-tag fields | `ffi_type_struct` (recursive `elements[]` skipping `0B` tag fields) | `%struct` (packed/aligned to match `FieldEntry` offsets) |

*Note*: Zero-byte tag fields (`size == 0`, such as `__sym_tag`, `__f64_tag`, `__ptr_tag`) occupy 0 bytes in Lang and are ignored when building `ffi_type_struct.elements`, so a tagged scalar like `{ __raw: raw8, __f64_tag: {} }` maps cleanly to `ffi_type_double` rather than a struct.

---

## 4. Two-Phase Unification: Interpreter vs. Phase 2 LLVM AOT

`PrototypeObject` (`CodeKind::NativeSym`) stores both the **portable ABI contract** and the **phase-specific execution handle**:

```cpp
struct FfiBinding {
    std::string lib_path;      // "" for process-global / default libc
    std::string symbol_name;   // C symbol name
    uint64_t args_shape_addr;  // Canonical closed ShapeLayout*
    uint64_t ret_shape_addr;   // Canonical closed ShapeLayout*
    SymbolId cc;               // Calling convention symbol (e.g. "c")
    // Phase 1 interpreter cache (not serialized):
    void* fn_ptr = nullptr;
    // Cached libffi ffi_cif + owned ffi_type descriptors
};
```

- **Phase 1 (Interpreter)**:
  - `load_symbol` resolves `fn_ptr = dlsym(lib, name)`, compiles `ffi_prep_cif` once, and caches it inside `PrototypeObject`.
  - `#evaluate` executes `ffi_call` with zero per-call descriptor allocation.
- **Phase 2 (LLVM AOT & `serialize_proto`)**:
  - `serialize_proto` writes **only** `{ lib_path, symbol_name, args_shape, ret_shape, cc }` — never raw pointers or OS library binaries.
  - `emit_executable` lowers each `NativeSym` `Prototype` into an LLVM `declare external` function using the exact same `Shape -> llvm::Type` table in §3, emits direct native C ABI calls, and passes `-l<lib>` to the system linker.
  - If the compile-time interpreter itself is later replaced by LLVM ORC JIT, the `libffi` backend in `FfiCaps.cpp` can be swapped out with zero changes to Lang code or `FfiBinding`.
