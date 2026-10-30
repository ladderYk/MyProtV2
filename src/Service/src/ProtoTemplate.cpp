// src/Service/src/ProtoTemplate.cpp
// Minimal protocol templates used by `--proto-init` (P0).
//
// Each template is a complete, validator-clean protocol file with exactly one
// read operation: the smallest shape that can drive a real device once the byte
// layout is replaced. Keep them short - their job is to be read, not to be
// comprehensive. Service.Tests parses every entry through the real validator.

#include "MyProt/Service/ProtoTemplate.hpp"

namespace MyProt { namespace Service {

namespace {

/// lengthfield - the recommended starting point: length-prefixed binary frames
/// (Modbus-like request/response). 12-byte request, length value = frame - 6.
const char* const kLengthFieldJson = R"JSON({
  "schemaVersion": 2,
  "protocolName": "my-lengthfield-proto",
  "_provenance": "由 myprot --proto-init lengthfield 生成: 最小可运行骨架, 请按设备手册替换字节布局。",
  "transport": { "type": "Tcp", "defaultPort": 502 },
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 4,
    "lengthFieldLength": 2,
    "lengthIncludesHeader": false,
    "byteOrder": "BigEndian",
    "headerLength": 6,
    "maxFrameSize": 1024
  },
  "dataByteOrder": "BigEndian",
  "inputs": {
    "UnitID": { "source": "static", "value": 1, "label": "设备/从站地址" },
    "StartByteAddress": { "source": "static", "value": 0, "label": "起始字节地址", "unit": "字节" },
    "ByteCount": { "source": "static", "value": 2, "label": "数据字节跨度", "unit": "字节" }
  },
  "operations": {
    "ReadRegisters": {
      "kind": "read",
      "requestTemplate": [
        "00 01", "00 00", "00 06",
        "{UnitID:X2}", "03",
        "{StartAddress:X4}", "{RegisterCount:X4}"
      ],
      "outputs": {
        "StartAddress": { "source": "auto", "strategy": "derivedLength", "expr": "StartByteAddress / 2", "label": "起始寄存器号" },
        "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "ByteCount / 2", "label": "寄存器数量" }
      },
      "responseParser": {
        "validCondition": "resp[7] == 0x03",
        "dataStartIndex": 9,
        "dataLengthExpr": "resp[8]"
      }
    }
  },
  "handshake": []
})JSON";

/// fixed - every frame is exactly fixedLength bytes (no length field).
/// 8-byte request, 8-byte response.
const char* const kFixedJson = R"JSON({
  "schemaVersion": 2,
  "protocolName": "my-fixed-proto",
  "_provenance": "由 myprot --proto-init fixed 生成: 最小可运行骨架, 请按设备手册替换字节布局。",
  "transport": { "type": "Tcp", "defaultPort": 502 },
  "framing": { "type": "Fixed", "fixedLength": 8 },
  "dataByteOrder": "BigEndian",
  "inputs": {
    "UnitID": { "source": "static", "value": 1, "label": "设备/从站地址" },
    "StartByteAddress": { "source": "static", "value": 0, "label": "起始字节地址", "unit": "字节" },
    "ByteCount": { "source": "static", "value": 2, "label": "数据字节跨度", "unit": "字节" }
  },
  "operations": {
    "ReadRegisters": {
      "kind": "read",
      "requestTemplate": [
        "{UnitID:X2}", "03",
        "{StartAddress:X4}", "{RegisterCount:X4}",
        "00 00"
      ],
      "outputs": {
        "StartAddress": { "source": "auto", "strategy": "derivedLength", "expr": "StartByteAddress / 2", "label": "起始寄存器号" },
        "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "ByteCount / 2", "label": "寄存器数量" }
      },
      "responseParser": {
        "validCondition": "resp[1] == 0x03",
        "dataStartIndex": 3,
        "dataLengthExpr": "resp[2]"
      }
    }
  },
  "handshake": []
})JSON";

/// serial - silence-framed RTU-style traffic on a serial line.
/// charTimeUs 1042 ~= 10 bits at 9600 8N1; frameGapUs is explicit (no built-in
/// Modbus convention). The device block may override portName.
const char* const kSerialJson = R"JSON({
  "schemaVersion": 2,
  "protocolName": "my-serial-proto",
  "_provenance": "由 myprot --proto-init serial 生成: 最小可运行骨架, 请按设备手册替换字节布局。",
  "transport": {
    "type": "Serial",
    "portName": "COM3",
    "baudRate": 9600,
    "dataBits": 8,
    "parity": "None",
    "stopBits": "One"
  },
  "framing": {
    "type": "Silence",
    "charTimeUs": 1042,
    "frameGapUs": 4000,
    "maxFrameSize": 256
  },
  "dataByteOrder": "BigEndian",
  "inputs": {
    "UnitID": { "source": "static", "value": 1, "label": "设备/从站地址" },
    "StartByteAddress": { "source": "static", "value": 0, "label": "起始字节地址", "unit": "字节" },
    "ByteCount": { "source": "static", "value": 2, "label": "数据字节跨度", "unit": "字节" }
  },
  "operations": {
    "ReadRegisters": {
      "kind": "read",
      "requestTemplate": [
        "{UnitID:X2}", "03",
        "{StartAddress:X4}", "{RegisterCount:X4}",
        "00 00"
      ],
      "outputs": {
        "StartAddress": { "source": "auto", "strategy": "derivedLength", "expr": "StartByteAddress / 2", "label": "起始寄存器号" },
        "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "ByteCount / 2", "label": "寄存器数量" }
      },
      "responseParser": {
        "validCondition": "resp[1] == 0x03",
        "dataStartIndex": 3,
        "dataLengthExpr": "resp[2]"
      }
    }
  },
  "handshake": []
})JSON";

} // namespace

const std::vector<ProtoTemplate>& ListProtoTemplates() {
    static const std::vector<ProtoTemplate> kAll = []() {
        std::vector<ProtoTemplate> v;
        {
            ProtoTemplate t;
            t.kind = "lengthfield";
            t.protocolName = "my-lengthfield-proto";
            t.exampleOperation = "ReadRegisters";
            t.json = kLengthFieldJson;
            t.summary = "TCP 长度域成帧 (推荐起点)";
            v.push_back(t);
        }
        {
            ProtoTemplate t;
            t.kind = "fixed";
            t.protocolName = "my-fixed-proto";
            t.exampleOperation = "ReadRegisters";
            t.json = kFixedJson;
            t.summary = "TCP 定长帧";
            v.push_back(t);
        }
        {
            ProtoTemplate t;
            t.kind = "serial";
            t.protocolName = "my-serial-proto";
            t.exampleOperation = "ReadRegisters";
            t.json = kSerialJson;
            t.summary = "串口静默成帧 (RTU 风格)";
            v.push_back(t);
        }
        return v;
    }();
    return kAll;
}

const ProtoTemplate* FindProtoTemplate(const std::string& kind) {
    const std::vector<ProtoTemplate>& all = ListProtoTemplates();
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].kind == kind) return &all[i];
    }
    return 0;
}

}} // namespace MyProt::Service
