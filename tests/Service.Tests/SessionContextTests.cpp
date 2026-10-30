// tests/Service.Tests/SessionContextTests.cpp
// KI-04 设备生命周期: DeviceConfig.enabled 开关 + Degraded 累积自动禁用 + Disabled 终态。
//
// 不启动物理连接: 直接驱动 SessionContext 的状态迁移, 锁定语义边界——
//   * Disabled 是会话内终态 (只有热重载重建 SessionContext 才能恢复);
//   * 连续 Degraded 轮次达到 resilience.disableAfterDegraded 才禁用, Connected 清零;
//   * disableAfterDegraded = 0 表示永不自动禁用 (向后兼容的关闭开关);
//   * 重复 SetLifecycle(Degraded) 幂等, 不重复累积。

#include "MyProt/Service/SessionContext.hpp"
#include "MyProt/Core/Config.hpp"
#include "MiniTest.hpp"

#include <string>

using MyProt::Core::DeviceConfig;
using MyProt::Core::ResilienceConfig;
using MyProt::Service::DeviceLifecycleState;
using MyProt::Service::SessionContext;

namespace {

/// 造一台设备, 可覆盖 resilience.disableAfterDegraded (0 = 关闭自动禁用)
DeviceConfig MakeDevice(const char* id, bool enabled, int disableAfterDegraded) {
    DeviceConfig dev;
    dev.id = id;
    dev.protocol = "simmodbus";
    dev.enabled = enabled;
    ResilienceConfig res;
    res.disableAfterDegraded = disableAfterDegraded;
    dev.resilience = res;
    return dev;
}

/// 一轮「重连 → 连接级故障」: Connecting → Degraded
void DegradedRound(SessionContext& s) {
    s.SetLifecycle(DeviceLifecycleState::Connecting);
    s.SetLifecycle(DeviceLifecycleState::Degraded);
}

} // namespace

// ── enabled 默认 true: 注册后是 New, 不受累积策略影响 ──
TEST(SessionContextTest, DefaultEnabledStartsNew) {
    const DeviceConfig dev = MakeDevice("dev-new", true, 10);
    SessionContext s(dev);

    EXPECT_TRUE(s.GetLifecycle() == DeviceLifecycleState::New);
    EXPECT_EQ(s.GetDegradedStreak(), 0);
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::New));
}

// ── enabled=false: 构造即 Disabled, 且不接受后续状态迁移 (终态) ──
TEST(SessionContextTest, DisabledConfigPinsDisabled) {
    const DeviceConfig dev = MakeDevice("dev-off", false, 10);
    SessionContext s(dev);

    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Disabled));

    s.SetLifecycle(DeviceLifecycleState::Connecting);
    s.SetLifecycle(DeviceLifecycleState::Connected);
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Disabled));
    EXPECT_EQ(s.GetDegradedStreak(), 0);
}

// ── 累积达到阈值: 第 N 轮 Degraded 直接落到 Disabled ──
TEST(SessionContextTest, DegradedAccumulatesToDisabled) {
    const DeviceConfig dev = MakeDevice("dev-accum", true, 3);
    SessionContext s(dev);

    DegradedRound(s);   // 1
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Degraded));
    EXPECT_EQ(s.GetDegradedStreak(), 1);

    DegradedRound(s);   // 2
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Degraded));
    EXPECT_EQ(s.GetDegradedStreak(), 2);

    DegradedRound(s);   // 3 == 阈值 → Disabled
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Disabled));
    EXPECT_EQ(s.GetDegradedStreak(), 3);

    // 终态: 下一轮 GOC 的 Connecting 不能把它"救活"
    s.SetLifecycle(DeviceLifecycleState::Connecting);
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Disabled));
}

// ── Connected 清零累积: 抖动设备只要连上过一次就重新计数 ──
TEST(SessionContextTest, ConnectedResetsDegradedStreak) {
    const DeviceConfig dev = MakeDevice("dev-reset", true, 3);
    SessionContext s(dev);

    DegradedRound(s);   // 1
    DegradedRound(s);   // 2
    EXPECT_EQ(s.GetDegradedStreak(), 2);

    s.SetLifecycle(DeviceLifecycleState::Connected);
    EXPECT_EQ(s.GetDegradedStreak(), 0);
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Connected));

    DegradedRound(s);   // 重新从 1 开始, 不应禁用
    EXPECT_EQ(s.GetDegradedStreak(), 1);
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Degraded));
}

// ── disableAfterDegraded = 0: 永不自动禁用 (保留无限重试行为) ──
TEST(SessionContextTest, ZeroThresholdNeverAutoDisables) {
    const DeviceConfig dev = MakeDevice("dev-never", true, 0);
    SessionContext s(dev);

    for (int i = 0; i < 25; ++i) DegradedRound(s);
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Degraded));
    EXPECT_EQ(s.GetDegradedStreak(), 25);
}

// ── 重复 Degraded 幂等: 同状态重复设置不重复累积 ──
TEST(SessionContextTest, RepeatedDegradedCountsOnce) {
    const DeviceConfig dev = MakeDevice("dev-idem", true, 3);
    SessionContext s(dev);

    s.SetLifecycle(DeviceLifecycleState::Degraded);
    EXPECT_EQ(s.GetDegradedStreak(), 1);
    s.SetLifecycle(DeviceLifecycleState::Degraded);
    s.SetLifecycle(DeviceLifecycleState::Degraded);
    EXPECT_EQ(s.GetDegradedStreak(), 1);
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Degraded));

    // 仍需要两轮独立迁移才会达到阈值
    DegradedRound(s);
    DegradedRound(s);
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Disabled));
}

// ── 阈值 = 1: 第一次 Degraded 立即禁用 (等价于"立即停用") ──
TEST(SessionContextTest, ThresholdOneDisablesOnFirstDegraded) {
    const DeviceConfig dev = MakeDevice("dev-one", true, 1);
    SessionContext s(dev);

    s.SetLifecycle(DeviceLifecycleState::Degraded);
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Disabled));
    EXPECT_EQ(s.GetDegradedStreak(), 1);
}

// ── 熔断器与生命周期正交: 禁用一个维度不影响另一个 ──
TEST(SessionContextTest, LifecycleOrthogonalToCircuitBreaker) {
    DeviceConfig dev = MakeDevice("dev-orth", true, 1);
    ResilienceConfig res;
    res.disableAfterDegraded = 1;
    res.failureThreshold = 2;
    dev.resilience = res;
    SessionContext s(dev);

    s.RecordFailure();
    EXPECT_TRUE(s.GetCircuitState() == MyProt::Service::CircuitState::Closed);

    s.SetLifecycle(DeviceLifecycleState::Degraded);   // 生命周期已 Disabled
    EXPECT_EQ(static_cast<int>(s.GetLifecycle()),
              static_cast<int>(DeviceLifecycleState::Disabled));
    // 熔断维度未受影响
    EXPECT_TRUE(s.GetCircuitState() == MyProt::Service::CircuitState::Closed);
    s.RecordSuccess();
    EXPECT_TRUE(s.GetCircuitState() == MyProt::Service::CircuitState::Closed);
}
