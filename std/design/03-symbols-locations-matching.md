# Symbol / Location / 匹配 设计记录

状态：已定稿（2026-10-24）

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)

## 核心原则

宇宙里只有三种东西：**symbol、location、value**。

- **symbol**：一等原子值（类似 Lisp symbol）。标识符 token 即 symbol 值字面量。
  symbol 可用于 pattern matching，可动态构造；它不是编译期概念。
- **location**：`allocate` 产生的格子，持有值。三个属性：标识（哪个格子）、
  形状约束（能装什么）、可变性（能否改写）。location 不可作为值传递。
- **value**：纯数据，永远不可变，可传递、可固化。"不可变"不是值的属性，
  而是 location 的缺省；值根本没有可变性概念。

一个 block 持有：若干 location + 一张 `symbol → location` 映射表。

## 两个前缀运算符（任何位置含义不变，无模式切换）

| 写法 | 含义 | 类别 |
|---|---|---|
| `a` | symbol 值本身 | value |
| `\a` | 把 symbol 沿帧树解析为 location（查 symbol→location 映射） | location |
| `$\a` | 读出该 location 的值 | value |

- `$`：location → value（读取）。
- `=`：mutable location × value → 写；表达式的值为被写的 location。
- 读取必须显式写 `$`，写入不需要。已知代价：普通代码读变量写作 `$\a`，
  比主流语言多两个字符；在现代工具辅助下可接受（对齐旧设计显式读的哲学）。

## 可变性

- 可变性是且仅是 location 的属性，`allocate`/绑定时确定，终身不变。
- 默认不可变，`mutable` 显式标注；与 `compile` 正交（见 01 词汇表）。
- **沿路径传递**：对不可变 location 取子路径，子位置同样不可变；
  mutable location 的子位置 mutable。推论：不可变 location 可达的整个
  对象图不可变——只读模块、固化常量、注入协程的能力对象免费获得深度保证。
- `mutable` 标注的粒度 = 一个 location（`allocate` 产生的整体）；
  字段级可变用嵌套对象表达。
- 退役：`readonly`/`mutable` 二选一且"默认取决于初始化"的旧规则。

## 绑定 = 匹配

只有一个操作：`match(pattern, value)`，标准库函数。
两个语法壳，方向固定、不可对换（两个操作数角色本质不对称）：

```
pattern := value     !- 绑定形式 -!
value : pattern      !- 约束形式（沿旧设计） -!
```

pattern 是由值写成的普通表达式，**不存在"pattern 模式求值"**：

- 裸 symbol 值 `a` → 未绑定则成为绑定目标（终点是 `%bind`）；
- `\a`（location）→ 匹配该 symbol 应绑到的 location 本身
  （重绑/别名类匹配走这里）；
- `$\a`（任意值）→ 按值相等匹配。这就是 `Exactly`——不是内置概念，
  是"pattern 里写了普通值"的自然推论；
- `a+1`、`{x, y}`、形状、字面量 → 全部由 stdlib 注册的 matcher 处理。

旧设计的 `Exactly`/`Symbol`/`Shape`/`If` 四类内置 pattern 消解为
值、symbol、location 三种本来存在的东西。无保留关键字，无 pin 运算符。

失败的语义：硬错误（`:`/`:=`）；可判定匹配与控制流的形式
（`?` 形式 / match 分派宏）留待控制流章。

## 编译器内建原语（最小集，共 5 个）

判据：需要触碰语言私有结构（帧树、对象布局）的必须内建；
只操作值的全部下放 stdlib。

| 原语 | 理由 |
|---|---|
| `\` 解析（symbol → location，帧树导航） | 帧是私有结构 |
| `%bind(symbol, value)` → 当前帧建立 location + 映射 | 匹配的绑定终点 |
| 基础类型的 bits 相等（每种 builtin 一个比较原语） | 值比较的底 |
| `%field(object, symbol-value)` → location | 动态字段访问 |
| 匹配失败信号 | 语义待控制流/错误章定 |

## 标准库提供（全部可推导）

- 相等 `eq`：形状反射取字段 symbol 列表 + `%field` 递归，底是内建 bits 相等；
- `Exactly`、`If` 守卫（= 调用闭包）；
- 形状匹配（comptime 反射生成检验代码，同 `derive` 机制）；
- 选择 `p1 | p2`、合取 `p1 & p2` 组合子；
- 可逆算术 pattern（`a+1 : 3` 绑 `a` 到 2）：stdlib 定义"可逆操作"协议，
  匹配器遇到注册过的操作反向应用。**内核不知道 `+` 可逆，无编译器白名单**；
  可逆操作的具体范围留待 stdlib 实现时讨论；
- range、字符串/正则、序列解包等一切花哨 pattern；
- 模式分派（match/switch）：宏，展开为连续匹配 + 分支。

**可扩展性界线**：`match` 对 pattern 值的形状分派——symbol/location/普通值
三种基本情况是语言语义固定的（保证已有代码的匹配行为永远可局部推理）；
带匹配器字段的新 pattern 形状经 `p.matcher(v)` 扩展，匹配行为由 pattern 值
自身携带，词法可见。退役的是旧设计"全局可重定义 `\$match`"这一入口
（它允许任何库改变别处已有代码的匹配语义）；扩展能力不退役，
stdlib 自身的形状匹配、可逆操作就是经同一扩展点实现的。
造绑定的能力被 `%bind` 锁在编译器手里，入口唯一。

## 指针

指针是 value（可传递、可存储、可跨 `@` 发送），指向 location 但自身
不是 location。解引用 `$ptr` 在当前求值环境**现场兑现**出 location，
兑现瞬间才受可变性检查。由此"location 不可传递"与指针共存：
被传递的是凭证，不是格子。

| | 瘦指针 | 胖指针 |
|---|---|---|
| 内容 | 仅地址 | 地址 + 形状信息（长度/形状引用/分派信息） |
| 指向 | 形状静态已知的 location | 长度/形状动态的对象（slice、动态分派） |
| 对应 01 机制 | 单态化一侧 | 装箱一侧：装箱 = 瘦指针变胖指针 |

**有效性不检查**：解引用失效指针是未定义行为，语言不提供保证。
文档明确划线：安全子集 = 无指针 + GC 协程 cell；指针是不安全特性。
（明确拒绝 Rust 式借用检查的复杂度。）

## 与 01/02 的对接

- 固化规则细化：**值直接固化；mutable location 固化为数据段槽（带初值）；
  不可变 location 固化为常量数据**。禁止固化指向 comptime 闭包的指针不变。
- comptime 堆里同样只有这三种东西，同一套规则——一个求值器成立。
- 02 的 `Proc` 交接槽 `f.in = x` / `n := $fact.in` 在新记号下照常成立
  （`n := $fact.in` 即 match：绑定 symbol `n` 到读出的值）。
- 跨协程：location 不可传递 + 指针有效性不检查 ⇒ 跨协程共享可变状态的
  受检通道仍然只有 cell 协程 + `@`/`\^` 消息；指针绕过了检查，
  由"不安全特性"定位承担。

## 词汇表

- 新增/明确：`symbol`（值）、`location`（格子）、`%bind`、`%field`。
- 运算符：`\`（解析）、`$`（读）、`=`（写）、`:=`/`:`（match 语法壳）。
- 退役：`Exactly` 等内置 pattern 类、全局 `\$match`、`` ` `` 取左值、
  左值类型一等公民及 `` ` ``/`$` 类型算子、`readonly`、
  "标识符可重绑定/销毁"、旧"右值无人引用即销毁"（值无生命周期概念；
  location 生命周期 = 词法作用域或 GC，见 02）。
- 沿用：`mutable`、路径传递、显式 `$` 读。

## 已知边界（诚实清单）

- 读取噪音（`$\a`）：已接受的取舍。
- `\` 同时承担转义与解析两个角色：语法章裁决（候选：换符号；
  或规定转义只发生在字面量/标识符内部，运算符只在前缀位置）。
- `\a` 在 pattern 中"匹配 location"的精确语义（别名 vs 重绑）
  需在 stdlib matcher 实现时细化。
- 动态绑定（pattern 里放计算出的 symbol 值）合法化 = 动态作用域回归风险；
  默认允许但记为边界，必要时由 lint 限制。

## 验收用例（自检清单）

1. `a := 3; $\a + 1` 得 4。
2. `{x, y} : point` 解包；`3 : 3` 通过；`3 : 4` 失败报错。
3. `a+1 : 3` 绑 `a` 到 2（stdlib 可逆协议，无编译器特判）。
4. pattern 中 `$\a` 按值相等匹配（Exactly 的消解验证）。
5. stdlib 实现 `eq`：递归比较嵌套对象，底为 bits 相等。
6. 不可变 location 的深层字段写入 → 编译期（或运行期）错误；
   mutable 根的深层写入成功。
7. 瘦指针存取、胖指针 slice 解引用；失效指针解引用 = UB（文档断言）。
8. match 分派宏展开为连续匹配，且 stdlib 之外无法造绑定
   （试图绕开 `%bind` 的 matcher 无法引入新 symbol 绑定）。

## 待办

- [ ] 控制流章：`?` 可判定匹配与失败语义、match 分派宏形态
- [ ] 语法章：`\` 的转义/解析双重角色裁决
- [ ] stdlib 章：可逆操作协议、matcher 注册机制、`\a` pattern 精确语义
- [ ] 指针的取址运算符形式（如何合法地拿到指向某 location 的指针值）
