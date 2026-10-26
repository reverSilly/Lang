# 02 - 求值模型、块与协程

前置：[01-comptime.md](01-comptime.md)
后续：[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)

## 一、核心原则

语言在内核层面彻底解耦 **“延迟代码（`prototype`）”** 与 **“环境/栈帧分配（`block` / `new`）”**：

1. **`^Expr`（Prototype）是全语言唯一拦截求值的语法特殊形式（Special Form）**：它把未求值的表达式 `Expr` 包装成一个纯代码值（`prototype` value），不携带任何环境或状态。
2. **求值原语全是接收 `prototype` 值的普通操作符**：环境永远由触发求值的操作符提供——在当前环境就地展开用 `evaluate`，开辟子帧求值用 `block`，开辟独立协程求值用 `new`。
3. **不存在独立的“函数定义”内核机制**：函数、闭包、生成器、协程、模块全部由这组原语与标准库协议导出。

## 二、求值原语与特殊标识符

### 2.1 核心求值机制

| 机制 | 类别 | 语义 | 返回结果 |
| :--- | :--- | :--- | :--- |
| `^Expr` | 唯一特殊形式 | 取 `Expr` 的延迟形态，不求值 `Expr` | `prototype`（`value` 子类） |
| `evaluate proto` | 内核求值原语 | 在**当前环境（current env）**直接执行 `proto`；不新建 location、不切换 env | 透传 `proto` 的结果（`value` 或 `location`） |
| `block proto` | 内核建帧原语 | 在当前 block 下隐式分配匿名 `self root` location 作为**新子 env**，在其中执行 `proto`，闭合时完成相变清理 | 该 `self root`（`location`，处于 `live` 状态） |
| `new proto` | 内核协程原语 | **惰性（Lazy）**创建独立协程根帧（初始停在 `proto` 入口 PC=0，不执行代码） | 协程 handle（`value` 子类） |
| `%resume(target, src_loc)` | 内核切换原语 | 原子地将 `src_loc` 移入 `target` 的 `\in`、将 `target` 的 `\^` 设为当前协程、挂起自身并恢复 `target` | 本协程下次被唤醒时的 `\in`（`location`） |
| `{ Expr }` | 标准库语法宏 | 直接展开为 `block ^(Expr)`；空块 `{}` 展开为 `block ^()` | 同 `block` |
| `target @ rhs` | 标准库分派器 | 若 `rhs` 为 `location` 则调 `%resume(target, rhs)`；若为 `value` 则装入临时槽后调 `%resume` | 本协程被唤醒时的 `\in`（`location`） |

### 2.2 三个核心特殊标识符

| 标识符 | 类别 | 含义 |
| :--- | :--- | :--- |
| `\this` | `value`（帧 handle） | 指向当前 `block` 的 `self root` location 的句柄 |
| `\^` | `value`（协程 handle） | 指向最近一次唤醒本协程的那个协程的句柄（由 `%resume` 原子更新） |
| `\in` | `location` | 本协程根帧持有的来信槽，存放最近一次唤醒本协程时由对方传入的内容 |

## 三、`evaluate` 与 `block` 的正交分工

### 3.1 为什么解耦 `evaluate` 与 `block`

- **`evaluate proto`（就地求值 / Mixin 注入）**：
  不创建新帧，也不隔离符号。`proto` 内部的 `%bind` 和字段访问直接作用于调用方的当前 `self root`。这使得多个 `prototype` 可以在同一个正在构建的 `block` 内顺序组合，共同完成分段初始化：
  ```
  obj := {
      evaluate $\init_part_a;
      evaluate $\init_part_b
  }
  ```
- **`block proto`（子帧隔离 / 对象构造）**：
  只要定义方希望词法隔离（如普通函数体、局部作用域、结构体构造），就用 `{ ... }`（即 `block ^(...)`）将自身包裹起来。

### 3.2 严禁显式 `with <loc> evaluate`

语言**不提供**将外部已存在的 `location` 重新作为当前 `env` 注入 `evaluate` 的原语：
- 一个 `location` 只有在作为匿名 `block` 的 `self root` 处于栈顶构建期（Constructing）时，才允许向其内部追加子 `location`（字段）；
- 一旦 `block` 结束，未命名临时量被剥离销毁，对象结构闭合为 `live`；
- 拒绝 `with <loc> evaluate` 从根本上保证了：**已闭合的对象绝不会被二次拆开追加字段，且构建了一半的 location 绝不会提前暴露给父作用域**（见 [05-ownership.md](05-ownership.md)）。

## 四、帧树与确定性导航

所有求值发生在帧树（Frame Tree）上：
- 每个 `block` 求值时在父 block 下挂载一个新节点（即其 `self root`）；
- **内核只提供确定性的单步导航原语**：
  - 向下访问子字段：`%field(place, symbol)`（语法记作 `.`）；
  - 向上访问直接父帧：`%parent`（单步父帧导航原语，具体表面符号由语法章固定）。
- **无隐式动态作用域**：因为 `prototype` 是纯代码、不携带环境，内核不存在隐式的环境查找链。被调用者若需访问外层帧的 location，在内核层必须经由显式的 `%parent` 路径；日常编码中的“沿词法作用域链自动向上查找”由标准库/编译期宏在编译期静态展开为确定性的 `%parent` 导航序列。
- **协程树隔离**：`new proto` 另起一棵独立的协程帧树，运行期不允许通过 `%parent` 跨越协程根帧访问创建者的运行期栈帧（静态拒绝）。

## 五、协程状态机：`new`、`%resume`（`@`）与 `\^` / `\in`

### 5.1 惰性创建与统一来信槽 `\in`

1. **创建（`c := new proto`）**：
   `new` 是**惰性（Lazy）**的——仅分配新协程的根帧（含空的来信槽 `\in`），将指令指针停在 `proto` 入口（PC=0），处于挂起态（Suspended），并立即向创建者返回协程 handle `c`。创建过程不执行 `proto` 的任何代码，不挂起创建者。
2. **首次唤醒（`c @ v1`）**：
   调用方通过 `@` 触发 `%resume(c, slot_of_v1)`：
   - 内核原子地将 `slot_of_v1` 的内容 `%move` 进协程 `c` 的 `\in` 槽；
   - 将 `c` 的 `\^` 设为调用方协程 handle；
   - 挂起调用方，协程 `c` 从 `proto` 的第 0 行开始执行，直接读取 `$\in` 即可取得首次传入的 `v1`，**没有任何值在启动时被丢弃，也不需要预先空跑预热（prime）**。
3. **中途挂起与再次唤醒（`\^ @ reply`）**：
   协程 `c` 内部执行 `\^ @ reply` 时，将 `reply` 移入恢复者 `\^` 的 `\in` 并挂起自己；当未来某协程再次以 `c @ v2` 唤醒 `c` 时，`v2` 移入 `c` 的 `\in`，且 `c` 中原本挂起的 `\^ @ reply` 表达式求值完成并返回 `c` 自身的 `\in` location。因此，唤醒后无论写 `msg := $(\^ @ reply)` 还是写 `\^ @ reply; msg := $\in`，语义完全等价。
4. **根帧结束与终止**：
   当协程 `c` 的根帧正常求值完毕，或 `break final_val` 弹出至协程根帧时（见 [04-control-flow.md](04-control-flow.md)）：
   - 协程 `c` 状态标记为已终止（Dead）；
   - 内核将最终结果移入当前 `\^` 的 `\in` 槽，销毁 `c` 的根帧，并恢复 `\^` 执行；
   - 对已终止（Dead）或正处于运行链上（Running）的协程执行 `%resume` 是运行时错误（trap）。

```
!- 示例 1：单次计算协程（零预热直接读 \in） -!
add_one := new ^{ break ($\in + 1) };
res := $(add_one @ 41);   !- res 为 42 -!

!- 示例 2：生成器（Generator） -!
gen := new ^{
    \^ @ 1;
    \^ @ 2;
    break 3
};
a := $(gen @ ());         !- 1 -!
b := $(gen @ ());         !- 2 -!
c := $(gen @ ());         !- 3 -!
```

## 六、函数、闭包与模块（标准库模式）

### 6.1 函数 = 不挂起的 `block` + `evaluate` 包装协议

- **调用帧（Per-call Frame）**：函数调用不是内核原语，而是 stdlib wrapper——每次调用通过 `block` 开辟独立的调用帧，在帧内分配参数 location 并用 `=`（Copy 或 `%move`）填入实参，随后 `evaluate` 函数体 `prototype`。
- **零开销退化**：词法可证明不发生协程挂起（`%resume`）的子程序，编译器直接将其 `block` 帧退化为普通硬件栈帧压栈。
- **递归（`fix` 协议）**：因为绑定在 RHS 求值完成后才生效（[05-ownership.md](05-ownership.md)），对象不能按名字自引用未完成的自身；递归函数由 stdlib 的 `fix` 组合子在调用帧中动态注入 `self` 参数实现。

### 6.2 闭包捕获

内核不提供隐式捕获，由 stdlib 分两层承载：
1. **值与对象移动捕获**：闭包是普通对象 `{ env: {...}, code: ^(...) }`。宏扫描自由符号，将外部 `value` 复制（或将 owned `location` 整棵 `%move`）进 `env` 子对象；代码通过显式的 `env` handle 访问捕获字段（不占用 `\this`）。
2. **共享可变状态捕获（`cell` 协程）**：需要跨闭包共享可变状态时，使用 `new` 创建一个持有可变状态的 `cell` 协程（由 GC 管理生命周期），多个闭包持有该协程的 handle Value，经 `@` 消息安全读写。

### 6.3 全局环境与模块导入

- **全局环境 = 编译期只读 `frozen` 值空间**。模块在编译期求值完毕并固化为 `frozen` 对象进入二进制，全程序与多协程共享同一只读实例，天然去重，不持任何可变状态。
- **静态导入**：`import` 是编译期元层 capability，返回模块 `prototype`，经编译期求值与固化后得到 `frozen` 模块实例。
- **协程能力注入（`withimport`）**：由于 `new` 默认隔离运行期环境，stdlib 提供 `withimport` helper 将所需的 `frozen` 模块引用注入新协程根帧，无跨栈引用风险。

## 七、GC 边界

- **GC 仅管理 `new` 创建的协程对象**（独立生命周期、句柄可复制传递、可相互引用成环）。
- **`block` 与 `allocate` 产生的 `location` 全部走词法所有权树**，绝不进入 GC。
- 不调用 `new` 创建协程的程序**零 GC 负担**。

## 八、验收用例（自检清单）

1. `{ a := 1; b := 2 }` 宏展开为 `block ^(a := 1; b := 2)`，返回包含字段 `a`、`b` 的 `live` 不可变 location。
2. 同一 `{ ... }` 内连续执行两次 `evaluate`，共同向当前 `self root` 绑定字段，闭合后一次性成为完整对象。
3. 递归函数通过 stdlib `fix` 协议实现，两个协程并发调用同一递归函数各自拥有独立的 `block` 调用帧，互不干扰。
4. `c := new proto` 惰性创建协程，首次 `c @ 10` 将 `10` 放入 `c` 的 `\in` 并从 `proto` 入口启动，无任何值丢失。
5. `cell` 协程通过 `\in` 与 `\^ @ ...` 实现多闭包交叉读写同一状态。
