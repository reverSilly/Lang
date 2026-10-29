# 03 - 本体、指针与形状（Shape）

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)
后续：[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)

## 一、核心原则：单一表达式结果 `value` 与内部存储 `location`

语言严格区分 **“表达式求值产生的数据（`value`）”** 与 **“块内管理内存与生命周期的槽（`location`）”**：

1. **一切表达式求值结果皆为 `value`（纯 POD 数据）**：
   - 任何表达式求值的结果都是 `value`，在物理上由一段无所有权的纯字节（`bytes`）与其对应的形状描述指针（`%shape(val)`）构成。
   - **所有 `value` 均无身份、无生命周期、无所有权，永远可自由按位复制（`memcpy`）**。
   - **内置基础 `value` 类别**：
     - **`byte` / `bytes(N)`**：最小寻址单位（8 bits）及定长字节序列，是一切算术类型与内联缓冲区的物理底座；
     - **算术与逻辑值**：`integer`、`usize`、`boolean` 等（关于基础类型的具体集合及其操作究竟采用内核原语、黑魔法还是经由 `compile` capability 导入标准库实现，见 [06-stdlib-foundations.md](06-stdlib-foundations.md) §5 待办议题）；
     - **`symbol`**：一等原子值（类似 Lisp symbol）。源码中的裸标识符 token `a` 即 `symbol` 字面量；
     - **`prototype`**：`^Expr` 产生的不透明纯代码值；
     - **`handle`**：专指协程句柄（`new` 返回值、`\^`）；
     - **`pointer`**：指向某个 `location`（或只读常量区对象）的普通胖指针值（**无独立的“帧指针”类型**，`\this`、`\in`、`%parent` 返回的均是普通 `pointer`），逻辑结构固定为三元组 **`(addr, shape_ptr, mutability)`**（绝不存储 `parent`）。
2. **`location` 是纯内部存储节点（不直接作为表达式结果出现）**：
   - `location` 是“一段内存及其所有权”的内部抽象，只存在于 `block` 的所有权树中，由 `allocate [mutable]` 或子 `block` 创建。
   - **表达式中从不直接持有 `location` 实体，而是持有指向该 `location` 的 `pointer` value**。所有操作 `location` 的内核原语均接收和返回 `pointer` value。

## 二、内存零 Header 模型、胖指针三元组与 Shape 零投影内省

### 2.1 内存零 Header 与胖指针 `(addr, shape_ptr, mutability)`

- **内存中不存储 `[shape-reference]` 头**：每个已闭合 `location` 对应的物理内存中**只存储纯粹的数据字节（`bytes`）**，与 C 语言 `struct` 100% 相同，零额外 Header 或控制块开销。
- **逻辑指针统一为 `(addr, shape_ptr, mutability)`**：
  - 因为内存中没有自解释头，指向任何 `location` 的 `pointer` value 在逻辑上都由三个分量组成：
    1. `ptr.addr`：目标内存的字节起始地址；
    2. `ptr.shape_ptr`：指向**目标 `location` 所存放对象之 `shape`** 的只读指针（`pointer(shape)`）；
    3. `ptr.mutability`：通过该指针访问目标的读写权限（`immutable` 或 `mutable`）。
  - **瘦指针作为编译期退化**：当编译器在编译期静态推导出目标的 `shape_ptr` 与 `mutability` 时，物理表示自动省去后两项，退化为 1 个机器字（`addr`）的瘦指针；只有在动态未知时才在运行期传递完整的胖指针。
- **严格区分 `%shape(val)` 与 `ptr.shape_ptr`**：
  - **`%shape(val) -> pointer(shape)`**：取**任意 `value` 自身**的形状指针（例如 `%shape(42)` 返回 `integer` 的 shape 指针；而对一个胖指针值 `ptr` 调用 `%shape(ptr)`，返回的是内置的 `pointer` shape 指针！）；
  - **`ptr.shape_ptr`（等价于 `%shape(%deref(ptr))`）**：取**胖指针 `ptr` 所指向的目标 `location` 内容**的形状指针（编译器直接读取胖指针三元组中的 `shape_ptr` 分量，无需实际拷贝目标内存）。
- **`frozen` 对象 = 指向只读常量区的安全 `pointer`**：
  - 不存在独立的 `frozen` 类型；编译期固化或 `%freeze` 产生的只读对象住在常量区（`.rodata`），直接以 `mutability = immutable` 的安全 `pointer` 表示，永不悬垂且允许跨阶段固化。

### 2.2 描述性 Shape、消除 `s.fields` 投影与单步符号游标

- **Shape 是纯粹描述性的（Descriptive）只读对象**：
  - 每个具体存在的对象都有唯一确定的描述性 `shape`（不存在模糊的 `any` shape 或并集 shape；形状约束与多态匹配属于标准库 Pattern 的职责）。
  - 所有闭合对象与基础类型的 `shape` 描述表都驻留在只读 `frozen` 区（全局去重/Interned）。
- **消除 `s.fields` 伪对象投影（基于代数同构的直接反射）**：
  - 在运行时底层，复合 `shape` 的 `LinkedHashMap` / `Trie` 中每个字段条目存储的 `(offset, sub_shape_ptr, field_mutability)`，在物理结构上就是一个**以 `0` 为基地址的相对胖指针（Relative Fat-Pointer）**！
  - 回看 `%field(ptr, sym)` 的定义：
    $$\%\text{field}((base\_addr, s, parent\_mut), sym) = (base\_addr + offset, sub\_shape, \min(parent\_mut, field\_mut))$$
  - 因此，**完全不需要在内存中构造 `s.fields` 这一层伪对象投影或生成第 2 层伴生形状 `sf`**：只需对一个 `base_addr = 0`、形状为 `s` 的基准胖指针直接调用现成的 `%field` 原语，它返回的胖指针 **`(0 + offset, sub_shape, field_mut)` 本身就已经完整携带了该字段在底层 HashMap 中的全部三元信息**（通过指针读取原语即可直接取出 `offset`、`sub_shape` 与 `mutability`）！
  - 若字段 `sym` 不存在于 `s`（或构建期槽处于 `uninitialized`/`empty`），`%field` 返回 **`null` 指针**（天然兼作 $O(1)$ 的 `contains` 判别）。
- **遍历字段名：首尾符号与单步游标原语**：
  - 底层 `LinkedHashMap` / `Trie` 只需在 `shape` 上暴露 `size`、`align`、`first_sym`、`last_sym`，并提供两个单步符号游标原语：
    - **`%next_sym(shape_ptr, sym) -> symbol`**：返回字段 `sym` 在定义序下的后继字段符号（末字段后继为特殊空符号 `none`）；
    - **`%prev_sym(shape_ptr, sym) -> symbol`**：返回字段 `sym` 在定义序下的前驱字段符号（首字段前驱为 `none`，供逆序析构遍历使用）。
  - 这一设计彻底省去了构造 `s.fields` 投影对象和伴生形状 `sf` 的开销，100% 贴合底层 C++ `Trie` / `LinkedHashMap` 的真实结构，且完全无需在内核引入 Array 或 Tuple。

### 2.3 内联多态（`Union` / `Optional`）与自定义 POD

1. **内联 `Union` 与 `Optional` = `bytes(N)` + `%recast`**：
   - 已闭合对象的普通字段恒为 `live`；若业务需要可空字段或变体类型，标准库通过内联 `Union(ShapeA, ShapeB)` / `Optional(T)` 实现：在编译期计算 $N = \max(\text{ShapeA.size}, \text{ShapeB.size})$，构造普通闭合对象 `{ tag := ...; payload := allocate mutable bytes(N); \destroy := ... }`；
   - 访问时核对 `tag`，并通过指针形状重解释原语 **`%recast(payload_ptr, target_shape_ptr)`** 生成携带目标 `shape` 的胖指针，零堆分配实现连续内联存储。
   - 语言**不引入 `unsafe` 关键字或语法传染**；指针重解释与悬垂指针解引用等操作在规范中标明由调用者承担有效性契约。
2. **自定义 POD 与 `$` 分派器**：
   - 内核提供无条件按位读取原语 **`%deref(ptr) -> value`**（不检查是否含 `\destroy`，直接按 `ptr.shape_ptr` 记录的 `size` 将目标内存拷贝为 `value`，供标准库在内部实现 `move`、`swap` 及 `$`）；
   - 标准库实现面向用户的读取算子 **`$ptr`**（见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）：通过读取 `ptr.shape_ptr` 检查目标是否为 POD（builtin POD 类型，或所有字段均为 POD 且不含 `\destroy` 字段的用户复合 shape），若是则调用 `%deref(ptr)` 取出 `value`，否则报错拒绝。

## 三、组合式记号与编译器内建原语全集

### 3.1 表面记号分解（零模式切换）

| 写法 | 底层展开 | 含义 | 结果（皆为 `value`） |
| :--- | :--- | :--- | :--- |
| `a` | 原子字面量 | `symbol` 值 `'a'` 本身 | `symbol` |
| `\this` | `%this` | 指向当前 `block` 的 `self root` 的普通胖指针 | `pointer` |
| `\a` | `%field(%this, a)` | 当前帧下名为 `a` 的直接子槽指针 | `pointer` |
| `\p.a` | `%field(%field(%this, p), a)` | 子字段 `a` 的指针 | `pointer` |
| `$\a` | `$(\a)` → 经 Stdlib POD 检查调 `%deref(\a)` | 从 POD 槽中按位读出 `value` 副本 | 目标类型的 `value` |

### 3.2 编译器内建原语最小全集（Core Primitives — 01-05 闭环版）

判据：**凡是需要触碰求值器私有结构（延迟代码、构建栈帧、槽分配与状态位、按字节内存读写与交换、协程栈、常量段）的必须入核；凡是 RAII 析构（`\destroy`）、多层 `break` 展开、深拷贝（`clone`）、模式匹配与语法糖全部归标准库（[06-stdlib-foundations.md](06-stdlib-foundations.md)）。**

| 分类 | Core 原语 | 签名与语义 |
| :--- | :--- | :--- |
| **1. 延迟与序列** | `^Expr` / `a ; b` | 唯一特殊形式生成 `prototype` / 顺序求值并丢弃前值（无析构） |
| **2. 环境求值** | `evaluate p` / `block p` / `new p` | 当前帧就地求值 / 开辟子构建帧并在闭合时剥离非 `live` 槽、压平为连续对象 / 惰性创建独立协程返回 `handle` |
| **3. 环境标识（皆零参）** | `%this` / `%parent` / `%caller` / `%in` | 当前构建帧指针（`\this`）/ 构建栈直接父帧指针 / 恢复者协程 `handle`（`\^`）/ 本协程来信槽指针（`\in`） |
| **4. 槽分配与三态演进** | `allocate [mutable]` / `%bind(sym, val)` / `%move(dst, src)` / `%drop(ptr)` | 分配构建期子槽 / 绑定未命名直接子槽（返回 `boolean`）/ 移入空槽（支持从活跃 `%parent` 移入并将源置 `empty`）/ 将构建期直接子槽置为 `empty`（零 RAII，要求子槽已全空） |
| **5. 比特读写与交换** | `%write(dst, val)` / `%deref(ptr)` / `%swap(p1, p2)` | 向空槽按位写入 `val` 并置 `live` / 从槽按位读出 `val` / 交换两个同 `shape` 的 `live` 槽内容（用于操作已闭合对象的可变字段，零空洞、零析构） |
| **6. 导航与形状反射** | `%field(ptr, sym)` / `%shape(val)` / `%next_sym(s, sym)` / `%prev_sym(s, sym)` / `%recast(ptr, s)` / `%freeze(ptr)` | 胖指针偏移寻址（不存在返 `null`；对 `base_addr = 0` 直接得相对胖指针）/ 取 `shape` 只读指针 / 符号链正逆序游标 / 重解释胖指针形状 / 冻结至只读常量区 |
| **7. 控制流与协程切换** | `%branch(c, p1, p2)` / `%break(src_ptr)` / `%resume(coro, src_ptr)` | 透明二路分支 / **仅退出当前最内层构建帧 `%this`** 并将 `src_ptr` 移入结果槽（零 RAII）/ 原子移入目标 `%in` 并切换协程 |
| **8. 基础类型与运算** | 待定（见 [06-stdlib-foundations.md](06-stdlib-foundations.md) §5） | 基础数据类型算术/比较操作究竟采用内核原语还是由标准库从 `compile` capability 获取对应 `prototype` 固化分派，待进一步收敛 |

## 四、验收用例（自检清单）

1. **零投影字段反射**：对形状 `s := (\p).shape_ptr` 构造 `base_addr = 0` 的基准胖指针 `z_ptr` 并调用 `%field(z_ptr, a)`，直接获得携带 `(offset, sub_shape, field_mut)` 的相对胖指针；配合 `%next_sym(s, a)` 与 `%prev_sym(s, b)` 即可正逆序遍历全部字段，无需 `s.fields` 投影对象。
2. `\a` 展开为 `%field(%this, a)`，若当前块未绑定 `a` 或 `\a` 已被 `%drop` 置为 `empty`，则返回 `null` 指针。
3. `%bind(x, rhs)` 在 `rhs` 为当前块未绑定直接子槽指针（如 `allocate` 或 `{...}`）时绑定并返回 `true`；在 `rhs` 为普通整数 `42` 或已绑定指针 `\a` 时不产生副作用并返回 `false`。
