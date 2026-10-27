# 02 - 求值模型、块与协程

前置：[01-comptime.md](01-comptime.md)
后续：[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)

## 一、核心原则

语言在内核层面彻底解耦 **“延迟代码（`prototype`）”** 与 **“环境/栈帧分配（`block` / `new`）”**，且**一切表达式求值结果皆为 `value`（对 `location` 的引用统一表示为 `pointer` value）**：

1. **`^Expr`（Prototype）是全语言唯一拦截求值的语法特殊形式（Special Form）**：它把未求值的表达式 `Expr` 包装成一个不透明的纯代码值（`prototype` value），不携带任何环境或 AST 包袱。
2. **求值原语全是接收 `prototype` 值的普通操作符**：环境永远由触发求值的操作符提供——在当前环境就地展开用 `evaluate`，开辟子帧求值用 `block`，开辟独立协程求值用 `new`。
3. **不存在独立的“函数定义”内核机制**：函数、闭包、生成器、协程、模块以及标准库分派器（`:=`、`=`、`$`、`@`）全部由这组原语导出。

## 二、求值原语与特殊内建值

### 2.1 核心求值机制

| 机制 | 类别 | 语义 | 返回结果（皆为 `value`） |
| :--- | :--- | :--- | :--- |
| `^Expr` | 唯一特殊形式 | 取 `Expr` 的延迟形态，不求值 `Expr` | `prototype` |
| `a ; b` | 内核序列原语 | 先求值 `a` 并直接丢弃其结果 `value`（不触发析构），再求值并返回 `b` | 透传 `b` 的结果 `value` |
| `evaluate proto` | 内核求值原语 | 在**当前环境（current env）**直接执行 `proto`；不新建 `self root`、不切换 env | 透传 `proto` 的结果 `value` |
| `block proto` | 内核建帧原语 | 在当前 block 下隐式分配匿名 `self root` location 作为**新子 env**，执行 `proto`；闭合时剥离未绑定与非 `live` 槽，将 `live` 绑定字段压平为连续内存并冻结 `shape` | 指向该 `self root` location 的 `pointer` |
| `new proto` | 内核协程原语 | **惰性（Lazy）**创建独立协程根帧（初始停在 `proto` 入口 PC=0，不执行代码） | 协程 `handle` |
| `%resume(target, src_ptr)` | 内核切换原语 | 原子地将 `src_ptr` 指向的 `location` 内容 `%move` 进 `target` 的 `%in` 槽（`src_ptr` 置 `empty`）、将 `target` 的 `\^` 设为本协程、挂起自身并恢复 `target` | 本协程下次被唤醒时指向自身 `%in` 槽的 `pointer` |
| `{ Expr }` | 标准库语法糖 | 直接展开为 `block ^(Expr)`；空块 `{}` 展开为 `block ^()` | 同 `block`（返回 `pointer`） |
| `target @ rhs` | 标准库分派器 | 若 `rhs` 需要装箱则经 `box(rhs)` 装入临时槽得到 `src_ptr`，再调 `%resume(target, src_ptr)` | 指向本协程 `%in` 槽的 `pointer` |

### 2.2 三个核心环境标识原语

| 原语 / 记号 | 结果类型 | 含义 |
| :--- | :--- | :--- |
| `%this`（记作 `\this`） | `pointer` | 指向当前 `block` 的 `self root` location 的指针（当前帧符号解析 `\sym` 即 `%field(%this, sym)` 的语法糖） |
| `%caller`（记作 `\^`） | `handle` | 指向最近一次唤醒本协程的那个协程的句柄（由 `%resume` 原子更新） |
| `%in`（记作 `\in`） | `pointer` | 指向本协程根帧持有的来信槽 location 的指针，存放最近一次唤醒本协程时由对方移入的内容 |

## 三、`evaluate` 与 `block` 的正交分工

### 3.1 为什么解耦 `evaluate` 与 `block`

- **`evaluate proto`（就地求值 / 分派器与 Mixin 引擎）**：
  不创建新帧，也不隔离符号。`proto` 内部的 `allocate`、`%bind` 和 `%field(%this, ...)` 直接作用于调用方的当前 `self root`。这不仅让多个 `prototype` 可以在同一个正在构建的 `block` 内顺序组合完成分段初始化，更让标准库的 `:=`、`box`、`if`、`while`、`defer` 等宏与分派器能够直接在调用方当前 `block` 内就地生效：
  ```
  obj := {
      evaluate $\init_part_a;
      evaluate $\init_part_b
  }
  ```
- **`block proto`（子帧隔离 / 连续内存对象构造）**：
  只要定义方希望词法隔离或构造一个独立对象（如函数调用帧、局部作用域、结构体构造），就用 `{ ... }`（即 `block ^(...)`）将自身包裹起来。

### 3.2 严禁显式 `with <ptr> evaluate`

语言**不提供**将外部已存在的 `location` 指针重新作为当前 `%this` 环境注入 `evaluate` 的原语：
- 一个 `location` 只有在作为 `block` 的 `self root` 处于栈顶构建期（Constructing）时，其内部 `shape` 才允许随 `%bind` 动态生长；
- 一旦 `block` 闭合，所有 `live` 字段被压平为固定偏移量的连续内存，其 `shape` 彻底凝固（见 [05-ownership.md](05-ownership.md)）；
- 拒绝 `with <ptr> evaluate` 从根本上保证了：**已闭合对象的固定 `shape` 与连续内存布局绝不会被二次拆开追加字段，且构建了一半的 location 绝不会提前暴露给父作用域**。

## 四、帧树与确定性导航

所有求值发生在帧树（Frame Tree）上：
- 每个 `block` 求值时在父 block 下挂载一个新 `self root` location；
- **内核只提供确定性的单步导航原语（全部返回 `pointer` value）**：
  - 向下访问子字段：`%field(ptr, symbol)`（语法记作 `ptr.symbol`；当前帧 `\sym` 即 `%field(%this, sym)`）；
  - 向上访问直接父帧：`%parent`（返回指向直接父 `block` 的 `self root` 的只读或受控指针）。
- **无隐式动态作用域**：因为 `prototype` 是纯代码、不携带环境，内核不存在隐式的环境查找链。日常编码中的“沿词法作用域链向上查找自由变量”由编译期语法层静态展开为确定性的 `%parent` 导航或闭包捕获。
- **协程树隔离**：`new proto` 另起一棵独立的协程帧树，运行期不允许通过 `%parent` 跨越协程根帧访问创建者的运行期栈帧（静态拒绝）。

## 五、协程状态机：`new`、`%resume`（`@`）与 `\^` / `\in`

### 5.1 惰性创建与统一来信槽 `\in`

1. **创建（`c := new proto`）**：
   `new` 是**惰性（Lazy）**的——仅分配新协程的根帧（含初始处于 `empty` 状态的来信槽 `\in`），将指令指针停在 `proto` 入口（PC=0），处于挂起态（Suspended），并立即返回协程 `handle` 值。创建过程不执行 `proto` 的任何代码，不挂起创建者。
2. **首次唤醒（`c @ v1`）**：
   调用方通过 `@` 将实参装入槽位 `src_ptr` 并触发 `%resume(c, src_ptr)`：
   - 内核原子地将 `src_ptr` 指向的内容 `%move` 进协程 `c` 的 `\in` 槽（`src_ptr` 变为 `empty`）；
   - 将 `c` 的 `\^` 设为调用方协程 handle；
   - 挂起调用方，协程 `c` 从 `proto` 的第 0 行开始执行，直接通过 `$\in`（即 `$(%in)`）即可读取首次传入的 `v1`，**零预热（prime）、无首值丢失**。
3. **中途挂起与再次唤醒（`\^ @ reply`）**：
   协程 `c` 内部执行 `\^ @ reply` 时，将 `reply` 移入恢复者 `\^` 的 `\in` 并挂起自己；当未来某协程再次以 `c @ v2` 唤醒 `c` 时，原本挂起的 `%resume` 表达式求值完成并返回指向 `c` 自身 `\in` 槽的 `pointer`。
4. **根帧结束与终止**：
   当协程 `c` 的根帧正常求值完毕，或 `%break` 弹出至协程根帧时（见 [04-control-flow.md](04-control-flow.md)）：
   - 协程 `c` 状态标记为已终止（Dead）；
   - 内核将最终结果槽的内容 `%move` 入当前 `\^` 的 `\in` 槽，销毁 `c` 的根帧，并恢复 `\^` 执行；
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

- **调用帧（Per-call Frame）**：函数调用不是内核原语，而是 stdlib wrapper——每次调用通过 `block` 开辟独立的调用帧，在帧内分配参数 location 并用 `%write`（Copy）或 `%move` 填入实参，随后 `evaluate` 函数体 `prototype`。
- **零开销退化**：词法可证明不发生协程挂起（`%resume`）的子程序，编译器直接将其 `block` 帧退化为普通硬件栈帧压栈。
- **递归（`fix` 协议）**：因为 `%bind` 在 RHS 求值完成后才生效（[05-ownership.md](05-ownership.md)），对象不能按名字自引用未完成的自身；递归函数由 stdlib 的 `fix` 组合子在调用帧中动态注入 `self` 参数实现。

### 6.2 闭包捕获

内核不提供隐式捕获，由 stdlib 分两层承载：
1. **值与对象移动捕获**：闭包是普通对象 `{ env := {...}; code := ^(...) }`。编译期宏扫描自由符号，将外部 POD `value` 复制（或将 owned `location` 经 `%move` 转移）进 `env` 子对象；代码通过显式的 `env` 指针访问捕获字段。
2. **共享可变状态捕获（`cell` 协程）**：需要跨闭包共享可变状态时，使用 `new` 创建一个持有可变状态的 `cell` 协程（由 GC 管理生命周期），多个闭包持有该协程的 `handle` value，经 `@` 消息安全读写。

### 6.3 全局环境与模块导入

- **全局环境 = 编译期只读常量区（`frozen` 区）**。模块在编译期求值完毕并经 `%freeze` 固化为只读常量区对象进入二进制，全程序与多协程通过只读 `pointer` 共享同一实例，天然去重，不持任何可变状态。
- **静态导入**：`import` 是编译期元层 capability，返回模块 `prototype`，经编译期 `block` 求值与 `%freeze` 固化后得到指向只读模块对象的安全 `pointer`。

## 七、GC 边界

- **GC 仅管理 `new` 创建的协程对象**（独立生命周期、`handle` 可复制传递、可相互引用成环）。
- **`block` 与 `allocate` 产生的 `location` 全部走词法所有权树**，绝不进入 GC。
- 不调用 `new` 创建协程的程序**零 GC 负担**。

## 八、验收用例（自检清单）

1. `{ a := 1; b := 2 }` 展开为 `block ^(a := 1; b := 2)`，返回指向包含连续字段 `a`、`b` 的 `self root` location 的 `pointer`。
2. 同一 `{ ... }` 内连续执行两次 `evaluate`，通过 `%this` 共同向当前构建中的 `self root` 绑定字段，闭合后一次性压平为完整连续对象。
3. 递归函数通过 stdlib `fix` 协议实现，两个协程并发调用同一递归函数各自拥有独立的 `block` 调用帧，互不干扰。
4. `c := new proto` 惰性创建协程，首次 `c @ 10` 将 `10` 移入 `c` 的 `\in` 槽并从 `proto` 入口启动，无任何值丢失。
5. `cell` 协程通过 `\in` 与 `\^ @ ...` 实现多闭包交叉读写同一状态。
