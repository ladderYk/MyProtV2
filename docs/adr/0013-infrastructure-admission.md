# ADR-0013：构建与测试基础设施准入（单一构建系统 · 空壳测试禁止 · CI 门禁）

| 项 | 内容 |
|----|------|
| 状态 | **提议（PROPOSED）** · 2026-09-14（四条规则已落地并经 `scripts/ci.ps1` 验证 5/5 全绿；待评审确认后转 ACCEPTED） |
| 日期 | 2026-09-14 |
| 相关文档 | [ADR-0010](./0010-vs2015-cpp11-toolchain.md)（VS2015/C++11 工具链，§5 构建系统）· [docs/architecture/05](../architecture/05_Observability_Config_Build.md) §13.1（构建产物）· [tests/MiniTest.hpp](../../tests/MiniTest.hpp) |
| 提案来源 | 构建与测试基础设施审计（2026-09-14）：文档声称"CMake 已废止"，仓库内却存在 9 个 CMakeLists；`Engine.Tests` 三个文件为空占位；回归全靠人工手跑 |

---

## 背景

### 1. 审计发现的偏差

| 项 | 实况 | 证据 |
|----|------|------|
| **CMake 配置** | 9 个子目录各有一份，**无顶层** | `src/{App,Core,Engine,Gateway,Polling,Service,Simulation,Transport,WebApi}/CMakeLists.txt` |
| ↳ 路径错位 | 各文件用 `${CMAKE_SOURCE_DIR}/src/...` 拼 include，但无顶层入口 → `${CMAKE_SOURCE_DIR}` 实为各自子目录，**全部 include 路径不成立** | `src/App/CMakeLists.txt` L11-L18 |
| ↳ 源文件路径错 | `add_executable(MyProtApp src/main.cpp src/RuntimeGlue.cpp)` —— 相对 `src/App` 展开为 `src/App/src/main.cpp`，**文件不存在** | 同上 L23 |
| ↳ 依赖获取错 | `find_package(asio REQUIRED)` —— 本仓库 asio 采用 vendoring（`third_party/`），无 config 包可 find | 同上 L20 |
| ↳ 工具集条件已失效 | `cmake_minimum_required(VERSION 3.10)`，而 CMake 3.12+ **已移除** "Visual Studio 14 2015" 生成器 → ADR-0010 §5 "仅当生成器仍支持 VS2015 时保留" 的前提**不成立** | 同上 L4 |
| ↳ 与文档矛盾 | 文档写"vcpkg 清单模式与 CMake **均已废止**"，仓库内文件仍在 | [05 §13.1](../architecture/05_Observability_Config_Build.md) L118 |
| **改名残留工程** | `src/Transport/Transport.vcxproj`：未被 sln / 脚本 / 文档任何位置引用；其源文件清单是现行工程的真子集（缺 `TlsChannel` / `SerialChannel`）→ 构建它只会产出一个残废的 `Transport.lib` | sln 仅含 `MyProt.Transport.vcxproj`；全仓引用 0 处；`/utf-8` 0 处 |
| **空壳测试** | `Engine.Tests` 三个测试文件各 10-11 行，仅一个 `SUCCEED()` 占位 → 项目**存在即通过**，工程名给出虚假保证 | `tests/Engine.Tests/*.cpp`（2026-09-14 前） |
| **无 CI 门禁** | 5 个可执行的回归靠人工手跑；"全绿"只在**当次、本机**成立，无法阻止红提交进入主干 | 仓库无任何流水线配置 |
| **外部依赖准入** | `third_party/gtest` 的 `lib/*/gtest.lib` 为 8 字节空归档（`!<arch>`）→ 测试长期无法链接；已由自研 `MiniTest.hpp` 取代 | `tests/MiniTest.hpp` L3-L5 |

### 2. 结论

上述偏差的共同性质不是"功能缺失"，而是**可验证性被削弱**：仓库里出现了"看起来能用、实际从未被验证"的构建配置与测试工程，以及"只在本机成立"的绿色结论。因此本 ADR 不新增功能，只确立四条准入规则。

---

## 决策

### 2.1 单一构建系统：仓库根 `MyProt.sln`

- 构建系统**唯一**为 `MyProt.sln`（MSBuild / VS2015 v140，Win32 + x64）。源码交付即开即编译，零网络依赖。
- **移除**上述 9 个失效 CMakeLists **与 1 个改名残留工程文件**（`src/Transport/Transport.vcxproj`），不再保留"可选辅助"承诺。ADR-0010 §5 中"CMake 降为可选辅助"一句，**由本 ADR 取代**。
- **恢复条件**（若将来要脱离 VS2015）：先补齐顶层 `CMakeLists.txt`、修正各级相对路径、明确 asio 获取方式，并在**能真正执行 cmake 的环境**完成一次构建验证，再以新 ADR 重新引入。
- **未经验证的构建配置不得入库** —— 这是本条的核心，而非"不许用 CMake"。

### 2.2 空壳测试禁止

- 登记进解决方案的测试工程，必须至少有一条**能失败**的断言；不得只含 `SUCCEED()` 占位。
- `Engine.Tests` 已补真用例：表达式优先级与错误路径、模板展开与缺失变量、三段变量合并优先级、模板布局（宽度/偏移/固定段）、auto 自增策略、字节序裁决链、响应条件校验。
- **宁缺勿假**：暂时无法测的模块，宁可不建测试工程，也不建空壳 —— "名字叫测试、里面没测试"比没有测试更有害。

### 2.3 CI 门禁：`scripts/ci.ps1`

- 门禁内容 = 编译 Release x64 + 运行**全部 5 个可执行**（`MyProt.Core/Engine/Transport/Service.Tests.exe` + `MyProt.E2E.exe`）。
- **退出码契约**：`0` = 全绿；非 0 = 失败步骤数。任何 CI runner / 计划任务 / 手工调用均可直接使用，不绑定特定流水线产品。
- 每个可执行设**超时上限**（单元测试 60s，E2E 300s），超时按失败计 —— E2E 含真实 socket、端口与时序，必须防止挂死。
- 输出保留每个可执行的退出码、耗时与失败行（`[FAIL]`），供日志追溯。

### 2.4 外部依赖准入

- 入库依赖必须"**开箱可链接**"：头文件 + 与目标工具集匹配的二进制。空归档、需手工编译的占位不得入库。
- 无法满足时，**自研最小替代优先于保留坏依赖**（先例：MiniTest 取代 gtest）。

---

## 影响

| 类别 | 变化 |
|------|------|
| 删除 | 9 个失效 CMakeLists + 1 个改名残留工程（`src/Transport/Transport.vcxproj`） |
| 新增 | `scripts/ci.ps1`（CI 门禁）、本 ADR |
| 测试 | `Engine.Tests`：3 个空占位 → 3 个文件 13 个真用例 |
| 文档 | architecture/01 §构建系统行、architecture/05 §13.1、ADR-0010 §5 与本 ADR 对齐 |
| 运行时 | **无行为变更** —— 不触碰任何 `src/` 生产代码路径、`.sln` / `.vcxproj` 构建语义 |

### 明确的不进范围

- 覆盖率统计、性能/压力基线**尚未**纳入门禁（工具选型未定，见后续 ADR）。
- **自动触发尚未接入**（决定：先不接）：`scripts/ci.ps1` 目前是一份"需要有人执行"的脚本，未挂入计划任务或流水线。因此"全绿"仍依赖人工发起，**不构成对红提交的自动拦截** —— 该缺口已知并记录在此，接线方式（内网 CI / 计划任务）待定。
- 自研日志（`MYPROT_LOG_*`）、`Expected/Optional/ByteView` 等基础设施替代品的取舍不在本 ADR 范围（属 ADR-0010 的工具链降级域）。
