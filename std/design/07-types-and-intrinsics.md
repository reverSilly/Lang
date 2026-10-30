# 07 - 基础数据类型、透明内建形状与 `compile` 算术能力

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)

## 一、核心原则：万物皆 `bytes(N, align)` + 零尺寸标记（ZST Marker）

为了在保持语言内核（Core）极简与正交的同时拥有完整的静态/动态类型表达力，基础数据类型遵循四条核心设计公理：

1. **`bytes(N, align = 1)` 是全语言唯一的物理数据原子**：
   - 内核不内置 `i32`、`u64`、`f64` 乃至独立的 `usize` 算术类型；一切标量、内建句柄与复合对象在物理层均由 `bytes(N, align)` 组合而成。
2. **零尺寸标记（ZST Marker）区分同尺寸描述性 `Shape`**：
   - 由于 `Shape` 是纯粹描述性的（Descriptive），两个同样包装 `bytes(4, 4)` 的类型（如 `i32` 与 `u32`）通过在结构中嵌入不同符号名的零尺寸空对象字段（如 `__i32_tag : {}` vs `__u32_tag : {}`）实现零内存开销的 `Shape` 区分与多态分派；且这些 `Shape` 直接由标准库构造，无需特殊元层能力。
3. **全内建类型透明化（Exposed Layout）**：
   - `Shape`、`Pointer`、`Symbol`、`Boolean`、`Prototype`、`Handle` 不再是黑盒幻影类型，而是公开了内部 `bytes(N, align)` 字段与专属 ZST Marker 的普通 POD 结构。这直接消灭了读取指针分量或形状元数据的专用内核原语。
4. **`compile` 元层注入算术指令与字面量语法**：
   - 底层算术、位运算与比较操作通过 `compile` 元层的 `capabilities.intrinsics` 按编译期已知槽位符号生成 `prototype` 并固化到标准库；数字与浮点字面量（`word` 与 `word.word`）同样在 `compile` 元层拦截分派。

## 二、物理底座 `bytes(N, align)` 与零尺寸标记机制

### 2.1 `bytes(N, align = 1)` 的 `Shape` 表示与 `null_sym` 唯一判别

任意定长字节块 `bytes(N, align)`（当省略 `align` 时缺省为 `1`，要求 `align` 为 $2$ 的幂）的 `Shape` 对象为：

```
{
    size        : N,         !- 字节长度（usize，底层 bytes(8, 8)） -!
    align       : align,     !- 字节对齐（usize，底层 bytes(8, 8)） -!
    first_sym   : null_sym,  !- 原子字节块无子字段 -!
    last_sym    : null_sym,  !- 原子字节块无子字段 -!
    __shape_tag : {}
}
```

- **为什么 `bytes(N, align)` 需要 `align`**：
  - 复合结构体闭合时的对齐要求由其所有子字段的对齐推导得出：
    $$\text{struct.align} = \max\left(1, \max_{f \in \text{fields}} f.\text{align}\right)$$
  - 将 `align` 作为 `bytes(N, align)` 的基础属性后，普通字符/字节数组 `bytes(64)` 默认 `align = 1`（嵌入结构体时不产生填充浪费），而标量底层存储如 `bytes(4, 4)`、`bytes(8, 8)` 则自动将 $4$ 字节或 $8$ 字节硬件对齐要求传染给包含它的任何上层结构体！
- **为什么无需特殊标示符即可唯一识别 `bytes(N, align)`（`null_sym` 完备性定理）**：
  - **当 $N > 0$ 时**：任何复合对象 `{ ... }` 的 `size` 均来自其内部字段。若一个复合对象没有任何字段（`first_sym == null_sym`），其 `size` 必然为 `0`；反之，任何 `size > 0` 的复合对象（或含 ZST 标记的包装类型）必然至少拥有一个字段，故其 `first_sym ~= null_sym`。因此 **`first_sym == null_sym`（且 `last_sym == null_sym`）已 100% 唯一标识了原子 `bytes(N, align)`**，绝不会与任何复合结构体发生碰撞！
  - **当 $N = 0, \text{align} = 1$ 时**：`bytes(0, 1)` 的形状为 `{ size: 0, align: 1, first_sym: null_sym, last_sym: null_sym }`，这与空块 `{}`（`block ^()`）闭合出的空对象形状**完全同一**。将 `bytes(0)` 与空对象 `{}` 统一为同一个零尺寸单位形状，消除了多余的边界特例。

### 2.2 零尺寸标记（ZST Marker）与描述性 Shape 区分

在纯描述性 `Shape` 体系中，若 `i32` 与 `u32` 都仅定义为 `{ __raw : bytes(4, 4) }`，两者的字段名、偏移量与子形状完全相同，全局 Intern 后会坍缩为同一个 `Shape`。

解决方案是在标准库中直接用空对象 `{}`（`size = 0, align = 1`）作为**零尺寸名义标记字段（ZST Marker）**：

```
i32 := shape_of({ __raw : bytes(4, 4), __i32_tag : {} });
u32 := shape_of({ __raw : bytes(4, 4), __u32_tag : {} });
f32 := shape_of({ __raw : bytes(4, 4), __f32_tag : {} });
```

- **物理零开销**：`__i32_tag` 的 `size = 0, align = 1`，因此在闭合对象中 `size` 仍为严格 $4$ 字节、`align` 仍为 $4$ 字节，物理内存与裸 `bytes(4, 4)` 100% 相同；
- **反射与分派唯一性**：两者的 `last_sym` 分别为 `__i32_tag` 与 `__u32_tag`，在全局 `Shape` 驻留池中拥有截然不同的 `shape_ptr` 地址，且 `%field(z_ptr, __i32_tag)` 可在 $O(1)$ 时间内完成类型判别与多态分派。

## 三、全内建类型的透明化布局（自举闭环）

所有内建类型均遵循“公开 `bytes(N, align)` 数据字段 + 专属 ZST Marker”的统一范式：

| 内建类型 | 透明结构布局（64 位目标平台） | 大小 / 对齐 | 核心设计收益与自举关系 |
| :--- | :--- | :--- | :--- |
| **`Symbol`** | `{ id: u32, __sym_tag: {} }`<br>（其中 `u32` 底层为 `bytes(4, 4)`；`id = 0` 保留给 `null_sym`） | `4B` / `4B` | 源码标识符即 4 字节唯一 Intern ID；支持按值相等比较，并可通过 `compile` 符号池能力与文本双向转换 |
| **`Boolean`** | `{ val: bytes(1, 1), __bool_tag: {} }` | `1B` / `1B` | 供内核 `%branch` 与 `%bind` 使用，同时与 `u8`/`i8` 保持严格的类型区分 |
| **`Shape`** | `{ size: usize, align: usize, first_sym: Symbol, last_sym: Symbol, __shape_tag: {} }`<br>（其中 `usize` 底层为 `bytes(8, 8)`） | `24B` / `8B` | `Shape` 自身也是普通只读对象，其 `shape_ptr` 指向只读常量区中的 `ShapeMeta`（即 `Shape` 自身的形状，形成不动点闭环） |
| **`Pointer`** | `{ addr: usize, shape_ptr: usize, mutability: bytes(1, 1), __ptr_tag: {} }` | `24B` / `8B`<br>（瘦化后 `8B`） | **彻底消灭指针分量读取原语**：直接通过 `.addr`、`.shape_ptr`、`.mutability` 访问胖指针三元组 |
| **`Prototype`** | `{ code_addr: usize, __proto_tag: {} }` | `8B` / `8B` | 指向编译期或运行期只读代码段入口的纯代码值 |
| **`Handle`** | `{ coro_addr: usize, __handle_tag: {} }` | `8B` / `8B` | 指向 GC 托管协程控制块的句柄值 |

### `Symbol` 与文本的双向转换能力

- **运行期零字符串负担**：运行期二进制中，`Symbol` 仅是一个轻量的 4 字节整数 ID（`{ id: u32, __sym_tag: {} }`），不强制在运行期保留符号名字符串表。
- **编译期符号池能力（`capabilities.symbols`）**：
  - `capabilities.symbols.to_bytes(sym) -> Slice(u8)`：在编译期（如序列化生成、调试打印、宏展开）反查符号对应的 UTF-8 字节序列；
  - `capabilities.symbols.from_bytes(slice) -> Symbol`：在编译期将动态拼接或解析出的文本驻留（Intern）为对应的 `Symbol` 值。

## 四、标准库算术类型与 `capabilities.intrinsics`

### 4.1 标准库定义的基础算术类型家族

内核不认识任何高层算术类型；以下类型全部由标准库用 `bytes(N, N)` + ZST Marker 定义：

| 类别 | 标准库类型 | 透明结构定义 |
| :--- | :--- | :--- |
| **有符号整数** | `i8` / `i16` / `i32` / `i64` / `isize` | `{ __raw: bytes(N, N), __i<bits>_tag: {} }` |
| **无符号整数** | `u8` / `u16` / `u32` / `u64` / `usize` | `{ __raw: bytes(N, N), __u<bits>_tag: {} }` |
| **IEEE 754 浮点** | `f32` / `f64` | `{ __raw: bytes(N, N), __f<bits>_tag: {} }` |

### 4.2 算术操作数传递与 `capabilities.intrinsics`

为什么算术操作不采用“内核原语”或“带洞的非闭合 `prototype`”？
1. **保持 `prototype` 100% 闭合**：若允许 `prototype` 内部留有未绑定的外部 AST 插槽，会破坏“`prototype` 是纯执行代码值”的根基。
2. **逻辑 `location` 消除栈帧分配担忧**：如 [03-symbols-locations-matching.md](03-symbols-locations-matching.md) §1 所述，`location` 是逻辑存储槽抽象，不要求对应物理内存（寄存器同样可承载逻辑 `location`）。
3. **编译期参数化槽位符号生成 `prototype`**：
   - `compile` 元层的 `capabilities.intrinsics` 提供一组编译期生成函数，接收**编译期已知的槽位 `Symbol` 常量**（如 `lhs_sym`、`rhs_sym`），返回对应的底层指令 `prototype`：
     ```
     !- lhs_sym 与 rhs_sym 均为编译期已知的 Symbol 常量 -!
     add_i32_proto := compile capabilities.intrinsics.add_i32(a, b);
     ```
   - 当该 `prototype` 在标准库算术包装帧中通过 `evaluate` 执行时，它直接从当前帧 `%this` 的 `lhs_sym`（`\a`）与 `rhs_sym`（`\b`）槽位读取字节，执行硬件级运算并直接返回结果 `value`：
     ```
     !- 标准库 i32 加法的底层构建示意 -!
     add_i32 := {
         a := ...;
         b := ...;
         evaluate (compile capabilities.intrinsics.add_i32(a, b))
     }
     ```
   - **涵盖的指令族**：
     - 整数运算：`add_<T>`、`sub_<T>`、`mul_<T>`、`div_<T>`、`rem_<T>`；
     - 位运算：`bit_and_<T>`、`bit_or_<T>`、`bit_xor_<T>`、`bit_not_<T>`、`shl_<T>`、`shr_<T>`；
     - 浮点运算：`fadd_<F>`、`fsub_<F>`、`fmul_<F>`、`fdiv_<F>`；
     - 比较运算（返回 `Boolean`）：`eq_<T>`、`lt_<T>`、`le_<T>`；
     - 位宽与数值转换：`sext_<From>_<To>`、`zext_<From>_<To>`、`trunc_<From>_<To>`、`itof_<I>_<F>`、`ftoi_<F>_<I>`。

## 五、`compile` 元层字面量分派（统一 `word` 与 `word.word`）

与 C++ 编译器实现（`compiler/Parser.cpp`）一致，语言内核的词法与语法分析器对字面量保持极简正交：

1. **内核 Parser 仅识别两类基础词元与统一的点号算符**：
   - **`BitsLiteral`（`"..."`）**：原始位串/字节序列字面量，直接生成静态只读 `bytes` 数据；
   - **`Chars`（统一记作 `word`）**：任何非空白、非保留分隔符的连续字符序列——**标识符（如 `foo`）与数字文本（如 `42`、`0xFF`）在内核 Parser 中统一表示为 `word` 节点**；
   - **中缀 `.`（`Access`）**：统一表示为二元结构节点 `lhs . rhs`。
2. **`compile` 元层语法钩子完成字面量解析与类型构造**：
   - **整数字面量（`word`）**：当 `compile` 元层处理 `word` 节点时，检查其文本是否匹配数字格式（十进制、十六进制、二进制等）；若匹配，则在编译期直接将其解析并构造为标准库整数 `value`（如默认 `i64` 或按上下文形状匹配），否则作为普通 `Symbol` 字面量处理；
   - **浮点字面量（`word.word`）**：在 `.`（`Access`）的 `compile` 元层语法规则中，若左操作数 `lhs` 与右操作数 `rhs` 均为纯数字 `word`（例如 `3.14` 被解析为 `Access("3", "14")`），则元层直接将其合并解析为 IEEE 754 浮点数 `value`（`f64`），否则展开为常规的字段访问 `%field(lhs, rhs)`！

## 六、验收用例（自检清单）

1. **`bytes(N, align)` 与 `null_sym` 判别**：`bytes(8, 8)` 的 `Shape` 满足 `size == 8`、`align == 8`、`first_sym == null_sym`；包含它的 `u64`（`{ __raw: bytes(8, 8), __u64_tag: {} }`）自动继承 `align == 8`，且因 `first_sym == __raw ~= null_sym` 与裸 `bytes(8, 8)` 严格区分。
2. **透明 `Pointer` 字段访问**：对任意胖指针 `p`，直接通过 `p.addr`、`p.shape_ptr`、`p.mutability` 读取其内部字段，无需专用指针拆包原语。
3. **`capabilities.intrinsics` 固化执行**：在 `compile` 期调用 `capabilities.intrinsics.add_i32(a, b)` 获得 `prototype` 并固化到标准库 `+` 算符中，运行期求值 `1 + 2` 直接在逻辑槽位上完成硬件加法并返回 `i32` 结果 `3`。
4. **元层字面量分派**：源码中的 `42`（单个数字 `word`）与 `3.14`（两侧均为数字的 `word.word`）在 `compile` 元层分别被折叠为标准库整数与浮点数 `value`，而 `obj.x` 正常展开为 `%field(\obj, x)`。
