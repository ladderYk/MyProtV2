# archive/MyProtCpp — 历史快照（v1 之前）

> **状态**: 已归档（2026-08-29）。**不在 v1 构建路径中**。
> MyProt.sln（根目录）已替换为 8 模块分层架构，archive/ 仅作版本回溯备查。
>
> - 原 .sln: `MyProtCpp.sln`（单进程，6 文件全在 main.cpp 同目录）
> - 原设计: `PollingEngine` + `DeviceManager` + `ProtocolEngine` + `TagReader` + `TcpChannel` 全栈一体
> - 撤换原因: v1 拆为 Core/Engine/Transport/Gateway/Polling/Service/WebApi/Simulation/App 八模块分层，
>   引入 IChannel/IFrameParser/ConfigStore/SchemaRegistry 等可复用抽象，参见
>   [architecture/02_Layered_Architecture.md](../../docs/architecture/02_Layered_Architecture.md)
>
> **本目录文件请勿修改**：仅供 diff 与决策留痕参考。
> 编译请使用根目录 `MyProt.sln`（MSBuild + VS2015 v140 工具集）。
