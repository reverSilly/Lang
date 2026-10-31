# 04 - 内核控制流原语

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)
后续：[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)

## 一、核心原则

语言内核**只提供三个最底层的控制流与调度原语**，且**内核控制流原语完全不认识 `\destroy`、不处理 RAII 析构回调、不检查 POD、不处理跨多层栈帧回退、也不认识错误类型**：

1. **`a ; b`（顺序原语）**：先求值 `a` 并直接丢弃其结果 `value`，再求值并返回 `b`。
2. **`%branch(cond, thenProto, elseProto)`（透明二路分支原语）**：根据布尔值 `cond` 选择其中一个 `Prototype` 在**当前环境（`evaluate`）**透明求值。
3. **`%break(val)`（单层构建帧退出原语）**：**仅退出当前最内层构建帧 `%this`**，将 `val` 直接装入当前 `block` 的 `self root` 结果槽并弹出当前帧。

语言内核**没有** `return`、`throw/try/catch`、循环帧、`label`、`continue`、`defer` 原语；所有伴随控制流跳转的 POD Gate 检验、RAII 逆序析构以及**跨多层块的 `break target_ptr val` 栈展开接力**全部在标准库中由上述原语组合实现（见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）。

## 二、`a ; b` 的纯粹性与匿名临时槽清理

- **纯丢弃语义**：`a ; b` 从左到右求值 `a` 和 `b`，直接丢弃 `a` 返回的 `value`（丢弃指针值本身不会在内核中触发任何析构），直接返回 `b` 的结果。
- **匿名临时槽（Unbound Locations）的生命周期**：
  若表达式 `a` 在当前块中创建了未经 `%bind` 命名的匿名 `live` 直接子槽（例如 `get_fd(open("tmp.txt"))` 中的临时 `File` 对象，或 `$({ ... })` 留下的匿名槽），其生命周期留待块闭合（或 `break`）时由标准库包裹层通过零参游标 **`%unbound`** 按 LIFO 逆序调用 `drop(%unbound)` 统一析构并置为 `empty`（用户或标准库 wrapper 亦可手动提前 `drop`）。

## 三、`%branch` 的透明性

```
%branch(cond, thenProto, elseProto)
```

- **当前环境透明执行**：被选中的 `Prototype` 通过 `evaluate` 在**当前 `%this` 环境**直接执行，**不分配新的 `self root`、不切换 `%this` 或 `%parent`**，求值结果 `value` 原样透传。
- **支撑块内分支初始化与父帧访问**：正因为 `%branch` 不产生嵌套 `block`，在分支内部执行的 `%allocate`、`%allocate_val`、`%bind`、`%write` 直接作用于外层块，且分支内的零参 `%parent` 直接指向外层块的父块：
  ```
  outer := {
      val := %allocate i32;
      %write(\val, 42);
      inner := {
          foo := %allocate i32;
          %branch($(%parent.val) == 42,
              ^(%write(\foo, -1)),
              ^(%write(\foo, 1))
          );
          %deref(\foo) !- -1 -!
      }
  }
  ```

## 四、单层 `%break(val)` 与协程根帧焊接

```
%break(val)
```

- **为什么内核 `%break(val)` 仅退出一层且直接接收 `val`？**
  - 一旦规定 Core 的 **`%break(val)` 只退出当前最内层 `block`（`%this`）且直接接收纯位段 `val`**（零 POD/RAII 检查）：
    1. 普通值退出（如 `%break(42)`）无需在即将销毁的栈帧中先分配一个临时 `location` 再搬运；
    2. 移动局部非 POD 变量退出（或构造 `move(\a)` 块）只需先通过 `v := %deref(\a); %drop(\a)` 取出位段并将源槽置为 `empty`，在清理完本帧其余全部 `live` 槽后直接调用 `%break(v)`，彻底消除了“清理当前帧时还需绕开某个结果槽 `src_ptr`”的特例！
    3. 防止非 POD 值被非法按位复制的职责，由面向用户的标准库 `break` / `@` wrapper 在入口处通过 `is_pod` Gate 统一把关（详见 [06-stdlib-foundations.md](06-stdlib-foundations.md) §3.3）。
- **前置条件与内核动作**：
  - **前置条件**：当前构建帧 `%this` 中的所有 `live` 子槽（含匿名槽与具名槽）均已由标准库调用 `drop`（或 `%drop`）置为 `empty`；
  - **内核动作**：自动剥离所有 `uninitialized` 与 `empty` 槽，将 `val` 直接装入当前 `block` 的 `self root` 结果槽（赋予 `%shape(val)` 并置为 `live`），弹出当前 `block` 帧并向父块返回指向该 `self root` 的未绑定 `Pointer`（挂入父块的 `%unbound` 栈顶）。
- **与协程根帧无缝焊接**：
  若当前帧 `%this` 是本协程的根帧，则本协程求值结束（状态转为 `Dead`），内核原子地将 `val` 经 `%resume` 通道装入恢复者 `\^` 的 `\in` 槽、释放本协程根帧并恢复 `\^` 执行。**协程内单步退帧 = `%break(val)`，协程间切换 = `%resume(coro, val)`，两者在协程根帧无缝焊接。**

## 五、验收用例（自检清单）

1. `%branch` 经 `evaluate` 透明执行选中分支，不额外产生嵌套的 `self root` location，分支内的 `%parent` 保持不变。
2. 内核 `%break(val)` 为接收 `value` 的单参原语，在当前构建帧全部 `live` 子槽已置 `empty` 的前提下，自动剥离 `uninitialized`/`empty` 槽，仅弹出当前最内层构建帧 `%this` 并将 `val` 装入 `self root` 结果槽返回给父块。
3. 在协程根帧执行 `%break(val)` 自动将 `val` 装入 `\^` 的 `\in` 槽并终止本协程。
