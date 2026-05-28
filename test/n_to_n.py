import struct
import socket
import time
import threading
import argparse

HEADER_FORMAT = '!HBHIHH'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
MAGIC = 520

def build_packet(msg_type, device_id, body=b''):
    header = struct.pack(HEADER_FORMAT,
        MAGIC,
        1,
        msg_type,
        len(body),
        device_id,
        0
    )
    return header + body

def collector_sender(sock, device_id, m, result):
    start = time.time()
    for i in range(m):
        sock.sendall(build_packet(2, device_id, b'x' * 64 * 1024))
    elapsed = time.time() - start

    result[device_id] = {
        'elapsed': elapsed
    }

def monitor_receiver(monitor_id, sock, total_frames, result):
    """每个 Monitor 独立接收线程"""
    total_bytes = 0
    frames_received = 0
    start_time = None

    expected_frame_size = HEADER_SIZE + 64 * 1024
    recv_buf = b''

    while True:
        try:
            data = sock.recv(256 * 1024)
        except Exception:
            break
        if not data:
            break

        if start_time is None:
            start_time = time.time()

        recv_buf += data

        # 按帧边界统计，避免粘包导致帧数统计错误
        while len(recv_buf) >= expected_frame_size:
            recv_buf = recv_buf[expected_frame_size:]
            frames_received += 1
            total_bytes += expected_frame_size

    end_time = time.time()

    result[monitor_id] = {
        'bytes':   total_bytes,
        'frames':  frames_received,
        'elapsed': end_time - start_time if start_time else 0,
    }

def main():
    parser = argparse.ArgumentParser(description='N-to-N')
    parser.add_argument('-n', type=int, default=2,   help='Collector和Monitor 数量（默认2）')
    parser.add_argument('-m', type=int, default=2,   help='每个Collector发送的帧数（默认1000）')

    args = parser.parse_args()
    N = args.n
    M = args.m
    print(f"压测参数：{N} Collector → {N} Monitor，发送 {M} 帧（每帧 64KB）")
    print(f"预计总发送量：{M * 64 / 1024:.1f} MB，预计总转发量：{M * N * 64 / 1024:.1f} MB\n")

    collectors = []
    for i in range(N):
        collector = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        collector.connect(('127.0.0.1', 8081))
        collector.sendall(build_packet(3, i + 1))
        collectors.append(collector)
        print(f"Collector 注册成功（device_id={i + 1}）")
    time.sleep(0.1)

    monitors = []
    for i in range(N):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(('127.0.0.1', 8081))
        sock.sendall(build_packet(4, i + 1))
        monitors.append(sock)
        print(f"Monitor {i+1} 注册成功")

    results = {}
    threads = []
    for i, sock in enumerate(collectors):
        t = threading.Thread(target=collector_sender, args=(sock, i + 1, M, results))
        t.daemon = True
        t.start()
        threads.append(t)

    results_m = {}
    threads_m = []
    for i, sock in enumerate(monitors):
        t = threading.Thread(target=monitor_receiver, args=(i + 1, sock, M, results_m))
        t.daemon = True
        t.start()
        threads_m.append(t)

    for t in threads:
        t.join()
        
    for sock in collectors:
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
    
    for t in threads_m:
        t.join()
        
    for sock in monitors:
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass


    # 6. 汇总输出
    print("\n========== 压测结果 ==========")
    send_elapsed = max(results[i]['elapsed'] for i in results if results[i]['elapsed'] > 0)
    send_throughput = (M * (HEADER_SIZE + 64 * 1024)) / send_elapsed / 1024 / 1024
    print(f"发送吞吐量:   {send_throughput:.2f} MB/s")
    print()

    total_recv_bytes  = 0
    total_recv_frames = 0
    for i in range(1, N + 1):
        r = results_m.get(i)
        if not r or r['elapsed'] == 0:
            print(f"Monitor {i}: 未收到数据")
            continue
        throughput = r['bytes'] / r['elapsed'] / 1024 / 1024
        loss = M - r['frames']
        loss_rate = loss / M * 100
        total_recv_bytes  += r['bytes']
        total_recv_frames += r['frames']
        print(f"Monitor {i}:  收到 {r['frames']}/{M} 帧  "
              f"丢帧 {loss}（{loss_rate:.1f}%）  "
              f"吞吐量 {throughput:.2f} MB/s")

    print()
    if total_recv_frames > 0:
        total_elapsed = max(results_m[i]['elapsed'] for i in results_m if results_m[i]['elapsed'] > 0)
        total_throughput = total_recv_bytes / total_elapsed / 1024 / 1024
        print(f"网关总转发吞吐量: {total_throughput:.2f} MB/s（{N} 个 Monitor 合计）")

    print("================================")
    

if __name__ == '__main__':
    main()
    

