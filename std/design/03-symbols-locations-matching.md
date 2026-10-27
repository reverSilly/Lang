# 03 - 本体、指针、形状与模式匹配

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)
后续：[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)

## 一、核心原则：单一表达式结果 `value` 与内部存储 `location`

语言严格区分 **“表达式求值产生的数据（`value`）”** 与 **“块内管理内存与生命周期的槽（`location`）”**：

1. **一切表达式求值结果皆为 `value`（纯 POD 数据）**：
   - 任何表达式求值的结果都是 `value`，在物理上由一段无所有权的纯字节（`bytes`）与其对应的形状描述指针（`shape`）构成。
   - **所有 `value` 均无身份、无生命周期、无所有权，永远可自由按位复制（`memcpy`）**。
   - **内置基础 `value` 类别**：
     - **`byte` / `bytes(N)`**：最小寻址单位（8 bits）及定长字节序列，是一切算术类型与内联缓冲区的物理底座；
     - **算术与逻辑值**：`integer`、`boolean` 等；
     - **`symbol`**：一等原子值（类似 Lisp symbol）。源码中的裸标识符 token `a` 即 `symbol` 字面量；
     - **`prototype`**：`^Expr` 产生的不透明纯代码值；
     - **`handle`**：协程句柄（`new` 返回值、`\^`）与帧句柄；
     - **`pointer`**：指向某个 `location`（或只读常量区对象）的胖指针值，逻辑结构为 **`(addr, shape_ptr, mutability)`**。
2. **`location` 是纯内部存储节点（不直接作为表达式结果出现）**：
   - `location` 是“一段内存及其所有权”的内部抽象，只存在于 `block` 的所有权树中，由 `allocate [mutable]` 或子 `block` 创建。
   - **表达式中从不直接持有 `location` 实体，而是持有指向该 `location` 的 `pointer` value**。所有操作 `location` 的内核原语均接收和返回 `pointer` value。

## 二、内存零 Header 模型、Shape 与统一胖指针

### 2.1 内存零 Header 与统一 Pointer 表示

- **内存中不存储 `[shape-reference]` 头**：每个 `location` 对应的物理内存中**只存储纯粹的数据字节（`bytes`）**，与 C 语言 `struct` 一样紧凑。
- **逻辑指针统一为 `(addr, shape_ptr, mutability)`**：
  - 因为内存中没有自解释头，指向任何 `location` 的 `pointer` value 在逻辑上都携带目标地址 `addr`、目标形状指针 `shape_ptr` 和访问权限 `mutability`；
  - **瘦指针作为编译期退化**：当编译器在编译期静态推导出目标的 `shape_ptr` 与 `mutability` 时，物理表示自动省去后两项，退化为 1 个机器字（`addr`）的瘦指针；只有在动态未知时才在运行期传递完整的胖指针。
- **`frozen` 对象 = 指向只读常量区的安全 `pointer`**：
  - 不存在独立的 `frozen` 类型；编译期固化或 `%freeze` 产生的只读对象住在常量区（`.rodata`），直接以 `mutability = immutable` 的安全 `pointer` 表示，永不悬垂且允许跨阶段固化。

### 2.2 描述性 Shape 与相对胖指针字段表

- **Shape 是纯粹描述性的（Descriptive）只读对象**：
  - 每个具体存在的对象都有唯一确定的描述性 `shape`（不存在模糊的 `any` shape 或并集 shape；形状约束与多态匹配属于 Pattern 的职责）。
  - 所有 `shape` 描述表都驻留在只读 `frozen` 区（全局去重/Interned），`%shape(x)` 返回的就是指向该描述表的只读指针 **`pointer(shape)`**。
  - 在 `block` 尚处于构建期（Constructing）时，内部实现维护可变的 shape 构建器随 `%bind` 动态扩展；一旦暴露给程序或当 `block` 闭合时，均呈现为不可变的 `shape` 对象。
- **复合 Shape 的内部结构 = `symbol → 相对胖指针`**：
  - 复合 `shape` 除记录总字节数 `size` 与对齐 `align` 外，其字段表将每个字段 `symbol` 映射为一个以 `0` 为基地址的**相对胖指针 `(offset, sub_shape_ptr, field_mutability)`**。
  - **字段访问 `%field(ptr, sym)`（`.`）即胖指针加法**：
    设父指针 `ptr` 为 `(base_addr, parent_shape, parent_mut)`，查 `parent_shape` 中符号 `sym` 得到 `(offset, sub_shape, field_mut)`，则 `%field(ptr, sym)` 直接返回：
    **`(base_addr + offset, sub_shape, min(parent_mut, field_mut))`**。
  - **消解 `\sym` 原语**：当前帧符号解析 `\sym` 不再是独立内核原语，它纯粹是 **`%field(%this, sym)`** 的语法糖！

### 2.3 内联多态（C Union / Rust Enum）与自定义 POD

1. **内联 Union = `bytes(N)` + Fat-Pointer 重解释**：
   - 标准库实现内联 `Union(ShapeA, ShapeB)` 时，在编译期计算 $N = \max(\text{ShapeA.size}, \text{ShapeB.size})$，构造普通闭合对象 `{ tag := ...; payload := allocate mutable bytes(N); \destroy := ... }`；
   - 访问时核对 `tag`（可直接存储当前 `pointer(shape)`），并通过指针形状重解释原语 `%recast(payload_ptr, target_shape_ptr)` 生成携带 `ShapeA` 或 `ShapeB` 的胖指针，零堆分配实现连续内联存储。
   - 语言**不引入 `unsafe` 关键字或语法传染**；指针重解释与悬垂指针解引用等操作在规范中标明由调用者承担有效性契约。
2. **自定义 POD 与 `$` 分派器**：
   - 内核提供无条件按位读取原语 **`%deref(ptr) -> value`**（按 `ptr.shape.size` 将目标内存拷贝为 `value`）；
   - 标准库实现读取算子 **`$ptr`**：检查 `ptr.shape` 是否为 POD（builtin POD 类型，或所有字段均为 POD 且不含 `\destroy` 字段的用户复合 shape）。若是则调用 `%deref(ptr)` 取出 `value`（使自定义小结构体如 `Point` 享受与内置整数完全相同的按值读取与传参体验）；若含非 POD 资源则报错拒绝。

## 三、组合式记号与全显式化原语分工

### 3.1 表面记号分解（零模式切换）

| 写法 | 底层展开 | 含义 | 结果（皆为 `value`） |
| :--- | :--- | :--- | :--- |
| `a` | 原子字面量 | `symbol` 值 `'a'` 本身 | `symbol` |
| `\this` | `%this` | 指向当前 `block` 的 `self root` 的指针 | `pointer` |
| `\a` | `%field(%this, a)` | 当前帧下名为 `a` 的直接子槽指针 | `pointer` |
| `\p.a` | `%field(%field(%this, p), a)` | 子字段 `a` 的指针 | `pointer` |
| `$\a` | `$(\a)` → 经 POD 检查调 `%deref(\a)` | 从 POD 槽中按位读出 `value` 副本 | 目标类型的 `value` |

### 3.2 内核显式原语 vs 标准库包装器（Wrapper）

为了使内核保持极致的最小化与无歧义，**内核原语不做隐式装箱、不自动调用析构钩子、不做类型分派**；高层便利语义全部由标准库在调用方当前环境（经 `evaluate`）组合完成：

| 操作领域 | 编译器内核原语（显式、轻量、单一职责） | 标准库包装器 / 分派器（面向用户语法） |
| :--- | :--- | :--- |
| **1. 装箱辅助** | `allocate [mutable]` + `%write(dst_ptr, val)` | **`box(val) -> ptr`**（在当前块 `evaluate`）：执行 `p := allocate; %write(p, val)` 并返回指向该新建直接子槽的 `ptr` |
| **2. 命名绑定** | **`%bind(sym, ptr)`**：仅接受指向**当前块未绑定直接子槽**的 `ptr`，将其登记入当前块符号表，返回 `()` | **`x := rhs`**：若 `rhs` 已是当前块未绑定的直接子槽 `ptr`（来自 `allocate` 或 `{...}`）则直接 `%bind(x, rhs)`；否则执行 `%bind(x, box(rhs))`（因此 `p := \a` 自然就是把已绑定指针 `\a` 装入新指针变量 `p`！） |
| **3. 按位写入** | **`%write(dst_ptr, val)`**：要求 `dst_ptr` 处于 `uninitialized` 或 `empty`（且可写），将 `val` 按位 `memcpy` 考入并置为 `live` | **`dst_ptr = val`**：若 `dst_ptr` 已处于 `live`（且 `mutable`），先调 stdlib `drop(dst_ptr)` 将其析构置为 `empty`，再调 `%write(dst_ptr, val)` |
| **4. 所有权转移** | **`%move(dst_ptr, src_ptr)`**：要求 `dst_ptr` 处于 `uninitialized` 或 `empty`（且可写），将 `src_ptr` 的内容移入 `dst_ptr`，`src_ptr` 置为 `empty` | **`move(dst_ptr, src_ptr)`**：若 `dst_ptr` 已处于 `live`（且 `mutable`），先调 stdlib `drop(dst_ptr)` 析构旧内容，再调 `%move(dst_ptr, src_ptr)` |
| **5. 销毁清理** | **`%drop(ptr)`**：要求 `ptr` 的所有子槽已处于 `empty`（或无子槽），仅负责将 `ptr` 自身状态置为 `empty` | **`drop(ptr)`**：先调 `ptr.\destroy` 钩子（若有），再按逆序对所有 `live` 子字段递归调用 `drop`，最后调 `%drop(ptr)` 置空 |
| **6. 深拷贝** | （无内核原语，纯靠反射与按位读写） | **`clone(dst_ptr, src_ptr)`**：对 POD 直接 `%write(dst_ptr, $src_ptr)`；对复合对象递归调用字段 `clone` 或自定义 `\clone` 协议 |

## 四、编译器内建原语全集

判据：**凡是需要触碰求值器私有结构（延迟代码、帧树、槽分配与状态位、协程栈、常量段）的必须入核；凡是递归遍历、析构调度与语法糖全部归标准库。**

| 分类 | 内核原语 | 签名与语义简述 |
| :--- | :--- | :--- |
| **代码与序列** | `^Expr` / `a ; b` | 唯一特殊形式生成 `prototype` / 顺序求值并丢弃前值 |
| **环境求值** | `evaluate` / `block` / `new` | 就地展开 `prototype` / 建子帧对象返回 `self root` `ptr` / 惰性建协程返回 `handle` |
| **帧与协程标识** | `%this` / `%caller` / `%in` | 当前块 `self root` 指针 / 恢复者协程 `handle` / 本协程来信槽 `ptr` |
| **槽分配与状态** | `allocate [mutable]` / `%bind` / `%move` / `%drop` | 分配直接子槽返回 `ptr` / 命名当前块未绑定子槽 / 移入空槽 / 将槽置为 `empty` |
| **内存读写** | `%write(dst_ptr, val)` / `%deref(ptr)` | 向空槽按位写入 `val` 并置 `live` / 从槽按位读出 `val` |
| **导航与形状** | `%field(ptr, sym)` / `%parent` / `%shape(val)` / `%recast(ptr, shape_ptr)` / `%freeze(ptr)` | 相对胖指针加法访问子字段 / 父帧指针 / 取 `shape` 只读指针 / 重解释指针形状 / 冻结为常量区只读指针 |
| **控制与切换** | `%branch(cond, then_p, else_p)` / `%break(handle, src_ptr)` / `%resume(coro, src_ptr)` | 透明二分支 / 弹帧并将 `src_ptr` 移入目标 `self root` / 将 `src_ptr` 移入目标 `%in` 并切换协程 |
| **基础运算** | 基础类型算术与按位相等 | 在 `byte`、`integer`、`boolean`、`symbol`、`pointer` 上的内建按位运算与判等 |

## 五、模式匹配（标准库纯函数）

**编译器内核不认识 Pattern。** 模式匹配完全由标准库纯函数与两个语法壳构成：

```
match(pattern, subject) -> Bindings | fail
```

1. **`value : pattern`（可反驳约束）**：
   - 求值 `value` 与 `pattern`（均为普通 `value` 表达式），调用 `match(pattern, value)`；
   - 若返回 `fail`：立即执行 `break <fail>` 终止当前 block（见 [04-control-flow.md](04-control-flow.md)）；
   - 若返回 `Bindings`（即 `(symbol, value)` 列表）：标准库提交循环依次在当前 block 执行 `%bind(sym, box(val))` 将所有绑定落入当前帧。
2. **`v ? pattern`（纯查询匹配）**：
   - 调用 `match(pattern, v)`，成功返回 `true`，失败返回 `false`；不执行绑定提交，无控制流副作用。
3. **Pattern 的自然消解**：
   - **裸 `symbol` 值 `a`**：匹配任意 `subject` 并产出绑定对 `(a, subject)`；
   - **普通 `value`（如字面量 `3` 或 `$\a`）**：直接按值相等（`eq`）匹配，无需内置 `Exactly` 关键字；
   - **带 `matcher` 字段的自定义 Pattern 对象**：调用 `pattern.matcher(subject)`，支持形状包含匹配（`<` / `>` / `=`）、组合子（`|` / `&`）、守卫 `If` 以及可逆算术匹配（`a + 1 : 3`）。

## 六、验收用例（自检清单）

1. `a := 3; $\a + 1`：`3` 经 `box(3)` 装入不可变槽并 `%bind` 到 `a`；`\a` 即 `%field(%this, a)` 返回指针，`$\a` 经 POD 检查调 `%deref(\a)` 得 `3`，加 `1` 得 `4`。
2. `a := 42; p := \a; $($\p)`：因为 `\a` 是指向已绑定槽的 `pointer` value，`p := \a` 将指针 `\a` 装入新槽 `p`；`$\p` 读出指针 `\a`，再 `$` 读出 `42`。
3. `Pt := { x := 1; y := 2 }`：因 `Pt` 所有字段皆为 POD 且无 `\destroy`，`$\Pt` 合法并按位读出整个 `Pt` 值；若对象含 `\destroy` 字段，则 `$obj` 被标准库拒绝。
4. `b := allocate mutable; \b = 1; \b = 2`：第二次 `=` 时，标准库检测到 `\b` 已处于 `live`，自动先调 `drop(\b)`（最终执行 `%drop(\b)` 置 `empty`），再调 `%write(\b, 2)`。
5. `a + 1 : 3`：通过 stdlib 可逆协议匹配成功，在当前块将 `a` 绑定到 `2`；`3 : 4` 匹配失败并 `break` 当前 block 返回 `fail`。
