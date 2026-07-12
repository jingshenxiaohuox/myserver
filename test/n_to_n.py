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


def receive(monitor_id, sock, device_id, target, started, deadline, results):
    frames = 0
    payload_bytes = 0
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
            magic, version, kind, length, frame_id, _ = struct.unpack(
                HEADER_FORMAT, buffer[:HEADER_SIZE]
            )
            size = HEADER_SIZE + length
            if magic != MAGIC or version != 1 or length > MAX_PAYLOAD:
                invalid += 1
                results[monitor_id] = (frames, payload_bytes, invalid, None)
                return
            if len(buffer) < size:
                break
            del buffer[:size]
            if kind == VIDEO_FRAME and frame_id == device_id:
                frames += 1
                payload_bytes += size
                if frames == target:
                    results[monitor_id] = (frames, payload_bytes, invalid, time.perf_counter())
                    return
    results[monitor_id] = (frames, payload_bytes, invalid, None)


def send(collector, device_id, count, body, started):
    started.wait()
    for _ in range(count):
        collector.sendall(packet(VIDEO_FRAME, device_id, body))
    collector.close()


def main():
    parser = argparse.ArgumentParser(description='N 对 N 配对转发压测')
    parser.add_argument('-n', type=int, default=2, help='Collector/Monitor 对数')
    parser.add_argument('-m', type=int, default=1000, help='每个 Collector 发送帧数')
    parser.add_argument('--timeout', type=float, default=30.0, help='整体超时秒数')
    args = parser.parse_args()
    body = b'x' * (64 * 1024)
    frame_size = HEADER_SIZE + len(body)
    print(f'压测参数：{args.n} 对 Collector/Monitor，每个 Collector {args.m} 帧')

    collectors, monitors = [], []
    for device_id in range(1, args.n + 1):
        collector = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        collector.connect(('127.0.0.1', 8081))
        collector.sendall(packet(3, device_id))
        collectors.append(collector)
    time.sleep(0.2)
    for device_id in range(1, args.n + 1):
        monitor = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        monitor.connect(('127.0.0.1', 8081))
        monitor.sendall(packet(4, device_id))
        monitors.append(monitor)
    time.sleep(0.5)  # 仅等待注册完成，不参与计时。

    started = threading.Event()
    deadline = {'value': float('inf')}
    results, receiver_threads, sender_threads = {}, [], []
    for device_id, monitor in enumerate(monitors, start=1):
        thread = threading.Thread(target=receive, args=(device_id, monitor, device_id,
                                  args.m, started, deadline, results), daemon=True)
        thread.start()
        receiver_threads.append(thread)
    for device_id, collector in enumerate(collectors, start=1):
        thread = threading.Thread(target=send, args=(collector, device_id, args.m, body, started), daemon=True)
        thread.start()
        sender_threads.append(thread)

    start = time.perf_counter()
    deadline['value'] = start + args.timeout
    started.set()
    for thread in sender_threads:
        thread.join()
    for thread in receiver_threads:
        thread.join(timeout=max(0, deadline['value'] - time.perf_counter()))
    for monitor in monitors:
        try:
            monitor.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        monitor.close()
    for thread in receiver_threads:
        thread.join()

    expected = args.n * args.m
    received_frames = sum(item[0] for item in results.values())
    received_bytes = sum(item[1] for item in results.values())
    incomplete = []
    for device_id in range(1, args.n + 1):
        frames, _, invalid, finished = results.get(device_id, (0, 0, 0, None))
        if frames != args.m or invalid:
            incomplete.append(f'Monitor {device_id}: {frames}/{args.m} 帧，非法帧 {invalid}')
    print('\n========== 压测结果 =========')
    if received_frames == expected and not incomplete:
        end = max(item[3] for item in results.values())
        elapsed = end - start
        print(f'全部收齐：{expected}/{expected} 帧')
        print(f'聚合转发有效吞吐：{received_bytes / elapsed / 1024 / 1024:.2f} MiB/s')
        print(f'总耗时：{elapsed:.3f}s（首帧发送至最后一个 Monitor 收齐）')
    else:
        print(f'本轮无效：收到 {received_frames}/{expected} 帧')
        for line in incomplete:
            print(line)
    print('==============================')


if __name__ == '__main__':
    main()
