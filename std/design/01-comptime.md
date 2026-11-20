# 01 - Comptime 与阶段边界

前置：无（系列总纲）
后续：[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)、[08-syntax.md](08-syntax.md)

## 一、核心原则

语言只有一个求值器。代码可在编译期或运行期被求值，两者的求值规则、控制流与所有权语义完全相同。
`#compile` 不是值上的属性，而是**求值发生的时机**。

## 二、阶段分层与元层能力

| 层级 | 执行时机 | 触发方式 | 核心职责 |
| :--- | :--- | :--- | :--- |
| **元层** | 编译期（必须） | 构建脚本入口、顶层 `#compile(...)` 块 | 构建配置、语法/算符/字面量注册、底层 `bytes(N, N)` 算术 `intrinsics` 获取、模块导入、编译期诊断 |
| **程序层（静态）** | 编译期（编译器尽力） | 常量折叠成功；`#compile(...)` 强制 | 单态化、静态连续内存布局推导、指针瘦化（消去静态已知的 `shape_ptr` 与 `mutability`） |
| **程序层（动态）** | 运行期 | 默认；静态未知时保留运行期描述信息 | 常规程序执行、胖指针动态寻址与分派 |

- **元层隔离与能力注入**：原生在编译期执行的元层代码与“被折叠到编译期执行的普通程序代码”明确区分。元层能力（IO、文件系统、`import`、字段映射 `capabilities.map`、符号池互转 `capabilities.symbols`、底层指令集 `capabilities.intrinsics`、语法注册 `capabilities.syntax` 与字节常量构造）通过显式的 `capabilities` 对象传入，默认沙箱化。
- **语法扩展与纯 `Prototype`**：AST 仅存在于编译期元层的语法重写阶段（见 [08-syntax.md](08-syntax.md)）；一旦生成 `Prototype`（`^Expr`），它在编译期与运行期都是不携带 AST 包袱的纯执行代码值，保证两阶段语义严格一致且运行期零 AST 开销。
- **确定性与可缓存**：单次编译期求值内部允许拥有局部可变状态（`mutable` location）；但跨求值缓存与跨阶段传递只允许不可变的固化值（见第四节），因此编译期求值天然可缓存、可测试、IDE 可安全执行。

## 三、与动态结构的关系

- 语言允许动态结构，因此语言层面“必须编译期已知”的语法位置趋近于零。
- 类型/形状约束、泛型参数等位置**不强制要求**编译期已知；它们只是“若编译期可折叠则解锁单态化、内联连续布局与瘦指针退化”的优化机会。
- **折叠成功** → 单态化 + 静态偏移计算 + 指针省去运行期 `shape_ptr` 与 `mutability` 字段（退化为单字瘦指针 `addr`）；**折叠失败** → 保留完整胖指针 + 动态查表寻址。
- **强制断言**：用户可用 `#compile(...)` 显式约束某表达式必须在编译期求值完成（例如 `capabilities.intrinsics` 要求的编译期常数参数）；若折叠失败则直接报编译错误，绝不静默退化为运行期求值。

## 四、阶段边界规则

1. **固化（Reification）**：编译期求值结果跨阶段进入运行期时，只允许以下不可变 `value` 跨越边界：
   - **非指针 POD `value`**（底层为无子字段叶子 `Shape` 的值，如各类整数、浮点数、`Boolean`、`Symbol` 等）：按位深拷贝为二进制常量数据；
   - **指向只读常量区（`frozen` 区）的安全只读 `Pointer`**：包括已闭合对象的 `Shape` 指针（`#shape(val)`）、编译期冻结的模块与只读对象快照（`#freeze(ptr)`）。编译器将指针指向的只读常量对象一并迁入二进制 `.rodata` 段并重定位地址；
   - **`Prototype`（纯代码值）**：包括由 `^Expr` 构造或由 `capabilities.intrinsics` 生成的底层算术/比较 `Prototype`，固化为可序列化的中间表示（IR）/机器码并参与后续编译。
   - **严禁固化**：指向编译期栈或协程堆上普通 `location` 的 `Pointer`、外部 OS 资源句柄、活跃块指针（`#this`）及协程 `Handle`（`#caller`）。
2. **阶段结束清理**：单次 `#compile` 求值结束时，其创建的局部资源按 [06-stdlib-foundations.md](06-stdlib-foundations.md) 的标准库 RAII 协议完整执行清理，仅固化值留存。
3. **单向性**：`compile → runtime` 单向不可逆。运行期值绝不能流回编译期（语言内核显式排除运行期 JIT）。
4. **步数上限**：编译期求值不追求理论可判定性；编译器设置求值步数上限，超限时报错并输出调用栈。

## 五、形状（`Shape`）与泛型接口

- **唯一类型概念 `Shape`**：全语言用 `Shape` 统一替代传统的 `type`（完整结构见 [07-types-and-intrinsics.md](07-types-and-intrinsics.md)）。
- **泛型即普通子程序**：泛型函数 = 参数中包含 `Pointer(Shape)` 的普通子程序；泛型类型构造器 = 返回 `Pointer(Shape)` 或构造原型的普通子程序。

## 六、阶段与可变性词汇表

求值阶段与位置可变性完全正交，全语言仅保留两个核心维度：

| 维度 | Core 机制 | 含义 | 缺省行为 |
| :--- | :--- | :--- | :--- |
| **求值时机** | `#compile(expr)` | 强制在编译期求值完成并固化 | 编译器尽力折叠，不作硬性保证 |
| **位置可变性** | `#allocate(s, is_mut)` / `#allocate_val(v, is_mut)` | `is_mut = true` 允许槽位在进入 `live` 后再次调用 `#write` 原地覆写 | 默认 `immutable`（仅允许 `uninitialized → live` 单次写入） |

- **废弃旧词**：`type`（统一为 `Shape`）、`constant`（混淆时机与可变性）、`variant`（缺省状态无需标注）、`comptime`（改用 `#compile`）、`static`（消除 C 系多义性）、`readonly`（缺省即为不可变）、`%seal`（单次初始化状态机已覆盖）。

用法示例：
```
! 1. 元层块：编译期执行构建与语法注册逻辑 !
#compile({ ... })

! 2. 编译期求值并装箱绑定为运行期不可变位置 !
size := #compile(f(x))

! 3. 编译期求初值、运行期可反复修改的位置 !
x := #allocate(i32, true);
.x = #compile(f())
```

## 七、验收用例（自检清单）

1. **编译期 `printf`**：在编译期解析格式串，并按参数 `Shape` 生成对应的格式化 `Prototype` 代码固化到运行期。
2. **编译期生成只读查找表**：`#compile` 块中用可变 `location` 计算完美哈希表，经 `#freeze` 冻结为指向只读常量区的安全指针后跨阶段供运行期只读访问。
3. **编译期反射 `derive(Serialize)`**：通过 `#shape` 与 `Shape.fields`（`capabilities.map`）遍历字段元数据，生成并返回序列化代码 `Prototype`。
4. **非法固化拦截**：编译期构造栈/堆 `location` 指针试图直接跨阶段固化时静态拒绝；POD `value`、只读常量区 `Pointer`（含闭合 `Shape` 与 `frozen` 对象）、`Prototype` 正常固化。
5. **编译期 RAII**：`#compile` 块内创建的资源与 `defer` 守卫在编译期块结束时按与运行期完全相同的标准库逆序规则执行 `destroy`。
