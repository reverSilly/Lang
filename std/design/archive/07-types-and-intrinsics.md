# 07 - 基础数据类型、透明自解释形状与 `#compile` 算术能力

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)
后续：[08-syntax.md](08-syntax.md)

## 一、核心原则：无子字段叶子 `Shape` 基底 + 零尺寸标记（ZST Marker）

1. **无子字段叶子 `Shape`（`size = N, align = A, fields = empty_map`）是 Core 唯一的物理归纳基底**：
   - 内核不提供 `%bytes` 原语，也不内置 `i32`、`u64`、`f64` 乃至独立的 `usize` 算术类型；
   - 在 Core 对象模型中，任何 `fields == empty_map` 且 `size = N, align = A` 的普通 `Shape` 契约上即指代一段占 `N` 字节、按 `A` 对齐的原始字节块（简记为 `bytes(N, A)`）。求值器在启动自举（Bootstrap）时仅需在 `ShapeArena` 中静态实例化 `1B/1`、`2B/2`、`4B/4`、`8B/8` 四个叶子 `Shape` 以支撑五大内建透明类型及位宽算术；标准库只需提供轻量辅助函数 `raw_bytes_shape(N, align)`，并在高层按需提供 `ByteArray` / `BitSet` / `Slice`。
2. **零尺寸标记（ZST Marker）区分同尺寸描述性 `Shape`**：
   - 由于 `Shape` 是纯粹描述性的（Descriptive），两个同样包装 `4B/4` 的类型（如 `i32` 与 `u32`）通过在结构中嵌入不同符号名的零尺寸空对象字段（如 `__i32_tag : {}` vs `__u32_tag : {}`）实现零内存开销的 `Shape` 区分与多态分派；且这些 `Shape` 直接由标准库构造，无需特殊元层能力。
3. **全内建类型透明化与 `Shape` 极简 4 字段自解释**：
   - `Shape`、`Pointer`、`Symbol`、`Prototype`、`Handle` 全部是以裸叶子位段为字段底座并带专属 ZST Marker 的透明结构（Core 不内置 `Boolean`，所有条件与可变性标志直接使用 `bytes(1, 1)` 且非零即真）；其中 `Shape` 仅包含 `{ size, align, fields: #compile(capabilities.map), __shape_tag }` 四个字段（彻底移除冗余的 `first_sym`/`last_sym` 与 `null_sym`）。
4. **`capabilities.intrinsics` 只操作裸 `bytes(N, N)`**：
   - `capabilities.intrinsics` 接收位宽参数 `N`、符号性 `signed` 及槽位符号名，直接读写裸 `bytes(N, N)` 并返回裸 `bytes` 结果；标准库通过 `.__raw` 字段拆封与重装，**完全无需任何“修改值的 shape”原语**。

## 二、物理底座、自解释 `Shape` 与零尺寸标记机制

### 2.1 自解释 `Shape` 结构（4 字段，24B）与无魔法叶子定义

`Shape` 对象显式暴露有序字段映射表 `fields`（由 `#compile(capabilities.map)` 提供查表、判空与正逆序遍历支持）：

```
Shape := {
    size        : bytes(8, 8),               ! 字节总大小 !
    align       : bytes(8, 8),               ! 字节对齐（2 的幂） !
    fields      : #compile(capabilities.map),! 有序映射：Symbol -> 相对胖指针 (offset, sub_shape, mut) !
    __shape_tag : {}
}
```

在此结构下，任意定长原始字节块形状没有任何黑魔法，标准库辅助构造函数直接表示为：

```
raw_bytes_shape(N, align) := {
    size        := N;
    align       := align;
    fields      := empty_map;
    __shape_tag := {}
}
```

- **为什么叶子 `Shape` 需要 `align`**：
  - 复合结构体闭合时的对齐要求由其所有子字段的对齐推导得出：$\text{struct.align} = \max(1, \max_{f \in \text{fields}} f.\text{align})$；
  - 普通字节缓冲区 `bytes(64, 1)` 保持 $1$ 字节对齐（嵌入结构体时不产生填充浪费），而标量底层存储如 `bytes(4, 4)`、`bytes(8, 8)` 则自动将 $4$ 字节或 $8$ 字节硬件对齐要求传染给包含它的任何上层结构体。
- **`fields == empty_map` 唯一判别与 POD 递归终点**：
  - 任何含子字段的复合对象必有 `fields ~= empty_map`；因此 `fields == empty_map` 100% 唯一标识了无子字段的原子字节块（当 $N = 0, \text{align} = 1$ 时与空块 `{}` 完全同一）；
  - 当标准库 `is_pod` 递归检查子字段时，遇到 `fields == empty_map` 即到达叶子节点并直接返回 `true`，彻底消除了“内置基础类型白名单”。

### 2.2 零尺寸标记（ZST Marker）与描述性 `Shape` 区分

```
i32 := #shape($({ __raw := $#allocate(raw_bytes_shape(4, 4), false); __i32_tag := {} }));
u32 := #shape($({ __raw := $#allocate(raw_bytes_shape(4, 4), false); __u32_tag := {} }));
f32 := #shape($({ __raw := $#allocate(raw_bytes_shape(4, 4), false); __f32_tag := {} }));
```

- **物理零开销**：`__i32_tag` 的 `size = 0, align = 1`，因此在闭合对象中 `size` 仍为严格 $4$ 字节、`align` 仍为 $4$ 字节，物理内存与裸 `bytes(4, 4)` 100% 相同；
- **零原语封装与拆包**：读取底层字节直接访问 `x.__raw`；将运算所得的裸 `bytes(4, 4)` 包装为 `i32` 直接构造 `{ __raw := raw_val; __i32_tag := {} }`。

## 三、全内建类型的透明化布局与 `capabilities.symbols`

### 3.1 五大内建类型的透明结构（零标准库反向依赖）

所有内建类型在 Runtime 自举时直接以 `1B/1`、`2B/2`、`4B/4`、`8B/8` 叶子 `Shape` 为字段底座（不依赖标准库的 `usize`、`u32` 或 `Bool`），实现 100% 自举闭环。内核不内置 `Boolean`：`#branch`、`#allocate`/`#allocate_val`、`#bind`、`#destroy` 与 `Pointer.mutability` 统一使用 `bytes(1, 1)`（`0x00` 为假、非零为真）；若标准库需要强类型 `Bool` 用于多态分派，可与 `i32`/`u8` 一样通过 `{ __raw: bytes(1, 1), __bool_tag: {} }` 自行定义：

| 内建类型 | 透明结构布局（64 位目标平台） | 大小 / 对齐 | 核心设计收益与自举关系 |
| :--- | :--- | :--- | :--- |
| **`Symbol`** | `{ __raw: bytes(4, 4), __sym_tag: {} }` | 4B / 4B | 源码静态 `Word` 在前端自动映射为 4 字节唯一 Intern ID；运行期比较仅为单条 4 字节相等比较 |
| **`Shape`** | `{ size: bytes(8, 8), align: bytes(8, 8), fields: capabilities.map, __shape_tag: {} }` | 24B / 8B | 极简 4 字段自解释只读对象，其 `shape_ptr` 指向只读常量区中的 `ShapeMeta`（形成不动点闭环） |
| **`Pointer`** | `{ addr: bytes(8, 8), shape_ptr: bytes(8, 8), mutability: bytes(1, 1), __ptr_tag: {} }` | 24B / 8B <br>（瘦化后 8B） | 彻底消灭指针拆包原语与 `%recast`：将指针装入槽位 `.p` 后直接读写 `.p.addr`、`.p.shape_ptr`、`.p.mutability`，在标准库即可完成反射基准指针构造、形状重解释与数组偏移寻址 |
| **`Prototype`** | `{ __raw: bytes(8, 8), __proto_tag: {} }` | 8B / 8B | 指向编译期或运行期只读代码段入口的纯代码值 |
| **`Handle`** | `{ __raw: bytes(8, 8), __handle_tag: {} }` | 8B / 8B | 指向 GC 托管协程控制块的句柄值 |

### 3.2 `Word` 自动转 `Symbol` 与 `capabilities.symbols`（动态符号池）

- **静态 `Word` 自动转为 `Symbol`**：源码中的裸 `Word`（如 `#bind(x, ...)`、`#field(p, a)`、`capabilities.intrinsics.add_int(4, a, b)` 中的标识符）在前端 Parse 阶段即自动映射为编译期驻留的 4 字节 `Symbol` ID，运行期无需任何字符串表查找。
- **动态文本 ↔ `Symbol` 互转收归 `capabilities.symbols`**：
  - Core 不再内置 `#sym_bytes` 或 `#intern_sym` 原语；
  - 需要动态反射序列化或动态加载外部模块的程序，通过显式注入的 **`capabilities.symbols`**（底层由全局 `Trie` 维护唯一性）获得 `intern`（字节序列 → `Symbol`）与 `name`（`Symbol` → 只读字节视图）能力；
  - **收益**：不引入 `capabilities.symbols` 的纯静态程序在编译产物中可彻底剥离（strip）全部符号文本表，实现零开销与沙箱可控。

## 四、参数化 `capabilities.intrinsics` 与标准库算术类型

### 4.1 按位宽 `N` 参数化且只操作裸 `bytes`

- `capabilities.intrinsics` 完全不认识 `i32`、`u64`、`usize`、`f64` 等标准库类型，仅操作物理叶子 `bytes(N, N)`（$N \in \{1, 2, 4, 8\}$）与符号性 `signed`（`bytes(1, 1)`，非零为有符号）；
- 所有生成函数均在 `#compile` 元层调用，接收编译期已知的字节宽度、符号性以及当前帧槽位 `Symbol` 常量，返回对应的纯代码 `Prototype`：

| 类别 | `capabilities.intrinsics` 生成函数 | 求值时读取的槽类型 → 返回的 `value` 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| **整数加减乘** | `add_int(N, lhs_sym, rhs_sym)` <br> `sub_int(N, lhs_sym, rhs_sym)` <br> `mul_int(N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(N, N)` | 补码加、减、低位乘对有符号与无符号完全同一 |
| **整数除余** | `div_int(signed, N, lhs_sym, rhs_sym)` <br> `rem_int(signed, N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(N, N)` | 由 `signed` 区分有符号（`sdiv`/`srem`）与无符号（`udiv`/`urem`） |
| **位运算与移位** | `bit_and` / `bit_or` / `bit_xor` / `bit_not` / `shl` / `shr(signed, N, lhs_sym, rhs_sym)` | `bytes(N, N) -> bytes(N, N)` | `shr` 由 `signed` 区分算术右移与逻辑右移 |
| **整数位宽转换** | `ext_int(signed, from_N, to_N, src_sym)` <br> `trunc_int(from_N, to_N, src_sym)` | `bytes(from_N, from_N) -> bytes(to_N, to_N)` | `ext_int` 由 `signed` 区分符号扩展（`sext`）与零扩展（`zext`） |
| **浮点四则运算** | `add_float` / `sub_float` / `mul_float` / `div_float(N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(N, N)` | $N \in \{4, 8\}$（IEEE 754 单/双精度） |
| **浮点与整数互转** | `ext_float` / `trunc_float` / `int_to_float(signed, int_N, float_N, src)` / `float_to_int(signed, float_N, int_N, src)` | `bytes(from_N, from_N) -> bytes(to_N, to_N)` | 覆盖 `f32 <-> f64` 及有/无符号整数与浮点互转 |
| **比较运算** | `eq_bytes` / `lt_int(signed, ...)` / `le_int(signed, ...)` / `eq_float` / `lt_float` / `le_float` | `(bytes(N, N), bytes(N, N)) -> bytes(1, 1)` | 返回 `bytes(1, 1)`（`0x01` 或 `0x00`），可直接供 `#branch` 使用，标准库亦可按需包装为带 tag 的 `Bool` |

```
! 标准库 i32 加法与符号扩展的底层构建示例（零特殊原语） !
add_i32_raw := #compile(capabilities.intrinsics.add_int(4, a, b));
sext_i16_i64_raw := #compile(capabilities.intrinsics.ext_int(true, 2, 8, x));
```

*(注：关于整数、浮点与字节串 `""` 字面量如何经由 Layer 1 语法钩子解析并构造为上述类型，见 [08-syntax.md](08-syntax.md)。)*

## 五、验收用例（自检清单）

1. **无子字段叶子 `Shape` 与 POD 递归终点**：`raw_bytes_shape(8, 8)` 是 `size == 8`、`align == 8`、`fields == empty_map` 的 4 字段普通 `Shape`；标准库 `is_pod` 递归检查子字段时以 `fields == empty_map` 为叶子终点。
2. **透明 `Pointer` 消除 `%recast`**：将胖指针 `p` 装入槽位后，直接通过 `.p.addr`、`.p.shape_ptr`、`.p.mutability` 读取其内部字段，并在标准库中直接构造新 `Pointer` 完成 `recast` 与 `z_ptr` 反射。
3. **`capabilities.symbols` 按需注入**：纯静态程序仅用 4 字节 `Symbol` ID 运行（零符号文本表开销）；需反射序列化或动态加载的模块显式注入 `capabilities.symbols` 完成 `Symbol <=> UTF-8` 互转。
4. **参数化 `capabilities.intrinsics` 裸 `bytes` 运算**：`#compile(capabilities.intrinsics.add_int(4, a, b))` 与 `ext_int(true, 2, 8, x)` 直接在裸 `bytes` 槽位上完成硬件运算，标准库经 `.__raw` 拆封与重装，全程无需内核认识 `i32`。
