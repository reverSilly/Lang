# 求值模型设计记录

状态：草案（2026-10-25 按 06 的 allocate/result/所有权模型重写）

前置：[01-comptime.md](01-comptime.md)、[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、
[06-ownership.md](06-ownership.md)

## 核心原则

内核只有一个求值原语：**在一个 location 中求值 prototype**，即 `allocate`。
函数、闭包、协程、生成器都是这一原语在不同操作符、不同环境下的模式，
不存在独立的"函数定义"机制。

**prototype 是纯代码，不携带环境；环境由 `allocate`/`new` 的操作符提供，
由 stdlib 的参数、闭包和 fix 协议组织。**

## 五件套

| 机制 | 含义 |
|---|---|
| `^Expr` | 原型：`Expr` 的未求值形态，是一个普通 value |
| `allocate` | 在当前 block 分配一个 location；参数是 prototype 时在其中求值并填充，参数是 shape 时留空；返回该 location |
| `new` | 创建一个新协程，以独立帧树求值 prototype；返回协程 handle |
| `@` | 挂起本协程，恢复目标协程并传 value |
| `\^` | 恢复本协程的协程的 handle |

- `allocate` 分配的 location 就是被求值 block 的 self root/result（06 §2.5）；
  不存在额外的调用结果槽。
- `allocate` 返回 location，因此可以被 `:=` 命名；返回 value 的函数只是
  stdlib wrapper 用 `$` 读出后的语法结果。
- 由 06 的 `:=` 规则，协程 handle、pointer、fail 等 **value** 不能直接用
  `:=` 命名；需要 `allocate ^value` 放进 location，或由 stdlib wrapper
  隐藏这一步（cell/new 的常见协议）。
- `new` / `@` 的调度细节（eager/lazy、表达式值、根帧返回）仍未定，
  见"诚实清单"；v1 不把它们作为函数调用的实现基础。

## 帧树与标识符解析

所有求值发生在一棵**帧树**上：

- 每个 `{}` block 在父 block 中分配自己的 result location/self root；
  block 的字段是其直接子 location；block 的符号表是这些字段的名字表。
- 符号解析是**词法**的：沿代码所在的 block 链查找；不存在隐式动态作用域。
- 跨帧访问必须显式导航：`.` 向下、上溯路径（符号待定）向上；被调用者
  访问调用者的局部值必须写成可见路径，不靠隐式环境链。
- `new` 创建独立的帧树：运行期栈帧隔离，但代码的静态词法/模块环境仍然
  随 prototype 代码；不自动注入 caller 的局部值。所需能力由创建者显式
  以 value/frozen 注入（06 §8）。
- 帧 handle（`\this`、`\^`）是 value；可通过 handle 取得临时 view 访问
  帧内 location；view 不能存储/捕获/返回（06 §2.4）。

## allocate / new 对照

| | `allocate p` | `new p` |
|---|---|---|
| 栈 | 当前协程的栈 | 新协程自己的栈 |
| 环境 | 当前 block（result location） | 全新帧树（静态代码环境保留） |
| 求值时机 | 立即进入 | 见待办：eager/lazy 未定 |
| 生命周期 | 当前 block 所有权树 / 词法作用域 | GC 协程 |
| 与外界的通道 | 帧树路径、字段、指针 | 仅 `@` / `\^`、显式注入 |
| 返回值 | 分配出的 location | 协程 handle（值） |

- `allocate` 的结果 location 是当前 block 的直接子，可被 `:=` 命名；
  其内容是 prototype 正常完成时的字段对象，或 break 写入的值。
- `new` 的协程根拥有其帧树上的 location；跨协程移动 value/object 按 06
  §7.1 的消息槽协议；不能传递 location 本身（指针除外，unsafe）。

## `\^`：恢复者 handle

`\^` 是特殊标识符，值为**恢复本协程的协程 handle**（普通 value）。

- 调用它：`\^@v`，把控制权连同 value 交还给恢复者；
- 访问它的帧内对象：经 handle 取得临时 view，受 06 的生命周期约束；
- 可以存储、传递（协程可以持有其他协程的引用）；过期 handle 使用是
  运行时错误；
- break 到协程根时，break 值经 `@` 通道交给 `\^`；协程内 = break，
  协程间 = `@`，两种非局部控制流在根帧焊接（04）。
- `\^` 不是 location，不能用 `:=` 命名；只能作为 value 传递。

## 函数 = allocate + stdlib wrapper

"函数调用"不是内核机制，是 stdlib 模式：

- **函数**：不挂起的 `allocate` 子程序；编译器在词法可证明时把它优化为
  普通栈帧。挂起点必须词法可见的纪律由 wrapper/类型系统承担，不能把
  动态分派后的挂起点当作函数。
- **参数是协议不是内核机制**：wrapper 在 callee 的 call frame 中分配
  参数 location，caller 以 value 复制或整棵 location move 填充；
  per-call frame 保证可重入。**不存在共享的 `Proc.in` 交接槽**；
  旧方案的共享槽不可重入，已退役。
- **返回值**：wrapper 在 caller 分配调用结果 location（06 §7），
  或把该 location 的叶子 `$` 读出后作为 value 表达式暴露；
  两种 wrapper 是不同接口契约，不能混淆。
- **递归**：block 不能通过名字引用自身；递归由 stdlib 的 fix/self 协议
  提供（Y combinator 风格），self 在 allocate 时动态提供；不暴露半构建
  的 location。fix 返回一个可调用的 location/函数对象，因此可用 `:=`
  命名。示意（非最终语法）：

```
fact := fix(^(self, n) =>
    alloc_result(
        1 if n <= 1
        else n * call(self, n - 1)
    )
)
```

- 旧的 `Proc.in` 共享槽、`f.in = x` 调用糖、对 block 名字的自引用全部退役。

## 捕获（capture）

内核不提供捕获机制，由 stdlib 分两层承载：

1. **值捕获**：闭包 = 普通对象 `{ env: {...}, code: ^(...) }`；
   env 的字段是 value 复制或整棵 owned 对象 move，不能绑定外部 location。
   代码通过**显式的 env handle/view** 访问字段，不能用 `\this` 代替：
   `\this` 指的是闭包对象自身的 self root，不是被捕获的外层帧。
2. **可变捕获**：被捕获的状态需要非词法生命周期 → 用 cell 协程承载
   （`new` 一个持有状态的 cell 协程，GC 管理生命周期），闭包持有其
   handle，经 `@`/`\^` 读写；cell 内 location 的所有权规则按 06 §8.1。

统一视角：**闭包是对象，对象的可变状态是协程，协程由 GC 托管。**
捕获清单由宏扫描自由标识符生成，但宏只能复制 value/move 对象；
不生成跨帧 location 别名。

## 全局环境、模块与导入

**全局环境 = 编译期值空间（01），只读，天然去重。** 不存运行期模块实例
cache——模块在编译期求值完毕，经固化进入二进制，全程序共享同一只读
frozen 实例。

- **静态导入**：由编译期 capability 提供；import 产生 prototype/code 值，
  经 `allocate`/固化后成为 frozen 模块。去重是编译期求值缓存的自然结果。
- **模块不持可变状态**：模块 = frozen 对象（纯代码与常量）。
  可变对象由外部持有者显式管理；共享可变状态在代码里永远有明确持有者。
- **动态导入**：需要运行期解析/求值代码，与 01 的“无 JIT”存在冲突；
  v1 只允许动态导入纯数据模块，或明确引入运行期解释器后再开放。
  静态导入与动态导入是两种机制，不混淆。
- **多文件独立编译**：`.proto` 模块是纯原型，外部引用保持为自由符号，
  在导入方求值时解析；模块头部以形状约束声明期望（模式匹配，非语法）。
- **协程的能力注入**：`withimport` 只能注入 frozen/value；禁止在 caller
  环境捕获 location 或可变对象。若需要，由 stdlib 显式复制/move 并写进
  协议；不产生隐式动态作用域。

## 程序入口

`main` = 运行时以全局 frozen 环境为初始环境 `allocate` 入口原型。
main 的 body 是一个 block，其 result location 是 main 的值；局部字段
住在自己的 self root 中，按 06 的所有权规则存活/销毁。
main 返回 fail 形状 ⇒ 运行时报告 fail 及 traceback。

## GC 边界

GC 只管理协程对象（独立生命周期、可互相持有引用、可能成环）。
`allocate` 的 location 全部走当前 block 的所有权树，不单独进 GC；
block 逃逸由父 block 拥有，跨协程转移用 move。不创建协程的程序零 GC
负担。裸指针不参与 GC 根，指向 GC 对象的指针有效性未定义（unsafe）。

## 显式条款（诚实清单）

- `new` / `@` 的调度与表达式值未定：eager/lazy、挂起时机、根帧返回值、
  协程初始 resume 都需要 05 的正式操作语义。本文不假设它们。
- 参数/结果的位置协议由 stdlib wrapper 固定，但 wrapper 的具体形状、
  Proc 形状、多参数展开仍需 05。
- 挂起点词法可见是函数优化的前提；动态分派/一等函数可能使子程序无法
  静态证明不挂起，此时不得按函数优化。
- 自由符号解析是 binding-time 问题（01）：缺省按动态结构编译；
  allocate 与 prototype 都静态可知时，可折叠为静态帧偏移。
- 帧 handle 的有效期检查是运行时错误；静态可证明时提前拒绝。
- 作用域/导入的动态能力注入可能滑向动态作用域；v1 只允许 frozen/value，
  禁止 caller 局部 location 注入。

## 词汇表

- 退役：`Proc.in` 交接槽、`f.in = x` 调用糖、独立函数定义语法、
  `?` 输入标识符、隐式 `arguments` 对象、隐式环境链、block 名字自引用、
  在 `\this` 下混用闭包 env。
- 复用：`{}`（block = 帧 + result location + 符号表）、
  `\^`（恢复者 handle）、`\this`（当前帧 handle，可取得 view）。
- 新增约定（非机制）：`Proc` 形状、per-call frame、值/对象捕获、
  fix/self 递归糖、frozen 模块。
- 非关键字：`import` 是编译期 capability，`withimport` 是 stdlib helper。

## 验收用例（自检清单）

1. `fact := fix(...)`：递归通过 self 参数，不依赖 `Proc.in` 或 block 名字；
   两个并发生成器调用同一 fact 不共享交接槽。
2. 值捕获闭包：env 字段为 value 副本/对象 move；修改源不改变闭包；
   闭包 env 经显式 env handle/view 访问，不用 `\this` 冒充。
3. `cell` 协程实现可变捕获，两个闭包交叉读写同一 cell。
4. 生成器：`new` + `\^@` yield，消费者经 `@` 驱动；调度细节待 05。
5. 静态检查：词法不可见挂起点的子程序拒绝按函数优化；
   `new` 的 prototype 引用运行期栈帧 → 编译错误。
6. `withimport`（stdlib helper）注入 frozen 模块；不注入 caller 局部值。
7. 编译期导入去重：两处导入同一路径得到同一 frozen 实例；
   模块头部形状约束不匹配 → 编译期链接错误。
8. `allocate` 返回调用者 block 的 result location；返回 location 的 wrapper
   可被 `:=` 绑定；返回 value 的 wrapper 不能。
9. `\this` 是帧 handle；块结束后使用是运行时错误；frame handle 不能固化。
10. 模块是 frozen；多协程只读共享；不能写或附 `\destroy`。

## 待办

- [ ] `new`/`@` 的正式操作语义与调度模型（05）
- [ ] per-call frame、Proc 形状、参数/结果协议的展开规则（05）
- [ ] fix/self 递归糖的具体形状（05）
- [ ] 闭包 env 的显式 handle/view 语法（05/语法章）
- [ ] 动态导入与运行期求值：数据限定或解释器方案（01/07）
- [ ] 自由符号的 binding-time 与静态优化规则（01）
