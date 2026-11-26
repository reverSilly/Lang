# Lang Core Specification (Frozen Phase 1 Source of Truth)

> **状态**：冻结（经 C++23 内核源码逐行审计对齐）。
> 本文档是 Lang Core（Layer 0 语法、Standalone AST、Runtime 内存与两态槽模型、21 个 Builtin、Capabilities 与 FFI ABI、CLI/打包契约）的**唯一事实来源**。旧探索稿已归档至 `std/design/archive/`，后续变更一律通过 1 页 RFC/ADR 提案推进。

---

## 1. Layer 0 词法与语法（`compiler/parser/Layer0.cpp`、`Peg.h`）

### 1.1 硬编码符号与词法规则
- **仅 5 组硬编码符号**：
  1. `\`：`Escaped := '\' any_byte()`（反转义为字面字节）。
  2. `! ... !`：`Comment`（注释内部复用 `Escaped`，任意 `\x` 成对消费，支持 `\!`）。
  3. `#`：`Builtin := '#' Word ( _ BuiltinArgs )?`（`#name` 与 `(` 之间允许空白与注释）。
  4. `( , )`：`Group := '(' _ Expr _ ')'`、`BuiltinArgs := '(' _ Expr ( _ ',' _ Expr )* _ ')'`。
  5. `;`：`Seq := Infix ( _ ';' _ Infix )*`。
- **黑名单 `Word` 字符集**：`Delimiter` 仅含 13 个字节（` ` `\t` `\r` `\n` `\f` `\v` `!` `#` `(` `)` `,` `;` `\`）；其余任意字节（含 `+ - * / . $ ^ { } " :` 等）及 `Escaped` 均构成 `Word`。零保留关键字，`Word` 延迟到求值期由 `RuntimeContext::symbols` 驻留。
- **严格非空约束**：空输入、空括号 `()`、空参数列表 `#foo()`、尾随分号 `a ;` 均为语法错误；`(Expr)` 单子节点自动透传，不产生额外 AST 节点。
- **扩展骨架**：Layer 0 预埋 16 条 PEG 规则骨架（`Program -> Expr -> Seq -> Infix -> Prefix -> Postfix -> Atom` 与 `PostfixOp`），支持水平 `prepend_choice`/`append_choice`、垂直 `wrap_rule` 与后缀左折叠 `register_postfix`。
- **PEG 引擎保护**：Packrat 记忆化 + 左递归检测（`in_progress` 抛 `ParseError`）+ `RepeatExpr` 零宽防死循环中断 + `lookahead_depth_` 屏蔽前瞻内部错误 + `CharSet`/`Fail`/`Filter` 原语。

---

## 2. Standalone AST 与跨阶段序列化（`compiler/ast/Ast.h`、`AstCodec.cpp`）

### 2.1 4 种 Core IR 节点
| AST 节点 | 语义 |
| :--- | :--- |
| `WordExpr { text }` | 求值为驻留后的 `Symbol` 值 |
| `BuiltinExpr { kind, args }` | 调用 21 个 Core Builtin 之一（参数左到右求值） |
| `SequenceExpr { items }` | 在当前帧顺序求值，返回最后一个表达式的值 |
| `ValueExpr { value: AstValue }` | 求值为经 `intern_ast_value` 驻留后的运行时 `Value` |

### 2.2 8 种可跨阶段 `AstValue` 变体
`RawBytes`、`SymbolVal`、`ShapeVal`、`AstProtoVal`、`IntrinsicVal`、`FfiExternVal`、`FfiBindPtrVal`、`StructVal`。

### 2.3 跨阶段边界（`reify_ast_value` / `intern_ast_value`）
- **三类 Arena 驻留物同构跨阶段**：
  - `Symbol`（4B）→ `SymbolVal` → 目标 `SymbolPool` 重新驻留。
  - `Prototype`（8B，`AstProto`/`IntrinsicProto`/`FfiProto`/`FfiBindPtrProto`）→ 对应 `AstValue` → 目标 `PrototypeArena` 重新驻留。
  - `Pointer(Shape)`（24B，满足 `shape_ptr == shape_meta && mutability == 0 && ShapeArena::owns(addr)`）→ 递归 `reify_shape` 展开为 `ShapeVal` → 目标端自底向上 `intern_shape` 进目标 `ShapeArena`，还原为只读 `Pointer(Shape)`。
- **严格拒绝嵌入 Standalone AST**：栈/帧 `Pointer`、解引用后的 40B `ShapeLayout` 值、`Handle`、`NativeProto`、`BoundFfiProto`。
- **二进制线格式**：12 字节定长头（8B magic `"LANGAST\0"` + 4B 小端版本号 `v1`）+ `cereal::PortableBinaryArchive` 载荷（保持 `shared_ptr` DAG 共享，严格拒绝空根与尾随字节）。

---

## 3. 内存模型、透明布局与两态槽（`Shape.h`、`FieldMap.h`、`FrameStack.h`）

### 3.1 零 Header 存储与自描述透明布局
所有 `Value` 与 `FrameSlot::storage` 均无 Header，按字段对齐紧致排布。内置透明布局定长不变量：
- **`PointerLayout`（24B / align 8）**：`{ addr: raw8 @0, shape_ptr: raw8 @8, mutability: raw1 @16, __ptr_tag: {} }`（7B padding）。
- **`FieldEntry`（32B / align 8）**：`{ sym: Symbol(4B) @0, state: raw1 @4, ptr: PointerLayout(24B) @8 }`。
- **`FieldMapInline`（24B / align 8）**：`{ entries_ptr: raw8, count: raw4, bucket_mask: raw4, buckets_ptr: raw8, __field_map_tag: {} }`。
  - **两层设计**：`entries` 数组保存全部现有槽（`Uninitialized` 与 `Live`，以及开放帧中 `sym == 0` 的未绑定槽与末尾 `active_child` 匿名槽）；开地址哈希表 `buckets`（`count > 8` 时启用）索引**全部已绑定槽（`sym != 0`，含 `Uninitialized`）**。
- **`ShapeLayout`（40B / align 8）**：`{ size: raw8 @0, align: raw8 @8, fields: FieldMapInline(24B) @16, __shape_tag: {} }`。
  - 语言层面统一以驻留只读 **`Pointer(Shape)`** 传递，`s1.addr == s2.addr` 即 O(1) 结构判等；访问元数据走普通 `#field`（如 `s.size`、`s.align`、`s.fields`）。
- **其他内置形状**：`unit_shape`（0B/1）、`raw1/2/4/8_shape`、`symbol_shape`（4B/4，`{ __raw, __sym_tag }`）、`prototype_shape`（8B/8，`{ __raw, __proto_tag }`）、`handle_shape`（8B/8，`{ __raw, __handle_tag }`）。

### 3.2 两态槽生命周期（无 `Empty` 僵尸态）
槽仅存在两种状态：`SlotState::Uninitialized (0)` 与 `SlotState::Live (1)`，或**不存在**。
- **创建**：`#allocate(shape_ptr, is_mut)` 分配 `Uninitialized` 未绑定直接子槽；`#allocate_val(val, is_mut)` 分配并初始化为 `Live` 未绑定直接子槽。`mutability` 在创建时确定，永不收窄。
- **初始化与写**：`#touch(ptr)` 仅允许将 `#this` 的 `Uninitialized` 直接子槽翻转为 `Live`；`#write(dst_ptr, val)` 允许写入 `Uninitialized` 槽（无论是否可变，写后翻转 `Live`）或 `mutable Live` 槽/闭合对象可变字段；`#deref(ptr)` 拒绝读取 `Uninitialized` 槽。
- **绑定与删除**：`#bind(sym, ptr)` 仅绑定 `#this` 的未绑定直接子槽，禁止一槽多绑与同名覆盖，返回 `raw1`（`1/0`）；`#drop(ptr)` 沿当前协程活跃构建链查找直接子槽，**同步删除槽实体与绑定关系**（支持循环复写与同名重绑）。
- **内核零 RAII / 零 POD 检查**：所有 Core 原语均不检查 `__destroy` 或 POD；`#deref`/`#write` 仅沿 `active_coro` 链校验 `Uninitialized`。

---

## 4. 21 个 Core Builtins 完整契约（`Evaluator.cpp`）

| 分类 | Builtin 签名 | 返回 | 核心语义 |
| :--- | :--- | :--- | :--- |
| **零参游标 (3)** | `#this` / `#caller` / `#in` | `Pointer` / `Handle` / `Pointer` | 当前帧指针 / 唤醒当前协程的调用者句柄 / 当前协程输入槽指针 |
| **导航与反射 (3)** | `#parent(ptr)` | `Pointer` | 获取开放帧的父帧指针（根帧返回空指针） |
| | `#field(ptr, sym)` | `Pointer` | 统一相对胖指针寻址：`{ ptr.addr + fe.offset, fe.sub_shape, ptr.mut && fe.mut }` |
| | `#shape(val)` | `Pointer(Shape)` | 返回 `val` 驻留在 `ShapeArena` 的只读 `Pointer(Shape)` |
| **求值与协程 (5)** | `#evaluate(proto)` | `Value` | 在当前 `#this` 就地执行 `proto`（不压新帧） |
| | `#block(proto, is_mut)` | `Pointer` | 压子帧执行 `proto`；正常结束 `close_and_flatten`（按 `bind_order` 仅保留 `Live` 绑定槽，静默剥离 `Uninitialized` 与未绑定槽），或捕获 `#break(val)`；退帧后在父帧以 `is_mut` 物化为未绑定 `Live` 匿名槽并返回其 `Pointer` |
| | `#new(proto)` | `Handle` | 惰性创建隔离帧树的新协程（初始停在 `Suspended`） |
| | `#destroy(handle)` | `raw1` (`1/0`) | 手动提前销毁协程并回收槽位 |
| | `#coro_root_frame(handle)` | `Pointer` | 返回目标协程根帧指针（为未来 stdlib mark-sweep GC 遍历保留的反射原语） |
| **槽状态机 (5)** | `#allocate(shape_ptr, is_mut)` | `Pointer` | 分配未初始化匿名槽（`shape_ptr` 须为 `ShapeArena` 中的 `Pointer(Shape)`） |
| | `#allocate_val(val, is_mut)` | `Pointer` | 分配并写入 `Live` 匿名槽 |
| | `#touch(ptr)` | `Unit` | 将 `#this` 的 `Uninitialized` 直接子槽翻转为 `Live` |
| | `#bind(sym, ptr)` | `raw1` (`1/0`) | 将 `#this` 的未绑定直接子槽绑定到 `sym` |
| | `#drop(ptr)` | `Unit` | 删除当前协程活跃链中的直接子槽及其绑定 |
| **读写 (2)** | `#deref(ptr)` / `#write(dst, val)` | `Value` / `Unit` | 原始按位读（拒空指针与未初始化槽）/ 写 |
| **控制流 (3)** | `#branch(cond, thenP, elseP)` | `Value` | `cond: raw1` 非零就地 `#evaluate(thenP)`，否则 `elseP`（零压帧） |
| | `#break(val)` | — | 单层跳出最内层 `#block` 或协程根帧并带出 `val` |
| | `#resume(coro, val)` | `Unit` | 原子校验 `Suspended`、写入目标 `in_slot` 并对称转移；子协程结束时自动回收并把结果写入调用者 `in_slot` 切回 |

---

## 5. Capabilities 与 FFI ABI（`Capabilities.cpp`、`RuntimeCaps.cpp`、`CompilerCaps.cpp`、`FfiCaps.cpp`、`FfiDispatch.cpp`）

所有 capability 原语均为 `NativeProto`，通过 `#evaluate` 从 `#this` 读取命名字段或位置字段（`0, 1, ...`）。目标隔离运行环境（`backends.interpreter.run` 与打包可执行文件）**零 `capabilities`**。

### 5.1 9 个编译期挂载模块
1. **`constants`（17 项）**：`true`/`false`（`raw1`）、`u64_zero`/`u64_one`/`u64_max`（`raw8`），以及 12 个 `Pointer(Shape)`（`unit_shape`, `raw1/2/4/8_shape`, `symbol_shape`, `pointer_shape`, `field_entry_shape`, `field_map_shape`, `shape_meta`, `prototype_shape`, `handle_shape`）。
2. **`field_map`（7 项）**：`empty`, `is_empty`, `count`, `get`, `at`, `insert`, `ptr_shape`（返回 `Pointer(Shape)`）。
3. **`symbols`（4 项）**：`byte_len`, `byte_at`, `to_bytes`, `intern_bytes`。
4. **`intrinsics`（30 项）**：
   - 27 个纯数据 `IntrinsicProto` 工厂（可跨阶段）：整数 `add/sub/mul/div/rem_int`、位运算 `bit_and/or/xor/not`/`shl`/`shr`、整数转换 `ext_int`/`trunc_int`、浮点 `add/sub/mul/div_float`、浮点转换 `ext_float`/`trunc_float`（仅读 `src`，固定 `4 <-> 8`）、`int_to_float`/`float_to_int`、比较 `eq_bytes`（支持 `width ∈ 1..255`）/`lt_int`/`le_int`/`eq_float`/`lt_float`/`le_float`。*（已移除 `eq_shape`，`Pointer(Shape)` 直接用 `.addr` 8B 比较。）*
   - 3 个 `RawBytes` 原语：`bytes_at`, `bytes_slice`, `bytes_concat`。
5. **`ffi`（2 项）**：
   - `extern_symbol({ lib: RawBytes, name: RawBytes, abi }) -> FfiProto`（可跨阶段；`lib` 原样传给 `dlopen`/`LoadLibraryA`，空串 `""` 表示当前进程符号表）。
   - `bind_ptr({ fn: Symbol, abi }) -> FfiBindPtrProto`（两阶段间接调用：运行期 `#evaluate` 读 `#this.<fn>` 得不可跨阶段的 `BoundFfiProto`）。
   - `abi := { args: Pointer(Shape), ret: Pointer(Shape), cc: c }`；两工厂在编译期即急切校验并缓存 `ffi_cif`。
6. **`ast`（10 项）**：`new_arena`, `destroy_arena`, `word`, `builtin`, `seq`, `value`, `proto`, `to_proto`, `serialize`, `deserialize`。
7. **`syntax`（19 项）**：`new_layer0`, `destroy_frontend`, `lit`, `range`, `ref`, `any`, `eof`, `seq`, `choice`, `repeat`, `lookahead`, `get_rule`, `set_rule`（执行 CST handler 时除 `node`/`arena` 外会隐式复制 `capabilities` 到回调帧）, `parse`, `cst_child_count`, `cst_view_sym`, `cst_view_bytes`, `cst_child_view_sym`, `cst_eval_child`。
8. **`backends.interpreter`（2 项）**：`run({ ast })`（新建隔离 `RuntimeContext`+`Evaluator` 执行并回传可跨阶段结果）、`emit_executable({ ast, out_path })`（输出独立打包可执行文件并赋 `+x`）。
9. **`io`（2 项，Phase 1 临时）**：`read_file`, `write_file`。

### 5.2 FFI `Shape -> ffi_type` 纯净 4 条映射规则（仅支持 `cc = c`）
1. `size == 0 && is_leaf()` → `ffi_type_void`（仅允许作返回值，禁止作参数）。
2. `size == 4 && has_zero_byte_tag("__f32_tag")` → `ffi_type_float`；`size == 8 && has_zero_byte_tag("__f64_tag")` → `ffi_type_double`。
3. `is_leaf()` 且 `size ∈ {1, 2, 4, 8}` → `ffi_type_uint{8, 16, 32, 64}`。
4. 其余所有含 `≥ 1` 非零字节字段的结构体（含单字段 C struct）→ 一律走 `ffi_type_struct`，并用 `ffi_get_struct_offsets` 严格校验整体 `size` 与逐字段 `offset`（不匹配则报错拒绝；ZST 整数包装由标准库自行解包 `.__raw`，不入侵 Core）。

---

## 6. CLI 与独立打包可执行契约（`main.cpp`、`stub_main.cpp`）

- **编译器入口（`main.cpp`）**：`lang <entry.lang> [args...]`。挂载全部 9 个 `capabilities` 模块，将 `argv[2..]` 注入 `args`（`{ count, 0, 1, ... }`），以 Layer 0 解析执行，成功恒返回退出码 `0`。
- **独立打包产物（`stub_main.cpp`）**：文件布局 `[embedded_lang_stub][Ast::serialize payload][PackTrailer: u64_le payload_size + magic]`。运行时自定位反序列化，注入完整 `argv[0..]` 到 `args`（**零 `capabilities`**），提取顶层返回值（裸 `raw1/2/4/8` 或带 `.__raw` 字段的包装整数）的低 8 位作为进程退出码。
