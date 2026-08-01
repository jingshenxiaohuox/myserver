# 工业设备数据采集与 Linux C++ 网关项目规划

> 文档用途：记录项目当前状态、方向调整、近期任务和秋招前的交付目标。  
> 项目定位：面向 2027 届秋招，形成一套“真实设备端采集 → Modbus RTU → Linux C++ 网关 → 监控端展示”的完整工程闭环。  
> 当前原则：优先补齐真实设备端和软硬件联调经验，不再盲目增加线程池、视频流、MQTT 或 AI 推理等高成本功能。

---

## 1. 项目最终定位

项目名称暂定：

**基于 ARM/FreeRTOS、Modbus RTU 和 Linux C++ Reactor 的工业设备数据采集与监控系统**

目标数据链路：

```text
I²C 传感器
    ↓
ARM Cortex-M + FreeRTOS
    ↓ UART / RS-485
Modbus RTU 从站
    ↓ 主站轮询
Linux C++ 网关
    ↓ 转换为内部 TLV 消息
设备 ID 发布订阅
    ↓ TCP
Qt / Python 监控端
```

项目重点不是追求最大并发或功能数量，而是完成以下闭环：

1. 真实传感器采集数据；
2. ARM 开发板运行 FreeRTOS；
3. 设备端通过 Modbus RTU 暴露寄存器；
4. Linux 网关通过串口轮询设备；
5. 网关将 Modbus 数据转换为现有 TLV 消息；
6. 网关通过发布订阅机制转发给监控端；
7. 监控端展示实时数据和设备状态；
8. 监控端可以反向修改一个设备参数或控制 LED。

---

## 2. 当前已完成进度

### 2.1 Linux C++ 网关核心

当前网关已经完成：

- C++11/14；
- Linux 非阻塞网络编程；
- `epoll` ET 模式；
- 单线程 Reactor；
- `EventLoop`、`Server`、`Connection` 模块拆分；
- 每连接独立收发缓冲区；
- 环形缓冲区；
- TLV 二进制协议；
- 魔数、版本号、消息类型、设备 ID、组织 ID；
- 半包和粘包处理；
- 发布订阅路由；
- 心跳与超时检测；
- `SIGPIPE` 处理；
- `EPOLLOUT` 按需注册；
- 高水位线和慢客户端保护；
- Collector、Monitor 角色注册；
- Python 功能测试与压测脚本；
- 1 Collector → N Monitor 扇出测试；
- N Collector → N Monitor 并发测试。

当前主程序已经完成瘦身：

```cpp
int main() {
    EventLoop loop;
    Server server(8081, &loop);
    server.start();
    loop.run();
}
```

### 2.2 已解决的主要问题

已完成或基本完成：

- 将原本集中在 `main.cpp` 中的逻辑拆分为三个核心类；
- `RingBuffer::append()` 改为返回 `bool`，调用方可以识别缓冲区溢出；
- 发送缓冲区扩展到 4 MB；
- 高水位线调整为 1 MB；
- 修复缓冲区溢出后静默丢弃数据的问题；
- 修复高水位线无法正确触发的问题；
- 修复定时器在每次 `recv` 时无限压入新条目的问题；
- 接入 `isValidHeader()`；
- 完成 1-to-N 和 N-to-N 压测脚本。

### 2.3 当前代码结构

```text
myserver/
├── CMakeLists.txt
├── include/
│   ├── protocol.h
│   ├── Server.h
│   ├── Connection.h
│   ├── EventLoop.h
│   ├── ringbuffer.h
│   └── utils.h
├── src/
│   ├── main.cpp
│   ├── protocol.cpp
│   ├── Server.cpp
│   ├── Connection.cpp
│   ├── EventLoop.cpp
│   └── ringbuffer.cpp
└── test/
    ├── test.py
    ├── 1_to_n.py
    ├── n_to_n.py
    └── bench_client.cpp
```

---

## 3. 当前阶段不再继续做的内容

秋招前暂停以下方向：

- 多 Reactor；
- 通用线程池；
- 无锁线程安全发送队列；
- MQTT；
- RTSP；
- FFmpeg；
- OpenCV；
- CUDA；
- TensorRT；
- ONNX Runtime；
- 边缘 AI 推理；
- 完整 Modbus 全功能码；
- 复杂 Qt 界面；
- 单纯为了数字继续冲高吞吐量。

原因：

1. 当前网关核心已经足够支撑面试追问；
2. 继续加线程池会引出 fd 归属、线程安全、跨线程发送、锁竞争和消息顺序等大量新问题；
3. 视频流和 AI 推理会使项目主线发散；
4. 当前最大的简历缺口是真实 ARM、RTOS、串口、Modbus 和软硬件联调经验；
5. 秋招前应优先完成一个可演示、可解释、可复现的闭环，而不是堆积未完成模块。

---

## 4. 接入 Modbus 前必须完成的网关收尾

### 4.1 发布订阅改为按 device_id 路由

当前路由关系主要依赖 Collector 的 TCP fd：

```text
device_id → collector_fd
collector_fd → monitor_fd 列表
```

未来 Modbus 设备没有 TCP `Connection`，因此需要改成：

```text
device_id → monitor_fd 集合
monitor_fd → device_id 集合
```

建议数据结构：

```cpp
std::unordered_map<uint16_t, std::unordered_set<int>>
    device_subscribers_;

std::unordered_map<int, std::unordered_set<uint16_t>>
    monitor_subscriptions_;
```

新增统一发布接口：

```cpp
void Server::publish(
    uint16_t device_id,
    MsgType type,
    const char* payload,
    size_t payload_len
);
```

以后无论数据来自：

- TCP Collector；
- Modbus RTU；
- 模拟设备；
- 其他协议适配器；

都统一调用：

```cpp
server.publish(device_id, MsgType::TelemetryData, data, len);
```

### 4.2 修复 Connection 生命周期风险

当前 `EventLoop` 的 handler 捕获 `Connection*` 裸指针，而 `onClose()` 会直接从 `connections_` 中删除对象。

风险：

- 成员函数尚未返回，对象已经析构；
- handler 中可能继续访问悬空指针；
- 同一个事件中同时出现 `EPOLLIN` 和 `EPOLLOUT` 时风险更明显。

建议：

```cpp
std::unordered_map<int, std::shared_ptr<Connection>> connections_;
```

handler 捕获 `std::weak_ptr<Connection>`：

```cpp
std::weak_ptr<Connection> weak_conn = conn;

loop_->setHandler(fd, [weak_conn](uint32_t events) {
    auto conn = weak_conn.lock();
    if (!conn) return;

    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        conn->requestClose();
        return;
    }

    if (events & EPOLLIN)  conn->handleRead();
    if (events & EPOLLOUT) conn->handleWrite();
});
```

### 4.3 修复心跳定时器只检查一次的问题

当前连接创建时只压入一次 30 秒定时器。

如果 30 秒后连接仍活跃，回调直接返回，但不会再次安排下一次检查。

修改方案：

```cpp
time_t expire = conn->last_active_time_ + 30;

if (expire > now) {
    loop_->pushTimer(fd, std::max<int>(1, expire - now));
    return;
}

onClose(conn.get());
```

### 4.4 处理接收缓冲区溢出

当前 `recv_buffer_.append()` 的返回值没有被检查。

需要修改为：

```cpp
if (!recv_buffer_.append(buffer, bytes_read)) {
    requestClose();
    break;
}
```

同时建议调整：

```text
MAX_PAYLOAD_SIZE：256 KB
recv_buffer：初始 8 KB，最大 256 KB
send_buffer：初始 8 KB，最大 1 MB
HIGH_WATER_MARK：512 KB
```

Modbus 遥测数据通常很小，后续不再围绕 4 MB 视频帧设计缓冲区。

### 4.5 完善 epoll 错误处理

需要补充：

- `epoll_create1()` 返回值检查；
- `epoll_ctl()` 返回值检查；
- `epoll_wait()` 返回值检查；
- `EINTR` 处理；
- `EPOLLERR`；
- `EPOLLHUP`；
- `EPOLLRDHUP`；
- `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`。

---

## 5. 第一阶段：网关结构收尾

预计周期：1 周

### 第 1 天

- 创建 Git 标签：

```bash
git tag gateway-v1-tcp
```

- 确认当前 TCP Collector → Gateway → Monitor 功能可运行；
- 保存当前压测结果。

完成标准：

- 当前版本可以随时回退；
- `test.py`、`1_to_n.py`、`n_to_n.py` 均能运行。

### 第 2 天

- 将 `connections_` 改为 `shared_ptr`；
- handler 改为捕获 `weak_ptr`；
- 增加 `requestClose()`；
- 处理 `EPOLLERR`、`EPOLLHUP`、`EPOLLRDHUP`。

完成标准：

- 客户端主动断开时网关不崩溃；
- 异常断开时连接被正确清理；
- AddressSanitizer 无明显 use-after-free。

### 第 3 天

- 修复定时器重复调度；
- 检查 `epoll_wait()`；
- 处理 `EINTR`；
- 给 `epoll_ctl()` 增加错误日志。

完成标准：

- 活跃连接不会被误踢；
- 长时间不发心跳的连接会被关闭；
- 活跃连接后续仍会持续接受超时检查。

### 第 4 天

- 检查 `recv_buffer_.append()`；
- 调整最大 payload；
- 调整缓冲区和高水位线；
- 清理重复发送逻辑。

完成标准：

- 缓冲区溢出不会静默丢包；
- 异常大包会被拒绝；
- 慢客户端会被关闭。

### 第 5 天

- 发布订阅改为按 `device_id`；
- Monitor 允许提前订阅尚未在线的设备；
- 新增统一 `publish()` 接口。

完成标准：

- TCP Collector 上线前，Monitor 可以完成订阅；
- Collector 上线并发数据后，Monitor 可以正常收到；
- 后续 Modbus 数据可以直接调用 `publish()`。

### 周末

- 修改压测脚本；
- Monitor 按预期帧数结束，不依赖固定休眠；
- 发送前先启动接收线程；
- 改用 128 B～1 KB 遥测帧；
- 更新本规划文档和架构文档。

---

## 6. 第二阶段：Modbus 软件模拟

预计周期：2 周

这一阶段暂时不依赖真实开发板。

### 6.1 新增模块

```text
include/
├── DeviceData.h
├── IDeviceAdapter.h
├── SerialPort.h
├── ModbusRtuCodec.h
└── ModbusRtuAdapter.h

src/
├── SerialPort.cpp
├── ModbusRtuCodec.cpp
└── ModbusRtuAdapter.cpp

tools/
└── modbus_slave_sim.py

test/
└── test_modbus_rtu.cpp
```

### 6.2 ModbusRtuCodec

职责：

- 计算 CRC16；
- 构造 `0x03` 读取保持寄存器请求；
- 解析 `0x03` 响应；
- 构造 `0x06` 写单寄存器请求；
- 解析异常响应；
- 校验从站地址；
- 校验功能码；
- 校验字节数；
- 校验 CRC。

接口示例：

```cpp
uint16_t calculateCrc(const uint8_t* data, size_t len);

std::vector<uint8_t> buildReadHoldingRegisters(
    uint8_t slave_id,
    uint16_t start_address,
    uint16_t count
);

bool parseReadHoldingRegistersResponse(
    const uint8_t* frame,
    size_t len,
    std::vector<uint16_t>& registers
);
```

### 6.3 SerialPort

职责：

- 打开 `/dev/ttyUSB0`；
- 设置波特率；
- 设置数据位；
- 设置停止位；
- 设置奇偶校验；
- 非阻塞读写；
- 关闭和重新打开；
- 串口异常检测。

SerialPort 不承担 Modbus 业务解析。

### 6.4 ModbusRtuAdapter

职责：

- 定时轮询设备；
- 发送 `0x03`；
- 接收并组帧；
- 超时和重试；
- CRC 错误计数；
- 从站离线检测；
- 设备恢复；
- 将寄存器转换为网关内部数据；
- 调用 `Server::publish()`。

建议使用 `timerfd` 接入现有 epoll，不新增线程池。

### 6.5 软件模拟环境

使用伪终端：

```text
ModbusRtuAdapter
    ↕
虚拟串口
    ↕
Python Modbus 从站模拟器
```

模拟器需要支持：

- 正常响应；
- CRC 错误；
- 延迟响应；
- 不响应；
- 异常功能码；
- 寄存器值动态变化。

完成标准：

- 连续运行 30 分钟；
- 正常数据可以进入 Monitor；
- CRC 错误不发布；
- 超时设备被标记离线；
- 从站恢复后自动重新采集。

---

## 7. 第三阶段：真实设备端

预计周期：2～3 周

### 7.1 硬件需求

需要准备：

- ARM Cortex-M 开发板；
- 板载或外接调试器；
- I²C 传感器；
- RS-485 收发模块；
- USB-RS485 转换器；
- 杜邦线；
- 面包板或接线端子。

开发板选择原则：

- 官方资料完整；
- FreeRTOS 示例多；
- UART 和 I²C 资源充足；
- 调试和烧录方便；
- 不追求高性能；
- 不购买过于冷门的板子。

### 7.2 设备端任务划分

```text
采集任务
    ↓ I²C 读取传感器
数据队列
    ↓
Modbus 通信任务
    ↓
UART / RS-485

日志任务
    ↓
调试串口

监控任务
    ↓
错误计数、任务状态、设备状态
```

### 7.3 第一版设备功能

只实现：

- I²C 读取一个传感器；
- FreeRTOS 两个或三个任务；
- 任务间队列；
- UART 接收；
- Modbus RTU 从站；
- CRC16；
- 保持寄存器；
- `0x03`；
- `0x06`；
- 基本错误处理；
- 串口日志。

暂时不做：

- Bootloader；
- OTA；
- CAN；
- SPI；
- 文件系统；
- 网络协议栈；
- Linux 驱动；
- 复杂功耗优化。

---

## 8. Modbus 寄存器规划

第一版寄存器表：

| 地址 | 名称 | 类型 | 说明 |
|---:|---|---|---|
| `0x0000` | 温度 | `int16_t` | 实际温度 × 10 |
| `0x0001` | 湿度 | `uint16_t` | 实际湿度 × 10 |
| `0x0002` | 设备状态 | bit 位 | 在线、传感器错误、通信错误 |
| `0x0003` | 采样周期 | `uint16_t` | 单位 ms，可通过 `0x06` 修改 |
| `0x0004` | 错误计数 | `uint16_t` | CRC、传感器或通信错误累计 |
| `0x0005` | LED 控制 | `uint16_t` | 0 关闭，1 打开 |

网关定期读取：

```text
0x0000 ～ 0x0005
```

监控端可修改：

```text
0x0003：采样周期
0x0005：LED 状态
```

---

## 9. 第四阶段：真实端到端闭环

预计周期：1～2 周

完成链路：

```text
传感器变化
    ↓
开发板更新寄存器
    ↓
网关发送 Modbus 0x03
    ↓
开发板返回数据
    ↓
网关校验并解析
    ↓
转换为内部 TLV
    ↓
Server::publish(device_id, ...)
    ↓
Monitor 显示实时数据
```

反向链路：

```text
Monitor 修改采样周期或 LED
    ↓
网关发送 Modbus 0x06
    ↓
开发板执行
    ↓
开发板返回响应
    ↓
Monitor 显示执行结果
```

### 故障测试

必须测试：

- 错误 CRC；
- 错误从站地址；
- 错误功能码；
- 串口断开；
- 开发板掉电；
- 延迟响应；
- 设备重新上线；
- Monitor 先订阅、设备后上线；
- 网关重启后恢复采集。

---

## 10. 第五阶段：项目包装

秋招前必须形成以下材料：

### 10.1 文档

- 项目总览；
- 架构图；
- 数据流图；
- Modbus 寄存器表；
- TLV 协议说明；
- 设备状态机；
- FreeRTOS 任务划分；
- 编译和运行说明；
- 故障测试说明；
- 已知限制。

### 10.2 演示

准备 3～5 分钟演示视频：

1. 启动网关；
2. 启动 Monitor；
3. 接通开发板；
4. 展示实时传感器数据；
5. 拔掉设备，显示离线；
6. 重新插入，显示恢复；
7. 通过 Monitor 修改采样周期；
8. 控制 LED；
9. 展示一条 CRC 错误日志。

### 10.3 简历描述参考

> 基于 C++ 和 Linux 构建工业设备数据采集网关，采用 epoll ET、非阻塞 I/O 和单线程 Reactor 管理设备与监控端连接，设计 TLV 二进制协议、环形缓冲区、心跳检测、高水位线及发布订阅路由。  
> 在 ARM Cortex-M 与 FreeRTOS 平台实现 I²C 传感器采集和 Modbus RTU 从站，并为网关增加串口主站适配层，完成真实设备数据采集、协议转换、TCP 转发、监控展示和反向参数控制的端到端闭环。

---

## 11. 项目验收标准

满足以下条件后，项目冻结，不继续增加功能：

- [ ] 原 TCP Collector → Gateway → Monitor 功能正常；
- [ ] 订阅关系已经改为按 device_id；
- [ ] Connection 生命周期风险已修复；
- [ ] 心跳会持续重新调度；
- [ ] 缓冲区溢出不会静默丢包；
- [ ] Modbus CRC 单元测试通过；
- [ ] 支持 `0x03`；
- [ ] 支持 `0x06`；
- [ ] Python 模拟从站连续运行 30 分钟；
- [ ] 错误 CRC 不会被发布；
- [ ] 串口断开后设备被标记离线；
- [ ] 串口恢复后自动重新采集；
- [ ] 真实传感器数据能显示在 Monitor；
- [ ] 数据端到端延迟控制在 1～2 秒；
- [ ] Monitor 可以修改采样周期；
- [ ] Monitor 可以控制 LED；
- [ ] README 可指导另一台 Ubuntu 重新构建；
- [ ] 有架构图、寄存器表和演示视频；
- [ ] 能脱离 AI 讲清完整数据流和关键代码。

---

## 12. 时间规划

### 第 1 周

网关结构收尾：

- Connection 生命周期；
- 定时器；
- 缓冲区；
- epoll 错误处理；
- 按 device_id 发布订阅。

### 第 2～3 周

Modbus 软件模拟：

- CRC；
- `0x03`；
- `0x06`；
- SerialPort；
- timerfd；
- Python 从站模拟；
- 超时与重连。

### 第 4～6 周

真实开发板：

- 烧录与调试；
- UART；
- I²C；
- FreeRTOS；
- 寄存器表；
- Modbus RTU 从站。

### 第 7 周

端到端联调：

- 设备采集；
- 网关轮询；
- TLV 发布；
- Monitor 展示；
- 参数控制。

### 第 8 周

项目包装：

- 测试；
- 文档；
- 演示；
- 简历；
- 面试题。

---

## 13. 项目决策原则

后续每次考虑增加功能时，先回答以下问题：

1. 这个功能能否明显补齐秋招岗位的核心缺口？
2. 这个功能能否被真实演示？
3. 这个功能能否在两周内完成？
4. 这个功能能否解释清楚？
5. 这个功能是否会破坏当前主线？
6. 这个功能是否只是为了显得“高级”？

只要第 1～4 项中有两项答不上来，或者第 5、6 项答案不理想，就暂时不做。

当前唯一主线：

> **真实设备端 + Modbus RTU + Linux C++ 网关 + 监控端闭环。**

---

## 14. 当前下一步

从现在开始，按以下顺序执行：

1. 给当前稳定版本打 Git 标签；
2. 修复 Connection 生命周期；
3. 修复心跳重复调度；
4. 检查接收缓冲区溢出；
5. 将订阅关系改为按 device_id；
6. 更新回归测试；
7. 再开始 Modbus RTU Codec；
8. 用虚拟串口和 Python 从站完成软件联调；
9. 软件链路稳定后再购买开发板；
10. 最后完成真实硬件闭环。
