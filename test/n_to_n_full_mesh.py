import argparse
import socket
import struct
import threading
import time

HEADER_FORMAT = '!HBHIHH'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
MAGIC = 520
VIDEO_FRAME = 2
MAX_PAYLOAD = 4 * 1024 * 1024


def packet(kind, device_id, body=b''):
    return struct.pack(HEADER_FORMAT, MAGIC, 1, kind, len(body), device_id, 0) + body


def receive(monitor_id, sock, collector_count, frames_per_collector, started,
            deadline, results):
    """一个 Monitor 接收全部 Collector 的帧，并按 device_id 分别计数。"""
    counts = {device_id: 0 for device_id in range(1, collector_count + 1)}
    bytes_received = 0
    invalid = 0
    buffer = bytearray()
    started.wait()
    sock.settimeout(0.2)
    while time.perf_counter() < deadline['value']:
        try:
            data = sock.recv(256 * 1024)
        except socket.timeout:
            continue
        except OSError:
            break
        if not data:
            break
        buffer.extend(data)
        while len(buffer) >= HEADER_SIZE:
            magic, version, kind, length, device_id, _ = struct.unpack(
                HEADER_FORMAT, buffer[:HEADER_SIZE]
            )
            size = HEADER_SIZE + length
            if magic != MAGIC or version != 1 or length > MAX_PAYLOAD:
                invalid += 1
                results[monitor_id] = (counts, bytes_received, invalid, None)
                return
            if len(buffer) < size:
                break
            del buffer[:size]
            if kind == VIDEO_FRAME and device_id in counts:
                counts[device_id] += 1
                bytes_received += size
                if all(count == frames_per_collector for count in counts.values()):
                    results[monitor_id] = (counts, bytes_received, invalid, time.perf_counter())
                    return
    results[monitor_id] = (counts, bytes_received, invalid, None)


def send(collector, device_id, frames, body, started):
    started.wait()
    for _ in range(frames):
        collector.sendall(packet(VIDEO_FRAME, device_id, body))
    collector.close()


def main():
    parser = argparse.ArgumentParser(description='N×N 全矩阵扇出压测')
    parser.add_argument('-n', type=int, default=2, help='Collector 与 Monitor 数量')
    parser.add_argument('-m', type=int, default=1000, help='每个 Collector 发送帧数')
    parser.add_argument('--timeout', type=float, default=30.0, help='整体超时秒数')
    args = parser.parse_args()
    body = b'x' * (64 * 1024)
    frame_size = HEADER_SIZE + len(body)
    expected_per_monitor = args.n * args.m
    expected_total = args.n * expected_per_monitor
    print(f'压测参数：{args.n} Collector × {args.n} Monitor，全矩阵订阅')
    print(f'每个 Collector {args.m} 帧；每个 Monitor 目标 {expected_per_monitor} 帧；'
          f'总目标 {expected_total} 帧 / {expected_total * frame_size / 1024 / 1024:.2f} MiB')

    collectors, monitors = [], []
    for device_id in range(1, args.n + 1):
        collector = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        collector.connect(('127.0.0.1', 8081))
        collector.sendall(packet(3, device_id))
        collectors.append(collector)
    time.sleep(0.2)

    for _ in range(args.n):
        monitor = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        monitor.connect(('127.0.0.1', 8081))
        for device_id in range(1, args.n + 1):
            monitor.sendall(packet(4, device_id))
        monitors.append(monitor)
    time.sleep(0.5)  # 等待全部订阅注册完成，不计入吞吐时间。

    started = threading.Event()
    deadline = {'value': float('inf')}
    results, receivers, senders = {}, [], []
    for monitor_id, monitor in enumerate(monitors, start=1):
        thread = threading.Thread(target=receive, args=(monitor_id, monitor, args.n,
                                  args.m, started, deadline, results), daemon=True)
        thread.start()
        receivers.append(thread)
    for device_id, collector in enumerate(collectors, start=1):
        thread = threading.Thread(target=send, args=(collector, device_id, args.m,
                                  body, started), daemon=True)
        thread.start()
        senders.append(thread)

    start = time.perf_counter()
    deadline['value'] = start + args.timeout
    started.set()
    for thread in senders:
        thread.join()
    for thread in receivers:
        thread.join(timeout=max(0, deadline['value'] - time.perf_counter()))
    for monitor in monitors:
        try:
            monitor.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        monitor.close()
    for thread in receivers:
        thread.join()

    total_frames = sum(sum(item[0].values()) for item in results.values())
    total_bytes = sum(item[1] for item in results.values())
    failures = []
    for monitor_id in range(1, args.n + 1):
        counts, _, invalid, completed = results.get(monitor_id, ({}, 0, 0, None))
        missing = {device_id: args.m - counts.get(device_id, 0)
                   for device_id in range(1, args.n + 1)
                   if counts.get(device_id, 0) != args.m}
        if missing or invalid:
            failures.append(f'Monitor {monitor_id}: 缺帧 {missing}，非法帧 {invalid}')

    print('\n========== 压测结果 =========')
    if total_frames == expected_total and not failures:
        end = max(item[3] for item in results.values())
        elapsed = end - start
        print(f'全部收齐：{total_frames}/{expected_total} 帧')
        print(f'聚合转发有效吞吐：{total_bytes / elapsed / 1024 / 1024:.2f} MiB/s')
        print(f'总耗时：{elapsed:.3f}s（首帧发送至最后一个 Monitor 收齐）')
    else:
        print(f'本轮无效：收到 {total_frames}/{expected_total} 帧')
        for failure in failures:
            print(failure)
    print('==============================')


if __name__ == '__main__':
    main()
