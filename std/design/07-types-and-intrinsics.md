# 07 - 基础数据类型、透明内建形状与 `compile` 算术能力

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)

## 一、核心原则：万物皆 `bytes(N, align)` + 零尺寸标记（ZST Marker）

为了在保持语言内核（Core）极简与正交的同时拥有完整的静态/动态类型表达力，基础数据类型遵循四条核心设计公理：

1. **`bytes(N, align = 1)` 是全语言唯一的物理数据原子**：
   - 内核不内置 `i32`、`u64`、`f64` 乃至独立的 `usize` 算术类型；一切标量、内建句柄与复合对象在物理层均由 `bytes(N, align)` 组合而成。
2. **零尺寸标记（ZST Marker）区分同尺寸描述性 `Shape`**：
   - 由于 `Shape` 是纯粹描述性的（Descriptive），两个同样包装 `bytes(4, 4)` 的类型（如 `i32` 与 `u32`）通过在结构中嵌入不同符号名的零尺寸空对象字段（如 `__i32_tag : {}` vs `__u32_tag : {}`）实现零内存开销的 `Shape` 区分与多态分派；且这些 `Shape` 直接由标准库构造，无需特殊元层能力。
3. **全内建类型透明化（Exposed Layout）**：
   - `Shape`、`Pointer`、`Symbol`、`Boolean`、`Prototype`、`Handle` 不再是黑盒幻影类型，而是直接以裸 `bytes(N, align)` 为字段底座并带专属 ZST Marker 的普通 POD 结构。这不仅消除了 Core 对标准库 `usize` 的反向依赖，还直接消去了读取指针分量或指针重解释（`%recast`）等专用内核原语。
4. **`capabilities.intrinsics` 只操作裸 `bytes(N, N)`，不认识标准库算术类型**：
   - `capabilities.intrinsics` 接收位宽参数 `N`、符号性 `signed` 及槽位符号名，直接读写裸 `bytes(N, N)` 并返回裸 `bytes` 结果；标准库通过访问包装结构的 `.__raw` 字段传入操作数，并将返回的 `bytes` 直接装回 `{ __raw := ..., __<type>_tag := {} }`，**完全无需任何“修改值的 shape”原语**。

## 二、物理底座 `bytes(N, align)` 与零尺寸标记机制

### 2.1 `%bytes(N, align)` 原语、`Shape` 表示与 `null_sym` 唯一判别

内核提供原子字节值产生原语 **`%bytes(N, align) -> value`**（当在标准库中省略 `align` 时缺省为 `1`，要求 `align` 为 $2$ 的幂）：
- **返回未指定字节内容的 `value`（默认不保证零初始化）**：
  - `%bytes(N, align)` 的求值结果是一个 `shape = bytes(N, align)` 的普通 POD `value`，**语义上不保证其字节内容为零（内容未指定）**；
  - 因为对裸 `bytes` 而言任何比特模式均为合法值，当通过 `%write(\slot, %bytes(N, align))` 将其写入槽位时，该槽位即刻获得 `bytes(N, align)` 形状并转入 `live` 状态，**运行期无需执行 `memset(0)` 清零（真正 $O(1)$ 零开销，非常适合 `Union` 内联载荷、未初始化容器缓冲区与类型占位）**；
  - **编译期确定性**：在 `compile` 期求值或跨阶段固化（Reification）进 `.rodata` 时，求值器内部统一填零，确保编译期求值 100% 确定且可缓存。
- **`bytes(N, align)` 的 `Shape` 对象结构**：
  ```
  {
      size        : N,         !- 字节长度（bytes(8, 8)） -!
      align       : align,     !- 字节对齐（bytes(8, 8)） -!
      first_sym   : null_sym,  !- 原子字节块无子字段 -!
      last_sym    : null_sym,  !- 原子字节块无子字段 -!
      __shape_tag : {}
  }
  ```

- **为什么 `bytes(N, align)` 需要 `align`**：
  - 复合结构体闭合时的对齐要求由其所有子字段的对齐推导得出：
    $$\text{struct.align} = \max\left(1, \max_{f \in \text{fields}} f.\text{align}\right)$$
  - 将 `align` 作为 `bytes(N, align)` 的基础属性后，普通字符/字节数组 `bytes(64, 1)` 保持 $1$ 字节对齐（嵌入结构体时不产生填充浪费），而标量底层存储如 `bytes(4, 4)`、`bytes(8, 8)` 则自动将 $4$ 字节或 $8$ 字节硬件对齐要求传染给包含它的任何上层结构体！
- **为什么无需特殊标示符即可唯一识别 `bytes(N, align)`（`null_sym` 完备性定理）**：
  - **当 $N > 0$ 时**：任何复合对象 `{ ... }` 的 `size` 均来自其内部字段。若一个复合对象没有任何字段（`first_sym == null_sym`），其 `size` 必然为 `0`；反之，任何 `size > 0` 的复合对象（或含 ZST 标记的包装类型）必然至少拥有一个字段，故其 `first_sym ~= null_sym`。因此 **`first_sym == null_sym`（且 `last_sym == null_sym`）已 100% 唯一标识了原子 `bytes(N, align)`**，绝不会与任何复合结构体发生碰撞！
  - **当 $N = 0, \text{align} = 1$ 时**：`bytes(0, 1)` 的形状为 `{ size: 0, align: 1, first_sym: null_sym, last_sym: null_sym }`，这与空块 `{}`（`block ^()`）闭合出的空对象形状**完全同一**。将 `bytes(0, 1)` 与空对象 `{}` 统一为同一个零尺寸单位形状，消除了多余的边界特例。

### 2.2 零尺寸标记（ZST Marker）与描述性 Shape 区分

在纯描述性 `Shape` 体系中，若 `i32` 与 `u32` 都仅定义为 `{ __raw : bytes(4, 4) }`，两者的字段名、偏移量与子形状完全相同，全局 Intern 后会坍缩为同一个 `Shape`。

解决方案是在标准库中直接用空对象 `{}`（`size = 0, align = 1`）作为**零尺寸名义标记字段（ZST Marker）**：

```
i32 := { __raw := %bytes(4, 4); __i32_tag := {} };
u32 := { __raw := %bytes(4, 4); __u32_tag := {} };
f32 := { __raw := %bytes(4, 4); __f32_tag := {} };
```

- **物理零开销**：`__i32_tag` 的 `size = 0, align = 1`，因此在闭合对象中 `size` 仍为严格 $4$ 字节、`align` 仍为 $4$ 字节，物理内存与裸 `bytes(4, 4)` 100% 相同；
- **零原语封装与拆包**：读取底层字节直接访问 `x.__raw`；将运算所得的裸 `bytes(4, 4)` 包装为 `i32` 直接构造 `{ __raw := raw_val; __i32_tag := {} }`，**无需任何“修改值的 shape”原语**！

## 三、全内建类型的透明化布局与全阶段符号池

### 3.1 六大内建类型的透明结构（零标准库反向依赖）

所有内建类型直接以裸 `bytes(N, align)` 为字段底座（不依赖标准库的 `usize` 或 `u32`），实现 100% 自举闭环：

| 内建类型 | 透明结构布局（64 位目标平台） | 大小 / 对齐 | 核心设计收益与自举关系 |
| :--- | :--- | :--- | :--- |
| **`Symbol`** | `{ __raw: bytes(4, 4), __sym_tag: {} }`<br>（全 0 字节保留给 `null_sym`） | `4B` / `4B` | 源码标识符即 4 字节唯一 Intern ID；运行期比较仅为单条 4 字节相等比较 |
| **`Boolean`** | `{ __raw: bytes(1, 1), __bool_tag: {} }` | `1B` / `1B` | 供内核 `%branch` 与 `%bind` 使用，同时与 `u8`/`i8` 保持严格的类型区分 |
| **`Shape`** | `{ size: bytes(8, 8), align: bytes(8, 8), first_sym: Symbol, last_sym: Symbol, __shape_tag: {} }` | `24B` / `8B` | `Shape` 自身也是普通只读对象，其 `shape_ptr` 指向只读常量区中的 `ShapeMeta`（即 `Shape` 自身的形状，形成不动点闭环） |
| **`Pointer`** | `{ addr: bytes(8, 8), shape_ptr: bytes(8, 8), mutability: bytes(1, 1), __ptr_tag: {} }` | `24B` / `8B`<br>（瘦化后 `8B`） | **彻底消灭指针拆包原语与 `%recast`**：直接读写 `.addr`、`.shape_ptr`、`.mutability` 即可在标准库完成反射基准指针构造、形状重解释与数组偏移寻址 |
| **`Prototype`** | `{ __raw: bytes(8, 8), __proto_tag: {} }` | `8B` / `8B` | 指向编译期或运行期只读代码段入口的纯代码值 |
| **`Handle`** | `{ __raw: bytes(8, 8), __handle_tag: {} }` | `8B` / `8B` | 指向 GC 托管协程控制块的句柄值 |

### 3.2 全阶段统一符号池（`Trie` 动态驻留与 `Symbol <=> bytes` 双向转换）

为什么符号池能力应当**同时支持编译期与运行期**（作为内核基础原语），而非仅限编译期？
1. **“单一求值器”哲学**：编译期与运行期共享同一个求值模型。若动态反射序列化（如运行期将任意对象序列化为 JSON、或从外部配置动态 `%bind` 构造对象）以及**动态加载外部 `prototype` / 动态模块 `import`** 需要在运行期对齐符号 ID，运行时本来就需要维护全局唯一的符号 `Trie` 树。
2. **静态零开销 + 动态按需驻留**：
   - 编译期已知的所有静态符号在编译结束时直接固化入二进制 `.rodata` 段的静态符号表，运行期符号比较（`sym1 == sym2`）与字段查找永远只是 $4$ 字节 ID 操作；
   - 运行时全局符号池由静态表初始化，并通过 `Trie` 树维护动态新增符号的唯一性（Uniqueness），提供两个全阶段通用的轻量原语：
     - **`%sym_bytes(sym) -> Pointer`**：通过 $4$ 字节 ID 以 $O(1)$ 数组索引直接返回指向只读常量区/符号池中对应 UTF-8 `bytes(len, 1)` 的胖指针（`ptr.shape_ptr` 的 `size` 即为字节长度）；
     - **`%intern_sym(bytes_ptr, len_bytes8) -> Symbol`**：在全局 `Trie` 中查找或插入长度为 `len` 的字节序列，返回全局唯一的 `Symbol`。

## 四、参数化 `capabilities.intrinsics` 与标准库算术类型

### 4.1 为什么 `capabilities.intrinsics` 应该按位宽 `N` 参数化且只操作裸 `bytes`？

1. **彻底解耦内核与标准库类型名**：`capabilities.intrinsics` **完全不需要认识 `i32`、`u64`、`usize`、`f64` 等标准库类型**！它只认识物理原子 `bytes(N, N)`（其中 $N \in \{1, 2, 4, 8\}$）与符号性 `signed`（`Boolean`）。
2. **消除组合爆炸**：将 `add_i8/i16/i32/i64/u8/u16/u32/u64` 与 `sext_<From>_<To>` 收敛为按 `(N, lhs_sym, rhs_sym)` 或 `(signed, from_N, to_N, src_sym)` 参数化的正交生成函数，使元层内建接口从上百个骤减至十余个！
3. **基于 `.__raw` 的零原语包装**：
   标准库算术类型（如 `i32 := { __raw: bytes(4, 4), __i32_tag: {} }`）在实现加法时，先将左右操作数的 `.__raw`（均为 `bytes(4, 4)`）绑定到局部槽 `a`、`b`，调用 `evaluate (compile capabilities.intrinsics.add_int(4, a, b))` 得到 `bytes(4, 4)` 结果，再直接装入 `{ __raw := ..., __i32_tag := {} }` 返回。

### 4.2 `capabilities.intrinsics` 参数化接口全表

所有生成函数均在 `compile` 元层调用，接收编译期已知的字节宽度（`1` / `2` / `4` / `8`）、符号性 `signed`（`true` 为有符号补码，`false` 为无符号）以及当前帧槽位 `Symbol` 常量，返回对应的纯代码 `Prototype`：

| 类别 | `capabilities.intrinsics` 生成函数 | 求值时读取的槽类型 → 返回的 `value` 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| **整数加减乘** | `add_int(N, lhs_sym, rhs_sym)`<br>`sub_int(N, lhs_sym, rhs_sym)`<br>`mul_int(N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(N, N)` | 补码加、减、低位乘对有符号与无符号完全同一，无需 `signed` 参数 |
| **整数除余** | `div_int(signed, N, lhs_sym, rhs_sym)`<br>`rem_int(signed, N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(N, N)` | 由 `signed` 区分有符号（`sdiv`/`srem`）与无符号（`udiv`/`urem`） |
| **位运算与移位** | `bit_and(N, lhs_sym, rhs_sym)`<br>`bit_or(N, lhs_sym, rhs_sym)`<br>`bit_xor(N, lhs_sym, rhs_sym)`<br>`bit_not(N, src_sym)`<br>`shl(N, lhs_sym, rhs_sym)`<br>`shr(signed, N, lhs_sym, rhs_sym)` | `bytes(N, N) -> bytes(N, N)` | `shr` 由 `signed` 区分算术右移（`ashr`）与逻辑右移（`lshr`） |
| **整数位宽转换** | `ext_int(signed, from_N, to_N, src_sym)`<br>`trunc_int(from_N, to_N, src_sym)` | `bytes(from_N, from_N) -> bytes(to_N, to_N)` | `ext_int` 由 `signed` 区分符号扩展（`sext`）与零扩展（`zext`） |
| **浮点四则运算** | `add_float(N, lhs_sym, rhs_sym)`<br>`sub_float(N, lhs_sym, rhs_sym)`<br>`mul_float(N, lhs_sym, rhs_sym)`<br>`div_float(N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(N, N)` | $N \in \{4, 8\}$（对应 IEEE 754 单精度与双精度） |
| **浮点与整数互转** | `ext_float(from_N, to_N, src_sym)`<br>`trunc_float(from_N, to_N, src_sym)`<br>`int_to_float(signed, int_N, float_N, src_sym)`<br>`float_to_int(signed, float_N, int_N, src_sym)` | `bytes(from_N, from_N) -> bytes(to_N, to_N)` | 覆盖 `f32 <-> f64` 及有/无符号整数与浮点互转 |
| **比较运算** | `eq_bytes(N, lhs_sym, rhs_sym)`<br>`lt_int(signed, N, lhs_sym, rhs_sym)`<br>`le_int(signed, N, lhs_sym, rhs_sym)`<br>`eq_float(N, lhs_sym, rhs_sym)`<br>`lt_float(N, lhs_sym, rhs_sym)`<br>`le_float(N, lhs_sym, rhs_sym)` | `(bytes(N, N), bytes(N, N)) -> bytes(1, 1)` | 返回 `bytes(1, 1)`（`0x01` 或 `0x00`），由标准库包装为 `Boolean` |

```
!- 标准库 i32 加法与符号扩展的底层构建示例（零特殊原语） -!
add_i32_raw := compile capabilities.intrinsics.add_int(4, a, b);
sext_i16_i64_raw := compile capabilities.intrinsics.ext_int(true, 2, 8, x);
```

## 五、`compile` 元层字面量分派（统一 `word` 与 `word.word`）

与 C++ 编译器实现（`compiler/Parser.cpp`）一致，语言内核的词法与语法分析器对字面量保持极简正交：

1. **内核 Parser 仅识别两类基础词元与统一的点号算符**：
   - **`BitsLiteral`（`"..."`）**：原始位串/字节序列字面量，直接生成静态只读 `bytes(N, 1)` 数据；
   - **`Chars`（统一记作 `word`）**：任何非空白、非保留分隔符的连续字符序列——**标识符（如 `foo`）与数字文本（如 `42`、`0xFF`）在内核 Parser 中统一表示为 `word` 节点**；
   - **中缀 `.`（`Access`）**：统一表示为二元结构节点 `lhs . rhs`。
2. **`compile` 元层语法钩子完成字面量解析与类型构造**：
   - **整数字面量（`word`）**：当 `compile` 元层处理 `word` 节点时，检查其文本是否匹配数字格式（十进制、十六进制、二进制等）；若匹配，则在编译期直接将其解析并构造为标准库整数 `value`（如 `{ __raw := ..., __i64_tag := {} }`），否则作为普通 `Symbol` 字面量处理；
   - **浮点字面量（`word.word`）**：在 `.`（`Access`）的 `compile` 元层语法规则中，若左操作数 `lhs` 与右操作数 `rhs` 均为纯数字 `word`（例如 `3.14` 被解析为 `Access("3", "14")`），则元层直接将其合并解析为 IEEE 754 浮点数 `value`（`f64`），否则展开为常规的字段访问 `%field(lhs, rhs)`！

## 六、验收用例（自检清单）

1. **`%bytes(N, align)` 与 `null_sym` 判别**：`%bytes(8, 8)` 返回 `Shape` 满足 `size == 8`、`align == 8`、`first_sym == null_sym` 的未指定内容 `value`（运行期写入槽位零清零开销）；包含它的 `u64`（`{ __raw: bytes(8, 8), __u64_tag: {} }`）自动继承 `align == 8`，且因 `first_sym == __raw ~= null_sym` 与裸 `bytes(8, 8)` 严格区分。
2. **透明 `Pointer` 消除 `%recast`**：对任意胖指针 `p`，直接通过 `p.addr`、`p.shape_ptr`、`p.mutability` 读取其内部字段，并在标准库中直接构造新 `Pointer` 完成 `recast` 与 `z_ptr` 反射。
3. **全阶段符号池 `Trie` 互转**：运行期调用 `%sym_bytes(sym)` 以 $O(1)$ 取得字段名 UTF-8 字节胖指针用于 JSON 序列化；调用 `%intern_sym(ptr, len)` 动态驻留外部字段名并与编译期静态符号共享同一套 `Symbol` ID。
4. **参数化 `capabilities.intrinsics` 裸 `bytes` 运算**：`compile capabilities.intrinsics.add_int(4, a, b)` 与 `ext_int(true, 2, 8, x)` 直接在裸 `bytes` 槽位上完成硬件运算，标准库经 `.__raw` 拆封与重装，全程无需内核认识 `i32` 或提供修改 `Shape` 原语。
