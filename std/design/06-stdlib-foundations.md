# 06 - 标准库基石：分派器、RAII（`destroy`/`drop`）、求值块与模式匹配

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)

## 一、核心原则：内核极简原语 × 标准库高层语义

由于编译器内核（Core）完全剥离了 `\destroy` 钩子、RAII 栈展开、所有权移动（`move`）、交换（`swap`）、深拷贝（`clone`）与语法糖，标准库（Stdlib）利用 **`evaluate`（在调用方当前环境就地展开）**、**`shape.fields` 双向符号链表** 以及 **`%this -> %parent` 帧链导航**，在零内核黑魔法的前提下构建出高层语言机制。

## 二、`destroy` 与 `drop` 的解耦及基础包装器家族

### 2.1 为什么要解耦 `destroy(ptr)` 与 `drop(slot_ptr)`？

- 在递归销毁已闭合对象的内部子字段（如 `p.f`）、覆写已闭合对象的 `mutable` 子字段（`\p.f = new_f`）或弹出动态容器元素时，目标位置**不是当前构建期块的直接子槽**，它只需要**递归执行 `\destroy` 钩子释放外部资源**，而不需要（也不允许）调用内核 `%drop` 将已闭合对象内部留出 `empty` 空洞。
- 唯有针对**构建期块的直接子槽（`slot_ptr`）**执行销毁或移动时，才需要调用内核 **`%drop(slot_ptr)`** 将该直接子槽置为 `empty`（从而在块闭合时自动剥离，兼作 `unbind`）。
- **无 `F_drop` 内核特例**：由于 `\destroy` 已经完全移出内核、归入标准库范畴，`destroy(ptr)` 调用目标的 `\destroy` 直接遵循标准库子程序调用约定（无需内核内置 `F_drop` 析构帧特例）。

### 2.2 标准库基础包装器与分派器总表

| 标准库工具 / 算符 | 适用目标与前置条件 | 底层展开实现（全部基于 Core 原语） | 语义说明 |
| :--- | :--- | :--- | :--- |
| **`box(val) -> ptr`** | 任意 `value` | 在调用方当前块 `evaluate` 分配匿名直接子槽 `p`（`allocate`）、执行 `%write(p, val)` 并返回 `p` | 在当前块分配未绑定的不可变直接子槽、按位写入 `val` 并返回该槽指针 |
| **`pattern := rhs`** | 模式 `pattern` 与任意 `rhs` | 委托给模式匹配（见 §4.2）：当 `pattern` 为单个 `symbol` `x` 时，先求值 `rhs` 一次（存入未 `%bind` 的匿名暂存槽 `tmp` 以保持 `rhs` 若为未绑定直接子槽的身份），执行 `(%bind(x, box(%deref(tmp)))) if (~%bind(x, %deref(tmp)))` | 若 `rhs` 已是当前块未绑定的直接子槽 `ptr`（如 `allocate` 或 `{...}`），`%bind(x, ...)` 直接成功并返回 `true`（配合 AST 重写层下推目标槽实现 RVO 就地构造）；否则返回 `false`，经 `box` 装箱后再 `%bind`（故 `p := \a` 自然将已绑定指针 `\a` 装入新指针变量 `p`） |
| **`$ptr`** | 任意 `live` 槽指针 | 读取胖指针的 `s := ptr.shape_ptr`（即 `%shape(%deref(ptr))`），检查 `s` 是否为 POD（builtin POD 或不含 `\destroy` 且子字段皆 POD 的复合 shape）：若是则 `%deref(ptr)`，否则报错 | 面向用户的安全 POD 读取算子（支持自定义小结构体如 `Point` 直接按值读取与传参） |
| **`destroy(ptr)`** | **任意 `live` 指针**（含闭合对象子字段、容器元素、构建期槽） | 1. 若 `%field(ptr, destroy) ~= null`，按标准库协议调用 `ptr.\destroy`；<br>2. 取指针所指对象的形状 `s := ptr.shape_ptr`，从 `s.last_sym` 开始沿 `prev_sym` 逆序遍历每个子字段 `sym`，递归调用 `destroy(%field(ptr, sym))` | **纯钩子递归执行器**：先执行自身 `\destroy`，再按字段逆序执行子字段 `destroy`；**从不调用内核 `%drop`** |
| **`drop(slot_ptr)`** | **构建期直接子槽**（处于 `live` 或 `uninitialized`） | 若 `slot_ptr` 处于 `live` 则先调 `destroy(slot_ptr)`；随后调 **`%drop(slot_ptr)`** | 先递归执行整棵对象树的 `\destroy` 钩子，再调用内核 `%drop` 将该构建期直接子槽置为 `empty`（自动 unbind，闭合时剥离） |
| **`move(dst_ptr, src_slot)`** | **`dst_ptr` 必须可写**（处于 `uninitialized`、`mutable empty` 或 `mutable live`）；**`src_slot` 必须为构建期 `live` 直接子槽** | 1. 若 `dst_ptr` 已处于 `live`（必为 `mutable`），先调 `destroy(dst_ptr)` 释放旧资源；<br>2. 执行 `%write(dst_ptr, %deref(src_slot))`（注：此处内部直接使用无检查的 `%deref` 按位搬运整棵对象字节，绕过 `$` 的 POD 限制）；<br>3. 调 `%drop(src_slot)` 将源槽置为 `empty` | 将 `src_slot` 的内容转移至 `dst_ptr`（不触发 `src_slot` 的 `destroy`），并将源构建期槽置为 `empty` |
| **`dst_ptr = val`** | **`dst_ptr` 必须可写**（处于 `uninitialized`、`mutable empty` 或 `mutable live`；对 `immutable live` 报错） | 1. 若 `dst_ptr` 已处于 `live`（必为 `mutable`），先调 **`destroy(dst_ptr)`**（释放旧资源但不置 `empty`）；<br>2. 调 **`%write(dst_ptr, val)`** | 统一支持构建期首次单次初始化、构建期 `mutable` 槽覆写、以及**已闭合对象 `mutable` 子字段的原地覆写（`\p.f = val`）** |
| **`swap(ptr1, ptr2)`** | 任意两个同 `shape` 的**可写（`mutable`）`live` 指针** | 在内部使用无检查的 `%deref` 与 `%write` 经匿名暂存槽原子交换两者的字节内容 | 原子交换两个可写槽的内容（不触发 `destroy`，也不产生 `empty` 槽） |
| **`target @ rhs`** | `target` 为协程 `handle`，`rhs` 为任意表达式 | 若 `rhs` 已是当前块未绑定直接子槽指针 `src_ptr` 则直接传给 `%resume(target, src_ptr)`；否则经 `src_ptr := box(rhs)` 装入临时槽后调用 **`%resume(target, src_ptr)`**，返回本协程醒来时的 `\in` 指针 | 协程恢复与消息传递分派器（支持跨协程传递 POD 值或直接移动构建期非 POD 对象树） |
| **`clone(dst_ptr, src_ptr)`** | 可写目标槽与源指针 | 若为 POD 则 `dst_ptr = $src_ptr`；若含自定义 `\clone` 则调用之；否则按 `src_ptr.shape_ptr.fields` 链表正序递归 `clone` 各字段 | 显式深拷贝 |

## 三、块包装器（`{ ... }` vs `block1`）与用户态 RAII

因为内核的 `block` 和 `%break` 完全不处理 RAII，标准库通过以下包装器在用户态实现确定性析构：

### 3.1 `defer(action)`：挂入构建期符号链的守卫

- `defer(action)` 宏在当前块中构造一个携带 `\destroy := ^action` 的守卫对象，并使用编译器生成的块内唯一隐藏符号（如 `__defer_1`）调用 `%bind` 挂入当前构建块的 `shape.fields` 双向链表中。
- 因为它挂在当前块的 `shape.fields` 链中，在块退出时可通过遍历当前块 `shape.fields` 的 `last_sym -> prev_sym` 链精确按创建逆序找到并执行它。

### 3.2 对象构造块 `{ Expr }` vs 过程求值块 `block1`

1. **对象构造块 `{ Expr }`（及空块 `{}`）**：
   - 目标：构造并返回一个持久化复合对象（保留所有用户显式 `%bind` 的字段）；**空块 `{}` 直接展开为 `block ^()`**。
   - 展开逻辑：在 `block` 内部求值 `Expr`；在闭合前的最后一刻，仅对块内的隐藏临时守卫（`__defer_k`）按逆序调用 `drop(__defer_k)`（执行其 `\destroy` 并经 `%drop` 置为 `empty` 从而在闭合时被剥离），而所有用户 `%bind` 的 `live` 字段保留并压平为最终连续内存对象。
2. **过程求值块 `block1`（`block ^(break (evaluate expr))`）**：
   - 目标：提供局部词法作用域（用于普通函数体、局部代码块、循环体），求值一系列语句后**自动逆序销毁块内定义的所有局部变量与 `defer` 守卫，仅返回最后一个表达式的值（In-place Construct 到外层结果槽）**！
   - 展开逻辑：
     ```
     block1(proto) := block ^(break (evaluate proto))
     ```
     当 `evaluate proto` 算出尾表达式结果时，外层的标准库 `break` 立即触发：先逆序 `drop` 当前块中绑定的所有局部变量与 `defer` 守卫，再通过内核 `%break` 将尾表达式结果直接就地填入当前块的 `self root` 返回！

### 3.3 标准库 `break` 包装器与逐层清理退栈

- **单层块退出 `break expr`（缺省目标为当前块 `%this`）**：
  1. 先在当前环境求值 `expr`（若结果来自当前块即将销毁的直接子槽，则通过 `%deref` 取出字节并调 `%drop` 将该源槽置为 `empty`，避免后续重复析构）；
  2. 读取当前块形状 `s := (%this).shape_ptr`，从 `s.last_sym` 沿 `prev_sym` 逆序遍历当前块所有仍处于 `live` 的绑定槽（含局部变量与 `__defer_k`），依次调用 `drop(%field(%this, sym))`；
  3. 调用内核 **`%break(%this, val)`** 完成物理退栈与 `self root` 结果写入。
- **跨多层块退出 `break target_ptr expr`**：
  - 由于内核只提供零元原语 `%parent`（取当前活跃块的直接父块指针），当 `target_ptr ~= %this` 时，标准库通过**逐层清理并向 `%parent` 接力回退**实现跨多层栈展开（具体跨块接力协议细节留待与函数/异常传播一并定稿，见第五节待办清单）。

## 四、分支、循环与模式匹配

### 4.1 `if` 与 `while`（含双层块 `break` / `continue` 模式）

- **`expr1 if cond else expr2`**：直接重写为 `%branch(cond, ^expr1, ^expr2)`（无 `else` 时默认为 `^()`）。
- **`body while cond`**：通过 `evaluate` 与 `%branch` 在调用方当前环境透明迭代求值（`while` 自身不产生 `block`）。
- **双层块循环控制模式（零循环专用关键字表达 `break`、`continue` 与轮末 RAII）**：
  通过外层块（提供整个循环的 `break` 靶子 `outer`）与内层块（提供单轮作用域兼 `continue` 靶子 `inner`，或直接使用 `block1`）的组合，无需任何额外原语即可完整表达所有循环控制：
  ```
  loop_res := {
      outer := \this;
      {
          inner := \this;
          x := compute_step();

          !- 1. continue（跳过本轮剩余部分，清理本轮变量并进入下一轮）：break 到内层块 -!
          (break $\inner ()) if should_skip($\x);

          !- 2. break（携值提前退出整个循环）：break 到外层块 -!
          (break $\outer $\x) if is_done($\x);

          !- 3. 轮末清理：以 break () 结束内层块，当场逆序 drop 本轮所有局部变量 x -!
          break ()
      } while cond
  }
  ```

### 4.2 模式匹配 `match`、`:` 与 `:=`

**编译器内核不认识 Pattern。** 模式匹配完全由标准库纯函数与语法壳构成：

```
match(pattern, subject) -> Bindings | fail
```

1. **Pattern 的分派规则**：
   - **裸 `symbol` 值 `a`**：匹配任意 `subject` 并产出绑定项 `(a, subject)`；
   - **普通 `value`（如字面量 `3` 或变量读取 `$\a`）**：按值相等（`==`）匹配，无需内置 `Exactly` 关键字；
   - **带 `matcher` 字段的自定义 Pattern 对象**：调用 `pattern.matcher(subject)`（支持形状比较 `<` / `>` / `=`、组合子 `|` / `&`、守卫 `If` 及可逆算术匹配 `a + 1 : 3`）。
2. **`value : pattern`（可反驳约束）与 `pattern := rhs`**：
   - 求值 `subject` 与 `pattern`，调用 `match(pattern, subject)`；
   - 若返回 `fail`：立即执行 `break <fail>` 终止当前块；
   - 若返回 `Bindings`：标准库提交循环依次对每项 `(sym, val)` 在当前块调用 `%bind(sym, val)`（若返回 `false` 则调 `%bind(sym, box(val))`）将绑定落入当前帧。
3. **`v ? pattern`（纯查询匹配）**：
   - 调用 `match(pattern, v)`，成功返回 `true`，失败返回 `false`，不提交绑定、无控制流副作用。
4. **错误传播 `try` / `catch`**：
   - `fail` 是标准库约定的普通 `value`；`try` / `catch` 将受检表达式包裹在子块中求值，通过匹配 `fail` 形状实现错误接力传播或捕获恢复。

## 五、待办议题（Open Items — 下一步 Brainstorm 重点）

以下议题属于标准库上层协议，目前尚未最终定稿，作为下一步逐步推进的议程：

1. **函数与 Lambda 的统一调用协议**：
   - 如何利用 `block1` / `block`、`evaluate` 与模式匹配 `:` 定稿最简的 Lambda 字面量语法与函数调用传参协议？
   - **`\destroy` 的具体调用签名**：在 `destroy(ptr)` 中调用 `ptr.\destroy` 时，如何将 `ptr` 最自然地传递给 `\destroy`？
2. **跨多层块的 `break target_ptr val` 逐层退栈接力机制**：
   - 在仅有零元 `%parent` 的前提下，跨多层块 `break target_ptr val` 如何通过逐层返回或帧内接力槽自动触发途经各帧的 `cleanup`？
3. **词法自由变量查找与闭包捕获协议**：
   - 编译期宏如何将自由变量静态展开为 `%parent` 链导航，以及闭包对象 `{ env, code }` 的自动捕获（Copy / Move / `cell` 协程）如何定义？
4. **`:=` 与 `block` 就地构造（RVO）的 AST 重写细则**：
   - 在 `compile` 元层 AST 重写时，如何将 `x := { ... }` 直接合成为原地绑定以省去临时槽中转？

## 六、验收用例（自检清单）

1. `p := { a := 1; f := open("x") }; drop(\p)`：`drop(\p)` 先调 `destroy(\p)`（逆序执行 `\p.f` 的 `\destroy` 关闭文件，不调 `%drop(\p.f)`），最后调一次内核 `%drop(\p)` 将 `\p` 在当前块置为 `empty`。
2. `\p.f = open("y")`：对已闭合对象 `p` 的 `mutable` 字段 `\p.f` 赋值时，`=` 先检验 `\p.f` 可写，调 `destroy(\p.f)` 关闭旧文件，再调 `%write` 将新文件写入 `\p.f`，全程 `\p.f` 不经过 `empty` 状态。
3. `res := block1 ^( a := open("x"); b := 41; $\b + 1 )`：`block1` 展开为 `block ^(break (evaluate ...))`，算出 `42` 后通过标准库 `break` 自动逆序 `drop(\b)` 与 `drop(\a)`（关闭文件 `"x"`），最终 `res` 直接获得 `42`。
