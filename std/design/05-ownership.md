# 05 - 所有权、可变性与生命周期状态机

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)

## 一、核心裁决总览

1. **表达式皆为无所有权的 `value`；所有权唯一绑定在内部存储节点 `location` 上。** `location` 是唯一可被拥有、转移内容（`%move`）和置空销毁（`%drop`）的实体，在表达式中统一通过指向它的 `pointer` value 进行操作。
2. **单拥有者森林（Single-Owner Forest）**：每个 `block`（即对象 `self root`）拥有其直接子 `location`，子 `location` 再拥有其下属子 `location`。owned 对象图永远是无环森林，无需环收集器即可确定性销毁。
3. **动态构建期 vs 静态闭合期（连续内存相变）**：
   - **父 Block 构建期（Constructing）**：当一个 `block` 仍在栈顶求值时，其 `self root` 的 `shape` 允许随 `%bind` 动态生长；其下属的 `location` 在 `uninitialized` 态无初始 `shape`，由首次写入决定 `shape`（且 `mutable` 子槽在父块闭合前允许改变 `shape`）。
   - **父 Block 闭合相变（Closed）**：一旦 `block` 求值完成闭合，未绑定临时量被逆序销毁，已绑定但处于 `empty` 或 `uninitialized` 的槽被**静默剥离**，唯有处于 `live` 状态的已绑定槽被压平（flatten）为**连续内存布局**，整棵对象的 `shape`（含各字段偏移量与子 `shape`）**彻底凝固，终身不可再变**。
4. **单次初始化（Write-Once）与可变重填（`mutable`）分离**：
   - 每个 `location` 在运行期经历三态演进：**`uninitialized` → `live` → `empty`**；
   - 任何处于 `uninitialized` 状态的 `location`（无论默认 `immutable` 还是 `mutable`）都允许通过 `%write` 或 `%move` 填入一次内容转入 `live`；
   - 唯有声明了 `mutable` 的 `location`，才允许在通过 `%drop`（或 `%move` 移出）变为 `empty` 后，再次通过 `%write` 或 `%move` 重新填入内容转回 `live`。
5. **内核状态原语与标准库析构调度彻底解耦**：
   - 内核 `%write(dst_ptr, val)` 与 `%move(dst_ptr, src_ptr)` **严格要求 `dst_ptr` 处于 `uninitialized` 或 `empty` 状态**，内核绝不隐式触发旧内容的析构；
   - 内核 `%drop(ptr)` **仅负责将子槽已全空的 `ptr` 置为 `empty`**；递归遍历子字段、调用 `\destroy` 钩子、以及在覆写 `live` 的 `mutable` 槽之前先将其清空，全部由标准库的 `drop`、`=` 与 `move` 包装器显式完成。

## 二、所有权不变量（R1–R8）

- **R1（唯一拥有者）**：每个非空 `location` 恰有一个直接拥有者（父 `block`/对象，或协程根帧）。
- **R2（分层析构责任）**：所有权沿 `location` 树向下传递；复合对象销毁时由标准库 `drop` 先调用自身 `\destroy` 钩子，再按创建逆序递归 `drop` 各 `live` 子字段，最后调用内核 `%drop` 将自身置为 `empty`。
- **R3（无环性）**：owned `location` 森林不存在环；引用环只能经由 GC 协程 `handle` 或裸 `pointer` 产生。
- **R4（子树可移动）**：当前 `block` 拥有其 `self root` 下的整棵所有权子树，因此可以对子树中的任意 `location` 指针（含直接子及更深层后代字段）发起 `%move(dst_ptr, src_ptr)`。源位置变为 `empty`，目标空槽获得整棵子树的拥有权。
- **R5（禁止隐式复制非 POD 对象）**：POD `value` 永远按位复制；含资源或 `\destroy` 的复合对象（非 POD）被标准库 `$` 拒绝按位读出，深拷贝必须调用显式的标准库 `clone`。
- **R6（非活跃槽禁读）**：处于 `uninitialized` 或 `empty` 的 `location` 不可读取（`%deref`）、不可作为 `%move` 的源。
- **R7（符号不持有所有权）**：`%bind(symbol, ptr)` 只在当前帧符号表中登记名字到直接子 `location` 的映射，不产生、不转移、也不延长所有权。
- **R8（安全只读共享与借用）**：跨协程只读共享经由指向只读常量区（`frozen` 区）的安全 `immutable` 指针完成；跨帧临时只读访问经由叠加了 `immutable` 掩码的胖指针完成。

## 三、Location 状态机与原语前置条件

### 3.1 三个生命周期状态

| 状态 | 含义 | Shape 状态 | 允许的内核原语操作 |
| :--- | :--- | :--- | :--- |
| **`uninitialized`** | 刚由 `allocate` 创建，尚未装入任何内容 | 尚无 `shape` | 作为 `%write` 或 `%move` 的目标（写入后获初始 `shape` 并转入 `live`）；`%drop`（无操作，直接转入 `empty`）；`%bind` 命名 |
| **`live`** | 持有有效内容（纯数据字节或子 `location` 树） | 持有确定 `shape` | `%deref` 按位读取；作为 `%move` 的源（内容移出后转入 `empty`）；当所有子槽均已 `empty` 时可调用 `%drop`（转入 `empty`） |
| **`empty`** | 曾处于 `live`（或由 `uninitialized` 经 `%drop`），内容已移走或已销毁 | 若父 `block` 已闭合，保留父 `shape` 表中记录的固定字段 `sub_shape` | 清理时自动跳过；**仅当契约为 `mutable` 时**允许再次作为 `%write` 或 `%move` 的目标重新填入内容（转回 `live`；若父块已闭合则新内容 `shape` 必须严格等于原 `sub_shape`） |

### 3.2 内核原语 vs 标准库包装器操作对照表

| 操作 | 前置条件 | 动作与状态转换 |
| :--- | :--- | :--- |
| **Core `%write(dst_ptr, val)`** | `dst_ptr` 为 `uninitialized`，或为 `mutable` 且 `empty`（若父块已闭合，需 `val.shape == dst_ptr.shape`） | 将 `val` 的 `bytes` 按位拷入 `dst_ptr`，状态转为 `live` |
| **Core `%move(dst_ptr, src_ptr)`** | `src_ptr` 在当前子树内且 `live`；`dst_ptr` 为 `uninitialized`，或为 `mutable` 且 `empty`（若父块已闭合，需 `shape` 严格相等） | 将 `src_ptr` 子树移入 `dst_ptr`，`src_ptr` 转为 `empty`，`dst_ptr` 转为 `live` |
| **Core `%drop(ptr)`** | `ptr` 在当前子树内，处于 `live`（且所有子槽已 `empty`）或 `uninitialized` | 将 `ptr` 状态置为 `empty`（对已 `empty` 报错） |
| **Stdlib `drop(ptr)`** | `ptr` 在当前子树内，处于 `live` 或 `uninitialized` | 若 `live`：先调 `ptr.\destroy`（若有），再按逆序对所有 `live` 子字段递归调 `drop`，最后调 `%drop(ptr)` 置为 `empty` |
| **Stdlib `dst_ptr = val`** | `dst_ptr` 为 `uninitialized`，或为 `mutable`（`live` 或 `empty`） | 若 `dst_ptr` 处于 `live`，先调 `drop(dst_ptr)` 置为 `empty`；再调 `%write(dst_ptr, val)` |
| **Stdlib `move(dst_ptr, src_ptr)`** | `src_ptr` 为 `live`；`dst_ptr` 为 `uninitialized` 或 `mutable` | 若 `dst_ptr` 处于 `live`，先调 `drop(dst_ptr)` 置为 `empty`；再调 `%move(dst_ptr, src_ptr)` |

### 3.3 为什么这套设计极其干净？

1. **内核彻底摆脱递归析构与覆写分支**：
   内核的 `%write`、`%move`、`%drop` 全是 $O(1)$ 的原子状态机跃迁，不涉及任何用户代码回调（`\destroy`），也不会在写入中途因为析构函数抛错而陷入半毁坏状态。
2. **天然支持不可变变量的延迟/分支单次初始化**：
   默认 `immutable` 的槽在 `uninitialized` 态允许执行一次 `%write` 或 `%move` 进入 `live`，此后因为不允许对 `immutable` 槽覆写或重填，天然保证初始化后不可变：
   ```
   x := allocate;
   (\x = 10) if cond else (\x = 20);
   !- 此处 x 已处于 live 且契约为 immutable，后续任何 \x = 30 均被拒绝 -!
   ```
3. **存储可变性按字段独立，只读性沿胖指针掩码传播**：
   - 每个字段在父对象 `shape` 的相对胖指针表中拥有独立的 `field_mutability`，因此不可变对象内部可以包含 `mutable` 字段；
   - 当通过只读胖指针访问对象时，`%field` 执行 `min(parent_mut, field_mut)`，使 `immutable` 掩码沿整条访问路径自动传染，提供深度只读保证。

## 四、`block` 对象构造、剥离与连续内存闭合

设 `block ^p` 在父块 `P` 中求值，其隐式分配的 `self root` 为 `R_B`：

1. **归属父块**：`R_B` 是父块 `P` 的直接子 `location`，`block ^p` 求值完毕后返回指向 `R_B` 的 `pointer` value，父块 `P` 可用 `%bind(x, ...)`（即 `x := { ... }`）将其命名到 `P` 的符号表。
2. **构建期动态性（Constructing）**：在 `p` 执行期间，`R_B` 处于构建状态，块内可通过 `%this`（`\this`）及 `\field`（即 `%field(%this, field)`）读写字段。
3. **闭合相变（Constructing → Closed Contiguous Object）**：
   - **正常完成时**：
     1. `R_B` 下所有**未经 `%bind` 命名**的直接子 `location`（临时量与 `defer` 守卫）按创建逆序调用 `drop` 销毁并剥离；
     2. `R_B` 下所有虽经 `%bind` 命名、但此时已处于 **`empty` 或 `uninitialized`** 状态的直接子 `location`（例如块内用完即被 `move` 走或显式 `drop` 的具名中间变量）**被直接静默剥离**，不进入最终对象；
     3. 剩余所有处于 **`live`** 状态的已 `%bind` 直接子 `location` 被紧密压平（flatten）为一块连续的字节内存，生成驻留在只读常量区的不可变复合 `shape`（记录各字段 `symbol -> (offset, sub_shape_ptr, mutability)`），`R_B` 状态定格为 `live` 且整体形状永久锁定。
   - **因 `%break(handle, src_ptr)` 提前完成时**：
     先摘下 `src_ptr` 的内容，将 `R_B` 内所有已存在子槽逆序 `drop` 清空，再将摘下的内容移入 `R_B` 并锁定其 `shape`。
4. **已闭合对象的局部掏空（Tombstone）**：
   - 对象 `p` 闭合后，其连续内存布局与 `shape` 已不可更改；
   - 若外部将后代字段 `\p.q` 经 `%move` 移走或执行 `drop(\p.q)`，`\p.q` 槽位在原地变为 `empty`（作为 tombstone 保留在 `p` 的固定偏移处，`p` 的 `shape` 不变）；
   - 若 `q` 契约为 `mutable`，后续可向 `\p.q` 重新移入或写入**相同 `sub_shape`** 的内容；当未来 `drop(\p)` 时，自动跳过已处于 `empty` 的 `\p.q`。

## 五、只读冻结（`%freeze`）与跨阶段/跨协程共享

| 机制 | 物理与语义本质 | 用途 |
| :--- | :--- | :--- |
| **`%freeze(ptr) -> frozen_ptr`** | 将不含外部可变引用或活跃句柄的对象子树深拷贝/迁入只读常量区（`.rodata`），返回 `mutability = immutable` 的安全 `pointer` | 模块、全局常量、编译期生成查找表、跨协程零拷贝只读共享 |
| **`cell` 协程** | 状态 `location` 归 `new` 创建的协程根帧私有（根帧未闭合，内部槽可持续演进），外部持 GC `handle` 经 `@` 消息读写 | 跨闭包、跨协程的安全可变共享 |
| **胖指针只读借用** | 将 `pointer` 的 `mutability` 掩码设为 `immutable`，通过 `%field` 自动传染给所有子字段访问 | 调用链内的深度只读视图 |

## 六、验收用例（自检清单）

1. **块内具名中间变量自动剥离**：`p := { tmp := 10; a := ($\tmp + 1); drop(\tmp) }` 闭合时，因 `tmp` 已处于 `empty`，被静默剥离；最终闭合对象 `p` 的 `shape` 仅含字段 `a`。
2. **闭合后连续布局与形状锁定**：`p := { a := allocate mutable; \a = 1; b := 2 }` 闭合前 `\a` 可赋任意形状值；闭合后 `p` 内存连续，`\p.a = 3` 合法（同为 `integer`），但试图向 `\p.a` 写入不同 `shape` 的值被拒绝。
3. **单次分支初始化**：`x := allocate; (\x = 1) if c else (\x = 2)` 中不可变槽 `x` 通过 `uninitialized → live` 完成初始化；此后写 `\x = 3` 因 `x` 非 `mutable` 而报错。
4. **显式覆写先析构后写入**：`y := allocate mutable; \y = res1; \y = res2` 中，第二次 `=` 由标准库先调 `drop(\y)` 执行 `res1` 的 `\destroy` 并将其 `%drop` 置为 `empty`，随后再调 `%write(\y, res2)`。
5. **局部掏空 Tombstone**：`p := { q := { b := 2 } }` 闭合后，执行 `move(\dst, \p.q)` 将 `\p.q` 掏空为 `empty`；此后读取 `$\p.q.b` 或再次 `drop(\p.q)` 报错，而外层块结束销毁 `p` 时自动跳过已 `empty` 的 `p.q`。
