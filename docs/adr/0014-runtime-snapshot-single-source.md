# ADR-0014：运行时配置快照单一化（RuntimeSnapshot）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-10-23（WSL Release 构建 + `ctest` 5/5 验证） |
| 日期 | 2026-10-23 |
| 相关文档 | [ADR-0005](./0005-config-versioning.md)（配置代际）· [ADR-0011](./0011-p1cd-write-mutex-and-per-bus-strand.md)（写路径并发）· [modules/08_App](../modules/08_App.md)（ApplyRuntimeSync）· [modules/10_Simulation](../modules/10_Simulation.md)（SimulationServer 生命周期） |
| 提案来源 | 架构评审 P1-3：同一代配置在内存中存在多份副本，且仿真器持有副本内元素的裸引用 |

---

## 背景

### 1. 多副本并存

`AppContext` 曾同时持有三组共享容器：

```cpp
std::shared_ptr<std::vector<ProtocolConfig> > protos;   // 协议查找
std::shared_ptr<std::vector<DeviceConfig> >   devices;  // 通道工厂 / 写路径
std::shared_ptr<std::vector<TagDefinition> >  tags;     // 写路径
std::vector<ProtocolConfig>*                   protoStore; // 仿真器
```

`ApplyRuntimeSync` 内部又持有一份 `LoadedConfig`，并在 `io.post` 的装配闭包中
把同一代配置分别拷入上述容器。结果是同一代配置存在 **3 份副本**，分别被
写路径（HTTP 线程读取）、通道工厂（io 线程）、仿真器（监听线程）消费。

### 2. 悬垂引用

`SimulationServer` 的构造函数接收 `const ProtocolConfig&`，`TemplateMatcher`
再持有该引用。热重载时 `protoStore` 被整体赋值，元素地址可能变化，而仿真器
在 `Stop()` 之前仍在监听线程读取 `_protocol`——存在悬垂引用与数据竞争。

### 3. 可见性窗口

若快照只在 `io.post` 的装配闭包中发布，管理面写路径在 `ApplyRuntimeSync`
返回后、io 线程执行装配前仍会读到上一代 tags/devices，导致"配置已保存但
首个写请求仍按旧配置路由"。

---

## 决策

引入 `Core::RuntimeSnapshot` 作为**一代运行时配置的唯一载体**：

```cpp
struct RuntimeSnapshot {
    std::vector<ProtocolConfig> protocols;
    std::vector<DeviceConfig>   devices;
    std::vector<TagDefinition>  tags;
    Optional<ResilienceConfig>  resilience;
    ServerConfig                server;      // server.json（仿真段等）
    const ProtocolConfig* FindProtocol(const std::string& name) const;
    const DeviceConfig*   FindDevice(const std::string& id) const;
};

typedef std::shared_ptr<const RuntimeSnapshot> RuntimeSnapshotPtr;

class RuntimeSnapshotHolder {                 // 线程安全
public:
    RuntimeSnapshotPtr Get() const;            // 锁内拷贝 shared_ptr
    void Set(const RuntimeSnapshotPtr& next);  // 整代替换
};
```

约束：

1. **一份产物**：`ConfigDirectoryLoader::Load` 的结果直接 `move` 进快照，
   不再向各组件分发副本；`AppContext` 只保留 `RuntimeSnapshotHolder*`。
2. **发布即只读**：读侧拿 `shared_ptr<const RuntimeSnapshot>`，热重载整代
   替换而非原地改写；旧快照由引用计数保活到最后一个读者/仿真器释放。
3. **同步发布**：`ApplyRuntimeSync` 在返回前调用 `holder.Set()`。管理面读
   立即看到新代；引擎/网关的内部重装配仍 `io.post` 到 io 线程，二者通过
   io 队列 FIFO 保证"重装配先于新到写请求"。
4. **长期持有者保存 shared_ptr**：`SimulationServer` 接收
   `shared_ptr<const ProtocolConfig>`（指向快照内元素的别名 shared_ptr），
   `SimulationConfig` 按值拷贝，不得保存裸引用。
5. **lookup/factory 读当前代**：闭包捕获 `RuntimeSnapshotHolder*`，每次调用
   `Get()` 后查表，避免闭包内固化某代配置。

---

## 备选方案与取舍

| 方案 | 否决原因 |
|------|----------|
| 保留多份副本，靠锁同步 | 副本漂移不可检测；写路径与仿真器仍可能读到不同代配置 |
| 原地改写 `vector` + 全局锁 | 读者需持锁遍历；in-flight 回调持锁跨异步边界，易死锁 |
| 每个组件各自持有 `shared_ptr` | 无法保证同代；跨组件比较/排障困难 |
| 快照只含协议/设备/标签 | `server.json` 也会热重载，仿真器需要同代 `simulation` 段 |

---

## 影响

- **内存**：每代配置仅一份；旧代在最后一个 `shared_ptr` 释放后回收。
- **线程**：`RuntimeSnapshotHolder` 只在取/换指针时持锁，读路径无长锁。
- **行为**：`ApplyRuntimeSync` 返回后管理面即按新代配置工作；若新配置非法，
  `Load` 失败会在发布前返回，旧快照保持不变（ConfigStore 自动回滚语义不变）。
- **构建**：`RuntimeSnapshot.hpp` 为 header-only，随 Core 头文件分发。

## 复审触发条件

- 出现需要**部分热更新**（单协议/单设备替换）的场景；
- 快照体积增大到复制/保活成本可观测（如千级标签的频繁重载）；
- 引入多 io 线程（当前 FIFO 顺序假设需要重新论证）。
