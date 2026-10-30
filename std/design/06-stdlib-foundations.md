# 06 - 标准库基石：三层销毁切分、`move`/`swap` 导出、单次求值 `:=` 与多层 `break` 接力

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)
后续：[07-types-and-intrinsics.md](07-types-and-intrinsics.md)

## 一、核心原则：Core 彻底零 RAII / 零 POD 化 × 标准库接管高层协议

通过将 **三层销毁职责（Stdlib `destroy` / Core `%drop` / Stdlib `drop`）**、**无条件按位读写（Core `%deref` / `%write`）** 与 **单层 `%break(src_ptr)` + `%unbound` 匿名槽游标** 结合，编译器内核（Core）不仅彻底剥离了所有与 RAII（`\destroy`）、POD 判定及多层栈帧展开相关的逻辑，还直接消去了 `%move` 与 `%swap` 原语——标准库在用户态以零黑魔法实现完整的移动、交换、赋值、重绑（Rebind）与控制流管理。

## 二、三层销毁职责切分与 `move` / `swap` / `=` 标准库导出

### 2.1 三层销毁职责的严格切分

| 层级与函数 | 作用对象 | 核心职责 | 典型场景 |
| :--- | :--- | :--- | :--- |
| **Stdlib `destroy(ptr)`** | **任意 `live` `Pointer`**（含已闭合对象内部可变字段、或 `bytes(N, A)` 经标准库 `recast` 重解释出的元素指针） | 1. 若 `ptr.shape_ptr` 含 `\destroy` 字段（`%field(z_ptr, destroy) ~= null`），按普通标准库调用约定传入 `ptr` 执行它；<br>2. 顺着 `s := ptr.shape_ptr` 的 `last_sym -> %prev_sym(s, sym)` 逆序遍历所有 `live` 字段，递归调用 `destroy(%field(ptr, sym))` | `DynamicArray.pop_back()` 销毁末尾元素；`\p.f = val` 覆写已闭合对象可变字段前就地析构旧值；被 `drop(ptr)` 调用 |
| **Core `%drop(ptr)`** | **构建期帧（`%this` 或 `%parent`）的直接子槽 `Pointer`** | 要求 `ptr` 下无未清空子槽，仅负责将该槽状态标记为 `empty` 并从 `first_sym..last_sym` 或 `%unbound` 链中摘除（闭合时由 `block` 静默剥离） | 释放构建期局部变量槽位、取消字段导出、在 `move` 与 `swap` 中零 RAII 释放源槽/临时槽 |
| **Stdlib `drop(ptr)`** | **构建期帧的 `live` 直接子槽 `Pointer`** | 先调用 `destroy(ptr)` 完成递归析构，再调用 Core `%drop(ptr)` 将槽位置为 `empty` | 局部资源提前销毁、块退出逆序清理、未闭合块同名变量跨类型 Rebind 前的旧槽清理 |

### 2.2 为什么 `move` 与 `swap` 不再需要是 Core 原语？

因为 Core 的 **`%deref(ptr)`** 是无条件按位读取（不检查 POD、不触发 `\destroy`），Core 的 **`%write(dst_ptr, val)`** 允许向任何 `uninitialized` 槽或任何 `mutable` 位置（含已闭合对象的 `mutable live` 字段）直接按位写入，且 Core 的 **`%drop(slot_ptr)`** 是零 RAII 的状态置空原语：

1. **标准库实现 `swap(p1, p2)`（仅需 4 行 Core 原语）**：
   ```
   !- Stdlib swap(p1, p2)：要求 p1 与 p2 均为 mutable live 且 shape_ptr 相同 -!
   tmp := %allocate_val(%deref(p1));  !- 1. 无条件按位读出 p1 字节暂存入当前块匿名槽 tmp -!
   %write(p1, %deref(p2));            !- 2. 将 p2 字节直接按位覆写进 p1（p1 保持 live 无空洞） -!
   %write(p2, %deref(tmp));           !- 3. 将 tmp 中暂存的原 p1 字节直接按位覆写进 p2 -!
   %drop(tmp)                         !- 4. 零 RAII 直接置空 tmp（绝不触发 \destroy）！ -!
   ```
2. **标准库实现 `move(src_slot) -> Pointer`（移入当前块新槽）与 `move_into(dst_ptr, src_slot)`（移入目标位置）**：
   ```
   !- 1. 移入当前块新槽：move [mutable] src_slot -> Pointer -!
   dst_ptr := %allocate [mutable] (target_shape(src_slot));
   %write(dst_ptr, %deref(src_slot));
   %drop(src_slot);
   dst_ptr

   !- 2. 移入任意可写目标位置 dst_ptr（含 uninitialized 槽、mutable 槽、或已闭合对象的 mutable 字段 \p.f） -!
   (destroy(dst_ptr)) if is_live(dst_ptr);  !- 若目标已 live（如 \p.f），先就地析构旧值（不调 %drop，零空洞） -!
   %write(dst_ptr, %deref(src_slot));       !- 将 src_slot 字节直接按位写入 dst_ptr（转为/保持 live） -!
   %drop(src_slot)                          !- 零 RAII 将源构建槽 src_slot 置为 empty！ -!
   ```
   *(注：甚至当向已闭合对象的可变字段 `\p.f` 移入一个 `immutable` 局部槽 `\src` 时，上述 `move_into` 也直接生效，无需像旧版 `%swap` 那样要求 `\src` 也是 `mutable`！)*

### 2.3 `x := rhs` 的单次求值展开与跨类型 Rebind

利用 **`%allocate_val`**（即原 `box`：求值一次并直接分配 `live` 真槽）：

```
!- x := rhs 的标准库就地展开（evaluate） -!
%bind(__tmp, %allocate_val(rhs));          !- 1. rhs 严格只求值一次，结果 value 装入临时槽并绑定到 __tmp -!
(%bind(x, %allocate_val($\__tmp)))         !- 2. 若 $\__tmp 是当前块未绑定子槽指针（如 {...} 或 %allocate_val），直接从 %unbound 绑定到 x； -!
    if (~%bind(x, $\__tmp));               !-    否则经 %allocate_val 装入新槽再绑定到 x（若 x 原绑定已 empty，直接完成跨 Shape 重绑！） -!
%drop(\__tmp)                              !- 3. 零 RAII 将 __tmp 置为 empty（块闭合时由内核静默剥离） -!
```

### 2.4 标准库基础包装器与分派器总表

| 标准库工具 / 算符 | 适用目标与前置条件 | 底层展开实现（全部基于 Core 原语） | 语义说明 |
| :--- | :--- | :--- | :--- |
| **`is_pod(s_ptr) -> Boolean`** | 任意 `Pointer(Shape)` | 若 `%field(z_ptr, destroy) ~= null` 返回 `false`；否则沿 `first_sym -> %next_sym` 递归检查所有子字段形状（当 `first_sym == null_sym` 时循环 0 次直接返回 `true`） | 以 `first_sym == null_sym` 为天然递归终点的纯标准库 POD 判定函数 |
| **`x := rhs`** | 符号 `x` 与任意 `rhs` | 见 §2.3 的 `__tmp` 暂存展开（若未闭合块中欲重绑已存在的 `x`，先调 `drop(\x)` 再执行 `x := rhs`） | `rhs` 严格求值一次；支持未绑定子槽直绑、普通值装箱绑定及 `empty` 后的跨类型 Rebind |
| **`recast(ptr, s_ptr)`** | 任意 `Pointer` 与目标 `Shape` 指针 | 将 `ptr` 装入临时槽 `\p`、目标形状指针装入 `\s`，构造并返回 `$({ addr := $\p.addr; shape_ptr := $\s.addr; mutability := $\p.mutability; __ptr_tag := {} })` | 零内核原语实现胖指针形状重解释（`Union` 与容器元素访问的核心工具） |
| **`$ptr`** | 任意 `live` 槽指针 | 调用 `is_pod(target_shape(ptr))`：若为 POD 则调用 `%deref(ptr)` 返回 `value`，否则报错拒绝 | 面向用户的安全 POD 读取算子 |
| **`dst_ptr = val`** | `dst_ptr` 为 `uninitialized` 子槽或 `mutable` 位置 | 1. 先求值 `val`；若 `is_pod(%shape(val))` 为假且无自定义拷贝则报错；<br>2. 若 `dst_ptr` 已处于 `live`（必为 `mutable`）：先调 `destroy(dst_ptr)` 就地析构旧值；<br>3. 若为 POD 直接调 **`%write(dst_ptr, val)`** 原地写入；否则调用自定义 copy 钩子 | 统一完成 `uninitialized` 初始化与 `mutable live`（含闭合对象字段 `\p.f = val`）的原地覆写 |
| **`move` / `swap`** | 见 §2.2 | 基于 `%allocate` / `%allocate_val` + `%deref` + `%write` + `%drop` 组合 | 100% 标准库实现，零 Core 原语开销 |
| **`target @ rhs`** | `target` 为协程 `Handle`，`rhs` 为任意表达式 | 若 `rhs` 已是当前块未绑定直接子槽指针 `src_ptr` 则直接传给 `%resume(target, src_ptr)`；否则经 `src_ptr := %allocate_val(rhs)` 装入临时槽后调用 **`%resume(target, src_ptr)`** | 协程恢复与消息传递分派器 |

## 三、块包装器、构建栈父链与多层 `break` 接力展开

### 3.1 构建期隐式父链槽 `__parent` 与 `defer(action)` 守卫

1. **构建期隐式父链槽 `__parent`**：
   - 每个由标准库 `{ ... }`（或 `block1`、函数块）开启的构建帧，在进入块时自动在首部执行 `__parent := %parent`；
   - 块内代码可通过 `%parent`（1 步）、`$($(\__parent).__parent)`（2 步）等链式寻址确定性访问任意外层活跃构建帧；
   - 当块准备正常闭合或通过 `break` 退出时，标准库包裹层调用 `%drop(\__parent)` 将其置为 `empty`，闭合时由内核静默剥离，在最终闭合对象中**占 0 字节**。
2. **`defer(action)`：捕获 `%parent` 的守卫对象**：
   - 在块 `B` 内写 `defer(action)` 时，守卫对象在块 `B` 内部构建，其 `%parent` 恰好就是块 `B`：
     ```
     __defer_k := { env := %parent; \destroy := ^(... 通过 \in.env 访问块 B 的变量 ...) }; ()
     ```
   - 当块 `B` 闭合或被 `break` 退出触发 `drop(\__defer_k)` 时，标准库调用其 `\destroy` 即可无损访问块 `B` 的全部状态。

### 3.2 对象构造块 `{ Expr }` vs 过程求值块 `block1`（含 `%unbound` 与 `uninit`/`empty` 清理闭环）

1. **对象构造块 `{ Expr }`（及空块 `{}`）**：
   - 空块 `{}` 直接展开为 `block ^()`；
   - 非空块 `{ Expr }` 在内部首部绑定 `__parent := %parent`，求值 `Expr`；
   - **闭合前清理**：
     1. 循环调用 **`(drop(%unbound)) while (%unbound ~= null)`**，按 LIFO 逆序析构并置空块内求值期间产生的所有未命名 `live` 临时槽（如 `a := get_fd(open("tmp.txt"))` 中的临时 `File` 对象）；
     2. 对所有隐藏守卫 `__defer_k` 按逆序调用 `drop(\__defer_k)`，并调用 `%drop(\__parent)`；
     3. 内核 `block` 闭合逻辑自动剥离所有 `uninitialized` 与 `empty` 槽，仅将剩余处于 `live` 状态的具名绑定字段压平为最终连续内存对象。
2. **过程求值块 `block1`（`block ^(break (evaluate expr))`）**：
   - 展开为 `block ^(break (evaluate proto))`：当 `evaluate proto` 算出尾表达式结果时，外层的标准库 `break` 立即触发，先通过 `%unbound` 逆序 `drop` 所有未命名临时槽，再顺着 `last_sym -> %prev_sym` 逆序 `drop` 当前块中所有处于 `live` 的具名局部变量与 `__defer_k` 守卫（由于 `first_sym..last_sym` 仅含 `live` 槽，天然跳过 `uninitialized` 与 `empty` 槽），最后通过内核单层 `%break(val_slot)` 将尾表达式结果移入当前块的 `self root` 返回。

### 3.3 单层 `%break(src_ptr)` 与标准库多层 `break target_ptr val`（及 `return`）接力

由于内核 `%break(src_ptr)` 只退出当前最内层构建帧 `%this`，标准库通过**展开信封（Unwind Envelope）**与外层 `{ ... }` 宏配合实现跨任意多层块的 `break target_ptr val`（以及 `return val`）：

1. **当目标 `target_ptr == %this`（单层退出）**：
   - 先将返回值存入当前块的未绑定结果槽 `val_slot`（若返回值来自当前块局部变量则 `move` 入 `val_slot`）；
   - 通过 `%unbound` 逆序 `drop` 除 `val_slot` 外的匿名临时槽，并顺着 `(%this).shape_ptr` 的 `last_sym -> %prev_sym` 逆序遍历当前块所有 `live` 具名绑定槽依次调用 `drop`；
   - 最后调用内核 **`%break(val_slot)`**（由内核自动剥离剩余 `uninitialized`/`empty` 槽）弹出当前帧。
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
   - **裸 `Symbol` 值 `a`**：匹配任意 `subject` 并产出绑定项 `(a, subject)`；
   - **普通 `value`（如字面量 `3` 或变量读取 `$\a`）**：按值相等（`==`）匹配，无需内置 `Exactly` 关键字；
   - **带 `matcher` 字段的自定义 Pattern 对象**：调用 `pattern.matcher(subject)`（支持形状比较 `<` / `>` / `=`、组合子 `|` / `&`、守卫 `If` 及可逆算术匹配 `a + 1 : 3`）。
2. **`value : pattern`（可反驳约束）与 `pattern := rhs`**：
   - 求值 `subject` 与 `pattern`，调用 `match(pattern, subject)`；
   - 若返回 `fail`：立即执行 `break <fail>` 终止当前块；
   - 若返回 `Bindings`：标准库提交循环依次对每项 `(sym, val)` 按 §2.3 的单次求值绑定逻辑将绑定落入当前帧。
3. **`v ? pattern`（纯查询匹配）**：
   - 调用 `match(pattern, v)`，成功返回 `true`，失败返回 `false`，不提交绑定、无控制流副作用。
4. **错误传播 `try` / `catch`**：
   - `fail` 是标准库约定的普通 `value`；`try` / `catch` 将受检表达式包裹在子块中求值，通过匹配 `fail` 形状实现错误接力传播或捕获恢复。

## 五、待办议题（Open Items — 下一步 Brainstorm 重点）

1. **函数与 Lambda 的统一调用协议及闭包捕获细则**：
   - 如何利用 `block1` / `block`、`evaluate` 与模式匹配 `:` 定稿最简的 Lambda 字面量语法与函数调用传参协议（含 `\destroy(ptr)` 与自定义拷贝钩子的标准调用形式）？
   - 闭包对象 `{ env, code }` 的自动捕获（Copy / Move / `cell` 协程）如何通过编译期宏与 `%parent` 链静态合成？

## 六、验收用例（自检清单）

1. **标准库实现 `move` 与 `swap`**：`swap(p1, p2)` 与 `move(dst, src)` 完全由 `%allocate`/`%allocate_val`、`%deref`、`%write` 与 `%drop` 组合而成，无需 `%move` 或 `%swap` 内核原语。
2. **`%unbound` 与 `first_sym..last_sym` 精确清理**：对象构造块 `{ a := get_fd(open("tmp.txt")); b := 2 }` 闭合前通过 `%unbound` 逆序 `drop` 匿名临时 `File` 对象，而 `a`、`b` 保留为闭合对象字段；`block1` 与 `break` 则通过 `%unbound` + `last_sym -> %prev_sym` 逆序 `drop` 全部 `live` 槽，未初始化的 `uninitialized` 槽不在游标链中，由内核直接安全剥离。
3. **多层 `break` 展开信封接力**：在内层循环执行 `break $\outer 42` 时，内层块逆序 `drop` 自身 `live` 变量后通过单层 `%break` 返回展开信封，外层块识别目标为自身后拆封得到 `42`。
