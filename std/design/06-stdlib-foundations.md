# 06 - 标准库基石：分派器、RAII（`destroy`/`drop`）、求值块与模式匹配

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)

## 一、核心原则：内核极简原语 × 标准库高层语义

由于编译器内核（Core）完全剥离了 `\destroy` 钩子、RAII 栈展开、所有权移动（`move`）、交换（`swap`）、深拷贝（`clone`）与语法糖，标准库（Stdlib）利用 **`evaluate`（在调用方当前环境就地展开）**、**`shape.fields` 双向符号链表** 以及 **`%this -> %parent` 帧链导航**，在零内核黑魔法的前提下完整构建出所有高层语言机制。

## 二、`destroy` 与 `drop` 的解耦及内存操作家族

### 2.1 为什么要解耦 `destroy(ptr)` 与 `drop(slot_ptr)`？

- 在递归销毁已闭合对象的内部子字段（如 `p.f`）、覆写已闭合对象的 `mutable` 子字段（`\p.f = new_f`）或弹出动态容器元素时，目标位置**不是当前构建期块的直接子槽**，它只需要**递归执行 `\destroy` 钩子释放外部资源**，而不需要（也不允许）调用内核 `%drop` 将已闭合对象内部留出 `empty` 空洞。
- 唯有针对**构建期块的直接子槽（`slot_ptr`）**执行销毁或移动时，才需要调用内核 **`%drop(slot_ptr)`** 将该直接子槽置为 `empty`（从而在块闭合时自动剥离，兼作 `unbind`）。

因此，标准库将析构与槽操作精确划分为以下正交原语组合：

| 标准库工具 / 算符 | 适用目标 | 底层展开实现（全部基于 Core 原语） | 语义说明 |
| :--- | :--- | :--- | :--- |
| **`box(val) -> ptr`** | 任意 `value` | 在调用方当前块 `evaluate`：<br>`p := allocate; %write(p, val); p` | 在当前块分配未绑定的不可变直接子槽、按位写入 `val` 并返回该槽指针 |
| **`x := rhs`** | 当前块符号 `x` 与任意 `rhs` | 在调用方当前块 `evaluate`：<br>`v := rhs; (%bind(x, box(v))) if (~%bind(x, v))` | 先尝试直接 `%bind(x, v)`；若返回 `false`（说明 `v` 非当前块未绑定直接子槽），则经 `box(v)` 装箱后再 `%bind`（故 `p := \a` 自然将指针 `\a` 装入新指针变量 `p`） |
| **`$ptr`** | 任意 `live` 槽指针 | 检查 `ptr.shape` 是否为 POD（builtin POD 或不含 `\destroy` 且子字段皆 POD 的复合 shape）：若是则 `%deref(ptr)`，否则报错 | 安全读取 POD 值（支持自定义小结构体如 `Point` 直接按值读取与传参） |
| **`destroy(ptr)`** | **任意 `live` 指针**（含闭合对象子字段、容器元素、构建期槽） | 1. 若 `%field(ptr, destroy) ~= null`，调用 `ptr.\destroy(ptr)`；<br>2. 从 `s := %shape(ptr)` 的 `s.last_sym` 开始沿 `prev_sym` 逆序遍历每个子字段 `sym`，递归调用 `destroy(%field(ptr, sym))` | **纯钩子递归执行器**：自顶向下执行自身 `\destroy`、自底向上逆序执行子字段 `destroy`；**从不调用内核 `%drop`** |
| **`drop(slot_ptr)`** | **构建期直接子槽** | `destroy(slot_ptr); %drop(slot_ptr)` | 先递归执行整棵对象树的 `\destroy` 钩子，再调用内核 `%drop` 将该构建期直接子槽置为 `empty`（自动 unbind，闭合时剥离） |
| **`move(dst_slot, src_slot)`** | 可写目标槽与**构建期直接子槽 `src_slot`** | 若 `dst_slot` 已处于 `live` 则先调 `destroy(dst_slot)`；随后执行：<br>`%write(dst_slot, %deref(src_slot)); %drop(src_slot)` | 将 `src_slot` 的字节整体拷入 `dst_slot`（不触发 `src_slot` 的 `destroy`），并调用内核 `%drop(src_slot)` 将源构建期槽置为 `empty` |
| **`dst_ptr = val`** | 任意可写槽指针（`uninitialized` 或 `mutable`） | 若 `dst_ptr` 已处于 `live`，先调 **`destroy(dst_ptr)`**（释放旧资源但不置 `empty`）；随后调 **`%write(dst_ptr, val)`** | 统一支持构建期首次初始化、构建期可变槽覆写、以及**已闭合对象 `mutable` 子字段的原地覆写（`\p.f = val`）** |
| **`swap(ptr1, ptr2)`** | 任意两个同 `shape` 的可写 `live` 指针 | `tmp := %deref(ptr1); %write(ptr1, %deref(ptr2)); %write(ptr2, tmp)` | 原子交换两个可写槽的字节内容（不触发 `destroy`，也不产生 `empty` 槽） |
| **`clone(dst_ptr, src_ptr)`** | 可写目标槽与源指针 | 若为 POD 则 `dst_ptr = $src_ptr`；若含自定义 `\clone` 则调用之；否则按 `shape.fields` 链表正序递归 `clone` 各字段 | 显式深拷贝 |

## 三、块包装器（`{ ... }` vs `block1`）与用户态 RAII / 栈展开

因为内核的 `block` 和 `%break` 完全不处理 RAII，标准库通过以下四个包装器在用户态 100% 闭环实现确定性析构与异常栈展开：

### 3.1 `defer(action)`：挂入构建期符号链的守卫

- `defer(action)` 宏在当前块中构造一个携带 `\destroy := ^action` 的守卫对象，并使用编译器生成的块内唯一隐藏符号（如 `__defer_1`）调用 `%bind` 挂入当前构建块的 `shape.fields` 双向链表中。
- 因为它挂在当前块的 `shape.fields` 链中，无论是块正常结束还是被 `break` 跨帧穿过，都能通过遍历 `shape.fields` 的 `last_sym -> prev_sym` 链精确按创建逆序找到并执行它！

### 3.2 对象构造块 `{ Expr }` vs 过程求值块 `block1`

1. **对象构造块 `{ Expr }`**：
   - 目标：构造并返回一个持久化复合对象（保留所有用户显式 `%bind` 的字段）。
   - 展开逻辑：在 `block` 内部求值 `Expr`；在闭合前的最后一刻，仅对块内的隐藏临时守卫（`__defer_k`）按逆序调用 `drop(__defer_k)`（执行其 `\destroy` 并经 `%drop` 置为 `empty` 从而在闭合时被剥离），而所有用户 `%bind` 的 `live` 字段保留并压平为最终连续内存对象。
2. **过程求值块 `block1`（`block ^(break (evaluate expr))`）**：
   - 目标：提供局部词法作用域（用于普通函数体、局部代码块、循环体），求值一系列语句后**自动逆序销毁块内定义的所有局部变量与 `defer` 守卫，仅返回最后一个表达式的值（In-place Construct 到外层结果槽）**！
   - 展开逻辑：
     ```
     block1(proto) := block ^(break (evaluate proto))
     ```
     当 `evaluate proto` 算出尾表达式结果时，外层的标准库 `break` 立即触发：先逆序 `drop` 当前块中绑定的所有局部变量与 `defer` 守卫，再通过内核 `%break` 将尾表达式结果直接就地填入当前块的 `self root` 返回！

### 3.3 标准库 `break` 包装器：沿 `%parent` 链自举栈展开（Unwinding）

当代码执行 `break [target_ptr] expr` 时（缺省 `target_ptr` 为当前块 `\this`）：
1. 先在当前环境求值 `val := expr`（若 `expr` 是正准备从即将销毁的栈帧中移出的构建期槽，则先 `%deref` 取出其字节并调用内核 `%drop` 将原槽置为 `empty`，防止后续展开时将其析构）；
2. **沿 `%this -> %parent` 链逆序清理途经帧**：
   ```
   cur := %this;
   {
       !- 顺着 cur.shape 的 last_sym -> prev_sym 逆序遍历所有 live 绑定槽 -!
       !- 对每个槽调用 drop(field_ptr)（即 destroy + %drop） -!
       cleanup_frame(\cur);
       (%break(\loop_block, ())) if ($\cur == target_ptr);
       \cur = cur.%parent
   } while true;
   ```
3. 当从当前帧一直到 `target_ptr` 的所有局部变量与 `defer` 守卫均已逆序执行完 `destroy` 后，最后调用内核 **`%break(target_ptr, val)`** 完成物理退栈与结果写入！

## 四、分支、循环与错误传播

### 4.1 `if` 与 `while`

- **`expr1 if cond else expr2`**：直接重写为 `%branch(cond, ^expr1, ^expr2)`（无 `else` 时默认为 `^()`）。
- **`body while cond`**：通过 `evaluate` 与 `%branch` 在当前环境透明迭代。配合 `block1` 求值块，单轮循环体内的所有局部变量在每轮末尾自动通过 `break` 逆序析构并原地复用物理栈槽。

### 4.2 模式匹配 `match` 与错误值 `fail`

- **纯函数匹配器**：
  ```
  match(pattern, subject) -> Bindings | fail
  ```
  - 裸 `symbol` 值 `a`：匹配任意 `subject` 并产出绑定对 `(a, subject)`；
  - 普通 `value`（如 `3` 或 `$\a`）：按值相等（`==`）匹配；
  - 带 `matcher` 字段的自定义 Pattern 对象：调用 `pattern.matcher(subject)`（支持形状比较 `<` / `>` / `=`、组合子 `|` / `&`、守卫 `If` 及可逆算术匹配 `a + 1 : 3`）。
- **`value : pattern`（可反驳约束）**：调用 `match(pattern, value)`，若返回 `fail` 则立即向当前块执行 `break <fail>`；若返回 `Bindings` 则依次在当前块执行 `sym := val` 提交绑定。
- **`try` / `catch`**：将受检表达式包裹在子块中求值，匹配 `fail` 形状以接力 `break` 传播或转入恢复分支。

## 五、函数、Lambda 与闭包捕获预览

- **统一子程序模型**：普通函数与 Lambda 统一由 `block1`（或 `block`）+ 参数模式匹配 + `evaluate` 构成；词法可证明不调用 `%resume` 挂起的子程序直接退化为硬件栈帧调用。
- **递归（`fix` 组合子）**：在调用帧中动态注入 `self` 绑定实现自引用。
- **闭包捕获**：
  - 值拷贝与构建期变量 `move` 捕获：装入闭包对象的 `env` 字段，内部通过显式 `env` 指针访问；
  - 共享可变状态捕获：通过 `new` 创建 GC 托管的 `cell` 协程，多个闭包持有该协程的 `handle` 经 `@` 消息安全读写。

## 六、验收用例（自检清单）

1. `p := { a := 1; f := open("x") }; drop(\p)`：`drop(\p)` 先调 `destroy(\p)`（逆序执行 `\p.f` 的 `\destroy` 关闭文件，不调 `%drop(\p.f)`），最后调一次内核 `%drop(\p)` 将 `\p` 在当前块置为 `empty`。
2. `\p.f = open("y")`：对已闭合对象 `p` 的 `mutable` 字段 `\p.f` 赋值时，`=` 先调 `destroy(\p.f)` 关闭旧文件，再调 `%write` 将新文件写入 `\p.f`，全程 `\p.f` 不经过 `empty` 状态。
3. `res := block1 ^( a := open("x"); b := 41; $\b + 1 )`：`block1` 展开为 `block ^(break (evaluate ...))`，算出 `42` 后通过标准库 `break` 自动逆序 `drop(\b)` 与 `drop(\a)`（关闭文件 `"x"`），最终 `res` 直接就地获得 `42`。
4. 跨多层块的 `break outer_ptr val` 自动沿 `%this -> %parent` 链逆序执行沿途各帧局部变量与 `defer` 的 `destroy`，随后调内核 `%break` 物理弹栈。
