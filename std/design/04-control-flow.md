# 控制流设计记录

状态：已定稿（2026-10-24）

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)

## 核心原则

内核唯一的控制流原语是 **`break`**。
分支 = 匹配（03）；早退、错误传播、资源清理由 break 派生；
跨协程控制流仍只有 `@`/`\^`（02）。
语言没有 return、throw、label、defer 原语，内核不认识"错误"概念。

## break

```
break [handle] value
```

- 终止当前 block 的求值，沿本协程帧栈逐层弹出至目标帧；
  目标 block 表达式的值 = `value`，外层求值照常继续。
- 缺省目标 = 内层 block。
- 目标是**帧 handle 值**，不是 label。帧树里 block 全部匿名（02），
  handle 经 `\this` 获得、作为普通值存储/传递/捕获（02 捕获机制）。
  逐层上溯即逐帧比对 handle。
- 弹出经过的每一帧执行清理协议（见下）。
- **不跨协程**：break 上溯至本协程根帧仍未停止 ⇒ 协程求值结束，
  break 值经 `@` 通道交给 `\^`（恢复者）。两种非局部控制流在根帧焊接：
  协程内 = break，协程间 = `@`。

02 中 `allocate` 把被求值体挂接为调用处节点的子树，帧链在 allocate
边界连续。注意 core 只认 block 树：**"函数"是 stdlib 约定（Proc 形状，
02），内核不存在函数边界**。break 只按帧链逐层弹出，不在乎帧由哪次
求值产生——所谓"跨函数逃逸"是 stdlib 视角的描述，内核层面无此概念。

## `\this`

特殊标识符，值为**当前 block 的帧 handle**（普通值，可传递）。

- 早退 = `break <体帧handle> v`。`return` 宏（05 章）的展开目标：
  宏在体首绑定 `\this`，嵌套原型经值捕获拿到它。
- 对齐旧设计"`{}` 中 `\this` 引用本对象"——帧 = block = 构造中的对象，
  同一概念，无需新增。

## 清理协议（RAII）

对象可携带保留方法 `\destroy`（普通值字段，约定见待办）。

**触发点有两类，地位相同：**

1. **帧弹出**：帧内仍存活的 location，逆声明序 destroy 其值。
   被 break 穿过的帧按同一规则处理。
2. **`;` 丢弃**：`;` 的语义是丢弃前序值、保留末值（见旧设计 `;.md`）。
   被丢弃的值**在 `;` 位置立即 destroy**。

   ```
   { ... }; break 1    !- 分号处 destroy 前一个 block 的结果对象 -!
   ```

**所有权随结果对象走**：block 求值完毕，局部值并入结果对象，
所有权转移给该对象；帧不再持有它们。destroy 一个对象 =
先调用其自身 `\destroy`（若有），再递归 destroy 各字段值。

**defer 派生**：`defer` 宏 = 在当前帧 allocate 一个 scope-guard 对象，
其 `\destroy` 即注册的动作。非原语。

## 分支与迭代

- **if = 匹配二分支糖**（05 宏）。`expr if cond` 中 **cond 是被匹配的
  值**，用来匹配它的 pattern 固定为 `Exactly@true`（按值匹配，03）。
  cond 位置不接受任意 pattern——任意 pattern 的判别是 `?` 的职务。
  值类型规则沿用旧设计（无 else → `optional[T]`；有 else → union）。
- **match 分派宏** = 连续匹配 + 分支（03 已记，05 定规格）。
- **循环 = 尾恢复**。`while` 宏展开为对原型的递归 allocate，零分配；
  迭代值为末次周期的值。
  旧设计"while 收集结果进 Sequence"退役：收集是显式 stdlib 构造器
  （collector cell 协程），代价写在脸上。
- **生成器协议**（`new` + `\^@` yield，02 验收用例 4）是 stdlib 协议；
  for/map/filter 等均为其消费方式，不进内核。
- 退出循环 = break 到循环帧；进入下一周期 = 循环宏展开中的尾恢复，
  无 break/continue 关键字。

## 错误

**内核无错误概念。** fail 对象是普通值，fail 形状是 stdlib 约定
（保留字段，建议携带 `site` 帧 handle——恢复链即栈回溯，免费获得）。

- `:​=`/`:` 匹配失败 ⇒ `break <fail对象>`，**仅弹当前 block 一级，
  不做任何接力**。这清算了 03"失败的语义"待办。
- **传播 = stdlib `try` 宏**：把代码包成原型 `allocate` 求值，
  对结果做形状判别——fail 则 break 接力，成功则正常绑定。
  原料全部是已有机制（02 的 allocate 控制流能力 + 03 的形状匹配）。
  接力密度由用户选择：逐语句、逐函数、或全手动。
- **捕获 = `catch` 宏**：包原型 `allocate`，判别结果，fail 进 handler。
- **丢弃纪律**：语言不检查被丢弃的 fail（无 `;` 检查、无自动接力——
  明确拒绝做成语言特性）。显式丢弃写 `_ :=`（03 通配 symbol）；
  lint 可对"被丢弃的 fail 形状值"警告，属于工具不属于内核。
- main 的值为 fail 形状 ⇒ 运行时报告 fail 对象（含 site 链）并退出。约定。

## `?` 形式

```
v ? pattern
```

纯查询匹配：问 `match(pattern, v)` 成败，返回 Boolean。
**无绑定、无 break、无控制效应**；可折叠；可对动态值使用。

不替代任何 pattern 构造器，各司其职：

- 裸 symbol → 绑定（`%bind` 终点，03）；
- `Exactly@x` → 按值相等判别（stdlib matcher，**不产生绑定**。
  `Exactly@a := a`：左侧严格匹配对象 `a` 本身，不会把 symbol `a`
  绑定到值 `a` 上）；
- Shape → 类型系统职责（06 章），形状匹配复用 match 分派机制，
  但"形状是否允许出现"是静态检查而非运行期匹配；
- `?` → 把以上一切判别能力暴露给条件位置的纯查询入口。

## 词汇表

- 新增原语：`break`、`\this`。
- 新增约定（非机制）：`\destroy` 清理协议、fail 形状（stdlib）。
- 退役：return、throw/try-catch 机制、label、defer 原语、
  break/continue 关键字、while 的 Sequence 收集、
  内核级 fail 概念、`;` fail 检查、一切自动接力。
- 沿用：`@`/`\^`（02）、匹配壳 `:=`/`:`（03）、`\destroy` 命名沿旧设计
  `\$match` 一族的保留字段约定。

## 与 01/02/03 的对接

- **01**：comptime 中 break/destroy 语义相同（一个求值器）；
  comptime 的 break 不产生额外阶段规则。
- **02**：帧链跨 allocate 连续 ⇒ break 沿帧链逐层弹，core 只认 block 树、
  无函数边界（函数 = stdlib 的 Proc 约定）；根帧焊接 `@`/`\^`；
  handle 捕获走 02 捕获机制，无新通道。
- **03**：清算"失败的语义"待办；`?` 落定；`Exactly` 等以 stdlib
  matcher 身份保留（修正 03"消解"表述：消解的是内核 pattern 类，
  不是这些概念本身）。

## 诚实清单

- break 目标是可传递的值 ⇒ 可跳到任意已知帧。无 label 的纪律靠
  "handle 的获取与传递永远词法可见"维持；远跳可读性由评审/lint 承担。
- 所有权/别名未定：带 `\destroy` 的值被裸拷贝 ⇒ 双重 destroy。
  临时约定：资源对象不裸拷贝，共享经 cell 协程（02）或显式 clone；
  是否引入 move/affine 检查挂账 06。**明确继续拒绝借用检查。**
- 忘记写 `try` 的函数把 fail 当普通值返回，调用方可能将其当数据使用。
  静态一侧：形状 union `T | fail` 可在类型检查时提醒（依赖 06 +
  01 binding-time）；动态一侧靠测试。接受此取舍。
- `\destroy` 递归深度、循环引用、destroy 中再次 break/出错的行为，
  挂账 06/stdlib 实现时细化。
- `{a}; {b}` 与 `{ {a}; {b} }` 相差一次 `;` destroy——序列点的
  销毁语义必须在文档与宏展开中保持一致（05 注意）。

## 验收用例（自检清单）

1. `r := { a := 1; break $(\a)+1 }` ⇒ `$\r` 为 2；`a` 的 location 随
   中止帧清理（无 `\destroy` ⇒ 无操作）。
2. RAII：带 `\destroy` 的资源对象所在 block 被 break 穿过 ⇒
   destroy 被调用；同帧多资源逆声明序调用。
3. `{ g := scope_guard }; break 1` ⇒ `;` 处 destroy 前一 block 的
   结果对象（含 `g`），随后 break 正常执行。
4. `return` 宏展开：深嵌套 block 中 return ⇒ break 至体帧 handle，
   途中各帧清理执行。
5. `try` 宏：内层 `:=` 失败 ⇒ fail 经逐层 try 接力至外层 catch，
   每站展开可见，无隐藏通道。
6. `catch` 宏：handler 收到 fail 对象（含 site handle）。
7. break 至协程根 ⇒ 协程结束，恢复者的 `@` 表达式取得 break 值。
8. `v ? Exactly@a` 纯查询返回 Boolean；`Exactly@a := a` 不产生绑定。
9. `if` 宏展开为二分支匹配：cond 作为值被 `Exactly@true` 匹配；
   任意 pattern 判别经 `?` 完成。
10. comptime 中执行含 break/destroy 的代码，语义与运行期一致（01）。

## 待办

- [ ] 05 章宏规格：return / try / catch / if / while / defer 的展开规则
- [ ] 06 章：fail 形状保留字段、所有权/别名规则、`\destroy` 签名与细则
- [ ] 帧上溯路径符号（02 待办）与 `\this` 的配合：导航 vs handle 直传
- [ ] lint 规则：被丢弃的 fail 形状值
