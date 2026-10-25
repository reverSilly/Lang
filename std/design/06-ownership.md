# 所有权设计记录

状态：草案（核心裁决已定，01–04 已回写；05 待写）
日期：2026-10-24，修订 2026-10-25

> 2026-10-25 裁决：block 的 result location 与 self root 是同一个 location；
> `\this` 是帧 handle（经 fat pointer 取得临时只读访问）；mutality 是
> shape 的一部分（不是 location 的独立属性）；shaped location 的 mutability
> 由 shape 指定，unshaped location 跟随内容；block 的 self root 是
> unshaped 默认 mutable，可经 `%seal` 单向冻结为 immutable；
> `allocate <shape>` 新建空 location（默认 mutable，不跑 prototype），
> `evaluate proto` 在当前 env force prototype（不新建 location、不换 env），
> `new proto` 在新协程 force prototype；旧 `allocate prototype` 写法已退役
> （拆为 `evaluate proto` 与 `allocate <shape>`）；move 可深入当前 block
> 的所有权子树；绑定在 RHS 完成后发生；非自有对象访问走裸指针；`;` 只
> 丢弃、不析构，未绑定 location 在所属 block 结束时统一回收。旧版本中的
> "独立隐式 result 槽""view 代替 handle""只能 move 直接子""分号处析构"
> "mutability 是 location 属性""默认不可变"均已废弃。
> 循环裁决：循环不产生 block；循环体里未包进 `{}` 的 `allocate` 属于外层
> block，循环不销毁它们。`{}` body 只是普通 block：未 `%drop` 时 root/
> 已命名字段活到父 block 结束；用户写 `%drop({ body })` 才每轮销毁。
> while 是 transparent macro/prototype，经 `evaluate` 跑 body/cond，不
> 分配脚手架、不换 env、不内部清理。
> `%drop`（参数为 location 表达式）是唯一显式销毁原语；`\destroy` 是被它调用的对象钩子；
> `;` 只丢弃、不销毁。
> bit-copyable 是 shape 属性（无 trait 机制）；`=` assign 是 stdlib dispatch
> （基础类型有内建 assign），非 bit-copyable 走 move/clone。`:=` 是 stdlib
> dispatcher，依 RHS kind 分派（location → `%bind`；value → 标准 symbol
> match + auto-box；fail → 当 value；fat-pointer 借用 → 报错）。

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、
[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、
[04-control-flow.md](04-control-flow.md)

范围：location 的所有权、`block` 作为对象字面量、`allocate <shape>` /
`evaluate` 与 `:=` 的分工、move / `%drop` / `%seal`、跨 block 与跨协程
转移、comptime 边界和指针的定位。
本章不解决 05 的宏展开细则与完整的静态类型系统；只固定所有权语义，
以及 shape 作为"内容当前结构 + mutability + 可选约束"的基础规则。

## 核心裁决

1. **value 没有所有权。** value 不可变、可复制、无身份、无析构。
   pointer、symbol、fail、frozen、prototype 都是 value 的子类。
2. **所有权绑定在 location 上。** location 是唯一可被拥有、移动、销毁的
   东西。
3. **所有权是一条父到直接子的边。** block/对象拥有其直接子 location；
   子 location 再拥有它的子 location。所有权图是一棵森林。
4. **block `{}` 是特殊形式（对象字面量），其 self root 就是它的 result
   location。** result 分配在**父 block** 中，是父 block 的直接子 location。
   block 求值期间它是对象容器（self root，也是新 env）；正常完成时内容为
   字段对象，`break` 时内容为 break 值。二者是同一个 location，不存在第
   二个隐式 result 槽。block 不是任何其他形式的糖。
5. **`allocate <shape>` 只做一件事：在当前 block 分配一个带 shape 的直接
   子 location。** 默认 mutable；要 immutable 用 `%seal`。不跑 prototype、
   不换 env。**`evaluate proto`** 在当前 env force 一个 prototype value；
   不新建 location、不换 env——prototype 体内的 `%bind`/`%field` 直接作
   用于当前 block 的 self root（现场填充 implicit root）。**`new proto`**
   在新协程 force prototype，env = 协程根 location。
6. **`:=` 是 stdlib dispatcher，依 RHS kind 分派**（不是内核原语）：
   - location → 直接 `%bind(symbol, location)`（绕过通用 match 保护其纯度）；
   - value → 标准 symbol match（stdlib 策略）+ auto-box：`allocate` 一个
     immutable location、bit-copy value、`%bind`（immutability 来自 shape
     或 `%seal`）；
   - fail → 当 value 处理；
   - 经 fat pointer 的借用 → 报错（要长期持有需 move/clone）。
   核心 `%bind` 仍只接 location；value 不经 core 绑定。绑定在 RHS 完全求值
   之后发生，不暴露构建了一半的 location。`x := 1` 合法（auto-box）。
7. **对象不能作为 value 传递。** block root、`allocate <shape>` 结果都是
   location；跨 block 使用只能 move location 子树，或经 fat pointer（携带
   mutability 叠加）临时只读借用。
8. **销毁恰好一次。** 沿所有权树递归；empty 槽跳过。
   可变共享只经 cell 协程；只读共享只经 frozen；裸指针不在安全子集内。
9. **没有借用检查。** `\this` 是帧 handle，经 fat pointer（携带 immutable
   叠加）取得对帧内 location 的临时只读访问；借用不能存储、捕获、返回。
   非自有对象的访问使用裸指针或 mutable fat pointer（unsafe），属不安全特性。
10. **shape 是内容结构 + mutability 的乘积，mutability 是 shape 的一部分**
    （不是 location 的独立属性）。shaped location 的 mutability 由 shape
    指定；unshaped location 无 shape 约束，shape+mutability 跟随当前内容
    （empty 时无 shape 无 mutability）。break/move 在有显式 shape 约束时
    检查；无约束的槽可接收任意 shape+mutability 的内容。
11. **move 不产生值。** 它只转移 location 子树的拥有权，本身产出 unit；
    目标槽由接收方预先分配（需 mutable、shape 匹配）。源可以是当前 block
    所有权子树中的任意位置，不限于直接子；源变 empty，并保留其 shape/尺寸
    约束（typed empty）。move 只对 location 子树，不对 value（value 永远是
    bit-copy）。
12. **block 的值就是它的 result location。** 正常完成时 result 内容是对象；
    `break v` 把 v 写入 result，替换原内容（原字段先销毁，除非被 v move 走）。
    result 与 self root 同一；`\this` 是该 location 的 handle。
13. **绑定之后才可命名。** block 求值完成前不绑定其 root；块内访问自身字段
    用 `\this`。block 不适合作为递归命名值；递归由 prototype + stdlib 的
    fix/self 协议提供（Y combinator 风格），不依赖半构建的 location。
14. **可见性后置。** v1 的 block 字段默认对可导航者可见；可见性修饰属于
    后续设计，不影响本章的所有权规则。
15. **`;` 只丢弃，不析构。** 丢弃 value 无操作，丢弃 location/place 也不
    销毁它。没有 `:=` 绑定的 location 在所属 block 结束时统一按创建逆序
    回收析构；显式提前回收用 `%drop`。`;` 不缩短生命周期。
16. **循环不产生 block。** `while` 是 stdlib transparent macro/prototype，
    经 `evaluate` 跑 body/cond，不分配自己的 self root、脚手架槽或 result
    location，不换 env，不建立用户可见的符号作用域，不内部清理。循环体
    里未包进 `{}`、也未显式 `%drop` 的 `allocate <shape>` 得到外层 block
    的直接子 location，循环结束后仍存活到外层 block 结束。
    `{}` body 只是普通 block：未绑定/未 `%drop` 时，root 与已命名字段作为
    父 block 的临时量活到父 block 结束；用户写 `%drop({ body })` 才在每轮
    结束销毁它。while 不引入额外的循环级销毁规则。
    内核没有"循环体自动析构"规则；`;` 和循环边界都不提供回收点。
17. **`%drop` 是唯一显式销毁原语，参数是产生 location 的表达式。** 它递归销毁目标拥有的整棵
    location 子树，目标 location 变 empty（typed empty），但**不从父 block
    删除**。绑定不参与销毁：目标上的 symbol 保留并指向 empty。`\destroy`
    是对象上的析构钩子，不是另一个销毁操作；`;` 仍然只丢弃。
18. **`%seal` 是 location 系统原语，单向 mutability mutable→immutable。**
    只对 mutability = mutable 的 location 合法（含 unshaped 跟随成 mutable
    的情况）；对已 immutable 是 no-op。shaped immutable location 由 shape
    直接指定，不经 `%seal`。shape 直接操作（含 immutable→mutable 等不安全
    变更）留作 unsafe 逃生口，拼写待定。
19. **assign 是 stdlib dispatch**（基础类型有内建 assign 原语）。对
    bit-copyable shape 的 value 做 bit-copy，销毁旧内容，location 标识/
    所有权/shape 不变；非 bit-copyable 不能 `=`（用 move/clone）。基础类型
    天生 bit-copyable；用户结构体若字段全 bit-copyable 且无 `\destroy`，
    其 shape 也是 bit-copyable。**无 trait 机制**，bit-copyable 是 shape 属性。
    value 永远是 bit-copyable shape 的实例；非 bit-copyable 对象不能 `$`
    读出 value、不能 `=`，只能 move/clone。

## 一、本体

### 1.1 本体

- **value**：不可变数据。无身份、无生命周期、无所有权。读取 value 是
  bit-copy。子类：基础值（integer/symbol/boolean）、pointer、fail、
  frozen 对象、prototype/code。
- **location**：有身份的存储槽。内容可以是叶子 value、若干子 location，
  或 empty；属性 = 标识 + 当前内容 + 所有权状态 + **shape（含 mutability）**。
  shaped location 的 shape（含 mutability）由 shape 约束静态指定；
  unshaped location 无 shape 约束，shape+mutability 跟随当前内容（empty
  时无 shape 无 mutability）。
- **symbol**：名字。只有绑定关系，没有所有权。是 value 子类。
- **block/对象**：block 求值时在父 block 分配的 result location（同时是
  self root，新 env）及其字段；对象就是一棵 location 树，不是独立的值。
  block 的 self root 是 unshaped，默认 mutable；可经 `%seal` 单向冻结为
  immutable。
- **place**：指向 location 的路径（`\p`、`p.field`），不是 kind；求值得到
  location。
- **借用**（view 已合并入 location）：经 fat pointer 携带 immutable overlay
  实现的临时只读访问；不是独立 kind。fat pointer 是 value（unsafe）。
- **pointer**：指向 location 的可复制 value（value 子类），用于访问非自有
  对象；不拥有、不保活、不检查有效性，属于不安全特性。fat pointer 携带
  shape（含 mutability），可与原 location 不同（mutable 叠加为 immutable
  即只读借用）。

### 1.2 所有权边

- block 拥有它的直接子 location。
- 一个对象 root 拥有它的字段 location。
- 一个 location 若拥有子 location，它就是这些子 location 的对象。
- 裸 value 没有被拥有这一说；它只是某个 location 的内容或被复制的数据。

### 1.3 所有权不变量

R1. 每个 live location 恰有一个拥有者：父 block、父对象，或 coroutine 根。

R2. 所有权沿 location 树向下传递；子 location 的销毁由父对象负责。

R3. owned 对象图是无环森林。环只能经 cell 协程或裸指针产生，
因此销毁不需要环收集。

R4. 一个 location 只能在它属于当前 block 的所有权子树时被 move（可以是
直接子，也可以是孙及更深）；源位置变 empty，目标获得整棵子树的拥有权。
被 move 出的位置保留 shape/尺寸约束，成为 typed empty，父对象布局不失效。

R5. value 可复制；location/对象绝不隐式复制。深拷贝只能经显式 `clone`。

R6. uninitialized / empty location 不可读、不可 move；`%drop` 对
uninitialized 允许（no-op，转 empty），对 empty 报错；递归清理时跳过
empty。uninitialized / empty 可以作为 move/`=` 的目标（初始化）；有显式
shape 约束时，目标内容必须满足约束。

R7. symbol 绑定不产生所有权。一个 symbol 的消失不会销毁 location；
一个 location 的销毁也不会因为 symbol 的存在而延迟。

R8. 跨 block 不得存在 location 别名。借用经 fat pointer（携带 immutable
叠加）是唯一的受控临时只读别名，来自帧 handle；裸指针/mutable fat pointer
不保证别名安全，属不安全子集。

### 1.4 形状与约束

- **shape = 内容/对象的当前结构 + mutability 的乘积**，由内容决定，不是
  location 的固有容量；empty location 没有内容形状、没有 mutability。
- location 可以是 **shaped**（有 shape 约束）或 **unshaped**（无约束）。
  shaped location 的 mutability 由 shape 指定（默认 mutable；要 immutable
  用 `%seal`）；unshaped location 的 shape+mutability 跟随当前内容。
  只有显式类型注解/接口契约才产生 shape 约束。
- 缺省无约束：empty 槽可以接收任意形状；有约束时，empty 仍保留约束，
  写入/move 必须满足它（typed empty 仍由 shape 指定 mutability）。
- 存储布局（内联、装箱、重新分配）属于实现，不属于 shape 语义。
  move 不是按位复制，目标槽不需要预先有足够大的空间。
- block 的 self root 是 **unshaped**（body 求值前不知道会装什么），
  默认 mutable；正常完成时形状来自字段对象；`break` 可以整体替换 root
  的内容为任意形状的值。可经 `%seal` 单向冻结为 immutable；冻结后
  `break` 替换 result 会报错。
- 静态可证明形状时，编译器可以内联固定布局、消去检查；
  动态形状走间接/重分配。二者语义相同，静态失败不得拒绝程序。

## 二、block = 对象字面量

### 2.1 self root 分配在父 block

设 block `B` 在父 block `P` 中被求值。`B` 的 self root `R_B`：

1. 由 `B` 的求值在 **P** 中隐式 `allocate`；
2. 是 `P` 的一个直接子 location，因此 `P` 的 `:=` 可以在 **B 完成后**绑定它；
3. 在 `B` 求值期间作为 `B` 的对象容器，同时就是 `B` 的 result location；
   其直接子 location 是 `B` 的字段；
4. `B` 内部的 symbol 只能绑定 `R_B` 的直接子 location；
5. `B` 内部的 `\this` 是 `R_B` 的帧 handle，经 fat pointer（immutable
   overlay）取得临时只读访问；`R_B` 本身不是 `B` 的直接子，`B` 不能用
   `:=` 命名它；
6. `P` 不能把 `R_B` 的字段绑定成自己的 symbol；可以通过路径读取/写入，
   或在持有整棵子树所有权时 move 该位置的子树。

这条规则由“symbol 只能绑定同一 block 下的 location”直接推出：
如果 root 分配在 block 自己内部，父 block 永远绑不到它。

```
P 的直接子 location
├─ p → R_B                  (p := { ... })
└─ ...
R_B 的直接子 location = B 的字段
├─ a → L_a                  (a := allocate ...)
└─ q → R_B2                 (q := { ... })
R_B2 的直接子 location = B2 的字段
└─ b → L_b                  (b := allocate ...)
```

- P 能绑定 p；
- B 能绑定 a、q；
- B2 能绑定 b；
- P 不能绑定 p.a 或 p.q.b，P 只能走路径 `p.a`、`p.q.b`。

> 术语约定：在 B 内部说“当前 block 的直接子”时，指的是 `R_B` 的直接子；
> B 的帧本身不拥有 location，`R_B` 是 B 的容器。`allocate` 在 B 内分配时
> 就是把新 location 挂到 `R_B` 下；`:=` 也只能命名这些直接子。

### 2.2 符号表、字段与寿命

- block 的符号表就是它当前已命名的直接子 location 表。
- `:=` 命名在右侧 block/prototype **完全求值之后**发生；块内部看不到尚未
  完成的同 block 绑定。块内访问自身字段用 `\this`。
- block 结束时，未命名的直接子 location 是临时量，按创建逆序销毁；
  已命名的直接子 location 成为对象字段，随 self root 存活。
- block 的**符号**在 block 结束时消失；**字段**随对象存活。
  这正是“block 是对象字面量”的含义。
- 若 `R_B` 没有被父 block 绑定，它本身就是父 block 的临时 location；
  在父 block **结束时**随未绑定 location 统一回收，其字段随之销毁。
  `;` 不会提前销毁它（核心裁决 15）。
- 循环体 `{}` 与普通 block 相同：未 `%drop` 时 `R_B` 随父 block 结束回收；
  写 `%drop({ body })` 时由该调用提前销毁它（§2.6）。
- 若 `R_B` 被绑定为 `p`，其寿命属于父 block，直到父 block 结束或被 move。

### 2.3 绑定 block

```
p := {
    a := allocate integer
    \a = 1
    q := { b := 2 }
}
```

- `R_B` 在父 block 中分配；`p := ...` 在 B 正常完成/break 完成后**命名**
  同一个 location，不移动、不复制。
- `B` 内部的 `a`、`q` 由 B 的符号表绑定；B 结束后不可再按名字访问，
  但可通过 `p.a`、`p.q.b` 的路径读取，或经指针访问。
- `p.q` 是 `p` 的子 location，不是当前 block 的直接子：当前 block 不能用
  `:=` 命名它；但在拥有 `p` 整棵子树的前提下，可以 move 它的子树（R4），
  或经 fat pointer 临时只读访问。
- 递归的 block 不通过名字自引用；块内字段用 `\this`，递归函数由 stdlib
  的 fix/self 协议构造（见 §2.4 与 §7）。

### 2.4 `\this` 是 handle，不是 location

`\this` 是当前 block 的**帧 handle**（一个普通 value），指向 `R_B`：

- 经 handle 的 fat pointer 默认携带 immutable overlay：取得对帧内字段的
  临时只读访问；借用不能存储、捕获、返回或 move。mutable handle 是单独的
  unsafe 操作（03）。
- 写自己的字段不需要 handle：块内 `\a = v` 经当前 env（`R_B`）解析，
  直接写当前 block 的直接子 location。
- `\this` 本身可以复制、存储、捕获；但 handle 的有效期绑定 `R_B` 的帧。
  帧结束后再经该 handle 访问或 break 是运行时错误；静态可证明时
  编译期拒绝。帧 handle 不允许固化到 comptime 边界。
- `B` 内不能 `x := \this` 的原因是 handle 是 value：`:=` dispatcher 会
  auto-box 它（合法但通常无意义），不会把它绑成帧内 location。
- `B` 内不能命名 `R_B` 本身：`R_B` 是 P 的直接子，不是 B 的直接子。
- 如果 `B` 需要返回自身对象，由 P 中的绑定或调用协议处理，
  而不是在 B 内部再造一个 symbol。

### 2.5 result location = self root

- `B` 的 result location 就是 `R_B`，在 block 进入时由 P 分配；
  没有第二个隐式 result 槽，也不存在“result 不是对象字段”这一层区别。
- **正常完成**：`R_B` 的内容是 B 的字段对象；未命名临时量先销毁。
  B 的值是 `R_B` 这个 location。末表达式不自动成为 block 值：
  若它为 value 则丢弃；若它新建了一个未绑定的直接子 location，则作为
  临时量在 block 结束时回收；若它只是已有 location 的 path，
  不改变其所有权，也不会被销毁。
- **`break v` 到 B**：`R_B` 的内容被 v 替换。写入前先销毁 B 的既有字段
  （含 `\destroy`），除非 v 是其中一个字段并被 move 出去；若 v 是 location，
  先把它从原位置摘下，再销毁剩余字段，最后装入 `R_B`，不得形成自引用环。
- **`break v` 到祖先 A**：先摘下 v；依次终止当前 block 到 A 之间的所有
  block，销毁各自的结果对象/字段；再以同样规则替换 `R_A` 的内容。
  A 随即完成，控制权回到 A 的父 block。
- result 默认无显式 shape 约束；若 `R_B` 显式带约束，最终内容必须满足它，
  否则是 shape/类型错误。break 不额外改变 block 对象以外的形状。
- `R_B` 是 unshaped 默认 mutable；block 完成后可经 `%seal(\R_B)`（即对
  block 结果 location）单向冻结为 immutable。冻结后 `break` 替换 result
  或任何写入都报错。
- `result` 是普通用户字段名，与 `R_B` 没有命名关系；`R_B` 没有源码标识符。

### 2.6 循环

- 循环表达式不是 block：没有 self root，不新增 `\this` 目标，不是 `:=`
  的附着对象，也不产生 block 结束时的自动回收。它只是 stdlib 宏展开出的
  控制流。
- 循环体里未包 `{}` 的 `:=` 落在外层 block，第二轮就是重复绑定，因此需要
  每轮新名字时必须把循环体写成 `{}`。未包 `{}`、也未显式 `%drop` 的
  `allocate` 同样属于外层 block，存活到外层 block 结束（`;` 和每轮结束都
  不销毁）。
- 循环体是 `{}` 时，它仍按 §2.1 分配 result location；未绑定/未 `%drop`
  时，root 与已命名字段作为父 block 的临时量活到父 block 结束。每轮 block
  完成只回收块内未绑定临时量（含 `defer` guard）；已命名字段不在此列。
- 需要每轮销毁整个 body block 时，用户写 `(%drop({ body })) while cond`。
  `%drop` 接收 block 的 result location，递归销毁后它变 empty；下一轮重新
  分配 root。该 root 通常是未绑定临时量，其 empty 槽可按 §6 被实现物理
  回收，因此不会按迭代积累空槽。while 不识别 body 语法、不引入额外的
  循环级销毁规则，也不持有 body root。
- 非 block body 若用户写 `%drop(allocate integer)`，同样每轮销毁该
  location；不写则按裁决 15 活到外层 block 结束。
- while 自己的控制状态/分支结果不得成为用户循环体 allocation 的所有者：
  它们的复用/回收是 05 的实现契约，不能表现为用户可见的 block。

## 三、allocate 与 :=

| | `allocate <shape>` | `:=` |
|---|---|---|
| 创建 location | 是 | 否（dispatcher：location → `%bind`；value → auto-box） |
| 建立所有权边 | 当前 block → 新 location | 否 |
| mutability | 在 shape 里；默认 mutable，`%seal` 冻结 | 不授予；auto-box 出的是 immutable |
| 要求 RHS 是 location | 不适用 | 仅在 location 分支要求，且必须是当前 block 直接子 |
| 产生值 | 返回 location | 产生 unit，不产生可丢弃的值 |

### 3.1 `x := 1` 是合法的

`1` 是 value：

- 没有 location，不能直接 `%bind`；
- `:=` dispatcher 对 value 走**标准 symbol match + auto-box**：
  在当前 block `allocate` 一个 immutable location（immutability 可来自
  shape 或 `%seal`）、bit-copy 1、`%bind`。

若需要不同的 mutability 或 shape，显式选择 location：

```
!- 不可变命名（auto-box，等价写法见上） -!
x := 1

!- 可变命名：分配后写入 -!
y := allocate integer
\y = 1

!- 冻结为 immutable（把已有 mutable location 单向 seal） -!
%seal(\y)

!- 不需要名字 -!
f(1)
```

- 所有权在**当前 block**，由 `allocate <shape>` 或 block self root 建立；
  `x` 只是名字。
- mutability 是 shape 的一部分：shaped location 由 shape 指定；
  unshaped 跟随内容。`allocate <shape>` 默认 mutable；要 immutable 用
  `%seal`。`:=` dispatcher 不授予 mutability。
- immutable 的常用写法：auto-box（`x := v`）一步得到 immutable location；
  或 `allocate <shape>` + `=` + `%seal`。
- mutable location 可以后续写入，也可保持未初始化。
- 未初始化的 location 处于 uninitialized（有 shape 约束时是 typed）；
  读取、再次 move 是错误；`%drop` 对 uninitialized 允许（no-op，转 empty）；
  uninitialized 可作为 move/`=` 的目标（初始化）。
- `allocate <shape>` 得到带 shape 约束的空 location（默认 mutable）；
  `evaluate proto` 不新建 location，在当前 env 里 force prototype
  （prototype 体的 `%bind` 直接写进当前 block 的 self root）；
  两者是不同原语，旧 `allocate prototype` 写法已退役。
- value 类的东西（symbol、pointer、frame handle、fail 等）可由 `:=` 的
  dispatcher auto-box（`x := v` 合法）；核心 `%bind` 仍只接 location。

### 3.2 绑定的同 block 规则

`:=` dispatcher 依 RHS kind 分派：

1. **location**：必须是当前 block 的直接子 location；当前未被绑定到另一
   symbol（v1 不允许多名一址；别名可后续作为独立特性引入）；可以是
   empty（例如刚 `allocate` 尚未初始化），但绑定本身不填值。
2. **value**：stdlib auto-box——在当前 block `allocate` 一个 immutable
   location + bit-copy value + `%bind`。`x := 1` 合法。
3. **fail**：当 value 处理（auto-box）。
4. **经 fat pointer 的借用**：报错；要长期持有需 move/clone。

因此：

```
x := 1                             !- 合法：auto-box value -!
x := allocate integer              !- 合法：allocate shape 后 %bind -!
p := { ... }                       !- 合法：block result 分配在父 block -!
x := \p.a                          !- 非法：p.a 不是当前 block 直接子 -!
x := $(...)                        !- 走 auto-box（value 路径） -!
x := &p.a                          !- 走 auto-box（pointer 是 value） -!
```

### 3.3 move 与源槽

- move 的单位是整棵 location 子树；被移动的是 location，不是 value。
  value 永远 bit-copy，不参与 move。
- move 的源可以是**当前 block 所有权子树中的任意位置**：直接子、孙及更深
  都可以。当前 block 直接拥有 root，因而拥有其整棵子树；不能 move 一棵
  不属于自己的子树。
- 当前 block 的直接子包括：`allocate <shape>` 的结果、子 block 的 result
  location、已绑定的字段等；孙位置通过 root 的路径访问。
- **目标槽必须是 mutable**；接收方先分配目标槽，move 只把子树填进去。
  move 到 immutable 目标是类型错误（v1 不提供 move-into-immutable；
  immutable 对象由 prototype 现场构造或 `=` 初始化后 `%seal`）。
- move 后源槽变 empty：槽还在、symbol 还在，但读、再次 move、`%drop`
  都是错误。
- 被 move 出的位置保留其 shape/尺寸约束（typed empty），父对象的布局继续
  有效；empty 可接收同一约束的后续 move。没有显式约束时，empty 不保留内容
  形状，只保留“曾占用的尺寸”作为实现选项。
- **目标槽不能由 move 任意指定。** 接收方先分配目标槽，move 只是把子树
  填进去；目标 block 由谁分配该槽决定。边界协议见第七章。
- 目标槽默认不限形状；若它显式带有 shape 约束，则检查被移入子树的形状；
  不满足是 shape/类型错误。没有约束时，存储可以重新分配或装箱，
  **不做按位复制**，也不存在“容量不够”。
- 目标槽一般处于 empty；若协议允许覆盖，先按销毁规则处理旧内容。
- move 本身产生 unit，不是值表达式；不存在“被移动值”。
- 源槽的存储可以立即释放；empty 槽本身仍属于其所有者对象。
  block 结束时符号绑定消失：若 block 对象是临时量，空槽随对象销毁；
  若对象逃逸，空槽作为 tombstone 留在对象形状中，销毁时跳过。

## 四、可变性

本章采用：

> **mutability 是 shape 的一部分；shaped location 的 mutability 由 shape
> 指定，unshaped location 跟随内容。只读借用经 fat pointer 的 immutable
> overlay 实现，沿访问路径传播。**

- `allocate <shape>` 默认 mutable；要 immutable 用 `%seal`（单向
  mutable→immutable；对已 immutable 是 no-op）。
- block 的 self root 是 unshaped，默认 mutable；可经 `%seal` 冻结。
  冻结后不能再写、不能再 break 替换 result。
- `:=` dispatcher 不改变 mutability；auto-box 出来的 location 是 immutable。
- block 的字段可以各自不同 mutability，因为每个字段都是独立 `allocate`
  的 location，各自 shape 决定。
- 只读借用 = fat pointer 携带 immutable overlay，使整条访问路径只读；
  这是"深度只读"的来源。fat pointer 的 shape 可与原 location 不同。
- block root 自身的 mutability 只影响"能否整体替换 root 这个 location"，
  不影响其字段各自的写权限。
- mutability 与 shape 约束不可分离（同一件事）：写入/move 先受所有权与
  mutability 约束，再检查 shape 兼容；没有显式约束就不做形状检查，
  mutability 跟随内容。
- **退役**："可变性是 location 的独立属性、allocate 时确定终身不变"、
  "默认不可变"、"readonly/mutable 二选一"、"只读沿 view 继承"。

## 五、状态机

一个 location 的所有权状态只有三个；mutability 不是状态，是 shape 的
一个字段：

- `uninitialized`：刚由 `allocate <shape>` 分配、尚未写入内容；保留 shape
  约束（typed），没有内容 shape / 内容 mutability（mutability 由 shape 指定）。
- `live`：有内容，归某个 block/对象所有；当前 shape+mutability 来自内容
  （shaped location 则来自 shape 约束）。
- `empty`：曾经 live、已被 move 或销毁；没有内容 shape、没有内容 mutability
  （shaped location 保留 shape 约束，为 typed empty，mutability 仍由 shape
  指定）。

**`%seal` 不是状态转移**：它只改 `location.shape.mutability ← immutable`
（单向；对已 immutable 是 no-op）。sealed location 仍是 owned、仍有身份、
销毁时仍走所有权析构——seal 改的是写权限，不是所有权。

**frozen 也不是状态**（见 §8.2）：frozen 是无所有权的只读快照，不在
location 状态机里。它是"freeze"操作从 owned 对象产生的 value（深拷贝、
不可变、无 `\destroy`、无资源），用于跨阶段固化与多协程只读共享。
**seal 改权限，freeze 造 value**——`%seal` 造不出 frozen：sealed 对象
销毁时仍恰好一次，无法被第二个所有者共享。

| 操作 | 前置 | 结果 |
|---|---|---|
| `allocate <shape>` | — | 新建 location，归当前 block；默认 mutable，处于 uninitialized；不跑 prototype |
| `evaluate proto` | — | 在当前 env force prototype；不建 location、不换 env；结果按 kind 透传 |
| `new proto` | — | 新协程根（新 env）；GC 管理 |
| read `$p` | p 是 value 叶子、live | 返回 bit-copy；object location 不可整体读 |
| move `\p` | p 在当前 block 所有权子树内、live；目标槽由接收方预分配且 mutable | p 变 typed empty；目标获得子树；无约束不查形状，有显式约束才查 |
| assign `\p = v` | p 是 mutable location（live 或 uninitialized）；v 是 bit-copyable shape 的 value | stdlib dispatch（基础类型内建）：销毁旧内容（若有），bit-copy v；p 标识/所有权/shape 不变 |
| clone `\p` | 图内只有 bit-copyable value 或可克隆资源 | 新的独立 owned 子树；bit-copyable 退化为 bit-copy |
| `%drop(\p)` | p 在当前 block 所有权子树内；live 或 uninitialized | live：递归销毁 owned 子树；uninitialized：no-op；p 变 empty |
| `%seal(\p)` | p 的 shape.mutability = mutable（含 unshaped 跟随成 mutable） | 改写 shape.mutability ← immutable（单向）；对已 immutable 是 no-op；所有权/身份/析构不变 |
| 借用（fat pointer） | 经 `\this` 等帧 handle，在同步调用期间；fat pointer 携带 immutable overlay | 临时只读访问，禁止存/捕获/返回/move |
| pointer `&p` | p 的 lifetime 由使用者保证 | 可复制 value；非自有访问，不检查有效性 |

错误分类：

- 静态可判定：`:=` 绑定非直接子 location、复制 owned 对象、移动借用、
  显式 shape 约束不兼容、非 bit-copyable 的 `=`、move 到 immutable 目标、
  `%drop` 已 empty；
- 动态需检查：读取 uninitialized/empty、重复 move、重复 `%drop`、使用已
  销毁对象、使用过期帧 handle；
- 无显式 shape 约束时不做形状检查；容量/布局不是语义错误；
- 指针失效不检查，解引用为 UB。

## 六、销毁

`%drop(expr)` 是唯一显式销毁原语，返回 unit。`expr` 是任意产生 location
的表达式（place path、block 字面量、`allocate <shape>` 的结果等），先求值
得到 location，再检查它属于当前 block 所有权子树内（直接子或后代）。
对 value 是类型错误；对 uninitialized 是 no-op（转 empty）；对 empty 是
错误（递归清理跳过 empty）。

销毁一个 owned 对象时：

1. 标记为 destroying，拒绝重入；
2. 若存在 `\destroy` 字段，以 self 的临时只读访问调用它；
3. 按创建逆序销毁仍 live 的直接子 location；empty 跳过；
4. 释放 root 的内容，状态变 empty（若有显式 shape 约束，保留为 typed empty）。

目标与后代：

- 语义上，目标是唯一保留为 empty 的 location；它在父 block/对象中的槽位
  不变，symbol/path 仍解析到它，但读、再 move、再 `%drop` 报错。
- 后代随目标内容一起销毁，不作为空的嵌套子槽保留在 empty 目标里。
- 若目标是某个 live 祖先的后代（如 `%drop(\p.q)`），`q` 本身作为 empty
  槽留在 `p` 中（tombstone），它的后代消失；之后销毁 `p` 时跳过 empty `q`。
- 对未绑定临时量的 empty 槽，实现可在无安全引用时物理回收槽位；绑定
  location/对象字段保留 empty 槽以维持身份与形状。

绑定：

- 绑定不在 location 内部，销毁不删除任何 symbol。命名目标的 symbol 保留
  并指向 empty；可重新初始化（mutable / typed empty）后继续使用。
- 后代的符号属于已结束的内层 block；`:=` 只命名当前 block 的直接子，
  因此不存在仍活着、指向被销毁后代的 symbol。

值/引用：

- value、frozen 引用、cell/GC handle、指针都不是 owned location，不参与
  递归销毁；指针指向被销毁子树后失效（解引用 UB），frozen/GC handle
  不受影响。
- `\destroy` 钩子不是 `%drop` 的替代：它是销毁过程中被调用的字段；
  直接调用它不等于销毁。
- 循环里显式写 `(%drop({ body })) while cond` 即每轮销毁 body block 的
  result location；while 不引入额外的循环级销毁规则（§2.6）。

`\destroy` 约束：

- `\destroy` 不得 move 出 self 或其字段；
- 不得 `break` 离开，不得挂起，不得创建新协程；
- 允许读取字段和调用子对象的关闭方法；
- 资源必须是 owned location（对象字段、块内临时量）或 cell 协程，
  不能是裸 value。
- block 结束时未命名的临时量按同一规则销毁。

`;` 的语义：

- `;` **只丢弃前一个表达式结果，不做析构**：value 无操作；location/place
  只是不再使用，不会被 `;` 销毁。
- `:=` 产生 unit，所以 `x := allocate ...; rest` 不会在分号处销毁 x。
- 未绑定的 location（临时量）在**所属 block 结束时**统一按创建逆序析构；
  已绑定字段的销毁只由 block 结束/对象销毁/`%drop` 负责。
- 需要提前回收时显式 `%drop(\p)`；`;` 不缩短任何 location 的生命周期。
- 因此 `;` 是纯序列点，不再承担 RAII 的“语句级析构”职责；资源释放的
  默认边界是 block/对象，而不是分号。
- 循环不是 block：`;` 与“循环结束”都不提供每轮回收点。回收点只有用户
  显式 `%drop`（如 `%drop({ body })`）和外层 block 结束；while 不引入
  额外的循环级销毁规则。

`break`：

- 中止当前 block，并把 break 值写入目标 block 的 **self root / result
  location**（同一个 location）；不存在独立的 result 槽；
- 目标 location 原内容（字段对象）先按第六章销毁；若 break 值是其中一个
  字段且被 move，则先摘下该子树，再销毁剩余字段，最后装入，禁止自引用环；
- 中途被终止的 block：先摘下 break 值，再依次销毁它们的 result location/
  字段；目标 block 的 result 保留并成为 block 值；
- target 默认无 shape 约束；若显式有约束，最终内容必须满足；
- break 到非祖先 handle 是运行时错误；过期 handle 是运行时错误。
- 通过 break 提前结束时，`\this` handle 随目标 frame 结束而失效。

## 七、调用与求值原语

内核没有"函数调用"原语。三个独立原语：

```
evaluate proto     !- 当前 env 里 force prototype；不建 location、不换 env -!
allocate <shape>   !- 在当前 block 新建空 location（默认 mutable）；不跑 prototype、不换 env -!
new proto          !- 新协程里 force prototype；env = 协程根 -!
```

- `evaluate` 不新建 location：prototype 体内的 `%bind`/`%field` 直接作用
  于当前 block 的 self root（现场填充 implicit root），结果按 kind 透传。
- `allocate <shape>` 单纯建槽：得到 empty（typed）location，默认 mutable；
  要 immutable 用 `%seal`，初值经 `=` 写入。
- `new` 创建独立协程，env 是协程根 location，GC 管理。
- 旧 `allocate prototype` 写法已退役：它就是 `evaluate proto`（同一操作，
  仅改名 + 语义澄清：不建 location、不换 env）。

"函数调用"只是 stdlib 的 wrapper：

- 参数协议、调用帧、`Proc` 形状、递归糖都由 stdlib 组合；
- wrapper 用 block 建 per-call frame（新 env），在新 env 里 `evaluate`
  函数 prototype body；函数的参数/结果 location 由 wrapper 在 call frame
  与 caller 中分配；
- wrapper 可以选择暴露 result location，或 `$` 读出 value 后作为 value
  表达式；这是库的接口契约，不是语言特例；
- `x := f()` 的合法性由 `:=` dispatcher 决定：location → `%bind`；
  value → auto-box。两者都合法，语义不同。

因此：

- 内核不区分函数/闭包/协程，只认 `evaluate`、`allocate <shape>`、
  `new`/`@`（02）；
- 参数必须以 value bit-copy 或 location move 进入 wrapper 的调用帧；
- wrapper 负责把参数 location 放进自己的所有权域，
  返回时把结果 location 移回调用者；
- 旧方案的共享 `Proc.in` 交接槽不满足本章的可重入性，已退役；
  调用参数改为 per-call frame，不能成为内核语义。
- 递归不依赖 block 名字：prototype body 里的 self 由 stdlib 的
  fix/self 协议在 call frame 时提供；绑定在 block/prototype 完成后发生，
  不暴露半构建的 location。

`block` 的 self root 与 `allocate <shape>` 的返回 location 是同一个概念：
都是当前 block 中分配、可被绑定的 location。`{}` 是对象形式的隐式分配
（特殊形式，创建新 env），`allocate <shape>` 是显式空槽。`evaluate` 不是
分配形式——它在当前 env 里跑 prototype，不建 location。

### 7.1 move 的目标槽

move 不指定目标 block，只指定源。目标槽由接收方预先分配：

| 边界 | 谁预分配目标槽 | 目标 block |
|---|---|---|
| block 字面量 `{}` | 父 block 隐式 allocate self root | 父 block（绑定不是 move） |
| 调用参数 | stdlib wrapper 在 callee call frame 分配参数槽 | callee |
| 调用结果 | wrapper 在 caller 分配结果槽；或 `evaluate` 直接透传 prototype 结果 | caller |
| `break` | 目标 block 的 self root / result location | 目标 block |
| `@` 消息 | 接收协程按消息协议准备消息槽 | 接收协程 |
| new / 闭包捕获 | new/wrapper 为新协程或 env 分配捕获槽 | 新协程 / env 对象 |

规则：

- 源必须位于发起方当前 block 的所有权子树中（含孙及更深）；
- 目标槽由接收方分配并拥有，必须是 mutable，通常处于 empty；覆盖已有
  内容时先销毁；move 到 immutable 目标是类型错误；
- move 本身产出 unit；如果协议需要返回值，返回的是目标 location
  （或 wrapper 用 `$` 读出的 value），不是 move 的结果；
- 没有通用的 `move to <block>` 语法。普通代码通过调用、break、new、
  `@` 等边界协议表达转移；目标由协议决定。局部重排若需要任意目标槽，
  由 stdlib 的显式容器协议提供（可使用指针）。
- 目标槽默认无 shape 约束；若接收方声明了约束，则检查源子树形状。
  目标槽的存储可重新分配/装箱，move 不是按位复制。

## 八、共享

### 8.1 可变共享：cell 协程

- location 全部住在 cell 协程帧内；
- 外部只持有可复制的 GC handle；
- 经 `@` 消息读写；
- 消息中的 value 复制，object 整棵 move；
- 没有指向普通栈 frame 的安全引用。

### 8.2 只读共享：frozen

- frozen 是 **"freeze"操作从 owned 对象产生的 value**：深拷贝整棵子树为
  不可变快照，无 `\destroy`、无指针、无资源、**无所有权**（不属于任何
  block 的所有权树）；freeze 后源对象照常存活/销毁，二者无关。
- frozen 引用是 value，可复制，不拥有对象、不参与销毁；生命周期必须是
  静态或 GC 保证的。
- **与 `%seal` 的区别**：seal 只改 shape.mutability（location 仍 owned、
  仍有身份、销毁仍恰好一次），造不出可共享的快照；freeze 才产出
  无所有者的 value。v1 里 frozen 不接受 mutable 字段路径写、不可 `%drop`。
- v1 只允许编译期产生的 frozen（模块、常量）；运行期 freeze 列为后续特性；
- 模块 = frozen 对象；多协程注入同一只读实例。

### 8.3 裸指针

- 指针是 value，指向 location，但不拥有、不保活；
- 访问不属于当前所有权子树的对象使用指针；帧 handle 的 fat pointer
  （immutable overlay）只用于借用帧内 location 的临时只读访问；
- 对象 move/`%drop` 后指针失效，解引用是 UB；
- 安全子集 = owned location + cell；借用（fat pointer immutable overlay）
  是受控临时只读访问，仍在安全语义内；裸指针/mutable fat pointer 在安全
  子集之外。标准库可以在内部使用指针并以不变量保证正确性；用户代码使用
  裸指针即离开安全子集。
- 指针参与的 GC 根/跨协程有效性尚未定义，属于实现边界。

### 8.4 fail

- fail 必须是可复制 value 或 frozen，不能拥有资源；
- traceback 是冻结的快照，不是活帧 handle。

## 九、comptime

- 与运行时同一套所有权规则。
- 阶段边界只允许 **value、frozen 对象、prototype/code 值** 固化。
- 不允许把 owned mutable location 固化为运行期可变数据槽；
  01 / 03 中相关条款必须回写。
- 不允许固化裸指针、资源、frame handle。
- comptime 结束销毁其 owned 树；跨越边界的只有 frozen/值。
- 这样 comptime 求值可缓存：缓存对象是不可变固化值，
  不依赖共享可变堆。

## 十、检查策略

- 静态能证明的：所有 move/use 在编译期检查，零运行时状态。
- 动态路径：location 携带 empty 状态检查；move/read/`%drop` 时 trap。
- shape 检查只对显式约束进行：有约束静态可证则编译期检查，
  否则运行时检查；无约束不检查形状，也不存在容量检查。
- 存储布局选择（内联/装箱/重分配）不改变语义；静态布局失败只影响优化。
- 这延续 01 的“静态尽力、动态可检查”。
- 不引入借用检查；借用经帧 handle 的 fat pointer（immutable overlay）
  取得，生命周期由调用栈结构保证：被调用帧不能把借用存进字段、捕获、
  返回或发送。非自有对象的借用由裸指针承担，安全性由使用者负责。
- 零开销承诺只适用于静态可证明的片段；
  动态路径的检查成本必须诚实记录。

## 十一、对 01–04 的修订点

- **01**：
  - 删除 mutable location 固化；只允许 value/frozen/prototype/code 跨阶段。
  - comptime 对象树在阶段结束时按本章销毁；共享可变 comptime 堆只存在于
    单次求值内，跨求值缓存必须是不变固化值。
  - 删除 `compile mutable x = ...` 这类旧绑定写法；comptime 自身不引入
    新的绑定语义。
- **02**：
  - 内核求值原语是 `evaluate proto`（当前 env force prototype，不建
    location、不换 env）、`allocate <shape>`（在当前 block 建空 location，
    默认 mutable，不跑 prototype、不换 env）、`new proto`（新协程根 env）。
    旧 `allocate prototype` 已拆掉。
  - 函数调用只是 stdlib wrapper：用 block 建 per-call frame（新 env），
    在新 env 里 `evaluate` 函数 prototype body；参数协议、Proc 形状、
    fix/self 递归糖都由 stdlib 组合。
  - 共享 Proc 交接槽已退役；调用参数改为 per-call frame，内核不假设它存在。
  - 捕获不能把外部 location 绑进闭包 env；只能复制 value 或整棵 move 对象；
    可变共享用 cell。闭包 env 的字段访问用显式 env handle，不用
    `\this` 代替当前帧 handle。
- **03**：
  - kinds 精简为 value/location；pointer 是 value 子类，place 是路径语法，
    view 并入 location（借用经 fat pointer 的 immutable overlay）。
  - mutability 是 shape 的一部分：shaped location 由 shape 指定（默认
    mutable，`%seal` 冻结），unshaped location 跟随内容。
  - `:=` 是 stdlib dispatcher：location → `%bind`；value → 标准 symbol
    match + auto-box；fail → 当 value；借用 → 报错。`x := 1` 合法。
  - pattern 匹配只出现在 `:` 约束形式；失败是 fail 值 + break，不是
    “硬错误”。匹配成功后由 stdlib 提交：`allocate` + `%bind`。
  - `%bind(symbol, location)` 只命名当前 block 的直接子 location。
  - `match` 的 subject 是 value；非自有对象的结构比较/匹配走裸指针，
    属于不安全子集；Bindings 仍只携带 value，不能把对象子树绑定成 value。
  - `=` 是 stdlib assign dispatch（基础类型内建）；bit-copyable 是 shape
    属性（无 trait）；非 bit-copyable 走 move/clone。
  - block 的 self root 就是 result location，分配在父 block；
    字段是 root 的直接子 location；self root 是 unshaped，默认 mutable。
  - shape 是内容结构 + mutability；约束可选；empty 无内容 shape/mutability，
    显式约束才跨 empty 保留。
- **04**：
  - block 的值就是它的 result location（self root），不是末表达式；
    正常完成内容为对象，`break v` 内容为 v；不存在独立的隐式 result。
  - `\this` 是帧 handle，经 fat pointer（immutable overlay）取得临时只读
    访问；handle 可复制/捕获，过期使用是运行时错误；不是 location。
  - `break` 写入目标 block 的 self root/result；中途 block 销毁；
    目标 block 原字段销毁（除非被 move 到 break 值中）；非祖先 handle 报错。
  - `;` 只丢弃表达式结果，不析构；未绑定 location 在所属 block 结束时
    统一回收。`:=` 产生 unit，不销毁绑定字段。
  - `%branch` 透明：分支经 `evaluate` 在当前 env force，不分配 result
    location，结果按 kind 透传；`if` 由 `%branch` 派生。
  - 循环不产生 block：循环体未包 `{}` 的 `allocate`/`:=` 属于外层 block。
    `{}` body 是普通 block；未 `%drop` 时 root/已命名字段随父 block 结束。
    用户写 `(%drop({ body })) while cond` 才每轮销毁；while 是 transparent
    macro/prototype，不引入额外的循环级销毁规则。
  - `%drop` 是唯一显式销毁原语：递归销毁 owned 子树，目标变 empty，
    bindings 不删除，后代随内容消失；`\destroy` 是钩子不是操作。
  - 裸 `break` 没有“只退循环”的目标；循环控制/退出协议由 05 的 stdlib
    提供，内核不新增 label 或循环帧。
  - `%branch` 不分配分支结果 location，因此与“循环不产生 block”不再冲突。
  - `\destroy` 的签名和限制按第六章；fail 必须可复制，traceback 是冻结快照。

## 十二、验收用例（自检清单）

1. `p := { a := allocate integer; \a = 1 }`；`$(\p.a)` 得 1；
   在父 block 用 `:=` 绑定 `p.a` 失败（非直接子）。
2. `x := 1` 经 auto-box 通过（immutable location）；`x := allocate integer`
   得到 mutable uninitialized，读取 trap，`%drop` 对它是 no-op，作为
   move/`=` 目标时可被初始化；`x := $(\p.a)` 走 auto-box（value 路径）。
3. 内层 block 结束、外层保留 p：`p.q.b` 可读；
   内层符号 `q`、`b` 不再可见。
4. 未绑定 block 作为临时量：父 block 结束时统一回收其字段；
   已绑定的 p 不在父 block 结束时销毁；`;` 不改变这个生命周期。
5. 整个对象或后代 location move 后源变 typed empty：再读/再 move trap；
   父对象布局仍有效；目标销毁一次；move 到 immutable 目标报错。
6. `r := { a := allocate integer; \a = 1; break 42 }`：r 绑定 self
   root/result location；42 写入同一个 location；字段 a 被销毁；
   `$(\r)` 得 42。
7. `r := { break (allocate integer) }`：break 值（location）move 进 result；
   result 是同一个 location；没有额外隐藏槽。
8. `evaluate proto` 不建 location、不换 env，结果按 kind 透传；
   `allocate <shape>` 建空 location 可被 `:=` 绑定；返回 value 的 wrapper
   走 `:=` dispatcher auto-box。
9. 闭包不能绑定外部 location；捕获 value 复制、对象整体 move；
   两个闭包共享可变对象必须经 cell。
10. module 为 frozen：多协程只读共享；写或附加 `\destroy` 拒绝。
11. comptime 构造可变 location 并试图固化到运行期：编译错误；
    只读数据、frozen、prototype/code 可固化。
12. 裸指针指向 location；对象 move/`%drop` 后解引用为 UB（文档断言）。
13. 静态可证明的 move 路径无运行时检查；动态路径 use-after-move trap。
14. `p := { result := 7 }`；`$(\p.result)` 为 7；
    `result` 是普通字段，没有隐式 result 概念。
15. 无约束的空 location 接收不同 shape 的 move 子树：成功，源 empty；
    实现可重分配/装箱，不得因容量拒绝；有显式 shape 约束的槽则按 shape
    拒绝或接受。
16. 逃逸 block 对象中的 empty 字段作为 tombstone 保留，销毁时跳过；
    临时 block 对象中的 empty 字段随对象一起释放。
17. 块内经 `\this` handle（fat pointer immutable overlay）读字段；
    借用不能存进字段/捕获/返回；`\this` handle 在块结束后使用是运行时错误。
18. `{ (allocate integer) while cond }`：integer 是外层 block 的子
    location，循环结束后仍可经路径访问，外层 block 结束时销毁。
19. 循环体 `{}` 未 `%drop`：root/已命名字段活到父 block 结束；每轮只回收
    块内未绑定临时量（含 defer guard）。写 `(%drop({ body })) while cond`
    时每轮销毁 body root，已命名字段/临时量/defer 都按轮销毁；while 是
    transparent macro/prototype，不引入额外的循环级销毁规则。非 block 体
    不回收，其 allocation 存活到外层 block 结束。
20. 循环体内不带 `{}` 的 `:=` 不能跨迭代重复；带 `{}` 时每轮新名字；
    `defer` 只有包在 `{}` 循环体里才每轮执行。
21. `p := { a := 1 }`；`%drop(\p)` 后 p 变 empty，symbol p 保留，
    `$\p` trap，重复 `%drop(\p)` 报错；字段 a 与 `\destroy` 钩子按第六章
    处理，后代消失。
22. `%drop(\p.q)`（q 是 live 对象 p 的后代）：q 变 empty 槽留在 p 中
    （tombstone），q 的后代消失；p 仍 live；之后 `%drop(\p)` 跳过 empty q。
23. `%drop` 对 value 是类型错误；对 empty 报错；未绑定临时量的 empty 槽
    可被实现物理回收，绑定 location/对象字段保留 empty 槽。
24. `%seal`：对 mutable location 单向冻结为 immutable；冻结后再 `=`/
    break 替换报错；对已 immutable 是 no-op；非 bit-copyable 对象的
    `=` 报错（需 move/clone）。
25. block self root 是 unshaped 默认 mutable：`x := { ... }` 得到 mutable
    location；`%seal(\x)` 冻结后不能再写。`evaluate SomeType(args)` 在当前
    env 现场填充 implicit root，`%bind` 直接写当前 self root。

## 十三、待办 / 未决

- [x] block 的 result location 与 self root 已统一：同一个 location，
      不存在需要额外消费的内部槽。
- [x] kinds 精简为 value/location；view 并入 location，借用经 fat pointer。
- [x] mutability 并入 shape；默认 mutable，`%seal` 单向冻结。
- [x] `:=` 改为 stdlib dispatcher（location → `%bind`；value → auto-box）。
- [x] `evaluate` / `allocate <shape>` / `new` 三原语分开；旧
      `allocate prototype` 退役。
- [x] frozen 移出 location 状态机：它是 freeze 操作产生的无所有权 value
      （§8.2），与 `%seal`（只改 shape.mutability）划清界限。
- [ ] 非自有对象的结构化匹配：match 的 subject 是 value；对象比较/匹配
      经裸指针属不安全 API；Bindings 不能携带对象子树。需要明确 v1 的
      destructuring 边界。
- [ ] shape 约束的语法、推断边界与检查时机（静态/运行时）。
- [ ] typed empty 在对象 shape 中的表示；部分 move 后父对象的压缩策略。
- [ ] `\this` handle 的完整能力面、过期检查与跨 comptime 边界规则；
      mutable handle（unsafe）的具体边界。
- [ ] 借用（fat pointer immutable overlay）是否推广到普通调用；当前
      非自有访问走裸指针。
- [ ] 析构函数挂起、失败、循环引用与协程 GC 终结的细则。
- [ ] frozen 的运行期构造与生命周期（v1 仅 comptime frozen）。
- [ ] stdlib wrapper 对调用结果的契约：暴露 location 还是 value。
- [ ] while 的循环协议：作为 transparent macro/prototype 的具体展开；
      body 销毁已由用户显式 `%drop` 表达，while 不引入额外的循环级销毁规则。
- [ ] `%drop` 的正式语法（参数是 location 表达式；拼写暂定）、body 值带出
      时的先 move 再 `%drop`。
- [ ] `%seal` 的正式语法与 unshaped 跟随内容时的冻结时机；shape 直接操作
      （unsafe）的拼写。
- [ ] `allocate` / `evaluate` / `move` / `%drop` / `clone` / `%seal` /
      指针取址的最终语法拼写。
