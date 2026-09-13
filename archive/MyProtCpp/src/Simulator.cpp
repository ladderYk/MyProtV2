// ModbusTCP 从站模拟器
// 监听 502 端口，支持全部常用功能码 (01/02/03/04/05/06/0F/10)
// 用法：直接运行，默认监听 0.0.0.0:502
//
// 数据区划分：
//   线圈 (0x)       - 读写位，地址 0~999
//   离散输入 (1x)   - 只读位，地址 0~999
//   输入寄存器 (3x) - 只读 16 位，地址 0~999
//   保持寄存器 (4x) - 读写 16 位，地址 0~999

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

// 模拟数据空间（地址 0~999）
static std::mutex g_mutex;
static std::map<int, uint16_t> g_holdingRegs;  // 保持寄存器 (4x)
static std::map<int, uint16_t> g_inputRegs;     // 输入寄存器 (3x)
static std::map<int, bool> g_coils;             // 线圈 (0x)
static std::map<int, bool> g_discreteInputs;    // 离散输入 (1x)
static std::atomic<bool> g_running{ true };

// --- 保持寄存器 ---
uint16_t GetHoldingReg(int addr)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_holdingRegs.find(addr);
    return (it != g_holdingRegs.end()) ? it->second : 0;
}
void SetHoldingReg(int addr, uint16_t val)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_holdingRegs[addr] = val;
}

// --- 输入寄存器 ---
uint16_t GetInputReg(int addr)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_inputRegs.find(addr);
    return (it != g_inputRegs.end()) ? it->second : 0;
}
void SetInputReg(int addr, uint16_t val)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_inputRegs[addr] = val;
}

// --- 线圈 ---
bool GetCoil(int addr)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_coils.find(addr);
    return (it != g_coils.end()) ? it->second : false;
}
void SetCoil(int addr, bool val)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_coils[addr] = val;
}

// --- 离散输入 ---
bool GetDiscreteInput(int addr)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_discreteInputs.find(addr);
    return (it != g_discreteInputs.end()) ? it->second : false;
}
void SetDiscreteInput(int addr, bool val)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_discreteInputs[addr] = val;
}

// 从 socket 精确读取 n 字节
bool ReadExact(SOCKET sock, std::vector<uint8_t>& buf, int n)
{
    buf.resize(n);
    int offset = 0;
    while (offset < n)
    {
        int r = recv(sock, reinterpret_cast<char*>(buf.data() + offset), n - offset, 0);
        if (r <= 0) return false;
        offset += r;
    }
    return true;
}

// 处理 ModbusTCP 请求
void HandleClient(SOCKET clientSock, const std::string& clientAddr)
{
    std::cout << "[Simulator] Client connected: " << clientAddr << std::endl;

    while (g_running.load())
    {
        // 读取 MBAP 头部（7 字节）：TransactionID(2) + ProtocolID(2) + Length(2) + UnitID(1)
        std::vector<uint8_t> header;
        if (!ReadExact(clientSock, header, 7))
            break;

        uint16_t transactionId = (header[0] << 8) | header[1];
        uint16_t protocolId = (header[2] << 8) | header[3];
        uint16_t length = (header[4] << 8) | header[5];
        uint8_t unitId = header[6];

        // 读取 PDU（length - 1 字节，因为 UnitID 已读）
        int pduLen = length - 1;
        if (pduLen <= 0 || pduLen > 253)
            break;

        std::vector<uint8_t> pdu;
        if (!ReadExact(clientSock, pdu, pduLen))
            break;

        uint8_t functionCode = pdu[0];
        std::vector<uint8_t> response;

        if (functionCode == 0x01) // Read Coils
        {
            uint16_t startAddr = (pdu[1] << 8) | pdu[2];
            uint16_t quantity = (pdu[3] << 8) | pdu[4];
            std::cout << "[Simulator] 0x01 ReadCoils: start=" << startAddr
                      << " count=" << quantity << std::endl;

            int byteCount = (quantity + 7) / 8;
            response.push_back(unitId);
            response.push_back(0x01);
            response.push_back(static_cast<uint8_t>(byteCount));
            for (int i = 0; i < byteCount; ++i)
            {
                uint8_t byteVal = 0;
                for (int bit = 0; bit < 8 && (i * 8 + bit) < quantity; ++bit)
                {
                    if (GetCoil(startAddr + i * 8 + bit))
                        byteVal |= (1 << bit);
                }
                response.push_back(byteVal);
            }
        }
        else if (functionCode == 0x02) // Read Discrete Inputs
        {
            uint16_t startAddr = (pdu[1] << 8) | pdu[2];
            uint16_t quantity = (pdu[3] << 8) | pdu[4];
            std::cout << "[Simulator] 0x02 ReadDiscreteInputs: start=" << startAddr
                      << " count=" << quantity << std::endl;

            int byteCount = (quantity + 7) / 8;
            response.push_back(unitId);
            response.push_back(0x02);
            response.push_back(static_cast<uint8_t>(byteCount));
            for (int i = 0; i < byteCount; ++i)
            {
                uint8_t byteVal = 0;
                for (int bit = 0; bit < 8 && (i * 8 + bit) < quantity; ++bit)
                {
                    if (GetDiscreteInput(startAddr + i * 8 + bit))
                        byteVal |= (1 << bit);
                }
                response.push_back(byteVal);
            }
        }
        else if (functionCode == 0x03) // Read Holding Registers
        {
            uint16_t startAddr = (pdu[1] << 8) | pdu[2];
            uint16_t quantity = (pdu[3] << 8) | pdu[4];
            std::cout << "[Simulator] 0x03 ReadHoldingRegisters: start=" << startAddr
                      << " count=" << quantity << std::endl;

            response.push_back(unitId);
            response.push_back(0x03);
            response.push_back(static_cast<uint8_t>(quantity * 2));
            for (int i = 0; i < quantity; ++i)
            {
                uint16_t val = GetHoldingReg(startAddr + i);
                response.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
                response.push_back(static_cast<uint8_t>(val & 0xFF));
            }
        }
        else if (functionCode == 0x04) // Read Input Registers
        {
            uint16_t startAddr = (pdu[1] << 8) | pdu[2];
            uint16_t quantity = (pdu[3] << 8) | pdu[4];
            std::cout << "[Simulator] 0x04 ReadInputRegisters: start=" << startAddr
                      << " count=" << quantity << std::endl;

            response.push_back(unitId);
            response.push_back(0x04);
            response.push_back(static_cast<uint8_t>(quantity * 2));
            for (int i = 0; i < quantity; ++i)
            {
                uint16_t val = GetInputReg(startAddr + i);
                response.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
                response.push_back(static_cast<uint8_t>(val & 0xFF));
            }
        }
        else if (functionCode == 0x05) // Write Single Coil
        {
            uint16_t coilAddr = (pdu[1] << 8) | pdu[2];
            uint16_t coilVal = (pdu[3] << 8) | pdu[4];
            bool val = (coilVal == 0xFF00);
            SetCoil(coilAddr, val);
            std::cout << "[Simulator] 0x05 WriteSingleCoil: addr=" << coilAddr
                      << " value=" << (val ? "ON" : "OFF") << std::endl;

            response.push_back(unitId);
            response.push_back(0x05);
            response.push_back(pdu[1]); response.push_back(pdu[2]);
            response.push_back(pdu[3]); response.push_back(pdu[4]);
        }
        else if (functionCode == 0x06) // Write Single Register
        {
            uint16_t regAddr = (pdu[1] << 8) | pdu[2];
            uint16_t value = (pdu[3] << 8) | pdu[4];
            SetHoldingReg(regAddr, value);
            std::cout << "[Simulator] 0x06 WriteSingleRegister: addr=" << regAddr
                      << " value=" << value << std::endl;

            response.push_back(unitId);
            response.push_back(0x06);
            response.push_back(pdu[1]); response.push_back(pdu[2]);
            response.push_back(pdu[3]); response.push_back(pdu[4]);
        }
        else if (functionCode == 0x0F) // Write Multiple Coils
        {
            uint16_t startAddr = (pdu[1] << 8) | pdu[2];
            uint16_t quantity = (pdu[3] << 8) | pdu[4];
            uint8_t byteCount = pdu[5];
            std::cout << "[Simulator] 0x0F WriteMultipleCoils: start=" << startAddr
                      << " count=" << quantity << std::endl;

            for (int i = 0; i < quantity; ++i)
            {
                int byteIdx = i / 8;
                int bitIdx = i % 8;
                if (byteIdx < byteCount && (6 + byteIdx) < (int)pdu.size())
                    SetCoil(startAddr + i, (pdu[6 + byteIdx] >> bitIdx) & 0x01);
            }

            response.push_back(unitId);
            response.push_back(0x0F);
            response.push_back(pdu[1]); response.push_back(pdu[2]);
            response.push_back(pdu[3]); response.push_back(pdu[4]);
        }
        else if (functionCode == 0x10) // Write Multiple Registers
        {
            uint16_t startAddr = (pdu[1] << 8) | pdu[2];
            uint16_t quantity = (pdu[3] << 8) | pdu[4];
            std::cout << "[Simulator] 0x10 WriteMultipleRegisters: start=" << startAddr
                      << " count=" << quantity << std::endl;

            for (int i = 0; i < quantity; ++i)
            {
                int offset = 6 + i * 2;
                if (offset + 1 < (int)pdu.size())
                {
                    uint16_t val = (pdu[offset] << 8) | pdu[offset + 1];
                    SetHoldingReg(startAddr + i, val);
                }
            }

            response.push_back(unitId);
            response.push_back(0x10);
            response.push_back(pdu[1]); response.push_back(pdu[2]);
            response.push_back(pdu[3]); response.push_back(pdu[4]);
        }
        else
        {
            std::cout << "[Simulator] Unsupported function: 0x" << std::hex
                      << (int)functionCode << std::dec << std::endl;
            response.push_back(unitId);
            response.push_back(functionCode | 0x80);
            response.push_back(0x01); // Illegal function
        }

        // 构建 MBAP 头
        uint16_t respLength = static_cast<uint16_t>(response.size());
        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>((transactionId >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(transactionId & 0xFF));
        frame.push_back(0x00);
        frame.push_back(0x00);
        frame.push_back(static_cast<uint8_t>((respLength >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(respLength & 0xFF));
        frame.insert(frame.end(), response.begin(), response.end());

        int sent = send(clientSock, reinterpret_cast<const char*>(frame.data()),
                        static_cast<int>(frame.size()), 0);
        if (sent <= 0)
            break;
    }

    closesocket(clientSock);
    std::cout << "[Simulator] Client disconnected: " << clientAddr << std::endl;
}

int main()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    // 预填模拟数据
    // 保持寄存器 (4x)
    SetHoldingReg(0, 2500);  // Temperature
    SetHoldingReg(1, 0);
    SetHoldingReg(2, 1013);  // Pressure
    SetHoldingReg(3, 0);
    // 输入寄存器 (3x)
    SetInputReg(0, 3600);    // Runtime hours
    SetInputReg(1, 100);     // Firmware version
    // 线圈 (0x)
    SetCoil(0, true);        // Running
    SetCoil(1, false);       // Error
    SetCoil(2, true);        // Ready
    // 离散输入 (1x)
    SetDiscreteInput(0, true);  // Digital input ON
    SetDiscreteInput(1, false); // Digital input OFF

    SOCKET serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSock == INVALID_SOCKET)
    {
        std::cerr << "Socket creation failed: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }

    int optval = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optval), sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(502);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "Bind failed: " << WSAGetLastError() << std::endl;
        std::cerr << "Tip: port 502 may need administrator privileges" << std::endl;
        closesocket(serverSock);
        WSACleanup();
        return 1;
    }

    if (listen(serverSock, 5) == SOCKET_ERROR)
    {
        std::cerr << "Listen failed: " << WSAGetLastError() << std::endl;
        closesocket(serverSock);
        WSACleanup();
        return 1;
    }

    std::cout << "=== ModbusTCP Simulator ===" << std::endl;
    std::cout << "Listening on 0.0.0.0:502" << std::endl;
    std::cout << std::endl;
    std::cout << "Supported functions:" << std::endl;
    std::cout << "  0x01 Read Coils           0x05 Write Single Coil" << std::endl;
    std::cout << "  0x02 Read Discrete Inputs  0x06 Write Single Register" << std::endl;
    std::cout << "  0x03 Read Holding Regs     0x0F Write Multiple Coils" << std::endl;
    std::cout << "  0x04 Read Input Regs       0x10 Write Multiple Regs" << std::endl;
    std::cout << std::endl;
    std::cout << "Preset data:" << std::endl;
    std::cout << "  Holding[0]=2500  Holding[2]=1013" << std::endl;
    std::cout << "  Input[0]=3600    Input[1]=100" << std::endl;
    std::cout << "  Coil[0]=ON  Coil[2]=ON  DI[0]=ON" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Enter to stop..." << std::endl;

    // 后台接受连接
    std::thread acceptThread([&]() {
        while (g_running.load())
        {
            sockaddr_in clientAddr{};
            int addrLen = sizeof(clientAddr);
            SOCKET clientSock = accept(serverSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
            if (clientSock == INVALID_SOCKET)
                break;

            char ip[64] = {};
            inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
            std::string addr = std::string(ip) + ":" + std::to_string(ntohs(clientAddr.sin_port));

            std::thread clientThread(HandleClient, clientSock, addr);
            clientThread.detach();
        }
    });

    std::cin.get();
    g_running.store(false);

    // 关闭 server socket 使 accept 返回
    closesocket(serverSock);
    acceptThread.join();

    WSACleanup();
    std::cout << "Simulator stopped." << std::endl;
    return 0;
}
