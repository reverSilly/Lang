# 05 - 所有权、可变性与构建期状态机

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)
后续：[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)

## 一、核心裁决总览

1. **表达式皆为无所有权的 `value`；所有权唯一绑定在内部逻辑存储节点 `location` 上。** `location` 是唯一可被拥有、写入（`%write`）、移动（`%move`）、交换（`%swap`）和置空（`%drop`）的实体，在表达式中统一通过指向它的 `pointer` value 进行操作；且 `location` 作为逻辑槽位抽象，不强制在任意时刻都对应物理内存（寄存器与内存均可承载 `location`）。
2. **单拥有者森林（Single-Owner Forest）**：每个 `block`（即对象 `self root`）拥有其直接子 `location`，子 `location` 再拥有其下属子 `location`。owned 对象图永远是无环森林。
3. **动态构建期 vs 静态闭合期（闭合对象无内部 `empty` 空洞铁律）**：
   - **构建期（Constructing Block）**：`uninitialized` / `live` / `empty` 三态**仅属于“构建期栈帧的直接子槽（Frame Slot）”**（状态仅维护在当前活跃构建帧上）；
   - **闭合期（Closed Contiguous Object）**：一旦 `block` 求值完成闭合压平为连续内存对象，所有未 `%bind` 槽及处于 `empty`/`uninitialized` 的槽被**直接静默剥离**，唯有处于 `live` 状态的已 `%bind` 直接子槽被压平（flatten）为**零 Header 连续内存布局**。**任何已闭合对象的所有内部子字段在其整个存活期内 100% 永远处于 `live` 状态（严禁单方面掏空已闭合对象的某个内部字段留下 `empty` 空洞）**，从而保证 `%field` 与 `%deref` 永远是零状态检查开销的硬件级访问！
4. **单次初始化（Write-Once）与可变操作（`mutable`）分离**：
   - 任何处于构建期 `uninitialized` 状态的直接子槽（无论默认 `immutable` 还是 `mutable`）都允许通过 `%write` 或 `%move` 填入一次内容以获得初始 `shape` 并转入 `live`；
   - 进入 `live` 后（或构建期经 `%drop`/`%move` 变为 `empty` 后），唯有契约声明为 `mutable`（且指针具有 `mutable` 权限）的槽，才允许再次填入新内容或通过 `%swap` 交换内容。
5. **内核状态机与标准库 RAII（`\destroy`）彻底解耦**：
   - **内核完全不知道 `\destroy` 的存在**，所有 RAII 析构均由标准库管理（见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）；
   - 内核原语 **`%write(dst_ptr, val)`** 与 **`%move(dst_ptr, src_ptr)`** 严格要求目标槽 `dst_ptr` 处于 `uninitialized` 或 `empty` 空态；
   - 内核原语 **`%drop(slot_ptr)`** 仅作用于构建期帧的直接子槽（要求其下无未清空的构建期子槽），仅负责将该槽在当前构建帧中的状态标记为 `empty`（闭合时自动剥离，兼作取消字段导出）；
   - 内核原语 **`%swap(ptr1, ptr2)`** 对称交换两个同 `shape` 的可写 `live` 槽内容（交换后双方依然都是 `live`，零空洞、零析构），专用于零拷贝提取或修改已闭合对象的 `mutable` 子字段！

## 二、所有权不变量（R1–R8）

- **R1（唯一拥有者）**：每个 `location` 恰有一个直接拥有者（父 `block`/对象，或协程根帧）。
- **R2（闭合对象无内部空洞）**：已闭合对象压平为零 Header 连续内存后，子字段指针 `\p.q` 只是指向 `p.addr + offset` 的胖指针，没有指向父对象 `p` 的反向指针；因此不允许单方面掏空已闭合对象的内部子字段留下 `empty` 空洞：
  1. **闭合对象的整体移动与销毁**：闭合对象 `p` 本身作为外层构建帧的一个直接子槽，依然可以整体 `%move` 或整体 `drop(\p)`；
  2. **闭合对象内部 `mutable` 字段的提取与修改**：若要零拷贝提取或替换已闭合对象 `p` 的可变字段 `\p.q`，只需通过对称的 **`%swap(\p.q, \tmp_slot)`** 与构建期槽交换内容（交换后双方依然都是 `live`），再在当前构建帧对换出的 `\tmp_slot` 调用 `drop(\tmp_slot)`；
  3. **业务可空字段**：若用户确实需要闭合对象内有“可空字段”，直接使用标准库的内联 `Optional(T)`（基于 `bytes(N, A)` + `tag` 的内联 `Union`），在连续内存中显式表达空状态。
- **R3（无环性）**：owned `location` 森林不存在环；引用环只能经由 GC 协程 `handle` 或裸 `pointer` 产生。
- **R4（允许从活跃构建链 `%parent` 移动 Location）**：当内层块 `{ ... }` 正在运行时，外层块 `%parent` 依然处于栈上的**构建期（Constructing）**。只要源槽属于当前协程活跃构建帧链（`%this` 或其任意层 `%parent`）的直接子槽，且该槽本身不是正在运行的活跃子帧根，**`%move(dst_ptr, src_ptr)` 允许将其从 `%parent` 移走并将外层构建帧中的源槽状态置为 `empty`**（等到外层块最终闭合时，该 `empty` 绑定会被自动静默剥离）。这是将外层块变量移入内层块（如函数调用帧、返回对象）最基础的所有权传递路径。
- **R5（禁止隐式复制非 POD 对象）**：POD `value` 永远按位复制；含 `\destroy` 的复合对象（非 POD）被标准库 `$` 拒绝按位读出，深拷贝必须调用显式的标准库 `clone`。
- **R6（非活跃槽禁读）**：构建期处于 `uninitialized` 或 `empty` 的槽不可读取（`%deref`）、不可作为 `%move` 的源或 `%swap` 的操作数。
- **R7（`%bind` 与 `%drop` 职责正交）**：`%bind(symbol, val)` 仅负责将当前块未绑定的直接子槽挂入符号表；`%drop(slot_ptr)` 仅负责将构建期直接子槽状态置为 `empty`（块闭合时自动静默剥离 `empty` 槽，从而自然取消字段导出）。
- **R8（安全只读共享与借用）**：跨协程只读共享经由指向只读常量区（`frozen` 区）的安全 `immutable` 指针完成；跨帧临时只读访问经由叠加了 `immutable` 掩码的胖指针完成。

## 三、构建期三态机与内核原语对照

### 3.1 构建期直接子槽的三个状态

| 状态（仅属于构建期帧直接子槽） | 含义 | Shape 状态 | 允许的内核原语操作 |
| :--- | :--- | :--- | :--- |
| **`uninitialized`** | 刚由 `allocate` 创建，尚未装入内容 | `sub_shape == null` | **`%write` 或 `%move` 填入初值**（无论 `immutable` 或 `mutable` 均允许单次填入，获初始 `shape` 并转入 `live`）；`%drop`（直接转入 `empty`）；`%bind` 命名 |
| **`live`** | 持有有效内容（构建期槽及所有已闭合对象字段均处于此态） | 持有确定 `shape` | `%deref` 按位读取；若可写（`mutable`）则允许 **`%swap(p1, p2)`** 与同 `shape` 的可写 `live` 槽交换内容；若为构建期直接子槽则允许作为 **`%move` 的源**或调用 **`%drop`**（转入 `empty`） |
| **`empty`** | 构建期直接子槽曾被 `%move` 移走或被 `%drop` 置空 | 无内容（`%field` 查找返 `null`） | 块闭合时被直接静默剥离；**仅当契约为 `mutable` 时**允许再次作为 `%write` 或 `%move` 的目标填入新内容转回 `live` |

### 3.2 内核状态与读写原语总结

| 内核原语 | 前置条件 | 动作与状态转换 |
| :--- | :--- | :--- |
| **`%write(dst_ptr, val)`** | `dst_ptr` 为构建期直接子槽，且处于 `uninitialized` 或 `mutable empty` | 将 `val` 的 `bytes` 按位拷入 `dst_ptr`，赋予 `val` 的 `shape` 并置为 `live` |
| **`%move(dst_ptr, src_ptr)`** | `src_ptr` 为活跃构建帧链（`%this` 或 `%parent`）上的 `live` 直接子槽；`dst_ptr` 处于 `uninitialized` 或 `mutable empty` | 将 `src_ptr` 的内容移入 `dst_ptr`（`dst_ptr` 转为 `live`），并将 `src_ptr` 在其所属构建帧中置为 `empty` |
| **`%drop(slot_ptr)`** | `slot_ptr` 为活跃构建帧链上的直接子槽（要求其下无未清空的构建期子槽），处于 `live` 或 `uninitialized` | 将 `slot_ptr` 在所属构建帧中的状态标记为 `empty`（零 RAII，对已 `empty` 报错） |
| **`%swap(ptr1, ptr2)`** | `ptr1` 与 `ptr2` 均为可写（`mutable`）的 `live` 指针（含已闭合对象的可变子字段），且 `ptr1.shape_ptr == ptr2.shape_ptr` | 按字节原地交换两槽的内容，交换后双方仍为 `live`（零空洞、零析构） |
| **`%deref(ptr) -> val`** | `ptr` 指向任意处于 `live` 状态的槽 | 按 `ptr.shape_ptr` 指向的 `Shape.size` 无条件读出字节返回 `val` |

## 四、`block` 对象构造、剥离与连续内存闭合

设 `block ^p` 在父块 `P` 中求值，其隐式分配的 `self root` 为 `R_B`：

1. **归属父块与就地构造**：`R_B` 是父块 `P` 的直接子 `location`，`block ^p` 求值完毕后返回指向 `R_B` 的 `pointer` value，父块 `P` 可用 `%bind(x, ...)`（即 `x := { ... }`）将其命名到 `P` 的符号表（物理上直接在 `P` 的槽位中就地构造）。
2. **构建期动态性（Constructing）**：在 `p` 执行期间，`R_B` 处于构建状态，块内可通过 `%this`（`\this`）及 `\field`（即 `%field(%this, field)`）读写字段。
3. **闭合相变（Constructing → Closed Contiguous Object）**：
   - 当 `p` 执行到底闭合时：
     1. `R_B` 下所有**未经 `%bind` 命名**的直接子槽，以及虽经 `%bind` 命名但此时处于 **`empty` 或 `uninitialized`** 状态的直接子槽（例如块内用完即被 `%move` 走或 `drop` 掉的具名中间变量、已清理的 `__parent` 槽及 `__defer_k` 守卫）**被直接静默剥离**；
     2. 剩余所有处于 **`live`** 状态的已 `%bind` 直接子槽按各自的 `align` 对齐并紧密压平（flatten）为一块连续的字节内存，生成驻留在只读常量区的不可变复合 `Shape`（其 `align = max(1, field.align)`；若包含零尺寸字段 `{}`，其 `size = 0` 但符号仍记录在 `first_sym .. last_sym` 链中），`R_B` 状态定格为 `live` 且整体形状永久锁定。

## 五、只读冻结（`%freeze`）与跨阶段/跨协程共享

| 机制 | 物理与语义本质 | 用途 |
| :--- | :--- | :--- |
| **`%freeze(ptr) -> frozen_ptr`** | 将不含外部可变引用或活跃句柄的对象子树深拷贝/迁入只读常量区（`.rodata`），返回 `mutability = immutable` 的安全 `pointer` | 模块、全局常量、编译期生成查找表、跨协程零拷贝只读共享 |
| **`cell` 协程** | 状态 `location` 归 `new` 创建的协程根帧私有（根帧未闭合，内部槽可持续演进），外部持 GC `handle` 经 `@` 消息读写 | 跨闭包、跨协程的安全可变共享 |
| **胖指针只读借用** | 将 `pointer` 的 `mutability` 掩码设为 `immutable`，通过 `%field` 自动传染给所有子字段访问 | 调用链内的深度只读视图 |

## 六、验收用例（自检清单）

1. **从 `%parent` 移动构建期槽与自动剥离**：`p := { tmp := open("x"); a := { f := allocate; %move(\f, %parent.tmp) } }` 中，内层块通过 `%move` 将外层正在构建的 `%parent.tmp` 移入 `f`，使外层的 `tmp` 变为 `empty`；当外层块闭合时，`tmp` 被自动静默剥离，最终对象 `p` 的 `shape` 仅含字段 `a`。
2. **闭合对象内部 `mutable` 字段的 `%swap` 修改**：`p := { f := allocate mutable; %write(\f, open("x")) }` 闭合后，对 `\p.f = open("y")` 赋值时，标准库在当前块分配临时槽 `\tmp` 装入新文件，调用 `%swap(\p.f, \tmp)` 将新文件换入 `\p.f`、旧文件换出到 `\tmp`，再调用 `drop(\tmp)` 销毁旧文件，全程 `p.f` 保持 `live` 无空洞。
3. **单次分支初始化**：`x := allocate; (\x = 1) if c else (\x = 2)` 中不可变槽 `x` 通过 `uninitialized → live` 完成初始化；此后写 `\x = 3` 因 `x` 非 `mutable` 而报错。
