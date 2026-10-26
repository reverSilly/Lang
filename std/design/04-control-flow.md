# 04 - 控制流、循环与错误

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)
后续：[05-ownership.md](05-ownership.md)

## 一、核心原则

语言内核**只有两个局部/块间控制流原语**（跨协程控制流由 [02-evaluation.md](02-evaluation.md) 的 `%resume` 承担）：

1. **`break [handle] value`**：非局部终止，沿当前协程帧链弹出至目标 `block`，清理途经帧并将结果写入目标 `block` 的 `self root`。
2. **`%branch(cond, thenProto, elseProto)`**：纯值二路分支，根据布尔条件选择其中一个 `prototype` 在**当前环境（`evaluate`）**透明求值。

语言内核**没有** `return`、`throw/try/catch`、循环帧、`label`、`continue`、`defer` 原语，内核也不认识“错误”概念；以上全部由标准库宏与协议导出。

## 二、`break` 与 `\this`

```
break value
break handle value
```

- **目标帧定位**：缺省目标为当前最内层 `block`；显式目标 `handle` 是通过 `\this` 获得的**帧 handle 值**（可作为普通 `value` 复制、传参或被嵌套原型值捕获）。目标必须是当前协程调用链上的活跃祖先帧；向非祖先或已结束的过期帧 handle 执行 `break` 触发运行时错误（静态可证时编译期拒绝）。
- **清理与结果替换**：
  1. 若 `value` 是从即将被销毁的帧子树中移出的 `location`，先将其从原树摘下；
  2. 从当前帧逐层向上弹出至目标 `block`：途中穿过的每一个非目标 `block` 执行完整析构；目标 `block` 自身已有的字段与未绑定临时量同样按逆序析构（[05-ownership.md](05-ownership.md)）；
  3. 将 `value` 填入目标 `block` 的 `self root` location，目标 `block` 求值即刻完成。
- **与协程根帧焊接**：
  若 `break` 弹出至本协程的根帧，则本协程求值结束（状态转为 `dead`），`break` 的值通过 `%resume` 通道移入恢复者 `\^` 的 `\in` 槽并恢复 `\^`。**协程内非局部跳转 = `break`，协程间跳转 = `%resume`（`@`），两者在根帧无缝焊接。**
- **`return` 宏派生**：内核无函数边界（函数体只是一个 `block`）；`return v` 宏在函数体入口捕获当前体帧的 `\this` handle，在任意深层嵌套块中展开为 `break <fn_handle> v`。

## 三、清理协议（RAII）、`;` 与 `defer`

### 3.1 相变清理与 `;` 的纯粹性

- **析构集中在 Block 相变时刻**：
  对象可携带保留字段 `\destroy`（析构钩子）。析构只在以下时刻触发：
  1. **`block` 正常闭合相变（构建中的帧 → 完整对象）**：块内所有**未经 `%bind` 命名**的直接子 `location`（临时量与 `defer` 守卫）按创建逆序销毁；已 `%bind` 命名的直接子 `location` 作为对象字段随 `self root` 存活。
  2. **`block` 被 `break` 终止或对象被 `%drop` / 覆写**：块内所有未绑定临时量与已绑定字段全部按创建逆序销毁（详见 [05-ownership.md](05-ownership.md)）。
- **`;` 是纯粹的序列算子**：
  `a; b` 从左到右求值 `a` 和 `b`，直接返回 `b` 的结果。`;` **只丢弃前序表达式的结果引用，绝不触发任何析构操作**，零额外检查开销。
- **无析构临时槽的物理复用**：
  对于未被 `%bind` 绑定、且无可观测析构副作用（无 `\destroy` 钩子，或已处于 `empty` / 仅含纯 `value`）的临时 `location`，一旦其结果被 `;` 丢弃，编译器可在 block 结束前的任意时刻直接回收或原地复用其物理栈槽。

### 3.2 `defer` 的零原语派生

- `defer(action)` 宏只需在当前 `block` 中 `allocate` 一个携带 `\destroy = ^action` 的**未命名（不经过 `%bind`）**守卫对象。
- 因为 `;` 不触发析构，该未命名守卫对象会一直存活到当前 `block` 正常闭合或被 `break` 穿过时，在“构建中的帧 → 对象”相变阶段按逆序自动执行 `\destroy`，且不会污染最终生成的对象字段。

## 四、分支与循环

### 4.1 `%branch` 与 `if` / `match`

- **`%branch(cond, thenProto, elseProto)` 是透明的**：被选中的 `prototype` 通过 `evaluate` 在**当前 env** 直接执行，**不分配新的结果 location，不切换 env**，求值结果按原本的 Kind（`value` 或 `location`）原样透传。
- **`if` 宏**：`expr1 if cond else expr2` 直接展开为 `%branch(cond, ^expr1, ^expr2)`；无 `else` 时默认分支为 `^()`。
- **`match` 分派宏**：对每个分支调用纯函数 `match(pattern, subject)`，失败则通过 `%branch` 进入下一分支，成功则在分支块内执行提交循环（`allocate` + copy + `%bind`）并求值分支体。

### 4.2 循环 `while` 与双层块控制模式

- **`while` 是透明宏/原型（不产生 Block）**：
  `body while cond` 经由 `evaluate` 与 `%branch` 在调用方的当前 env 中透明迭代求值——**`while` 自身不分配 `self root`、不切换 env、不建立符号作用域，也不做隐式清理**。
- **用嵌套 `{}` 表达局部作用域、`break` 与 `continue`**：
  无需任何循环专用关键字，通过外层 `{}`（循环整体块）与内层 `{}`（单轮迭代块）的组合即可完整表达所有循环控制与内存回收模式：

```
!- 标准双层块循环模式 -!
loop_res := {
    outer := \this;
    {
        inner := \this;
        x := compute_step();

        !- 1. continue（跳过本轮剩余部分，进入下一轮）：break 到内层块 -!
        (break $\inner ()) if should_skip($\x);

        !- 2. break（携值提前退出整个循环）：break 到外层块 -!
        (break $\outer $\x) if is_done($\x);

        !- 3. 轮末清理：以 break () 结束内层块，当场销毁本轮所有绑定字段 x -!
        break ()
    } while cond
}
```

- **单轮内存生命周期的三种清晰写法**：
  1. **内层块以 `break ()` 结束（推荐用于过程式循环体）**：`break ()` 触发内层块清理，当场逆序析构本轮所有局部变量（含带 `\destroy` 的资源）与 `defer` 守卫，内层 `self root` 仅剩 `()` 值，其物理栈槽由编译器在每轮原地复用；
  2. **显式 `%drop({ body }) while cond`**：若内层块正常闭合构造了一个对象，外层用 `%drop` 显式在每轮末尾销毁该未绑定对象结果，下一轮复用物理槽；
  3. **不包 `{}` 的 `(allocate ...) while cond`**：因为 `while` 透明，分配直接落在外层 `block` 下，存活至外层 `block` 结束。

## 五、错误与传播

**内核无错误概念。** `fail` 是标准库约定的普通 `value`（携带冻结的调用栈快照，不持有活帧 handle 或外部资源）：
- **产生**：`value : pattern` 可反驳匹配失败时，向当前 block 执行 `break <fail>`（仅弹出当前 block 一级，绝不隐式跨多层接力）。
- **传播（`try` 宏）**：将受检表达式包裹在子 `block` 中求值，若结果为 `fail` 形状则继续向调用方帧 `break` 接力，否则提取正常结果。
- **捕获（`catch` 宏）**：在子 `block` 求值结果上匹配 `fail` 形状并转入错误处理分支。
- **丢弃**：`fail` 作为普通 `value` 被 `;` 丢弃在内核层面合法；未处理的 `fail` 可由静态类型检查或 lint 工具给出警告。若程序入口 `main` 最终返回 `fail`，运行时打印其冻结栈回溯并退出。

## 六、验收用例（自检清单）

1. `r := { a := 1; break ($\a + 41) }`：`break` 销毁块内字段 `a`，将 `42` 写入 `self root`，`$\r` 得 `42`。
2. `defer` 守卫作为未 `%bind` 的临时 location，跨过多条 `;` 语句保持存活，在所在 `block` 正常结束或被 `break` 穿过时精确逆序调用 `\destroy`。
3. `%branch` 经 `evaluate` 透明执行选中分支，不额外产生嵌套的 `self root` location。
4. 双层块 `{{ ...; break () } while cond}` 中：`break inner ()` 实现 `continue`，`break outer val` 实现退出循环，轮末 `break ()` 确定性析构本轮局部资源且栈空间 $O(1)$ 复用。
5. `try` / `catch` 宏仅用 `block`、`match`、`:` 与 `break` 组合实现完整错误传播链，内核零专用异常机制。
