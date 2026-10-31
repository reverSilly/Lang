# 03 - 本体、指针与形状（Shape）

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)
后续：[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)

## 一、核心原则：位段结果 `value`、逻辑槽 `location` 与 Core 零 POD 概念

语言在内核层只处理 **“带形状描述的纯位段数据（`value`）”** 与 **“块内管理存储与生命周期的逻辑槽（`location`）”**：

1. **一切表达式求值结果皆为 `value`（内核不区分 POD 与非 POD）**：
   - 任何表达式求值的结果都是 `value`，在物理上仅由一段定长字节与其对应的形状描述指针（`%shape(val)`）构成。
   - **Core 原语（`%deref`、`%write`、`%allocate_val`、`%break`、`%resume`）完全不认识也不检查 POD 或 `\destroy`**：在内核视角下，任何 `value` 都只是长度为 `(%shape(val)).size` 的纯字节序列。
   - **POD 是纯粹的标准库递归概念（以 `first_sym == null_sym` 为递归终点）**：
     - 当基础类型全部拆解为普通 `Shape` 后，全语言所有 `Shape` 树的叶子节点都是 `first_sym == null_sym`（即无子字段的 `bytes(N, align)` 或 `{}`）；
     - 因此在标准库中，判断形状 `s` 是否为 POD 拥有天然的递归终点：**若 `s` 不含 `\destroy`（或自定义拷贝钩子），且其 `first_sym .. last_sym` 中的所有子字段形状递归皆为 POD（当 `first_sym == null_sym` 时循环零次直接返回 `true`），则 `s` 为 POD**！标准库算子 `$`、`=`、`break` 与 `@` 据此设置 POD Gate，拦截未经 `move` 的非 POD 值隐式按位复制。
2. **`location` 是纯内部逻辑存储节点（不直接作为表达式结果出现）**：
   - `location` 是“一个存储槽及其所有权”的内部逻辑抽象，只存在于 `block` 的所有权树中，由 `%allocate [mutable] shape_ptr`、`%allocate_val [mutable] val` 或子 `block` 创建。
   - **单个 `location` 终身形状固定**：任何 `location` 在创建那一刻就确定了 `Shape`（含 `size` 与 `align`）并分配好真实存储空间；单个 `location` 绝不改变 `Shape`（构建期变量改类型通过标准库 `drop` 旧槽 + 分配新槽 + `%bind` 重绑实现）。
   - **逻辑存储抽象（不强制绑定物理内存）**：语言规范仅定义 `location` 的逻辑状态转换与读写契约，**不要求所有 `location` 在任意时刻都严格对应一段物理内存（RAM）**——在逻辑上，“一段寄存器”与“一段物理内存”均可承载一个 `location`。
   - **表达式中从不直接持有 `location` 实体，而是持有指向该 `location` 的 `Pointer` value**。所有操作 `location` 的内核原语均接收和返回 `Pointer` value。

## 二、内存零 Header 模型、透明胖指针与自解释 `Shape`

### 2.1 内存零 Header 与透明胖指针 `Pointer`

- **内存中不存储 `[shape-reference]` 头**：每个已闭合 `location` 对应的物理存储中**只存放纯粹的数据字节**，与 C 语言 `struct` 100% 相同，零额外 Header 或控制块开销。
- **逻辑指针统一为透明复合结构 `Pointer`**：
  - 指向任何 `location` 的 `Pointer` value 自身也是一个暴露了内部字段的普通对象（详见 [07-types-and-intrinsics.md](07-types-and-intrinsics.md) §3），直接以裸 `bytes` 为底座：
    1. `ptr.addr`（`bytes(8, 8)`）：目标存储的字节起始地址（或相对偏移量）；
    2. `ptr.shape_ptr`（`bytes(8, 8)`）：指向**目标 `location` 所存放对象之 `Shape`** 的只读地址；
    3. `ptr.mutability`（`bytes(1, 1)`）：通过该指针访问目标的读写权限（`0` 为 `immutable`，`1` 为 `mutable`）；
    4. `ptr.__ptr_tag`（`{}`）：零尺寸类型区分标记。
  - **全程真指针不变量**：因为 `%allocate(shape)` 与 `%allocate_val(val)` 在创建槽位时均已知晓确切 `Shape` 并分配了真实空间，所以全语言任何合法非空指针（包括 `%this`、`%parent`、以及指向 `uninitialized` 槽的 `\x`）**100% 都是 `addr ~= 0, shape_ptr ~= 0` 的真 `Pointer`**！唯有查找不到字段或无匿名槽时才返回 `null`（`addr == 0, shape_ptr == 0`）。
  - **严格区分“访问被指对象的字段”与“访问 `Pointer` 结构体自身的字段”**：
    - **`%field(ptr, sym)` 永远在 `ptr.shape_ptr`（即被指对象 `T` 的 `Shape`）中查找 `sym`**：例如 `x := { a := 1 }` 时，`\x` 是指向该对象的 `Pointer`，故 `%field(\x, a)`（`\x.a`）查找的是对象内部的字段 `a`；
    - **要访问 `Pointer` 结构体自身的 `.addr` / `.shape_ptr` / `.mutability` 字段**，只需将该 `Pointer` 值放入一个槽位（如 `p := %allocate_val(ptr)`，或直接写 `p := \x`——因 `\x` 已绑定，`:=` 会自动将指针值装入新槽 `\p`），此时 `\p` 是**指向 `Pointer` 的指针**（`(\p).shape_ptr == Shape(Pointer)`），于是 `%field(\p, addr)`（`\p.addr`）便毫无歧义地访问 `Pointer` 结构体自身的 `addr` 字段！
  - **瘦指针作为编译期退化**：当编译器在编译期静态推导出目标的 `shape_ptr` 与 `mutability` 时，物理表示自动省去后两项，退化为 1 个机器字（`addr`）的瘦指针；只有在动态未知时才在运行期传递完整的胖指针。
- **严格区分 `%shape(val)` 与 `ptr.shape_ptr`**：
  - **`%shape(val) -> Pointer(Shape)`**：取**任意 `value` 自身**的形状指针；
  - **`ptr.shape_ptr`（等价于 `%shape(%deref(ptr))` 的地址）**：取**胖指针 `ptr` 所指向的目标 `location` 内容**的形状地址。
- **`frozen` 对象 = 指向只读常量区的安全 `Pointer`**：
  - 不存在独立的 `frozen` 类型；编译期固化或 `%freeze` 产生的只读对象住在常量区（`.rodata`），直接以 `mutability = 0`（`immutable`）的安全 `Pointer` 表示，永不悬垂且允许跨阶段固化。

### 2.2 自解释 `Shape`（含 `fields: capabilities.map`）与零投影内省

- **`Shape` 是完全自解释（Self-Interpreting）的透明只读对象**：
  - 为了彻底消除“内置 Shape 藏着不可见的内部符号表”的黑盒魔法，`Shape` 显式包含 `fields` 映射字段（标准库与用户均可通过该结构直接构造自定义 `Shape` 对象）：
    ```
    Shape := {
        size       : bytes(8, 8),               !- 字节总大小 -!
        align      : bytes(8, 8),               !- 字节对齐要求（2 的幂） -!
        first_sym  : Symbol,                    !- 定义序首个 live 字段符号（无字段时为 null_sym） -!
        last_sym   : Symbol,                    !- 定义序末个 live 字段符号（无字段时为 null_sym） -!
        fields     : compile capabilities.map,  !- 符号到相对胖指针 (offset, sub_shape, mut) 的只读映射表 -!
        __shape_tag: {}                         !- 零尺寸 Shape 标记 -!
    }
    ```
- **原子 `bytes(N, align = 1)` 的 `Shape`（零 `%bytes` 原语）**：
  - `bytes(N, align = 1)` 没有任何黑魔法，它的 `Shape` 就是一个 `first_sym = last_sym = null_sym` 且 `fields` 为空映射表的普通 `Shape` 对象：
    `{ size: N, align: align, first_sym: null_sym, last_sym: null_sym, fields: empty_map, __shape_tag: {} }`。
  - **为什么无需特殊标识符即可识别 `bytes(N, align)`**：
    任何含子字段的复合对象必有 `first_sym ~= null_sym`；而 `first_sym == null_sym`（无子字段）时，它要么是 $N > 0$ 的原子字节块 `bytes(N, align)`，要么是 $N = 0, \text{align} = 1$ 的空对象 `{}`（即 `bytes(0, 1)`）。因此 **`first_sym == null_sym` 天然唯一标识原子字节块，且可直接作为标准库 POD 递归判定的终止基底**！
- **消除 `s.fields` 伪对象投影（基于代数同构的直接反射）**：
  - 在 `Shape.fields` 映射表中，每个字段条目存储的 `(offset, sub_shape_ptr, field_mutability)` 在物理结构上就是一个**以 `0` 为基地址的相对胖指针（Relative Fat-Pointer）**！
  - 回看 `%field(ptr, sym)` 的定义：
    $$\\%\text{field}((base\\_addr, s, parent\\_mut), sym) = (base\\_addr + offset, sub\\_shape, \min(parent\\_mut, field\\_mut))$$
  - 因此，只需构造一个 `addr = 0`、指向形状 `s` 的基准胖指针 `z_ptr` 并直接调用 `%field(z_ptr, sym)`，它返回的胖指针 **`(0 + offset, sub_shape ~= 0, field_mut)` 本身就已经完整携带了该字段在 `fields` 表中的全部三元信息**（不存在时返回 `addr = 0, shape_ptr = 0` 的 `null` 指针）！
- **遍历 `live` 字段名：首尾符号与单步游标原语**：
  - **`(%this).shape_ptr` 的 `first_sym .. last_sym` 链仅包含当前处于 `live` 状态的已绑定槽**（`uninitialized` 槽在经 `%write` 或 `%touch` 转为 `live` 时挂入链尾；在经 `%drop` 转为 `empty` 时从链中摘除）：
    - **`%next_sym(shape_ptr, sym) -> Symbol`**：返回字段 `sym` 在定义序下的后继 `live` 字段符号（末字段后继为 `null_sym`）；
    - **`%prev_sym(shape_ptr, sym) -> Symbol`**：返回字段 `sym` 在定义序下的前驱 `live` 字段符号（首字段前驱为 `null_sym`，供标准库逆序析构遍历使用，保证遍历到的 100% 是需要析构的 `live` 槽）。

### 2.3 内联多态（`Union` / `Optional`）与 `%touch` 原语

- **内联 `Union` 与 `Optional` = `%allocate(bytes_shape)` + `%touch` + 透明 `Pointer` `recast`**：
  - 若业务需要可空字段或变体类型，标准库通过内联 `Union(ShapeA, ShapeB)` / `Optional(T)` 实现：计算 $N = \max(\text{ShapeA.size}, \text{ShapeB.size})$ 与 $A = \max(\text{ShapeA.align}, \text{ShapeB.align})$，分配 `payload := %allocate mutable (bytes_shape(N, A))`，并调用 **`%touch(\payload)`**！
  - **`%touch(slot_ptr)` 原语的作用**：将构建期直接子槽 `slot_ptr` 的状态直接从 `uninitialized`（或 `mutable empty`）翻转为 `live`，**运行期不修改任何内存字节（$O(1)$ 零开销）**。这确保了即使 `Optional(T)` 处于 `None` 分支（未向 `payload` 写入任何数据），`payload` 槽也处于 `live` 状态而不会被 `block` 闭合剥离，同时其内部变体的 RAII 析构由 `Union`/`Optional` 自身的 `\destroy` 根据 `tag` 精确管理。

## 三、组合式记号与编译器内建原语全集

### 3.1 表面记号分解（零模式切换）

| 写法 | 底层展开 | 含义 | 结果（皆为 `value`） |
| :--- | :--- | :--- | :--- |
| `a` | 原子字面量（或经 `compile` 钩子分派） | `Symbol` 值 `'a'` 本身（数字形态在元层分派为对应数值） | `Symbol`（或数值 `value`） |
| `\this` | `%this` | 指向当前 `block` 的 `self root` 的普通胖指针 | `Pointer` |
| `\a` | `%field(%this, a)` | 当前帧下名为 `a` 的直接子槽指针 | `Pointer` |
| `\p.a` | `%field(%field(%this, p), a)` | 子字段 `a` 的指针 | `Pointer` |
| `$\a` | `$(\a)` → 经 Stdlib POD Gate 检查调 `%deref(\a)` | 从 POD 槽中按位读出 `value` 副本 | 目标类型的 `value` |

### 3.2 编译器内建原语最小全集（Core Primitives — 终极极简闭环版）

判据：**凡是触碰求值器栈帧、槽三态机与内存读写的入核；凡是 RAII（`\destroy`）、POD Gate、`move`、`swap`、`recast`、算术指令（归 `capabilities.intrinsics`）、多层 `break`、模式匹配全部归元层能力或标准库（见 [06-stdlib-foundations.md](06-stdlib-foundations.md) 与 [07-types-and-intrinsics.md](07-types-and-intrinsics.md)）。**

| 分类 | Core 原语 | 签名与语义 |
| :--- | :--- | :--- |
| **1. 延迟与序列** | `^Expr` / `a ; b` | 唯一特殊形式生成 `Prototype` / 顺序求值并丢弃前值（无析构） |
| **2. 环境求值** | `evaluate p` / `block p` / `new p` | 当前帧就地求值 / 开辟子构建帧并在闭合时由内核剥离 `uninit` 与 `empty` 槽、将 `live` 绑定槽压平为连续对象 / 惰性创建独立协程返回 `Handle` |
| **3. 环境与槽游标（皆零参）** | `%this` / `%parent` / `%unbound` / `%caller` / `%in` | 当前构建帧指针（`\this`）/ 构建栈直接父帧指针 / 当前帧栈顶未 `%bind` 的匿名 `live` 子槽指针（无则 `null`）/ 恢复者协程 `Handle`（`\^`）/ 本协程来信槽指针（`\in`） |
| **4. 槽分配与状态演进** | `%allocate [mutable] s` / `%allocate_val [mutable] val` / `%touch(ptr)` / `%bind(sym, val)` / `%drop(ptr)` | 按 `Shape` `s` 分配 `uninitialized` 真槽 / 按 `val` 直接分配 `live` 真槽 / 将直接子槽零开销置为 `live` / 绑定未命名直接子槽（返 `Boolean`，若旧同名槽为 `empty` 则重绑）/ 将构建链直接子槽置为 `empty`（零 RAII） |
| **5. 比特读写（零 POD/RAII 检查）** | `%deref(ptr) -> val` / `%write(dst_ptr, val)` | 从 `live` 槽无条件按位读出 `val` / 当 `$dst_ptr.shape_ptr == (%shape(val)).addr` 且 `dst_ptr` 为 `uninitialized` 或 `mutable`（含已闭合对象的 `mutable live` 字段）时，无条件按位写入并置为 `live` |
| **6. 导航、反射与符号池** | `%field(ptr, sym)` / `%shape(val)` / `%next_sym(s, sym)` / `%prev_sym(s, sym)` / `%freeze(ptr)` / `%sym_bytes(sym)` / `%intern_sym(ptr, len)` | 胖指针偏移寻址 / 取 `Shape` 只读指针 / `live` 符号链正逆序游标 / 冻结至只读常量区 / 符号与字节序列双向转换（全局 `Trie` 符号池） |
| **7. 控制流与协程切换** | `%branch(c, p1, p2)` / `%break(val)` / `%resume(coro, val)` | 透明二路分支 / **仅退出当前最内层构建帧 `%this`** 并将 `val` 直接填入当前块 `self root` 结果槽（零 POD/RAII 检查）/ 原子地将 `val` 填入目标 `%in` 槽并切换协程（零 POD/RAII 检查） |

## 四、验收用例（自检清单）

1. **Core 已彻底消去 `%move`、`%swap`、`%recast`、`%bytes`**：`move`（基于 `{ %break(...) }` 返回 `%unbound` 新槽）与 `swap` 由标准库用 `%allocate_val` + `%deref` + `%write` + `%drop` + `%break` 组合实现；`recast` 由透明 `Pointer` 构造实现；`bytes(N, align)` 直接作为 `first_sym == null_sym` 的普通 `Shape` 表示。
2. **POD 递归终点与 Stdlib Gate 自洽**：标准库检查 `Shape` 是否为 POD 时，以不含 `\destroy` 且 `first_sym == null_sym`（零子字段的 `bytes(N, align)` 或 `{}`）作为天然递归基底；`$`、`=`、`break`、`@` 在标准库入口通过 `is_pod` Gate 拦截未经 `move` 的非 POD 值。
3. **全程真指针**：`x := %allocate i32` 执行后，`\x` 立即返回 `addr ~= 0, shape_ptr == Shape(i32)` 的真 `Pointer`；未执行 `%write(\x, 42)` 或 `%touch(\x)` 前，`x` 不进入 `first_sym .. last_sym` 链，且 `%deref(\x)` 报错拒绝读取。
