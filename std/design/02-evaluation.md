# 02 - 求值模型、块与协程

前置：[01-comptime.md](01-comptime.md)
后续：[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)

## 一、核心原则

语言在内核层面彻底解耦 **“延迟代码（`prototype`）”** 与 **“环境/栈帧分配（`block` / `new`）”**，且**一切表达式求值结果皆为 `value`（对 `location` 的引用统一表示为普通的胖指针 `pointer` value）**：

1. **`^Expr`（Prototype）是全语言唯一拦截求值的语法特殊形式（Special Form）**：它把未求值的表达式 `Expr` 包装成一个不透明的纯代码值（`prototype` value），不携带任何环境或 AST 包袱。
2. **求值原语全是接收 `prototype` 值的普通操作符**：环境永远由触发求值的操作符提供——在当前环境就地展开用 `evaluate`，开辟子帧求值用 `block`，开辟独立协程求值用 `new`。
3. **内核零高层概念**：内核不存在独立的“函数定义”、不认识 `\destroy` 钩子、不处理 RAII；标准库基础包装器（`{...}`、`block1`、`:=`、`=`、`$`、`@`、`destroy`、`drop`、`move`、`swap`）在 [06-stdlib-foundations.md](06-stdlib-foundations.md) 中由这组原语导出（注：基础算术与类型操作的内建原语 vs `compile` capability 设计、函数调用传参协议、Lambda 表面语法与闭包捕获细则列于 `06` 末节待办）。

## 二、求值原语与特殊内建值

### 2.1 核心求值原语

| 机制 | 类别 | 语义 | 返回结果（皆为 `value`） |
| :--- | :--- | :--- | :--- |
| `^Expr` | 唯一特殊形式 | 取 `Expr` 的延迟形态，不求值 `Expr` | `prototype` |
| `a ; b` | 内核序列原语 | 先求值 `a` 并直接丢弃其结果 `value`（不触发任何析构），再求值并返回 `b` | 透传 `b` 的结果 `value` |
| `evaluate proto` | 内核求值原语 | 在**当前环境（current env）**直接执行 `proto`；不新建 `self root`、不切换 env | 透传 `proto` 的结果 `value` |
| `block proto` | 内核建帧原语 | 在当前 block 下隐式分配匿名 `self root` location 作为**新子 env**，执行 `proto`；闭合时直接丢弃未 `%bind` 槽及处于 `empty`/`uninitialized` 的槽，仅将 `live` 绑定字段压平为连续内存并锁定 `shape` | 指向该 `self root` location 的 `pointer` |
| `new proto` | 内核协程原语 | **惰性（Lazy）**创建独立协程根帧（初始停在 `proto` 入口 PC=0，不执行代码） | 协程 `handle` |
| `%resume(target, src_ptr)` | 内核切换原语 | 原子地将 `src_ptr` 指向的直接子槽内容移入 `target` 的 `%in` 槽（`src_ptr` 置 `empty`）、将 `target` 的 `\^` 设为本协程、挂起自身并恢复 `target` | 本协程下次被唤醒时指向自身 `%in` 槽的 `pointer` |
| `{ Expr }` | 标准库语法糖 | 展开为带隐式父帧链与局部守卫清理的 `block ^(Expr)`；**空块 `{}` 直接展开为 `block ^()`**（返回空的闭合对象指针） | 同 `block`（返回 `pointer`） |
| `target @ rhs` | 标准库分派器 | 将 `rhs` 装入或移入当前块临时直接子槽 `src_ptr`，调用 `%resume(target, src_ptr)`（详见 `06` §2.2） | 指向本协程 `%in` 槽的 `pointer` |

### 2.2 四个核心环境标识原语（皆为零参原语）

| 原语 / 记号 | 结果类型 | 含义 |
| :--- | :--- | :--- |
| `%this`（记作 `\this`） | `pointer` | 指向当前 `block` 的 `self root` location 的普通胖指针（全语言**无独立的“帧指针/帧 handle”类型**，`\this` 即普通 `pointer`；当前帧符号解析 `\sym` 即 `%field(%this, sym)` 的语法糖） |
| `%parent` | `pointer` | 零参原语，返回指向当前栈顶构建帧 `%this` 在协程构建栈（Construction Stack）上的直接父构建帧指针（通用 `pointer` 不存储 parent，唯独构建栈上未闭合的 `block` 具有父子链） |
| `%caller`（记作 `\^`） | `handle` | 指向最近一次唤醒本协程的那个协程的句柄（由 `%resume` 原子更新；`handle` 唯一专指协程句柄） |
| `%in`（记作 `\in`） | `pointer` | 指向本协程根帧持有的来信槽 location 的普通胖指针，存放最近一次唤醒本协程时由对方移入的内容 |

## 三、`evaluate` 与 `block` 的正交分工

### 3.1 为什么解耦 `evaluate` 与 `block`

- **`evaluate proto`（就地求值 / 分派器与 Mixin 引擎）**：
  不创建新帧，也不隔离符号。`proto` 内部的 `allocate`、`%bind`、`%drop` 和 `%field(%this, ...)` 直接作用于调用方的当前 `self root`。这不仅支持多个 `prototype` 在同一个正在构建的 `block` 内顺序组合完成分段初始化，更让标准库的 `:=`、`box`、`if`、`while`、`defer`、`break` 等包装器能够直接在调用方当前 `block` 内就地生效：
  ```
  obj := {
      evaluate $\init_part_a;
      evaluate $\init_part_b
  }
  ```
- **`block proto`（子帧隔离 / 连续内存对象构造与就地构造）**：
  - 只要定义方希望词法隔离或构造一个独立对象，就使用 `block`（标准库衍生出保留字段的对象构造块 `{ ... }` 与自动清理局部变量的求值块 `block1`，见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）。
  - **就地构造（In-place Construct / RVO）**：当 `block proto` 作为初始化表达式（如 `x := { ... }` 或向已置空的槽位构造）时，编译期 AST 重写层或编译器后端可直接将父块的目标槽位作为该 `block` 的 `self root` 原地构造，消除中间临时槽拷贝与移动开销。

### 3.2 严禁显式 `with <ptr> evaluate`

语言**不提供**将外部已存在的 `location` 指针重新作为当前 `%this` 环境注入 `evaluate` 的原语：
- 一个 `location` 只有在作为 `block` 的 `self root` 处于栈顶构建期（Constructing）时，其内部 `shape` 才允许随 `%bind` 和 `%drop` 动态演进；
- 一旦 `block` 闭合，所有 `live` 字段被压平为固定偏移量的连续内存，其 `shape` 彻底凝固且所有字段恒为 `live`（见 [05-ownership.md](05-ownership.md)）；
- 拒绝 `with <ptr> evaluate` 从根本上保证了：**已闭合对象的固定 `shape` 与连续内存布局绝不会被二次拆开追加字段，且构建了一半的 location 绝不会提前暴露给父作用域**。

## 四、构建栈与逐步递归上溯导航

所有求值发生在当前协程的构建栈（Construction Stack）与帧树上：
- **通用 `pointer` 不存储 `parent`**：普通胖指针固定为 `(addr, shape_ptr, mutability)`，不包含父指针；**唯有当前协程构建栈上尚未闭合的 `block` 才具有父子链关系**（由协程运行时维护）。
- **内核只提供确定性的单步导航原语（全部返回普通 `pointer` value）**：
  - 向下访问子字段：`%field(ptr, symbol)`（语法记作 `ptr.symbol`；当前帧 `\sym` 即 `%field(%this, sym)`；若字段不存在或处于构建期 `empty`/`uninitialized`，返回 `null` 指针）；
  - 向上访问直接父构建帧：**零参原语 `%parent`**（仅返回当前栈顶构建帧 `%this` 在协程构建栈中的直接父构建帧指针）。
- **跨多层构建帧的逐步递归上溯（零闭合内存开销）**：
  - 每个由标准库 `{ ... }`（或函数/循环块）开启的构建帧，在进入块时自动绑定一个指向直接父帧的隐式父链槽位：`__parent := %parent`；
  - 向上走 1 步：`%parent`（等价于 `$(\__parent)`）；
  - 向上走 2 步：`$($(\__parent).__parent)`；
  - 向上走 $k$ 步：编译期宏自动展开为 $k$ 次 `%field` + `%deref` 链式寻址；
  - **闭合时零内存开销**：当该 `block` 准备闭合为对象（或通过 `break` 退出）时，标准库包裹层在末尾调用 `%drop(\__parent)` 将其置为 `empty`。根据闭合相变规则，处于 `empty` 的 `__parent` 绑定在闭合时被**自动静默剥离**，最终闭合出的连续内存对象中不占 1 个字节！
- **协程树隔离**：`new proto` 另起一棵独立的协程构建栈，运行期不允许通过 `%parent` 跨越协程根帧访问创建者的运行期栈帧（静态拒绝）。

## 五、协程状态机：`new`、`%resume` 与 `\^` / `\in`

### 5.1 为什么 `%resume` 必须是内核原子原语

将“移入 `\in` + 更新 `\^` + 切换协程控制流”合为单一内核原子原语 `%resume(target, src_ptr)`，是为了坚守三条内核安全不变量：
1. **跨协程帧树隔离**：每个协程的帧树只归该协程自己所有，严禁外部协程通过普通指针直接改写目标协程内部的 location；
2. **恢复者身份 `\^` 不可伪造**：`\^` 是代表“真正挂起自身并将控制权移交给本协程的恢复者”的内核凭证，不可在切换之外被外部篡改；
3. **根帧终止焊接**：当协程根帧求值结束或 `%break` 弹出至根帧时，内核必须自动将最终结果移入 `\^` 的 `\in` 槽并恢复 `\^`。

### 5.2 惰性创建与统一来信槽 `\in`

1. **创建（`c := new proto`）**：
   `new` 是**惰性（Lazy）**的——仅分配新协程的根帧（含初始处于 `empty` 状态的来信槽 `\in`），将指令指针停在 `proto` 入口（PC=0），处于挂起态（Suspended），并立即返回协程 `handle` 值。创建过程不执行 `proto` 的任何代码，不挂起创建者。
2. **首次唤醒（`c @ v1`）**：
   调用方通过标准库 `@` 将实参装入当前块直接子槽 `src_ptr` 并触发 `%resume(c, src_ptr)`：
   - 内核原子地将 `src_ptr` 的内容移入协程 `c` 的 `\in` 槽（并将调用方的 `src_ptr` 置为 `empty`）；
   - 将 `c` 的 `\^` 设为调用方协程 `handle`；
   - 挂起调用方，协程 `c` 从 `proto` 的第 0 行开始执行，直接通过 `$\in`（即 `$(%in)`）即可读取首次传入的 `v1`，**零预热（prime）、无首值丢失**。
3. **中途挂起与再次唤醒（`\^ @ reply`）**：
   协程 `c` 内部执行 `\^ @ reply` 时，将 `reply` 移入恢复者 `\^` 的 `\in` 并挂起自己；当未来某协程再次以 `c @ v2` 唤醒 `c` 时，原本挂起的 `%resume` 表达式求值完成并返回指向 `c` 自身 `\in` 槽的 `pointer`。
4. **根帧结束与终止**：
   当协程 `c` 的根帧正常求值完毕，或 `%break(src_ptr)` 弹出至协程根帧时（见 [04-control-flow.md](04-control-flow.md)）：
   - 协程 `c` 状态标记为已终止（Dead）；
   - 内核将最终结果移入当前 `\^` 的 `\in` 槽，释放 `c` 的根帧，并恢复 `\^` 执行；
   - 对已终止（Dead）或正处于运行链上（Running）的协程执行 `%resume` 是运行时错误（trap）。

```
!- 示例 1：单次计算协程（零预热直接读 \in） -!
add_one := new ^{ break ($\in + 1) };
res := $(add_one @ 41); !- res 为 42 -!

!- 示例 2：生成器（Generator） -!
gen := new ^{
    \^ @ 1;
    \^ @ 2;
    break 3
};
a := $(gen @ ()); !- 1 -!
b := $(gen @ ()); !- 2 -!
c := $(gen @ ()); !- 3 -!
```

## 六、GC 边界

- **GC 仅管理 `new` 创建的协程对象**（独立生命周期、`handle` 可复制传递、可相互引用成环）。
- **`block` 与 `allocate` 产生的 `location` 全部走词法所有权树**，绝不进入 GC。
- 不调用 `new` 创建协程的程序**零 GC 负担**。

## 七、验收用例（自检清单）

1. `{ a := 1; b := 2 }` 展开后通过 `block` 返回指向包含连续字段 `a`、`b` 的 `self root` location 的 `pointer`（块内隐式绑定的 `__parent` 在闭合前被 `%drop` 置空并自动剥离，最终对象零额外字节开销）；空块 `{}` 展开为 `block ^()` 返回空对象指针。
2. 同一 `{ ... }` 内连续执行两次 `evaluate`，通过 `%this` 共同向当前构建中的 `self root` 绑定字段，闭合后一次性压平为完整连续对象。
3. 在嵌套块内，零参 `%parent` 指向直接外层构建块，多层嵌套块（如双层 `while` 循环）通过 `$($(\__parent).__parent)` 逐步递归上溯访问外层函数帧，且透明 `if`（`%branch`）不产生额外帧层。
4. `c := new proto` 惰性创建协程，首次 `c @ 10` 将 `10` 移入 `c` 的 `\in` 槽并从 `proto` 入口启动，无任何值丢失。
