# 01 - 编译期与运行期：严格分层与阶段边界

前置：无（系列总纲）
后续：[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)、[08-syntax.md](08-syntax.md)、[09-syntax-capabilities.md](09-syntax-capabilities.md)、[10-ffi-and-capabilities.md](10-ffi-and-capabilities.md)

## 一、核心原则

1. **同一套内核求值语义**：编译期脚本与运行期目标程序共享同一套 Core 语义（21 个 `#` 内建原语、三态槽状态机、透明 `Shape` 与对称协程）。
2. **严格的编译期 / 目标运行期隔离**：不存在把编译器前端泄入运行期的“运行时 `import`”或隐式 `#compile` 魔法；构建脚本（编译期）与目标程序（运行期）通过**环境无关的 Standalone `Ast`** 严格解耦：
   - **编译期（Build Script）**：在挂载了 `capabilities`（Tier 1–3）的宿主 `RuntimeContext` 中运行，负责自举语法前端、读取并解析多文件源码、构造/拼装 Core IR（`Ast`）、实例化编译期常量与外部声明（`intrinsics`、`ffi`），最后将封闭的 `Ast` 交给后端。
   - **目标运行期（Target Program）**：在**完全孤立的运行环境**中执行（Phase 1 由 `capabilities.backends.interpreter.run` 创建全新的 `RuntimeContext` + `Evaluator`；Phase 2 由 `capabilities.backends.llvm.emit_executable` 生成独立原生可执行文件），**根帧不含 `capabilities`、`#parent(#this) = null`**。

## 二、`Ast`、`Prototype` 与后端的职责划分

| 概念 / 原语 | 语义 | 是否绑定 `RuntimeContext` |
| :--- | :--- | :---: |
| **`Ast`（Core IR）** | 唯一的环境无关程序表示（`WordExpr`、`BuiltinExpr`、`SequenceExpr`、`ValueExpr{ AstValue }`） | **否** |
| **`Prototype`** | 某个活运行环境内的可执行实体（`AstProto`、`IntrinsicProto`、`FfiProto`、`FfiBindPtrProto`、`BoundFfiProto`、`NativeProto`） | **是** |
| **`ast.to_proto({ ast })`** | 将 `Ast` **嵌入（stage 到）当前编译脚本的 `RuntimeContext`**，生成活 `AstProto` 供脚本自身 `#evaluate` / `#block` 执行 | **是** |
| **`ast.serialize` / `ast.deserialize`** | `Ast` ↔ `Bytes` 的无损二进制编解码，用于增量缓存、预编译模块与跨进程传递 IR（取代旧 `serialize_proto`） | **否** |
| **`backends.interpreter.run({ ast })`** | **Phase 1 解释器后端**：构造全新独立的 `RuntimeContext` + `Evaluator`，将 `Ast` 实例化并孤立执行 | 目标独立 |
| **`backends.llvm.emit_executable({ ast, ... })`** | **Phase 2 AOT 后端**：直接将同一棵 `Ast` 降级为 LLVM IR 与独立原生二进制 | 无 `RuntimeContext` |

* **多文件构建模型**：多文件项目由编译脚本驱动——纯目标模块全程保持为 `Ast`，按依赖顺序通过 `ast.seq` / `ast.builtin` 拼装成单一根 `Ast`（无需 `to_proto`）；仅需要在编译期借助 `capabilities` 计算导出值的模块（如 `lib/std/libc.lang`）先经 `ast.to_proto` + `#block` 求值，再将其纯可移植结果通过 `ast.value` 提升进目标 `Ast`。

## 三、阶段边界规则（Stageability / `ast.value`）

编译期求值结果跨阶段进入 Standalone `Ast`（`ast.value({ arena, val })`）时，语言强制执行一刀切的 **Stageability 边界**：

1. **允许跨越边界的值（8 种结构化 `AstValue`，零 `RuntimeContext` 依赖）**：
   - **`RawBytes`**：任意叶子位模式（`unit`、`raw1/2/4/8`、整数/浮点原始字节、以及字符串字面量 `"hello"` 等 `Shape{ size = N, align = 1 }` 纯字节值）；
   - **`SymbolVal`**：按名称保存的符号常量（延迟 intern）；
   - **`ShapeVal`**：纯值语义的 `AstShape` 布局树（无值仅布局）；
   - **`AstProtoVal`**：嵌套代码原型 `^Expr`（持有子 `ExprPtr`）；
   - **`IntrinsicVal`**：5 变体 LLVM 对齐硬件指令规格（槽名存 `std::string`）；
   - **`FfiExternVal`**：纯声明式外部链接符号规格（`{ lib, symbol, args, ret, cc }`）；
   - **`FfiBindPtrVal`**：二次实例化间接函数指针绑定器（`{ fn_slot, args, ret, cc }`）；
   - **`StructVal`**：闭合复合结构体常量 `{ size, align, vector<{ name, offset, AstValue }> }`（子字段 `Shape` 由 `AstValue` 自身推导，零重复）。
2. **严禁跨越边界的值（`ast.value` 直接拒绝报错）**：
   - **所有 `Pointer`**（Core 已移除 `#freeze`，不存在任何“可跨阶段的冻结指针”；字符串字面量直接按 `RawBytes` 值嵌入）；
   - **所有 `Handle`**（`AstArena*`、`FrontendArena*`、协程句柄等）；
   - **所有 `NativeProto`**（宿主编译器 C++ 函数指针，包括整个 `capabilities`）；
   - **所有运行期已绑定裸指针的 `BoundFfiProto`**（`ffi.bind_ptr` 在运行期二次实例化后的产物）；
   - 任何递归包含上述类型的结构体。
3. **单向性**：`compile-time → Ast → target runtime` 严格单向不可逆；运行期值绝不回流编译期。

## 四、与动态结构及可变性的正交性

- **位置可变性与阶段完全正交**：位置是否可原地覆写仅由 `#allocate(s, is_mut)` / `#allocate_val(v, is_mut)` 的 `is_mut`（`bytes(1, 1)`）决定；默认不可变（仅允许 `uninitialized → live` 单次写入）。
- **静态折叠作为优化机会**：类型/形状约束与泛型均以普通 `Shape` 值表达；编译期可折叠时解锁单态化、静态偏移与瘦指针退化（消去运行期 `shape_ptr` 与 `mutability`），折叠失败时自然退回完整胖指针动态寻址。
- **动态内存与动态 FFI 均显式化**：目标运行期不再存在隐藏的 `#freeze` 永生堆分配，也不保留 `capabilities`；运行期堆内存通过声明式 FFI 引入 `libc.malloc` / `libc.free` 管理，运行期动态加载插件库通过 `ffi.extern_symbol` 引入 `dlopen` / `dlsym` + `ffi.bind_ptr` 完成。

## 五、验收用例（自检清单）

1. **孤立执行验证**：通过 `backends.interpreter.run({ ast })` 运行目标程序时，目标根帧内 `#field(#this, capabilities)` 与 `#parent(#this)` 均为 `null` 指针，且使用独立 `RuntimeContext` + `Evaluator`。
2. **非法跨阶段值拦截**：试图将 `Pointer`、`Handle`、`NativeProto`（如 `capabilities`）或 `BoundFfiProto` 传入 `ast.value` 时静态抛错拒绝；8 种可移植 `AstValue`（含 `libc` 声明式符号与 `IntrinsicProto`）正常提升并在新 `RuntimeContext` 中无损 intern。
3. **多文件编译脚本拼装**：构建脚本用 `syntax.parse` 解析多个模块为 `Ast`，通过 `ast.seq` + `ast.value` 合并为单一封闭 `Ast` 并直接交由后端执行，全程无需 `to_proto`。
4. **纯字节字符串与零 `#freeze`**：`"hello"` 求值为 `Shape{ size = 5, align = 1 }` 的 `RawBytes` 值，可直接参与结构体构造与 FFI，全内核零 `#freeze` 原语。
