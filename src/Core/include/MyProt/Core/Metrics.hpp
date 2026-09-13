// src/Core/include/MyProt/Core/Metrics.hpp
// 轻量指标注册表 — 计数器/仪表盘 + Prometheus 文本暴露 (architecture/05, KI-07)
// header-only 与 Core 工程约定一致; 埋点走进程级 Instance(), 渲染走 GET /metrics。
//
// 指集 (命名约定 myprot_*):
//   counter  myprot_poll_reads_total{device}        轮询采集样本数
//   counter  myprot_poll_read_failures_total{device} 轮询失败样本数
//   counter  myprot_writes_total{device}            写操作成功次数
//   counter  myprot_write_failures_total{device}    写操作失败次数
//   counter  myprot_channel_connects_total{device}  物理通道建立次数 (含重连)
//   counter  myprot_circuit_opens_total{device}     熔断器开启次数
//   gauge    myprot_circuit_state{device}           熔断器状态 (0=Closed 1=HalfOpen 2=Open)
//   gauge    myprot_device_lifecycle_state{device}  设备生命周期 (0=New 1=Connecting 2=Connected 3=Degraded 4=Disabled)
//   counter  myprot_device_lifecycle_transitions_total{device,from,to} 生命周期转换次数
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <map>
#include <mutex>
#include <sstream>

namespace MyProt { namespace Core {

/// 内置指标名常量 — 埋点侧统一引用, 防手写漂移
/// (须先于 MetricsRegistry 声明: 类内联函数体不可见其后声明的名字空间量;
///  便捷函数在同名字空间的后续块中定义)
namespace metrics {

const char* const kPollReadsTotal        = "myprot_poll_reads_total";
const char* const kPollReadFailuresTotal = "myprot_poll_read_failures_total";
const char* const kWritesTotal           = "myprot_writes_total";
const char* const kWriteFailuresTotal    = "myprot_write_failures_total";
const char* const kChannelConnectsTotal  = "myprot_channel_connects_total";
const char* const kCircuitOpensTotal     = "myprot_circuit_opens_total";
const char* const kCircuitState          = "myprot_circuit_state";
const char* const kDeviceLifecycleState  = "myprot_device_lifecycle_state";
const char* const kDeviceLifecycleTransitionsTotal = "myprot_device_lifecycle_transitions_total";

} // namespace metrics

/// 进程级指标注册表 — Prometheus 文本格式 (exposition v0.0.4) 渲染
/// 并发模型: 全局互斥; 埋点为纳秒级临界区, io 单线程 + WebApi 少量线程下无竞争压力
class MetricsRegistry {
public:
    typedef std::vector<std::pair<std::string, std::string> > Labels;

private:
    // ── 数据结构 (前置声明, 规避 v140 对类内后置类型的解析限制) ──
    struct Series {
        Series() : value(0.0) {}
        Labels labels;
        double value;
    };
    struct Family {
        Family() : registered(false) {}
        std::string type;
        std::string help;
        bool registered;
    };

    mutable std::mutex _mtx;
    std::map<std::string, Family> _families;              // 族名 → 元信息
    std::map<std::string, std::map<std::string, Series> >
        _series;                                          // 族名 → 键 → 序列

public:
    static MetricsRegistry& Instance() {
        static MetricsRegistry r;
        return r;
    }

    /// 预登记指标族元信息 (type: "counter"|"gauge"; help 非空时写入)
    void Describe(const std::string& name, const std::string& type,
                  const std::string& help) {
        std::lock_guard<std::mutex> lock(_mtx);
        Family& f = _families[name];
        if (f.type.empty()) f.type = type;
        if (!help.empty()) f.help = help;
        f.registered = true;
    }

    /// counter += v (缺省 1)
    void CounterAdd(const std::string& name, const Labels& labels, double v = 1.0) {
        std::lock_guard<std::mutex> lock(_mtx);
        TouchFamily(name, "counter");
        std::map<std::string, Series>& fam = _series[name];
        Series& s = fam[MakeKey(labels)];
        if (s.labels.empty()) s.labels = labels;
        s.value += v;
    }

    /// gauge = v (直接置值; 状态类指标用)
    void GaugeSet(const std::string& name, const Labels& labels, double v) {
        std::lock_guard<std::mutex> lock(_mtx);
        TouchFamily(name, "gauge");
        std::map<std::string, Series>& fam = _series[name];
        Series& s = fam[MakeKey(labels)];
        s.labels = labels;
        s.value = v;
    }

    /// 渲染全部指标为 Prometheus 文本 (输出按指标名/标签键排序, 结果确定)
    std::string RenderPrometheus() const {
        std::ostringstream os;
        std::lock_guard<std::mutex> lock(_mtx);
        for (std::map<std::string, Family>::const_iterator fit =
                 _families.begin(); fit != _families.end(); ++fit) {
            const std::string& name = fit->first;
            const Family& f = fit->second;
            typedef std::map<std::string, std::map<std::string, Series> >
                SeriesByFamily;
            typedef std::map<std::string, Series> SeriesMap;
            SeriesByFamily::const_iterator sit = _series.find(name);
            if (sit == _series.end() || sit->second.empty()) continue;
            if (!f.help.empty())
                os << "# HELP " << name << ' ' << EscapeHelp(f.help) << '\n';
            os << "# TYPE " << name << ' '
               << (f.type == "gauge" ? "gauge" : "counter") << '\n';
            for (SeriesMap::const_iterator it = sit->second.begin();
                 it != sit->second.end(); ++it) {
                os << name << FormatLabels(it->second.labels) << ' '
                   << it->second.value << '\n';
            }
        }
        return os.str();
    }

private:
    MetricsRegistry() { RegisterBuiltinHelp(); }

    void TouchFamily(const std::string& name, const char* type) {
        Family& f = _families[name];
        if (!f.registered) {
            f.type = type;
            f.registered = true;
        }
    }

    /// 内置指标族说明集中登记 (渲染时 HELP 文本)
    void RegisterBuiltinHelp() {
        using namespace metrics;
        Describe(kPollReadsTotal, "counter", "轮询采集样本数");
        Describe(kPollReadFailuresTotal, "counter", "轮询失败样本数");
        Describe(kWritesTotal, "counter", "写操作成功次数");
        Describe(kWriteFailuresTotal, "counter", "写操作失败次数");
        Describe(kChannelConnectsTotal, "counter",
                 "物理通道建立次数 (含重连)");
        Describe(kCircuitOpensTotal, "counter", "熔断器开启次数");
        Describe(kCircuitState, "gauge",
                 "熔断器状态 (0=Closed 1=HalfOpen 2=Open)");
        Describe(kDeviceLifecycleState, "gauge",
                 "设备生命周期 (0=New 1=Connecting 2=Connected 3=Degraded 4=Disabled)");
        Describe(kDeviceLifecycleTransitionsTotal, "counter",
                 "设备生命周期转换次数");
    }

    static std::string MakeKey(const Labels& labels) {
        // 标签键序即渲染序 — 排序保证同族多标签序列输出确定
        Labels sorted(labels);
        SortLabels(sorted);
        std::ostringstream os;
        for (size_t i = 0; i < sorted.size(); ++i) {
            os << sorted[i].first << '=' << sorted[i].second << ';';
        }
        return os.str();
    }

    static std::string FormatLabels(const Labels& labels) {
        if (labels.empty()) return "";
        Labels sorted(labels);
        SortLabels(sorted);
        std::ostringstream os;
        os << '{';
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (i) os << ',';
            os << sorted[i].first << "=\"" << EscapeLabel(sorted[i].second)
               << '"';
        }
        os << '}';
        return os.str();
    }

    static void SortLabels(Labels& labels) {
        for (size_t i = 0; i + 1 < labels.size(); ++i) {
            size_t min = i;
            for (size_t j = i + 1; j < labels.size(); ++j) {
                if (labels[j] < labels[min]) min = j;
            }
            if (min != i) std::swap(labels[i], labels[min]);
        }
    }

    static std::string EscapeLabel(const std::string& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            char c = v[i];
            if (c == '\\' || c == '"') out += '\\';
            else if (c == '\n') { out += "\\n"; continue; }
            out += c;
        }
        return out;
    }

    static std::string EscapeHelp(const std::string& v) { return EscapeLabel(v); }
};

/// 埋点便捷层 — device 标签构造 + 计数器/仪表盘快捷调用
/// (重开 namespace metrics; 常量块见类前)
namespace metrics {

typedef MetricsRegistry::Labels Lbl;

/// 单 device 标签构造 (最常用形态)
inline Lbl Device(const std::string& deviceId) {
    Lbl l;
    l.push_back(std::make_pair(std::string("device"), deviceId));
    return l;
}

inline void CounterInc(const char* name, const Lbl& labels, double v = 1.0) {
    MetricsRegistry::Instance().CounterAdd(name, labels, v);
}
inline void GaugeSet(const char* name, const Lbl& labels, double v) {
    MetricsRegistry::Instance().GaugeSet(name, labels, v);
}

} // namespace metrics

}} // namespace MyProt::Core
