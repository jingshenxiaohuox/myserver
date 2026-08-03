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

### ClientContext（连接级上下文）替换为Connection类

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
### Connection (ClientContext 重构后的替代)
```cpp
#pragma once
#include 
#include 
#include "ringbuffer.h"

class Connection {
public:
    enum class Role { Unknow, Collector, Monitor };

    Connection(int fd, int epoll_fd);
    ~Connection();  // epoll_ctl DEL + close(fd_)

    void handleRead();   // TLV 状态机 + 触发 on_message_
    void handleWrite();  // 消费 send_buffer
    void send(const char* data, size_t len);

    std::function on_message_;
    std::function on_close_;

    int fd_;
    int epoll_fd_;
    Role role_ = Role::Unknow;
    uint16_t device_id_ = 0;
    time_t last_active_time_;

private:
    RingBuffer recv_buffer_;
    RingBuffer send_buffer_;
    void updateEpollout();
};
```
> subscribers / subscribed_to 移至 Server 统一管理
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

| # | 问题 | 位置 | 风险 | 修复方案 |
|---|------|------|------|--------|
| 1 | ~~`HIGH_WATER_MARK = 6144`（6KB）远小于单帧 64KB~~ | ~~main.cpp:21~~ | ✅ 已修复，调整为 1MB |
| 2 | ~~`full_packet` 在 main() 作用域声明，跨整个事件循环复用~~ | ~~main.cpp:212~~ | ✅️非重入，如果未来多线程会有竞态；单线程下安全但语义不清晰 | 定义代码放在了状态C成功解包下面
| 3 | ~~timer_heap 每次 recv 都 push 一个新条目，不做去重~~ | ~~main.cpp:282~~ | ✅️长连接下堆无限增长，内存泄漏风险 | 把本来再recv地方push新timer_heap条目的代码删除了
| 11 | ~~`send_buffer` 只有 128KB，单帧 64KB，两帧即满~~ | ~~main.cpp ClientContext~~ | ✅️N路并发转发时 EAGAIN 后 append 立即溢出，数据静默丢失 |修改send_buffer为4MB|
| 12 | ~~`RingBuffer::append` 溢出时静默丢弃，返回 void~~ | ~~ringbuffer.h/cpp~~ | ✅️调用方无法感知写入失败，HIGH_WATER_MARK 逻辑完全失效 |改为bool返回类型|
| 13 | ~~`send_buffer` 溢出后 HIGH_WATER_MARK 检查永远不触发~~ | ~~main.cpp send_data~~ | ✅️应踢掉的慢客户端未被踢，数据丢失但连接保留，行为不可预期 |设置水位线低于send_buffer容量|

### 🟡 中优先级

| # | 问题 | 位置 | 风险 |
|---|------|------|------|
| 4 | ~~`isValidHeader()` 在 protocol.cpp 定义但 main.cpp 中未使用~~ | ~~protocol.cpp~~ | ✅️代码冗余，逻辑分散（main.cpp 自己做 magic 校验）|
| 5 | ~~main.cpp 约 400 行，所有逻辑混在一个文件~~ | ~~main.cpp~~ | ✅️已完成重构,模块边界不清，Phase 1 接入新协议时难以维护 |
| 6 | ~~`send_data` 和 `handle_write` 存在逻辑重叠~~ | ~~main.cpp:94/129~~ | ✅️部分发送路径不一致，存在 send_buffer 双重操作风险 |
| 7 | 日志全用 `std::cerr/cout`，无级别无时间戳 | 全局 | 生产环境无法使用，定位问题困难 |

### 🟢 低优先级（Phase 1 前处理即可）

| # | 问题 | 说明 |
|---|------|------|
| 8 | 协议字段 `organization` 未使用 | 预留字段，后续可做多租户隔离 |
| 9 | `bench_client.cpp` 为空 | 缺乏 C++ 层压测能力 |
| 10 | Monitor 注册/订阅无 ACK 或错误回包 | 客户端只能用固定等待猜测注册完成；压测应在收到 ACK 后再发压 |

---

## 测试工具

| 文件 | 用途 | 状态 |
|------|------|------|
| test/test.py | 单采集端 + 单监控端功能验证 + 吞吐量测试 | ✅ 可用 |
| test/bench_client.cpp | C++ 高并发压测客户端 | ❌ 空壳待实现(暂时不需要用到) | 
| test/1_to_n.py | 1 Collector → N Monitor 扇出压测 | 已完成 TLV 完整帧统计与收齐计时 |
| test/n_to_n.py | N 对 N 配对转发压测 | 已完成 TLV 完整帧统计与收齐计时 |
| test/n_to_n_full_mesh.py | N×N 全矩阵订阅压测 | 已完成 TLV 完整帧统计与收齐计时 |
| test/BENCHMARK.md | 压测口径与实验记录 | 可用 |

---

## 文件目录结构

```
myserver/
├── CMakeLists.txt
├── include/
│   ├── protocol.h          # TLV 协议定义
│   ├── server.h          # server 定义
│   ├── connection.h          # connection 协议定义
│   ├── utils.h          # setnonblocking定义
│   ├── Eventloop.h          # 待完成
│   └── ringbuffer.h        # 环形缓冲区接口
├── src/
│   ├── main.cpp            # Reactor 主循环 + 所有业务逻辑（待拆分）
│   ├── protocol.cpp        # isValidHeader（当前未被 main 使用）
│   ├── server.cpp        # server实现（当前未被 main 使用）
│   ├── connection.cpp        # Connection实现（当前未被 main 使用）
│   ├── Eventloop.cpp          # 待完成
│   └── ringbuffer.cpp      # 环形缓冲区实现
└── test/
    ├── test.py             # Python 功能+吞吐测试
    ├── bench_client.cpp    # C++ 压测客户端（空壳）
    └── 1_to_n.py          # 扇出压测（空）
```

---

## #5 重构计划：拆分 main.cpp

### 目标三个类

- `Connection` ← ClientContext 的替代，头文件已设计完成
- `Server`     ← 管理所有连接 + 订阅关系 + 路由逻辑
- `EventLoop`  ← epoll 事件循环 + 定时器

### 代码归属映射
```
setNonBlocking()          → EventLoop 或工具函数
socket/bind/listen/accept → Server
epoll_create/epoll_wait   → EventLoop
timer_heap                → EventLoop
ClientContext             → Connection
clients map               → Server
device_id_to_fd           → Server
send_data()               → Connection::send()
handle_write()            → Connection::handleWrite()
update_epollout()         → Connection::updateEpollout()
close_client()            → ~Connection() + on_close_ 回调
TLV 状态机                → Connection::handleRead()
心跳/注册/路由业务逻辑    → Server 的 on_message_ 回调
```

### 重构完成后 main.cpp 只剩
```cpp
int main() {
    EventLoop loop;
    Server server(8081, &loop);//⚠️ Server 构造函数签名待改：(int port, int epoll_fd) → (int port, EventLoop*)
    server.start();
    loop.run();
}
```

### 当前进度
- [x] Connection.h 设计完成
- [x] Connection.cpp 完成
- [x] Server.h 
- [x] Server.cpp
- [x] EventLoop.h / EventLoop.cpp
- [x] main.cpp 瘦身

### Modbus 准入收尾进度（更新至 2026-08-03）

- [x] 任务 1：Connection 生命周期与事件安全。
  - `connections_` 使用 `shared_ptr`，事件 handler 捕获 `weak_ptr` 并在本轮事件内保活。
  - `requestClose()` 提供幂等关闭；`onClose()` 校验连接身份，转发时使用订阅列表快照。
- [x] 任务 2：系统调用与 epoll 错误处理。
  - `epoll_create1/epoll_ctl/epoll_wait` 已检查错误；`EINTR` 可重试，真实错误带操作名、fd、errno 和错误文本。
  - 监听端使用 `SOCK_NONBLOCK | SOCK_CLOEXEC`，客户端使用 `accept4()` 原子设置相同标志。
  - `recv/write` 区分 `EINTR`、`EAGAIN/EWOULDBLOCK` 与真实错误。
  - 已处理 `EPOLLERR/EPOLLHUP/EPOLLRDHUP`，其中 `EPOLLERR` 通过 `SO_ERROR` 获取真实 socket 错误。
- [x] 任务 3：心跳定时器重新调度。
  - TimerEntry 到期时重新计算 `last_active_time_ + 30s`；连接尚未超时时按剩余时间重新入堆，否则发起幂等关闭。
  - 双连接计时验证中，静默连接约 `t=30s` 关闭，`t=20s` 活动一次的连接约 `t=50s` 关闭。
- [x] 任务 4：接收缓冲区与协议上限统一。
  - `MAX_PAYLOAD_SIZE` 为允许取到的 256 KiB 上限；完整接收容量覆盖 Header、最大 Payload 和一次 64 KiB `recv()` 余量。
  - `RingBuffer::append()` 失败时记录 fd、可读/可写字节和本次读取长度，随后立即发起幂等关闭。
  - Python 客户端使用相同的 256 KiB 上限与 `length > MAX_PAYLOAD_SIZE` 非法判定。
- [x] 任务 5：改为基于 `device_id` 的发布订阅。
  - 正向索引为 `device_id → Monitor fd 集合`，反向索引为 `Monitor fd → device_id 集合`；Collector fd 仅用于在线状态和来源身份校验。
  - TCP Collector、后续 Modbus 适配器和模拟数据源可复用统一 `publish(device_id, type, payload, payload_len)` 入口。
  - Monitor 可预订阅离线设备，重复订阅自动去重；Collector 断线重连后保留订阅，重复设备上线和错误设备身份会被拒绝。

任务 2 验证口径：Debug 与 ASan/UBSan 构建均通过；同机 loopback 下完成 1→1、1→2、2→2 小规模正确性回归，以及慢 Monitor、TCP RST、`EPOLLRDHUP` 半关闭场景。这里的瞬时速率不代表真实网络带宽或稳定吞吐。

任务 4 验证口径：Debug 与 ASan/UBSan 构建均通过；同机 loopback 下覆盖 0 字节 Payload、64 KiB 帧、最大合法 Payload、超限 1 字节 Header、Header/Payload 半包、粘包和 RingBuffer 回绕。另以 `/tmp` 隔离的小容量测试构建强制触发 `append()` 失败，确认只关闭问题连接且网关继续服务。这些结果只表示本机正确性，不代表真实网络带宽或稳定吞吐。

任务 5 验证口径：Debug 与 ASan/UBSan 构建均通过；同机 loopback 下完成 1→2 小规模回归，并覆盖离线预订阅、重复订阅去重、一个 Monitor 订阅多个 device、Collector 换 fd 重连、重复 Collector 拒绝、错误设备身份拒绝及 Monitor 断线后的 fd 复用隔离。这些结果只表示本机正确性，不代表真实网络带宽或稳定吞吐。

当前 `include/utils.h` 中的 `setNonBlocking()` 已无运行时调用；监听和客户端 fd 均在创建时原子设置非阻塞。该遗留文件在最终文档与文件结构清理时处理。

## Phase 1 预规划（先做 Modbus TCP）

Modbus 准入结论：

- 生命周期、系统调用错误、定时器、协议边界和 `device_id` 发布订阅五项收尾任务均已完成并通过 Debug、ASan/UBSan 关键验证。
- `test/BENCHMARK.md` 已审计：保留历史样本的测试提交、构建模式、机器、loopback 环境、负载、未采集项及适用边界，不将瞬时样本表述为真实网络带宽或稳定吞吐。
- 注册/订阅 ACK、Release 构建、CPU/RSS/延迟采样和结构化日志继续作为非阻塞技术债务，不阻止最小 Modbus TCP 闭环启动。

Phase 1 最小闭环：

```text
Modbus TCP ADU 解析 → 统一内部消息 → 复用现有发布订阅分发
```

先实现 `modbus_tcp.h/cpp`；MQTT 编解码延后到 Modbus 闭环稳定后再加入。

---

## Phase 0 压测结论与下一步（2026-07-12）

- 已新增 `test/BENCHMARK.md`、`test/1_to_n.py`、`test/n_to_n.py` 和 `test/n_to_n_full_mesh.py`，按完整 TLV 帧统计并以最后一个 Monitor 收齐作为结束条件。
- 当前结果只描述 Debug / 同机 loopback / 64 KiB 帧的突发负载，不可表述为真实网卡带宽或稳定吞吐。
- 1→N：本次成功完成到 62 个 Monitor；64 个 Monitor 连续两次触发高水位线保护并未收齐。
- N 对 N 配对：本次成功完成到 15 对；16 对触发慢客户端保护。
- N×N 全矩阵：2×2 完成；3×3 在 100 帧突发下重复性不足，说明单线程输出排空与 1 MiB 高水位线已成为边界。
- Phase 0 后续：心跳定时器刷新已修正；仍需补 Release 构建和 CPU/RSS/延迟采样，并区分突发背压边界与限速后的可持续吞吐。
- 路线决策：Phase 1 先实现最小 Modbus TCP 闭环（解析设备数据 → 统一内部消息 → 现有订阅分发）；Phase 2 的多 Reactor/线程池由真实协议负载与持续压测数据驱动。

---

## 对话续接指南

新开对话时，请上传以下文件：

1. **本文件**（ARCHITECTURE.md）— 提供整体上下文
2. **当前讨论的具体模块源文件** — 具体问题具体分析

无需每次上传整个项目。
