<!--
Title your PR in English. Fill in what applies.
PR 标题请用英文，按实际情况填写。
-->

## What / 改动内容

## Why / 动机
<!-- Link the issue: 关联 Issue: `Closes #` -->

## Type / 类型
- [ ] Config/protocol (JSON only, no code) / 配置或协议（仅 JSON）
- [ ] Bug fix / 缺陷修复
- [ ] Feature / 新功能
- [ ] Docs / 文档
- [ ] Refactor / 重构

## Checklist / 自检
- [ ] Builds with VS2015 (v140), C++11 only / 用 VS2015(v140) 编译，仅 C++11
- [ ] `scripts\ci.ps1` passes / 通过
- [ ] No C++14/17 features, no `std::optional`/`tl::expected`/`std::span`
- [ ] Docs/comments updated to match behaviour / 文档注释与行为同步
- [ ] No third-party protocol code or vendor-doc text copied / 未复制第三方实现或厂商文档内容
