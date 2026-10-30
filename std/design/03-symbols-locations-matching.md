# 03 - 本体、指针与形状（Shape）

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)
后续：[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)

## 一、核心原则：单一表达式结果 `value` 与逻辑存储 `location`

语言严格区分 **“表达式求值产生的数据（`value`）”** 与 **“块内管理存储与生命周期的逻辑槽（`location`）”**：

1. **一切表达式求值结果皆为 `value`（纯 POD 数据）**：
   - 任何表达式求值的结果都是 `value`，在物理上由一段无所有权的定长字节（`bytes(N, align)`）与其对应的形状描述指针（`%shape(val)`）构成。
   - **所有 `value` 均无身份、无生命周期、无所有权，永远可自由按位复制（`memcpy`）**。
   - **`bytes(N, align = 1)` 是全语言唯一的物理原子**：
     - 一切内建类型（`Shape`、`Pointer`、`Symbol`、`Boolean`、`Prototype`、`Handle`）与标准库算术类型（`i32`、`u64`、`f64`、`usize` 等）在物理上全部构建在 `bytes(N, align)` 与零尺寸标记字段（`{}`）之上（详见 [07-types-and-intrinsics.md](07-types-and-intrinsics.md)）。
2. **`location` 是纯内部逻辑存储节点（不直接作为表达式结果出现）**：
   - `location` 是“一个存储槽及其所有权”的内部逻辑抽象，只存在于 `block` 的所有权树中，由 `allocate [mutable]` 或子 `block` 创建。
   - **逻辑存储抽象（不强制绑定物理内存）**：语言规范仅定义 `location` 的逻辑状态转换与读写契约，**不要求所有 `location` 在任意时刻都严格对应一段物理内存（RAM）**——在逻辑上，“一段寄存器”与“一段物理内存”均可承载一个 `location`。
   - **表达式中从不直接持有 `location` 实体，而是持有指向该 `location` 的 `Pointer` value**。所有操作 `location` 的内核原语均接收和返回 `Pointer` value。

## 二、内存零 Header 模型、透明胖指针与 Shape 零投影内省

### 2.1 内存零 Header 与透明胖指针 `Pointer`

- **内存中不存储 `[shape-reference]` 头**：每个已闭合 `location` 对应的物理存储中**只存放纯粹的数据字节（`bytes`）**，与 C 语言 `struct` 100% 相同，零额外 Header 或控制块开销。
- **逻辑指针统一为透明复合结构 `Pointer`**：
  - 指向任何 `location` 的 `Pointer` value 自身也是一个暴露了内部字段的普通 POD 对象（详见 [07-types-and-intrinsics.md](07-types-and-intrinsics.md) §3），直接以裸 `bytes` 为底座（零标准库 `usize` 循环依赖）：
    1. `ptr.addr`（`bytes(8, 8)`）：目标存储的字节起始地址（或相对偏移量）；
    2. `ptr.shape_ptr`（`bytes(8, 8)`）：指向**目标 `location` 所存放对象之 `Shape`** 的只读地址；
    3. `ptr.mutability`（`bytes(1, 1)`）：通过该指针访问目标的读写权限（`0` 为 `immutable`，`1` 为 `mutable`）；
    4. `ptr.__ptr_tag`（`{}`）：零尺寸类型区分标记。
  - **直接字段读取与构造，消去专用指针原语**：由于 `Pointer` 自身是公开的复合 `Shape`，读取指针的 `addr`、`shape_ptr`、`mutability`，或者构造 `addr = 0` 的反射基准指针 `z_ptr`、实现指针重解释 `recast(ptr, target_shape)`、实现数组元素偏移寻址，全部在标准库中通过普通字段读写与对象构造完成！
  - **瘦指针作为编译期退化**：当编译器在编译期静态推导出目标的 `shape_ptr` 与 `mutability` 时，物理表示自动省去后两项，退化为 1 个机器字（`addr`）的瘦指针；只有在动态未知时才在运行期传递完整的胖指针。
- **严格区分 `%shape(val)` 与 `ptr.shape_ptr`**：
  - **`%shape(val) -> Pointer(Shape)`**：取**任意 `value` 自身**的形状指针（例如对一个胖指针值 `ptr` 调用 `%shape(ptr)`，返回的是 `Pointer` 这一类型的 `Shape` 指针！）；
  - **`ptr.shape_ptr`（等价于 `%shape(%deref(ptr))` 的地址）**：取**胖指针 `ptr` 所指向的目标 `location` 内容**的形状地址（直接读取胖指针结构中的 `shape_ptr` 字段，无需实际拷贝目标内存）。
- **`frozen` 对象 = 指向只读常量区的安全 `Pointer`**：
  - 不存在独立的 `frozen` 类型；编译期固化或 `%freeze` 产生的只读对象住在常量区（`.rodata`），直接以 `mutability = 0`（`immutable`）的安全 `Pointer` 表示，永不悬垂且允许跨阶段固化。

### 2.2 描述性 `Shape`、`bytes(N, align)` 基底表示与零投影内省

- **`Shape` 是纯粹描述性的（Descriptive）透明只读对象**：
  - 每个具体存在的对象都有唯一确定的描述性 `Shape`，驻留在只读 `frozen` 区（全局去重/Interned）。
  - `Shape` 自身也是普通只读对象，其公开布局为：
    ```
    Shape := {
        size       : bytes(8, 8), !- 字节总大小 -!
        align      : bytes(8, 8), !- 字节对齐要求（2 的幂） -!
        first_sym  : Symbol,      !- 定义序首个字段符号（无字段时为 null_sym） -!
        last_sym   : Symbol,      !- 定义序末个字段符号（无字段时为 null_sym） -!
        __shape_tag: {}           !- 零尺寸 Shape 标记 -!
    }
    ```
- **原子 `bytes(N, align = 1)` 的 `Shape` 与 `null_sym` 唯一判别定理**：
  - `bytes(N, align = 1)` 的 `Shape` 为 `{ size: N, align: align, first_sym: null_sym, last_sym: null_sym, __shape_tag: {} }`。
  - **为什么需要 `align`**：复合对象的对齐要求由其所有字段的最大对齐决定（$\text{align} = \max(1, \max_f f.\text{align})$）。支持 `bytes(N, align)`（缺省 `align = 1`）使得普通字节缓冲区 `bytes(64)` 保持 1 字节对齐，而标量底座 `bytes(4, 4)`、`bytes(8, 8)` 自动将 4/8 字节硬件对齐要求传染给包含它的上层复合结构体。
  - **为什么无需特殊标识符即可识别 `bytes(N, align)`**：
    1. 任何包含至少一个字段（哪怕是零尺寸标记字段 `{ __tag: {} }`）的复合对象，其 `first_sym` 与 `last_sym` 必然非空（`~= null_sym`）；
    2. 任何不含任何字段（`first_sym == null_sym`）的复合对象 `{}`，其 `size` 必然为 `0` 且 `align` 为 `1`——这与 `bytes(0, 1)` 在物理和语义上**完全同一**！
    3. 因此，**`first_sym == null_sym`（且 `last_sym == null_sym`）已经天然、唯一地标识了原子字节块 `bytes(size, align)`**，无需引入任何特殊的伪符号或额外标记位。
- **消除 `s.fields` 伪对象投影（基于代数同构的直接反射）**：
  - 在运行时底层，复合 `Shape` 的 `LinkedHashMap` / `Trie` 中每个字段条目存储的 `(offset, sub_shape_ptr, field_mutability)`，在物理结构上就是一个**以 `0` 为基地址的相对胖指针（Relative Fat-Pointer）**！
  - 回看 `%field(ptr, sym)` 的定义：
    $$\\%\text{field}((base\\_addr, s, parent\\_mut), sym) = (base\\_addr + offset, sub\\_shape, \min(parent\\_mut, field\\_mut))$$
  - 因此，**完全不需要在内存中构造 `s.fields` 这一层伪对象投影或生成第 2 层伴生形状 `sf`**：只需构造一个 `addr = 0`、指向形状 `s` 的基准胖指针 `z_ptr` 并直接调用现成的 `%field(z_ptr, sym)`，它返回的胖指针 **`(0 + offset, sub_shape, field_mut)` 本身就已经完整携带了该字段在底层 HashMap 中的全部三元信息**！
  - 若字段 `sym` 不存在于 `s`（或构建期槽处于 `uninitialized`/`empty`），`%field` 返回 **`null` 指针**（天然兼作 $O(1)$ 的 `contains` 判别）。
- **遍历字段名：首尾符号与单步游标原语**：
  - 配合 `Shape` 上的 `first_sym` 与 `last_sym`，内核仅提供两个单步符号游标原语：
    - **`%next_sym(shape_ptr, sym) -> Symbol`**：返回字段 `sym` 在定义序下的后继字段符号（末字段后继为空符号 `null_sym`）；
    - **`%prev_sym(shape_ptr, sym) -> Symbol`**：返回字段 `sym` 在定义序下的前驱字段符号（首字段前驱为 `null_sym`，供逆序析构遍历使用）。

### 2.3 内联多态（`Union` / `Optional`）与自定义 POD

1. **内联 `Union` 与 `Optional` = `bytes(N, A)` + 透明 `Pointer` 重解释**：
   - 已闭合对象的普通字段恒为 `live`；若业务需要可空字段或变体类型，标准库通过内联 `Union(ShapeA, ShapeB)` / `Optional(T)` 实现：计算 $N = \max(\text{ShapeA.size}, \text{ShapeB.size})$ 与 $A = \max(\text{ShapeA.align}, \text{ShapeB.align})$，构造普通闭合对象 `{ tag := ...; payload := allocate mutable; %write(\payload, %bytes(N, A)); \destroy := ... }`；
   - 由于 `%bytes(N, A)` 仅产出形状为 `bytes(N, A)` 的 `value` 而不强制清零字节内容，上述 `%write(\payload, %bytes(N, A))` 在运行期仅使槽位获得 `bytes(N, A)` 形状并进入 `live` 状态（零 `memset` 开销）；
   - 访问时核对 `tag`，并通过标准库 `recast(payload_ptr, target_shape_ptr)`（直接替换透明 `Pointer` 的 `shape_ptr` 字段）生成携带目标 `Shape` 的胖指针，零堆分配实现连续内联存储。
2. **自定义 POD 与 `$` 分派器**：
   - 内核提供无条件按位读取原语 **`%deref(ptr) -> value`**（不检查是否含 `\destroy`，直接按 `ptr.shape_ptr` 记录的 `size` 将目标槽内容读出为 `value`，供标准库在内部实现 `move`、`swap` 及 `$`）；
   - 标准库实现面向用户的读取算子 **`$ptr`**（见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）：通过 `ptr.shape_ptr` 检查目标是否为 POD（原子 `bytes` 或所有字段均为 POD 且不含 `\destroy` 字段的复合 `Shape`），若是则调用 `%deref(ptr)` 取出 `value`，否则报错拒绝。

## 三、组合式记号与编译器内建原语全集

### 3.1 表面记号分解（零模式切换）

| 写法 | 底层展开 | 含义 | 结果（皆为 `value`） |
| :--- | :--- | :--- | :--- |
| `a` | 原子字面量（或经 `compile` 钩子分派） | `Symbol` 值 `'a'` 本身（数字形态在元层分派为对应数值） | `Symbol`（或数值 `value`） |
| `\this` | `%this` | 指向当前 `block` 的 `self root` 的普通胖指针 | `Pointer` |
| `\a` | `%field(%this, a)` | 当前帧下名为 `a` 的直接子槽指针 | `Pointer` |
| `\p.a` | `%field(%field(%this, p), a)` | 子字段 `a` 的指针 | `Pointer` |
| `$\a` | `$(\a)` → 经 Stdlib POD 检查调 `%deref(\a)` | 从 POD 槽中按位读出 `value` 副本 | 目标类型的 `value` |

### 3.2 编译器内建原语最小全集（Core Primitives — 全闭环版）

判据：**凡是需要触碰求值器私有结构（延迟代码、构建栈帧、原子字节值产生、槽分配与状态位、按字节槽读写与交换、符号 Trie 池、协程栈、常量段）的必须入核；凡是基础算术指令生成（归 `compile` `capabilities.intrinsics`）、指针重解释 `recast`（归透明 `Pointer` 字段构造）、RAII 析构（`\destroy`）、多层 `break` 展开、深拷贝（`clone`）、模式匹配与语法糖全部归元层能力或标准库（见 [06-stdlib-foundations.md](06-stdlib-foundations.md) 与 [07-types-and-intrinsics.md](07-types-and-intrinsics.md)）。**

| 分类 | Core 原语 | 签名与语义 |
| :--- | :--- | :--- |
| **1. 延迟与序列** | `^Expr` / `a ; b` | 唯一特殊形式生成 `Prototype` / 顺序求值并丢弃前值（无析构） |
| **2. 环境求值** | `evaluate p` / `block p` / `new p` | 当前帧就地求值 / 开辟子构建帧并在闭合时剥离非 `live` 槽、压平为连续对象 / 惰性创建独立协程返回 `Handle` |
| **3. 环境标识（皆零参）** | `%this` / `%parent` / `%caller` / `%in` | 当前构建帧指针（`\this`）/ 构建栈直接父帧指针 / 恢复者协程 `Handle`（`\^`）/ 本协程来信槽指针（`\in`） |
| **4. 原子字节与槽演进** | `%bytes(N, align)` / `allocate [mutable]` / `%bind(sym, val)` / `%move(dst, src)` / `%drop(ptr)` | 返回 `shape = bytes(N, align)` 的 `value`（不保证零初始化，运行期零清零开销）/ 分配构建期子槽 / 绑定未命名直接子槽（返 `Boolean`）/ 移入空槽 / 将构建期直接子槽置为 `empty` |
| **5. 比特读写与交换** | `%write(dst, val)` / `%deref(ptr)` / `%swap(p1, p2)` | 向空槽按位写入 `val` 并置 `live` / 从槽按位读出 `val` / 交换两个同 `Shape` 的 `live` 槽内容（零空洞、零析构） |
| **6. 导航、反射与符号池** | `%field(ptr, sym)` / `%shape(val)` / `%next_sym(s, sym)` / `%prev_sym(s, sym)` / `%freeze(ptr)` / `%sym_bytes(sym)` / `%intern_sym(ptr, len)` | 胖指针偏移寻址 / 取 `Shape` 只读指针 / 符号链正逆序游标 / 冻结至只读常量区 / 符号与字节序列双向转换（基于全局 `Trie` 符号池） |
| **7. 控制流与协程切换** | `%branch(c, p1, p2)` / `%break(src_ptr)` / `%resume(coro, src_ptr)` | 透明二路分支 / **仅退出当前最内层构建帧 `%this`** 并将 `src_ptr` 移入结果槽（零 RAII）/ 原子移入目标 `%in` 并切换协程 |

## 四、验收用例（自检清单）

1. **`bytes(N, align)` 唯一判别与零清零开销**：`%bytes(4, 4)` 返回 `Shape` 为 `size = 4, align = 4, first_sym = null_sym, last_sym = null_sym` 的 `value`（运行期不强制清零）；而 `i32` 的 `Shape` `{ __raw: bytes(4, 4), __i32_tag: {} }` 具有 `size = 4, align = 4, first_sym = __raw, last_sym = __i32_tag`，无需特殊标示符即可由 `first_sym == null_sym` 区分裸字节与包装结构。
2. **零投影字段反射与标准库 `recast`**：由于 `Pointer` 是透明结构体 `{ addr, shape_ptr, mutability, __ptr_tag }`，标准库直接构造 `addr = 0` 的基准胖指针 `z_ptr` 并调用 `%field(z_ptr, a)` 即可反射字段；`recast(ptr, target_shape)` 直接替换 `shape_ptr` 字段即可完成，无需内核 `%recast` 原语。
3. `\a` 展开为 `%field(%this, a)`，若当前块未绑定 `a` 或 `\a` 已被 `%drop` 置为 `empty`，则返回 `null` 指针。
