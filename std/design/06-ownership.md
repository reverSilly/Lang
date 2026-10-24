# 所有权设计记录

状态：草案（核心裁决已定，待回写 01–04）
日期：2026-10-24

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、
[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、
[04-control-flow.md](04-control-flow.md)

范围：location 的所有权、`block` 作为对象字面量、`allocate` 与 `:=` 的分工、
move / destroy、跨 block 与跨协程转移、comptime 边界。
本章不解决 05 的宏展开细则与完整的静态类型系统；只固定所有权语义，
以及 shape 作为“内容当前结构 + 可选约束”的基础规则。

## 核心裁决

1. **value 没有所有权。** value 不可变、可复制、无身份、无析构。
2. **所有权绑定在 location 上。** location 是唯一可被拥有、移动、销毁的东西。
3. **所有权是一条父到直接子的边。** block/对象拥有其直接子 location；
   子 location 再拥有它的子 location。所有权图是一棵森林。
4. **block `{}` 是对象字面量。** 它产生自己的 self root location，
   这个 root 分配在**父 block** 中，而不是 block 自己内部。
5. **`allocate` 分配 location。** 它在当前 block 中创建一个直接子 location，
   设定可变性与可选的 shape 约束（缺省无约束），并初始化内容。
6. **`:=` 只绑定 location，不分配。** 它只能把当前 block 的一个直接子
   location 绑定给一个 symbol；不移动、不改变所有权、不隐式决定可变性。
7. **对象不能作为 value 传递。** block root、allocate 结果都是 location；
   跨 block 使用只能整棵 location 子树 move，或通过 view 临时访问。
8. **销毁恰好一次。** 沿所有权树递归；empty 槽跳过。
   可变共享只经 cell 协程；只读共享只经 frozen；裸指针不在安全子集内。
9. **没有借用检查。** view 只在一次同步调用内有效，不能存储、捕获、返回。
10. **shape 属于内容，不属于 location。** location 只可有可选的 shape 约束，
    缺省无约束；break/move 不受固定形状约束。
11. **move 不产生值。** 它只转移整棵 location 子树的拥有权，本身产出 unit；
    目标槽由接收方预先分配。
12. **break 直接改写目标 block 的 self root。** block 正常完成的值是它的对象；
    末表达式不自动成为 block 值；不引入 `\result` 字段或投影语法。
    `\result` 仍属于保留标识符命名空间，普通 `result` 留给用户定义。

## 一、本体

### 1.1 五种东西

- **value**：不可变数据。无身份、无生命周期、无所有权。读取 value 是复制。
- **location**：有身份的存储槽。内容可以是叶子 value、若干子 location，
  或 empty；可附带可选的 shape 约束（缺省无约束）。location 的当前形状
  由内容决定，而不是槽固有。
- **symbol**：名字。只有绑定关系，没有所有权。
- **block/对象**：block 求值时创建的 self root location 及其字段；
  对象就是一棵 location 树，不是一个额外种类的值。
- **place/view**：place 是指向 location 的路径（`\p`、`p.field`），不是值；
  view 是一次调用期间对某棵 location 子树的临时访问权，也不是值。

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

R4. 一个 location 只能在它的直接拥有者是当前 block 时被 move；
源位置变 empty，目标获得整棵子树的拥有权。

R5. value 可复制；location/对象绝不隐式复制。深拷贝只能经显式 `clone`。

R6. empty location 不可读、不可再 move；销毁时跳过。

R7. symbol 绑定不产生所有权。一个 symbol 的消失不会销毁 location；
一个 location 的销毁也不会因为 symbol 的存在而延迟。

R8. 跨 block 不得存在 location 别名。view 是唯一受控的临时别名。

### 1.4 形状与约束

- **形状（shape）是 value/对象的当前结构**，由内容决定，不是 location 的
  固有容量；empty location 没有内容形状。
- location 可以有**可选的 shape 约束**；只有显式类型注解/接口契约才产生。
  缺省无约束：empty 槽可以接收任意形状；有约束时，empty 仍保留约束，
  写入/move 必须满足它。
- 存储布局（内联、装箱、重新分配）属于实现，不属于 shape 语义。
  move 不是按位复制，目标槽不需要预先有足够大的空间。
- block root 的形状是动态的：正常完成是字段对象；`break` 可以整体替换
  root 的内容为任意形状的值。除非显式约束，`break` 不要求值与 block
  原字段形状一致。
- 静态可证明形状时，编译器可以内联固定布局、消去检查；
  动态形状走间接/重分配。二者语义相同，静态失败不得拒绝程序。

## 二、block = 对象字面量

### 2.1 self root 分配在父 block

设 block `B` 在父 block `P` 中被求值。`B` 的 self root `R_B`：

1. 由 `B` 的求值在 **P** 中隐式 `allocate`；
2. 是 `P` 的一个直接子 location，因此 `P` 的 `:=` 可以绑定它；
3. 在 `B` 求值期间作为 `B` 的对象容器；其直接子 location 是 `B` 的字段；
4. `B` 内部的 symbol 只能绑定 `R_B` 的直接子 location；
5. `B` 自己不能把 `R_B` 绑定成内部 symbol；`\this` 只是 `R_B` 的 view；
6. `P` 不能绑定 `R_B` 的字段；只能通过路径访问，或让整个 `R_B` move。

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

### 2.2 符号表、字段与寿命

- block 的符号表就是它当前已命名的直接子 location 表。
- block 结束时，未命名的直接子 location 是临时量，按创建逆序销毁；
  已命名的直接子 location 成为对象字段，随 self root 存活。
- block 的**符号**在 block 结束时消失；**字段**随对象存活。
  这正是“block 是对象字面量”的含义。
- 若 `R_B` 没有被父 block 绑定，它本身就是父 block 的临时 location；
  在父 block 的序列点被销毁，其字段随之销毁。
- 若 `R_B` 被绑定为 `p`，其寿命属于父 block，直到父 block 结束或被 move。

### 2.3 绑定 block

```
p := {
    a := allocate mutable integer = 1
    q := { b := allocate integer = 2 }
}
```

- `R_B` 已经在父 block 中分配；`p :=` **只是命名，不移动、不复制**。
- `B` 内部的 `a`、`q` 由 B 的符号表绑定；B 结束后不可再按名字访问，
  但可通过 `p.a`、`p.q.b` 访问。
- `x := p.q` 是错的：`p.q` 不是当前 block 的直接子 location。
  当前 block 既不能绑定它，也不能单独 move 它。
  如果需要外层直接拥有内层对象，应重新设计对象边界，
  而不是穿透所有权层级。

### 2.4 block 不能绑定自己

`R_B` 属于 `P`，不属于 `B`。因此：

- `B` 内不能 `x := \this`；
- `\this` 是 view，不能 move、不能 drop、不能存入字段；
- 如果 `B` 需要返回自身对象，由 `P` 中的绑定或调用协议处理，
  而不是在 `B` 内部再造一个 symbol。

### 2.5 block 的 result 与动态 shape

- block root 的当前形状 = 字段 + 当前 `\result`。
- `\result` 是 root 的一个直接子 location；break 值复制或 move 进去后，
  它的形状就是该值的形状；没有显式约束时不受 block 原字段形状限制。
- 因此 `{ a := allocate integer = 1; break 42 }` 是合法的：
  result 成为 42，block 对象的当前形状随之改变。
- 若 block 或 result 显式声明了 shape 约束，则 break 值按该约束检查；
  这是可选契约，不是 block 字面量的默认行为。
- block 被父 block 绑定后，其当前形状（包括 result）随对象存活；
  字段/result 的后续写权限由各自的 location 可变性决定。

## 三、allocate 与 :=

| | `allocate` | `:=` |
|---|---|---|
| 创建 location | 是 | 否 |
| 建立所有权边 | 当前 block → 新 location | 否 |
| 修改可变性 | 是，分配时确定 | 否 |
| 要求 RHS 是 location | 不适用 | 是 |
| 要求 RHS 是当前 block 直接子 | 新 location 天然满足 | 是 |
| 产生值 | 返回 location（place） | 产生 unit，不产生可丢弃的值 |

### 3.1 `x := 1` 为什么非法

`1` 是 value：

- 没有 location，因此没有可变性；
- 没有所有者，因此没有销毁语义；
- `:=` 是 location 的命名操作，不是 value 的存储操作。

若需要名字，必须显式选择 location 与可变性：

```
!- 不可变命名 -!
x := allocate integer = 1

!- 可变命名 -!
y := allocate mutable integer = 1

!- 不需要名字 -!
f(1)
```

- 所有权在**当前 block**，由 `allocate` 建立；`x` 只是名字。
- 可变性只由 location 决定，`:=` 不授予可变性。
- 不可变 location 的初值必须在 `allocate` 时给出；之后只读。
- 可变 location 可以后续写入。
- 未初始化的 location 处于 empty，读取/移动是错误。

### 3.2 绑定的同 block 规则

`:=` 只接受满足以下条件的 location：

1. 它是当前 block 的直接子 location；
2. 它当前未被绑定到另一个 symbol（v1 不允许同 block 多名一址；
   别名可后续作为独立特性引入）；
3. 它可以是 empty（例如刚 `allocate` 尚未初始化），但绑定本身不填值。

因此：

```
x := allocate integer = 1          !- 合法 -!
p := { ... }                       !- 合法：block root 分配在父 block -!
x := \p.a                          !- 非法：p.a 不是当前 block 的直接子 -!
x := 1                             !- 非法：value 不是 location -!
x := $(...)                        !- 非法：value 不是 location -!
```

### 3.3 move 与源槽

- move 的单位是整棵 location 子树；被移动的是 location，不是 value。
- move 的源必须是**当前 block 的直接子 location**。当前 block 尚未结束，
  所以它可以 move 自己的直接子；孙节点及更深位置不能由它直接 move，
  只能随其所属对象整体移动。
- 当前 block 的直接子包括：`allocate` 的结果、子 block 的 self root、
  已绑定的字段等。
- move 后源槽变 empty：槽还在、symbol 还在，但读、再次 move、drop 都是错误。
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

本章建议并采用：

> **存储可变性按 location 独立；只读是 view 的属性，沿访问路径传播。**

- `allocate` 决定 location 的可变性；缺省不可变，`mutable` 显式声明。
- `:=` 不改变可变性。
- block 的字段可以各自不同可变性，因为每个字段都是独立 `allocate` 的 location。
- 只读 view 使整条访问路径只读；这是“深度只读”的来源。
- block root 自身的可变性只影响“能否整体替换 root 这个 location”，
  不影响其字段各自的写权限。
- 可变性与 shape 约束正交：写入/move 先受所有权与可变性约束，
  再检查可选的 shape 约束；没有显式约束就不做形状检查。

这与 [03-symbols-locations-matching.md:34](03-symbols-locations-matching.md#L34) 的
“可变性沿路径传递”冲突。必须回写 03：**存储可变性不继承；只读 view 才继承。**
否则一个 block 无法同时拥有 mutable 与 immutable 字段。

## 五、状态机

一个 location 的所有权状态至少包含：

- `live`：有内容，归某个 block/对象所有；当前形状来自内容；
- `empty`：已被 move 或销毁；没有内容形状，但可保留显式 shape 约束；
- `frozen`：不可变、无析构、可共享的只读对象（见第八节）。

| 操作 | 前置 | 结果 |
|---|---|---|
| create | — | live，归当前 block 所有；可选 shape 约束 |
| read `$p` | p 是 value 叶子 | 返回副本；object location 不可整体读 |
| move `\p` | p 是当前 block 直接子、live；目标槽由接收方预分配 | p 变 empty；目标获得子树；无约束不查形状，有显式约束才查 |
| assign `\p = v` | p 是 mutable location | 先销毁旧内容，再写入 value；有 shape 约束则检查 |
| drop `\p` | p 是当前 block 直接子、live | 递归销毁，p 变 empty |
| clone `\p` | 图内只有 value/可克隆资源 | 新的独立 owned 子树 |
| view `\p` | 同步调用期间 | 临时访问，禁止存/捕获/返回/move |
| freeze `\p` | 仅编译期或受限构造 | 转 frozen，失去所有权与析构 |

错误分类：

- 静态可判定：绑定 value、绑定非直接子、复制 owned 对象、移动 view/frozen、
  显式 shape 约束不兼容；
- 动态需检查：读取 empty、重复 move、使用已销毁对象；
- 无显式 shape 约束时不做形状检查；容量/布局不是语义错误；
- 指针失效不检查，解引用为 UB。

## 六、销毁

销毁一个 owned 对象时：

1. 标记为 destroying，拒绝重入；
2. 若存在 `\destroy` 字段，以 self 的 view 调用它；
3. 按创建逆序销毁仍 live 的直接子 location；empty 跳过；
4. 释放 root，状态变 empty。

约束：

- `\destroy` 不得 move 出 self 或其字段；
- 不得 `break` 离开，不得挂起，不得创建新协程；
- 允许读取字段和调用子对象的关闭方法；
- 资源必须是对象字段或 cell 协程，不能是裸 value；
- block 结束时未命名的临时量按同一规则销毁。

`;` 的销毁范围：

- `;` 丢弃前一个**表达式结果**：value 无操作；location 立即销毁；
- `:=` 产生 unit，所以 `x := allocate ...; rest` 不会在分号处销毁 x；
- 绑定字段的销毁只由 block 结束或对象销毁负责。

`break`：

- 中止当前 block，目标 block 的 `\result` location 接收 break 值；
- `\result` 默认无 shape 约束，break 值的形状可以不同于 block 原字段形状；
  block 对象的当前形状随之改变；
- 若 block/result 显式声明的 shape 约束存在，则 break 值按约束检查；
- 途经的 block 对象按本节规则销毁，目标对象继续完成；
- break 到非祖先 handle 是运行时错误。

## 七、调用与 allocate prototype

内核没有“函数调用”原语。原语是：

```
allocate prototypeExpr
```

它在**调用者当前 block**中分配一个 result location，把 prototype 在其中求值，
并返回这个 location。这个 result location 是当前 block 的直接子 location，
因此可以被 `:=` 绑定。result location 默认没有 shape 约束；
只有 wrapper/接口显式声明时才附加约束。

“函数调用”只是 stdlib 的 wrapper：

- 参数协议、调用帧、`Proc` 形状、递归糖都由 stdlib 组合；
- wrapper 内部最终使用 `allocate prototype` 得到 result location；
- wrapper 可以选择返回该 location，或 `$` 读出 value 后返回 value；
- 如果 wrapper 返回 value，`x := f()` 仍然是非法绑定；
- 如果 wrapper 返回 location，`x := f()` 合法；
- 这是库的接口契约，不是语言特例。

因此：

- 内核不区分函数/闭包/协程，只认 `allocate`（以及 `new`/`@`，见 02）；
- 参数必须以 value 复制或 location move 进入 wrapper 的调用帧；
- wrapper 负责把参数 location 放进自己的所有权域，
  返回时把结果 location 移回调用者；
- 02 的共享 `Proc.in` 交接槽只是某一种 stdlib 协议；
  若要满足本章的可重入性，应改为 per-call frame，而不是内核假设。

`block` 的 self root 与 `allocate` 的 result location 在语义上是同一类东西：
都是当前 block 中分配、可被绑定的 location。`{}` 是对象形式的隐式分配。

### 7.1 move 的目标槽

move 不指定目标 block，只指定源。目标槽由接收方预先分配：

| 边界 | 谁预分配目标槽 | 目标 block |
|---|---|---|
| block 字面量 `{}` | 父 block 隐式 allocate self root | 父 block（绑定不是 move） |
| 调用参数 | stdlib wrapper 在 callee call frame 分配参数槽 | callee |
| 调用结果 | `allocate prototype` 在 caller 分配 result location | caller |
| `break` | 目标 block 的 `\result`，在目标 block 创建时已存在 | 目标 block |
| `@` 消息 | 接收协程按消息协议准备消息槽 | 接收协程 |
| new / 闭包捕获 | new/wrapper 为新协程或 env 分配捕获槽 | 新协程 / env 对象 |

规则：

- 源必须是发起方当前 block 的直接子 location；
- 目标槽由接收方分配并拥有，通常处于 empty；覆盖已有内容时先销毁；
- move 本身产出 unit；如果协议需要返回值，返回的是目标 location
  （或 wrapper 用 `$` 读出的 value），不是 move 的结果；
- 没有通用的 `move to <block>` 语法。普通代码通过调用、break、new、
  `@` 等边界协议表达转移；目标由协议决定。
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

- frozen 对象不可变、无 `\destroy`、无指针、无资源；
- frozen 引用是 value，可复制，不拥有对象、不参与销毁；
- 生命周期必须是静态或 GC 保证的；
- v1 只允许编译期产生的 frozen（模块、常量）；
  运行期 freeze 列为后续特性；
- 模块 = frozen 对象；多协程注入同一只读实例。

### 8.3 裸指针

- 指针是 value，指向 location，但不拥有、不保活；
- 对象 move/drop 后指针失效，解引用是 UB；
- 安全子集 = owned location + view + cell；指针在安全子集之外。

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
- 动态路径：location 携带 empty 状态检查；move/read/drop 时 trap。
- shape 检查只对显式约束进行：有约束静态可证则编译期检查，
  否则运行时检查；无约束不检查形状，也不存在容量检查。
- 存储布局选择（内联/装箱/重分配）不改变语义；静态布局失败只影响优化。
- 这延续 01 的“静态尽力、动态可检查”。
- 不引入借用检查；view 的生命周期由调用栈结构保证：
  被调用帧不能把 view 存进字段、捕获、返回或发送。
- 零开销承诺只适用于静态可证明的片段；
  动态路径的检查成本必须诚实记录。

## 十一、对 01–04 的修订点

- **01**：
  - 删除 mutable location 固化；只允许 value/frozen 跨阶段。
  - comptime 对象树在阶段结束时按本章销毁。
- **02**：
  - 函数调用只是 stdlib wrapper；内核原语是 `allocate prototype`，
    返回 caller block 中的 result location。
  - Proc 交接槽属于 wrapper 协议，不应作为内核语义；
    推荐 per-call frame。
  - 捕获不能把外部 location 绑进闭包 env；只能复制 value 或
    整棵 move 对象；可变共享用 cell。
- **03**：
  - `%bind(symbol, location)`：只命名当前 block 的直接子 location；
    旧 `%bind(symbol, value)` 作废。
  - 模式匹配可以比较 value，但 symbol 绑定不再隐式分配 location。
    需要绑定的模式必须先有目标 location；`x := 1` 因此非法。
    这会改写 03 中 `a+1 : 3`、`{x, y} : point` 一类例子。
  - `:=` 只绑定 location；不产生值；匹配失败语义只适用于可反驳的
    `:` 约束形式，不适用于不可反驳的 location 绑定。
  - location 不再有必有的形状约束；shape 是内容的当前结构，
    约束可选（见本章 §1.4）。empty 无内容形状；显式约束才跨 empty 保留并检查。
  - 可变性改为 per-location；只读 view 才沿路径传播。
  - block 字面量的 self root 分配在父 block；字段是 root 的直接子 location。
- **04**：
  - block 产生 location，不产生 value；
    04 的“block 值 = 末表达式/break 值”必须改写。
    建议给 block root 增加保留的 `\result` location：
    末表达式/break 值 move 或复制到该 location，
    wrapper/macro 再决定是否投影为 value。
  - `\result` 默认无 shape 约束，break 值可以改变 block 的当前形状；
    只有显式声明的约束才检查。
  - `;` 只销毁丢弃的表达式结果；`:=` 产生 unit，不销毁绑定字段。
  - `break` 到非祖先 handle 报错，不静默结束协程。
  - `\destroy` 的签名和限制按第六章；fail 必须可复制。

## 十二、验收用例（自检清单）

1. `p := { a := allocate mutable integer = 1 }`；
   `$(\p.a)` 得 1；在父 block 绑定 `p.a` 或 move 它均失败。
2. `x := 1` 拒绝；`x := allocate integer = 1` 通过；
   `x := $(\p.a)` 拒绝；`x := allocate integer = $(\p.a)` 通过。
3. 内层 block 结束、外层保留 p：`p.q.b` 可读；
   内层符号 `q`、`b` 不再可见。
4. 未绑定 block 作为临时量：父 block 的 `;` 处销毁其字段；
   已绑定的 p 不在 `;` 处销毁。
5. 整个对象 move 后源变 empty：再读/再 move trap；目标销毁一次。
6. break 穿过中间 block：中间对象及其字段逆序销毁；
   目标对象继续，值进入 `\result`。
7. `allocate prototype` 在 caller block 产生 result location；
   返回 location 的 wrapper 可被 `:=` 绑定；
   返回 value 的 wrapper 不能。
8. 闭包不能绑定外部 location；捕获 value 复制、对象整体 move；
   两个闭包共享可变对象必须经 cell。
9. module 为 frozen：多协程只读共享；写或附加 `\destroy` 拒绝。
10. comptime 构造可变 location 并试图固化到运行期：编译错误；
    只读数据或 frozen 对象可固化。
11. 裸指针指向 location；对象 move/drop 后解引用为 UB（文档断言）。
12. 静态可证明的 move 路径无运行时检查；动态路径 use-after-move trap。
13. `{ a := allocate integer = 1; break 42 }` 合法：result 呈 integer 形状，
    block 的当前形状与字段形状不同；若显式声明返回约束则检查。
14. 无约束的空 location 接收不同形状的 move 子树：成功，源 empty；
    实现可重分配/装箱，不得因容量拒绝；有显式约束的槽则按形状拒绝或接受。
15. 逃逸 block 对象中的 empty 字段作为 tombstone 保留，销毁时跳过；
    临时 block 对象中的 empty 字段随对象一起释放。

## 十三、待办 / 未决

- [ ] block 的 `\result` 字段与 04 的表达式值投影语法（05/06）。
- [ ] 03 的模式绑定：是否允许显式“绑定即分配”的第二机制；
      当前建议是不要，保持 `:=` 纯命名。
- [ ] 可变性从 03 的路径继承改为 per-location 的正式修订文本。
- [ ] 可选 shape 约束的语法、推断边界与检查时机（静态/运行时）。
- [ ] empty 槽在对象 shape 中如何表示（保留 tombstone 还是允许压缩）；
      部分 move / 整体 move 的边界。
- [ ] block root 的整体替换（写 root location）是否需要；
      与字段可变性分离。
- [ ] view 参数是否需要从方法接收者推广到普通调用。
- [ ] 析构函数挂起、失败、循环引用与协程 GC 终结的细则。
- [ ] frozen 的运行期构造与生命周期（v1 仅 comptime frozen）。
- [ ] stdlib wrapper 对 call result 的契约：返回 location 还是 value；
      建议分别提供两种入口，不混淆。
- [ ] `allocate` / `move` / `drop` / `clone` / `freeze` 的最终语法拼写。
