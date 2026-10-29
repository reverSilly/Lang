# 04 - 内核控制流原语

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)
后续：[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)

## 一、核心原则

语言内核**只提供三个最底层的控制流与调度原语**，且**内核控制流原语完全不认识 `\destroy`、不处理 RAII 析构回调、不处理跨多层栈帧回退、也不认识错误类型**：

1. **`a ; b`（顺序原语）**：先求值 `a` 并直接丢弃其结果 `value`，再求值并返回 `b`。
2. **`%branch(cond, thenProto, elseProto)`（透明二路分支原语）**：根据布尔值 `cond` 选择其中一个 `prototype` 在**当前环境（`evaluate`）**透明求值。
3. **`%break(src_ptr)`（单层构建帧退出原语）**：**仅退出当前最内层构建帧 `%this`**，将 `src_ptr` 的内容移入当前 `block` 的 `self root` 结果槽并弹出当前帧。

语言内核**没有** `return`、`throw/try/catch`、循环帧、`label`、`continue`、`defer` 原语；所有伴随控制流跳转的 RAII 逆序析构以及**跨多层块的 `break target_ptr val` 栈展开接力**全部在标准库中由上述原语组合实现（见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）。

## 二、`a ; b` 的纯粹性与物理槽复用

- **纯丢弃语义**：`a ; b` 从左到右求值 `a` 和 `b`，直接丢弃 `a` 返回的 `value`（哪怕该 `value` 是指向某匿名 `location` 的 `pointer`，丢弃指针值本身绝不会触发任何析构操作），直接返回 `b` 的结果。
- **未绑定临时槽的物理复用**：
  对于在当前块中分配但未经 `%bind` 绑定的匿名直接子槽，由于内核 `block` 闭合时会直接将其丢弃，只要其结果指针被 `;` 丢弃且后续不可再访问，编译器可在当前块结束前的任意时刻原地复用其物理栈槽，保证循环等场景下的 $O(1)$ 栈空间占用。

## 三、`%branch` 的透明性

```
%branch(cond, thenProto, elseProto)
```

- **当前环境透明执行**：被选中的 `prototype` 通过 `evaluate` 在**当前 `%this` 环境**直接执行，**不分配新的 `self root`、不切换 `%this` 或 `%parent`**，求值结果 `value` 原样透传。
- **支撑块内分支初始化与父帧访问**：正因为 `%branch` 不产生嵌套 `block`，在分支内部执行的 `allocate`、`%bind`、`%write` 直接作用于外层块，且分支内的零参 `%parent` 直接指向外层块的父块：
  ```
  outer := {
      val := allocate;
      %write(\val, 42);
      inner := {
          foo := allocate;
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
  - 此前之所以认为内核必须认识 `\destroy`，唯一的原因是旧版 `%break(target_ptr, src_ptr)` 试图在内核里一口气跨多层栈帧弹出；
  - 一旦规定 Core 的 **`%break(src_ptr)` 只退出当前最内层 `block`（`%this`）**，整个生命周期与控制流瞬间彻底解耦：当前帧的局部变量清理以及跨多层块的展开接力全部交由标准库 `break target_ptr val` 与外层 `{ ... }` 宏配合完成（详见 [06-stdlib-foundations.md](06-stdlib-foundations.md) §3.3），内核的复杂度直接降到了理论下限！
- **前置条件与内核动作**：
  - **前置条件**：当前构建帧 `%this` 中除 `src_ptr` 以外的所有 `live` 子槽均已置为 `empty`（由标准库的 `break` 宏在调用 `%break` 前遍历 `%this` 逆序调用 `drop` 完成）；
  - **内核动作**：将 `src_ptr` 的内容移入当前 `block` 的 `self root` 结果槽，弹出当前 `block` 帧并向父块返回指向该 `self root` 的 `pointer`。
- **与协程根帧无缝焊接**：
  若当前帧 `%this` 是本协程的根帧，则本协程求值结束（状态转为 `Dead`），内核原子地将 `src_ptr` 的内容经 `%resume` 通道移入恢复者 `\^` 的 `\in` 槽、释放本协程根帧并恢复 `\^` 执行。**协程内单步退帧 = `%break`，协程间切换 = `%resume`，两者在协程根帧无缝焊接。**

## 五、验收用例（自检清单）

1. `%branch` 经 `evaluate` 透明执行选中分支，不额外产生嵌套的 `self root` location，分支内的 `%parent` 保持不变。
2. 内核 `%break(src_ptr)` 为单参原语，在当前构建帧其余子槽已置 `empty` 的前提下，仅弹出当前最内层构建帧 `%this` 并将 `src_ptr` 移入 `self root` 结果槽。
3. 在协程根帧执行 `%break(src_ptr)` 自动将结果送入 `\^` 的 `\in` 槽并终止本协程。
