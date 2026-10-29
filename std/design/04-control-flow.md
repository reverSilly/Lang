# 04 - 内核控制流原语

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)
后续：[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)

## 一、核心原则

语言内核**只提供三个最底层的控制流与调度原语**，且**内核控制流原语完全不认识 `\destroy`、不处理 RAII 析构回调、不认识错误类型**：

1. **`a ; b`（顺序原语）**：先求值 `a` 并直接丢弃其结果 `value`，再求值并返回 `b`。
2. **`%branch(cond, thenProto, elseProto)`（透明二路分支原语）**：根据布尔值 `cond` 选择其中一个 `prototype` 在**当前环境（`evaluate`）**透明求值。
3. **`%break(target_ptr, val)`（物理栈帧回退原语）**：沿当前协程活跃帧链直接回退至祖先帧 `target_ptr`，释放途经栈帧并将 `val` 写入 `target_ptr` 的 `self root`。

语言内核**没有** `return`、`throw/try/catch`、循环帧、`label`、`continue`、`defer` 原语；所有伴随控制流跳转的 RAII 逆序析构（栈展开 Unwinding）及高层控制结构（`break`、`block1`、`if`、`while`、`defer`、`try`）全部在标准库中由上述原语组合实现（见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）。

## 二、`a ; b` 的纯粹性与物理槽复用

- **纯丢弃语义**：`a ; b` 从左到右求值 `a` 和 `b`，直接丢弃 `a` 返回的 `value`（哪怕该 `value` 是指向某匿名 `location` 的 `pointer`，丢弃指针值本身绝不会触发任何析构操作），直接返回 `b` 的结果。
- **未绑定临时槽的物理复用**：
  对于在当前块中分配但未经 `%bind` 绑定的匿名直接子槽，由于内核 `block` 闭合时会直接将其丢弃，只要其结果指针被 `;` 丢弃且后续不可再访问，编译器可在当前块结束前的任意时刻原地复用其物理栈槽，保证循环等场景下的 $O(1)$ 栈空间占用。

## 三、`%branch` 的透明性

```
%branch(cond, thenProto, elseProto)
```

- **当前环境透明执行**：被选中的 `prototype` 通过 `evaluate` 在**当前 `%this` 环境**直接执行，**不分配新的 `self root`、不切换 `%this` 或 `%parent`**，求值结果 `value` 原样透传。
- **支撑块内分支初始化与父帧访问**：正因为 `%branch` 不产生嵌套 `block`，在分支内部执行的 `allocate`、`%bind`、`%write` 直接作用于外层块，且分支内的零元 `%parent` 直接指向外层块的父块：
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

## 四、`%break(target_ptr, val)` 与协程根帧焊接

```
%break(target_ptr, val)
```

- **目标帧定位（普通 `pointer`，无独立 Frame Handle）**：`target_ptr` 是通过 `%this`（`\this`）或 `%parent` 获得的活跃祖先块 `self root` 指针（即普通的胖指针 `pointer` value，可复制、传参或被原型捕获）。目标必须是当前协程调用链上的活跃祖先帧；向非祖先或已结束的过期帧指针执行 `%break` 触发运行时错误（静态可证时编译期拒绝）。
- **纯粹的物理回退（Zero-RAII Kernel Unwind）**：
  - 内核 `%break(target_ptr, val)` **不执行任何 `\destroy` 回调**（途经帧与目标帧已有局部变量的逆序 `destroy` 由标准库 `break` 包装器在调用 `%break` 之前执行，见 [06-stdlib-foundations.md](06-stdlib-foundations.md) §3.3）；
  - 内核直接将当前协程栈从 `%this` 回退至 `target_ptr`，丢弃途经帧与 `target_ptr` 原有的构建期子槽，将 `val` 填入 `target_ptr` 的 `self root` 并锁定其 `shape`，使 `target_ptr` 对应的 `block` 求值即刻完成并返回指向该 `self root` 的 `pointer`。
- **与协程根帧无缝焊接**：
  若 `target_ptr` 是本协程的根帧，则本协程求值结束（状态转为 `Dead`），内核原子地将 `val` 填入恢复者 `\^` 的 `\in` 槽、释放本协程根帧并恢复 `\^` 执行。**协程内跳转 = `%break`，协程间切换 = `%resume`，两者在协程根帧无缝焊接。**

## 五、验收用例（自检清单）

1. `%branch` 经 `evaluate` 透明执行选中分支，不额外产生嵌套的 `self root` location，分支内的 `%parent` 保持不变。
2. 内核 `%break(target_ptr, val)` 仅接受祖先块的 `\this` 普通指针与结果 `val`，以 $O(1)$ 物理回退栈帧并改写 `target_ptr` 的 `self root`。
3. 对本协程根帧执行 `%break` 自动将结果送入 `\^` 的 `\in` 槽并终止本协程。
