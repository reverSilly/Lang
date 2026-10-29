# 06 - 标准库基石：三层销毁切分、单次求值 `:=`、多层 `break` 接力与模式匹配

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)

## 一、核心原则：Core 彻底零 RAII 化 × 标准库接管高层协议

通过将 **三层销毁职责（Stdlib `destroy` / Core `%drop` / Stdlib `drop`）** 与 **单层 `%break(src_ptr)` + 构建栈 `%parent` 逐步上溯** 结合，编译器内核（Core）彻底剥离了所有与 RAII（`\destroy`）及多层栈帧展开相关的逻辑，标准库在用户态以零黑魔法实现完整的生命周期与控制流管理。

## 二、三层销毁职责切分与基础包装器家族

### 2.1 三层销毁职责的严格切分

| 层级与函数 | 作用对象 | 核心职责 | 典型场景 |
| :--- | :--- | :--- | :--- |
| **Stdlib `destroy(ptr)`** | **任意 `pointer`**（含已闭合对象内部、或 `bytes(N)` 经 `%recast` 重解释出的元素指针） | 1. 若 `ptr.shape_ptr` 含 `\destroy` 字段（`%field(z_ptr, destroy) ~= null`），按普通标准库调用约定传入 `ptr` 执行它；<br>2. 顺着 `s := ptr.shape_ptr` 的 `last_sym -> %prev_sym(s, sym)` 逆序遍历所有字段，递归调用 `destroy(%field(ptr, sym))` | `DynamicArray.pop_back()` 销毁末尾元素；容器扩容/缩容销毁缓冲区内对象；被 `drop(ptr)` 调用 |
| **Core `%drop(ptr)`** | **构建期帧（Constructing Block）的直接子槽 `pointer`** | 要求 `ptr` 下无未清空的构建期子槽，仅负责将该槽在当前构建帧中的状态标记为 `empty`（闭合时由 `block` 静默剥离） | 释放构建期局部变量槽位、取消字段导出 |
| **Stdlib `drop(ptr)`** | **构建期帧的直接子槽 `pointer`** | 先调用 `destroy(ptr)` 完成递归析构，再调用 Core `%drop(ptr)` 将槽位置为 `empty` | 局部资源提前销毁、`=` / `move` 覆写非空 `mutable` 构建期槽前的清理 |

- **带来的核心好处**：
  1. **Core 完全不知道 `\destroy` 的存在**：`\destroy` 不再需要是内核特殊对待的裸 `prototype`，也不需要内核在 `%drop` 时为其构造特殊的 `%parent` 临时帧。它就是一个再普通不过的标准库函数/方法约定（接收 `ptr` 作为参数），在里面拥有完整的标准库调用与词法环境。
  2. **`DynamicArray` 等底层容器一等公民化**：容器管理的是 `bytes(N)` 缓冲区而非构建帧的直接子槽，`pop_back` 时只需对元素胖指针调用 `destroy(elem_ptr)` 即可完整执行析构，无需也不必调用 `%drop`，location 生命周期由 `DynamicArray` 自身管理。

### 2.2 `x := rhs` 的单次求值展开闭环

当 `x := rhs` 右侧是任意表达式时，若简单展开为 `(%bind(x, box(rhs))) if (~%bind(x, rhs))`，会导致 `rhs` 被求值两次。利用 **“一切表达式结果皆为 POD `value`”** + **“块闭合时静默剥离 `empty` 绑定”** 这两条公理，标准库用纯现有原语写出严格只求值 `rhs` 一次的 `:=` 展开：

```
!- x := rhs 的标准库就地展开（evaluate） -!
%bind(__tmp, allocate);        !- 1. 分配临时槽并绑定到卫生符号 __tmp -!
%write(\__tmp, rhs);           !- 2. rhs 严格只求值一次，结果 value（含指针值）按位写入 __tmp -!
(%bind(x, box($\__tmp)))       !- 3. 若 $\__tmp 是当前块未绑定子槽指针则直接绑定到 x； -!
    if (~%bind(x, $\__tmp));   !-    否则经 box 装入新槽再绑定到 x -!
%drop(\__tmp)                  !- 4. 将 __tmp 置为 empty；当前块闭合时自动静默剥离 __tmp！ -!
```

因为 `allocate` 或 `{ ... }` 返回的 `pointer` 本身也是 POD `value`，将它暂存入 `\__tmp` 不会改变目标槽“未绑定直接子槽”的身份；而在最后 `%drop(\__tmp)` 将临时槽置为 `empty` 后，块闭合相变会自动将 `__tmp` 从最终对象的 `shape` 中抹去，不留任何痕迹。

### 2.3 标准库基础包装器与分派器总表

| 标准库工具 / 算符 | 适用目标与前置条件 | 底层展开实现（全部基于 Core 原语） | 语义说明 |
| :--- | :--- | :--- | :--- |
| **`box(val) -> ptr`** | 任意 `value` | 在调用方当前块 `evaluate`：分配匿名直接子槽 `p`（`allocate`）、执行 `%write(p, val)` 并返回 `p` | 在当前块分配未绑定的不可变直接子槽、按位写入 `val` 并返回该槽指针 |
| **`x := rhs`** | 符号 `x` 与任意 `rhs` | 见 §2.2 的 `__tmp` 暂存展开 | `rhs` 严格求值一次；若为未绑定直接子槽则直接绑定，否则 `box` 装箱后绑定 |
| **`$ptr`** | 任意 `live` 槽指针 | 读取胖指针的 `s := ptr.shape_ptr`，通过 `base_addr = 0` 的 `%field` 与 `%next_sym` 检查 `s` 是否为 POD（builtin POD 或不含 `\destroy` 且子字段皆 POD 的复合 shape）：若是则 `%deref(ptr)`，否则报错 | 面向用户的安全 POD 读取算子（支持自定义小结构体如 `Point` 直接按值读取与传参） |
| **`move(dst_ptr, src_slot)`** | `dst_ptr` 可写；`src_slot` 为活跃构建帧链（`%this` 或 `%parent`）的 `live` 直接子槽 | 1. 若 `dst_ptr` 为构建期空槽（`uninitialized` 或 `mutable empty`），直接调 **`%move(dst_ptr, src_slot)`**；<br>2. 若 `dst_ptr` 已处于 `live`（必为 `mutable`）：若 `dst_ptr` 为构建期直接子槽，先调 `drop(dst_ptr)` 置空再调 `%move(dst_ptr, src_slot)`；若 `dst_ptr` 为已闭合对象的 `mutable` 子字段，直接调 **`%swap(dst_ptr, src_slot)`** 再调 **`drop(src_slot)`**！ | 统一支持向空槽移动、覆写构建期可变槽、以及覆写已闭合对象的 `mutable` 子字段（全程保持闭合对象无 `empty` 空洞） |
| **`dst_ptr = val`** | `dst_ptr` 可写（`uninitialized`、`mutable empty` 或 `mutable live`） | 1. 若 `dst_ptr` 为构建期空槽，直接调 **`%write(dst_ptr, val)`**；<br>2. 若 `dst_ptr` 已处于 `live`（必为 `mutable`）：在当前块通过 `tmp := box(val)` 构造新值槽，调 **`%swap(dst_ptr, tmp)`** 原地交换，再调 **`drop(tmp)`** 销毁换出的旧值！ | 统一用 Copy-and-Swap 完成已初始化 `mutable` 槽（含闭合对象子字段 `\p.f = val`）的安全覆写，零空洞、异常安全 |
| **`target @ rhs`** | `target` 为协程 `handle`，`rhs` 为任意表达式 | 若 `rhs` 已是当前块未绑定直接子槽指针 `src_ptr` 则直接传给 `%resume(target, src_ptr)`；否则经 `src_ptr := box(rhs)` 装入临时槽后调用 **`%resume(target, src_ptr)`**，返回本协程醒来时的 `\in` 指针 | 协程恢复与消息传递分派器 |
| **`clone(dst_ptr, src_ptr)`** | 可写目标槽与源指针 | 若为 POD 则 `dst_ptr = $src_ptr`；若含自定义 `\clone` 则调用之；否则用 `%next_sym` 遍历 `src_ptr.shape_ptr` 各字段递归 `clone` | 显式深拷贝 |

## 三、块包装器、构建栈父链与多层 `break` 接力展开

### 3.1 构建期隐式父链槽 `__parent` 与 `defer(action)` 守卫

1. **构建期隐式父链槽 `__parent`**：
   - 每个由标准库 `{ ... }`（或 `block1`、函数块）开启的构建帧，在进入块时自动在首部执行 `__parent := %parent`；
   - 块内代码可通过 `%parent`（1 步）、`$($(\__parent).__parent)`（2 步）等链式寻址确定性访问任意外层活跃构建帧（例如跨双层 `while` 块访问外层函数局部变量）；
   - 当块准备正常闭合或通过 `break` 退出时，标准库包裹层调用 `%drop(\__parent)` 将其置为 `empty`，闭合时自动静默剥离，在最终闭合对象中**占 0 字节**。
2. **`defer(action)`：捕获 `%parent` 的守卫对象**：
   - 在块 `B` 内写 `defer(action)` 时，`action` 往往需要访问块 `B` 的局部变量。由于守卫对象是在块 `B` 内部构建的，在构建该守卫对象的那一刻，它的 `%parent` 恰好就是块 `B`！
   - 因此 `defer(action)` 宏只需在当前块 `B` 中构造守卫对象并绑定到隐藏符号 `__defer_k`：
     ```
     __defer_k := { env := %parent; \destroy := ^(... 通过 \in.env 访问块 B 的变量 ...) }; ()
     ```
   - 当块 `B` 闭合或被 `break` 退出触发 `drop(\__defer_k)` 时，标准库调用其 `\destroy` 并传入守卫对象自身指针，通过守卫自身的 `env` 字段即可无损访问块 `B` 的全部状态，无需增加任何额外内核原语。

### 3.2 对象构造块 `{ Expr }` vs 过程求值块 `block1`

1. **对象构造块 `{ Expr }`（及空块 `{}`）**：
   - 空块 `{}` 直接展开为 `block ^()`；
   - 非空块 `{ Expr }` 在内部首部绑定 `__parent := %parent`，求值 `Expr`；在闭合前的最后一刻，对所有隐藏守卫 `__defer_k` 按逆序调用 `drop(\__defer_k)`，并调用 `%drop(\__parent)`，使所有用户显式 `%bind` 的 `live` 字段压平为最终连续内存对象。
2. **过程求值块 `block1`（`block ^(break (evaluate expr))`）**：
   - 展开为 `block ^(break (evaluate proto))`：当 `evaluate proto` 算出尾表达式结果时，外层的标准库 `break` 立即触发，逆序 `drop` 当前块中绑定的所有局部变量与 `__defer_k` 守卫、置空 `__parent`，再通过内核单层 `%break(val_slot)` 将尾表达式结果移入当前块的 `self root` 返回。

### 3.3 单层 `%break(src_ptr)` 与标准库多层 `break target_ptr val`（及 `return`）接力

由于内核 `%break(src_ptr)` 只退出当前最内层构建帧 `%this`，标准库通过**展开信封（Unwind Envelope）**与外层 `{ ... }` 宏配合实现跨任意多层块的 `break target_ptr val`（以及 `return val`）：

1. **当目标 `target_ptr == %this`（单层退出）**：
   - 先将返回值存入当前块的未绑定结果槽 `val_slot`（若返回值来自当前块局部变量则 `%move` 入 `val_slot`）；
   - 顺着 `(%this).shape_ptr` 的 `last_sym -> %prev_sym` 逆序遍历当前块除 `val_slot` 外的所有 `live` 绑定槽（含局部变量与 `__defer_k`），依次调用 `drop`，并 `%drop(\__parent)`；
   - 最后调用内核 **`%break(val_slot)`** 弹出当前帧。
2. **当目标 `target_ptr ~= %this`（跨层跳出外层循环或函数 `return`）**：
   - 将目标帧指针与载荷打包为标准库的展开信封对象：`env_slot := { __unwind_target := target_ptr; __payload := val }`；
   - 按单层退出流程清理本帧其余全部 `live` 槽后，调用 **`%break(env_slot)`** 退回上一层父帧；
   - 外层 `{ ... }` / `block1` 宏在子块返回后检查返回值：若检测到返回值匹配展开信封且 `__unwind_target ~= %this`，则自动清理自身局部槽并继续向下一层 `%break` 接力；若 `__unwind_target == %this`，则拆开信封取出 `__payload` 作为本块最终结果完成退出！

## 四、分支、循环与模式匹配

### 4.1 `if` 与 `while`（含双层块 `break` / `continue` 模式）

- **`expr1 if cond else expr2`**：直接重写为 `%branch(cond, ^expr1, ^expr2)`（无 `else` 时默认为 `^()`）。
- **`body while cond`**：通过 `evaluate` 与 `%branch` 在调用方当前环境透明迭代求值（`while` 自身不产生 `block`）。
- **双层块循环控制模式**：
  ```
  loop_res := {
      outer := \this;
      {
          inner := \this;
          x := compute_step();

          !- 1. continue（跳过本轮剩余部分，清理本轮变量并进入下一轮）：break 到内层块 -!
          (break $\inner ()) if should_skip($\x);

          !- 2. break（携值提前退出整个循环，经展开信封接力退至外层块）：break 到外层块 -!
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
   - 若返回 `Bindings`：标准库提交循环依次对每项 `(sym, val)` 按 §2.2 的单次求值绑定逻辑将绑定落入当前帧。
3. **`v ? pattern`（纯查询匹配）**：
   - 调用 `match(pattern, v)`，成功返回 `true`，失败返回 `false`，不提交绑定、无控制流副作用。
4. **错误传播 `try` / `catch`**：
   - `fail` 是标准库约定的普通 `value`；`try` / `catch` 将受检表达式包裹在子块中求值，通过匹配 `fail` 形状实现错误接力传播或捕获恢复。

## 五、待办议题（Open Items — 下一步 Brainstorm 重点）

1. **基础数据类型及其操作的实现边界（Core 原语 vs Blackmagic vs `compile` Capability + Stdlib）**：
   - 核心问题（来自 Talk 4 结尾）：那几个基础数据类型（如算术类型的 `+`、`-`、`*`、`/`、`%`、位运算与比较）应该怎么处理？
   - 候选权衡：
     - 若全部用内核原语实现，是否会导致 Core 原语过于杂乱？
     - 若交给标准库实现：由标准库从 `compile` capability 获得对应底层操作的 `prototype`，固化（reify）到运行期后直接分派；同时需要一并界定 `compile` capability 的最小接口形态。
2. **函数与 Lambda 的统一调用协议及闭包捕获细则**：
   - 如何利用 `block1` / `block`、`evaluate` 与模式匹配 `:` 定稿最简的 Lambda 字面量语法与函数调用传参协议（含 `\destroy(ptr)` 的标准调用形式）？
   - 闭包对象 `{ env, code }` 的自动捕获（Copy / Move / `cell` 协程）如何通过编译期宏与 `%parent` 链静态合成？

## 六、验收用例（自检清单）

1. **三层销毁与容器解耦**：`DynamicArray.pop_back()` 对缓冲区中经 `%recast` 取得的非 POD 元素指针调用 `destroy(elem_ptr)` 触发其 `\destroy`，无需调用 `%drop`；而块内局部变量 `drop(\p)` 则依次执行 `destroy(\p)` 与 `%drop(\p)`。
2. **`x := rhs` 单次求值验证**：`x := side_effect_fn()` 展开为 `__tmp` 暂存序列，`side_effect_fn()` 严格只执行一次，且块闭合后 `__tmp` 被静默剥离，不在闭合对象 `shape` 中留下任何字段。
3. **多层 `break` 展开信封接力**：在内层循环执行 `break $\outer 42` 时，内层块逆序 `drop` 自身局部变量后通过单层 `%break` 返回展开信封，外层块识别目标为自身后拆封得到 `42`。
