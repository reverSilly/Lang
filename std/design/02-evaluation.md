# 02 - 求值模型、块与协程

前置：[01-comptime.md](01-comptime.md)
后续：[03-symbols-locations-matching.md](03-symbols-locations-matching.md)、[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)、[06-stdlib-foundations.md](06-stdlib-foundations.md)、[07-types-and-intrinsics.md](07-types-and-intrinsics.md)、[08-syntax.md](08-syntax.md)

## 一、核心原则

语言在内核层面彻底解耦 **“延迟代码（`Prototype`）”** 与 **“环境/栈帧分配（`#block` / `#new`）”**，且**一切表达式求值结果皆为 `value`（对 `location` 的引用统一表示为普通的胖指针 `Pointer` value）**：

1. **`^Expr`（Prototype）是全语言唯一拦截求值的语法特殊形式**：它把未求值的表达式 `Expr` 包装成一个纯代码值（`Prototype` value），不携带任何环境或 AST 包袱。
2. **求值原语全是接收 `Prototype` 值的普通内置调用**：环境永远由触发求值的原语提供——在当前环境就地展开用 `#evaluate(p)`，开辟子帧求值用 `#block(p, is_mut)`，开辟独立协程求值用 `#new(p)`。
3. **内核零高层概念**：内核不存在独立的“函数定义”、不认识 `__destroy` 钩子、不处理 RAII、不检查 POD、也不内置高层算术类型；完整 Core 原语签名总表见 [03-symbols-locations-matching.md](03-symbols-locations-matching.md) §3。

## 二、`#evaluate` 与 `#block` 的正交分工

### 2.1 为什么解耦 `#evaluate` 与 `#block`

- **`#evaluate(proto)`（就地求值 / 分派器与内建算术引擎）**：
  不创建新帧，也不隔离符号。`proto` 内部的 `#allocate`、`#allocate_val`、`#bind`、`#drop` 和 `#field(#this, ...)` 直接作用于调用方的当前 `self root`。这不仅支持多个 `Prototype` 在同一个正在构建的 `block` 内顺序组合完成分段初始化，更让标准库的 `:=`、`=`、`if`、`while`、`defer`、`break` 以及 `capabilities.intrinsics` 生成的算术 `Prototype` 能够直接在调用方当前帧就地生效：
  ```
  obj := {
      #evaluate($.init_part_a);
      #evaluate($.init_part_b)
  }
  ```
- **`#block(proto, is_mut)`（子帧隔离 / 连续内存对象构造与就地构造）**：
  - 显式双参，与 `#allocate(s, is_mut)`、`#allocate_val(v, is_mut)` 完全对称（`is_mut` 为 `bytes(1, 1)`，`0x00` 不可变、非零可变）。无论是正常闭合还是经 `#break(val)` 退出，都在父帧中物化出未绑定 `live` 直接子槽 `self root`（其 `mutability` 永久定格为 `is_mut`，`#bind` 不改变可变性）并返回指向它的胖指针 `Pointer`。
  - 只要定义方希望词法隔离、函数调用或构造独立对象，都统一使用 `#block`（标准库衍生出保留字段的对象构造块 `{ ... }` 与自动清理局部变量的求值块 `block1` / 函数调用 `ApplyExpr`，见 [06-stdlib-foundations.md](06-stdlib-foundations.md)）。
  - **就地构造（In-place Construct / RVO）**：当 `#block(proto, is_mut)` 作为初始化表达式（如 `x := { ... }`、`b := move(.a)` 或向空槽构造）时，编译器后端可直接将父块的目标槽位作为该 `block` 的 `self root` 原地构造，消除中间临时槽拷贝与移动开销。

### 2.2 严禁显式 `with <ptr> #evaluate`

语言**不提供**将外部已存在的 `location` 指针重新作为当前 `#this` 环境注入 `#evaluate` 的原语：
- 一个 `location` 只有在作为 `#block` 的 `self root` 处于栈顶构建期（Constructing）时，其内部字段集合才允许随 `#bind` 和 `#drop` 动态演进；
- 一旦 `block` 闭合，所有 `live` 字段被压平为固定偏移量的连续内存，其 `Shape` 彻底凝固且所有字段恒为 `live`（见 [05-ownership.md](05-ownership.md)）；
- 拒绝 `with <ptr> #evaluate` 从根本上保证了：**已闭合对象的固定 `Shape` 与连续内存布局绝不会被二次拆开追加字段，且构建了一半的 location 绝不会提前暴露给父作用域**。

## 三、构建栈与逐步递归上溯导航

所有求值发生在当前协程的构建栈（Construction Stack）与帧树上：
- **通用 `Pointer` 不存储 `parent`**：普通胖指针固定为 `{ addr, shape_ptr, mutability, __ptr_tag }`，不包含父指针；**唯有当前协程构建栈上尚未闭合的 `block` 才具有父子链关系**（由协程运行时维护）。
- **内核只提供确定性的单步导航原语（全部返回普通 `Pointer` value）**：
  - 向下访问子字段：`#field(ptr, symbol)`（标准库注册为中缀 `ptr.symbol` 与当前帧前缀 `.symbol` ≡ `#field(#this, symbol)`）；
  - 向上访问任意层祖先构建帧：**一元原语 `#parent(ptr)`**（接收任意活跃 OpenFrame 指针，返回其在构建栈中的直接父构建帧指针；对根帧、普通数据指针或已闭合对象返回 `null` 指针）。
- **跨多层构建帧的直接链式上溯（零槽位开销）**：
  - 向上走 1 步：`#parent(#this)`；
  - 向上走 2 步：`#parent(#parent(#this))`；
  - 向上走 $k$ 步：直接嵌套 $k$ 次 `#parent(...)`，无需在帧内额外绑定或 `#drop` 隐式 `__parent` 槽位，闭合时天然零内存开销。
- **协程树隔离**：`#new(proto)` 另起一棵独立的协程构建栈，协程根帧的 `#parent(root)` 返回 `null`，不会跨越协程边界访问创建者的栈帧。

## 四、协程状态机：`#new`、`#resume` 与 `#caller` / `#in`

### 4.1 为什么 `#resume` 必须是内核原子原语

将“写入 `#in` + 更新 `#caller` + 切换协程控制流”合为单一内核原子原语 `#resume(target, val)`，是为了坚守三条内核安全不变量：
1. **跨协程帧树隔离**：每个协程的帧树只归该协程自己所有，严禁外部协程通过普通指针直接改写目标协程内部的 location；
2. **恢复者身份 `#caller` 不可伪造**：`#caller` 是代表“真正挂起自身并将控制权移交给本协程的恢复者”的内核凭证，不可在切换之外被外部篡改；
3. **根帧终止焊接**：当协程根帧求值结束或 `#break(val)` 弹出至根帧时，内核必须自动将最终结果 `val` 装入 `#caller` 的 `#in` 槽并恢复 `#caller`。

### 4.2 惰性创建与统一来信槽 `#in`

1. **创建（`c := #new(proto)`）**：
   `#new` 是**惰性（Lazy）**的——仅分配新协程的根帧（含初始处于 `empty` 状态的来信槽 `#in`），将指令指针停在 `proto` 入口（PC=0），处于挂起态（Suspended），并立即返回协程 `Handle` 值。创建过程不执行 `proto` 的任何代码，不挂起创建者。
2. **首次唤醒（`c @ v1`）**：
   调用方通过标准库 `@`（经 POD Gate 检验，或接收 `move` 转移槽提取出 `val`）触发 `#resume(c, v1)`：
   - 内核原子地将 `v1` 装入协程 `c` 的 `#in` 槽；
   - 将 `c` 的 `#caller` 设为调用方协程 `Handle`；
   - 挂起调用方，协程 `c` 从 `proto` 的第 0 行开始执行，直接通过 `$#in` 即可读取首次传入的 `v1`，**零预热（prime）、无首值丢失**。
3. **中途挂起与再次唤醒（`#caller @ reply`）**：
   协程 `c` 内部执行 `#caller @ reply` 时，将 `reply` 装入恢复者 `#caller` 的 `#in` 并挂起自己；当未来某协程再次以 `c @ v2` 唤醒 `c` 时，原本挂起的 `#resume` 表达式求值完成并返回指向 `c` 自身 `#in` 槽的 `Pointer`（若旧 `#in` 槽含非 POD 资源，由标准库在下次挂起或协程结束时负责 `destroy`）。
4. **根帧结束与终止**：
   当协程 `c` 的根帧正常求值完毕，或 `#break(val)` 弹出至协程根帧时（见 [04-control-flow.md](04-control-flow.md)）：
   - 协程 `c` 状态标记为已终止（Dead）；
   - 内核将最终结果装入当前 `#caller` 的 `#in` 槽，释放 `c` 的根帧，并恢复 `#caller` 执行；
   - 对已终止（Dead）或正处于运行链上（Running）的协程执行 `#resume` 是运行时错误（trap）。

```
! 示例 1：单次计算协程（零预热直接读 #in） !
add_one := #new(^{ break ($#in + 1) });
res := $($.add_one @ 41); ! res 为 42 !

! 示例 2：生成器（Generator） !
gen := #new(^{
    #caller @ 1;
    #caller @ 2;
    break 3
});
a := $($.gen @ ()); ! 1 !
b := $($.gen @ ()); ! 2 !
c := $($.gen @ ()); ! 3 !
```

## 五、GC 边界

- **GC 仅管理 `#new` 创建的协程对象**（独立生命周期、`Handle` 可复制传递、可相互引用成环）。
- **`#block`、`#allocate` 与 `#allocate_val` 产生的 `location` 全部走词法所有权树**，绝不进入 GC。
- 不调用 `#new` 创建协程的程序**零 GC 负担**。

## 六、验收用例（自检清单）

1. `{ a := 1; b := 2 }` 展开后通过 `#block(p, false)` 返回指向父帧中匿名 `self root` location（含连续字段 `a`、`b`，`mutability = 0`）的未绑定 `Pointer`（块内产生的未绑定临时槽由标准库按 `FieldMap` 逆序 `drop`，隐式绑定的 `__parent` 在闭合前由 `#drop` 删除槽与 bind，最终对象零额外字节开销）；空块 `{}` 展开为 `#block(p_empty, false)` 返回空对象未绑定 `Pointer`。
2. 同一 `{ ... }` 内连续执行两次 `#evaluate`，通过 `#this` 共同向当前构建中的 `self root` 绑定字段，闭合后一次性压平为完整连续对象。
3. 在嵌套块内，`#parent(#this)` 指向直接外层构建块，多层嵌套块可通过 `#parent(#parent(#this))` 直接逐级上溯访问任意祖先帧，且透明 `if`（`#branch`）不产生额外帧层。
4. `c := #new(proto)` 惰性创建协程，首次 `$.c @ 10` 直接经 `#resume($.c, 10)` 将 `10` 装入 `c` 的 `#in` 槽并从 `proto` 入口启动，无任何值丢失。
