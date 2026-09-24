// src/Core/include/MyProt/Core/Metrics.hpp
// Lightweight metrics registry - counters/gauges + Prometheus text exposition (architecture/05, KI-07)
// header-only, consistent with the Core project convention; instrumentation uses the process-level Instance(), rendering goes through GET /metrics.
//
// Metric set (naming convention myprot_*):
//   counter  myprot_poll_reads_total{device}        poll acquisition sample count
//   counter  myprot_poll_read_failures_total{device} poll failure sample count
//   counter  myprot_writes_total{device}            successful write operations
//   counter  myprot_write_failures_total{device}    failed write operations
//   counter  myprot_channel_connects_total{device}  physical channel establishments (incl. reconnects)
//   counter  myprot_circuit_opens_total{device}     circuit-breaker openings
//   gauge    myprot_circuit_state{device}           circuit-breaker state (0=Closed 1=HalfOpen 2=Open)
//   gauge    myprot_device_lifecycle_state{device}  device lifecycle (0=New 1=Connecting 2=Connected 3=Degraded 4=Disabled)
//   counter  myprot_device_lifecycle_transitions_total{device,from,to} lifecycle transitions
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <map>
#include <mutex>
#include <sstream>

namespace MyProt { namespace Core {

/// Built-in metric-name constants - referenced uniformly by instrumentation, preventing hand-written drift
/// (must be declared before MetricsRegistry: an in-class inline function body cannot see namespace-scope entities declared after it;
///  the convenience functions are defined in a later block of the same-name namespace)
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

/// Process-level metrics registry - renders Prometheus text format (exposition v0.0.4)
/// Concurrency model: a global mutex; instrumentation is a nanosecond-scale critical section, with no contention under the single io thread + a few WebApi threads
class MetricsRegistry {
public:
    typedef std::vector<std::pair<std::string, std::string> > Labels;

private:
    // ── data structures (forward-declared to work around v140's parse limit on in-class trailing types) ──
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
    std::map<std::string, Family> _families;              // family name -> meta info
    std::map<std::string, std::map<std::string, Series> >
        _series;                                          // family name -> key -> series

public:
    static MetricsRegistry& Instance() {
        static MetricsRegistry r;
        return r;
    }

    /// Pre-register metric-family meta info (type: "counter"|"gauge"; help written only when non-empty)
    void Describe(const std::string& name, const std::string& type,
                  const std::string& help) {
        std::lock_guard<std::mutex> lock(_mtx);
        Family& f = _families[name];
        if (f.type.empty()) f.type = type;
        if (!help.empty()) f.help = help;
        f.registered = true;
    }

    /// counter += v (defaults to 1)
    void CounterAdd(const std::string& name, const Labels& labels, double v = 1.0) {
        std::lock_guard<std::mutex> lock(_mtx);
        TouchFamily(name, "counter");
        std::map<std::string, Series>& fam = _series[name];
        Series& s = fam[MakeKey(labels)];
        if (s.labels.empty()) s.labels = labels;
        s.value += v;
    }

    /// gauge = v (direct set; used for state-type metrics)
    void GaugeSet(const std::string& name, const Labels& labels, double v) {
        std::lock_guard<std::mutex> lock(_mtx);
        TouchFamily(name, "gauge");
        std::map<std::string, Series>& fam = _series[name];
        Series& s = fam[MakeKey(labels)];
        s.labels = labels;
        s.value = v;
    }

    /// Render all metrics as Prometheus text (output sorted by metric name / label key, deterministic result)
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

    /// Centralized registration of built-in metric-family descriptions (HELP text at render time)
    /// HELP strings are bilingual (English / Chinese); enum-value annotations stay in English.
    void RegisterBuiltinHelp() {
        using namespace metrics;
        Describe(kPollReadsTotal, "counter",
                 "poll acquisition sample count / 轮询采集样本数");
        Describe(kPollReadFailuresTotal, "counter",
                 "poll failure sample count / 轮询失败样本数");
        Describe(kWritesTotal, "counter",
                 "successful write operations / 写操作成功数");
        Describe(kWriteFailuresTotal, "counter",
                 "failed write operations / 写操作失败数");
        Describe(kChannelConnectsTotal, "counter",
                 "physical channel establishments (incl. reconnects) / 物理通道建立数(含重连)");
        Describe(kCircuitOpensTotal, "counter",
                 "circuit-breaker openings / 熔断器开启次数");
        Describe(kCircuitState, "gauge",
                 "circuit-breaker state (0=Closed 1=HalfOpen 2=Open) / 熔断器状态");
        Describe(kDeviceLifecycleState, "gauge",
                 "device lifecycle (0=New 1=Connecting 2=Connected 3=Degraded 4=Disabled) / 设备生命周期状态");
        Describe(kDeviceLifecycleTransitionsTotal, "counter",
                 "device lifecycle transitions / 设备生命周期状态迁移次数");
    }

    static std::string MakeKey(const Labels& labels) {
        // label key order is the render order - sorting guarantees deterministic output for multi-label series in the same family
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

/// Instrumentation convenience layer - device-label construction + counter/gauge shortcuts
/// (reopens namespace metrics; the constants block is before the class)
namespace metrics {

typedef MetricsRegistry::Labels Lbl;

/// single device-label construction (the most common form)
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
