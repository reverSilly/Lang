# 所有权设计记录

状态：草案（核心裁决已定，01–04 已回写；05 待写）
日期：2026-10-24，修订 2026-10-25

> 2026-10-25 裁决：block 的 result location 与 self root 是同一个 location；
> `\this` 是帧 handle（可取得临时 view）；`allocate` 统一为“分配 location，
> shape 留空 / prototype 填充”；move 可深入当前 block 的所有权子树；
> 绑定在 RHS 完成后发生；非自有对象访问走裸指针；`;` 只丢弃、不析构，
> 未绑定 location 在所属 block 结束时统一回收。旧版本中的“独立隐式
> result 槽”“view 代替 handle”“只能 move 直接子”“分号处析构”均已废弃。

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、
[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、
[04-control-flow.md](04-control-flow.md)

范围：location 的所有权、`block` 作为对象字面量、`allocate` 与 `:=` 的分工、
move / destroy、跨 block 与跨协程转移、comptime 边界和指针的定位。
本章不解决 05 的宏展开细则与完整的静态类型系统；只固定所有权语义，
以及 shape 作为“内容当前结构 + 可选约束”的基础规则。

## 核心裁决

1. **value 没有所有权。** value 不可变、可复制、无身份、无析构。
2. **所有权绑定在 location 上。** location 是唯一可被拥有、移动、销毁的东西。
3. **所有权是一条父到直接子的边。** block/对象拥有其直接子 location；
   子 location 再拥有它的子 location。所有权图是一棵森林。
4. **block `{}` 是对象字面量，其 self root 就是它的 result location。**
   result 分配在**父 block** 中，是父 block 的直接子 location。block 求值期间
   它是对象容器（self root）；正常完成时内容为字段对象，`break` 时内容为
   break 值。二者是同一个 location，不存在第二个隐式 result 槽。
5. **`allocate` 只做一件事：在当前 block 分配一个直接子 location。**
   参数是 shape 时得到有约束的 empty location；参数是 prototype 时在该
   location 中求值 prototype，并在完成/break 后返回同一个 location。
   两种参数本质相同，区别只在内容由谁填充。
6. **`:=` 只命名 location，不分配、不移动。** 它只接受当前 block 的直接子
   location，且在 RHS 完全求值之后发生；不暴露构建了一半的 location。
   value（symbol、pointer、frame handle、fail 等）若要命名，必须显式
   `allocate ^value` 放进 location。**这是有意的强制显式分配，不是待优化点。**
7. **对象不能作为 value 传递。** block root、allocate 结果都是 location；
   跨 block 使用只能 move location 子树（整棵或后代），或经指针/受限 view
   临时访问。
8. **销毁恰好一次。** 沿所有权树递归；empty 槽跳过。
   可变共享只经 cell 协程；只读共享只经 frozen；裸指针不在安全子集内。
9. **没有借用检查。** `\this` 是帧 handle，可从中取得临时 view；
   view 只在一次同步调用内有效，不能存储、捕获、返回。
   非自有对象的访问使用裸指针，属不安全特性。
10. **shape 属于内容，不属于 location。** location 只可有可选的 shape 约束，
    缺省无约束；break/move 在无显式约束时不检查形状。
11. **move 不产生值。** 它只转移 location 子树的拥有权，本身产出 unit；
    目标槽由接收方预先分配。源可以是当前 block 所有权子树中的任意位置，
    不限于直接子；源变 empty，并保留其 shape/尺寸约束（typed empty）。
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
    回收析构；显式提前回收用 `drop`。`;` 不缩短生命周期。

## 一、本体

### 1.1 本体

- **value**：不可变数据。无身份、无生命周期、无所有权。读取 value 是复制。
- **location**：有身份的存储槽。内容可以是叶子 value、若干子 location，
  或 empty；可附带可选的 shape 约束（缺省无约束）。location 的当前形状
  由内容决定，而不是槽固有。
- **symbol**：名字。只有绑定关系，没有所有权。
- **block/对象**：block 求值时在父 block 分配的 result location（同时是
  self root）及其字段；对象就是一棵 location 树，不是独立的值。
- **place/view**：place 是指向 location 的路径（`\p`、`p.field`），不是值；
  view 是从 `\this` 等帧 handle 取得的、一次同步求值内有效的临时访问权，
  也不是值。
- **pointer**：指向 location 的可复制 value，用于访问非自有对象；
  不拥有、不保活、不检查有效性，属于不安全特性。

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

R6. empty location 不可读、不可再 move；销毁时跳过。empty 可以作为 move
的目标（初始化）；有显式 shape 约束时，目标内容必须满足约束。

R7. symbol 绑定不产生所有权。一个 symbol 的消失不会销毁 location；
一个 location 的销毁也不会因为 symbol 的存在而延迟。

R8. 跨 block 不得存在 location 别名。view 是唯一的受控临时别名，来自帧
handle；裸指针不保证别名安全，属不安全子集。

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
2. 是 `P` 的一个直接子 location，因此 `P` 的 `:=` 可以在 **B 完成后**绑定它；
3. 在 `B` 求值期间作为 `B` 的对象容器，同时就是 `B` 的 result location；
   其直接子 location 是 `B` 的字段；
4. `B` 内部的 symbol 只能绑定 `R_B` 的直接子 location；
5. `B` 内部的 `\this` 是 `R_B` 的帧 handle，可取得临时 view 访问字段；
   `R_B` 本身不是 `B` 的直接子，`B` 不能用 `:=` 命名它；
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
- 若 `R_B` 被绑定为 `p`，其寿命属于父 block，直到父 block 结束或被 move。

### 2.3 绑定 block

```
p := {
    a := allocate ^1
    q := { b := allocate ^2 }
}
```

- `R_B` 在父 block 中分配；`p := ...` 在 B 正常完成/break 完成后**命名**
  同一个 location，不移动、不复制。
- `B` 内部的 `a`、`q` 由 B 的符号表绑定；B 结束后不可再按名字访问，
  但可通过 `p.a`、`p.q.b` 的路径读取，或经指针访问。
- `p.q` 是 `p` 的子 location，不是当前 block 的直接子：当前 block 不能用
  `:=` 命名它；但在拥有 `p` 整棵子树的前提下，可以 move 它的子树（R4），
  或经指针/受限 view 访问。
- 递归的 block 不通过名字自引用；块内字段用 `\this`，递归函数由 stdlib
  的 fix/self 协议构造（见 §2.4 与 §7）。

### 2.4 `\this` 是 handle，不是 location

`\this` 是当前 block 的**帧 handle**（一个普通 value），指向 `R_B`：

- 可以从 handle 取得临时 view，用来读/写当前 block 的字段；
  view 不能存储、捕获、返回或 move。
- `\this` 本身可以复制、存储、捕获；但 handle 的有效期绑定 `R_B` 的帧。
  帧结束后再经该 handle 取 view 或 break 是运行时错误；静态可证明时
  编译期拒绝。帧 handle 不允许固化到 comptime 边界。
- `B` 内不能 `x := \this`：`:=` 只命名 location，而 handle 是 value；
  这不是对 handle 存储的额外禁止。
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
- `result` 是普通用户字段名，与 `R_B` 没有命名关系；`R_B` 没有源码标识符。

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
!- 不可变命名：用 prototype 在分配时产生初值 -!
x := allocate ^1

!- 可变命名：先分配 empty，再写入 -!
y := allocate mutable integer
\y = 1

!- 不需要名字 -!
f(1)
```

- 所有权在**当前 block**，由 `allocate` 建立；`x` 只是名字。
- 可变性只由 location 决定；`:=` 不授予可变性。
- 不可变 location 的初值由 allocate 的 prototype 产生，或经 move 初始化；
  初始化完成后只读。
- 可变 location 可以后续写入。
- 未初始化的 location 处于 empty；读取、再次 move 是错误，但 empty 可作为
  move 的目标（初始化）。
- `allocate integer`（shape 参数）得到有约束的 empty；`allocate ^1`
  （prototype 参数）得到由 prototype 填充的 location；两者是同一个原语的
  两种参数形态。
- `allocate integer = 1` 是旧“默认 mutable + 分配时写入”写法的遗留，已废弃；
  初值要么由 prototype 产生，要么在 mutable 分配后显式写入。
- value 类的东西（symbol、pointer、frame handle、fail 等）若要命名，也必须
  先 `allocate ^value` 放进 location；`:=` 不直接命名 value。这是显式
  location 模型的代价，也是 `x := 1` 非法的完整推论。

### 3.2 绑定的同 block 规则

`:=` 只接受满足以下条件的 location：

1. 它是当前 block 的直接子 location；
2. 它当前未被绑定到另一个 symbol（v1 不允许同 block 多名一址；
   别名可后续作为独立特性引入）；
3. 它可以是 empty（例如刚 `allocate` 尚未初始化），但绑定本身不填值。

因此：

```
x := allocate ^1                   !- 合法：prototype 初始化 -!
p := { ... }                       !- 合法：block result 分配在父 block -!
x := \p.a                          !- 非法：p.a 不是当前 block 的直接子 -!
x := 1                             !- 非法：value 不是 location -!
x := $(...)                        !- 非法：value 不是 location -!
```

### 3.3 move 与源槽

- move 的单位是整棵 location 子树；被移动的是 location，不是 value。
- move 的源可以是**当前 block 所有权子树中的任意位置**：直接子、孙及更深
  都可以。当前 block 直接拥有 root，因而拥有其整棵子树；不能 move 一棵
  不属于自己的子树。
- 当前 block 的直接子包括：`allocate` 的结果、子 block 的 result location、
  已绑定的字段等；孙位置通过 root 的路径访问。
- move 后源槽变 empty：槽还在、symbol 还在，但读、再次 move、drop 都是错误。
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
| move `\p` | p 在当前 block 所有权子树内、live；目标槽由接收方预分配 | p 变 typed empty；目标获得子树；无约束不查形状，有显式约束才查 |
| assign `\p = v` | p 是 mutable location | 先销毁旧内容，再写入 value；有 shape 约束则检查 |
| drop `\p` | p 在当前 block 所有权子树内、live | 递归销毁，p 变 empty |
| clone `\p` | 图内只有 value/可克隆资源 | 新的独立 owned 子树 |
| view | 经 `\this` 等帧 handle，在同步调用期间 | 临时访问，禁止存/捕获/返回/move |
| freeze `\p` | 仅编译期或受限构造 | 转 frozen，失去所有权与析构 |
| pointer `&p` | p 的 lifetime 由使用者保证 | 可复制 value；非自有访问，不检查有效性 |

错误分类：

- 静态可判定：绑定 value、绑定非直接子、复制 owned 对象、移动 view/frozen、
  显式 shape 约束不兼容；
- 动态需检查：读取 empty、重复 move、使用已销毁对象、使用过期帧 handle；
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
- 资源必须是 owned location（对象字段、块内临时量）或 cell 协程，
  不能是裸 value。
- block 结束时未命名的临时量按同一规则销毁。

`;` 的语义：

- `;` **只丢弃前一个表达式结果，不做析构**：value 无操作；location/place
  只是不再使用，不会被 `;` 销毁。
- `:=` 产生 unit，所以 `x := allocate ...; rest` 不会在分号处销毁 x。
- 未绑定的 location（临时量）在**所属 block 结束时**统一按创建逆序析构；
  已绑定字段的销毁只由 block 结束/对象销毁/`drop` 负责。
- 需要提前回收时显式 `drop \p`；`;` 不缩短任何 location 的生命周期。
- 因此 `;` 是纯序列点，不再承担 RAII 的“语句级析构”职责；资源释放的
  默认边界是 block/对象，而不是分号。

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

## 七、调用与 allocate

内核没有“函数调用”原语。原语是：

```
allocate shapeOrPrototype
```

它在**当前 block**中分配一个直接子 location，并返回该 location：

- 参数是 shape：location 处于 empty，带可选 shape 约束；
- 参数是 prototype：在该 location 中求值 prototype；正常完成时内容为
  prototype 构造的对象，break 时内容为 break 值。返回值仍是同一个
  location。block 的 self root 就是这个 location。
- 若 prototype 的体本身是一个 `{}` block，该 block 的 self root 就是
  allocate 已经分配的这个 location；**不再分配第二个 root**。prototype
  体不是 block 时，其结果直接作为 location 的内容/求值程序。

“函数调用”只是 stdlib 的 wrapper：

- 参数协议、调用帧、`Proc` 形状、递归糖都由 stdlib 组合；
- wrapper 内部最终使用 `allocate prototype` 得到调用结果 location；
- wrapper 可以选择暴露该 location，或 `$` 读出 value 后作为 value 表达式；
- 如果 wrapper 暴露 value，`x := f()` 仍然是非法绑定；
- 如果 wrapper 暴露 location，`x := f()` 合法；
- 这是库的接口契约，不是语言特例。

因此：

- 内核不区分函数/闭包/协程，只认 `allocate`（以及 `new`/`@`，见 02）；
- 参数必须以 value 复制或 location move 进入 wrapper 的调用帧；
- wrapper 负责把参数 location 放进自己的所有权域，
  返回时把结果 location 移回调用者；
- 旧方案的共享 `Proc.in` 交接槽不满足本章的可重入性，已退役；
  调用参数改为 per-call frame，不能成为内核语义。
- 递归不依赖 block 名字：prototype body 里的 self 由 stdlib 的
  fix/self 协议在 allocate 时提供；绑定在 block/prototype 完成后发生，
  不暴露半构建的 location。

`block` 的 self root 与 `allocate` 的返回 location 是同一个概念：
都是当前 block 中分配、可被绑定的 location。`{}` 是对象形式的隐式分配，
`allocate prototype` 是显式的 prototype 形式。

### 7.1 move 的目标槽

move 不指定目标 block，只指定源。目标槽由接收方预先分配：

| 边界 | 谁预分配目标槽 | 目标 block |
|---|---|---|
| block 字面量 `{}` | 父 block 隐式 allocate result location | 父 block（绑定不是 move） |
| 调用参数 | stdlib wrapper 在 callee call frame 分配参数槽 | callee |
| 调用结果 | `allocate prototype` 在当前 block 分配 result location | caller |
| `break` | 目标 block 的 self root / result location | 目标 block |
| `@` 消息 | 接收协程按消息协议准备消息槽 | 接收协程 |
| new / 闭包捕获 | new/wrapper 为新协程或 env 分配捕获槽 | 新协程 / env 对象 |

规则：

- 源必须位于发起方当前 block 的所有权子树中（含孙及更深）；
- 目标槽由接收方分配并拥有，通常处于 empty；覆盖已有内容时先销毁；
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

- frozen 对象不可变、无 `\destroy`、无指针、无资源；
- frozen 引用是 value，可复制，不拥有对象、不参与销毁；
- 生命周期必须是静态或 GC 保证的；
- v1 只允许编译期产生的 frozen（模块、常量）；
  运行期 freeze 列为后续特性；
- 模块 = frozen 对象；多协程注入同一只读实例。

### 8.3 裸指针

- 指针是 value，指向 location，但不拥有、不保活；
- 访问不属于当前所有权子树的对象使用指针；view 只从帧 handle 取得，
  用于自有对象的临时访问；
- 对象 move/drop 后指针失效，解引用是 UB；
- 安全子集 = owned location + view + cell；指针在安全子集之外。
  标准库可以在内部使用指针并以不变量保证正确性；用户代码使用指针
  即离开安全子集。
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
- 动态路径：location 携带 empty 状态检查；move/read/drop 时 trap。
- shape 检查只对显式约束进行：有约束静态可证则编译期检查，
  否则运行时检查；无约束不检查形状，也不存在容量检查。
- 存储布局选择（内联/装箱/重分配）不改变语义；静态布局失败只影响优化。
- 这延续 01 的“静态尽力、动态可检查”。
- 不引入借用检查；view 只从帧 handle 取得，生命周期由调用栈结构保证：
  被调用帧不能把 view 存进字段、捕获、返回或发送。
  非自有对象的借用由裸指针承担，安全性由使用者负责。
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
  - 内核原语是 `allocate`：分配当前 block 的直接子 location；prototype
    在该 location 中求值，返回同一个 location（即 self root/result）。
  - 函数调用只是 stdlib wrapper；参数协议、per-call frame、Proc 形状、
    fix/self 递归糖都由 stdlib 组合。
  - 共享 Proc 交接槽已退役；调用参数改为 per-call frame，内核不假设它存在。
  - 捕获不能把外部 location 绑进闭包 env；只能复制 value 或整棵 move 对象；
    可变共享用 cell。闭包 env 的字段访问用显式 env handle/view，不用
    `\this` 代替当前帧 handle。
- **03**：
  - `:=` 只命名 location；不接受 pattern，不产生可丢弃值；绑定在 RHS
    完成后发生。
  - pattern 匹配只出现在 `:` 约束形式；失败是 fail 值 + break，不是
    “硬错误”。匹配成功后由 stdlib 提交：`allocate` + `%bind`。
  - `%bind(symbol, location)` 只命名当前 block 的直接子 location。
  - `match` 的 subject 是 value；非自有对象的结构比较/匹配走裸指针，
    属于不安全子集；Bindings 仍只携带 value，不能把对象子树绑定成 value。
  - `x := 1` 非法；`allocate integer = 1` 是废弃写法，改为
    `allocate ^1`（prototype 初始化）或 mutable 分配后写入。
  - block 的 self root 就是 result location，分配在父 block；
    字段是 root 的直接子 location。
  - 可变性 per-location；只读 view 才沿路径传播。
  - shape 是内容结构，约束可选；empty 无内容形状，显式约束才跨 empty 保留。
- **04**：
  - block 的值就是它的 result location（self root），不是末表达式；
    正常完成内容为对象，`break v` 内容为 v；不存在独立的隐式 result。
  - `\this` 是帧 handle，可取得临时 view；handle 可复制/捕获，过期使用
    是运行时错误；不是 location，不能用 `:=` 命名。
  - `break` 写入目标 block 的 self root/result；中途 block 销毁；
    目标 block 原字段销毁（除非被 move 到 break 值中）；非祖先 handle 报错。
  - `;` 只丢弃表达式结果，不析构；未绑定 location 在所属 block 结束时
    统一回收。`:=` 产生 unit，不销毁绑定字段。
  - `%branch` 的值是被选分支 prototype 的值（即其 result location）；
    `if` 由 `%branch` 派生。
  - `\destroy` 的签名和限制按第六章；fail 必须可复制，traceback 是冻结快照。

## 十二、验收用例（自检清单）

1. `p := { a := allocate ^1 }`；`$(\p.a)` 得 1；
   在父 block 用 `:=` 绑定 `p.a` 失败（非直接子）。
2. `x := 1` 拒绝；`x := allocate ^1` 通过；
   `x := $(\p.a)` 拒绝；`x := allocate integer` 得到 empty，读取 trap，
   作为 move 目标时可被初始化。
3. 内层 block 结束、外层保留 p：`p.q.b` 可读；
   内层符号 `q`、`b` 不再可见。
4. 未绑定 block 作为临时量：父 block 结束时统一回收其字段；
   已绑定的 p 不在父 block 结束时销毁；`;` 不改变这个生命周期。
5. 整个对象或后代 location move 后源变 typed empty：再读/再 move trap；
   父对象布局仍有效；目标销毁一次。
6. `r := { a := allocate ^1; break 42 }`：r 绑定 self root/result location；
   42 写入同一个 location；字段 a 被销毁；`$(\r)` 得 42。
7. `r := { break allocate ^1 }`：break 值 move 进 result；
   result 是同一个 location；没有额外隐藏槽。
8. `allocate ^1` 在 caller block 产生 location；返回 location 的 wrapper
   可被 `:=` 绑定；返回 value 的 wrapper 不能。
9. 闭包不能绑定外部 location；捕获 value 复制、对象整体 move；
   两个闭包共享可变对象必须经 cell。
10. module 为 frozen：多协程只读共享；写或附加 `\destroy` 拒绝。
11. comptime 构造可变 location 并试图固化到运行期：编译错误；
    只读数据、frozen、prototype/code 可固化。
12. 裸指针指向 location；对象 move/drop 后解引用为 UB（文档断言）。
13. 静态可证明的 move 路径无运行时检查；动态路径 use-after-move trap。
14. `p := { result := allocate ^7 }`；`$(\p.result)` 为 7；
    `result` 是普通字段，没有隐式 result 概念。
15. 无约束的空 location 接收不同形状的 move 子树：成功，源 empty；
    实现可重分配/装箱，不得因容量拒绝；有显式约束的槽则按形状拒绝或接受。
16. 逃逸 block 对象中的 empty 字段作为 tombstone 保留，销毁时跳过；
    临时 block 对象中的 empty 字段随对象一起释放。
17. 块内 `\this` 取得 view 读取字段；view 不能存进字段/捕获/返回；
    `\this` handle 在块结束后使用是运行时错误。

## 十三、待办 / 未决

- [x] block 的 result location 与 self root 已统一：同一个 location，
      不存在需要额外消费的内部槽。
- [x] 03 的模式绑定：`:=` 纯命名；`:` pattern 的提交 = stdlib
      `allocate` + `%bind`；match 返回 `Bindings | fail`。
- [ ] 非自有对象的结构化匹配：match 的 subject 是 value；对象比较/匹配
      经裸指针属不安全 API；Bindings 不能携带对象子树。需要明确 v1 的
      destructuring 边界。
- [ ] 可变性从 03 的路径继承改为 per-location 的正式修订文本（已开始）。
- [ ] 可选 shape 约束的语法、推断边界与检查时机（静态/运行时）。
- [ ] typed empty 在对象 shape 中的表示；部分 move 后父对象的压缩策略。
- [ ] `\this` handle 的完整能力面、过期检查与跨 comptime 边界规则。
- [ ] view 参数是否从 `\this` 推广到普通调用；当前非自有访问走指针。
- [ ] 析构函数挂起、失败、循环引用与协程 GC 终结的细则。
- [ ] frozen 的运行期构造与生命周期（v1 仅 comptime frozen）。
- [ ] stdlib wrapper 对调用结果的契约：暴露 location 还是 value。
- [ ] `allocate` / `move` / `drop` / `clone` / `freeze` / 指针取址的最终语法拼写。
