# Symbol / Location / 匹配 设计记录

状态：草案（2026-10-25 按 06 的 location 模型回写）

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、
[06-ownership.md](06-ownership.md)

## 核心原则

宇宙里只有四种基本东西：**symbol、location、value、pointer**。

- **symbol**：一等原子 value（类似 Lisp symbol）。标识符 token 即 symbol
  value 字面量；可用于 pattern matching，可动态构造；不是编译期概念。
- **location**：`allocate` 产生的格子，持有内容（value 或子 location）。
  属性：标识、当前内容、可变性、所有权状态；可附带可选的 shape 约束。
  location 不可作为 value 传递。
- **value**：纯数据，永远不可变，可传递、可固化。value 没有可变性和
  生命周期概念；不可变是 location 的缺省，不是 value 的属性。
- **pointer**：指向 location 的可复制 value，不拥有、不保活、不检查有效性。
  用于访问**不属于当前所有权子树**的对象；属不安全特性。

一个 block 持有若干 location + 一张 `symbol → location` 映射表；block 的
self root 就是它的 result location（06 §2.5），对象是一棵 location 树。

### 形状与约束

- **形状（shape）是内容/对象的当前结构**，由内容决定；empty 没有内容形状。
- **shape 约束是可选的静态契约**，不是 location 的固有属性；缺省不存在。
  写入/move 时只检查显式约束；无约束的槽可接收任意形状。
- 显式约束在 empty 时保留；empty 只能按该约束接收内容。
- block/对象形状是动态的；`break` 写入 self root/result，可整体替换其内容。
- 存储布局（内联、装箱、重新分配）属于实现，不属于 shape 语义。

## 记号

| 写法 | 含义 | 类别 |
|---|---|---|
| `a` | symbol value 本身 | value |
| `\a` | 把 symbol 沿帧树解析为 location | location |
| `\p.a` | location 路径/子 location | location |
| `$\p.a` | 读出叶子 location 的 value | value |
| `&p.a` | 取 location 的指针（拼写待定） | value (pointer) |

- `$`：location → value（读取）；只对叶子合法，object location 不能整体读。
- `=`：mutable location × value → 写；表达式的值为被写的 location。
- `:=`：命名一个当前 block 的直接子 location；不分配、不移动、不产生
  可丢弃值。绑定在 RHS 完全求值之后发生。
- `.`：location 路径；文档中 `p.a` 是 `\p.a` 的简写，正式源码必须显式
  解析 symbol（语法章可再决定是否放宽）。
- 读取必须显式写 `$`；已接受其噪音。指针解引用写作 `$ptr`，兑现为
  location，再读需 `$($ptr)`；拼写待语法章裁决。

## 可变性与所有权

- 可变性是且仅是 location 的属性，`allocate` 时确定，终身不变。
- 默认不可变，`mutable` 显式标注；与 `compile` 正交（01）。
- **存储可变性不沿路径继承**：一个不可变 location 可以有 mutable 字段；
  每个字段是独立 allocate 的 location（06 §4）。
- **只读沿 view 传播**：从帧 handle 取得的只读 view 使整条访问路径只读；
  这是深度只读的来源。view 只用于自有对象的临时访问，不存储、不捕获、
  不返回。
- `mutable` 的粒度 = 一个 location；字段级可变性由各字段自己的 allocate 决定。
- 退役：`readonly`/`mutable` 二选一、深只读随根继承、旧“标识符可重绑定”。

## 绑定与 pattern

只有两个操作，方向固定：

```
x := location               !- 命名；RHS 是当前 block 的直接子 location -!
value : pattern             !- 可反驳约束；失败产生 fail 并 break 当前 block -!
```

- **`:=` 不是匹配，也不接受 pattern。** 它只做命名；RHS 必须是当前 block
  的直接子 location，且在 RHS 完成后才建立绑定。`x := 1` 非法；
  `x := { ... }` 命名 block 的 result location；`x := allocate ^1` 命名
  一个 prototype 初始化的 location。
- **`:` 是唯一的 pattern 语法壳。** pattern 是由 value 写成的普通表达式，
  不存在“pattern 模式求值”：
  - 裸 symbol `a` → 绑定目标（提交阶段经 `%bind`）；
  - `$\a`（任意 value）→ 按值相等匹配，即 `Exactly`；
  - `a+1`、stdlib 构造的 pattern value、shape、字面量 → 由 stdlib
    matcher 处理；
  - **对象 pattern 由 stdlib 构造器构建**：block 是对象/帧，不是 value
    literal；`{x, y}` 只是概念简写，源码中由 stdlib 的对象 pattern
    构造器生成（具体名字见 stdlib 章）。语法层不新增 pattern 字面量；
  - `\a`（location）**不是 v1 pattern**：location 不可作为 value 传给
    match。需要引用语义时使用指针（不安全）或显式 alias 特性（后续）。
- pattern 的匹配是纯函数 `match(pattern, subject) -> Bindings | fail`
  （stdlib）；subject 必须是 value，或经指针传入的非自有对象（不安全）。
  Bindings 是 `(symbol, value)` 列表，不能携带 location/对象子树。
- 匹配成功后，调用方（stdlib 的提交循环）对每个绑定执行
  `allocate` + `%bind`，把 value 复制进当前 block 的新 location。
  匹配期间绑定不可见；组合子/守卫需自行传递 accumulator。
- 匹配失败 ⇒ fail 值 + `break` 当前 block 的 result（04 §错误）。
  `:=` 不产生 fail；它失败是静态/动态的命名错误。
- **对象的结构化匹配边界**：非自有对象必须经指针，属不安全 API；
  Bindings 只能绑定叶子 value，不能把对象字段绑定成 value/location。
  v1 的安全匹配范围 = value + frozen 对象；对象内部遍历由 stdlib/
  comptime 反射或指针完成。

旧设计的 `Exactly`/`Symbol`/`Shape`/`If` 四类内置 pattern 消解为
value、symbol 和 stdlib matcher；无保留关键字，无 pin 运算符。
`:=` 左侧的 pattern 语法已退役。

## 编译期内核化简

**内核不认识 pattern。** 匹配是 stdlib 的纯函数：

```
match(pattern, subject) -> Bindings | fail
```

- Bindings 只装 value；location 不可作为 value 传递。解构绑定 = 调用方
  先 `allocate` 新 location，再 `%bind`，因此不需要“绑定即分配”的第二内核机制。
- matcher 必须纯：没有 rollback 兜底，副作用无法撤销。深嵌套短路靠组合子
  逐级返回 fail。纯度是 API 契约，不是内核强制；违反者自行承担失败残留。
- 退役：内核 `%match`、checkpoint/rollback、匹配失败信号、全局可重定义
  `\$match`、`\a` location pattern。

## 编译器内建原语（最小集，共 5 个）

判据：需要触碰语言私有结构（帧树、对象布局）的必须内建；
只操作 value 的全部下放 stdlib。

| 原语 | 理由 |
|---|---|
| `\` 解析（symbol → location，帧树导航） | 帧是私有结构 |
| `%bind(symbol, location)` → 命名当前 block 的直接子 location | 绑定的唯一入口 |
| 基础类型的 bits 相等（每种 builtin 一个比较原语） | value 比较的底 |
| `%field(place/object, symbol-value)` → location | 动态字段访问 |
| `%branch(cond, then-prototype, else-prototype)` | 纯值判别分支，一切分支的底 |

- `%branch` 不做绑定、不看 pattern、无 checkpoint：cond 为真走 then，
  否则走 else。`if`、`?`、match 分派、`while` 全部在其上由 stdlib 展开。
- **cond 的具体 builtin 类型（cond-type）留待 07；v1 暂定 Boolean。**
- `%field` 作用于自有对象的 place/view，或经指针兑现的 location；
  非自有对象访问必须显式使用指针。

## 标准库提供（全部可推导）

- `match(pattern, subject)` 纯函数：返回 `Bindings | fail`；
- 提交循环：`allocate` + `%bind`，把 Bindings 落进当前 block；
- 相等 `eq`：对 value/frozen 递归；对象比较走 `%field`/指针，属不安全面；
- `Exactly(x)`、`If` 守卫（= 调用闭包）；
- 对象/序列 pattern 构造器（具体名字由 stdlib 章定；语法层不内建
  对象 pattern literal）；
- 形状匹配（comptime 反射生成检验代码，同 `derive` 机制）；
- 选择 `p1 | p2`、合取 `p1 & p2` 组合子；
- 可逆算术 pattern（`a+1 : 3` 绑 `a` 到 2）：stdlib 定义“可逆操作”协议，
  匹配器遇到注册过的操作反向应用。内核不知道 `+` 可逆；
- range、字符串/正则、序列解包等一切花哨 pattern；
- 模式分派（match/switch）：宏，展开为连续 match + `%branch`。

**可扩展性界线**：`match` 对 pattern value 的形状分派——symbol/location/
普通 value 三种基本情况是语言语义固定的；带 matcher 字段的新 pattern
形状经 `p.matcher(v)` 扩展，匹配行为由 pattern value 自身携带，词法可见。
造绑定的能力被 `%bind` 锁在编译器手里，入口唯一；matcher 只能产出
Bindings 数据，提交由调用方执行。

## 指针

指针是 value（可传递、可存储、可跨 `@` 发送），指向 location 但自身
不是 location。解引用 `$ptr` 在当前求值环境现场兑现出 location，
兑现瞬间才受可变性检查。

| | 瘦指针 | 胖指针 |
|---|---|---|
| 内容 | 仅地址 | 地址 + 形状信息（长度/形状引用/分派信息） |
| 指向 | 形状静态已知的 location | 长度/形状动态的对象（slice、动态分派） |
| 对应机制 | 单态化一侧 | 装箱一侧：装箱 = 瘦指针变胖指针 |

**有效性不检查**：解引用失效指针是未定义行为。安全子集 =
owned location + view + cell；指针是不安全特性。非自有对象的读取、
结构化比较、容器内部链接使用指针；标准库可以内部使用并依靠不变量，
用户代码使用即离开安全子集。指针参与的 GC 根/跨协程有效性尚未定义。

## 与 01/02/06 的对接

- 固化：只允许 value、frozen、prototype/code 跨阶段；禁止固化 owned
  mutable location、裸指针、资源、frame handle（06 §9）。
- comptime 堆里同样只有 symbol/location/value/pointer，同一套规则。
- 02 的调用改为 `allocate` 分配 caller 的结果 location；共享 `Proc.in`
  交接槽已退役，改为 per-call frame；递归由 fix/self 协议提供。
- 跨协程共享可变状态的受检通道仍然只有 cell 协程 + `@`/`\^` 消息；
  指针绕过了检查，由“不安全特性”定位承担。
- block 的 self root = result location；`:=` 在块完成后命名它；块内字段
  用 `\this` handle/view。

## 词汇表

- 新增/明确：symbol（value）、location、pointer、`%bind`、`%field`、
  `%branch`、`\this`（帧 handle）。
- 运算符：`\`（解析）、`$`（读/指针兑现）、`=`（写）、`:=`（命名）、
  `:`（pattern 约束）。
- 退役：内置 pattern 类、全局 `\$match`、`` ` `` 取左值、左值类型一等公民、
  `readonly`、“标识符可重绑定/销毁”、内核 `%match`、checkpoint/rollback、
  匹配失败信号、`pattern := value`、`\a` location pattern。
- 留作后续设计：`%branch` 的 cond-type、`\` 的转义/解析双角色、
  指针取址拼写、可逆操作协议、matcher 注册机制。

## 已知边界（诚实清单）

- 读取噪音（`$\a`）：已接受的取舍。
- `\` 同时承担转义与解析两个角色：语法章裁决（候选：换符号；
  或规定转义只发生在字面量/标识符内部，运算符只在前缀位置）。
- 对象 pattern 不内建语法：由 stdlib 构造器生成 pattern value；
  结构化匹配/绑定受限于 value + frozen，自有对象跨字段解构需要指针或
  显式 move 协议，v1 不提供。
- 动态绑定（pattern 里放计算出的 symbol 值）合法化 = 动态作用域回归风险；
  默认允许但记为边界，必要时由 lint 限制。
- matcher 纯度不由内核强制；“纯”是失败残留语义的契约。

## 验收用例（自检清单）

1. `a := allocate ^3; $\a + 1` 得 4。
2. `3 : 3` 通过；`3 : 4` 失败产生 fail 并 break 当前 block；
   `a := 3` 非法（`:=` 只命名 location）。
3. `a+1 : 3` 绑 `a` 到 2（stdlib 可逆协议），提交循环为 `a` 分配新
   location；无编译器特判。
4. `$(\a) : 3` 按 value 匹配；`Exactly` 是 stdlib matcher，不产生绑定。
5. `eq` 对叶子 value/frozen 递归；对象比较需要指针，属不安全面。
6. per-location 可变性：不可变 root 可以含 mutable 字段；
   只读 view 使整条路径只读。
7. 瘦指针存取；胖指针 slice 解引用；失效指针解引用 = UB（文档断言）。
8. match 分派宏展开为连续 match + `%branch`，且 stdlib 之外无法造绑定
   （试图绕开 `%bind` 的 matcher 无法引入新 symbol 绑定）。
9. `:=` 在 RHS 完成后绑定；块内用 `\this` 访问字段，不暴露半构建 location。
10. 非自有对象的结构匹配必须显式使用指针；安全匹配范围 = value/frozen。

## 待办

- [ ] 控制流章已重写；05 章需固定 try/catch/if/matcher 宏形态
- [ ] 语法章：`\` 的转义/解析双重角色、指针取址拼写
- [ ] 07 章：Boolean/cond-type、shape 与静态类型检查
- [ ] stdlib 章：可逆操作协议、matcher 注册机制、对象匹配的 unsafe API
- [ ] 指针的取址运算符形式与 GC 根规则
