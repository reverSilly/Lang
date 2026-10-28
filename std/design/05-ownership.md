# 05 - 所有权、可变性与构建期状态机

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)
后续：[06-stdlib-foundations.md](06-stdlib-foundations.md)

## 一、核心裁决总览

1. **表达式皆为无所有权的 `value`；所有权唯一绑定在内部存储节点 `location` 上。** `location` 是唯一可被拥有、写入（`%write`）和置空（`%drop`）的实体，在表达式中统一通过指向它的 `pointer` value 进行操作。
2. **单拥有者森林（Single-Owner Forest）**：每个 `block`（即对象 `self root`）拥有其直接子 `location`，子 `location` 再拥有其下属子 `location`。owned 对象图永远是无环森林。
3. **动态构建期 vs 静态闭合期（闭合对象字段恒 `live` 铁律）**：
   - **构建期（Constructing Block）**：当一个 `block` 仍在栈顶求值时，其直接子槽经历 **`uninitialized` → `live` → `empty`** 三态演进（状态仅维护在当前构建期栈帧上）；
   - **闭合期（Closed Contiguous Object）**：一旦 `block` 求值完成闭合，所有未 `%bind` 槽及处于 `empty`/`uninitialized` 的槽被**直接剥离丢弃**，唯有处于 `live` 状态的已 `%bind` 直接子槽被压平（flatten）为**连续内存布局**。**任何已闭合对象的所有字段在其整个存活期内 100% 永远处于 `live` 状态（绝不存在已闭合对象的 `empty` 字段）**，从而保证 `%field` 与 `%deref` 永远是零状态检查开销的硬件级访问！
4. **单次初始化（Write-Once）与可变覆写（`mutable`）分离**：
   - 任何处于构建期 `uninitialized` 状态的直接子槽（无论默认 `immutable` 还是 `mutable`）都允许通过 `%write` 填入一次内容以获得初始 `shape` 并转入 `live`；
   - 唯有声明了 `mutable` 的槽，才允许在进入 `live` 后（或构建期经 `%drop` 变为 `empty` 后）再次调用 `%write` 写入新内容。
5. **内核状态机与标准库 RAII（`\destroy`）彻底解耦**：
   - **内核完全不认识 `\destroy`**，也不内置 `%move` 或 `%swap` 原语；
   - 内核仅提供针对构建期直接子槽的置空原语 **`%drop(slot_ptr)`**（将构建期直接子槽置为 `empty`，在 `%field` 查找时视为不存在并在块闭合时自动剥离，天然兼任 `unbind`）；
   - 递归执行 `\destroy` 钩子（`destroy`）、构建期变量销毁（`drop`）、所有权移动（`move`）、交换（`swap`）全部由标准库基于 `%deref`、`%write` 与 `%drop` 组合实现（见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）。

## 二、所有权不变量（R1–R8）

- **R1（唯一拥有者）**：每个 `location` 恰有一个直接拥有者（父 `block`/对象，或协程根帧）。
- **R2（闭合对象完整性）**：任何已闭合对象的所有字段恒为 `live`，不允许将已闭合对象的单个内部子字段单独掏空为 `empty`（业务可空字段显式使用标准库 `Optional(T)`）。
- **R3（无环性）**：owned `location` 森林不存在环；引用环只能经由 GC 协程 `handle` 或裸 `pointer` 产生。
- **R4（构建期槽可转移与摘除）**：在构建期块（`%this` 或活跃父帧 `%parent`）中，直接子槽可通过标准库 `move`（`%write` + `%deref` + `%drop`）转移内容并将原槽置为 `empty`（闭合时自动剥离）。
- **R5（禁止隐式复制非 POD 对象）**：POD `value` 永远按位复制；含 `\destroy` 的复合对象（非 POD）被标准库 `$` 拒绝按位读出，深拷贝必须调用显式的标准库 `clone`。
- **R6（非活跃槽禁读）**：构建期处于 `uninitialized` 或 `empty` 的槽不可读取（`%deref`）。
- **R7（`%bind` 与 `%drop` 职责正交）**：`%bind(symbol, val)` 仅负责将当前块未绑定的直接子槽挂入符号表；`%drop(slot_ptr)` 仅负责将构建期直接子槽状态置为 `empty`（因 `%field` 与块闭合自动忽略 `empty` 槽，从而自然达成 `unbind` 效果，无需独立的 `%unbind` 原语）。
- **R8（安全只读共享与借用）**：跨协程只读共享经由指向只读常量区（`frozen` 区）的安全 `immutable` 指针完成；跨帧临时只读访问经由叠加了 `immutable` 掩码的胖指针完成。

## 三、构建期三态机与内核原语前置条件

### 3.1 构建期直接子槽的三个状态

| 状态（仅存在于构建期栈帧） | 含义 | Shape 状态 | 允许的内核原语操作 |
| :--- | :--- | :--- | :--- |
| **`uninitialized`** | 刚由 `allocate` 创建，尚未装入内容 | `sub_shape == null` | `%write` 填入初值（获初始 `shape` 并转入 `live`）；`%drop`（直接转入 `empty`）；`%bind` 命名 |
| **`live`** | 持有有效内容（构建期槽及所有已闭合对象字段均处于此态） | 持有确定 `shape` | `%deref` 按位读取；若为 `mutable` 则允许 `%write` 按位覆写；若为构建期直接子槽则允许 `%drop`（转入 `empty`） |
| **`empty`** | 构建期直接子槽曾被 `%drop`（因销毁或移动）置空 | 无内容（`%field` 查找返 `null`） | 块闭合时被直接剥离丢弃；**仅当契约为 `mutable` 时**允许再次通过 `%write` 填入内容转回 `live` |

### 3.2 为什么“仅构建期直接子槽有 `empty`、闭合对象字段恒 `live`”是终极解？

1. **彻底消除每次字段访问的运行期状态检查（100% 0-Overhead）**：
   因为已闭合对象的内部字段绝不可能处于 `empty`，所以在已闭合对象上执行 `%field(ptr, sym)` 或 `%deref(ptr)` 时，**不需要查任何全局墓碑表（`tombstone_set`）、不需要旁路位图（`State Bitvector`）、也不需要检查 `\destroy == null`**，直接执行 `addr + offset` 即可！
2. **彻底避开首字段地址重合（`offset == 0`）悖论**：
   由于内核 `%drop(slot_ptr)` 仅作用于构建期栈帧上的直接子槽（而不作用于已闭合对象内部偏移为 `0` 的子字段），构建期栈帧的每个直接子槽在帧槽表中拥有各自独立的槽位，绝不会与内嵌子结构体发生状态混淆。
3. **单次延迟/分支初始化不可变变量无需 `%seal`**：
   默认 `immutable` 的槽在构建期 `uninitialized` 态允许执行一次 `%write` 进入 `live`，此后禁止再次 `%write`：
   ```
   x := allocate;
   (\x = 10) if cond else (\x = 20);
   !- 此处 x 已处于 live 且契约为 immutable，后续任何 \x = 30 均被拒绝 -!
   ```

## 四、`block` 对象构造、剥离与连续内存闭合

设 `block ^p` 在父块 `P` 中求值，其隐式分配的 `self root` 为 `R_B`：

1. **归属父块与就地构造**：`R_B` 是父块 `P` 的直接子 `location`，`block ^p` 求值完毕后返回指向 `R_B` 的 `pointer` value，父块 `P` 可用 `%bind(x, ...)`（即 `x := { ... }`）将其命名到 `P` 的符号表（物理上直接在 `P` 的槽位中就地构造）。
2. **构建期动态性（Constructing）**：在 `p` 执行期间，`R_B` 处于构建状态，块内可通过 `%this`（`\this`）及 `\field`（即 `%field(%this, field)`）读写字段。
3. **闭合相变（Constructing → Closed Contiguous Object）**：
   - 当 `p` 执行到底闭合时：
     1. `R_B` 下所有**未经 `%bind` 命名**的直接子槽，以及虽经 `%bind` 命名但此时处于 **`empty` 或 `uninitialized`** 状态的直接子槽（例如块内用完即被 `move` 走或 `drop` 掉的具名中间变量、以及已执行完毕的 `defer` 守卫）**被直接剥离丢弃**；
     2. 剩余所有处于 **`live`** 状态的已 `%bind` 直接子槽被紧密压平（flatten）为一块连续的字节内存，生成驻留在只读常量区的不可变复合 `shape`（其 `fields` 为底层 `LinkedHashMap<symbol, FieldNode>`），`R_B` 状态定格为 `live` 且整体形状永久锁定。

## 五、只读冻结（`%freeze`）与跨阶段/跨协程共享

| 机制 | 物理与语义本质 | 用途 |
| :--- | :--- | :--- |
| **`%freeze(ptr) -> frozen_ptr`** | 将不含外部可变引用或活跃句柄的对象子树深拷贝/迁入只读常量区（`.rodata`），返回 `mutability = immutable` 的安全 `pointer` | 模块、全局常量、编译期生成查找表、跨协程零拷贝只读共享 |
| **`cell` 协程** | 状态 `location` 归 `new` 创建的协程根帧私有（根帧未闭合，内部槽可持续演进），外部持 GC `handle` 经 `@` 消息读写 | 跨闭包、跨协程的安全可变共享 |
| **胖指针只读借用** | 将 `pointer` 的 `mutability` 掩码设为 `immutable`，通过 `%field` 自动传染给所有子字段访问 | 调用链内的深度只读视图 |

## 六、验收用例（自检清单）

1. **块内具名中间变量自动剥离**：`p := { tmp := 10; a := ($\tmp + 1); drop(\tmp) }` 中，`drop(\tmp)` 将 `\tmp` 置为 `empty`；闭合时 `tmp` 被自动剥离，最终闭合对象 `p` 的 `shape` 仅含字段 `a`。
2. **闭合后连续布局与零检查访问**：`p := { a := allocate mutable; \a = 1; b := 2 }` 闭合后内存连续，访问 `\p.a` 与 `\p.b` 无任何 `empty` 状态检查开销。
3. **单次分支初始化**：`x := allocate; (\x = 1) if c else (\x = 2)` 中不可变槽 `x` 通过 `uninitialized → live` 完成初始化；此后写 `\x = 3` 因 `x` 非 `mutable` 而报错。
