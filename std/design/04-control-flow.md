# 控制流设计记录

状态：草案（2026-10-25 重写，按 06 的 block/result 归并语义；
同日修订循环与 `%drop`：循环不产生 block，while 不做隐式销毁，
`{}` body 需用户显式 `%drop` 才按轮回收）

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、
[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、
[06-ownership.md](06-ownership.md)

## 核心原则

内核控制流只有两个原语：

- **`break`**：非局部终止，把值写进目标 block 的 self root/result location；
- **`%branch`**：纯值二分支，选择 then/else prototype 之一求值。

分支 = `%branch` + 匹配（03）；早退、错误传播、资源清理由 break 派生；
跨协程控制流仍只有 `@`/`\^`（02）。
语言没有 return、throw、label、defer 原语，内核不认识"错误"概念。
`if`/`while`/`try`/`catch`/`return`/`defer` 都是 stdlib 宏（05）；
`while` 的契约归 05，实现在需要时可降为编译器派生 form。

## break

```
break value
break handle value     !- 显式目标；语法章裁决分隔符 -!
```

- break 终止从当前 block 到目标 block 的求值，把所有经过的 block 的
  result location 按 06 §6 处理；目标 block 的 result location 接收 value。
- 缺省目标是当前 block；显式目标是**帧 handle 值**，不是 label。
  目标必须是当前协程中的祖先帧；非祖先/过期 handle 是运行时错误。
- 目标 result location 就是目标 block 的 self root（06 §2.5），不是独立槽。
  写入前，目标已有的字段对象按 06 §6 销毁；如果 value 是从目标子树中
  move 出来的，先摘下该子树，再销毁其余字段，最后装入，禁止自引用环。
- 途经的非目标 block：先摘下 break value（若它位于其中），再逐个销毁其
  result location/字段；目标 block 的 result 保留并成为该 block 的值。
- break 到本协程根帧仍未停止 ⇒ 协程执行结束，值经 `@` 通道交给恢复者
  `\^`。协程内 = break，协程间 = `@`；两种非局部控制流在根帧焊接。
- 内核没有"函数边界"：帧链跨 allocate 连续；"跨函数 return"只是
  stdlib 的 handle/result 协议，不是新的内核机制。

## `\this`

`\this` 是当前 block 的**帧 handle**（普通 value），指向该 block 的
self root/result location。它**不是** location，不能用 `:=` 命名。

- 可以复制、存储、捕获；但 handle 有效期绑定其 block 的 frame。
  帧结束后再使用是运行时错误；静态可证明时编译期拒绝；禁止固化到
  comptime 边界。
- 从 handle 取得临时 view 以读写当前 block 的字段；view 不能存储、
  捕获、返回或 move（06 §2.4）。
- 早退 = `break <体帧handle> v`。`return` 宏（05）在体首取得体帧 handle，
  嵌套原型经值捕获拿到它；捕获的是 handle，不是 view。
- `\this` 不能替代别的帧：访问调用者的局部值必须经显式 handle/路径；
  不存在隐式动态环境。

## 清理协议（RAII）

对象可携带保留方法 `\destroy`（普通字段，约定见 06 §6）。
`%drop(location)` 是唯一显式销毁原语；`\destroy` 是销毁过程中被调用的
钩子，不是另一个操作。

**触发点：**

1. **对象销毁**：move 覆盖、block 被 break 终止、父对象销毁、协程结束、
   显式 `%drop`。销毁顺序：先 `\destroy`（以 self 的 view 调用），再按创建
   逆序销毁仍 live 的直接子 location，empty 跳过。
2. **block 正常完成**：未命名/未绑定的直接子 location（临时量）逆序销毁；
   已命名的成为 self root 的字段，随对象存活。未绑定的 block result
   location 在**父 block 结束**时统一回收，而不是在 `;` 处。
3. **`;` 只丢弃，不析构**：`;` 丢弃前序表达式结果；value 无操作，
   location/place 不会被销毁。`:=` 产生 unit，所以
   `x := allocate ...; rest` 不会在分号处销毁 x。

- `\destroy` 不得 move 出 self 或其字段，不得 break 离开、不得挂起、
  不得创建新协程；允许读取字段和调用子对象的关闭方法。
- 资源必须是 owned location（对象字段、块内临时量，如 defer guard）
  或 cell 协程，不能是裸 value；提前释放用 `%drop`。
- **defer 派生**：在当前 block allocate 一个**未命名**的 scope-guard
  location（不 `:=` 绑定），其 `\destroy` 即注册的动作。
  `;` 不再销毁它；它在 block 结束时作为未绑定临时量被回收，因此
  defer 不需要新的内核机制，也不会随逃逸对象存活。
  这依赖核心裁决 15（`;` 只丢弃）。

## 分支与迭代

- **分支的底是 `%branch(cond, thenProto, elseProto)`**（03 原语）：
  纯值判别，cond 为真走 then prototype，否则走 else prototype；不绑定、
  不看 pattern、无 checkpoint。cond 使用哪个 builtin 类型留待 07，
  v1 暂定为 Boolean。
- 两个 prototype 分别经 `allocate` 在当前 block 求值；**`%branch` 的值就是
  被选中分支的 result location**（06 §2.5）。因此分支的值是 place，不是
  value；需要叶子值时写 `$`，需要对象时直接绑定/移动该 location。
- **if = 二分支糖**（05 宏），直接展开在 `%branch` 上：
  `expr if cond else expr2` ⇒ `%branch(cond, ^expr, ^expr2)`；
  无 else 时 else 分支为 `^()`，v1 不引入 optional 值类型。
  cond 位置只接受 value；任意 pattern 的判别是 `?` 的职务。
- **match 分派宏** = 对每个分支调纯函数 `match(pattern, subject)`，
  得到 `Bindings | fail`；失败则 `%branch` 到下一分支；成功则先执行提交
  循环（`allocate` + `%bind`，Bindings 落进当前 block），再进入分支体。
  matcher 必须纯；绑定在匹配期间不可见（03 §匹配的内核化简）。
- **循环不产生 block（核心裁决 16）**。`while` 是 stdlib 宏；循环本身没有
  自己的 block、self root 或符号作用域。循环体和条件里未包进 `{}` 的部分
  按**外层 block** 求值，循环不提供新的所有权或符号边界。因此：
  - 循环体里没有包进 `{}`、也没有显式 `%drop` 的 `allocate` 是外层 block
    的直接子 location，循环结束后仍然存活到外层 block 结束；循环不销毁它：

    ```
    {
        (allocate integer) while cond
        !- integer 属于本 block，循环结束后仍存在 -!
    }
    ```

  - 同理，循环体里没有 `{}` 的 `:=` 落在外层 block。它在第二轮就是重复
    绑定（03/06 禁止）；需要每轮新名字时必须用 `{}`。
  - 需要“整个循环一个作用域”时，把 **while 整体**包进 `{}`；这仍然只是
    一个 block，不是每轮一个。
  - `defer` 遵循同一规则：循环体没有 `{}` 时 guard 留在外层 block，
    循环结束后才执行；需要每轮执行时把循环体写成 `{}`（guard 是未绑定
    临时量，随每轮 body block 完成回收）。
- **`{}` 体本身不带来每轮 root 回收**。body block 的 self root 按 06 §2.1
  分配在父 block；若未绑定/未 `%drop`，root 与已命名字段作为父 block 的
  临时量活到父 block 结束。每轮 block 完成只回收块内未绑定临时量（含
  `defer` guard）；已命名字段随 root。
- **需要每轮销毁 body block 时，用户显式写 `%drop`**：
  `(%drop({ body })) while cond`。`%drop` 接收 block 的 result location，
  递归销毁后它变 empty（06 §6）；下一轮重新分配 root。`%drop` 是唯一的
  显式销毁原语，不是 while 的内置规则。
- **while 不做任何自动销毁**，也不需要识别 body 是不是 block。body 就是
  普通表达式：`{...}`、`%drop({...})`、`(allocate integer)`……while 只负责
  条件与迭代；销毁策略由用户表达式显式决定。非 block 体若写
  `%drop(allocate integer)`，同样每轮销毁该 location；不写则按裁决 15
  活到外层 block 结束。
- **不需要额外的包装组合子**。`{}` 不是自动销毁标记，`%drop` 才是销毁
  动作；不新增 `destroy` 操作，`%drop` 是唯一拼写（`\destroy` 是析构钩子，
  不是操作）。
- **循环自身的脚手架**（`%branch` 的分支结果、尾恢复状态）由 while 的展开
  自行复用或回收，不得按迭代数泄漏；它们不是用户可见的 block，也不得
  拥有用户循环体分配出的 location。现在的 `%branch` 总会分配分支结果
  location，这是 05 必须解决的冲突：要么让结果被丢弃的分支在原 block 中
  求值，要么引入不建立所有权边界的迭代形式。若无法同时满足，优先保持
  本节的生命周期规则，改动分支原语；纯 stdlib 展开做不到时，允许把 while
  定为编译器派生 form——接口与宏契约不变，内核原语集合不增。
- **没有循环帧 ⇒ 没有内核级“只退循环”**。break 的目标是当前 block 的帧
  handle：非 block 体的裸 `break` 退到包含循环的那个 block；`{}` 体的裸
  `break` 只退到 body block，即结束本轮、while 继续。两种都不是“只退循环”。
  若 05 需要 `break`/`continue` 式循环控制，必须由 while 的 stdlib 协议
  显式提供（例如宏注入的 handle）；内核不新增 label、循环帧或循环块。
  v1 以条件退出为准；提前退出 = break 外层 block。
- **迭代值**：while 不收集结果（旧的 `Sequence` 收集退役）；需要收集时用
  显式 stdlib collector cell。while 表达式自身的值由 05 固定。
- **生成器协议**（`new` + `\^@` yield，02 验收用例 4）是 stdlib 协议；
  for/map/filter 等均为其消费方式，不进内核。

## 错误

**内核无错误概念。** fail 对象是普通 value，fail 形状是 stdlib 约定。

- `value : pattern` 匹配失败 ⇒ 产生 fail 值并 `break` 当前 block 的
  result location；**只终结当前 block 一级**，不自动接力。
  `:=` 是命名，不是匹配，不产生 fail。
- **传播 = stdlib `try` 宏**：把代码包成 prototype `allocate` 求值，
  对 result 做形状/值判别——fail 则 break 接力，成功则正常绑定。
  原料是已有机制（allocate 的控制流能力 + match）。
- **捕获 = `catch` 宏**：包 prototype `allocate`，判别结果，fail 进 handler。
- **fail 必须可复制**（06 §8.4）：fail 不能拥有资源；`site`/traceback 是
  **冻结快照**，不是活帧 handle。恢复链的打印不触碰已弹出的 frame。
- **丢弃纪律**：语言不检查被丢弃的 fail（无 `;` 检查、无自动接力）。
  fail 是 value，直接以 `;` 丢弃；若它是 location，则用 `%drop` 显式销毁，
  `;` 只丢弃 place、不析构。没有 `_ :=` 这种“绑定 value”写法
  （`:=` 只命名 location）。lint 可警告被丢弃的 fail 形状值，但不进内核。
- main 的值为 fail 形状 ⇒ 运行时报告 fail 对象及其 traceback 并退出。

## `?` 形式

```
v ? pattern
```

纯查询匹配：调 `match(pattern, v)`（stdlib 纯函数），返回 Boolean。
**无绑定**（Bindings 丢弃，不执行提交）、无 break、无控制效应；
可折叠；可对动态值使用。

不替代任何 pattern 构造器，各司其职：

- 裸 symbol 在 pattern 中 → 绑定目标（提交阶段经 `%bind`）；
- `Exactly(x)` → 按值相等判别（stdlib matcher，不产生绑定）；
- Shape → 类型/形状系统职责（07），形状匹配复用 match 分派机制；
- `?` → 把判别能力暴露给条件位置的纯查询入口。

非自有对象的结构匹配需要指针/不安全 API；v1 的安全匹配只覆盖 value
与 frozen 对象。这一点与 03 的 Bindings 限制一致。

## 词汇表

- 新增原语：`break`、`%branch`、`\this`、`%drop`。
- 新增约定（非机制）：`\destroy` 清理协议、fail 形状、defer guard、
  fix/self 递归糖。
- 退役：return、throw/try-catch 机制、label、defer 原语、
  break/continue 关键字、while 的 Sequence 收集、循环帧/尾恢复帧、
  独立隐式 result 槽、内核级 fail 概念、内核匹配失败信号与
  checkpoint/rollback、`;` fail 检查、一切自动接力。
- 沿用：`@`/`\^`（02）、match 壳 `:`（03）、`\destroy` 命名。

## 与 01/02/03/06 的对接

- **01**：comptime 中 break/销毁语义相同（一个求值器）；
  comptime 的 break 不产生额外阶段规则；owned 树在阶段结束销毁。
- **02**：帧链跨 allocate 连续 ⇒ break 沿帧链逐层弹，core 只认 block 树、
  无函数边界；根帧焊接 `@`/`\^`；handle 捕获走值复制，无新通道。
- **03**：`:=` 命名、`:` pattern；`%branch` 是分支底；Bindings 只含 value；
  匹配失败是 fail 返回值，不是内核信号。
- **06**：block 的 self root = result location；break 写入同一个 location；
  `\this` 是 handle；清理/销毁/typed empty/非自有指针按 06；
  循环不产生 block（裁决 16），非 `{}` 体 allocation 属于外层 block。

## 诚实清单

- break 目标是可传递的 value ⇒ 可跳到任意已知祖先帧。无 label 的纪律靠
  "handle 的获取与传递永远词法可见"维持；远跳可读性由评审/lint 承担。
- 过期/非祖先 handle 的检查是运行时错误；静态可证明时提前拒绝，
  但不保证全静态。
- 所有权/资源对象不裸拷贝；共享经 cell 或显式 clone。
  是否引入 move/affine 检查由 06 的 per-location 规则承担，不引入借用检查。
- 忘记写 `try` 的函数把 fail 当普通 value 返回，调用方可能将其当数据使用；
  静态形状 union 提醒依赖 07 + 01 binding-time；动态一侧靠测试。
- 循环不产生 block：非 block 体的裸 break 会退出包含循环的 block，`{}`
  体的裸 break 只结束本轮；两者都没有“只退循环”。循环体内不带 `{}` 的
  `:=` 不能跨迭代重复。提前退出协议需显式构造 block 或依赖 while 的
  stdlib 约定。
- 不写 `%drop` 时，`{}` body 的 root/已命名字段活到父 block 结束；写
  `(%drop({ body })) while cond` 才每轮销毁。非 block 体同理：不写 `%drop`
  就留到外层 block 结束；while 不做隐式销毁。
- `\destroy` 递归深度、循环引用、`\destroy` 中再次 break/出错的行为，
  挂账 06/stdlib 实现。
- block 字段默认可见；可见性修饰是后续特性（06 核心裁决 14）。

## 验收用例（自检清单）

1. `r := { a := allocate ^1; break 42 }` ⇒ `r` 绑定 self root/result
   location；`$(\r)` 为 42；`a` 的 location 随 break 清理销毁。
2. RAII：带 `\destroy` 的资源对象所在 block 被 break 穿过 ⇒
   `\destroy` 被调用；同帧多资源逆声明序调用。
3. `defer` 展开为未命名 guard location；`;` 不销毁它；
   block 结束/break 穿过时它作为未绑定临时量被回收，`\destroy` 执行。
4. `return` 宏展开：深嵌套 block 中 return ⇒ break 至体帧 handle，
   途中各帧 result location 清理执行；handle 通过值捕获获得。
5. `try` 宏：内层 `:` 失败 ⇒ fail 经逐层 try 接力至外层 catch，
   每站展开可见，无隐藏通道。
6. `catch` 宏：handler 收到 fail 对象（含冻结 traceback 快照）。
7. break 至协程根 ⇒ 协程结束，恢复者的 `@` 表达式取得 break 值。
8. `v ? Exactly(x)` 纯查询返回 Boolean；不执行提交/绑定。
9. `if` 宏展开为 `%branch(cond, ^then, ^else)`；分支值是被选中分支的
   result location；无 else 的 else 分支为 `^()`。
10. comptime 中执行含 break/销毁的代码，语义与运行期一致（01）；
    固化时只允许 value/frozen/prototype，且销毁 owned 树。
11. 循环不产生 block：`{ (allocate integer) while cond }` 结束后
    integer 仍是外层 block 的子 location；把 while 整体包进 `{}` 时，
    该 block 结束时才销毁。
12. 循环体 `{}` 未 `%drop`：root/已命名字段活到父 block 结束，每轮只回收
    块内未绑定临时量；写 `(%drop({ body })) while cond` 时每轮销毁 root，
    字段/临时量/defer 按轮销毁；非 block 体写 `%drop(allocate integer)`
    同样每轮销毁，不写则活到外层 block 结束。while 不做隐式销毁。
13. 循环体内不带 `{}` 的 `:=` 不能跨迭代重复；带 `{}` 时块内每轮新名字。
    非 block 体的裸 break 退外层 block；`{}` 体的裸 break 只结束本轮。
    两者都没有内核级的“只退循环”。

## 待办

- [ ] 05 章宏规格：return / try / catch / if / while / defer 的展开规则
- [ ] 05 章循环协议：循环自身控制 location 的回收、`%branch` 与“循环不
      产生 block”的冲突消解；body 销毁由用户显式 `%drop` 表达，while 不
      处理。
- [ ] 07 章：fail 形状保留字段、Boolean/条件类型、形状与类型检查
- [ ] break 显式目标的语法分隔符（`break handle value` 的解析）
- [ ] frame handle 的完整能力面、过期检查与跨 comptime 边界规则
- [ ] lint 规则：被丢弃的 fail 形状值
