# 04 - 内核控制流原语

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)
后续：[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)

## 一、核心原则

语言内核**只提供三个最底层的控制流与调度原语**，且**内核控制流原语完全不认识 `\destroy`、不处理 RAII 析构回调、不处理跨多层栈帧回退、也不认识错误类型**：

1. **`a ; b`（顺序原语）**：先求值 `a` 并直接丢弃其结果 `value`，再求值并返回 `b`。
2. **`%branch(cond, thenProto, elseProto)`（透明二路分支原语）**：根据布尔值 `cond` 选择其中一个 `Prototype` 在**当前环境（`evaluate`）**透明求值。
3. **`%break(src_ptr)`（单层构建帧退出原语）**：**仅退出当前最内层构建帧 `%this`**，将 `src_ptr` 的内容移入当前 `block` 的 `self root` 结果槽并弹出当前帧。

语言内核**没有** `return`、`throw/try/catch`、循环帧、`label`、`continue`、`defer` 原语；所有伴随控制流跳转的 RAII 逆序析构以及**跨多层块的 `break target_ptr val` 栈展开接力**全部在标准库中由上述原语组合实现（见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）。

## 二、`a ; b` 的纯粹性与匿名临时槽清理

- **纯丢弃语义**：`a ; b` 从左到右求值 `a` 和 `b`，直接丢弃 `a` 返回的 `value`（丢弃指针值本身不会在内核中触发任何析构），直接返回 `b` 的结果。
- **匿名临时槽（Unbound Locations）的生命周期**：
  若表达式 `a` 在当前块中创建了未经 `%bind` 命名的匿名 `live` 直接子槽（例如 `get_fd(open("tmp.txt"))` 中的临时 `File` 对象），标准库包裹层通过零参游标 **`%unbound`** 按 LIFO 逆序调用 `drop(%unbound)` 将其析构并置为 `empty`（编译器随后可原地复用该物理栈槽，保证循环等场景下的 $O(1)$ 栈空间占用）。

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

## 四、单层 `%break(src_ptr)` 与协程根帧焊接

```
%break(src_ptr)
```

- **为什么内核 `%break(src_ptr)` 仅退出一层（单参、仅退 `%this`）？**
  - 一旦规定 Core 的 **`%break(src_ptr)` 只退出当前最内层 `block`（`%this`）**，整个生命周期与控制流瞬间彻底解耦：当前帧的匿名临时槽（`%unbound`）与具名 `live` 变量（`last_sym -> %prev_sym`）的逆序清理，以及跨多层块的展开接力，全部交由标准库 `break target_ptr val` 与外层 `{ ... }` 宏配合完成（详见 [06-stdlib-foundations.md](06-stdlib-foundations.md) §3.3），内核的复杂度降到理论下限！
- **前置条件与内核动作**：
  - **前置条件**：当前构建帧 `%this` 中除 `src_ptr` 以外的所有 `live` 子槽（含匿名槽与具名槽）均已由标准库调用 `drop` 置为 `empty`；
  - **内核动作**：自动剥离所有 `uninitialized` 与 `empty` 槽，将 `src_ptr` 的内容移入当前 `block` 的 `self root` 结果槽，弹出当前 `block` 帧并向父块返回指向该 `self root` 的 `Pointer`。
- **与协程根帧无缝焊接**：
  若当前帧 `%this` 是本协程的根帧，则本协程求值结束（状态转为 `Dead`），内核原子地将 `src_ptr` 的内容经 `%resume` 通道移入恢复者 `\^` 的 `\in` 槽、释放本协程根帧并恢复 `\^` 执行。**协程内单步退帧 = `%break`，协程间切换 = `%resume`，两者在协程根帧无缝焊接。**

## 五、验收用例（自检清单）

1. `%branch` 经 `evaluate` 透明执行选中分支，不额外产生嵌套的 `self root` location，分支内的 `%parent` 保持不变。
2. 内核 `%break(src_ptr)` 为单参原语，在当前构建帧其余 `live` 子槽已置 `empty` 的前提下，自动剥离 `uninitialized`/`empty` 槽，仅弹出当前最内层构建帧 `%this` 并将 `src_ptr` 移入 `self root` 结果槽。
3. 在协程根帧执行 `%break(src_ptr)` 自动将结果送入 `\^` 的 `\in` 槽并终止本协程。
