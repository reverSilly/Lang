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
     - **算术与逻辑值**：`integer`、`usize`、`boolean` 等；
     - **`symbol`**：一等原子值（类似 Lisp symbol）。源码中的裸标识符 token `a` 即 `symbol` 字面量；
     - **`prototype`**：`^Expr` 产生的不透明纯代码值；
     - **`handle`**：专指协程句柄（`new` 返回值、`\^`）；
     - **`pointer`**：指向某个 `location`（或只读常量区对象）的普通胖指针值（**无独立的“帧指针”类型**，`\this`、`\in`、`%parent` 返回的均是普通 `pointer`），逻辑结构固定为三元组 **`(addr, shape_ptr, mutability)`**。
2. **`location` 是纯内部存储节点（不直接作为表达式结果出现）**：
   - `location` 是“一段内存及其所有权”的内部抽象，只存在于 `block` 的所有权树中，由 `allocate [mutable]` 或子 `block` 创建。
   - **表达式中从不直接持有 `location` 实体，而是持有指向该 `location` 的 `pointer` value**。所有操作 `location` 的内核原语均接收和返回 `pointer` value。

## 二、内存零 Header 模型、胖指针三元组与 `%shape` 区分

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

### 2.2 描述性 Shape、侵入式双向符号链与第 2 层自指不动点

- **Shape 是纯粹描述性的（Descriptive）只读对象**：
  - 每个具体存在的对象都有唯一确定的描述性 `shape`（不存在模糊的 `any` shape 或并集 shape；形状约束与多态匹配属于标准库 Pattern 的职责）。
  - 所有闭合对象与基础类型的 `shape` 描述表都驻留在只读 `frozen` 区（全局去重/Interned）。
- **底层 `LinkedHashMap` 与 `s.fields` 零投影合一**：
  - 在运行时内部，复合 `shape` 的字段表是一张侵入式 `LinkedHashMap<symbol, FieldNode>`（支持 $O(1)$ 按 `symbol` 哈希/Trie 查找、构建期 $O(1)$ `%bind` 插入、以及沿双向符号链 $O(n)$ 正/逆序遍历，**无需在内核引入 Array 或 Tuple**）。
  - 复合 `shape` 对象暴露的结构为：
    ```
    Shape := {
        size      : usize,
        align     : usize,
        first_sym : symbol,   !- 第一个字段的 symbol；空对象为特殊空符号 none -!
        last_sym  : symbol,   !- 最后一个字段的 symbol，供逆序遍历/析构使用 -!
        fields    : FieldsMap !- 底层 LinkedHashMap 本尊 -!
    }
    ```
  - 其中每个字段节点 `FieldNode` 的内存布局与语言层暴露的只读结构完全一致（不含运行期 `status`，纯静态布局描述）：
    ```
    FieldNode := {
        offset     : usize,
        sub_shape  : pointer, !- 指向子字段 shape 的只读指针 -!
        mutability : boolean,
        prev_sym   : symbol,  !- 前驱字段 symbol（首字段前驱为 none） -!
        next_sym   : symbol   !- 后继字段 symbol（末字段后继为 none） -!
    }
    ```
- **统一 `%field(ptr, sym)`（`.`）查询与 `contains` 语义**：
  - **作用于普通对象指针 `obj_ptr`**：在 `obj_ptr.shape_ptr` 指向的底层 `LinkedHashMap` 中以 $O(1)$ 查找 `sym`：
    - 若不存在（或构建期槽处于 `uninitialized`/`empty`），返回 **`null` 指针**（天然兼作 $O(1)$ 的 `contains` 判别）；
    - 若存在，取得 `FieldNode` 中的 `(offset, sub_shape, field_mut)`，直接通过胖指针加法返回：
      **`(obj_ptr.addr + offset, sub_shape, min(obj_ptr.mutability, field_mut))`**。
  - **作用于 `s.fields`**：复用同一个 `%field` 原语，在底层 `LinkedHashMap` 中查到 `sym` 对应的 `FieldNode` 后，直接返回指向该 `FieldNode` C 结构体的只读胖指针 `(node_addr, S_node, immutable)`，零运行时投影开销！
  - **消解 `\sym` 原语**：当前帧符号解析 `\sym` 纯粹是 **`%field(%this, sym)`** 的语法糖。
- **严格数学性质：第 2 层自指不动点 $\mathcal{S}(\mathcal{F}(\mathcal{S}(\mathcal{F}(s_0)))) \equiv \mathcal{S}(\mathcal{F}(s_0))$**：
  设任意对象 $O$（含字段 $k_1, \dots, k_n$）的形状为 $s_0 = \mathcal{S}(O)$。由于 `FieldNode` 的 5 个字段类型均为编译期固定的内置原子类型，其自身形状 $S_{\text{node}}$ 是全局唯一的常量。在 $s_1 = \mathcal{S}(s_0.\text{fields})$ 中，每个字段 $k_i$ 的偏移量归一为常数 $(i-1)\cdot\text{sizeof}(\text{FieldNode})$，子形状归一为常数 $S_{\text{node}}$，可变性归一为 `false`。因此求第 2 层形状 $s_2 = \mathcal{S}(s_1.\text{fields})$ 时，其所有字段的 `FieldNode` 与 $s_1$ 逐比特完全相等，在常量区驻留下满足 **$s_2 \equiv s_1$**，只需将 $s_1.\text{fields}$ 的形状指针指向 $s_1$ 自身即构成有限闭环。

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

### 3.2 编译器内建原语最小全集（Core Primitives）

判据：**凡是需要触碰求值器私有结构（延迟代码、帧树、构建期槽分配与状态位、按字节内存读写、协程栈、常量段）的必须入核；凡是 RAII 析构（`\destroy`）、移动（`move`）、交换（`swap`）、深拷贝（`clone`）、模式匹配与语法糖全部归标准库（[06-stdlib-foundations.md](06-stdlib-foundations.md)）。**

| 分类 | 内核原语 | 签名与语义简述 |
| :--- | :--- | :--- |
| **代码与序列** | `^Expr` / `a ; b` | 唯一特殊形式生成 `prototype` / 顺序求值并丢弃前值（无析构） |
| **环境求值** | `evaluate` / `block` / `new` | 就地展开 `prototype` / 建子帧对象返回 `self root` `ptr` / 惰性建协程返回 `handle` |
| **帧与协程标识** | `%this` / `%caller` / `%in` | 当前块 `self root` 指针（`\this`）/ 恢复者协程 `handle`（`\^`）/ 本协程来信槽指针（`\in`） |
| **槽分配与状态** | `allocate [mutable]` / `%bind(sym, val) -> bool` / `%drop(slot_ptr)` | 在当前构建块分配 `uninitialized` 直接子槽返回 `ptr` / 若 `val` 为当前块未绑定直接子槽 `ptr` 则绑定入符号表并返回 `true`，否则返回 `false` / 将构建期直接子槽置为 `empty`（不调 `\destroy`，闭合时自动剥离，兼作 `unbind`） |
| **按位内存读写** | `%write(dst_ptr, val)` / `%deref(ptr) -> val` | 将 `val` 的字节按位 `memcpy` 拷入可写目标槽 `dst_ptr`（`uninitialized`、`empty` 或 `mutable live`）并置 `live` / 从 `live` 槽无条件按 `ptr.shape_ptr.size` 按位读出 `val` |
| **导航与形状** | `%field(ptr, sym)` / `%parent` / `%shape(val)` / `%recast(ptr, shape_ptr)` / `%freeze(ptr)` | 相对胖指针加法访问子字段（不存在返 `null`）/ 零元原语返回当前块直接父帧指针 / 取 `val` 自身 `shape` 只读指针（指针所指对象之 `shape` 由 `ptr.shape_ptr` 即 `%shape(%deref(ptr))` 读取）/ 重解释指针形状 / 冻结为常量区只读指针 |
| **控制与切换** | `%branch(cond, then_p, else_p)` / `%break(target_ptr, val)` / `%resume(coro, src_ptr)` | 透明二分支 / 物理回退栈帧至 `target_ptr` 并将 `val` 写入其 `self root`（不调 `\destroy`）/ 原子将 `src_ptr` 移入目标 `%in` 并切换协程 |
| **基础运算** | 基础类型算术与按位相等 | 在 `byte`、`integer`、`boolean`、`symbol`、`pointer`、`handle` 上的内建按位运算与判等 |

## 四、验收用例（自检清单）

1. `s := (\p).shape_ptr`（或 `%shape(%deref(\p))`）取得指针 `\p` 所指向对象的 `shape`：`s.first_sym` 为首字段名，`s.fields.c` 在 `c` 不存在时返回 `null` 指针；且 `%shape((%shape(s.fields)).fields) == %shape(s.fields)`。
2. `\a` 展开为 `%field(%this, a)`，若当前块未绑定 `a` 或 `\a` 已被 `%drop` 置为 `empty`，则返回 `null` 指针。
3. `%bind(x, rhs)` 在 `rhs` 为当前块未绑定直接子槽指针（如 `allocate` 或 `{...}`）时绑定并返回 `true`；在 `rhs` 为普通整数 `42` 或已绑定指针 `\a` 时不产生副作用并返回 `false`。
