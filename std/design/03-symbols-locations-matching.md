# Symbol / Location / 匹配 设计记录

状态：草案（2026-10-25 按 06 的 location 模型回写；同日精简 kinds 为
value/location，mutability 并入 shape，`:=` 改为 stdlib dispatcher）

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、
[06-ownership.md](06-ownership.md)

## 核心原则

宇宙里只有两种 result kind：**value 与 location**。

- **value**：纯不可变数据，可传递、可固化、可 bit-copy，无身份与生命周期。
  子类（按 shape 分）：
  - 基础值：integer、symbol、boolean 等；
  - **pointer**：指向 location 的可复制 value，不拥有、不保活、不检查有效性；
  - fail、frozen 对象、prototype/code 值。
- **location**：`allocate <shape>` 或 block 的 self root 产生的格子，
  持有内容（value 或子 location）。属性：标识、当前内容、所有权状态、
  **shape（含 mutability）**。location 不可作为 value 传递；要把它"借给"
  别处用 pointer（不安全）或显式 alias（后续）。
- **place**（`\p`、`p.field`）不是 kind，是指向 location 的路径语法，
  求值得到 location。**view 不再是独立 kind**：借用经 fat pointer 实现，
  其携带的 shape 可与原 location 不同（含把 mutable 叠加为 immutable
  的只读借用），见 §指针。

**symbol** 是一等原子 value（类似 Lisp symbol）。标识符 token 即 symbol
value 字面量；可用于 pattern matching，可动态构造；不是编译期概念。

一个 block 持有若干 location + 一张 `symbol → location` 映射表；block 的
self root 就是它的 result location（06 §2.5），对象是一棵 location 树。

### 形状与 mutability

- **shape 是内容/对象的当前结构 + mutability 的乘积**，由内容决定。
  shaped location 的 mutability 在 shape 里；unshaped location 无 shape
  约束，shape+mutability 跟随当前内容，empty 时无 shape 无 mutability。
- **shape 约束是可选的静态契约**，不是 location 的固有属性。shaped
  location 的 mutability 由 shape 指定（默认 mutable，要 immutable 用
  `%seal`）；unshaped location 的 mutability 跟随内容。
- 显式约束在 empty 时保留；empty 只能按该约束接收内容。
- block/对象形状是动态的；block 的 self root 是 unshaped（必然——
  body 求值前不知道会装什么），默认 mutable，可单向 `%seal` 为 immutable。
- 存储布局（内联、装箱、重新分配）属于实现，不属于 shape 语义。

## 记号

| 写法 | 含义 | 类别 |
|---|---|---|
| `a` | symbol value 本身 | value |
| `^p` | 取 p 的 prototype value（延迟形态） | value (prototype) |
| `evaluate proto` | 在当前 env force 一个 prototype value | 透传（value/location） |
| `allocate <shape>` | 在当前 block 新建空 location（默认 mutable） | location |
| `new proto` | 在新协程 force prototype | 协程 handle（value） |
| `\a` | 把 symbol 沿帧树解析为 location | location |
| `\p.a` | location 路径/子 location | location |
| `$\p.a` | 读出叶子 location 的 value | value |
| `&p.a` | 取 location 的指针（拼写待定） | value (pointer) |
| `%drop(loc-expr)` | 递归销毁 location 及其 owned 子树 | unit（操作） |
| `%seal(loc)` | 单向 mutable → immutable；对已 immutable 是 no-op | unit（操作） |

- `$`：location → value（读取）；只对叶子合法，object location 不能整体读。
- `=`：stdlib dispatch（基础类型有内建 assign）；对 bit-copyable shape
  的 value 做 bit-copy，销毁旧内容，location 标识/所有权/shape 不变。
  非 bit-copyable 不能 `=`（用 move/clone）。表达式的值为被写的 location。
- `:=`：stdlib dispatcher（见 §绑定与 pattern）。不分配、不移动、不产生
  可丢弃值。绑定在 RHS 完全求值之后发生。
- `%drop`：唯一显式销毁原语；参数是产生 location 的表达式（place path、
  block 字面量、`allocate` 的结果等），先求值出 location。递归销毁 owned
  子树，目标变 empty，不从父 block 删除，绑定不删除。语义见 06 §6。
- `%seal`：location 系统原语，单向 mutability mutable→immutable。只对
  mutability = mutable 的 location 合法（含 unshaped 跟随成 mutable 的情况）；
  对已 immutable 是 no-op。shaped immutable location 由 shape 直接指定，
  不经 `%seal`。shape 直接操作（含 immutable→mutable 等不安全变更）留作
  unsafe 逃生口，拼写待定。
- `.`：location 路径；文档中 `p.a` 是 `\p.a` 的简写，正式源码必须显式
  解析 symbol（语法章可再决定是否放宽）。
- 读取必须显式写 `$`；已接受其噪音。指针解引用写作 `$ptr`，兑现为
  location，再读需 `$($ptr)`；拼写待语法章裁决。

## 可变性与所有权

- mutability 是 **shape 的一部分**，不是 location 的独立属性。
  shaped location 的 mutability 由 shape 指定；unshaped location 跟随内容。
- `allocate <shape>` 默认 mutable；要 immutable 用 `%seal`。block 的
  self root 是 unshaped 默认 mutable；可经 `%seal` 单向冻结为 immutable。
- **存储 mutability 不沿路径继承**：一个 immutable location 可以有
  mutable 字段；每个字段是独立 allocate 的 location（06 §4）。
- **只读借用经 fat pointer**：fat pointer 携带的 shape 可与原 location
  不同，把 mutable 叠加为 immutable 即得只读借用。深度只读的来源是
  fat pointer shape 的传播规则（具体传播范围待指针章）。借用是临时
  访问，不存储、不捕获、不返回。
- 字段级 mutability 由各字段自己的 allocate（shape）决定。
- 退役：`readonly`/`mutable` 二选一、深只读随根继承、旧"标识符可重绑定"、
  旧"mutability 是 location 属性、allocate 时确定终身不变"、
  旧"默认不可变"。

## 绑定与 pattern

绑定与 pattern 只有两个核心操作，方向固定：

```
x := <expr>                 !- stdlib dispatcher：依 RHS kind 分派 -!
value : pattern             !- 可反驳约束；失败产生 fail 并 break 当前 block -!
```

- **`:=` 是 stdlib dispatcher，不是内核原语**。它依 RHS 的 kind 分派：
  - **location**：直接 `%bind(symbol, location)`——绑定到现有 location，
    不复制、不 move。绕过通用 match 以保护 match 的纯度（match 主体
    不接受 location）。
  - **value**：走 **标准 symbol match**（stdlib 策略，对裸 symbol 返回
    `(symbol, value)`），提交循环 `allocate` + bit-copy + `%bind`，
    即 **auto-box**：value 进一个 **immutable** location。`x := 1` 合法。
  - **fail**：当 value 处理（auto-box；不自动接力）。
  - **借用**（经 fat pointer 兑现出的临时 location）：借用规则不允许 `:=`，
    报错（要长期持有需 move/clone）。
- **核心 `%bind(symbol, location)` 仍只接 location**——value 不经 core
  绑定。auto-box 是 stdlib 把 value 提升为 location 的桥。
- **`:` 是唯一的 pattern 语法壳。** pattern 是由 value 写成的普通表达式，
  不存在"pattern 模式求值"：
  - 裸 symbol `a` → 绑定目标（提交阶段经 `%bind`）；
  - `$\a`（任意 value）→ 按值相等匹配，即 `Exactly`；
  - `a+1`、stdlib 构造的 pattern value、shape、字面量 → 由 stdlib
    matcher 处理；
  - **对象 pattern 是 stdlib 构造出的普通 value，不是字面量**。block `{}`
    是对象/帧，不是 value literal，因此对象 pattern 不能借用 block 语法
    书写；源码里写的是构造器调用（例如 `ObjPattern(a, pa, b, pb)`，
    名字与参数形状由 stdlib 章定），实参是 symbol value 与子 pattern
    value。`{...}` 在源码中永远只表示 block/对象，任何位置都不表示
    pattern。语法层不新增 pattern 字面量；
  - `\a`（location）**不是 v1 pattern**：location 不可作为 value 传给
    match。需要引用语义时使用指针（不安全）或显式 alias 特性（后续）。
- pattern 的匹配是纯函数 `match(pattern, subject) -> Bindings | fail`
  （stdlib）；subject 必须是 value，或经指针传入的非自有对象（不安全）。
  Bindings 是 `(symbol, value)` 列表，不能携带 location/对象子树。
- 匹配成功后，调用方（stdlib 的提交循环）对每个绑定执行
  `allocate` + `%bind`，把 value 复制进当前 block 的新 location。
  匹配期间绑定不可见；组合子/守卫需自行传递 accumulator。
- 匹配失败 ⇒ fail 值 + `break` 当前 block 的 result（04 §错误）。
  `:=` 不产生 fail（auto-box 不失败）；它失败是静态/动态的命名错误。
- **对象的结构化匹配边界**：非自有对象必须经指针，属不安全 API；
  Bindings 只能绑定叶子 value，不能把对象字段绑定成 value/location。
  v1 的安全匹配范围 = value + frozen 对象；对象内部遍历由 stdlib/
  comptime 反射或指针完成。

旧设计的 `Exactly`/`Symbol`/`Shape`/`If` 四类内置 pattern 消解为
value、symbol 和 stdlib matcher；无保留关键字，无 pin 运算符。
`:=` 左侧的 pattern 语法已退役；`x := 1` 在旧版非法，现在经 auto-box
合法。

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

## 编译器内建原语（最小集，共 7 个）

判据：需要触碰语言私有结构（帧树、对象布局、location 系统状态）的必须
内建；只操作 value 的全部下放 stdlib。

| 原语 | 理由 |
|---|---|
| `\` 解析（symbol → location，帧树导航） | 帧是私有结构 |
| `%bind(symbol, location)` → 命名当前 block 的直接子 location | 绑定的唯一入口 |
| 基础类型的 bits 相等（每种 builtin 一个比较原语） | value 比较的底 |
| `%field(place/object, symbol-value)` → location | 动态字段访问 |
| `%branch(cond, then-prototype, else-prototype)` | 纯值判别分支，一切分支的底 |
| `%drop(loc-expr)` → 递归销毁 owned 子树，目标置空 | 触碰所有权状态、析构钩子与对象布局 |
| `%seal(loc)` → 单向 mutable→immutable | 触碰 location 系统的 shape/状态 |

- `%branch` 不做绑定、不看 pattern、无 checkpoint：cond 为真走 then，
  否则走 else。`if`、`?`、match 分派、`while` 全部在其上由 stdlib 展开。
  分支经 `evaluate` 在当前 env force，**不分配 result location，结果
  直接透传**（value/location 由被选 prototype 决定）。
- **cond 的具体 builtin 类型（cond-type）留待 07；v1 暂定 Boolean。**
- `%field` 作用于自有对象的 place，或经 fat pointer 兑现出的 location
  （携带的 mutability 决定只读/可写）；非自有对象访问必须显式使用指针。
- `%drop` 是唯一暴露给用户的销毁原语；`\destroy` 是销毁过程中调用的
  对象钩子，不是另一个操作。
- `%seal` 是 location 系统原语，单向 mutability mutable→immutable；
  对已 immutable 是 no-op。shape 直接操作（含 immutable→mutable）留作
  unsafe 逃生口。

## 标准库提供（全部可推导）

- **`:=` dispatcher**：依 RHS kind 分派（location → `%bind`；
  value → 标准 symbol match + auto-box；fail → 当 value；借用 → 报错）。
- **`=` assign dispatch**：依 LHS shape 分派；bit-copyable shape 走
  bit-copy（基础类型有内建 assign 原语），非 bit-copyable 报错（用
  move/clone）。assign 销毁旧内容，location 标识/所有权/shape 不变。
- **bit-copyable 判定**：基础类型天生 bit-copyable；用户结构体若字段
  全 bit-copyable 且无 `\destroy`，其 shape 也是 bit-copyable。判定在
  shape 定义时静态完成。**无 trait 机制**，bit-copyable 是 shape 属性。
- `match(pattern, subject)` 纯函数：返回 `Bindings | fail`；
- 提交循环：`allocate` + `%bind`，把 Bindings 落进当前 block；
- 相等 `eq`：对 value/frozen 递归；对象比较走 `%field`/指针，属不安全面；
- `Exactly(x)`、`If` 守卫（= 调用闭包）；
- 对象/序列 pattern 构造器（具体名字由 stdlib 章定；语法层不内建
  对象 pattern 字面量）；
- 形状匹配（comptime 反射生成检验代码，同 `derive` 机制）；
- 选择 `p1 | p2`、合取 `p1 & p2` 组合子；
- 可逆算术 pattern（`a+1 : 3` 绑 `a` 到 2）：stdlib 定义“可逆操作”协议，
  匹配器遇到注册过的操作反向应用。内核不知道 `+` 可逆；
- range、字符串/正则、序列解包等一切花哨 pattern；
- 模式分派（match/switch）：宏，展开为连续 match + `%branch`。

**可扩展性界线**：`match` 对 pattern value 的形状分派——symbol/value
两种基本情况是语言语义固定的；带 matcher 字段的新 pattern 形状经
`p.matcher(v)` 扩展，匹配行为由 pattern value 自身携带，词法可见。
造绑定的能力被 `%bind` 锁在编译器手里，入口唯一；matcher 只能产出
Bindings 数据，提交由调用方执行。

## 指针

指针是 value（可传递、可存储、可跨 `@` 发送），指向 location 但自身
不是 location。解引用 `$ptr` 在当前求值环境现场兑现出 location，
兑现瞬间才受可变性检查。

| | 瘦指针 | 胖指针 |
|---|---|---|
| 内容 | 仅地址 | 地址 + shape 信息（含 mutability、长度/形状引用/分派信息） |
| 指向 | shape 静态已知的 location | 长度/shape 动态的对象（slice、动态分派） |
| mutability | 取原 location 的 shape | **可携带与原 location 不同的 mutability**（含把 mutable 叠加为 immutable 的只读借用） |
| 对应机制 | 单态化一侧 | 装箱一侧：装箱 = 瘦指针变胖指针 |

- **只读借用 = fat pointer with immutable overlay**：原 location 的
  mutability 可以被 fat pointer 改写为 immutable 呈现，不修改原 location
  的 shape。这是"深度只读"的来源。
- **写借用 = fat pointer with mutable**：unsafe，绕过所有者；只有显式
  unsafe 代码可用。
- frame handle（`\this`、`\^`）类似 fat pointer：默认给出 immutable
  叠加的访问；mutable handle 是单独的 unsafe 操作。

**有效性不检查**：解引用失效指针是未定义行为。安全子集 =
owned location + cell；指针是不安全特性。非自有对象的读取、结构化比较、
容器内部链接使用指针；标准库可以内部使用并依靠不变量，用户代码使用即
离开安全子集。指针参与的 GC 根/跨协程有效性尚未定义。

## 与 01/02/06 的对接

- 固化：只允许 value、frozen、prototype/code 跨阶段；禁止固化 owned
  mutable location、裸指针、资源、frame handle（06 §9）。
- comptime 堆里同样只有 value/location（+ pointer value 子类），同一套规则。
- 02 的调用改为 stdlib wrapper 用 block 建 per-call frame + `evaluate`
  函数 prototype body；共享 `Proc.in` 交接槽已退役；递归由 fix/self 协议
  提供。
- 跨协程共享可变状态的受检通道仍然只有 cell 协程 + `@`/`\^` 消息；
  指针绕过了检查，由"不安全特性"定位承担。
- block 的 self root = result location；`:=` 在块完成后可命名它（auto-box
  value 或直接 `%bind` location）；块内字段用 `\this` handle 访问。

## 词汇表

- 新增/明确：value（含 symbol、pointer、fail、frozen、prototype 子类）、
  location、`%bind`、`%field`、`%branch`、`%drop`、`%seal`、
  `\this`（帧 handle）、`^p`（取 prototype）、`evaluate proto`、
  `allocate <shape>`。
- 运算符：`\`（解析）、`$`（读/指针兑现）、`=`（stdlib assign）、
  `:=`（stdlib dispatcher）、`:`（pattern 约束）、`%drop`（销毁）、
  `%seal`（mutability 冻结）。
- 退役：内置 pattern 类、全局 `\$match`、`` ` `` 取左值、左值类型一等公民、
  `readonly`、"标识符可重绑定/销毁"、内核 `%match`、checkpoint/rollback、
  匹配失败信号、`pattern := value`、`\a` location pattern、`destroy` 操作、
  `view` 作为独立 kind（合并入 location，借用经 fat pointer）、
  `mutable` 作为 location 独立属性（改为 shape 的一部分）、
  `allocate prototype` 写法（拆为 `evaluate proto` 与 `allocate <shape>`）、
  旧"默认不可变"（默认 mutable，要 immutable 用 `%seal`）。
- 留作后续设计：`%branch` 的 cond-type、`\` 的转义/解析双角色、
  指针取址拼写、可逆操作协议、matcher 注册机制、`%drop` 的正式语法、
  `%seal` 的正式语法、shape 直接操作（unsafe）的拼写。

## 已知边界（诚实清单）

- 读取噪音（`$\a`）：已接受的取舍。
- `\` 同时承担转义与解析两个角色：语法章裁决（候选：换符号；
  或规定转义只发生在字面量/标识符内部，运算符只在前缀位置）。
- 对象 pattern 不内建语法：由 stdlib 构造器生成 pattern value；`{...}`
  永远只表示 block/对象，不是 pattern。结构化匹配/绑定受限于 value +
  frozen，自有对象跨字段解构需要指针或显式 move 协议，v1 不提供。
- 动态绑定（pattern 里放计算出的 symbol 值）合法化 = 动态作用域回归风险；
  默认允许但记为边界，必要时由 lint 限制。
- matcher 纯度不由内核强制；"纯"是失败残留语义的契约。
- `:=` dispatcher 依 kind 分派需要运行期 kind tag 或 core `%kind` 原语；
  动态类型一侧有运行期开销，具体方案待 07。
- unshaped location 的 shape+mutability 跟随内容，意味着同一 location
  在不同时刻可呈现不同 mutability；`%seal` 后才不可逆冻结。lint 可提示
  unshaped mutable 跨越复杂控制流时的可读性风险。

## 验收用例（自检清单）

1. `a := 3; $\a + 1` 得 4（auto-box：`a` 绑定 immutable location）。
2. `3 : 3` 通过；`3 : 4` 失败产生 fail 并 break 当前 block；
   `a := 3` 合法（stdlib dispatcher auto-box value）。
3. `a+1 : 3` 绑 `a` 到 2（stdlib 可逆协议），提交循环为 `a` 分配新
   location；无编译器特判。
4. `$(\a) : 3` 按 value 匹配；`Exactly` 是 stdlib matcher，不产生绑定。
5. `eq` 对叶子 value/frozen 递归；对象比较需要指针，属不安全面。
6. per-location mutability：immutable root 可以含 mutable 字段；
   只读借用经 fat pointer immutable overlay 使访问只读。
7. 瘦指针存取；胖指针 slice 解引用；fat pointer 携带 immutable overlay
   实现只读借用；失效指针解引用 = UB（文档断言）。
8. match 分派宏展开为连续 match + `%branch`，且 stdlib 之外无法造绑定
   （试图绕开 `%bind` 的 matcher 无法引入新 symbol 绑定）。
9. `:=` 在 RHS 完成后绑定；块内用 `\this` 访问字段，不暴露半构建 location。
10. 非自有对象的结构匹配必须显式使用指针；安全匹配范围 = value/frozen。
11. `%drop(\p)` 递归销毁 owned 子树，p 变 empty 但 symbol 保留；
    `%drop` 对 value 是类型错误，对 empty 报错；`\destroy` 只是被调用的
    钩子，不是另一个销毁操作。
12. `x := { ... }`：block self root 是 unshaped 默认 mutable；
    `%seal(\x)` 单向冻结为 immutable；对已 immutable 是 no-op。
    `x := 1` auto-box 出一个 immutable location（标准 symbol match）。
13. `%branch` 经 evaluate 在当前 env force 被选 prototype；不分配 result
    location，结果透传。while 同样 transparent（04）。

## 待办

- [ ] 控制流章已重写；05 章需固定 try/catch/if/matcher/while 宏形态
- [ ] 语法章：`\` 的转义/解析双重角色、指针取址拼写、`^p`/`evaluate`/`allocate` 的具体语法
- [ ] 07 章：Boolean/cond-type、shape 与静态类型检查、运行期 kind tag 或 `%kind` 原语
- [ ] stdlib 章：可逆操作协议、matcher 注册机制、对象匹配的 unsafe API、
      bit-copyable shape 判定与 `=` assign dispatch、`:=` dispatcher 细节
- [ ] `%drop` 的正式语法（参数是 location 表达式）
- [ ] `%seal` 的正式语法与 unshaped 跟随内容时的具体冻结时机
- [ ] shape 直接操作（unsafe）的拼写与边界
- [ ] 指针的取址运算符形式与 GC 根规则、fat pointer mutability 叠加语法
