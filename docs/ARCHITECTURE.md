# myserver — 工业智能边缘网关 架构文档

> 维护说明：每次重要模块完成后更新本文档。  
> 本文档是跨对话上下文的核心锚点，新开对话时优先上传此文件。

---

## 项目定位

Linux C++ 工业智能边缘网关系统，目标方向：

- 工业协议接入（Modbus TCP / MQTT）
- 多路视频流处理（RTSP / FFmpeg / OpenCV）
- 边缘 AI 推理（ONNX Runtime / TensorRT）

目标企业：海康威视、大华、工业自动化、机器人、边缘 AI 相关企业。

---

## 演进阶段总览

```
Phase 0（当前）：单线程 Reactor 网关核心 ← 你在这里
Phase 1：工业协议能力（Modbus TCP / MQTT）
Phase 2：多 Reactor + 线程池 + 配置系统
Phase 3：视频流能力（RTSP / FFmpeg / OpenCV）
Phase 4：边缘 AI 能力（ONNX Runtime / TensorRT）
```

---

## Phase 0：当前架构状态

### 整体架构

```
┌─────────────────────────────────────────┐
│              单线程 Reactor              │
│                                         │
│  epoll ET ──► 事件分发 ──► 协议解析      │
│                  │                      │
│           ┌──────▼──────┐               │
│           │  ClientCtx  │               │
│           │ recv_buffer │  RingBuffer   │
│           │ send_buffer │  (128KB×2)    │
│           │ role/id     │               │
│           │ subscribers │               │
│           └─────────────┘               │
│                                         │
│  最小堆定时器 ──► 心跳保活（30s超时）    │
└─────────────────────────────────────────┘
```

### 已完成模块清单

| 模块 | 文件 | 状态 | 说明 |
|------|------|------|------|
| 网络核心 | main.cpp | ✅ | epoll ET + 非阻塞 IO |
| 单线程 Reactor | main.cpp | ✅ | 事件循环 + 事件分发 |
| 环形缓冲区 | ringbuffer.h/cpp | ✅ | peek/retrieve 无拷贝消费 |
| TLV 有限状态机 | main.cpp | ✅ | 三状态拆包（A:包头 B:包体 C:提取） |
| 双缓冲 ClientContext | main.cpp | ✅ | 每连接独立收/发 RingBuffer |
| 发布订阅路由 | main.cpp | ✅ | Collector→Monitor 转发模型 |
| 心跳保活 | main.cpp | ✅ | 最小堆定时器 + 懒惰删除 |
| SIGPIPE 处理 | main.cpp | ✅ | signal(SIGPIPE, SIG_IGN) |
| EPOLLOUT 按需注册 | main.cpp | ✅ | send_buffer 非空时才注册写事件 |
| 高水位线防护 | main.cpp | ✅ | 慢客户端超过 HIGH_WATER_MARK 踢出 |
| 自定义协议 | protocol.h/cpp | ✅ | TLV + Magic + Version + MsgType |

### 待完成（Phase 0 收尾）

- [ ] `bench_client.cpp`：基准测试客户端（当前为空壳）
- [ ] `1_to_n.py`：1 采集端对 N 监控端压测脚本（当前为空）
- [ ] 无锁线程安全发送队列（为 Phase 2 多线程做准备）

---

## 核心数据结构

### MsgHeader（协议头，紧凑布局）

```cpp
#pragma pack(push, 1)
struct MsgHeader {
    uint16_t magic;        // 魔法数 = 520，用于识别垃圾包
    uint8_t  version;      // 协议版本，当前 = 1
    uint16_t type;         // MsgType 枚举
    uint32_t length;       // Payload 长度（不含包头）
    uint16_t id;           // 设备 ID
    uint16_t organization; // 组织 ID
};
#pragma pack(pop)
// sizeof(MsgHeader) = 13 字节
```

### MsgType 枚举

```cpp
enum class MsgType : uint16_t {
    HeartBeat        = 0x0001,  // 心跳，服务端原样回复
    VideoFrame       = 0x0002,  // 视频帧，转发给所有订阅者
    RegisterCollector = 0x0003, // 注册为采集端
    RegisterMonitor  = 0x0004,  // 注册为监控端（订阅某 device_id）
};
```

### ClientContext（连接级上下文）

```cpp
struct ClientContext {
    RingBuffer recv_buffer;  // 128KB，接收缓冲
    RingBuffer send_buffer;  // 128KB，发送缓冲
    int        client_fd;
    time_t     last_active_time;

    enum class Role { Unknow, Collector, Monitor } role;
    uint16_t device_id;

    // 采集端专属：谁订阅了我
    std::vector<int> subscribers;
    // 监控端专属：我订阅了谁
    std::vector<int> subscribed_to;
};
```

### RingBuffer（核心收发缓冲）

```
写指针 write_idx_ ──────►
┌──────────────────────────────────┐
│  已消费  │   可读数据   │  可写  │
└──────────────────────────────────┘
           ▲
读指针 read_idx_

容量 = capacity + 1（多 1 字节区分满/空）
核心操作：append / peek（不移动读指针）/ retrieve（移动读指针）
```

---

## 核心数据流

### 发布订阅路由流程

```
采集端                    网关                     监控端
  │                        │                         │
  │── RegisterCollector ──►│ device_id_to_fd[id]=fd  │
  │                        │                         │
  │                        │◄── RegisterMonitor ─────│
  │                        │ Collector.subscribers   │
  │                        │   .push_back(monitor_fd)│
  │                        │                         │
  │── VideoFrame(64KB) ───►│                         │
  │                        │── VideoFrame ──────────►│
  │                        │   (转发给所有订阅者)     │
```

### TLV 拆包状态机

```
状态 A：检查可读字节 >= sizeof(MsgHeader)？
           │ 否 → 等待下次 epoll 唤醒（半包）
           ↓ 是
        peek 包头 → 校验 magic 数
           │ 校验失败 → close_client
           ↓ 校验通过
状态 B：检查可读字节 >= sizeof(MsgHeader) + body_length？
           │ 否 → 等待（包体未到齐）
           ↓ 是
状态 C：peek 完整包 → retrieve → 业务分发 → 回到状态 A
```

### EPOLLOUT 按需写流程

```
send_data() 被调用
    │
    ├─ write() 一次性发完 ──► update_epollout（关闭 EPOLLOUT）
    │
    ├─ write() 部分发完 ──► 剩余数据追加到 send_buffer
    │                        └► send_buffer > HIGH_WATER_MARK？ → kick
    │                        └► update_epollout（注册 EPOLLOUT）
    │
    └─ EAGAIN（内核缓冲满）──► 全部数据追加到 send_buffer
                               └► 同上高水位判断
```

---

## 全局状态

```cpp
// 连接管理
std::unordered_map<int, ClientContext> clients;         // fd → ctx
std::unordered_map<uint16_t, int>      device_id_to_fd; // device_id → fd

// 定时器
std::priority_queue<TimerEntry, ...> timer_heap;        // 最小堆，懒惰删除

// 常量
const int PORT           = 8081;
const int MAX_EVENTS     = 1024;
const size_t HIGH_WATER_MARK = 6144; // ⚠️ 见已知问题
```

---

## 已知技术债务 & 风险点

### 🔴 高优先级

| # | 问题 | 位置 | 风险 |
|---|------|------|------|
| 1 | ~~`HIGH_WATER_MARK = 6144`（6KB）远小于单帧 64KB~~ | ~~main.cpp:21~~ | ✅ 已修复，调整为 1MB |
| 2 | `full_packet` 在 main() 作用域声明，跨整个事件循环复用 | main.cpp:212 | 非重入，如果未来多线程会有竞态；单线程下安全但语义不清晰 |
| 3 | timer_heap 每次 recv 都 push 一个新条目，不做去重 | main.cpp:282 | 长连接下堆无限增长，内存泄漏风险 |
| 11 | `send_buffer` 只有 128KB，单帧 64KB，两帧即满 | main.cpp ClientContext | N路并发转发时 EAGAIN 后 append 立即溢出，数据静默丢失 |
| 12 | `RingBuffer::append` 溢出时静默丢弃，返回 void | ringbuffer.h/cpp | 调用方无法感知写入失败，HIGH_WATER_MARK 逻辑完全失效 |
| 13 | `send_buffer` 溢出后 HIGH_WATER_MARK 检查永远不触发 | main.cpp send_data | 应踢掉的慢客户端未被踢，数据丢失但连接保留，行为不可预期 |

### 🟡 中优先级

| # | 问题 | 位置 | 风险 |
|---|------|------|------|
| 4 | `isValidHeader()` 在 protocol.cpp 定义但 main.cpp 中未使用 | protocol.cpp | 代码冗余，逻辑分散（main.cpp 自己做 magic 校验）|
| 5 | main.cpp 约 400 行，所有逻辑混在一个文件 | main.cpp | 模块边界不清，Phase 1 接入新协议时难以维护 |
| 6 | `send_data` 和 `handle_write` 存在逻辑重叠 | main.cpp:94/129 | 部分发送路径不一致，存在 send_buffer 双重操作风险 |
| 7 | 日志全用 `std::cerr/cout`，无级别无时间戳 | 全局 | 生产环境无法使用，定位问题困难 |

### 🟢 低优先级（Phase 1 前处理即可）

| # | 问题 | 说明 |
|---|------|------|
| 8 | 协议字段 `organization` 未使用 | 预留字段，后续可做多租户隔离 |
| 9 | `bench_client.cpp` 为空 | 缺乏 C++ 层压测能力 |
| 10 | Monitor 订阅不存在的 Collector 时只打日志，未向客户端回包 | 客户端无感知 |

---

## 测试工具

| 文件 | 用途 | 状态 |
|------|------|------|
| test/test.py | 单采集端 + 单监控端功能验证 + 吞吐量测试 | ✅ 可用 |
| test/bench_client.cpp | C++ 高并发压测客户端 | ❌ 空壳待实现(暂时不需要用到) | 
| test/1_to_n.py | 1 采集端对 N 监控端扇出压测 | ❌ 空文件待实现 |✅️已实现
| test/n_to_n.py | N采集端对N监控端全矩阵压测 | ✅ 可用（已发现上述 #11~#13）|

---

## 文件目录结构

```
myserver/
├── CMakeLists.txt
├── include/
│   ├── protocol.h          # TLV 协议定义
│   └── ringbuffer.h        # 环形缓冲区接口
├── src/
│   ├── main.cpp            # Reactor 主循环 + 所有业务逻辑（待拆分）
│   ├── protocol.cpp        # isValidHeader（当前未被 main 使用）
│   └── ringbuffer.cpp      # 环形缓冲区实现
└── test/
    ├── test.py             # Python 功能+吞吐测试
    ├── bench_client.cpp    # C++ 压测客户端（空壳）
    └── 1_to_n.py          # 扇出压测（空）
```

---

## Phase 1 预规划（Modbus TCP / MQTT）

进入 Phase 1 前，建议先完成以下重构：

1. **拆分 main.cpp** → Server 类 / EventLoop 类 / Connection 类
2. **修复 HIGH_WATER_MARK**，调整为至少 4MB 以支持视频帧
3. **RingBuffer::append 改为返回 bool**：让上层能感知写入失败
4. **send_data 失败时正确触发 close_client**：使 HIGH_WATER_MARK 真正生效
5. **拆分 main.cpp** → Server 类 / EventLoop 类 / Connection 类
6. **修复 timer_heap 无限增长问题**

Phase 1 新增模块：
```
src/
├── protocol/
│   ├── modbus_tcp.h/cpp    # Modbus TCP ADU 解析
│   └── mqtt_codec.h/cpp    # MQTT 3.1.1 报文编解码
```

---

## 对话续接指南

新开对话时，请上传以下文件：

1. **本文件**（ARCHITECTURE.md）— 提供整体上下文
2. **当前讨论的具体模块源文件** — 具体问题具体分析

无需每次上传整个项目。
