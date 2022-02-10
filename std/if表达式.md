语法：`[Expr-1]`+`if`+`[rvalue-Boolean-Expr]`(+`else`+`[Expr-2]`)?

若 `[Boolean-Expr]` 为真时计算 `[Expr-1]` 作为表达式的值，否则计算 `[Expr-2]` （如果有的话）或空值（`()`）作为表达式的值。

语言保证对于两个表达式只会计算其一的值。

`else`永远与最近的`if`结合。

若 `if表达式` 中不含 `else` 则其值类型为 `optional[typeof([Expr-1])]` 否则类型为 `typeof([Expr-1])|typeof([Expr-2])`


例子：
```
a:=114514 if true else 1919810
//a 是 114514
b:=
114 if a==1919810
else 514 if a~=114514
else 1919810
//b 是 1919810
```