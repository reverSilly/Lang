## 关联 Issue
Closes #

## 改动摘要
- 

## 自检清单
- [ ] 遵循 `std/design/CORE_SPEC.md`（若修改了 Core 契约，已同步更新 `CORE_SPEC.md`）
- [ ] `lib/std/syntax/` 中的纯运行时逻辑优先写为 `Prototype`（AST 仅作薄胶水）
- [ ] 本地已通过 `ctest --test-dir build --output-on-failure`
- [ ] 本地已通过 `tests/test_pack_exe.lang` 打包端到端验证（exit 42）
