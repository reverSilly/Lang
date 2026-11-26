# Lang

一门追求极简 Core、严格分层、零魔法与可自举的编程语言（Phase 1：C++23 解释器与自举前端；Phase 2 规划：LLVM AOT 编译与原生 FFI）。

## 核心设计理念

- **极简 Core**：核心仅包含 Layer 0 极简语法、4 种 AST 节点、21 个内置原语（Builtins）与两态槽生命周期（`Uninitialized` / `Live` vs 不存在）。
- **严格分层与零魔法**：标准库（`lib/std/`）与用户库完全同权；禁止为 Layer 1 语法便利污染 `Layer0.cpp` 或 Core 运行时。
- **可自举语法**：通过编译期能力（`capabilities.syntax` / `capabilities.ast`）在纯 `.lang` 代码中自举完整的 Layer 1 语法与类型系统。

## 目录结构

```text
compiler/           # C++23 核心实现
  ast/              # AST 定义与二进制序列化（AstCodec / PackFormat）
  parser/           # PEG 引擎与 Layer 0 解析器
  runtime/          # FrameStack、两层 FieldMap、Shape、Prototype、libffi 分发
  eval/             # Evaluator 与编译期/运行期 Capabilities
  tests/            # C++ 单元测试
lib/std/            # 纯 .lang 自举标准库
  boot/             # boot0 -> boot1 -> core_syntax 引导链
  syntax/           # Layer 1 语法模块（types / literals / ops / functions / tuples）
  start.lang        # 统一引导入口
  run.lang          # 目标程序隔离运行器（注入纯 stageable libc）
  build.lang        # 独立打包可执行文件构建器
  libc.lang         # 声明式标准 C FFI 模块
tests/              # 端到端与 Layer 1 测试套件
std/design/         # 冻结核心契约 CORE_SPEC.md 与归档历史设计稿（archive/）
editors/vscode/     # VS Code 语法高亮插件
```

## 构建与测试

依赖：支持 C++23 的编译器、`CMake >= 3.20`、`libffi` 开发头文件（如 `apt install libffi-dev`）。`cereal` 与 `Boost.PFR` 由 CMake 自动拉取。

```bash
# 1. 编译编译器与测试
cmake -S compiler -B build
cmake --build build -j

# 2. 运行全量 C++ 单测与 Layer 1 端到端测试
ctest --test-dir build --output-on-failure

# 3. 使用 Layer 1 运行目标程序
./build/lang lib/std/start.lang lib/std/run.lang tests/test_layer1.lang

# 4. 打包独立可执行文件
./build/lang lib/std/start.lang lib/std/build.lang tests/test_pack_exe.lang ./build/pack_exe
./build/pack_exe hello
```

## 工程约定与协作规范

为控制自举与语义演进阶段的复杂度，本仓库遵循以下约定：

1. **Prototype 优先，AST 仅作薄胶水**
   - 在 `lib/std/syntax/` 编写语法处理器时，凡是**纯运行时逻辑**（类型/Shape 分发、智能绑定分支、函数帧挂载、词法作用域查找等），一律用可读的 Layer 1 语法写成 `Prototype` (`^( ... )`) 并通过 `ast.value` 提升。
   - AST 节点构造仅保留最薄的一层胶水：把尚未求值的子表达式包成 `ast.proto`，或将编译期已知符号/参数绑入槽后 `#evaluate` 固定 Prototype。避免手写大段“AST 汇编”。
2. **单一事实来源 + 1 页变更单（Spec & RFC/ADR）**
   - Core 边界经逐行审计冻结后，全局只维护一份与 C++ 实现 100% 对齐的核心契约（[`std/design/CORE_SPEC.md`](std/design/CORE_SPEC.md)），旧设计稿归档至 `std/design/archive/`，不再多头同步修改。
   - 新特性（如 RAII、Pattern Matching）在动代码前先写 **1 页变更单**（目标、核心规则、正反边界用例、影响范围）；确认实现后归档为不可变决策记录（ADR）。
3. **垂直切片与拆分测试**
   - 每次只推进一个正交特性或重构任务；测试按特性拆分为独立短文件，保证单点改动可快速定位。
4. **GitHub 项目管理与 CI**
   - 动态任务与阶段进度统一使用 **GitHub Issues + Milestones** 管理（提交信息中使用 `Closes #N` 自动关联关单）。
   - 后续通过 **GitHub Actions CI** 在每次推送/PR 时自动运行全量回归测试。
