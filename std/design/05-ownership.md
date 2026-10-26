# 05 - 所有权、可变性与生命周期状态机

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)

## 一、核心裁决总览

1. **`value` 无所有权；所有权唯一绑定在 `location` 上。** `location` 是唯一可被拥有、移动（`%move`）和销毁（`%drop`）的实体。
2. **单拥有者森林（Single-Owner Forest）**：每个 `block`（即对象 `self root`）拥有其直接子 `location`，子 `location` 再拥有其下属子 `location`。owned 对象图永远是无环森林，无需环收集器即可确定性递归销毁。
3. **契约属性终身固定，状态机驱动生命周期**：
   - 每个 `location` 在创建时固定其**契约属性**（可选 `shape` 约束 + `mutability`，默认 `immutable`），终身不可更改（内核不提供修改 `mutability` 或 `shape` 约束的原语）。
   - 每个 `location` 在运行期经历三态演进：**`uninitialized` → `live` → `empty`**。
4. **单次初始化（Write-Once）与可变覆写（`mutable`）分离**：
   - 任何处于 `uninitialized` 状态的 `location`（无论契约是默认 `immutable` 还是 `mutable`）都允许被写入或移入一次以完成初始化，状态转入 `live`；
   - 进入 `live`（或后续变为 `empty`）后，**唯有显式声明了 `mutable` 契约的 `location` 才允许再次被 `=` 覆写或 `%move` 填入**。
5. **Block 是对象构造器，`self root` 即结果位置**：
   - `block ^p` 在父 block 中分配其 `self root`；
   - 在 `block` 闭合的相变时刻，已 `%bind` 的直接子 `location` 成为对象的持久化字段，未 `%bind` 的直接子 `location`（临时量与 `defer` 守卫）按创建逆序销毁。
6. **无借用检查器**：跨帧临时只读访问经由帧 handle 的不可变胖指针（借用）完成，受调用栈结构约束不可逃逸；可变共享走 GC 托管的 `cell` 协程；只读跨协程共享走无所有权的 `frozen` 值；非自有对象的任意引用走 `unsafe` 裸指针。

## 二、所有权不变量（R1–R8）

- **R1（唯一拥有者）**：每个非空 `location` 恰有一个直接拥有者（父 `block`/对象，或协程根帧）。
- **R2（递归析构责任）**：所有权沿 `location` 树向下传递；父 `location` 销毁时负责按创建逆序递归销毁其所有仍处于 `live` 状态的直接子 `location`。
- **R3（无环性）**：owned `location` 森林不存在环；引用环只能经由 GC 协程 handle 或 `unsafe` 裸指针产生。
- **R4（子树可移动）**：当前 `block` 拥有其 `self root` 下的整棵所有权子树，因此可以对子树中的任意 `location`（含直接子及更深层后代字段）发起 `%move`。源位置变为 `empty`（若有显式 shape 约束则保留为 typed empty），目标位置获得整棵子树的拥有权。
- **R5（禁止隐式复制 Location）**：`value` 永远按位复制；`location`/对象绝不隐式复制，深拷贝必须调用显式的标准库 `clone`。
- **R6（非活跃槽禁读）**：处于 `uninitialized` 或 `empty` 的 `location` 不可读取（`$`）、不可作为 `%move` 的源。
- **R7（符号不持有所有权）**：`%bind(symbol, loc)` 只在当前帧符号表中登记名字到直接子 `loc` 的映射，不产生、不转移、也不延长所有权。
- **R8（无跨块可变别名）**：安全子集内不存在指向同一 `location` 的跨块可变别名。

## 三、Location 状态机与可变性规则

### 3.1 三个生命周期状态

| 状态 | 含义 | 内容形状（Content Shape） | 允许的操作 |
| :--- | :--- | :--- | :--- |
| **`uninitialized`** | 刚由 `allocate` 创建，尚未装入任何内容 | 无内容形状（若带显式 shape 约束则保留该约束） | 首次初始化写入（`=` / `%move` 目标，转入 `live`）；`%drop`（无操作，直接转入 `empty`）；`%bind` 命名 |
| **`live`** | 持有有效内容（叶子 `value` 或子 `location` 树） | 由当前内容决定 | 读取 `$`（仅叶子）；作为 `%move` 源（转入 `empty`）；`%drop`（递归析构并转入 `empty`）；**仅当契约为 `mutable` 时**允许 `=` 覆写或作为 `%move` 目标（先析构旧内容，保持 `live`） |
| **`empty`** | 曾处于 `live`，内容已被 `%move` 移走或被 `%drop` 销毁 | 无内容形状（若带显式 shape 约束则保留约束） | 递归清理时自动跳过；**仅当契约为 `mutable` 时**允许再次通过 `=` 或 `%move` 重新填入内容（转回 `live`）；显式对 `empty` 调 `%drop` 或读取报错 |

### 3.2 为什么这套状态机极其简洁？

1. **无需 `%seal` 原语即可支持延迟/分支初始化不可变变量**：
   因为默认 `immutable` 的槽在 `uninitialized` 态允许写入一次以进入 `live`，以下代码天然合法且 `x` 在初始化后严格不可变：
   ```
   x := allocate integer;
   (\x = 10) if cond else (\x = 20);
   !- 此处 x 已处于 live 且契约为 immutable，后续任何 \x = 30 均被拒绝 -!
   ```
2. **存储可变性按 Location 独立，只读性沿胖指针借用传播**：
   - 每个字段都是独立 `allocate` 或 `block` 产生的 `location`，因此一个不可变对象内部完全可以包含 `mutable` 字段（反之亦然）；
   - 对象 `self root` 自身的 `mutable` 属性仅决定“能否用新内容整体覆写该 root 位置”，不影响其内部各字段按各自契约读写；
   - 当通过帧 handle（如 `\this`）获取不可变胖指针借用时，胖指针携带的 `immutable` 掩码沿整个访问路径传染，提供深度只读保证。

### 3.3 形状（Shape）与可选约束

- **形状属于内容，不属于容器容量**：对象的当前形状是其字段集合与各字段当前形状的递归组合。
- **显式约束可选**：`location` 只有在声明了显式 shape 约束时才在写入/`%move` 时检验内容形状；无显式约束的 `mutable` 槽在覆写时可接收不同形状的新内容（底层内联、装箱或重分配属于编译器实现策略，不影响语言语义）。

## 四、`block` 对象构造与生命周期相变

设 `block ^p` 在父块 `P` 中求值，其隐式分配的 `self root` 为 `R_B`：

1. **归属父块**：`R_B` 是父块 `P` 的直接子 `location`，因此在 `block ^p` 求值完毕后，父块 `P` 可用 `x := block ^p`（即 `x := { ... }`）将其 `%bind` 到 `P` 的符号表。
2. **不暴露半成品**：`%bind` 严格发生在 RHS 完全求值之后；在 `p` 执行期间，父块 `P` 中尚不存在指向 `R_B` 的符号绑定，块内仅能通过当前 env（如 `\field`）或 `\this` handle 访问自身。
3. **闭合相变（Constructing → Live Object）**：
   - 当 `p` 正常求值到底时，`R_B` 中未经过 `%bind` 的直接子 `location`（临时量与 `defer` 守卫）按创建逆序析构；
   - 经过 `%bind` 的直接子 `location` 保留为对象字段，`R_B` 状态定格为 `live`（契约默认 `immutable`）。
   - 当 `p` 因 `break v` 提前完成时，`R_B` 中已有的字段与未绑定临时量全部逆序析构（除非 `v` 是从其中摘下移出的子树），随后 `v` 填入 `R_B`，`R_B` 状态定格为 `live`。
4. **未绑定临时 Location 的物理槽回收规则**：
   - 若 `R_B`（或任意 `allocate` 结果）在父块 `P` 中未被 `%bind`，语义上其 `\destroy` 钩子在 `P` 闭合相变时逆序触发；
   - **优化法则**：只要某未绑定 `location` 子树内不含可观测的 `\destroy` 钩子（例如已执行过 `%drop`、或由 `break ()` 清空了字段只剩纯 `value`），一旦其表达式结果被 `;` 丢弃，编译器可立即在当前语句后复用其物理内存槽，保证循环等场景下的 $O(1)$ 栈空间占用。

## 五、销毁协议：`%drop` 与 `\destroy`

### 5.1 `%drop(loc-expr)` 原语

`%drop` 是唯一暴露给用户的显式销毁原语，接收一个求值为 `location` 的表达式（如 `\p`、`\p.q` 或 `{ body }`）：
1. 检验目标 `location` 位于当前 `block` 的所有权子树内：
   - 若目标为 `uninitialized`：无操作，状态直接置为 `empty`；
   - 若目标为 `empty`：触发错误（禁止重复显式 `%drop`）；
   - 若目标为 `live`：执行第 2 步递归销毁。
2. **递归销毁流程**：
   - 将目标标记为 `destroying`（防止重入）；
   - 若目标对象含有 `\destroy` 字段，以目标的临时只读胖指针借用调用 `\destroy`；
   - 按创建逆序递归销毁目标下所有仍处于 `live` 状态的直接子 `location`（跳过 `empty` 与 `uninitialized` 子槽）；
   - 释放目标存储内容，将其状态置为 `empty`（后代节点随内容一并消失，目标槽位与父块中的 symbol 绑定仍保留，作为 tombstone 指向 `empty`）。

### 5.2 `\destroy` 钩子纪律

- `\destroy` 内部只允许读取自身字段或调用子资源的关闭逻辑；
- 严禁在 `\destroy` 中 `%move` 自身字段、严禁向块外 `break`、严禁调用 `%resume` 挂起或 `new` 创建新协程。

## 六、共享与跨阶段固化汇总

| 机制 | 用途 | 所有权与可变性特征 | 安全性 |
| :--- | :--- | :--- | :--- |
| **`cell` 协程** | 跨闭包 / 跨协程可变共享 | 状态 location 归 cell 协程根帧私有；外部持 GC 句柄经 `@` 消息读写 | 安全子集 |
| **`frozen` 对象** | 模块 / 常量 / 跨协程只读共享 | 从 owned 对象深拷贝生成的无主不可变 `value` 快照，无 `\destroy`、不参与析构 | 安全子集 |
| **帧 handle 借用** | 同一调用链内临时只读访问 | 经 `\this` 等取得的不可变胖指针掩码，禁止存储、捕获、返回或 `%move` | 安全子集 |
| **裸指针（`pointer`）** | 容器底层实现 / 非自有对象访问 | 普通 `value`，不拥有、不保活；目标 `%move` 或 `%drop` 后解引用为 UB | `unsafe` |

## 七、验收用例（自检清单）

1. `p := { a := 1; q := { b := 2 } }`：`p`、`p.a`、`p.q`、`p.q.b` 均为 `live` + `immutable`；在父块写 `x := \p.q` 被静态拒绝（`%bind` 仅允许命名当前块直接子 location），但写 `dst := allocate _; \dst = \p.q` 合法（`%move` 允许移动当前所有权子树内的后代 `p.q`，移动后 `p.q` 变为 `empty` tombstone）。
2. `x := allocate integer; (\x = 1) if c else (\x = 2)`：不可变槽 `x` 通过 `uninitialized → live` 完成分支初始化；此后写 `\x = 3` 报错。
3. `y := allocate mutable integer; \y = 1; %drop(\y); \y = 2`：`mutable` 槽在 `%drop` 变为 `empty` 后允许再次写入重回 `live`。
4. `p := { a := 1 }; %drop(\p)`：`%drop` 后 `p` 变为 `empty`，读取 `$\p` 或再次 `%drop(\p)` 触发运行时 trap；外层块结束清理时自动跳过已处于 `empty` 的 `p`。
