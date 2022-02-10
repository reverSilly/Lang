语法：`[Idn]`+`as`+`[rvalue-type-Expr]`

用于显式指定一个重载的标识符的类型（语义）。
对于非重载标识符，也可以使用 `as限定符` ，若 `[rvalue-type-Expr]` 不符合 `[IdN]` 绑定的类型，则将导致编译时错误（若 `[rvalue-type-Expr]` 为 `constant` ）或导致一个运行时异常（若 `[rvalue-type-Expr]` 为 `dynamic` ）。