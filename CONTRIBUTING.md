# 参与 MyProt 贡献

**简体中文** | [English](CONTRIBUTING.en.md)

感谢你有意改进 MyProt —— 一个纯配置驱动的 C++11 工业协议网关。本指南说明如何搭建、构建、测试与提交改动。

## 开始之前

- **语言/工具链是硬约束**：C++11，须能用 **VS2015（平台工具集 v140）** 编译。禁止引入 C++14/17/20 特性；禁止使用 `tl::expected` / `std::optional` / `std::span` —— 项目在 `src/Core` 自研了 `Expected<T,E>` / `Optional<T>` / `ByteView`。详见 [ADR-0010](docs/adr/0010-vs2015-cpp11-toolchain.md)。
- **零协议硬编码**：引擎只解释 JSON。新增协议是在 `configs/protocols/` 下加配置，而不是写 C++ 代码。若你认为某功能必须落到代码，请先开 Issue 讨论。
- **先读文档**：[docs/README.md](docs/README.md)（索引）、[docs/Config_Schema.md](docs/Config_Schema.md)（配置契约唯一事实来源）。

## 环境搭建

```powershell
# Windows + PowerShell
scripts\setup.bat      # 下载并 vendor 第三方依赖（asio、nlohmann/json 等）
scripts\build.bat      # 构建 VS2015 解决方案（MyProt.sln）
```

用 Visual Studio 2015 打开 `MyProt.sln` 开发，产物输出到 `build/`。

## 运行测试

```powershell
scripts\ci.ps1         # 编译 + 运行全部可执行，强制 CI 退出码契约
```

- 单元测试在 `tests/`（GoogleTest，vendored 于 `third_party/gtest`）。
- 端到端测试在 `src/Tests/E2EMain.cpp`（`MyProt.E2E` 独立进程）。

## 代码风格

- 与周边代码保持一致：命名、包含顺序、注释密度。
- 公开头文件在文件头注释里写明契约；声明与行为文档保持同步。
- 遵循单一职责，提取通用函数而非复制粘贴。
- 改动须在 v140 下保持零警告构建。

## 提交改动

1. **大改动先开 Issue** —— 架构调整、新传输、Schema 变更。小修可直接提 PR。
2. 从 `main` 拉分支，保持提交聚焦。
3. 填写 Pull Request 模板，关联对应 Issue。
4. 请求评审前确保 `scripts\ci.ps1` 通过。

### 新增一个协议（无需写代码）

1. 新建 `configs/protocols/<你的协议>.json`，描述 transport、framing、请求模板与响应解析（字节级对照见 [docs/protocols/](docs/protocols/)）。
2. 在 `configs/tags.json` 中引用它。
3. 重启即生效，无需重新编译。

## 许可

提交贡献即表示你同意你的贡献以项目的 [MIT 许可证](LICENSE) 授权。请勿从第三方实现（libsnap7、PLC4X、Neuron 等）或厂商文档中粘贴代码或字节约定 —— 协议字节约定必须独立推导（公开规范或你自己的抓包）。

## 商标

所有产品与协议名称（MODBUS、SIEMENS/S7、OMRON、Mitsubishi/MELSEC、TwinCAT/ADS、SEER 等）均为各自权利人的商标，仅用于描述互操作性。参见 [README](README.md#商标声明)。
