语法：`typeof`+`[Expr]`

用于取得一个值的类型。返回一个`type`类型的右值。取决于 `[Expr]` 是否为静态类型，返回值可能是 `constant` 的。

保证若返回值是 `constant` 的，保证 `[Expr]` 不会被求值。