# 11 - Phase 2 静态分析与 LLVM AOT 路线备忘（Draft / TODO）

前置：[01-comptime.md](01-comptime.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)、[10-ffi-and-capabilities.md](10-ffi-and-capabilities.md)

> **状态**：设计讨论备忘（Phase 1 期间不实现，留待 Phase 2 启动时细化）。

---

## 一、核心原则

1. **后端完全基于 Layer 0（Core）**：
   - `capabilities.backends.llvm` 属于 Core 能力，输入是环境无关的 Standalone `Ast`（4 种节点 + 21 个 Core Builtin）。
   - **绝不假设任何 Layer 1 约定**（如函数调用协议 `->`、标准库类型包装规则等）；静态分析与代码生成必须直接建立在 Layer 0 原语语义之上。
2. **避免“仅展开解释器循环”的伪 AOT**：
   - 若直接把 `#bind` / `#field` / `#deref` / `#shape` 翻译为调 C++ `RuntimeContext` 胶水函数，本质上与 Phase 1 的 `interpreter.emit_executable` 架构雷同。
   - Phase 2 的核心价值在于**通过全程序静态分析证明局部无动态逃逸**，从而把动态 `FieldMap` 消除为固定结构体偏移、把胖指针退化为瘦指针、把 `eq_shape` 常量分支坍缩为单条 LLVM 指令。

---

## 二、为什么放弃“单遍在线部分求值”，转向声明式不动点分析

单纯从上到下的单遍在线部分求值（Online Partial Evaluation）在 Lang 上有两个根本短板：
1. **副作用与控制流汇合**：运行期 `#branch` / 循环内可能执行 `#bind`、`#drop` 或改写父帧槽位，需要跨分支/跨循环的不动点收敛，单遍遍历无法准确判断出口帧状态。
2. **运行期多态 `Prototype`**：`Prototype` 可能是运行期值（如来自 `#branch` 或可变槽位），且存在 `AstProto`、`FfiProto`、`FfiBindPtrProto`、`BoundFfiProto`、`IntrinsicProto` 等多种变体，不能靠简单内联解决，必须先求出每个 `#evaluate` 点可能流到的候选 `Prototype` 集合。

---

## 三、候选技术路线：声明式求解器（Datalog / E-Graph）+ 薄层 LLVM Codegen

将 Phase 2 拆为两个职责清晰的小模块：

1. **静态分析引擎（优先评估 [egglog](https://github.com/egraphs-good/egglog) / [Soufflé](https://souffle-lang.github.io/)）**：
   - 把 Standalone `Ast` 导出为关系事实（`Bind`、`Drop`、`Field`、`Deref`、`Write`、`Branch`、`Block`、`Evaluate`、`Shape` 等）。
   - 用声明式规则自动求全程序不动点，回答以下关键优化判定：
     - **逃逸 / 静态帧判定**：某个 `#block` 创建的帧指针是否从未流向动态键 `#field(p, dyn_sym)` → 成立则彻底消除该帧的动态 `FieldMap`，降级为静态 `alloca` / 固定结构体偏移。
     - **指向分析（Points-to）与 `Prototype` 候选集**：每个 `#evaluate(p)` 可能收到的 `Prototype` 集合；集合大小为 1 时直接静态特化/内联，小集合生成 `switch` 分派，未知集合退回统一 thunk 调用。
     - **`Shape` 流向与常量折叠**：推断每个值的可能 `Shape` 集合，配合 `eq_shape` 消除死分支与 ZST 零开销包装（由 LLVM `mem2reg` + `SROA` 收尾）。
2. **薄层 LLVM IR 生成**：
   - 消费求解器产出的静态事实，将已证明静态的部分直接 emit 为零运行期开销的 LLVM IR（`IntrinsicVal` → 单条硬件指令，`FfiExternVal` → `declare external` + 原生 C ABI `call`）。
   - 无法静态消解的局部按需退回最小动态运行时；协程 `#new` / `#resume` 评估直接映射 `llvm.coro.*`。
3. **SMT 求解器（[Z3](https://github.com/z3prover/z3) / [Formulog](https://arxiv.org/html/2408.14017v3)）定位**：
   - 不作为主力数据流优化器，保留用于未来可选的细化类型检查、路径互斥死代码证明或越界验证。
