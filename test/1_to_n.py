import argparse
import socket
import struct
import threading
import time

HEADER_FORMAT = '!HBHIHH'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
MAGIC = 520
VIDEO_FRAME = 2
MAX_PAYLOAD_SIZE = 256 * 1024


def build_packet(msg_type, device_id, body=b''):
    header = struct.pack(
        HEADER_FORMAT, MAGIC, 1, msg_type, len(body), device_id, 0
    )
    return header + body


def monitor_receiver(monitor_id, sock, device_id, total_frames, start_event,
                     deadline_ref, result):
    """按 TLV 边界统计完整 VideoFrame，收齐目标帧后立即结束。"""
    received_bytes = 0
    received_frames = 0
    invalid_frames = 0
    buffer = bytearray()

    start_event.wait()
    sock.settimeout(0.2)
    while time.perf_counter() < deadline_ref['value']:
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
            magic, version, msg_type, length, frame_device_id, _ = struct.unpack(
                HEADER_FORMAT, buffer[:HEADER_SIZE]
            )
            packet_size = HEADER_SIZE + length
            if magic != MAGIC or version != 1 or length > MAX_PAYLOAD_SIZE:
                invalid_frames += 1
                result[monitor_id] = {
                    'bytes': received_bytes,
                    'frames': received_frames,
                    'invalid_frames': invalid_frames,
                    'completed_at': None,
                }
                return
            if len(buffer) < packet_size:
                break

            del buffer[:packet_size]
            if msg_type == VIDEO_FRAME and frame_device_id == device_id:
                received_frames += 1
                received_bytes += packet_size
                if received_frames == total_frames:
                    result[monitor_id] = {
                        'bytes': received_bytes,
                        'frames': received_frames,
                        'invalid_frames': invalid_frames,
                        'completed_at': time.perf_counter(),
                    }
                    return

    result[monitor_id] = {
        'bytes': received_bytes,
        'frames': received_frames,
        'invalid_frames': invalid_frames,
        'completed_at': None,
    }


def main():
    parser = argparse.ArgumentParser(description='1 Collector → N Monitor 扇出压测')
    parser.add_argument('-n', type=int, default=2, help='Monitor 数量（默认 2）')
    parser.add_argument('-m', type=int, default=1000, help='每个 Monitor 目标帧数（默认 1000）')
    parser.add_argument('--timeout', type=float, default=30.0,
                        help='等待所有 Monitor 收齐的最长秒数（默认 30）')
    args = parser.parse_args()

    monitor_count = args.n
    frame_count = args.m
    device_id = 1
    frame_body = b'x' * (64 * 1024)
    packet_size = HEADER_SIZE + len(frame_body)

    print(f'压测参数：1 Collector → {monitor_count} Monitor，每端目标 {frame_count} 帧')
    print(f'帧大小：{packet_size} B；目标聚合转发量：'
          f'{frame_count * monitor_count * packet_size / 1024 / 1024:.2f} MiB')

    collector = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    collector.connect(('127.0.0.1', 8081))
    collector.sendall(build_packet(3, device_id))

    monitors = []
    for _ in range(monitor_count):
        monitor = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        monitor.connect(('127.0.0.1', 8081))
        monitor.sendall(build_packet(4, device_id))
        monitors.append(monitor)
    time.sleep(0.5)  # 仅等待注册完成；不参与吞吐计时。

    results = {}
    threads = []
    start_event = threading.Event()
    deadline_ref = {'value': float('inf')}
    for index, monitor in enumerate(monitors, start=1):
        thread = threading.Thread(
            target=monitor_receiver,
            args=(index, monitor, device_id, frame_count, start_event,
                  deadline_ref, results),
            daemon=True,
        )
        thread.start()
        threads.append(thread)

    print(f'开始发送 {frame_count} 帧…')
    start = time.perf_counter()
    deadline_ref['value'] = start + args.timeout
    start_event.set()
    for _ in range(frame_count):
        collector.sendall(build_packet(VIDEO_FRAME, device_id, frame_body))
    send_elapsed = time.perf_counter() - start
    collector.close()
    print(f'Collector 发送完成，耗时 {send_elapsed:.3f}s')

    for thread in threads:
        thread.join(timeout=max(0, deadline_ref['value'] - time.perf_counter()))
    for monitor in monitors:
        try:
            monitor.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        monitor.close()
    for thread in threads:
        thread.join()

    print('\n========== 压测结果 =========')
    enqueue_rate = frame_count * packet_size / send_elapsed / 1024 / 1024
    print(f'Collector 发送速率：{enqueue_rate:.2f} MiB/s（客户端写入速度，不是网关吞吐）')

    received_bytes = 0
    received_frames = 0
    for index in range(1, monitor_count + 1):
        item = results.get(index)
        if item is None:
            print(f'Monitor {index}: 无结果')
            continue
        loss = frame_count - item['frames']
        completion = ('未收齐'
                      if item['completed_at'] is None
                      else f"{item['completed_at'] - start:.3f}s 收齐")
        print(f"Monitor {index}: 收到 {item['frames']}/{frame_count} 帧，"
              f"丢帧 {loss}，{completion}，非法帧 {item['invalid_frames']}")
        received_bytes += item['bytes']
        received_frames += item['frames']

    if received_frames == frame_count * monitor_count:
        completed_at = max(item['completed_at'] for item in results.values())
        elapsed = completed_at - start
        goodput = received_bytes / elapsed / 1024 / 1024
        print(f'聚合转发有效吞吐：{goodput:.2f} MiB/s')
        print(f'总耗时：{elapsed:.3f}s（首帧发送至最后一个 Monitor 收齐）')
    else:
        print('本轮未全部收齐，结果无效；请检查丢帧、慢客户端或超时。')
    print('==============================')


if __name__ == '__main__':
    main()
