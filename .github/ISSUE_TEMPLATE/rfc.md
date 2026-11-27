---
name: RFC / 1-Page 设计变更单
about: 新语言特性、Core/Layer 1 语义变更或架构重构前的 1 页设计提案
title: "[RFC] "
labels: rfc
assignees: ""
---

## 目标（Goal）
一句话说明要解决什么问题，以及为什么需要这个改动。

## 核心规则（Core Rules）
- 规则 1：
- 规则 2：
- 分层归属（Core / `lib/std/boot` / `lib/std/syntax` / `lib/std`）：

## 正反边界用例（Examples & Edge Cases）
```lang
! 正例 !

! 反例 / 预期报错 !
```

## 影响范围（Impact）
- [ ] 是否触及 `std/design/CORE_SPEC.md`（若触及需说明具体条款）
- [ ] C++ Core（`compiler/`）
- [ ] 标准库 / 语法自举（`lib/std/`）
- [ ] 测试（`tests/`）

## 验收标准（Acceptance Criteria）
- [ ] 新增独立特性测试文件并通过 `ctest`
- [ ] 打包可执行文件端到端测试通过
