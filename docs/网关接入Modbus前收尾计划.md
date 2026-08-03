# 网关接入 Modbus 前收尾计划

> 创建日期：2026-07-19  
> 项目目录：`/home/ljq/workspace/myserver`  
> 当前基线：单线程 Reactor、epoll ET、非阻塞 TCP、TLV、RingBuffer、Collector/Monitor 发布订阅  
> 协作方式：用户主写 `src/`、`include/`，AI 负责调用链导航、方案讨论、diff 检查、编译测试和问题定位。

## 一、目标与完成标准

本计划只处理接入 Modbus 前必须完成的网关稳定性和路由收尾，不在这一阶段加入多 Reactor、线程池、MQTT、视频处理或 AI 推理。

全部任务完成后，网关应满足：

- Connection 在事件回调、关闭回调和容器删除过程中不存在悬空指针或重复关闭。
- 活跃连接会重新安排下一次超时检查，沉默连接最终一定能被清理。
- 协议允许的最大消息一定能被接收缓冲区安全容纳；溢出不再静默丢数据。
- epoll、socket、fcntl、accept、recv、write 的正常重试状态与真实错误得到区分。
- `EPOLLERR`、`EPOLLHUP`、`EPOLLRDHUP` 有明确处理策略。
- 发布订阅以稳定的 `device_id` 为核心，而不是依赖 Collector 的临时 TCP fd。
- 现有 Collector → Gateway → Monitor 功能测试仍然通过。
- 完成 ASan/UBSan、半包/粘包、超时、异常断开、慢客户端等关键验证。
- `docs/ARCHITECTURE.md` 恢复为有效的架构事实来源，并记录最终状态。

## 二、实施顺序

按下面顺序完成，前一阶段验收通过后再进入下一阶段：

1. Connection 生命周期与 epoll 事件安全。
2. 系统调用错误处理与事件标志补齐。
3. 心跳定时器重新调度。
4. 接收缓冲区、协议上限与溢出处理统一。
5. 发布订阅改为基于 `device_id` 路由。
6. 全量回归、文档更新和 Modbus 准入检查。

其中第 1、2 项需要一起考虑；第 4 项合并原来的“检查 `append()` 返回值”和“128 KiB 接收缓冲区与 4 MiB Payload 不一致”两个问题。

---

## 三、任务 1：Connection 生命周期安全

### 当前问题

- `connections_` 使用 `std::unique_ptr<Connection>`。
- EventLoop handler 捕获 `Connection*` 裸指针。
- `Connection::handleRead()`、`send()` 或 `handleWrite()` 可以通过 `on_close_` 立即调用 `Server::onClose()`。
- `Server::onClose()` 会从 `connections_` 删除当前对象，导致成员函数尚未返回时对象已经析构。
- 同一个事件同时包含 `EPOLLIN | EPOLLOUT` 时，`handleRead()` 删除对象后，handler 仍可能调用 `handleWrite()`。

### 推荐设计

- `connections_` 改为保存 `std::shared_ptr<Connection>`。
- EventLoop handler 捕获 `std::weak_ptr<Connection>`。
- 每次事件开始时调用 `weak_ptr::lock()`，得到本轮处理期间持有的局部 `shared_ptr`。
- Connection 增加幂等关闭状态，例如 `closing_`，并提供 `requestClose()` / `isClosing()`。
- 所有错误路径统一调用 `requestClose()`；第一次调用触发 `on_close_`，后续调用直接返回。
- `Server::onClose()` 可以从 `connections_` 删除连接；handler 持有的局部 `shared_ptr` 保证对象在本轮事件返回前仍然存活。
- 一旦读事件处理导致连接进入关闭状态，当前 handler 必须立即返回，不再处理同一事件中的 `EPOLLOUT`。

### 需要重点检查的关闭入口

- `recv()` 返回 0。
- `recv()` 返回真实错误。
- Header 校验失败。
- 接收缓冲区溢出。
- `write()` 返回真实错误。
- 发送缓冲区溢出或超过高水位线。
- 心跳超时。
- `EPOLLERR/HUP/RDHUP`。

### 验收标准

- 连续连接、断开不会崩溃。
- 同一 fd 同时出现读写事件且读路径关闭连接时，不会继续访问已关闭 Connection。
- 多个错误路径同时请求关闭时，只清理一次订阅关系、只关闭一次 fd。
- handler 中 `weak_ptr::lock()` 失败时安全返回。
- ASan 不报告 heap-use-after-free、double-free 或 invalid fd access。

---

## 四、任务 2：系统调用和 epoll 事件错误处理

### 事件标志

客户端 fd 应关注并处理：

- `EPOLLIN`：读取数据或 EOF。
- `EPOLLOUT`：排空发送缓冲区，仅在有积压时注册。
- `EPOLLERR`：读取 `SO_ERROR` 记录原因，然后关闭连接。
- `EPOLLHUP`：连接挂断；如同时有可读数据，按既定策略处理后关闭。
- `EPOLLRDHUP`：对端关闭发送方向；当前网关协议默认将其视为连接结束。

事件处理必须保证：任何分支关闭连接后立即停止处理本轮后续事件。

### 系统调用分类

#### 正常的非阻塞状态

- `EAGAIN` / `EWOULDBLOCK`：不是错误，返回 EventLoop 等待下一次通知。

#### 可重试状态

- `EINTR`：系统调用被信号中断，`accept`、`recv`、`write`、`epoll_wait` 应按各自循环语义重试。

#### 真实错误

- 记录操作名、fd、`errno` 和 `strerror(errno)`。
- 连接级错误只关闭当前连接。
- `epoll_create1()` 等进程级初始化错误应阻止网关继续启动。

### 需要补齐的检查

- `epoll_create1()` 返回值。
- `epoll_ctl(ADD/MOD/DEL)` 返回值。
- `epoll_wait()` 的 `EINTR` 和其他错误。
- `fcntl(F_GETFL/F_SETFL)` 返回值。
- `socket()`、`setsockopt()`、`bind()`、`listen()` 失败后的清理。
- `accept()` 的 `EAGAIN`、`EINTR` 和真实错误。
- `recv()`、`write()` 的 `EAGAIN`、`EINTR` 和真实错误。

### 推荐改进

- 新连接优先使用 `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`，原子地设置非阻塞和 close-on-exec。
- 明确 EventLoop 的错误传播方式：初始化失败可抛出 `std::system_error`；连接级 ADD/MOD 失败应让调用方关闭对应连接。
- `updateEpollout()` 必须始终保留客户端需要的基础事件，例如 `EPOLLIN | EPOLLRDHUP | EPOLLET`。

### 验收标准

- `epoll_wait()` 被信号中断时网关继续运行。
- `recv()` / `write()` 遇到 `EINTR` 时不会误关健康连接。
- 纯 `EPOLLERR/HUP/RDHUP` 事件不会留下僵尸 Connection。
- `epoll_ctl(MOD)` 失败不会让待发送数据永久静默积压。
- 所有关键错误日志能够说明“哪个操作、哪个 fd、什么 errno”。

---

## 五、任务 3：心跳定时器重新调度

### 当前问题

连接建立时只压入一次 30 秒 TimerEntry。数据到达只更新 `last_active_time_`，不会修改定时器堆。

例如：

```text
t=0   建立连接，安排 t=30 检查
t=20  收到数据，last_active_time 更新为 20
t=30  回调发现理论到期时间是 t=50，于是直接返回
t=50  堆中没有 TimerEntry，因此不会再检查
```

### 修改目标

- 定时器到期时先确认 fd 对应连接仍然存在。
- 计算 `expire = last_active_time + timeout`。
- 如果 `expire > now`，按剩余时间重新 `pushTimer()`。
- 如果 `expire <= now`，请求关闭连接。
- 查找连接使用 `find()` 得到的迭代器，避免通过 `operator[]` 意外创建元素。

### 验收场景

- 连接建立后完全沉默：约 30 秒后关闭。
- `t=20` 活动一次：不会在 `t=30` 关闭，会在约 `t=50` 关闭。
- 持续按小于 30 秒的间隔发送心跳：连接持续存活，定时器堆不会无限增长。
- 连接提前关闭：旧 TimerEntry 到期时安全忽略。
- 超时关闭与网络错误同时发生：幂等关闭保证只清理一次。

### 暂不要求

- 暂不重写为时间轮。
- 暂不为了精确到毫秒而重构整个 EventLoop。
- 当前固定 5 秒 `epoll_wait` 超时可能带来数秒检查延迟，先记录，若验收需要再改为按最近定时器计算等待时间。

---

## 六、任务 4：接收缓冲区与协议上限统一

### 当前问题

- `MAX_PAYLOAD_SIZE` 接近 4 MiB。
- 每个 Connection 的 `recv_buffer_` 只有 128 KiB。
- `handleRead()` 忽略 `recv_buffer_.append()` 的 `false` 返回值。
- 因此协议层认为合法的消息可能永远无法装入缓冲区，溢出后还会继续保留错误的解析状态。

### 先确定容量策略

接入 Modbus 前不再围绕多 MiB 视频帧设计。建议保留现有 64 KiB 测试帧兼容性，并把最大 Payload 降到明确的有限值，例如 256 KiB。

最终必须满足以下不变量：

```text
任意协议允许的完整消息，都能被接收逻辑安全容纳并解析。
```

需要同时考虑：

- 完整包大小是 `sizeof(MsgHeader) + payload_length`，不能只比较 Payload。
- 当前实现每次先把整次 `recv()` 结果 append，再开始解析；粘包时可能同时存在“上一个大包尾部 + 下一个包开头”。
- 可选择让接收缓冲区覆盖最大包加一次读取余量，或让每次 `recv()` 不超过 RingBuffer 当前可写空间，并在空间不足时先完成已有包解析。
- 不能只把缓冲区设置成与最大 Payload 一样大，否则还缺少 Header 空间。

### 溢出处理

- 必须检查 `append()` 返回值。
- append 失败时记录 fd、当前可读/可写字节和本次读取长度。
- 将溢出视为协议/资源错误，调用幂等关闭并立即退出当前读取流程。
- 不允许打印日志后继续解析已经不完整的字节流。

### 协议边界统一

- 明确 `MAX_PAYLOAD_SIZE` 是“允许的最大值”还是“不可达到的上界”。
- Python 测试客户端和 C++ `isValidHeader()` 使用同一比较规则。
- 检查整数溢出：计算完整包大小前，先完成 length 上限校验。

### 验收场景

- 0 字节 Payload。
- 64 KiB 现有 VideoFrame。
- 最大合法 Payload。
- 比最大值多 1 字节的 Header。
- Header 半包、Payload 半包。
- 多个消息粘包。
- 环形缓冲区发生回绕。
- append 确实失败时连接被明确关闭，不静默丢数据。

---

## 七、任务 5：改为基于 device_id 的发布订阅（已完成）

### 改造前问题

当前主要关系是：

```text
device_id → Collector fd
Collector fd → Monitor fd 列表
Monitor fd → Collector fd 列表
```

真正转发 `VideoFrame` 时使用发送消息的 `conn->fd_`，没有校验消息 Header 中的设备 ID。Modbus 设备不会天然拥有一个 TCP Collector Connection，因此这种路由无法直接复用。

### 已实现的数据结构

```cpp
std::unordered_map<uint16_t, std::unordered_set<int>>
    device_subscribers_;

std::unordered_map<int, std::unordered_set<uint16_t>>
    monitor_subscriptions_;
```

如仍需管理 TCP Collector 身份，可以保留：

```cpp
device_id → TCP Collector fd
```

但它只负责来源注册和校验，不再决定发布订阅路由。

### 已实现的统一发布接口

当前接口：

```cpp
void Server::publish(
    uint16_t device_id,
    MsgType type,
    const char* payload,
    size_t payload_len
);
```

调用来源可以是：

- TCP Collector。
- 将来的 Modbus RTU/TCP 适配器。
- 模拟设备。
- 其他协议适配器。

### 行为约定

- Monitor 可以先订阅一个当前没有在线 Collector 的 `device_id`；数据源以后上线时无需重新建立订阅。
- 使用 `unordered_set` 防止重复注册导致同一个 Monitor 收到重复消息。
- TCP Collector 发布时校验注册身份和消息中的 `device_id` 一致。
- Monitor 关闭时，从它订阅的所有 device 集合中删除 fd。
- TCP Collector 关闭时不删除 device 的 Monitor 订阅，因为该 device 以后可能重连或由 Modbus 数据源继续发布。
- `publish()` 使用订阅集合副本遍历，并在发送前验证对应 Connection 仍存在、角色为 Monitor 且未进入关闭状态；常规订阅清理由 `onClose()` 完成。

### 已验证场景

- [x] Collector 先注册、Monitor 后订阅。
- [x] Monitor 先订阅、Collector 后上线。
- [x] 一个 device 对多个 Monitor。
- [x] 一个 Monitor 订阅多个 device。
- [x] 重复订阅不产生重复转发。
- [x] Collector 断线重连后 fd 改变，原 Monitor 订阅仍然有效。
- [x] 重复 Collector 注册被拒绝。
- [x] TCP Collector 使用错误 device ID 发布时被拒绝。
- [x] Monitor 断开后所有 device 订阅集合不残留其 fd。

---

## 八、回归测试与验证矩阵

### 构建

- Debug 构建开启 `-Wall -Wextra`，不得新增警告。
- 增加一次 ASan/UBSan 构建并运行连接建立、发送、关闭场景。
- 当前环境缺少 CMake 时可临时用 g++ 验证，但正式完成前应恢复项目标准 CMake 构建流程。

### 现有功能回归

- 1 Collector → 1 Monitor，少量帧功能测试。
- `test/1_to_n.py` 小规模扇出测试，所有 Monitor 收齐。
- `test/n_to_n.py` 小规模配对测试，所有 Monitor 收齐。
- 不把同机 loopback 的瞬时结果描述为真实网络带宽或稳定吞吐。

### 稳定性专项

- 正常四次挥手关闭。
- RST/异常断开。
- 同一事件含 `EPOLLIN | EPOLLOUT | EPOLLRDHUP`。
- 慢 Monitor 触发高水位线。
- 非法 magic、version、length。
- 半包、粘包、最大包、回绕。
- 心跳持续刷新和最终超时。
- fd 关闭后旧定时器到期。

### 每个任务的协作流程

1. AI 先解释涉及文件、调用链、当前行为和备选设计。
2. 用户确认理解后亲自编辑 `src/`、`include/`。
3. AI 检查 `git diff`，只讨论当前任务相关改动。
4. 先编译，再跑最小功能测试，再跑对应异常测试。
5. 通过后更新架构文档，并进入下一项。

---

## 九、进入 Modbus 的准入清单

以下项目全部勾选后再开始添加 Modbus：

- [x] Connection 回调期间对象生命周期安全。
- [x] 关闭请求幂等，不重复清理或重复关闭 fd。
- [x] ERR/HUP/RDHUP 和系统调用错误处理完成。
- [x] 活跃连接定时器能够持续重新调度。
- [x] 接收缓冲区容量与协议最大包定义一致。
- [x] `append()` 失败得到明确处理。
- [x] device_id 发布订阅和统一 `publish()` 接口完成。
- [x] 现有 1→1、1→N、N→N 小规模测试通过。
- [x] ASan/UBSan 关键场景通过。
- [x] `docs/ARCHITECTURE.md` 恢复并更新为当前事实来源。
- [x] `test/BENCHMARK.md` 只记录可复现且口径准确的结果。
- [ ] git diff 中没有 `.idea/`、`__pycache__/` 等无关生成文件。

## 十、Modbus 阶段的第一步（本计划完成后）

稳定性收尾完成后，先定义最小闭环，而不是一次实现完整 Modbus：

```text
Modbus 数据源
  → 解析一个最小功能码/寄存器数据
  → 转换为内部 TLV Telemetry 消息
  → Server::publish(device_id, ...)
  → 已订阅 Monitor
```

具体选择 Modbus RTU 还是 Modbus TCP、主站轮询接口、寄存器映射和串口配置，在进入下一阶段时单独讨论并形成新的实施计划。
