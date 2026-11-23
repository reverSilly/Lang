# 03 - 本体、指针、形状与 Core 原语全集

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)
后续：[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)、[08-syntax.md](08-syntax.md)

## 一、核心原则：位段结果 `value`、逻辑槽 `location` 与 Core 零 POD 概念

语言在内核层只处理 **“带形状描述的纯位段数据（`value`）”** 与 **“块内管理存储与生命周期的逻辑槽（`location`）”**：

1. **一切表达式求值结果皆为 `value`（内核不区分 POD 与非 POD）**：
   - 任何表达式求值的结果都是 `value`，在物理上仅由一段定长字节与其对应的形状描述指针（`#shape(val)`）构成。
   - **Core 原语（`#deref`、`#write`、`#allocate_val`、`#break`、`#resume`）完全不认识也不检查 POD 或 `__destroy`**：在内核视角下，任何 `value` 都只是长度为 `(#shape(val)).size` 的纯字节序列。
   - **POD 是纯粹的标准库递归概念（以 `fields == empty_map` 为递归终点）**：
     - 全语言所有 `Shape` 树的叶子节点都是 `fields == empty_map`（无子字段的裸字节块或 `{}`）；
     - 因此在标准库中，若形状 `s` 不含 `__destroy`（或自定义拷贝钩子），且其 `fields`（`capabilities.map`）中的所有子字段形状递归皆为 POD（当 `fields` 为空时循环零次直接返回 `true`），则 `s` 为 POD。标准库算子 `$`、`=`、`break` 与 `@` 据此设置 POD Gate。
2. **`location` 是纯内部逻辑存储节点（不直接作为表达式结果出现）**：
   - `location` 是“一个存储槽及其所有权”的内部逻辑抽象，只存在于 `block` 的所有权树中，由 `#allocate(shape_ptr, is_mut)`、`#allocate_val(val, is_mut)` 或子 `#block` 创建。
   - **单个 `location` 终身形状固定**：任何 `location` 在创建那一刻就确定了 `Shape`（含 `size` 与 `align`）并分配好真实存储空间；单个 `location` 绝不改变 `Shape`（构建期变量改类型通过标准库 `drop` 旧槽 + 分配新槽 + `#bind` 重绑实现）。
   - **逻辑存储抽象（不强制绑定物理内存）**：语言规范仅定义 `location` 的逻辑状态转换与读写契约，**不要求所有 `location` 在任意时刻都严格对应一段物理内存（RAM）**——在逻辑上，“一段寄存器”与“一段物理内存”均可承载一个 `location`。
   - **表达式中从不直接持有 `location` 实体，而是持有指向该 `location` 的 `Pointer` value**。所有操作 `location` 的内核原语均接收和返回 `Pointer` value。

## 二、内存零 Header 模型、透明胖指针与 `#field` 寻址语义

### 2.1 内存零 Header 与全程真指针不变量

- **内存中不存储 `[shape-reference]` 头**：每个已闭合 `location` 对应的物理存储中**只存放纯粹的数据字节**，与 C 语言 `struct` 100% 相同，零额外 Header 或控制块开销。
- **逻辑指针统一为透明复合结构 `Pointer`**（完整字段布局见 [07-types-and-intrinsics.md](07-types-and-intrinsics.md) §3.1：`{ addr, shape_ptr, mutability, __ptr_tag }`）：
  - **全程真指针不变量**：因为 `#allocate` 与 `#allocate_val` 在创建槽位时均已知晓确切 `Shape` 并分配了真实空间，所以全语言任何合法非空指针（包括 `#this`、`#parent`、以及指向 `uninitialized` 槽的 `.x`）**100% 都是 `addr ~= 0, shape_ptr ~= 0` 的真 `Pointer`**！唯有查找不到字段或无匿名槽时才返回 `null`（`addr == 0, shape_ptr == 0`）。
  - **严格区分“访问被指对象的字段”与“访问 `Pointer` 结构体自身的字段”**：
    - **`#field(ptr, sym)` 永远在 `ptr.shape_ptr`（即被指对象 `T` 的 `Shape`）中查找 `sym`**：例如 `x := { a := 1 }` 时，`.x` 是指向该对象的 `Pointer`，故 `#field(.x, a)`（`.x.a`）查找的是对象内部的字段 `a`；
    - **要访问 `Pointer` 结构体自身的 `.addr` / `.shape_ptr` / `.mutability` 字段**，只需将该 `Pointer` 值放入一个槽位（如 `p := .x`——因 `.x` 已绑定，`:=` 自动将指针值装入新槽 `.p`），此时 `.p` 是**指向 `Pointer` 的指针**，于是 `.p.addr`（`#field(.p, addr)`）毫无歧义地访问 `Pointer` 结构体自身的 `addr` 字段。
  - **瘦指针作为编译期退化**：当编译器在编译期静态推导出目标的 `shape_ptr` 与 `mutability` 时，物理表示自动省去后两项，退化为 1 个机器字（`addr`）的瘦指针；只有在动态未知时才在运行期传递完整的胖指针。
- **严格区分 `#shape(val)` 与 `ptr.shape_ptr`**：
  - **`#shape(val) -> Pointer(Shape)`**：取**任意 `value` 自身**的形状指针；
  - **`ptr.shape_ptr`**：取**胖指针 `ptr` 所指向的目标 `location` 内容**的形状地址。
- **无 `#freeze` 与 `.rodata` 概念**：
  - 编译期常量（含字符串字面量 `Shape{ size = N, align = 1 }` 纯字节值、常量结构体、`Shape`、`IntrinsicProto`、`FfiProto`）直接通过 `ast.value` 按值嵌入 Standalone `Ast`；运行期跨帧存活的内存通过声明式 FFI 显式管理（如 `libc.malloc` / `libc.free`），不存在隐式永生冻结原语。

### 2.2 自解释 `Shape` 与 `#field` 的代数同构

- **`Shape` 是完全自解释的透明只读对象**（完整结构见 [07-types-and-intrinsics.md](07-types-and-intrinsics.md) §2.1）：仅包含 4 个字段 `{ size, align, fields: #compile(capabilities.map), __shape_tag }`（无冗余的 `first_sym`/`last_sym`）。
- **相对胖指针寻址与零投影反射**：
  - 在 `Shape.fields` 映射表中，每个字段条目存储的 `(offset, sub_shape_ptr, field_mutability)` 在物理结构上就是一个**以 `0` 为基地址的相对胖指针**；
  - `#field((base_addr, s, parent_mut), sym) = (base_addr + offset, sub_shape, min(parent_mut, field_mut))`；
  - 因此，只需构造一个 `addr = 0`、指向形状 `s` 的基准胖指针 `z_ptr` 并调用 `#field(z_ptr, sym)`，返回的胖指针即携带该字段的全部三元信息（不存在时返回 `addr = 0, shape_ptr = 0` 的 `null` 指针）。
- **字段正逆序遍历归属 `Shape.fields`（`capabilities.map`）**：
  - Core 不提供独立的游标原语；判空与正逆序遍历当前 `live` 字段符号全部通过 `Shape.fields`（`capabilities.map`）提供的接口完成。
  - 处于构建期的 `#this`，其 `fields` 游标仅包含当前处于 `live` 状态的已绑定槽（`uninitialized` 槽经 `#write` 或 `#touch` 激活后挂入；经 `#drop` 置空后立即摘除）。

### 2.3 内联多态（`Union` / `Optional`）与 `#touch` 原语

- 标准库通过 `#allocate(raw_bytes_shape(N, A), true)` 分配未初始化字节载荷，并调用 **`#touch(slot_ptr)`**（将构建期直接子槽的状态零开销翻转为 `live`，不改动内存字节）配合标准库 `recast` 实现零堆分配的内联 `Union` / `Optional`。

## 三、编译器内建原语唯一全集（Core Primitives）

判据：**凡是触碰求值器栈帧、槽三态机与内存读写的入核；凡是 RAII（`__destroy`）、POD Gate、`move`、`swap`、`recast`、字段 map 遍历（归 `capabilities.map`）、符号文本互转（归 `capabilities.symbols`）、算术指令（归 `capabilities.intrinsics`）、多层 `break`、模式匹配与所有语法糖（`.`、`$`、`{}`、`""` 等）全部归元层能力或标准库。**

Core 共包含 **2 个语法级原语** + **21 个 `#` 前缀 Builtin（4 个零参 + 17 个带参）**：

| 分类 | Core 原语 | 签名与语义 |
| :--- | :--- | :--- |
| **1. 延迟与序列（2）** | `^Expr` / `a ; b` | 唯一延迟特殊形式生成 `Prototype` / 顺序求值并丢弃前值（无析构） |
| **2. 环境求值与阶段（4）** | `#evaluate(p)` / `#block(p)` / `#new(p)` / `#compile(e)` | 当前帧就地求值 / 开辟子构建帧并在闭合时剥离 `uninit` 与 `empty` 槽、将 `live` 绑定槽压平为连续对象 / 惰性创建独立协程返回 `Handle` / 强制编译期求值并固化 |
| **3. 环境与槽游标（5，皆零参）** | `#this` / `#parent` / `#unbound` / `#caller` / `#in` | 当前构建帧指针 / 构建栈直接父帧指针 / 当前帧栈顶未 `#bind` 的匿名 `live` 子槽指针（无则 `null`）/ 恢复者协程 `Handle` / 本协程来信槽指针 |
| **4. 槽分配与状态演进（5）** | `#allocate(s, is_mut)` / `#allocate_val(v, is_mut)` / `#touch(ptr)` / `#bind(sym, val)` / `#drop(ptr)` | 按 `Shape` `s` 分配 `uninitialized` 真槽 / 按 `v` 直接分配 `live` 真槽 / 将直接子槽零开销置为 `live` / 绑定未命名直接子槽（返 `bytes(1, 1)`：`0x01`/`0x00`，若旧同名槽为 `empty` 则重绑）/ 将构建链直接子槽置为 `empty`（零 RAII） |
| **5. 比特读写（2，零 POD/RAII 检查）** | `#deref(ptr)` / `#write(dst_ptr, val)` | 从 `live` 槽无条件按位读出 `val` / 当 `dst_ptr.shape_ptr == (#shape(val)).addr` 且 `dst_ptr` 为 `uninitialized` 或 `mutable`（含已闭合对象的 `mutable live` 字段）时，无条件按位写入并置为 `live` |
| **6. 导航与反射（2）** | `#field(ptr, sym)` / `#shape(val)` | 胖指针偏移寻址 / 取值的透明 `Shape` 元数据 |
| **7. 控制流与协程切换（3）** | `#branch(c, p1, p2)` / `#break(val)` / `#resume(coro, val)` | 透明二路分支 / **仅退出当前最内层构建帧 `#this`** 并将 `val` 直接填入当前块 `self root` 结果槽（零 POD/RAII 检查）/ 原子地将 `val` 填入目标 `#in` 槽并切换协程（零 POD/RAII 检查） |

## 四、验收用例（自检清单）

1. **Core 极简收敛**：`move`、`swap`、`recast`、`bytes`、字段遍历游标与符号文本互转全部移出 Core；全内核仅由上表 2 + 21 个原语构成。
2. **POD 递归终点与 Stdlib Gate 自洽**：标准库检查 `Shape` 是否为 POD 时，以不含 `__destroy` 且 `fields == empty_map` 作为天然递归基底；`$`、`=`、`break`、`@` 在标准库入口通过 `is_pod` Gate 拦截未经 `move` 的非 POD 值。
3. **全程真指针**：`x := #allocate(i32, false)` 执行后，`.x` 立即返回 `addr ~= 0, shape_ptr == Shape(i32)` 的真 `Pointer`；未执行 `#write(.x, 42)` 或 `#touch(.x)` 前，`x` 不进入 `live` 字段游标，且 `#deref(.x)` 报错拒绝读取。
