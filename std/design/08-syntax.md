# 08 - 词法、Core 最小语法（Layer 0）与标准库语法层（Layer 1）

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)

## 一、核心原则：严格双层语法切分与符号纯粹性

1. **Layer 0（Core 硬编码最小语法）零保留关键字**：
   - Core 不占用任何普通单词（不再保留 `if`、`else`、`new`、`block`、`allocate`、`mutable`、`shape`、`match` 等关键字）；所有内核原语统一走专职前缀 `#`。
2. **符号纯粹性（一符一职，拒绝歧义复用）**：
   - **`\`**：100% 归还给全文词法转义（`\` + 任意字符构成普通转义字符，失去原语法含义）；
   - **`! ... !`**：语言唯一的注释语法（废除旧版 `!pragma!` 编译器指令通道，元编程统一走 `#compile` + `capabilities`），不给 `/` 与 `*` 增添注释负担；
   - **`#`**：专职 Core Builtin 前缀（如 `#block(p)`、`#this`、`#write(dst, val)`），**废除 `#` 作为中缀连接运算符的含义**；
   - **`.`**：归属 Layer 1（Stdlib），同时提供中缀 `p.a`（展开为 `#field(p, a)`）与前缀 `.a`（展开为 `#field(#this, a)`），读值即 `$.a`。
3. **一切字面量与运算符皆归 Layer 1（Stdlib）**：
   - 整数、浮点数（`word.word`）、双引号字节串/字符串（`"..."`）、块 `{ ... }`、算术/比较/赋值/控制流算符，全部在 `#compile` 元层向 Parser 注册，不硬编码进 Core 语义。

## 二、Layer 0：Core 最小词法与文法

### 2.1 词法规则

| 词元 | 形式 | 语义说明 |
| :--- | :--- | :--- |
| **`WhiteSpace`** | 空格、`\t`、`\r`、`\n` 等空白符 | 分隔符，忽略 |
| **`Comment`** | `! ... !`（内部支持 `\!` 转义） | 唯一注释语法（支持单行与多行），作为分隔符忽略 |
| **`Escaped`** | `\` + 任意单字符 | 转义后字符失去原有语法含义，作为普通 `Char` 参与构成 `Word` |
| **`Word`** | 一个或多个连续的字母、数字、`_`、非 ASCII 字节或 `Escaped` 字符 | 统一词元（标识符与数字文本在词法层不作区分）；Layer 0 中默认求值为 `Symbol` 值 |
| **`Builtin`** | `#` 紧贴 `Word`（如 `#this`、`#write`） | 指代 [03-symbols-locations-matching.md](03-symbols-locations-matching.md) §3 中的 24 个内置原语 |

### 2.2 Layer 0 文法（仅 4 条产生式）

```ebnf
Expr        := SeqExpr
SeqExpr     := Primary ( ';' Primary )*
Primary     := '^' Primary                             ! 唯一延迟特殊形式：生成 Prototype !
             | Builtin '(' ArgList? ')'                ! 19 个带参 Core Builtin 调用 !
             | Builtin                                 ! 5 个零参 Core Builtin：#this #parent #unbound #caller #in !
             | '(' Expr? ')'                           ! 分组；空括号 () 表示空单元 !
             | Word                                    ! Symbol 字面量 !
ArgList     := Expr ( ',' Expr )*
```

- **全 Layer 0 硬编码符号仅 6 个**：`\`（转义）、`!`（注释）、`#`（Builtin 前缀）、`(` `,` `)`（分组与参数列表）、`^`（Prototype）、`;`（序列）。
- **无 `block sth` / `new sth` / `mutable` 等特殊前缀语法**：`#allocate(s, is_mut)`、`#allocate_val(v, is_mut)`、`#block(p)`、`#evaluate(p)`、`#new(p)`、`#compile(e)` 全部统一在 `Builtin '(' ArgList ')'` 这一条产生式下。

## 三、Layer 1：标准库算符与字面量注册层

在 Layer 0 之上，标准库通过 `#compile` 元层的语法注册机制定义日常编程语法（与早期 `std/operators` 演进对齐，去除已废弃的 `#` 中缀、`\` 前缀与旧版左值符号 `` ` ``）：

### 3.1 字面量分派（全部归属 Stdlib）

| 表面写法 | Parser 匹配结构 | 标准库元层处理 |
| :--- | :--- | :--- |
| **整数字面量**（如 `42`、`0xFF`） | 单个 `Word` | 元层 `Word` 钩子识别数字格式，经编译期常量构造能力生成对应 `bytes` 并包装为 `{ __raw := ..., __i64_tag := {} }`；非数字则保持为普通 `Symbol` |
| **浮点字面量**（如 `3.14`） | `Word . Word` | 元层 `.` 钩子检测到两侧均为纯数字 `Word` 时，合并解析为 IEEE 754 `f64`；否则展开为常规字段访问 `#field(lhs, rhs)` |
| **字节串/字符串字面量**（`"..."`） | Stdlib 注册的 `"`...`"` 规则 | `BitsLiteral`（`""`）不再硬编码于 Layer 0；由标准库注册语法规则并处理转义，在 `#compile` 元层构造为 `bytes(N, 1)` 或标准库 `String` 切片 |

### 3.2 标准库核心运算符优先级参考表（数值越小优先级越高）

| 优先级 | 算符 | 形态 | 目标展开 / 归属 |
| :--- | :--- | :--- | :--- |
| **1** | `.a` | prefix | `#field(#this, a)`（当前帧字段指针） |
| **2** | `p.a` | infix | `#field(p, a)`（或两侧数字 `Word` 走浮点字面量） |
| **3** | `{ Expr }` | circumfix | 标准库对象构造块（展开见 [06-stdlib-foundations.md](06-stdlib-foundations.md) §3.2） |
| **4** | `coro @ rhs` | infix | 协程恢复与消息传递分派器（POD Gate + `#resume`） |
| **5** | `$ptr` | prefix | 标准库 POD Gate 读取算子（`is_pod` + `#deref`） |
| **6** | `^Expr` | prefix | Layer 0 唯一延迟特殊形式（`Prototype`） |
| **7** | `+` `-` `~` | prefix | 一元正、负、按位/逻辑取反 |
| **8** | `*` `/` `%` | infix | 乘、除、取模（Stdlib `capabilities.intrinsics` 分派） |
| **9** | `+` `-` | infix | 加、减 |
| **10** | `<<` `>>` | infix | 左移、右移 |
| **11** | `<` `>` `~<` `~>` | infix | 大小比较 |
| **12** | `==` `~=` `<=>` | infix | 相等、不等、三路比较 |
| **13** | `&` / `|` | infix | 按位/逻辑与、或 |
| **14** | `->` | infix | Lambda / 映射构造（待函数协议定稿） |
| **15** | `v : pat` / `v ? pat` | infix | 模式匹配可反驳约束 / 纯查询匹配 |
| **16** | `a if c else b` | mixfix | `#branch(c, ^a, ^b)` |
| **17** | `body while cond` | infix | 透明循环展开 |
| **18** | `dst = val` | infix | 标准库赋值分派器（POD Gate + `destroy` + `#write`） |
| **19** | `x := rhs` | infix | 标准库单次求值绑定与 Rebind 展开 |
| **20** | `,` | infix | 序列/元组分隔 |
| **21** | `;` | infix | Layer 0 顺序求值原语 |

## 四、待办议题（Open Items — 留待 MVP Phase 2 精细设计）

1. **`capabilities.syntax` 与 AST 暴露接口**：
   - 标准库注册的编译期语法处理函数如何接收 Parser 产出的树结构（AST 节点 `Shape` 表示、子节点遍历、卫生符号生成以及返回目标 AST/字节常量构造接口）。
   - **MVP 工程策略**：在 MVP Phase 1 中，先用宿主代码实现 Layer 0 Parser、单一 Core 求值器与一层可替换的内置 Layer 1 脱糖器；待求值器与函数调用协议跑通后，在 Phase 2 将内置脱糖器正式抽离为 `capabilities.syntax` 的自举实现。
