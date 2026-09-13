#!/usr/bin/env bash
# =============================================================================
# MyProt V2 — 配置 CRUD 端点 curl 级联调脚本
# 用途: 在有 VS2015 的机器上编译运行 WebApi 后, 用本脚本验证全部配置 CRUD 端点。
#
# ── 前置条件 ──────────────────────────────────────────────────────────────
#   1. 已编译并运行 MyProt.App (WebApi 监听 127.0.0.1:8080)。
#      - 运行前确保 third_party 依赖齐全: cpp-httplib/httplib.h, nlohmann, openssl, spdlog
#        (见 third_party/README.md; httplib.h 需放置到 third_party/cpp-httplib/httplib.h)
#   2. 配置目录已就绪: configs/protocols/ 与 configs/tags.json。
#      (本脚本会创建示例配置, 首次运行无需手动建)
#   3. 若 WebApiConfig.requireAuth=true, 需设置环境变量 MYPROT_API_TOKEN。
#      (本脚本默认 TOKEN 为空, 对应 requireAuth=false)
#
# ── 用法 ──────────────────────────────────────────────────────────────────
#   export MYPROT_API_TOKEN=your_token   # 仅在 requireAuth=true 时需要
#   bash scripts/curl-config-crud.sh
#
# 每个用例以 "TEST n: 描述" 开头, 输出 HTTP 状态行 + 响应体。
# 可逐个执行 (用 ; 分隔) 或整段运行。
# =============================================================================

set -u

BASE="${BASE:-http://127.0.0.1:8080}"
TOKEN="${MYPROT_API_TOKEN:-}"
AUTH=()
if [ -n "$TOKEN" ]; then
  AUTH=(-H "Authorization: Bearer $TOKEN")
fi

# 断言辅助: 期望 HTTP 状态码
expect() { # $1=期望码  $2=实际码  $3=用例名
  if [ "$1" = "$2" ]; then
    echo "  >> PASS [$3] (HTTP $2)"
  else
    echo "  >> FAIL [$3] 期望 $1 实际 $2"
  fi
}

echo "=============================================================="
echo "MyProt V2 配置 CRUD 联调 (BASE=$BASE, requireAuth=$( [ -n "$TOKEN" ] && echo on || echo off ))"
echo "=============================================================="

# ── 准备示例协议配置 (本地写入, 不经过 API) ────────────────────────────
mkdir -p configs/protocols
cat > configs/protocols/Modbus.json <<'JSON'
{
  "schemaVersion": 1,
  "protocolName": "Modbus",
  "transport": { "type": "Tcp", "tcp": { "defaultPort": 502 } },
  "framing": { "type": "Fixed", "fixed": { "fixedLength": 0 } },
  "operations": {
    "ReadHoldingRegisters": {
      "requestTemplate": ["01", "03", "{StartAddress:uint16be}", "{RegisterCount:uint16be}"],
      "responseParser": { "validCondition": "resp[1]==0x03", "dataStartIndex": 3 },
      "timeoutMs": 3000
    }
  },
  "handshake": []
}
JSON
cat > configs/tags.json <<'JSON'
{
  "schemaVersion": 1,
  "devices": [],
  "tags": []
}
JSON
echo ">> 已生成示例 configs/protocols/Modbus.json 与 configs/tags.json"

# ════════════════════════════════════════════════════════════════════════
echo "TEST 1: 列出协议配置"
r=$(curl -s -w "\n%{http_code}" "${AUTH[@]}" "$BASE/api/config/protocols")
code=$(echo "$r" | tail -1); body=$(echo "$r" | head -n -1)
echo "$body"; expect 200 "$code" "GET /api/config/protocols"

echo "TEST 2: 读取单个协议 (Modbus)"
r=$(curl -s -w "\n%{http_code}" "${AUTH[@]}" "$BASE/api/config/protocols/Modbus")
code=$(echo "$r" | tail -1); body=$(echo "$r" | head -n -1)
echo "$body"; expect 200 "$code" "GET /api/config/protocols/Modbus"

echo "TEST 3: 读取不存在的协议 → 应 404"
r=$(curl -s -w "\n%{http_code}" "${AUTH[@]}" "$BASE/api/config/protocols/NoSuch")
code=$(echo "$r" | tail -1); echo "$(echo "$r" | head -n -1)"; expect 404 "$code" "GET 不存在协议"

echo "TEST 4: 修改协议 (把 timeoutMs 改为 5000, 合法 JSON)"
r=$(curl -s -w "\n%{http_code}" -X PUT -H "Content-Type: application/json" "${AUTH[@]}" \
  -d '{"schemaVersion":1,"protocolName":"Modbus","transport":{"type":"Tcp","tcp":{"defaultPort":502}},"framing":{"type":"Fixed","fixed":{"fixedLength":0}},"operations":{"ReadHoldingRegisters":{"requestTemplate":["01","03","{StartAddress:uint16be}","{RegisterCount:uint16be}"],"responseParser":{"validCondition":"resp[1]==0x03","dataStartIndex":3},"timeoutMs":5000}},"handshake":[]}' \
  "$BASE/api/config/protocols/Modbus")
code=$(echo "$r" | tail -1); echo "$(echo "$r" | head -n -1)"; expect 200 "$code" "PUT 修改协议"

echo "TEST 5: 提交非法 JSON → 应 400 (校验失败不落盘)"
r=$(curl -s -w "\n%{http_code}" -X PUT -H "Content-Type: application/json" "${AUTH[@]}" \
  -d '{"schemaVersion": 1, "broken' "$BASE/api/config/protocols/Modbus")
code=$(echo "$r" | tail -1); echo "$(echo "$r" | head -n -1)"; expect 400 "$code" "PUT 非法 JSON"

echo "TEST 6: 提交 schemaVersion 不匹配 → 应 400 (版本门禁)"
r=$(curl -s -w "\n%{http_code}" -X PUT -H "Content-Type: application/json" "${AUTH[@]}" \
  -d '{"schemaVersion":99,"protocolName":"Modbus","transport":{"type":"Tcp"},"framing":{"type":"Fixed"},"operations":{},"handshake":[]}' \
  "$BASE/api/config/protocols/Modbus")
code=$(echo "$r" | tail -1); echo "$(echo "$r" | head -n -1)"; expect 400 "$code" "PUT schemaVersion 门禁"

echo "TEST 7: 读取 tags.json"
r=$(curl -s -w "\n%{http_code}" "${AUTH[@]}" "$BASE/api/config/tags")
code=$(echo "$r" | tail -1); body=$(echo "$r" | head -n -1)
echo "$body"; expect 200 "$code" "GET /api/config/tags"

echo "TEST 8: 修改 tags.json (写入一个设备)"
r=$(curl -s -w "\n%{http_code}" -X PUT -H "Content-Type: application/json" "${AUTH[@]}" \
  -d '{"schemaVersion":1,"devices":[{"id":"PLC-001","protocol":"Modbus","connection":{"host":"127.0.0.1","port":502,"timeoutMs":3000}}],"tags":[{"name":"PLC-001.Temperature","deviceId":"PLC-001","operation":"ReadHoldingRegisters","variables":{"StartAddress":0,"RegisterCount":1},"scanRateMs":1000,"registerCount":1,"finalType":"UInt16","reportMode":"Always"}]}' \
  "$BASE/api/config/tags")
code=$(echo "$r" | tail -1); echo "$(echo "$r" | head -n -1)"; expect 200 "$code" "PUT /api/config/tags"

echo "TEST 9: 查询 schema 元数据 (M2 骨架返回空对象)"
r=$(curl -s -w "\n%{http_code}" "${AUTH[@]}" "$BASE/api/config/schema")
code=$(echo "$r" | tail -1); body=$(echo "$r" | head -n -1)
echo "$body"; expect 200 "$code" "GET /api/config/schema"

echo "TEST 10: 列出备份 (上述 PUT 后应已有 bak.1)"
r=$(curl -s -w "\n%{http_code}" "${AUTH[@]}" "$BASE/api/config/protocols/Modbus/backups")
code=$(echo "$r" | tail -1); body=$(echo "$r" | head -n -1)
echo "$body"; expect 200 "$code" "GET backups"

echo "TEST 11: 回滚到 bak.1"
r=$(curl -s -w "\n%{http_code}" -X POST "${AUTH[@]}" "$BASE/api/config/protocols/Modbus/restore/bak.1")
code=$(echo "$r" | tail -1); echo "$(echo "$r" | head -n -1)"; expect 200 "$code" "POST restore/bak.1"

echo "TEST 12: 手动热重载"
r=$(curl -s -w "\n%{http_code}" -X POST "${AUTH[@]}" "$BASE/api/config/reload")
code=$(echo "$r" | tail -1); echo "$(echo "$r" | head -n -1)"; expect 200 "$code" "POST /api/config/reload"

echo "TEST 13: 未带 token 访问 (requireAuth=on 时 → 401)"
r=$(curl -s -w "\n%{http_code}" "$BASE/api/config/protocols")
code=$(echo "$r" | tail -1); echo "$(echo "$r" | head -n -1)"
if [ -n "$TOKEN" ]; then expect 401 "$code" "无 token"; else expect 200 "$code" "无 token (鉴权关闭)"; fi

echo "=============================================================="
echo "联调结束。若存在 FAIL, 请对照响应体检查服务端日志。"
echo "=============================================================="