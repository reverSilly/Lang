# 01 - Comptime 与阶段边界

前置：无（系列总纲）
后续：[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)

## 一、核心原则

语言只有一个求值器。代码可在编译期或运行期被求值，两者的求值规则、控制流与所有权语义完全相同。
`compile` 不是值上的属性，而是**求值发生的时机**。

## 二、阶段分层与语法扩展

| 层级 | 执行时机 | 触发方式 | 核心职责 |
| :--- | :--- | :--- | :--- |
| **元层** | 编译期（必须） | 构建脚本入口、顶层 `compile` 块、编译器指令 | 构建配置、语法/算符/字面量注册、底层算术 `intrinsics` 获取、AST 重写、模块导入、编译期诊断 |
| **程序层（静态）** | 编译期（编译器尽力） | 常量折叠成功；`compile` 标注强制 | 单态化、静态连续内存布局推导、指针瘦化（消去静态已知的 `shape_ptr` 与 `mutability`） |
| **程序层（动态）** | 运行期 | 默认；静态未知时保留运行期描述信息 | 常规程序执行、胖指针动态寻址与分派 |

- **元层隔离与能力注入**：原生在编译期执行的元层代码与“被折叠到编译期执行的普通程序代码”明确区分。元层能力（IO、文件系统、`import`、Parser 与 AST 操弄接口、符号池 `symbols` 与底层指令集 `intrinsics`）通过显式的 `capabilities` 对象传入，默认沙箱化（详见 [07-types-and-intrinsics.md](07-types-and-intrinsics.md)）。
- **轻量语法重写层（Parser Pattern + `compile` 处理函数）**：
  - 语言内核不内置复杂的语法糖、数字字面量解析或 RAII 调度逻辑；算符、语法结构（如 `{ ... }`、`block1`、`:=`、`=`、`$`、`@`、`if ... else ...`、`while`、`defer`、`break`、`try`）以及数字/浮点字面量（`word` 与 `word.word`）均在 `compile` 元层注册（见 [06-stdlib-foundations.md](06-stdlib-foundations.md) 与 [07-types-and-intrinsics.md](07-types-and-intrinsics.md)）；
  - 编译器 Parser 支持以动态字符串模式声明语法规则与优先级（如 `.'if'.('else'.)?`），匹配成功后将捕获的 AST 节点传入 `compile` 期处理函数，展开为纯粹的内核原语序列（结合 `evaluate` 在调用方当前 `block` 就地生效）；
  - **`prototype` 保持纯粹**：AST 仅存在于 `compile` 元层的语法重写阶段；一旦生成 `prototype`（`^Expr`），它在编译期与运行期都是不携带 AST 包袱的纯执行代码值，保证两阶段语义严格一致且运行期零 AST 开销。
- **确定性与可缓存**：单次编译期求值内部允许拥有局部可变状态（`mutable` location）；但跨求值缓存与跨阶段传递只允许不可变的固化值（见第四节），因此编译期求值天然可缓存、可测试、IDE 可安全执行。

## 三、与动态结构的关系

- 语言允许动态结构，因此语言层面“必须编译期已知”的语法位置趋近于零。
- 类型/形状约束、泛型参数等位置**不强制要求**编译期已知；它们只是“若编译期可折叠则解锁单态化、内联连续布局与瘦指针退化”的优化机会。
- **折叠成功** → 单态化 + 静态偏移计算 + 指针省去运行期 `shape_ptr` 与 `mutability` 字段（退化为单字瘦指针 `addr`）；**折叠失败** → 保留完整胖指针 `(addr, shape_ptr, mutability)` + 动态查表寻址。
- **强制断言**：用户可用 `compile` 显式约束某位置必须在编译期求值完成（例如 `capabilities.intrinsics` 要求的编译期符号常量参数）；若折叠失败则直接报编译错误，绝不静默退化为运行期求值。

## 四、阶段边界规则

1. **固化（Reification）**：编译期求值结果跨阶段进入运行期时，只允许以下不可变 `value` 跨越边界：
   - **非指针 POD `value`**（底层为 `bytes(N, align)` 的值，如各类整数、浮点数、`boolean`、`symbol` 等）：按位深拷贝为二进制常量数据；
   - **指向只读常量区（`frozen` 区）的安全只读 `pointer`**：包括已闭合对象的 `shape` 指针（`%shape(val)`）、编译期冻结的模块与只读对象快照（`%freeze(ptr)`）。编译器将指针指向的只读常量对象一并迁入二进制 `.rodata` 段并重定位地址；
   - **`prototype`（纯代码值）**：包括由 `^Expr` 构造或由 `capabilities.intrinsics` 生成的底层算术/比较 `prototype`，固化为可序列化的中间表示（IR）/机器码并参与后续编译。
   - **严禁固化**：指向编译期栈或协程堆上普通 `location` 的 `pointer`、外部 OS 资源句柄、活跃块指针（`%this`）及协程 `handle`（`\^`）。
2. **阶段结束清理**：单次 `compile` 求值结束时，其创建的局部资源按 [06-stdlib-foundations.md](06-stdlib-foundations.md) 的标准库 RAII 协议完整执行清理，仅固化值留存。
3. **单向性**：`compile` → `runtime` 单向不可逆。运行期值绝不能流回编译期（语言内核显式排除运行期 JIT）。
4. **步数上限**：编译期求值不追求理论可判定性；编译器设置求值步数上限，超限时报错并输出调用栈。

## 五、形状（Shape）系统接口

- **唯一类型概念 `shape`**：全语言用 `shape` 统一替代传统的 `type`。
  - **构建期动态性**：尚处于构建期（Constructing）的 `block` 在内部维护可变的 `shape` 构建器随 `%bind` 与 `%drop` 演进；
  - **闭合对象与基础值的 `shape` 驻留常量区**：任何基础 `value` 与已闭合（Closed）对象的 `shape` 描述表均驻留在只读常量区（`frozen` 区），通过指向它的只读指针 `pointer(Shape)` 表示，且 `Shape` 自身也是暴露了 `{ size, align, first_sym, last_sym, __shape_tag }` 的普通只读对象（见 [03-symbols-locations-matching.md](03-symbols-locations-matching.md) 与 [07-types-and-intrinsics.md](07-types-and-intrinsics.md)）。
- **泛型即普通子程序**：泛型函数 = 参数中包含 `pointer(Shape)` 的普通子程序；泛型类型构造器 = 返回 `pointer(Shape)` 或构造原型的普通子程序。

## 六、阶段与可变性词汇表

求值阶段与位置可变性完全正交，全语言仅用两个修饰词：

| 标注 | 作用目标 | 含义 | 缺省行为 |
| :--- | :--- | :--- | :--- |
| `compile` | 求值时机 / 模式约束 | 强制编译期求值完成 | 编译器尽力折叠，不作硬性保证 |
| `mutable` | `location` 契约属性 | 允许槽位在初始化（进入 `live`）后再次调用 `%write` 覆写或 `%swap` 交换 | 默认 `immutable`（仅允许 `uninitialized → live` 单次写入） |

- **废弃旧词**：`type`（统一为 `shape`）、`constant`（混淆时机与可变性）、`variant`（缺省状态无需标注）、`comptime`（改用自解释的 `compile`）、`static`（消除 C 系多义性）、`readonly`（缺省即为不可变）、`%seal`（单次初始化状态机已覆盖）。

用法示例：

```
!- 1. 元层块：编译期执行构建与语法注册逻辑 -!
compile { ... }

!- 2. 位置约束：要求参数 n 在编译期已知 -!
n : compile usize

!- 3. 编译期求值并装箱绑定为运行期不可变位置 -!
size := compile f(x)

!- 4. 编译期求初值、运行期可反复修改的位置 -!
x := allocate mutable;
\x = compile f()
```

## 七、验收用例（自检清单）

1. **编译期 `printf`**：在编译期解析格式串，并按参数 `shape` 生成对应的格式化 `prototype` 代码固化到运行期。
2. **编译期生成只读查找表**：`compile` 块中用 `mutable` location 计算完美哈希表，经 `%freeze` 冻结为指向只读常量区的安全指针后跨阶段供运行期只读访问。
3. **编译期反射 `derive(Serialize)`**：通过 `%shape`（或胖指针的 `ptr.shape_ptr`）配合 `base_addr = 0` 的 `%field` 相对胖指针与 `%next_sym`/`%prev_sym` 游标遍历字段元数据，生成并返回序列化代码 `prototype`。
4. **编译期构造栈/堆 `location` 指针试图直接跨阶段固化**：编译器静态拒绝；POD `value`、只读常量区 `pointer`（含闭合 `shape` 与 `frozen` 对象）、`prototype` 正常固化。
5. **编译期 RAII**：`compile` 块内创建的资源与 `defer` 守卫在编译期块结束时按与运行期完全相同的标准库逆序规则执行 `destroy`。
