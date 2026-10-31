# 05 - 所有权、可变性与构建期状态机

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)
后续：[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)

## 一、核心裁决总览

1. **表达式皆为无所有权的 `value`；所有权唯一绑定在内部逻辑存储节点 `location` 上。** `location` 是唯一可被拥有、分配（`%allocate` / `%allocate_val`）、写入（`%write`）、触活（`%touch`）和置空（`%drop`）的实体，在表达式中统一通过指向它的真 `Pointer` value 进行操作；且 `location` 作为逻辑槽位抽象，不强制在任意时刻都对应物理内存（寄存器与内存均可承载 `location`）。
2. **单个 `location` 终身 `Shape` 固定**：每个 `location` 在由 `%allocate [mutable] shape_ptr` 或 `%allocate_val [mutable] val` 创建时即确定 `Shape` 并分配好存储空间；未闭合块中改变变量类型通过标准库 **`drop(\x)` 释放旧槽 + 分配新 `Shape` 槽 + `%bind` 重绑（Rebind）** 实现。
3. **单拥有者森林（Single-Owner Forest）**：每个 `block`（即对象 `self root`）拥有其直接子 `location`，子 `location` 再拥有其下属子 `location`。owned 对象图永远是无环森林。
4. **动态构建期 vs 静态闭合期（闭合对象无内部 `empty` 空洞铁律）**：
   - **构建期（Constructing Block）**：`uninitialized` / `live` / `empty` 三态**仅属于“构建期栈帧的直接子槽（Frame Slot）”**；
   - **闭合期（Closed Contiguous Object）**：一旦 `block` 求值完成闭合（且未命名临时槽已由标准库通过 `%unbound` 清理完毕），内核自动静默剥离所有处于 `empty` 或 `uninitialized` 的直接子槽，唯有处于 **`live`** 状态的已 `%bind` 直接子槽被压平（flatten）为**零 Header 连续内存布局**。**任何已闭合对象的所有内部子字段在其整个存活期内 100% 永远处于 `live` 状态（严禁对已闭合对象的内部字段调用 `%drop` 留下 `empty` 空洞）**！
5. **单次初始化（Write-Once）与可变操作（`mutable`）分离**：
   - 任何处于构建期 `uninitialized` 状态的直接子槽（无论默认 `immutable` 还是 `mutable`）都允许通过 `%write` 或 `%touch` 完成一次初始化转入 `live`；
   - 一旦进入 `live`（或构建期经 `%drop` 变为 `empty` 后），唯有契约声明为 `mutable`（且指针具有 `mutable` 权限）的位置，才允许再次调用 **`%write(dst_ptr, val)`** 覆写新内容！
6. **内核状态机与标准库 RAII（`\destroy`）/ POD Gate / `move` / `swap` 彻底解耦**：
   - **内核完全不知道 `\destroy` 或 POD 的存在**，也不内置 `%move` 或 `%swap` 原语；
   - **`%deref(ptr)`**、**`%write(dst_ptr, val)`**、**`%break(val)`** 与 **`%resume(coro, val)`** 全是纯粹的位段读写/传递原语（不检查 POD、不触发 `\destroy`）；
   - **`%drop(slot_ptr)`** 仅作用于活跃构建帧链（`%this` 或 `%parent`）的直接子槽，仅负责将该槽状态标记为 `empty` 并从 `live` 链中摘除（零 RAII）；
   - **`move` 与 `swap` 全部在标准库中由 `block`、`%break`、`%allocate_val`、`%deref`、`%write`、`%drop` 与 `destroy` 组合导出**（见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）！

## 二、所有权不变量（R1–R8）

- **R1（唯一拥有者）**：每个 `location` 恰有一个直接拥有者（父 `block`/对象，或协程根帧）。
- **R2（闭合对象无内部空洞）**：已闭合对象压平为零 Header 连续内存后，不允许对其内部子字段调用 `%drop` 留下 `empty` 空洞：
  1. **闭合对象的整体移动与销毁**：闭合对象 `p` 本身作为外层构建帧的一个直接子槽，依然可以整体 `move(\p)` 或整体 `drop(\p)`；
  2. **闭合对象内部 `mutable` 字段的直接覆写与移动**：由于 `%write(dst_ptr, val)` 直接支持任何具有 `mutable` 权限且形状匹配的 `dst_ptr`（含已闭合对象的 `mutable live` 子字段 `\p.q`），修改 `\p.q` 只需在标准库执行 **`destroy(\p.q); %write(\p.q, new_val)`**（若从构建槽 `\src` 移入 `\p.q`，则执行 **`destroy(\p.q); %write(\p.q, %deref(\src)); %drop(\src)`**），全程 `\p.q` 100% 保持 `live` 无空洞！
  3. **业务可空字段**：使用标准库的内联 `Optional(T)` / `Union`（通过 `%allocate mutable (bytes_shape(N, A))` + `%touch` 构造始终处于 `live` 状态的未初始化字节载荷）。
- **R3（无环性）**：owned `location` 森林不存在环；引用环只能经由 GC 协程 `Handle` 或裸 `Pointer` 产生。
- **R4（允许对活跃构建链 `%parent` 的直接子槽执行 `%drop` 以支撑 `{ %break(...) }` 跨帧 `move`）**：当内层块 `{ ... }` 正在运行时，外层块 `%parent` 依然处于栈上的**构建期（Constructing）**。只要源槽 `src_ptr` 属于当前协程活跃构建帧链（`%this` 或其任意层 `%parent`）的直接子槽，且该槽本身不是正在运行的活跃子帧根，**`%drop(src_ptr)` 允许将其在外层构建帧中的状态置为 `empty`**（等到外层块最终闭合时，该 `empty` 槽被内核自动静默剥离）。标准库的 `move(\a)` 正是通过在子块中读取 `%deref(%parent.a)`、调用 `%drop(%parent.a)` 置空外层源槽、再通过 `%break(val)` 将值装入子块 `self root`（作为调用方父块的未绑定 `%unbound` 新槽返回），完美实现零原语的槽移动！
- **R5（标准库 POD Gate 把关非 POD 拷贝）**：Core `%deref`、`%write`、`%break`、`%resume` 不检查 POD；面向用户的标准库 `$`、`=`、`break` 与 `@` 包装器通过递归检查 `Shape`（以 `first_sym == null_sym` 为终点，且不含 `\destroy`）设置 POD Gate，拦截未经 `move` 的非 POD 对象被隐式按位复制。
- **R6（非活跃槽禁读）**：构建期处于 `uninitialized` 或 `empty` 的槽不可读取（`%deref` 报错）。
- **R7（`%bind`、`%touch` 与 `%drop` 职责正交）**：
  - `%bind(symbol, val)` 仅负责将当前块未绑定的直接子槽挂入符号表（若同名旧绑定已处于 `empty` 态则直接覆盖重绑）；
  - 唯有当已绑定槽处于 `live` 状态（由 `%allocate_val` 创建，或经 `%write`/`%touch` 激活）时，其符号才出现在 `(%this).shape_ptr` 的 `first_sym .. last_sym` 游标链中；
  - `%drop(slot_ptr)` 将构建期直接子槽置为 `empty`，并立即将其从 `first_sym .. last_sym`（或 `%unbound`）中摘除。
- **R8（安全只读共享与借用）**：跨协程只读共享经由指向只读常量区（`frozen` 区）的安全 `immutable` 指针完成；跨帧临时只读访问经由将 `Pointer.mutability` 置为 `0`（`immutable`）的胖指针完成。

## 三、构建期三态机与内核原语对照

### 3.1 构建期直接子槽的三个状态

| 状态（仅属于构建期帧直接子槽） | 产生方式 | 是否在 `first_sym..last_sym` 或 `%unbound` 中 | 允许的内核原语操作 |
| :--- | :--- | :--- | :--- |
| **`uninitialized`** | 刚由 `%allocate [mutable] s` 创建（已分配好内存且 `shape_ptr == s`） | **否**（`\x` 可寻址，但游标不遍历它；若直到块闭合仍为 `uninit`，由内核直接静默剥离） | **`%write(\x, val)` 填入初值**或 **`%touch(\x)` 零开销激活用作缓冲区**（无论 `immutable` 或 `mutable` 均允许一次 `uninit → live`）；`%drop`（转入 `empty`）；`%bind` 命名 |
| **`live`** | 由 `%allocate_val` 创建，或经 `%write` / `%touch` 激活（所有已闭合对象字段恒处于此态） | **是**（已命名槽在 `first_sym..last_sym` 中；未命名槽在 `%unbound` 栈中） | `%deref` 按位读取；**若 `mutability == mutable` 则允许直接调用 `%write(ptr, val)` 覆写新字节**；若为构建链直接子槽则允许调用 **`%drop(ptr)`**（转入 `empty`） |
| **`empty`** | 构建期直接子槽被 `%drop` 置空 | **否**（已从游标链摘除；`\x` 查找返 `null` 或仅允許 `mutable` 槽原地 `%write` 复活；块闭合时由内核静默剥离） | 同名符号可被新 `%bind` 覆盖重绑（Rebind）；若契约为 `mutable` 亦可直接通过保存的槽指针再次 `%write` 或 `%touch` 转回 `live` |

### 3.2 内核状态与读写原语总结（共 7 个，无 `%move` / `%swap`）

| 内核原语 | 前置条件 | 动作与状态转换 |
| :--- | :--- | :--- |
| **`%allocate [mutable] s -> ptr`** | `s` 为有效 `Pointer(Shape)` | 在当前块 `%this` 分配大小与对齐为 `(s.size, s.align)` 的直接子槽，状态置为 `uninitialized`，返回真 `Pointer` |
| **`%allocate_val [mutable] val -> ptr`** | `val` 为任意 `value` | 在当前块 `%this` 分配形状为 `%shape(val)` 的直接子槽，拷入 `val` 字节，状态直接置为 `live`（挂入 `%unbound`），返回真 `Pointer` |
| **`%touch(slot_ptr)`** | `slot_ptr` 为 `%this` 的直接子槽，处于 `uninitialized` 或 `mutable empty` | **不改动任何内存字节**，直接将该槽状态置为 `live`（用于 `Optional`/`Union` 未初始化载荷） |
| **`%bind(sym, val) -> Boolean`** | `sym` 为 `Symbol` | 若 `val` 为 `%this` 下未绑定的直接子槽指针（且 `sym` 未绑定或原绑定已 `empty`），将其从 `%unbound` 移入具名符号表并返回 `true`；否则返回 `false` |
| **`%write(dst_ptr, val)`** | `$dst_ptr.shape_ptr == (%shape(val)).addr`，且 `dst_ptr` 为 `uninitialized` 直接子槽 **或** `dst_ptr.mutability == mutable`（含已闭合对象的 `mutable live` 字段） | 将 `val` 的字节无条件按位拷入 `dst_ptr.addr`，并将槽状态置为/保持为 `live`（零 POD/RAII 检查） |
| **`%deref(ptr) -> val`** | `ptr` 指向任意处于 `live` 状态的槽 | 按 `ptr.shape_ptr` 指向的 `Shape.size` 无条件读出字节返回 `val`（零 POD/RAII 检查） |
| **`%drop(slot_ptr)`** | `slot_ptr` 为活跃构建帧链（`%this` 或 `%parent`）上的直接子槽（要求其下无未清空子槽），处于 `live` 或 `uninitialized` | 将 `slot_ptr` 状态标记为 `empty` 并从 `first_sym..last_sym` 或 `%unbound` 中摘除（零 RAII，对已 `empty` 或已闭合对象内部字段报错） |

## 四、`block` 对象构造、剥离与连续内存闭合

设 `block ^p` 在父块 `P` 中求值，其隐式分配的 `self root` 为 `R_B`：

1. **归属父块与就地构造**：`R_B` 是父块 `P` 的直接子 `location`，`block ^p` 求值完毕（无论是自然到底还是经 `%break(val)` 退出）后返回指向 `R_B` 的未绑定 `live` `Pointer` value（挂入 `P` 的 `%unbound`），父块 `P` 可用 `%bind(x, ...)`（即 `x := { ... }` 或 `b := move(\a)`）将其直接命名到 `P` 的符号表。
2. **构建期动态性（Constructing）**：在 `p` 执行期间，`R_B` 处于构建状态，块内可通过 `%this`（`\this`）及 `\field`（即 `%field(%this, field)`）读写字段。
3. **闭合相变（Constructing → Closed Contiguous Object）**：
   - 当 `{ ... }` 块准备闭合时：
     1. 标准库包裹层先循环检查 **`%unbound`**，对所有未命名的临时 `live` 子槽按 LIFO 逆序调用 `drop(%unbound)`（执行 `destroy` + `%drop`），并逆序 `drop` 所有 `__defer_k` 守卫、调用 `%drop(\__parent)`；
     2. 内核 `block` 闭合逻辑自动将所有处于 **`empty` 或 `uninitialized`** 状态的直接子槽**静默剥离**；
     3. 剩余所有处于 **`live`** 状态的已 `%bind` 直接子槽按各自的 `align` 对齐并紧密压平（flatten）为一块连续的字节内存，生成驻留在只读常量区的不可变复合 `Shape`，`R_B` 状态定格为 `live` 且整体形状永久锁定。

## 五、只读冻结（`%freeze`）与跨阶段/跨协程共享

| 机制 | 物理与语义本质 | 用途 |
| :--- | :--- | :--- |
| **`%freeze(ptr) -> frozen_ptr`** | 将不含外部可变引用或活跃句柄的对象子树深拷贝/迁入只读常量区（`.rodata`），返回 `mutability = 0` 的安全 `Pointer` | 模块、全局常量、编译期生成查找表、跨协程零拷贝只读共享 |
| **`cell` 协程** | 状态 `location` 归 `new` 创建的协程根帧私有（根帧未闭合，内部槽可持续演进），外部持 GC `Handle` 经 `@` 消息读写 | 跨闭包、跨协程的安全可变共享 |
| **胖指针只读借用** | 在标准库中构造 `mutability = 0` 的 `Pointer` 副本，通过 `%field` 自动传染给所有子字段访问 | 调用链内的深度只读视图 |

## 六、验收用例（自检清单）

1. **标准库基于 `{ %break(...) }` 无损实现 `move`**：`b := move(\a)` 展开为在子 `block` 中将外层 `a` 的字节读入临时槽并调用 `%drop` 将外层 `a` 置为 `empty`，随后通过 `%break` 将位段填入子 `block` 的 `self root`（作为外层块的未绑定 `%unbound` 直接子槽返回），外层 `b := ...` 直接 `%bind` 该槽成功！
2. **闭合对象内部 `mutable` 字段的直接覆写**：`p := { f := %allocate_val mutable (open("x")) }` 闭合后，对 `\p.f = open("y")` 赋值时，标准库先对新值求值，再调用 `destroy(\p.f)` 就地析构旧文件，随后直接调用 **`%write(\p.f, new_file)`** 原地写入新文件，全程 `p.f` 保持 `live` 无空洞，且无需 `%swap` 原语！
3. **单次分支初始化**：`x := %allocate i32; (\x = 1) if c else (\x = 2)` 中不可变槽 `x` 通过 `uninitialized → live` 完成初始化；此后写 `\x = 3` 因 `x` 非 `mutable` 而报错。
