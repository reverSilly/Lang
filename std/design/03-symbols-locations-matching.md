# 03 - 本体、符号、位置与模式匹配

前置：[01-comptime.md](01-comptime.md)、[02-evaluation.md](02-evaluation.md)
后续：[04-control-flow.md](04-control-flow.md)、[05-ownership.md](05-ownership.md)

## 一、核心原则：二元结果本体

求值结果在宇宙里只有两种基本类别（Kind）：**`value`** 与 **`location`**。

1. **`value`（纯值）**：
   - 永远不可变、无身份、无生命周期、无所有权、可自由按位复制（bit-copy）、可跨阶段固化。
   - **子类**：
     - 基础类型值（`integer`、`boolean`、`bits` 等）；
     - **`symbol`**：一等原子值（类似 Lisp symbol）。源码中的裸标识符 token `a` 即 `symbol` 字面量；可动态构造、可参与模式匹配，不是编译期专属概念；
     - **`prototype`**：`^Expr` 产生的纯代码值；
     - **`handle`**：帧 handle（`\this`）与协程 handle（`new` 返回值、`\^`）；
     - **`pointer`**：指向 `location` 的地址凭证（瘦指针与胖指针），不拥有目标、不保活；
     - **`frozen`**：编译期或受控冻结产生的只读对象快照；
     - **`fail`**：表示错误的普通值。
2. **`location`（存储位置 / 格子）**：
   - 由 `allocate <shape>` 或 `block proto` 产生，持有内容（叶子 `value` 或子 `location` 树）。
   - 具有唯一身份、所有权状态（`uninitialized` / `live` / `empty`）与固定契约（可选 `shape` 约束 + `mutability`）。
   - **`location` 绝不可作为 `value` 传递或隐式复制**。

一个 `block` 帧就是一个容器 `location`（`self root`），内部持有若干直接子 `location` 以及一张 `symbol → location` 的符号映射表。

## 二、组合式前缀记号（零模式切换）

语言采用严格自底向上的组合算子，任何标识符在任何位置含义恒定，不存在“左值模式/右值模式/模式匹配模式”的切换：

| 写法 | 运算分解 | 含义 | 结果类别 |
| :--- | :--- | :--- | :--- |
| `a` | 原子字面量 | `symbol` 值 `'a'` 本身 | `value` (`symbol`) |
| `\a` | `\` 作用于 `a` | 将 `symbol` 值沿当前帧解析为对应的 `location`（支持动态计算出的 symbol：`\(expr)`） | `location` |
| `$\a` | `$` 作用于 `\a` | 从叶子 `location` 读出其存放的 `value` 副本 | `value` |
| `\p.a` | `%field(\p, a)` | 访问 `location` `\p` 下名为 `a` 的子 `location` | `location` |
| `$\p.a` | `$` 作用于 `\p.a` | 读取子 `location` `\p.a` 的 `value` | `value` |

- **`\`（解析算子，`symbol → location`）**：在当前帧符号表中查找 `symbol` 对应的 `location`。
- **`$`（读取/兑现算子）**：
  - 作用于存储叶子的 `location`：读出其 `value`（含子 `location` 的复合对象不可用 `$` 整体读出为 `value`，只能按字段访问、移动或克隆）；
  - 作用于 `pointer` value（`$ptr`）：在当前求值环境现场兑现出被指向的 `location`（若要继续读值则写 `$($ptr)`）。

## 三、三大“内核 Location 原语 × 标准库分派器”对偶

为了保持编译器内核最小化，内核状态修改原语**只接受 `location`**；而面向用户的常用操作符（`:=`、`=`、`@`）全部是标准库提供的轻量 **Kind-Dispatcher**，负责按右操作数是 `location` 还是 `value` 进行分派：

| 领域 | 编译器内核原语（仅操作 `location`） | 标准库分派器（接受 `location \| value`） |
| :--- | :--- | :--- |
| **1. 命名绑定** | **`%bind(symbol, loc)`**：将当前 block 下尚未绑定的直接子 `loc` 挂入当前帧符号表，返回 `()` | **`x := rhs`**：<br>• 若 `rhs` 为当前块直接子 `location` → 直接 `%bind(x, rhs)`；<br>• 若 `rhs` 为 `value` → 在当前块 `allocate` 一个不可变空槽、拷贝填入 `rhs`、再 `%bind`（Auto-box）；<br>• 若 `rhs` 为只读借用 → 静态/动态报错 |
| **2. 写入与移动** | **`%move(dst_loc, src_loc)`**：将当前块所有权子树内的 `src_loc` 整棵移入 `dst_loc`，`src_loc` 置为 `empty`，返回 `()` | **`\dst = rhs`**：<br>• 若 `rhs` 为 `location` → 调用 `%move(\dst, rhs)`（所有权转移，不复制）；<br>• 若 `rhs` 为 `value` → 调用标准库 `copy`（基础类型调内建按位写，复合类型调派生或自定义 copy）写入 `\dst` |
| **3. 协程传递** | **`%resume(target, src_loc)`**：原子地将 `src_loc` `%move` 进 `target` 的 `\in`、设 `target` 的 `\^` 为本协程、挂起并切换；恢复后返回本协程的 `\in` | **`target @ rhs`**：<br>• 若 `rhs` 为 `location` → 直接 `%resume(target, rhs)`（跨协程移动子树）；<br>• 若 `rhs` 为 `value` → 装入临时 location 后调用 `%resume(target, temp_loc)`；<br>• 均返回本协程醒来时的 `\in` location |

这一对偶带来的核心性质：
- **字面区分 Copy 与 Move**：`\b = $\a` 显式用 `$` 读出了 `value`，因此是 **Copy**（`\a` 保持 `live`）；`\b = \a` 未写 `$`，右操作数是 `location`，因此是 **Move**（`\a` 变 `empty`）。全语言无需单独的 `move` 关键字。
- **绑定结果一致性**：由于新建 `location`（无论是 `allocate` 还是 `block`）默认契约均为 `immutable`（允许 `uninitialized → live` 初始化一次，见 [05-ownership.md](05-ownership.md)），`x := 1`（走 value auto-box）与 `p := { a := 1 }`（走 location 直接 `%bind`）得到的 `x` 与 `p` **全部是一致的 `live` + `immutable` location**。

## 四、编译器内建原语最小集

判据：**凡是需要触碰语言私有结构（AST 延迟、帧树、对象内存布局、所有权状态机、协程栈）的必须入核；凡是只操作值的全部下放标准库。**

| 分类 | 内核原语 | 语义简述 |
| :--- | :--- | :--- |
| **代码延迟** | `^Expr` | 唯一特殊形式：将 `Expr` 包装为 `prototype` 值 |
| **环境求值** | `evaluate` / `block` / `new` | 在当前帧 / 新子帧 / 新协程根帧中安置或求值 `prototype` |
| **位置分配** | `allocate [mutable] <shape>` | 在当前 block 下分配直接子 `location`（初始处于 `uninitialized` 状态） |
| **帧树导航** | `\` / `%field` / `%parent` | 当前帧 symbol 解析 / 子字段 location 访问 / 单步父帧导航 |
| **位置读写** | `$` / 基础类型 copy | 从叶子 location 读 value（或兑现指针）/ 向基础类型槽写入 bits |
| **所有权演进** | `%bind` / `%move` / `%drop` | 符号表命名 / 子树所有权转移 / 递归析构并置 `empty` |
| **控制与调度** | `break` / `%branch` / `%resume` | 非局部弹帧写结果 / 纯值二路透明分支 / 跨协程原子传槽与切换 |
| **基础比较** | 基础类型 bits 相等 | 每种 builtin 基础类型的按位相等原语，构成一切相等与匹配的底座 |

## 五、模式匹配（标准库纯函数）

**编译器内核不认识 Pattern。** 模式匹配完全由标准库纯函数与两个语法壳构成：

```
match(pattern, subject) -> Bindings | fail
```

### 5.1 语法壳与提交流程

- **`value : pattern`（可反驳约束）**：
  1. 求值 `value` 与 `pattern`（两者都是普通表达式，无特殊求值模式）；
  2. 调用纯函数 `match(pattern, value)`；
  3. 若返回 `fail`：立即执行 `break <fail>` 终止当前 block（见 [04-control-flow.md](04-control-flow.md)）；
  4. 若返回 `Bindings`（即 `(symbol, value)` 列表）：标准库提交循环依次在当前 block 执行 `allocate` + copy + `%bind`，将所有绑定落入当前帧。匹配执行期间新绑定尚不可见。
- **`v ? pattern`（纯查询匹配）**：
  调用 `match(pattern, v)`，成功返回 `true`，返回 `fail` 则返回 `false`；丢弃 `Bindings`，不产生任何绑定或 `break` 控制效应。

### 5.2 Pattern 的自然消解与可扩展性

由于 `pattern` 是普通值表达式，`match` 根据 `pattern` 值的形状分派：
- **裸 `symbol` 值 `a`**：作为绑定目标，匹配任意 `subject` 并产出绑定对 `(a, subject)`；
- **普通 `value`（如字面量 `3` 或变量读取 `$\a`）**：按值相等（`eq`）匹配——不需要内置 `Exactly` 关键字，在 pattern 位置写 `$\a` 天然就是按值相等匹配；
- **带 `matcher` 协议的自定义 Pattern 值**：调用 `pattern.matcher(subject)`。标准库借此实现：
  - 形状/类型匹配（编译期反射生成字段检验逻辑）；
  - 选择 `p1 | p2`、合取 `p1 & p2`、守卫 `If(pred)`；
  - 可逆算术模式（如 `a + 1 : 3` 由 `+` 注册的逆运算求得 `a = 2`，内核无任何算术白名单）；
  - 对象与序列解构构造器（如 `ObjPattern(...)`，源码中 `{...}` 永远只表示 `block`，不复用于 pattern 字面量）。
- **安全边界**：造绑定的唯一入口 `%bind` 锁在内核手中；自定义 matcher 只能返回纯数据 `Bindings | fail`，保证了任何第三方 pattern 都具有局部可推理性。

## 六、指针与只读借用

指针是 `value`（可复制、可存储、可经 `@` 跨协程发送），指向 `location` 但自身不拥有目标。

| 类型 | 内容构成 | 指向目标 | 可变性呈现 |
| :--- | :--- | :--- | :--- |
| **瘦指针（Thin Pointer）** | 仅内存地址 | 形状静态已知的 `location` | 遵循原 `location` 契约 |
| **胖指针（Fat Pointer）** | 地址 + 动态形状/分派信息 + 可变性掩码 | 长度或形状动态的对象（slice、装箱对象、帧借用） | **可叠加 `immutable` 掩码（只读借用）** |

- **受控只读借用（Safe Subset）**：通过帧 handle（如 `\this`、`\^`）获取的访问默认表现为叠加了 `immutable` 掩码的临时胖指针（使整条访问路径呈现深度只读），且受栈帧结构约束不得存储、捕获或返回。
- **裸指针（Unsafe）**：显式取址获得的指针不检查目标生命周期；一旦目标 `location` 被 `%move` 或 `%drop`，解引用失效指针属于未定义行为（UB）。安全子集 = owned `location` + 帧 handle 临时只读访问 + `cell` 协程。

## 七、验收用例（自检清单）

1. `a := 3; $\a + 1`：`:=` 将 `3` auto-box 入不可变 location 并绑定到 `a`，`$\a + 1` 得 `4`。
2. `x := allocate integer; \x = 1`：`x` 经历 `uninitialized → live` 单次初始化成功；若再次执行 `\x = 2` 则因 `x` 默认不可变而报错。
3. `a := allocate integer; \a = 1; b := allocate integer; \b = \a`：`\b = \a` 触发 `%move`，`\b` 得 `1`，`\a` 变为 `empty`。
4. `a + 1 : 3`：通过 stdlib 可逆协议匹配成功，在当前块将 `a` 绑定到 `2`；`3 : 4` 匹配失败并 `break` 当前 block 返回 `fail`。
5. `$(\a) : 3`：因为左侧求值为普通 value，直接按值相等匹配，不产生新绑定。
