# MyProtV2 — Simulation 模块

> **所属**: MyProtV2 模块设计系列
> **上一节**: [初始化时序](./09_Initialization.md)

---

**依赖**: `Core` + `Transport` + standalone Asio

**定位**: 可选模块——**配置驱动的协议仿真服务器**。复用协议 JSON 的 `transport` / `framing` / `operations` 正向定义**反向应答**，为开发调试、回归测试、容错验证提供零硬编码的模拟设备端；对上层引擎完全无感知（设备侧把仿真器当真实设备连）。
> 未实装的高级仿真能力（自定义通道 / 回放 / 故障注入）见 [ROADMAP.md](../ROADMAP.md)。

## 10.1 设计原则 — 配置驱动闭环

同一份协议 JSON 被双向消费：

| 方向 | 消费者 | 用途 |
|------|------|------|
| 正向 | PollingEngine / TagReader | 以 `requestTemplate` 构造请求、按 `responseParser` 校验解析应答 |
| 反向 | SimulationServer | 收到请求后用 TemplateMatcher **逆向识别**操作名与变量值，按 `simulation` 节声明的数据方向摆弄数据区，再按 `responseParser` 规格**正向合成**应答 |

报文格式知识（字节布局、长度字段、校验位置）全部来自协议配置，仿真器代码对任意自描述协议零硬编码——新增协议只需改 JSON，仿真器自动可用（前提：`simulation.listenPort > 0` 且声明了对应操作的仿真行为）。

## 10.2 目录结构（实际实现）

```
src/Simulation/
├── include/MyProt/Simulation/
│   ├── SimulationServer.hpp      # 配置驱动仿真服务器 (TCP 监听, 本期主线)
│   ├── TemplateMatcher.hpp       # requestTemplate 反向匹配器
│   ├── SimulationDataStore.hpp   # 16 位寄存器阵列数据区 (HslCommunication 风格)
│   └── ResponseSynthesizer.hpp   # responseParser 反向复用的应答合成器
└── src/                          # 对应 .cpp 实现
```

## 10.3 数据流与线程模型

```
客户端请求 → acceptor.accept (AcceptLoop 线程, 同步模型)
           → HandleConnection: 同步 read_some 累积缓冲
           → TryExtractFrame: 按 framing 切出完整帧
           → HandleFrame:
               → TemplateMatcher.Match(frame)        # 操作名 + 变量值表 (含 auto 通配捕获)
               → 查 SimPlan (simulation.operations)  # read / write 数据方向
               → SimulationDataStore                 # ReadRegisters / WriteRegisters
               → ResponseSynthesizer.Synthesize      # 按响应规格合成完整应答帧
           → write 回客户端
```

- **线程模型**：`Start()` 自起 AcceptLoop 线程做同步 accept，连接处理在同一线程串行进行——**不需要外部 io_context.run() 驱动**（构造传入的 io_context 仅用于 socket 对象归属）。`Stop()` 关闭 acceptor + join。
- **监听边界**：仅绑定环回 `127.0.0.1:simulation.listenPort`，不暴露到外部网络。

## 10.4 TemplateMatcher — 反向模板匹配

把正向的 `requestTemplate` 文法反过来用：编译期将每个模板行预编译为字面量段 + 变量段序列，运行期对收到的帧逐段比对。

| 模板元素 | 匹配行为 |
|------|------|
| 十六进制字面量（如 `"03"`） | 与帧内同偏移字节精确比对 |
| `{Name:Xn}`（两段 = Variable） | 跳过 Xn 宽度的字节并**捕获**该值为变量（如 `StartAddress`） |
| `{Name:auto:Xn}`（三段 = Wildcard） | 同上但无论变量名为何都通配捕获（事务 ID 等每请求变化的字段） |

- 变量名大小写无关；`auto:` 前缀使占位符退化为纯宽度跳过。
- **先比总长**：`totalSize == frame.size()` 才进入逐段比对，不等直接失配——不同操作靠帧长与字面量组合天然区分。
- 多个操作同时注册时逐一尝试，首个完全匹配者胜出。

## 10.5 SimulationDataStore — 寄存器数据区

16 位寄存器阵列（HslCommunication DataStore 风格），互斥锁保护：

```cpp
class SimulationDataStore {
public:
    explicit SimulationDataStore(std::size_t registerCount);
    /// 单寄存器写入 (initialValues 预置与运行时改值共用)
    bool WriteRegisterValue(uint16_t address, uint16_t value);
    /// 批量写入 (data 每 2 字节一个大端整数依次落地); 越界返回 false
    bool WriteRegisters(uint16_t address, const std::vector<uint8_t>& data);
    /// 读 count 个寄存器 → count*2 字节大端; 越界返回空
    std::vector<uint8_t> ReadRegisters(uint16_t address, uint16_t count) const;
    std::size_t Size() const;
};
```

初值来源：启动时把 `simulation.initialValues`（地址十进制字符串 → 值）逐项 `WriteRegisterValue` 预置。

## 10.6 ResponseSynthesizer — responseParser 的反向复用

客户端用 `responseParser` 校验应答；仿真器把同一规格当作"应答布局说明"，从请求反向生成应答帧：

| responseParser 子句 | 合成行为 |
|------|------|
| `validCondition` `"resp[N]==V"` | 应答偏移 N 写入字面量 V（如功能码回显位 `resp[7]==0x03` → 应答第 7 字节为 0x03） |
| `dataLengthExpr` `"resp[N]"` | 应答偏移 N 写实际数据长度（字节计） |
| `dataStartIndex` M | 应答前 M 字节镜像请求前缀，数据区从 M 起 |
| `dataLengthExpr` 为数字常量 | 数据区长度对齐该常量（截断 / 补零） |

最后按 framing 配置**重算帧内长度字段**——与 `LengthFieldFrameParser::CalculateTotalFrameSize` 切帧公式互逆，保证客户端用同一份 framing 配置能正确切出本帧。无法合成时返回空向量（调用方静默丢弃）。

### 自定义应答模板 responseTemplate（P1#8）

`simulation.operations[].responseTemplate`（可选，`array<string>`）：非空时**覆盖默认 echo 反向合成**，按模板逐行拼接应答帧；长度字段仍按 framing 自动重算。缺省时行为完全不变。

行文法（加载期由 ConfigDeepValidator 校验，非法即拒绝加载）：

| 片段 | 含义 |
|------|------|
| hex 字面量 `"AA 0B"` | 原样写入字节（空格 / 制表符分隔，每段恰两位十六进制） |
| `{req:N:M}` | 从请求帧偏移 N 拷贝 M 字节（十进制；越界 → 合成失败，不应答） |
| `{data}` | 展开数据区（read = 寄存器值序列，write 为空） |

示例（Modbus FC10 写单寄存器的自定义应答——TID/UID 回显 + 固定 FC + 地址回显 + 自定义尾缀）：

```jsonc
"responseTemplate": [
  "{req:0:2}",   // TID 回显
  "00 00",       // PID 常量
  "00 06",       // LEN 占位 (framing 重算覆盖)
  "{req:6:1}",   // UID 回显
  "10",          // FC 固定位
  "{req:8:4}",   // Addr+Value 回显
  "AA BB"        // 自定义尾缀 (echo 合成做不到)
]
```

运行期分派：`SimulationServer::HandleFrame` 中模板非空走 `ResponseSynthesizer::SynthesizeFromTemplate`（文法错误 / 请求越界返回 false → 不应答），否则走 echo 反向合成。

## 10.7 SimulationServer — 仿真服务器

```cpp
// src/Simulation/include/MyProt/Simulation/SimulationServer.hpp
class SimulationServer {
public:
    /// protocol 须在服务器生命周期内保持有效 (App protoStore 保证, 含 reload 联动)
    /// simOverride 来自 ServerConfig.simulation (server.json)
    /// 自持独立 io_context + 全异步 accept/read; Stop() 通过 io.stop() 即刻唤醒
    explicit SimulationServer(const Core::ProtocolConfig& protocol,
                              const Core::SimulationConfig& simOverride);

    SimulationDataStore& Store();                 // 给 WebApi 直接读写数据区
    std::uint16_t ListenPort() const;             // Start 成功后有效; 0 = 未启动
    /// 预编译匹配器/应答规格、预置初值并启动监听;
    /// 未启用仿真节 / 端口绑定失败 → false + err 说明
    bool Start(std::string& err);

    /// 停止监听并结束工作线程 (阻塞至线程退出)
    void Stop();

private:
    struct SimPlan {
        Core::SimOperationConfig cfg;             // simulation.operations 条目
        ResponseSynthesizer::Spec spec;           // 应答规格 (预解析缓存)
    };
    TemplateMatcher _matcher;
    SimulationDataStore _store;
    std::map<std::string, SimPlan> _plans;        // sim 声明操作的执行计划
};
```

**静默失败语义**（调试须知）：以下情形一律**不应答直接返回**，客户端读超时即故障信号——

1. TemplateMatcher 无操作匹配成功；
2. 匹配成功但 `simulation.operations` 未声明该操作（无 SimPlan）；
3. 匹配出的地址/数量变量缺失或数据区越界；
4. ResponseSynthesizer 合成返回空。

## 10.8 配置示例（Modbus TCP 全闭环）

协议 JSON（`configs/protocols/modbus-tcp.json` 增加 simulation 节）：

```jsonc
{
  "schemaVersion": 1,
  "protocolName": "modbus-tcp",
  "transport":  { "type": "Tcp", "defaultPort": 502 },
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 4, "lengthFieldLength": 2,
    "lengthIncludesHeader": false, "byteOrder": "BigEndian",
    "headerLength": 6,            // MBAP 头 = 事务ID+协议ID+长度 = 6B, 必须显式声明!
    "lengthAdjustment": 0, "maxFrameSize": 260
  },
  "operations": {
    "ReadHoldingRegisters": {
      "requestTemplate": [
        "{TransactionID:auto:X4}", "{ProtocolID:X4}", "{Length:calc:X4}",
        "{UnitID:X2}", "03", "{StartAddress:X4}", "{RegisterCount:X4}"
      ],
      "responseParser": {
        "validCondition": "resp[7] == 0x03",
        "dataStartIndex": 9,
        "dataLengthExpr": "resp[8]"
      }
    }
  },
  "handshake": [],
  "builtInFunctions": ["auto", "calc"],
  "simulation": {
    "listenPort": 11520,
    "registerCount": 65536,
    "initialValues": { "0": 5 },
    "operations": {
      "ReadHoldingRegisters": {
        "kind": "read",
        "addressVar": "StartAddress",
        "countVar": "RegisterCount"
      }
    }
  }
}
```

闭环效果：引擎（或任意 Modbus 主站）连 `127.0.0.1:11520` 以 FC03 读地址 0 → 仿真器识别操作 → 从数据区读 2 字节大端（初值 5）→ 合成 `resp[7]=0x03`、`resp[8]=2` 的标准 MBAP 应答（长度字段自动重算）。

## 10.9 运行时操控与 reload 联动

**WebApi 操控**（App 的 `/api/sim/*` 扩展路由，详见 [modules/07 §7.5](./07_WebApi.md)）：

- `GET /api/sim/status` — 仿真器清单（协议/端口/寄存器数）；
- `GET /api/sim/registers?proto=&start=&count=` — 直读数据区（绕过协议）；
- `POST /api/sim/registers` `{"start":N,"values":[..]}` — 手动改值，**即时对设备侧轮询可见**（UI 仿真控制台的单行改值走此路径）。

**reload 联动**：UI 保存协议 JSON / `POST /api/config/reload` 成功后，App 以 `ApplyRuntimeSync` 全停重建所有仿真器并清空实时快照——端口迁移后旧端口立即不可连、新初值立即生效；坏配置 reload 失败则运行态保持不变（详见 [modules/07 §7.4](./07_WebApi.md)，E2E Test 12 覆盖）。

## 10.10 Phase 边界与已知限制

| 项 | 状态 |
|------|------|
| TCP 监听（仅环回）+ LengthField / Fixed 切帧 | ✅ v1 已落地 |
| read / write 两类数据方向 | ✅ v1 已落地 |
| Silence（串口 RTU）仿真 | ❌ 不在本期范围（需串口虚拟化或 Silence 成帧服务端支持） |
| 握手流程仿真、多从站共享总线仿真 | ❌ 未实现 |
| 匹配失败静默不应答 | ⚠️ 有意设计（防畸形流量放大），排障时以客户端读超时为准 |
| 仿真器启动失败（端口占用等） | 启动期 `[WARN]` 日志，不阻断主程序 |

## 10.11 旧高级特性设计

ISimChannel / DeviceSimulator / PlaybackEngine / FaultInjector 当前不存在于仓库。设计与启用评估见 [ROADMAP.md](../ROADMAP.md)。

---

> **相关文档**: [Config_Schema §2.6 simulation 节](../Config_Schema.md) · [WebApi 模块](./07_WebApi.md) · [ADR-0002 传输抽象](../adr/0002-transport-abstraction.md)
>
> 尚未实装的扩展项统一登记在 [ROADMAP.md](../ROADMAP.md)。
