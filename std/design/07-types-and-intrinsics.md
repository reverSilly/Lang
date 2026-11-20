# 07 - 基础数据类型、透明自解释形状与 `#compile` 算术能力

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)
后续：[08-syntax.md](08-syntax.md)

## 一、核心原则：万物皆 `bytes(N, align)` + 零尺寸标记（ZST Marker）

1. **`bytes(N, align = 1)` 是全语言唯一的物理数据原子（且无需 `%bytes` 原语）**：
   - 内核不内置 `i32`、`u64`、`f64` 乃至独立的 `usize` 算术类型；一切标量、内建句柄与复合对象在物理层均由 `bytes(N, align)` 组合而成；
   - `bytes(N, align)` 自身没有任何黑魔法，它只是一个 `first_sym = last_sym = null_sym` 且 `fields` 映射为空的普通 `Shape` 对象；需要未初始化字节槽时直接调用 `#allocate(bytes_shape(N, align), is_mut)`（配合 `#touch` 激活为 `live`）。
2. **零尺寸标记（ZST Marker）区分同尺寸描述性 `Shape`**：
   - 由于 `Shape` 是纯粹描述性的（Descriptive），两个同样包装 `bytes(4, 4)` 的类型（如 `i32` 与 `u32`）通过在结构中嵌入不同符号名的零尺寸空对象字段（如 `__i32_tag : {}` vs `__u32_tag : {}`）实现零内存开销的 `Shape` 区分与多态分派；且这些 `Shape` 直接由标准库构造，无需特殊元层能力。
3. **全内建类型透明化与 `Shape` 自解释化**：
   - `Shape`、`Pointer`、`Symbol`、`Boolean`、`Prototype`、`Handle` 全部是以裸 `bytes(N, align)` 为字段底座并带专属 ZST Marker 的透明结构；其中 `Shape` 显式包含 `fields: #compile(capabilities.map)` 字段，消除内部符号表的黑盒魔法，并允许标准库与用户直接构造自定义 `Shape` 对象。
4. **`capabilities.intrinsics` 只操作裸 `bytes(N, N)`**：
   - `capabilities.intrinsics` 接收位宽参数 `N`、符号性 `signed` 及槽位符号名，直接读写裸 `bytes(N, N)` 并返回裸 `bytes` 结果；标准库通过 `.__raw` 字段拆封与重装，**完全无需任何“修改值的 shape”原语**。

## 二、物理底座 `bytes(N, align)`、自解释 `Shape` 与零尺寸标记机制

### 2.1 自解释 `Shape` 结构与 `bytes(N, align)` 的无魔法定义

`Shape` 对象显式暴露其字段映射表 `fields`（由 `#compile(capabilities.map)` 提供底层查表与正逆序遍历支持）：

```
Shape := {
    size        : bytes(8, 8),               ! 字节总大小 !
    align       : bytes(8, 8),               ! 字节对齐（2 的幂） !
    first_sym   : Symbol,                    ! 首字段符号（无字段时为 null_sym） !
    last_sym    : Symbol,                    ! 末字段符号（无字段时为 null_sym） !
    fields      : #compile(capabilities.map),! 符号 -> 相对胖指针 (offset, sub_shape, mut) 映射与遍历 !
    __shape_tag : {}
}
```

在此结构下，任意定长字节块形状 `bytes(N, align = 1)` 没有任何特殊原语或黑魔法，直接表示为：

```
bytes_shape(N, align) := #freeze({
    size        := N;
    align       := align;
    first_sym   := null_sym;
    last_sym    := null_sym;
    fields      := empty_map;
    __shape_tag := {}
})
```

- **为什么 `bytes(N, align)` 需要 `align`**：
  - 复合结构体闭合时的对齐要求由其所有子字段的对齐推导得出：$\text{struct.align} = \max(1, \max_{f \in \text{fields}} f.\text{align})$；
  - 普通字节数组 `bytes(64, 1)` 保持 $1$ 字节对齐（嵌入结构体时不产生填充浪费），而标量底层存储如 `bytes(4, 4)`、`bytes(8, 8)` 则自动将 $4$ 字节或 $8$ 字节硬件对齐要求传染给包含它的任何上层结构体。
- **`null_sym` 唯一判别定理与 POD 递归终点**：
  - 任何含子字段的复合对象必有 `first_sym ~= null_sym`；因此 `first_sym == null_sym`（且 `last_sym == null_sym`）100% 唯一标识了无子字段的原子 `bytes(N, align)`（当 $N = 0, \text{align} = 1$ 时与空块 `{}` 完全同一）；
  - 当标准库 `is_pod` 递归检查子字段时，遇到 `first_sym == null_sym` 即到达叶子节点并直接返回 `true`，彻底消除了“内置基础类型白名单”。

### 2.2 零尺寸标记（ZST Marker）与描述性 `Shape` 区分

```
i32 := #shape($({ __raw := $#allocate(bytes_shape(4, 4), false); __i32_tag := {} }));
u32 := #shape($({ __raw := $#allocate(bytes_shape(4, 4), false); __u32_tag := {} }));
f32 := #shape($({ __raw := $#allocate(bytes_shape(4, 4), false); __f32_tag := {} }));
```

- **物理零开销**：`__i32_tag` 的 `size = 0, align = 1`，因此在闭合对象中 `size` 仍为严格 $4$ 字节、`align` 仍为 $4$ 字节，物理内存与裸 `bytes(4, 4)` 100% 相同；
- **零原语封装与拆包**：读取底层字节直接访问 `x.__raw`；将运算所得的裸 `bytes(4, 4)` 包装为 `i32` 直接构造 `{ __raw := raw_val; __i32_tag := {} }`。

## 三、全内建类型的透明化布局与全阶段符号池

### 3.1 六大内建类型的透明结构（零标准库反向依赖）

所有内建类型直接以裸 `bytes(N, align)` 为字段底座（不依赖标准库的 `usize` 或 `u32`），实现 100% 自举闭环：

| 内建类型 | 透明结构布局（64 位目标平台） | 大小 / 对齐 | 核心设计收益与自举关系 |
| :--- | :--- | :--- | :--- |
| **`Symbol`** | `{ __raw: bytes(4, 4), __sym_tag: {} }` <br>（全 `0` 字节保留给 `null_sym`） | 4B / 4B | 源码标识符即 4 字节唯一 Intern ID；运行期比较仅为单条 4 字节相等比较 |
| **`Boolean`** | `{ __raw: bytes(1, 1), __bool_tag: {} }` | 1B / 1B | 供内核 `#branch` 与 `#bind` 使用，同时与 `u8`/`i8` 保持严格的类型区分 |
| **`Shape`** | `{ size: bytes(8, 8), align: bytes(8, 8), first_sym: Symbol, last_sym: Symbol, fields: capabilities.map, __shape_tag: {} }` | 32B / 8B | 完全自解释的普通只读对象，其 `shape_ptr` 指向只读常量区中的 `ShapeMeta`（即 `Shape` 自身的形状，形成不动点闭环） |
| **`Pointer`** | `{ addr: bytes(8, 8), shape_ptr: bytes(8, 8), mutability: bytes(1, 1), __ptr_tag: {} }` | 24B / 8B <br>（瘦化后 8B） | 彻底消灭指针拆包原语与 `%recast`：将指针装入槽位 `.p` 后直接读写 `.p.addr`、`.p.shape_ptr`、`.p.mutability`，在标准库即可完成反射基准指针构造、形状重解释与数组偏移寻址 |
| **`Prototype`** | `{ __raw: bytes(8, 8), __proto_tag: {} }` | 8B / 8B | 指向编译期或运行期只读代码段入口的纯代码值 |
| **`Handle`** | `{ __raw: bytes(8, 8), __handle_tag: {} }` | 8B / 8B | 指向 GC 托管协程控制块的句柄值 |

### 3.2 全阶段统一符号池（`Trie` 动态驻留与 `Symbol <=> bytes` 双向转换）

编译期与运行期共享同一个求值模型，且动态反射序列化与动态加载外部 `Prototype`/模块均需在运行期对齐符号 ID：
- 编译期已知的所有静态符号在编译结束时固化入 `.rodata` 静态符号表；
- 运行时全局符号池由静态表初始化，并通过 `Trie` 树维护动态新增符号的唯一性，提供两个全阶段原语：
  - **`#sym_bytes(sym) -> Pointer`**：通过 $4$ 字节 ID 以 $O(1)$ 数组索引直接返回指向只读常量区/符号池中对应 UTF-8 `bytes(len, 1)` 的胖指针；
  - **`#intern_sym(bytes_ptr, len_bytes8) -> Symbol`**：在全局 `Trie` 中查找或插入长度为 `len` 的字节序列，返回全局唯一的 `Symbol`。

## 四、参数化 `capabilities.intrinsics` 与标准库算术类型

### 4.1 按位宽 `N` 参数化且只操作裸 `bytes`

- `capabilities.intrinsics` 完全不认识 `i32`、`u64`、`usize`、`f64` 等标准库类型，仅操作物理原子 `bytes(N, N)`（$N \in \{1, 2, 4, 8\}$）与符号性 `signed`（`Boolean`）；
- 所有生成函数均在 `#compile` 元层调用，接收编译期已知的字节宽度、符号性以及当前帧槽位 `Symbol` 常量，返回对应的纯代码 `Prototype`：

| 类别 | `capabilities.intrinsics` 生成函数 | 求值时读取的槽类型 → 返回的 `value` 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| **整数加减乘** | `add_int(N, lhs_sym, rhs_sym)` <br> `sub_int(N, lhs_sym, rhs_sym)` <br> `mul_int(N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(N, N)` | 补码加、减、低位乘对有符号与无符号完全同一 |
| **整数除余** | `div_int(signed, N, lhs_sym, rhs_sym)` <br> `rem_int(signed, N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(N, N)` | 由 `signed` 区分有符号（`sdiv`/`srem`）与无符号（`udiv`/`urem`） |
| **位运算与移位** | `bit_and` / `bit_or` / `bit_xor` / `bit_not` / `shl` / `shr(signed, N, lhs_sym, rhs_sym)` | `bytes(N, N) -> bytes(N, N)` | `shr` 由 `signed` 区分算术右移与逻辑右移 |
| **整数位宽转换** | `ext_int(signed, from_N, to_N, src_sym)` <br> `trunc_int(from_N, to_N, src_sym)` | `bytes(from_N, from_N) -> bytes(to_N, to_N)` | `ext_int` 由 `signed` 区分符号扩展（`sext`）与零扩展（`zext`） |
| **浮点四则运算** | `add_float` / `sub_float` / `mul_float` / `div_float(N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(N, N)` | $N \in \{4, 8\}$（IEEE 754 单/双精度） |
| **浮点与整数互转** | `ext_float` / `trunc_float` / `int_to_float(signed, int_N, float_N, src)` / `float_to_int(signed, float_N, int_N, src)` | `bytes(from_N, from_N) -> bytes(to_N, to_N)` | 覆盖 `f32 <-> f64` 及有/无符号整数与浮点互转 |
| **比较运算** | `eq_bytes` / `lt_int(signed, ...)` / `le_int(signed, ...)` / `eq_float` / `lt_float` / `le_float` | `(bytes(N, N), bytes(N, N)) -> bytes(1, 1)` | 返回 `bytes(1, 1)`（`0x01` 或 `0x00`），由标准库包装为 `Boolean` |

```
! 标准库 i32 加法与符号扩展的底层构建示例（零特殊原语） !
add_i32_raw := #compile(capabilities.intrinsics.add_int(4, a, b));
sext_i16_i64_raw := #compile(capabilities.intrinsics.ext_int(true, 2, 8, x));
```

*(注：关于整数、浮点与字节串 `""` 字面量如何经由 Layer 1 语法钩子解析并构造为上述类型，见 [08-syntax.md](08-syntax.md)。)*

## 五、验收用例（自检清单）

1. **`bytes(N, align)` 无魔法 `Shape` 与 POD 递归终点**：`bytes_shape(8, 8)` 是 `size == 8`、`align == 8`、`first_sym == null_sym`、`fields == empty_map` 的普通 `Shape`；标准库 `is_pod` 递归检查子字段时以 `first_sym == null_sym` 为叶子终点。
2. **透明 `Pointer` 消除 `%recast`**：将胖指针 `p` 装入槽位后，直接通过 `.p.addr`、`.p.shape_ptr`、`.p.mutability` 读取其内部字段，并在标准库中直接构造新 `Pointer` 完成 `recast` 与 `z_ptr` 反射。
3. **全阶段符号池 `Trie` 互转**：运行期调用 `#sym_bytes(sym)` 以 $O(1)$ 取得字段名 UTF-8 字节胖指针；调用 `#intern_sym(ptr, len)` 动态驻留外部字段名并与编译期静态符号共享同一套 `Symbol` ID。
4. **参数化 `capabilities.intrinsics` 裸 `bytes` 运算**：`#compile(capabilities.intrinsics.add_int(4, a, b))` 与 `ext_int(true, 2, 8, x)` 直接在裸 `bytes` 槽位上完成硬件运算，标准库经 `.__raw` 拆封与重装，全程无需内核认识 `i32`。
